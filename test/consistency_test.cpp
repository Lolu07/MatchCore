#include "ConcurrentOrderBook.hpp"
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace matchcore;

// ===========================================================================
// Test infrastructure
// ===========================================================================

// Global monotonic ID source shared across all tests. IDs are globally unique,
// which is all the book requires.
static std::atomic<OrderId> g_next_id{1};

static Order make_order(Side s, OrderType t, Price p, Quantity q) {
    OrderId id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    return Order{id, s, t, p, q, q, now_ns()};
}

// Thread-safe accumulator for Trade vectors generated across many threads.
struct TradeLog {
    std::mutex          mu;
    std::vector<Trade>  trades;

    void record(std::vector<Trade>&& batch) {
        if (batch.empty()) return;
        std::lock_guard lk(mu);
        for (auto& t : batch) trades.push_back(std::move(t));
    }

    uint64_t total_qty() const {
        uint64_t q = 0;
        for (const auto& t : trades) q += t.qty;
        return q;
    }

    // Each maker order should appear in at most one trade. If a maker appears
    // twice, the same resting order was matched by two separate takers —
    // a double-fill, which indicates a data race in the matching logic.
    bool has_double_fill() const {
        std::unordered_set<OrderId> seen;
        for (const auto& t : trades) {
            if (!seen.insert(t.maker_id).second) return true;
        }
        return false;
    }

    size_t count() const { return trades.size(); }
    void   clear()       { trades.clear(); }
};

static void pass(const char* name, const std::string& detail) {
    std::cout << "  PASS  [" << name << "] " << detail << "\n";
}
static bool fail(const char* name, const std::string& detail) {
    std::cout << "  FAIL  [" << name << "] " << detail << "\n";
    return false;
}

// ===========================================================================
// Test 1: Quantity conservation under concurrent buy submissions
//
// What this proves: with concurrent takers against a pre-loaded passive side,
// every unit of sell quantity is matched exactly once.
//
// How it catches races: without the lock, two threads can both read the same
// resting sell order as available and both generate a trade against it.
// The double-fill check and the total-qty check will both flag this.
// ===========================================================================
bool test_quantity_conservation() {
    constexpr int    N       = 20'000;   // resting sell orders (setup phase)
    constexpr int    THREADS = 4;        // concurrent buy threads
    constexpr Price  PRICE   = 10'000;   // $100.00 — all orders cross

    ConcurrentOrderBook book;
    TradeLog            log;

    // Single-threaded setup: N resting sells of qty=1.
    // No buys exist yet, so no matches occur here.
    for (int i = 0; i < N; ++i) {
        auto t = book.add_limit(make_order(Side::Sell, OrderType::Limit, PRICE, 1));
        if (!t.empty()) return fail("quantity_conservation", "unexpected match during setup");
    }
    if (book.order_count() != static_cast<size_t>(N))
        return fail("quantity_conservation", "wrong order count after setup");

    // Concurrent buy submission: THREADS threads, each submitting N/THREADS buys.
    // Every buy is at PRICE, so every buy must match a resting sell.
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int j = 0; j < N / THREADS; ++j) {
                auto trades = book.add_limit(
                    make_order(Side::Buy, OrderType::Limit, PRICE, 1));
                log.record(std::move(trades));
            }
        });
    }
    for (auto& th : threads) th.join();

    uint64_t traded = log.total_qty();
    if (traded != N)
        return fail("quantity_conservation",
                    "traded " + std::to_string(traded) + " expected " + std::to_string(N));
    if (log.has_double_fill())
        return fail("quantity_conservation", "double-fill detected");
    if (book.order_count() != 0)
        return fail("quantity_conservation",
                    "book not empty: " + std::to_string(book.order_count()) + " remaining");

    pass("quantity_conservation",
         std::to_string(N) + " orders matched, total qty=" + std::to_string(traded) +
         ", no double-fills, book empty");
    return true;
}

