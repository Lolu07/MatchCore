// Phase 3 benchmark: measures throughput (orders/s) and per-call latency
// (mean, p50, p99) for both concurrency models, across 1–8 producer threads.
//
// Measurement discipline:
//   • Workload is pre-generated before the timer starts — RNG cost is excluded.
//   • For ConcurrentOrderBook, each call is timed individually with now_ns().
//     The timer wraps only the execution loop, not thread-join overhead.
//   • For MatchingEngine, engine.stop() is inside the timed window because
//     it blocks until the matching thread has drained the queue — without it
//     we would not be measuring the full pipeline cost.
//   • A warm-up run (discarded) precedes all measurements to stabilise CPU
//     frequency and warm instruction/data caches.

#include "ConcurrentOrderBook.hpp"
#include "MatchingEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace matchcore;
using namespace std::chrono;

// ─── Workload ──────────────────────────────────────────────────────────────

enum class Op : uint8_t { Limit, Market, Cancel };

struct WorkItem {
    Op       op;
    Side     side;
    Price    price;
    Quantity qty;
};

// Generate the full workload before any benchmark run. Every thread gets the
// *same* pre-generated items — this ensures thread counts are directly
// comparable (same total work per thread, different parallelism).
std::vector<WorkItem> gen_work(int n, uint64_t seed,
                                Price lo, Price hi,
                                double mkt_frac, double cxl_frac) {
    std::mt19937_64                     rng(seed);
    std::uniform_int_distribution<Price>    px(lo, hi);
    std::uniform_int_distribution<int>      sd(0, 1);
    std::uniform_real_distribution<double>  op_roll(0.0, 1.0);
    std::uniform_int_distribution<Quantity> qt(1, 50);

    std::vector<WorkItem> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        double r = op_roll(rng);
        Op op    = (r < cxl_frac)             ? Op::Cancel  :
                   (r < cxl_frac + mkt_frac)  ? Op::Market  :
                                                 Op::Limit;
        v.push_back({op, sd(rng) ? Side::Buy : Side::Sell, px(rng), qt(rng)});
    }
    return v;
}

// ─── Cancel ring buffer ────────────────────────────────────────────────────
//
// Each worker thread keeps a private ring of recently submitted OrderIds.
// When the workload calls for a cancel, we target the order submitted
// LOOKBACK slots ago — fresh enough to probably exist, old enough that it
// may still be resting (very recent orders in a hot book are often matched
// immediately). If the target was already matched, book.cancel / engine.cancel
// returns false silently — that is correct and realistic behaviour.

struct Ring {
    static constexpr int CAP     = 64;
    static constexpr int LOOKBACK = 32; // cancel the order 32 slots back

    OrderId ids[CAP]{};
    int     head = 0; // next write position
    int     fill = 0; // how many slots are populated

    void push(OrderId id) {
        ids[head] = id;
        head      = (head + 1) % CAP;
        if (fill < CAP) ++fill;
    }

    // Returns 0 if the ring is too empty to target a slot.
    OrderId cancel_target() const {
        if (fill < LOOKBACK) return 0;
        int slot = (head - LOOKBACK + CAP) % CAP;
        return ids[slot];
    }
};

// ─── Results ───────────────────────────────────────────────────────────────

struct Result {
    int    threads;
    long   n_ops;        // total operations submitted
    long   n_trades;     // total trade events generated
    double elapsed_s;
    double ops_per_s;
    double trades_per_op; // analogous to fill rate

    // Populated only for ConcurrentOrderBook (synchronous calls):
    double lat_mean_ns = 0;
    double lat_p50_ns  = 0;
    double lat_p99_ns  = 0;
};

// Compute mean and percentiles from a vector of nanosecond latency samples.
// Sorting is done after the timed section so it does not affect measurements.
static void fill_latency_stats(Result& r,
                                std::vector<std::vector<uint64_t>>& per_thread) {
    // Merge per-thread vectors without extra allocation passes
    size_t total = 0;
    for (auto& v : per_thread) total += v.size();
    if (total == 0) return;

    std::vector<uint64_t> all;
    all.reserve(total);
    for (auto& v : per_thread) all.insert(all.end(), v.begin(), v.end());

    // Mean — use uint64_t accumulator to avoid double-precision loss on large N
    uint64_t sum = 0;
    for (auto x : all) sum += x;
    r.lat_mean_ns = static_cast<double>(sum) / static_cast<double>(all.size());

    // Percentiles via full sort (done post-timing)
    std::sort(all.begin(), all.end());
    r.lat_p50_ns = static_cast<double>(all[all.size() * 50 / 100]);
    r.lat_p99_ns = static_cast<double>(all[all.size() * 99 / 100]);
}

// ─── ConcurrentOrderBook benchmark ────────────────────────────────────────
//
// N threads each execute the full workload vector, calling add_limit /
// add_market / cancel directly. Every call is bracketed by now_ns() to
// capture per-call latency — the timer overhead (~5–15 ns per call on
// modern hardware) is a known systematic bias; acceptable here because
// our operations are 100s of ns to µs.

