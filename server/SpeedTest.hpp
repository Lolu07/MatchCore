#pragma once
// On-demand micro-benchmark for the demo UI. Runs the phase3_bench workload
// (80% limit / 10% market / 10% cancel, $99.00–$101.00, qty 1–50, cancel the
// order 32 submissions back) against *private* engine and book instances —
// the shared demo book is never touched. Numbers are real measurements on
// whatever machine the server runs on.

#include "MatchingEngine.hpp"
#include "OrderBook.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

namespace matchcore::speedtest {

struct Result {
    size_t   ops;
    double   engine_ops_per_s;   // MatchingEngine: 1 producer → queue → matching thread
    uint64_t engine_trades;
    double   book_mean_ns;       // OrderBook called directly, single thread, timed per call
    double   book_p50_ns;
    double   book_p99_ns;
    unsigned cpus;
};

inline Result run(size_t n) {
    enum class Op : uint8_t { Limit, Market, Cancel };
    struct Item { Op op; Side side; Price price; Quantity qty; };

    // Workload generated up front so RNG cost is excluded from timing.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price>    px(9'900, 10'100);
    std::uniform_int_distribution<Quantity> qt(1, 50);
    std::uniform_real_distribution<double>  roll(0.0, 1.0);
    std::vector<Item> work;
    work.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double r = roll(rng);
        Op op = r < 0.10 ? Op::Cancel : r < 0.20 ? Op::Market : Op::Limit;
        work.push_back({op, (rng() & 1) ? Side::Buy : Side::Sell, px(rng), qt(rng)});
    }
    constexpr size_t kLookback = 32;

    Result res{};
    res.ops  = n;
    res.cpus = std::thread::hardware_concurrency();

    // A) MatchingEngine throughput. stop() is inside the timed window: it
    //    returns only once the matching thread has drained every request.
    {
        std::atomic<uint64_t> trades{0};
        MatchingEngine engine([&](const Trade&) { trades.fetch_add(1, std::memory_order_relaxed); });
        std::vector<OrderId> ids;
        ids.reserve(n);
        engine.start();
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& w : work) {
            switch (w.op) {
                case Op::Limit:  ids.push_back(engine.submit_limit(w.side, w.price, w.qty)); break;
                case Op::Market: ids.push_back(engine.submit_market(w.side, w.qty)); break;
                case Op::Cancel: if (ids.size() >= kLookback) engine.cancel(ids[ids.size() - kLookback]); break;
            }
        }
        engine.stop();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        res.engine_ops_per_s = static_cast<double>(n) / s;
        res.engine_trades    = trades.load();
    }

    // B) OrderBook per-call latency, no queue and no lock.
    {
        OrderBook book;
        std::vector<uint64_t> lat;
        lat.reserve(n);
        OrderId next = 1;
        for (const auto& w : work) {
            uint64_t t0 = now_ns();
            switch (w.op) {
                case Op::Limit:
                    book.add_limit({next, w.side, OrderType::Limit, w.price, w.qty, w.qty, t0});
                    ++next;
                    break;
                case Op::Market:
                    book.add_market({next, w.side, OrderType::Market, 0, w.qty, w.qty, t0});
                    ++next;
                    break;
                case Op::Cancel:
                    if (next > kLookback) book.cancel(next - kLookback);
                    break;
            }
            lat.push_back(now_ns() - t0);
        }
        uint64_t sum = 0;
        for (auto x : lat) sum += x;
        std::sort(lat.begin(), lat.end());
        res.book_mean_ns = static_cast<double>(sum) / static_cast<double>(lat.size());
        res.book_p50_ns  = static_cast<double>(lat[lat.size() * 50 / 100]);
        res.book_p99_ns  = static_cast<double>(lat[lat.size() * 99 / 100]);
    }
    return res;
}

} // namespace matchcore::speedtest