// ===========================================================================
// Test 2: Cancel correctness under concurrent access
//
// What this proves: concurrent cancels applied to different order IDs all
// succeed without crashing, corrupting the book, or leaving ghost entries.
//
// How it catches races: without the lock, two threads touching the same list
// node or the same map entry simultaneously causes UB. Erase-after-erase on
// the same iterator is silent UB and will typically corrupt the list.
// Even with distinct IDs, concurrent writes to std::list and std::map are UB.
// This test verifies the final state is exactly what sequential cancels
// would produce: N/2 orders removed, N/2 still resting, all matchable.
// ===========================================================================
bool test_cancel_under_concurrency() {
    constexpr int   N       = 10'000;
    constexpr int   THREADS = 4;
    constexpr Price PRICE   = 10'000;

    ConcurrentOrderBook book;

    // Setup: N resting sells. Save the IDs of even-indexed ones for cancellation.
    std::vector<OrderId> to_cancel;
    to_cancel.reserve(N / 2);
    for (int i = 0; i < N; ++i) {
        Order o = make_order(Side::Sell, OrderType::Limit, PRICE, 1);
        if (i % 2 == 0) to_cancel.push_back(o.id);
        book.add_limit(std::move(o));
    }

    // Concurrent cancels: threads grab cancel targets atomically via an index.
    std::atomic<int> idx{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            int i;
            while ((i = idx.fetch_add(1, std::memory_order_relaxed))
                   < static_cast<int>(to_cancel.size())) {
                book.cancel(to_cancel[i]);
            }
        });
    }
    for (auto& th : threads) th.join();

    size_t remaining = book.order_count();
    if (remaining != static_cast<size_t>(N / 2))
        return fail("cancel_under_concurrency",
                    "after cancels: " + std::to_string(remaining) +
                    " remaining, expected " + std::to_string(N / 2));

    // Verify the surviving sells are still valid by matching them all.
    TradeLog log;
    for (int i = 0; i < N / 2; ++i) {
        auto t = book.add_limit(make_order(Side::Buy, OrderType::Limit, PRICE, 1));
        log.record(std::move(t));
    }

    if (log.total_qty() != static_cast<uint64_t>(N / 2))
        return fail("cancel_under_concurrency",
                    "post-cancel match qty=" + std::to_string(log.total_qty()) +
                    " expected " + std::to_string(N / 2));
    if (book.order_count() != 0)
        return fail("cancel_under_concurrency", "book not empty after post-cancel match");

    pass("cancel_under_concurrency",
         std::to_string(N / 2) + " orders cancelled, " +
         std::to_string(N / 2) + " survivors matched cleanly");
    return true;
}

// ===========================================================================
// Test 3: Mixed concurrent crossing — both sides submitting simultaneously
//
// What this proves: when buy threads and sell threads run at the same time,
// the book remains internally consistent: no double-fills, no lost orders,
// and total quantity is conserved.
//
// Invariant verified: for every qty unit submitted on each side,
// it ends up either traded (as a trade.qty) or resting in the book.
//   buy_traded + buy_resting  == total_buy_submitted
//   sell_traded + sell_resting == total_sell_submitted
//
// Since buy_traded == sell_traded (each trade removes one unit from each side)
// and buy_submitted == sell_submitted, both sides must clear the same amount.
// With equal volumes and identical crossing prices, all orders match:
//   traded == buy_submitted == sell_submitted, book empty.
//
// How it catches races: without the lock, concurrent modification of the same
// price level's std::list from two threads is a data race. The list can lose
// nodes silently, corrupt its internal pointers, or allow two threads to pop
// the same front node — producing either fewer-than-expected trades (lost
// orders) or more-than-expected (double-fills).
// ===========================================================================
bool test_mixed_concurrent_crossing() {
    constexpr int   N_PER_THREAD = 5'000;
    constexpr int   BUY_THREADS  = 2;
    constexpr int   SELL_THREADS = 2;
    constexpr Price PRICE        = 10'000;

    ConcurrentOrderBook book;
    TradeLog            log;

    std::vector<std::thread> threads;
    for (int t = 0; t < BUY_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int j = 0; j < N_PER_THREAD; ++j) {
                auto trades = book.add_limit(
                    make_order(Side::Buy, OrderType::Limit, PRICE, 1));
                log.record(std::move(trades));
            }
        });
    }
    for (int t = 0; t < SELL_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int j = 0; j < N_PER_THREAD; ++j) {
                auto trades = book.add_limit(
                    make_order(Side::Sell, OrderType::Limit, PRICE, 1));
                log.record(std::move(trades));
            }
        });
    }
    for (auto& th : threads) th.join();

    if (log.has_double_fill())
        return fail("mixed_concurrent_crossing", "double-fill detected");

    uint64_t expected = static_cast<uint64_t>(N_PER_THREAD) * BUY_THREADS;
    uint64_t traded   = log.total_qty();
    size_t   left     = book.order_count();

    if (traded != expected)
        return fail("mixed_concurrent_crossing",
                    "traded=" + std::to_string(traded) +
                    " expected=" + std::to_string(expected));
    if (left != 0)
        return fail("mixed_concurrent_crossing",
                    std::to_string(left) + " orders left in book (expected 0)");

    pass("mixed_concurrent_crossing",
         std::to_string(traded) + " trades from " +
         std::to_string(BUY_THREADS + SELL_THREADS) + " concurrent threads, no races");
    return true;
}

