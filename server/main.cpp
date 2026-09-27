// MatchCore API server — a thin HTTP adapter over MatchingEngine.
//
// All matching, cancellation and book mutation happens inside the existing
// MatchingEngine / OrderBook. This file only:
//   • validates JSON requests and turns them into submit_limit / submit_market /
//     cancel_with_ack calls,
//   • records trades delivered by the engine's TradeHandler (matching thread),
//   • reads the book via MatchingEngine::snapshot(), which is queued like any
//     other request, so the book is never touched off the matching thread.
//
// Prices cross the API as integer ticks (cents), exactly as the engine stores
// them; dollars exist only in the UI.
//
// Endpoints:
//   GET    /api/health
//   GET    /api/book?depth=N        (default 20, max 100 levels per side)
//   GET    /api/trades?limit=N      (default 50, max 500, newest first)
//   GET    /api/metrics
//   POST   /api/orders              {"side":"buy"|"sell","type":"limit"|"market",
//                                     "price":<ticks, limit only>,"quantity":<n>}
//   DELETE /api/orders/<id>
//   POST   /api/admin/reset          header X-Reset-Token (only if MATCHCORE_RESET_TOKEN is set)
//
// Anything outside /api is served from --static <dir> (the built frontend),
// so a deployment is one process on one origin.

#include "HttpServer.hpp"
#include "Json.hpp"
#include "MatchingEngine.hpp"
#include "StaticFiles.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace matchcore;

namespace {

constexpr int64_t  kPriceScale     = 100;              // ticks per dollar (cents)
constexpr Price    kMaxPriceTicks  = 1'000'000'000;    // $10M — sanity bound
constexpr Quantity kMaxQuantity    = 1'000'000'000;
constexpr size_t   kTradeLogCap    = 10'000;

// Public-demo guards. They bound memory and response size; they do not
// affect matching semantics.
constexpr size_t   kMaxRestingOrders   = 2'000;   // new orders rejected beyond this
constexpr size_t   kMaxOrdersPerLevel  = 50;      // per-level queue listed in JSON
constexpr double   kWriteBurst         = 20;      // per-client token bucket
constexpr double   kWritesPerSecond    = 5;

int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Trades arrive on the matching thread via the TradeHandler; HTTP handlers read
// them on the server thread. A mutex-guarded bounded log keeps that simple.
struct TradeRecord {
    uint64_t seq;       // 1-based execution sequence number, assigned here
    Trade    trade;
    int64_t  wall_ms;   // wall-clock receipt time (Trade::ts_ns is steady_clock)
};

class TradeLog {
public:
    void record(const Trade& t) {
        std::lock_guard lk(mu_);
        ++fills_;
        volume_ += t.qty;
        recent_.push_back({fills_, t, wall_ms()});
        if (recent_.size() > kTradeLogCap) recent_.pop_front();
    }

    // Newest first.
    std::vector<TradeRecord> latest(size_t n) const {
        std::lock_guard lk(mu_);
        n = std::min(n, recent_.size());
        return {recent_.rbegin(), recent_.rbegin() + static_cast<std::ptrdiff_t>(n)};
    }

    // Oldest first, all records with seq > after.
    std::vector<TradeRecord> since(uint64_t after) const {
        std::lock_guard lk(mu_);
        std::vector<TradeRecord> out;
        for (const auto& r : recent_)
            if (r.seq > after) out.push_back(r);
        return out;
    }

    void clear() {
        std::lock_guard lk(mu_);
        recent_.clear();
        fills_ = volume_ = 0;
    }

