// Tests for the read-only / acknowledged query APIs used by the API server:
//   OrderBook::snapshot, MatchingEngine::snapshot, MatchingEngine::cancel_with_ack.
//
// These APIs do not participate in matching; the tests check that they report
// the book faithfully and respect the engine's FIFO request ordering.

#include "MatchingEngine.hpp"
#include "OrderBook.hpp"
#include <iostream>
#include <mutex>
#include <vector>

using namespace matchcore;

static int g_run = 0, g_fail = 0;
static const char* g_suite = "";

static void check(bool cond, const char* expr, int line) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::cout << "    FAIL  [" << g_suite << "] line " << line << ": " << expr << "\n";
    }
}

#define EXPECT(cond)      check(!!(cond),   #cond,        __LINE__)
#define EXPECT_EQ(a, b)   check((a) == (b), #a " == " #b, __LINE__)
#define SUITE(name)       do { g_suite = name; \
                               std::cout << "  " << name << "\n"; } while(0)

static OrderId g_id = 1;
static Order limit(Side s, Price p, Quantity q) {
    return Order{g_id++, s, OrderType::Limit, p, q, q, g_id * 1000u};
}

// Levels come out best-price-first; orders within a level in FIFO order.
static void test_snapshot_ordering() {
    SUITE("snapshot_ordering");
    OrderBook b;
    auto b1 = limit(Side::Buy,  9'900, 10);
    auto b2 = limit(Side::Buy, 10'000, 20);
    auto b3 = limit(Side::Buy, 10'000, 30);
    auto a1 = limit(Side::Sell, 10'200, 5);
    auto a2 = limit(Side::Sell, 10'100, 7);
    for (auto& o : {b1, b2, b3, a1, a2}) b.add_limit(o);

    auto s = b.snapshot(10);
    EXPECT_EQ(s.order_count, 5u);
    EXPECT_EQ(s.bid_levels, 2u);
    EXPECT_EQ(s.ask_levels, 2u);
    EXPECT_EQ(s.bids.size(), 2u);
    EXPECT_EQ(s.bids[0].price, 10'000);
    EXPECT_EQ(s.bids[0].total_qty, 50u);
    EXPECT_EQ(s.bids[0].orders.size(), 2u);
    EXPECT_EQ(s.bids[0].orders[0].id, b2.id);   // earlier order first
    EXPECT_EQ(s.bids[0].orders[1].id, b3.id);
    EXPECT_EQ(s.bids[1].price, 9'900);
    EXPECT_EQ(s.asks[0].price, 10'100);
    EXPECT_EQ(s.asks[1].price, 10'200);
}

// max_levels truncates the ladder but not the full-book totals.
static void test_snapshot_depth_limit() {
    SUITE("snapshot_depth_limit");
    OrderBook b;
    for (Price p = 9'000; p < 9'010; ++p) b.add_limit(limit(Side::Buy, p, 1));
    auto s = b.snapshot(3);
    EXPECT_EQ(s.bids.size(), 3u);
    EXPECT_EQ(s.bids[0].price, 9'009);
    EXPECT_EQ(s.bid_levels, 10u);
    EXPECT_EQ(s.order_count, 10u);
    EXPECT(b.snapshot(0).bids.empty());
}

// Partial fills are reflected as reduced leaves.
static void test_snapshot_after_partial_fill() {
    SUITE("snapshot_after_partial_fill");
    OrderBook b;
    auto ask = limit(Side::Sell, 10'000, 100);
    b.add_limit(ask);
    b.add_limit(limit(Side::Buy, 10'000, 40));
    auto s = b.snapshot(10);
    EXPECT_EQ(s.asks.size(), 1u);
    EXPECT_EQ(s.asks[0].total_qty, 60u);
    EXPECT_EQ(s.asks[0].orders[0].id, ask.id);
    EXPECT_EQ(s.asks[0].orders[0].leaves, 60u);
    EXPECT(s.bids.empty());
}

// The engine's snapshot is queued behind earlier requests, so it observes
// every order and trade submitted before it (read-your-writes).
static void test_engine_snapshot_read_your_writes() {
    SUITE("engine_snapshot_read_your_writes");
    std::mutex mu;
    std::vector<Trade> trades;
    MatchingEngine e([&](const Trade& t) { std::lock_guard lk(mu); trades.push_back(t); });
    e.start();

    auto bid  = e.submit_limit(Side::Buy,  9'900, 10);
    auto ask  = e.submit_limit(Side::Sell, 10'100, 10);
    auto take = e.submit_limit(Side::Buy,  10'100, 4);   // crosses the ask

    auto s = e.snapshot(10).get();
    {
        std::lock_guard lk(mu);
        EXPECT_EQ(trades.size(), 1u);
        EXPECT_EQ(trades[0].maker_id, ask);
        EXPECT_EQ(trades[0].taker_id, take);
        EXPECT_EQ(trades[0].price, 10'100);
        EXPECT_EQ(trades[0].qty, 4u);
    }
    EXPECT_EQ(s.order_count, 2u);
    EXPECT_EQ(s.bids[0].orders[0].id, bid);
    EXPECT_EQ(s.asks[0].orders[0].leaves, 6u);
    e.stop();
}

static void test_engine_cancel_with_ack() {
    SUITE("engine_cancel_with_ack");
    MatchingEngine e;
    e.start();
    auto id = e.submit_limit(Side::Buy, 9'900, 10);
    EXPECT(e.cancel_with_ack(id).get());         // resting → cancelled
    EXPECT(!e.cancel_with_ack(id).get());        // already gone
    EXPECT(!e.cancel_with_ack(999'999).get());   // never existed
    EXPECT_EQ(e.snapshot(10).get().order_count, 0u);
    e.stop();
}

int main() {
    std::cout << "engine_query_test\n";
    test_snapshot_ordering();
    test_snapshot_depth_limit();
    test_snapshot_after_partial_fill();
    test_engine_snapshot_read_your_writes();
    test_engine_cancel_with_ack();
    std::cout << "\n" << (g_run - g_fail) << "/" << g_run << " checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