Result bench_book(int nthreads, const std::vector<WorkItem>& work) {
    ConcurrentOrderBook  book;
    std::atomic<OrderId> next_id{1};
    std::atomic<long>    n_trades{0};

    const int n = static_cast<int>(work.size());
    std::vector<std::vector<uint64_t>> lats(nthreads);
    for (auto& v : lats) v.reserve(n);

    auto t0 = steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int tid = 0; tid < nthreads; ++tid) {
        threads.emplace_back([&, tid] {
            Ring  ring;
            auto& my_lats = lats[tid];

            for (const auto& item : work) {
                uint64_t ts = now_ns();  // ← start timing this operation

                if (item.op == Op::Cancel) {
                    OrderId target = ring.cancel_target();
                    if (target) book.cancel(target);
                } else {
                    OrderId id = next_id.fetch_add(1, std::memory_order_relaxed);
                    Order o{id, item.side,
                            item.op == Op::Market ? OrderType::Market : OrderType::Limit,
                            item.price, item.qty, item.qty, now_ns()};

                    auto trades = (item.op == Op::Market)
                                  ? book.add_market(o)
                                  : book.add_limit(o);

                    n_trades.fetch_add(static_cast<long>(trades.size()),
                                       std::memory_order_relaxed);
                    ring.push(id);
                }

                my_lats.push_back(now_ns() - ts);  // ← stop timing
            }
        });
    }
    for (auto& t : threads) t.join();

    auto t1 = steady_clock::now();

    long total = static_cast<long>(n) * nthreads;
    long trds  = n_trades.load();

    Result r;
    r.threads       = nthreads;
    r.n_ops         = total;
    r.n_trades      = trds;
    r.elapsed_s     = duration<double>(t1 - t0).count();
    r.ops_per_s     = total / r.elapsed_s;
    r.trades_per_op = static_cast<double>(trds) / total;

    fill_latency_stats(r, lats); // post-timing
    return r;
}

// ─── MatchingEngine benchmark ──────────────────────────────────────────────
//
// N producer threads enqueue operations into the MPSC request queue.
// The single matching thread drains and processes them.
// engine.stop() is inside the timed window: it blocks until the matching
// thread has processed everything. Excluding it would under-count the time.
//
// Per-call latency is NOT measured here because submit_*() is asynchronous —
// it returns before the order is processed. End-to-end latency for the engine
// model requires a feedback channel (acknowledgement queue or promise/future),
// which is beyond this benchmark's scope. Throughput is the primary metric.

Result bench_engine(int nthreads, const std::vector<WorkItem>& work) {
    std::atomic<long> n_trades{0};

    MatchingEngine engine([&](const Trade&) {
        n_trades.fetch_add(1, std::memory_order_relaxed);
    });
    engine.start();

    auto t0 = steady_clock::now(); // ← start (producers enqueue + engine drains)

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (int tid = 0; tid < nthreads; ++tid) {
        threads.emplace_back([&] {
            Ring ring;
            for (const auto& item : work) {
                if (item.op == Op::Cancel) {
                    OrderId target = ring.cancel_target();
                    if (target) engine.cancel(target);
                } else {
                    OrderId id = (item.op == Op::Market)
                                 ? engine.submit_market(item.side, item.qty)
                                 : engine.submit_limit(item.side, item.price, item.qty);
                    ring.push(id);
                }
            }
        });
    }
    for (auto& t : threads) t.join(); // all ops enqueued
    engine.stop();                     // drain queue + join matching thread

    auto t1 = steady_clock::now(); // ← stop (every op has been processed)

    long total = static_cast<long>(work.size()) * nthreads;
    long trds  = n_trades.load();

    Result r;
    r.threads       = nthreads;
    r.n_ops         = total;
    r.n_trades      = trds;
    r.elapsed_s     = duration<double>(t1 - t0).count();
    r.ops_per_s     = total / r.elapsed_s;
    r.trades_per_op = static_cast<double>(trds) / total;
    return r;
}

// ─── Formatting helpers ────────────────────────────────────────────────────

static std::string comma(long n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
        s.insert(static_cast<size_t>(i), ",");
    return s;
}

// Auto-scale nanosecond values into a readable unit.
static std::string ns_str(double ns) {
    if (ns >= 1'000'000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << ns / 1'000'000.0 << " ms";
        return oss.str();
    }
    if (ns >= 1'000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << ns / 1'000.0 << " µs";
        return oss.str();
    }
    return std::to_string(static_cast<long>(ns)) + " ns";
}