    uint64_t fills()  const { std::lock_guard lk(mu_); return fills_; }
    uint64_t volume() const { std::lock_guard lk(mu_); return volume_; }

private:
    mutable std::mutex      mu_;
    std::deque<TradeRecord> recent_;
    uint64_t                fills_  = 0;
    uint64_t                volume_ = 0;
};

// Bookkeeping owned by the (single) HTTP thread — never touched by the engine.
struct ApiStats {
    uint64_t limit_orders      = 0;
    uint64_t market_orders     = 0;
    uint64_t cancels_requested = 0;
    uint64_t cancels_succeeded = 0;
    // Side of every order submitted through the API, so trades can report the
    // aggressor side (the engine's Trade struct carries IDs, not sides).
    std::unordered_map<OrderId, Side> side_of;
};

const char* side_str(Side s) { return s == Side::Buy ? "buy" : "sell"; }

std::string opt_price(const std::optional<Price>& p) {
    return p ? std::to_string(*p) : "null";
}

std::string levels_json(const std::vector<DepthLevel>& levels) {
    std::string out = "[";
    for (size_t i = 0; i < levels.size(); ++i) {
        const auto& l = levels[i];
        if (i) out += ",";
        out += "{\"price\":" + std::to_string(l.price) +
               ",\"quantity\":" + std::to_string(l.total_qty) +
               ",\"order_count\":" + std::to_string(l.orders.size()) + ",\"orders\":[";
        // Only the front of each FIFO queue is listed, to bound response size.
        for (size_t j = 0; j < std::min(l.orders.size(), kMaxOrdersPerLevel); ++j) {
            if (j) out += ",";
            out += "{\"id\":" + std::to_string(l.orders[j].id) +
                   ",\"leaves\":" + std::to_string(l.orders[j].leaves) + "}";
        }
        out += "]}";
    }
    return out + "]";
}

std::string book_json(const BookSnapshot& s) {
    std::optional<Price> bid, ask;
    if (!s.bids.empty()) bid = s.bids.front().price;
    if (!s.asks.empty()) ask = s.asks.front().price;
    std::optional<Price> spread;
    if (bid && ask) spread = *ask - *bid;

    return "{\"price_scale\":" + std::to_string(kPriceScale) +
           ",\"best_bid\":" + opt_price(bid) +
           ",\"best_ask\":" + opt_price(ask) +
           ",\"spread\":" + opt_price(spread) +
           ",\"bid_levels\":" + std::to_string(s.bid_levels) +
           ",\"ask_levels\":" + std::to_string(s.ask_levels) +
           ",\"resting_orders\":" + std::to_string(s.order_count) +
           ",\"bids\":" + levels_json(s.bids) +
           ",\"asks\":" + levels_json(s.asks) + "}";
}

std::string trade_json(const TradeRecord& r, const ApiStats& stats) {
    auto it = stats.side_of.find(r.trade.taker_id);
    std::string aggressor = it == stats.side_of.end() ? "null" : json::quote(side_str(it->second));
    return "{\"seq\":" + std::to_string(r.seq) +
           ",\"price\":" + std::to_string(r.trade.price) +
           ",\"quantity\":" + std::to_string(r.trade.qty) +
           ",\"maker_id\":" + std::to_string(r.trade.maker_id) +
           ",\"taker_id\":" + std::to_string(r.trade.taker_id) +
           ",\"aggressor\":" + aggressor +
           ",\"engine_ts_ns\":" + std::to_string(r.trade.ts_ns) +
           ",\"time_ms\":" + std::to_string(r.wall_ms) + "}";
}

std::string trades_json(const std::vector<TradeRecord>& trades, const ApiStats& stats) {
    std::string out = "[";
    for (size_t i = 0; i < trades.size(); ++i) {
        if (i) out += ",";
        out += trade_json(trades[i], stats);
    }
    return out + "]";
}

// Strict unsigned parse for path/query parameters.
std::optional<uint64_t> parse_u64(const std::string& s) {
    if (s.empty() || s.size() > 19) return std::nullopt;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

size_t clamp_param(const std::string& raw, size_t def, size_t max) {
    auto v = parse_u64(raw);
    return v ? std::min<size_t>(*v, max) : def;
}

// Token bucket per client, for write endpoints. Touched only by the HTTP thread.
class RateLimiter {
public:
    bool allow(const std::string& client) {
        auto now = std::chrono::steady_clock::now();
        if (buckets_.size() > 10'000) prune(now);
        auto [it, fresh] = buckets_.try_emplace(client, Bucket{kWriteBurst, now});
        Bucket& b = it->second;
        if (!fresh) {
            double dt = std::chrono::duration<double>(now - b.last).count();
            b.tokens = std::min(kWriteBurst, b.tokens + dt * kWritesPerSecond);
            b.last   = now;
        }
        if (b.tokens < 1) return false;
        b.tokens -= 1;
        return true;
    }

private:
    struct Bucket { double tokens; std::chrono::steady_clock::time_point last; };

    void prune(std::chrono::steady_clock::time_point now) {
        std::erase_if(buckets_, [&](const auto& kv) {
            return now - kv.second.last > std::chrono::seconds(60);
        });
    }

    std::unordered_map<std::string, Bucket> buckets_;
};

struct Config {
    std::string reset_token;          // empty → /api/admin/reset disabled
    bool        trust_proxy = false;  // take client IP from X-Forwarded-For
    bool        seed        = false;  // start with a resting book (public demo)
};

// Deterministic starting book: 5 levels a side, 10 ticks apart around $100.00,
// several orders per level so FIFO priority is visible. Levels never cross,
// so seeding produces no trades.
struct SeedLevel { Price offset; std::vector<Quantity> qtys; };
const SeedLevel kSeed[] = {
    {10, {20, 15}}, {20, {25, 20, 10}}, {30, {40, 25}}, {40, {50, 30, 20}}, {50, {80, 40}},
};
constexpr Price kSeedMid = 10'000;

// Constant-time comparison so the reset token can't be probed by timing.
bool token_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

class Api {
public:
    explicit Api(Config cfg) : cfg_(std::move(cfg)), started_ms_(wall_ms()) { start_engine(); }
    ~Api() { engine_->stop(); }

    http::Response handle(const http::Request& req) {
        const std::string& p = req.path;
        if ((req.method == "POST" || req.method == "DELETE") && !limiter_.allow(client_of(req)))
            return {429, json::error("rate limit exceeded — slow down")};

        if (p == "/api/admin/reset") return only("POST", req, [&] { return reset(req); });
        if (p == "/api/health")  return only("GET", req, [&] { return http::Response{200, R"({"ok":true})"}; });
        if (p == "/api/book")    return only("GET", req, [&] { return get_book(req); });
        if (p == "/api/trades")  return only("GET", req, [&] { return get_trades(req); });
        if (p == "/api/metrics") return only("GET", req, [&] { return get_metrics(); });
        if (p == "/api/orders")  return only("POST", req, [&] { return post_order(req); });

        const std::string prefix = "/api/orders/";
        if (p.rfind(prefix, 0) == 0)
            return only("DELETE", req, [&] { return delete_order(p.substr(prefix.size())); });

        return {404, json::error("not found")};
    }

private:
    // TradeHandler runs on the matching thread; TradeLog::record is thread-safe.
    void start_engine() {
        engine_ = std::make_unique<MatchingEngine>([this](const Trade& t) { log_.record(t); });
        engine_->start();
        seeded_ = 0;
        if (!cfg_.seed) return;
        // Plain submissions through the engine's public API, like any client.
        for (const auto& lvl : kSeed) {
            for (Quantity q : lvl.qtys) {
                engine_->submit_limit(Side::Buy,  kSeedMid - lvl.offset, q);
                engine_->submit_limit(Side::Sell, kSeedMid + lvl.offset, q);
                seeded_ += 2;
            }
        }
    }

    std::string client_of(const http::Request& req) const {
        if (cfg_.trust_proxy) {
            std::string xff = req.header("x-forwarded-for");
            if (!xff.empty()) return xff.substr(0, xff.find(','));
        }
        return req.peer_ip;
    }

    // Fresh engine and empty book. Safe without extra locking: this thread is
    // the only producer, and stop() joins the matching thread first.
    http::Response reset(const http::Request& req) {
        if (cfg_.reset_token.empty()) return {404, json::error("not found")};
        if (!token_equals(req.header("x-reset-token"), cfg_.reset_token))
            return {403, json::error("bad reset token")};
        engine_->stop();
        log_.clear();
        stats_ = {};
        start_engine();
        return {200, R"({"reset":true})"};
    }

    template <typename F>
    http::Response only(const char* method, const http::Request& req, F&& f) {
        if (req.method != method) return {405, json::error(std::string("use ") + method)};
        return f();
    }

    http::Response get_book(const http::Request& req) {
        size_t depth = clamp_param(http::query_param(req.query, "depth"), 20, 100);
        return {200, book_json(engine_->snapshot(depth).get())};
    }

    http::Response get_trades(const http::Request& req) {
        size_t limit = clamp_param(http::query_param(req.query, "limit"), 50, 500);
        return {200, trades_json(log_.latest(limit), stats_)};
    }

    http::Response get_metrics() {
        // Queued behind any outstanding requests, so counts are consistent
        // with everything this server has submitted so far.
        BookSnapshot s = engine_->snapshot(0).get();
        uint64_t submitted = stats_.limit_orders + stats_.market_orders;
        return {200,
            "{\"engine\":\"MatchingEngine (MPSC queue, single matching thread)\""
            ",\"orders_submitted\":" + std::to_string(submitted) +
            ",\"limit_orders\":" + std::to_string(stats_.limit_orders) +
            ",\"market_orders\":" + std::to_string(stats_.market_orders) +
            ",\"cancels_requested\":" + std::to_string(stats_.cancels_requested) +
            ",\"cancels_succeeded\":" + std::to_string(stats_.cancels_succeeded) +
            ",\"fills\":" + std::to_string(log_.fills()) +
            ",\"volume\":" + std::to_string(log_.volume()) +
            ",\"resting_orders\":" + std::to_string(s.order_count) +
            ",\"bid_levels\":" + std::to_string(s.bid_levels) +
            ",\"ask_levels\":" + std::to_string(s.ask_levels) +
            ",\"seeded_orders\":" + std::to_string(seeded_) +
            ",\"uptime_s\":" + std::to_string((wall_ms() - started_ms_) / 1000) + "}"};
    }

    http::Response post_order(const http::Request& req) {
        auto obj = json::parse_flat_object(req.body);
        if (!obj) return {400, json::error("body must be a flat JSON object with string/integer values")};

        auto str = [&](const char* k) -> std::optional<std::string> {
            auto it = obj->find(k);
            if (it == obj->end()) return std::nullopt;
            if (auto* v = std::get_if<std::string>(&it->second)) return *v;
            return std::nullopt;
        };
        auto integer = [&](const char* k) -> std::optional<int64_t> {
            auto it = obj->find(k);
            if (it == obj->end()) return std::nullopt;
            if (auto* v = std::get_if<int64_t>(&it->second)) return *v;
            return std::nullopt;
        };

        auto side_s = str("side");
        if (!side_s || (*side_s != "buy" && *side_s != "sell"))
            return {400, json::error("side must be \"buy\" or \"sell\"")};
        Side side = *side_s == "buy" ? Side::Buy : Side::Sell;

        std::string type = str("type").value_or("limit");
        if (type != "limit" && type != "market")
            return {400, json::error("type must be \"limit\" or \"market\"")};

        auto qty = integer("quantity");
        if (!qty || *qty <= 0 || static_cast<Quantity>(*qty) > kMaxQuantity)
            return {400, json::error("quantity must be an integer in [1, 1000000000]")};

        std::optional<int64_t> price;
        bool has_price = obj->count("price") && !std::holds_alternative<std::nullptr_t>(obj->at("price"));
        if (type == "limit") {
            price = integer("price");
            if (!price || *price <= 0 || *price > kMaxPriceTicks)
                return {400, json::error("price must be an integer number of ticks (cents) in [1, 1000000000]")};
        } else if (has_price) {
            return {400, json::error("market orders do not take a price")};
        }

        // Bound memory on a public deployment. A market order never rests,
        // so only limit orders are subject to the cap.
        if (type == "limit" && engine_->snapshot(0).get().order_count >= kMaxRestingOrders)
            return {409, json::error("book is full (" + std::to_string(kMaxRestingOrders) +
                                     " resting orders) — cancel some orders first")};

        // This thread is the only producer, so every trade recorded between
        // `before` and the snapshot barrier below belongs to this order.
        uint64_t before = log_.fills();
        OrderId id = type == "limit"
            ? engine_->submit_limit(side, *price, static_cast<Quantity>(*qty))
            : engine_->submit_market(side, static_cast<Quantity>(*qty));
        stats_.side_of[id] = side;
        (type == "limit" ? stats_.limit_orders : stats_.market_orders)++;

        // Barrier: the snapshot is processed after the order, so by the time
        // it returns, the order has been matched and all its trades delivered.
        BookSnapshot snap = engine_->snapshot(20).get();
        auto fills = log_.since(before);

        Quantity filled = 0;
        for (const auto& r : fills) filled += r.trade.qty;
        Quantity leaves = static_cast<Quantity>(*qty) - filled;

        // Engine contract: limit residual rests; market residual is discarded.
        const char* status =
            leaves == 0            ? "filled" :
            type == "market"       ? (filled ? "partially_filled_remainder_discarded" : "unfilled_discarded") :
            filled                 ? "partially_filled_resting" :
                                     "resting";

        return {201,
            "{\"order_id\":" + std::to_string(id) +
            ",\"side\":" + json::quote(*side_s) +
            ",\"type\":" + json::quote(type) +
            ",\"price\":" + (price ? std::to_string(*price) : "null") +
            ",\"quantity\":" + std::to_string(*qty) +
            ",\"filled\":" + std::to_string(filled) +
            ",\"leaves\":" + std::to_string(leaves) +
            ",\"status\":" + json::quote(status) +
            ",\"fills\":" + trades_json(fills, stats_) +
            ",\"book\":" + book_json(snap) + "}"};
    }

    http::Response delete_order(const std::string& raw_id) {
        auto id = parse_u64(raw_id);
        if (!id) return {400, json::error("order id must be a positive integer")};

        ++stats_.cancels_requested;
        bool ok = engine_->cancel_with_ack(*id).get();
        if (ok) ++stats_.cancels_succeeded;

        BookSnapshot snap = engine_->snapshot(20).get();
        return {200,
            "{\"order_id\":" + std::to_string(*id) +
            ",\"cancelled\":" + (ok ? "true" : "false") +
            ",\"reason\":" + (ok ? "null" : json::quote("order not resting (unknown, filled, or already cancelled)")) +
            ",\"book\":" + book_json(snap) + "}"};
    }

    Config                          cfg_;
    TradeLog                        log_;   // declared before engine_: outlives its handler
    std::unique_ptr<MatchingEngine> engine_;
    ApiStats                        stats_;
    RateLimiter                     limiter_;
    int64_t                         started_ms_;
    uint64_t                        seeded_ = 0;
};

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true); }

} // namespace