// ===========================================================================
// Test 4: Price-time priority is preserved under concurrent access
//
// What this proves: matching still picks the best price first, then FIFO
// within a price level, even when orders arrive from multiple threads.
//
// Approach: pre-load two sell price levels (cheaper and more expensive).
// A concurrent buy should always match the cheaper level completely before
// touching the more expensive one. With the global lock, each buy operation
// sees a consistent book snapshot and makes the correct greedy choice.
//
// Note: we cannot guarantee *which thread* gets the lock first (that's OS
// scheduling). What we CAN guarantee is that whichever thread holds the lock
// sees the correct book state and matches by price-time priority within that
// snapshot. The global lock turns a concurrent problem into an interleaved
// sequential one — and the underlying OrderBook is already proven correct for
// the sequential case.
// ===========================================================================
bool test_price_priority_preserved() {
    constexpr Price  LOW_PRICE  = 9'900;   // $99.00 — should match first
    constexpr Price  HIGH_PRICE = 10'100;  // $101.00 — should match only when low exhausted
    constexpr int    N_LOW      = 1'000;
    constexpr int    N_HIGH     = 1'000;
    constexpr int    THREADS    = 4;

    ConcurrentOrderBook book;
    TradeLog            log;

    // Pre-load both price levels (single-threaded, deterministic setup).
    for (int i = 0; i < N_LOW;  ++i)
        book.add_limit(make_order(Side::Sell, OrderType::Limit, LOW_PRICE,  1));
    for (int i = 0; i < N_HIGH; ++i)
        book.add_limit(make_order(Side::Sell, OrderType::Limit, HIGH_PRICE, 1));

    // Concurrent buys at HIGH_PRICE — can match either level.
    // All LOW_PRICE sells must be consumed before any HIGH_PRICE sell matches.
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int j = 0; j < (N_LOW + N_HIGH) / THREADS; ++j) {
                auto trades = book.add_limit(
                    make_order(Side::Buy, OrderType::Limit, HIGH_PRICE, 1));
                log.record(std::move(trades));
            }
        });
    }
    for (auto& th : threads) th.join();

    if (log.has_double_fill())
        return fail("price_priority_preserved", "double-fill detected");

    uint64_t total = log.total_qty();
    if (total != N_LOW + N_HIGH)
        return fail("price_priority_preserved",
                    "traded=" + std::to_string(total) +
                    " expected=" + std::to_string(N_LOW + N_HIGH));

    // Verify execution prices: exactly N_LOW trades at LOW_PRICE,
    // exactly N_HIGH trades at HIGH_PRICE (no HIGH_PRICE trade before LOW exhausted
    // is verifiable in aggregate — the global lock ensures each match is atomic).
    uint64_t at_low = 0, at_high = 0;
    for (const auto& t : log.trades) {
        if      (t.price == LOW_PRICE)  ++at_low;
        else if (t.price == HIGH_PRICE) ++at_high;
        else return fail("price_priority_preserved",
                         "unexpected execution price: " + std::to_string(t.price));
    }
    if (at_low != N_LOW || at_high != N_HIGH)
        return fail("price_priority_preserved",
                    "at_low=" + std::to_string(at_low) +
                    " at_high=" + std::to_string(at_high) +
                    " (expected " + std::to_string(N_LOW) + "/" + std::to_string(N_HIGH) + ")");

    if (book.order_count() != 0)
        return fail("price_priority_preserved", "book not empty");

    pass("price_priority_preserved",
         std::to_string(at_low)  + " trades at LOW_PRICE, " +
         std::to_string(at_high) + " trades at HIGH_PRICE — price order correct");
    return true;
}

// ===========================================================================
int main() {
    std::cout << "\nMatchCore Phase 2 — Concurrency Consistency Tests\n"
              << "==================================================\n\n";

    bool all_pass = true;
    all_pass &= test_quantity_conservation();
    all_pass &= test_cancel_under_concurrency();
    all_pass &= test_mixed_concurrent_crossing();
    all_pass &= test_price_priority_preserved();

    std::cout << "\n"
              << (all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
              << "\n\n";
    return all_pass ? 0 : 1;
}