// ─── main ──────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Default: 300,000 ops/thread — change with ./phase3_bench <N>
    const int N_OPS = (argc > 1) ? std::atoi(argv[1]) : 300'000;

    // Tight price band → many crosses → matching path is stressed.
    // Wide band (e.g. 9000–11000) would stress resting/cancel instead.
    constexpr Price  LO  = 9'900, HI = 10'100; // $99.00–$101.00
    constexpr double MKT = 0.10;               // 10% market orders
    constexpr double CXL = 0.10;               // 10% cancels

    // Pre-generate once; each thread iterates the same vector so total work
    // per thread is identical regardless of the thread count being tested.
    const auto work = gen_work(N_OPS, /*seed=*/42, LO, HI, MKT, CXL);

    const std::vector<int> TCOUNTS = {1, 2, 4, 8};

    // ── warm-up (not timed, results discarded) ──────────────────────────────
    // Stabilises CPU frequency (avoids thermal throttle bias on first run)
    // and warms instruction/data caches.
    {
        auto warm = gen_work(40'000, 99, LO, HI, MKT, CXL);
        bench_book(1, warm);
        bench_engine(1, warm);
    }

    // ── header ──────────────────────────────────────────────────────────────
    const std::string RULE(72, '=');
    const std::string LINE(72, '-');

    std::cout
        << "\n" << RULE << "\n"
        << "  MatchCore Phase 3 — Throughput & Latency Benchmark\n"
        << RULE << "\n\n"
        << "  Ops/thread : " << comma(N_OPS) << "\n"
        << "  Mix        : 80% limit  |  10% market  |  10% cancel\n"
        << "  Prices     : $99.00 – $101.00  (tight band, high match rate)\n"
        << "  Qty/order  : 1 – 50  (uniform random)\n"
        << "  Clock      : std::chrono::steady_clock  (hardware timer)\n";

    // ── A) MatchingEngine ────────────────────────────────────────────────────
    std::cout
        << "\n" << LINE << "\n"
        << "  A)  MatchingEngine   MPSC queue + dedicated matching thread\n"
        << "      Producers only enqueue; matching is single-threaded.\n"
        << "      Expected: throughput flat — bounded by matching thread.\n"
        << LINE << "\n\n"
        << "  Threads │  Throughput (ops/s)  │  Trades      │  Trades/op\n"
        << "  ────────┼─────────────────────┼──────────────┼───────────\n";

    for (int t : TCOUNTS) {
        auto r = bench_engine(t, work);
        std::cout
            << "  " << std::setw(7) << r.threads                           << " │ "
            << std::setw(21) << comma(static_cast<long>(r.ops_per_s))      << " │ "
            << std::setw(12) << comma(r.n_trades)                          << " │ "
            << std::fixed << std::setprecision(3) << r.trades_per_op       << "\n";
    }

    // ── B) ConcurrentOrderBook ───────────────────────────────────────────────
    std::cout
        << "\n" << LINE << "\n"
        << "  B)  ConcurrentOrderBook   shared_mutex + N direct-access threads\n"
        << "      Every write serializes on unique_lock.\n"
        << "      Expected: throughput falls with N; p99 grows faster than mean\n"
        << "      (lock-convoy: a long match holds the lock while threads pile up).\n"
        << LINE << "\n\n"
        << "  Threads │  Throughput (ops/s)  │  Mean lat  │   p50 lat  │   p99 lat  │  Trades/op\n"
        << "  ────────┼─────────────────────┼────────────┼────────────┼────────────┼───────────\n";

    for (int t : TCOUNTS) {
        auto r = bench_book(t, work);
        std::cout
            << "  " << std::setw(7) << r.threads                           << " │ "
            << std::setw(21) << comma(static_cast<long>(r.ops_per_s))      << " │ "
            << std::setw(10) << ns_str(r.lat_mean_ns)                      << " │ "
            << std::setw(10) << ns_str(r.lat_p50_ns)                       << " │ "
            << std::setw(10) << ns_str(r.lat_p99_ns)                       << " │ "
            << std::fixed << std::setprecision(3) << r.trades_per_op       << "\n";
    }

    // ── how to read this ─────────────────────────────────────────────────────
    std::cout
        << "\n" << RULE << "\n"
        << "  How to read these results\n"
        << RULE << "\n\n"
        << "  Throughput (ops/s)\n"
        << "    MatchingEngine: stays roughly constant as threads increase.\n"
        << "    The matching thread is the ceiling; producers cannot outrun it.\n"
        << "    This is the design goal — the book is never contended.\n"
        << "\n"
        << "    ConcurrentOrderBook: falls with each added thread.\n"
        << "    Writers hold unique_lock for the entire match duration.\n"
        << "    N threads → N-1 sleeping at any moment — Amdahl's Law in action.\n"
        << "\n"
        << "  Latency (p50, p99)\n"
        << "    p50 reflects a typical uncontended lock acquisition + one match.\n"
        << "    p99 grows disproportionately with N — this is the lock-convoy:\n"
        << "    a single long match (many price levels swept) holds the lock while\n"
        << "    all other threads pile up. When it releases they all re-contend\n"
        << "    simultaneously, spiking tail latency.\n"
        << "\n"
        << "  Trades/op\n"
        << "    Fraction of submitted operations that generated a trade event.\n"
        << "    Should be stable across thread counts — it is a property of the\n"
        << "    workload (price distribution + qty mix), not the concurrency model.\n"
        << "    If it drifts, that could indicate a correctness issue.\n"
        << "\n"
        << "  Design choice\n"
        << "    MatchingEngine: maximum throughput, async submit, bounded latency.\n"
        << "    ConcurrentOrderBook: direct access, measurable per-call latency,\n"
        << "    concurrent reads (best_bid/best_ask share the lock).\n"
        << RULE << "\n\n";

    return 0;
}