int main(int argc, char** argv) {
    // Environment first (Render/Fly/Heroku-style PORT), flags override.
    std::string host = std::getenv("HOST") ? std::getenv("HOST") : "127.0.0.1";
    uint16_t    port = std::getenv("PORT") ? static_cast<uint16_t>(std::atoi(std::getenv("PORT"))) : 8080;
    std::string static_dir;
    Config      cfg;
    if (const char* t = std::getenv("MATCHCORE_RESET_TOKEN")) cfg.reset_token = t;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!std::strcmp(argv[i], "--static") && i + 1 < argc) {
            static_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--trust-proxy")) {
            cfg.trust_proxy = true;
        } else if (!std::strcmp(argv[i], "--seed")) {
            cfg.seed = true;
        } else {
            std::cerr << "usage: " << argv[0]
                      << " [--host 127.0.0.1] [--port 8080] [--static frontend/dist] [--trust-proxy] [--seed]\n"
                         "env: PORT, HOST, MATCHCORE_RESET_TOKEN\n";
            return 2;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);   // a client hanging up mid-response must not kill us
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        std::unique_ptr<http::StaticFiles> files;
        if (!static_dir.empty()) files = std::make_unique<http::StaticFiles>(static_dir);

        http::Server server(host, port);
        Api api(cfg);
        std::cout << "MatchCore API listening on http://" << host << ":" << port
                  << "  (prices in ticks, " << kPriceScale << " ticks = $1)"
                  << (files ? "  serving UI from " + static_dir : "") << std::endl;

        server.serve([&](const http::Request& r) {
            if (files && r.path.rfind("/api/", 0) != 0) return files->serve(r);
            return api.handle(r);
        }, g_stop);

        std::cout << "\nshutting down — draining matching engine\n";
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
