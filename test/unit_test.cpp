// Unit tests for OrderBook — single-threaded correctness.
// Covers: price-time priority, partial fills, market orders, cancellations.
//
// The concurrency tests live in consistency_test.cpp.
// These tests exercise the OrderBook directly (not via MatchingEngine or
// ConcurrentOrderBook) because the matching logic is the thing being proven.

#include "OrderBook.hpp"
#include <cassert>
#include <iostream>
#include <string>

using namespace matchcore;

// ─── Minimal test harness ─────────────────────────────────────────────────────

static int g_run = 0, g_fail = 0;
static const char* g_suite = "";

static void check(bool cond, const char* expr, int line) {
    ++g_run;
    if (!cond) {
        ++g_fail;
        std::cout << "    FAIL  [" << g_suite << "] line " << line << ": " << expr << "\n";
    }
}

#define EXPECT(cond)      check(!!(cond),    #cond,       __LINE__)
#define EXPECT_EQ(a, b)   check((a) == (b),  #a " == " #b, __LINE__)
#define EXPECT_TRUE(a)    check(!!(a),        #a,          __LINE__)
#define EXPECT_FALSE(a)   check(!(a),         "!" #a,      __LINE__)
#define SUITE(name)       do { g_suite = name; \
                               std::cout << "  " << name << "\n"; } while(0)

// ─── Order factories ──────────────────────────────────────────────────────────

static OrderId g_id = 1;

// Each call uses a strictly increasing timestamp so that submission order
// equals time-priority order, which is what price-time priority requires.
static Order limit(Side s, Price p, Quantity q) {
    return Order{g_id++, s, OrderType::Limit, p, q, q, g_id * 1000u};
}
static Order market(Side s, Quantity q) {
    return Order{g_id++, s, OrderType::Market, 0, q, q, g_id * 1000u};
}

// Helper: sum fill quantities across a trade vector.
static Quantity total_qty(const std::vector<Trade>& t) {
    Quantity q = 0;
    for (auto& tr : t) q += tr.qty;
    return q;
}

// ─── Tests ────────────────────────────────────────────────────────────────────

// A spread that doesn't cross → no trades, both orders rest.
static void test_no_match_open_spread() {
    SUITE("no_match_open_spread");
    OrderBook b;
    auto t1 = b.add_limit(limit(Side::Buy,  9'900, 100));
    auto t2 = b.add_limit(limit(Side::Sell, 10'100, 100));
    EXPECT(t1.empty());
    EXPECT(t2.empty());
    EXPECT_EQ(b.order_count(), 2u);
    EXPECT_EQ(*b.best_bid(), 9'900);
    EXPECT_EQ(*b.best_ask(), 10'100);
}

// A buy at exactly the ask price → immediate match.
static void test_crossing_limit_generates_trade() {
    SUITE("crossing_limit_generates_trade");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 100));   // resting ask
    auto trades = b.add_limit(limit(Side::Buy, 10'000, 100));
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 100u);
    EXPECT_EQ(b.order_count(), 0u);
}

// Trade always executes at the maker's (resting) price, not the taker's.
static void test_execution_price_is_maker_price() {
    SUITE("execution_price_is_maker_price");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 50));    // maker resting at 10000
    auto trades = b.add_limit(limit(Side::Buy, 10'200, 50)); // taker bids 10200
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10'000);            // fills at maker price, not 10200
}

// Within a single price level, orders must be served FIFO by submission time.
static void test_time_priority_within_price_level() {
    SUITE("time_priority_within_price_level");
    OrderBook b;
    // Two asks at the same price. The one with the smaller ts_ns arrives first.
    OrderId first_id  = g_id;
    Order first_ask   = {g_id++, Side::Sell, OrderType::Limit, 10'000, 100, 100, 1'000};
    Order second_ask  = {g_id++, Side::Sell, OrderType::Limit, 10'000, 100, 100, 2'000};
    b.add_limit(first_ask);
    b.add_limit(second_ask);

    // A small buy should match only the earlier (first) ask, not the second.
    auto trades = b.add_limit(limit(Side::Buy, 10'000, 50));
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, first_id);       // FIFO: first submitted wins
    EXPECT_EQ(b.order_count(), 2u);                // first ask partial, second intact
}

// Better-priced resting orders match before worse-priced ones, regardless of time.
static void test_price_priority_across_levels() {
    SUITE("price_priority_across_levels");
    OrderBook b;
    OrderId worse_id  = g_id;
    b.add_limit({g_id++, Side::Sell, OrderType::Limit, 10'200, 100, 100, 1'000}); // worse ask
    OrderId better_id = g_id;
    b.add_limit({g_id++, Side::Sell, OrderType::Limit, 10'000, 100, 100, 2'000}); // better ask (later!)

    // The better-priced ask (10000) should match first despite arriving later.
    auto trades = b.add_limit(limit(Side::Buy, 10'200, 50));
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, better_id);
    EXPECT_EQ(trades[0].price,    10'000);
    (void)worse_id;
}

// Taker qty > maker qty → taker partially filled, remainder posted to book.
static void test_taker_partial_fill_rests_in_book() {
    SUITE("taker_partial_fill_rests");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 40));    // 40 available
    auto trades = b.add_limit(limit(Side::Buy, 10'000, 100)); // wants 100

    EXPECT_EQ(total_qty(trades), 40u);             // only 40 filled
    EXPECT_EQ(b.order_count(), 1u);                // buy residual (60) rests
    EXPECT_EQ(*b.best_bid(), 10'000);
    EXPECT_FALSE(b.best_ask().has_value());        // ask fully consumed
}

// Taker qty < maker qty → maker stays in book with reduced quantity.
static void test_maker_partial_fill_stays_in_book() {
    SUITE("maker_partial_fill_stays");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 100));   // 100 available
    auto trades = b.add_limit(limit(Side::Buy, 10'000, 40)); // wants 40

    EXPECT_EQ(total_qty(trades), 40u);
    EXPECT_EQ(b.order_count(), 1u);                // ask still resting (60 remaining)
    EXPECT_EQ(*b.best_ask(), 10'000);
    EXPECT_FALSE(b.best_bid().has_value());
}

// A single large taker sweeps across multiple price levels.
static void test_multi_level_sweep() {
    SUITE("multi_level_sweep");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 50));
    b.add_limit(limit(Side::Sell, 10'100, 50));
    b.add_limit(limit(Side::Sell, 10'200, 50));

    auto trades = b.add_limit(limit(Side::Buy, 10'200, 150));

    EXPECT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 10'000);            // cheapest first
    EXPECT_EQ(trades[1].price, 10'100);
    EXPECT_EQ(trades[2].price, 10'200);
    EXPECT_EQ(total_qty(trades), 150u);
    EXPECT_EQ(b.order_count(), 0u);
}

// Limit order only crosses levels within its price limit.
static void test_limit_order_stops_at_price_boundary() {
    SUITE("limit_stops_at_price_boundary");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 50));
    b.add_limit(limit(Side::Sell, 10'100, 50));    // too expensive for this buyer

    auto trades = b.add_limit(limit(Side::Buy, 10'000, 100)); // won't pay 10100
    EXPECT_EQ(total_qty(trades), 50u);             // only the 10000 ask matched
    EXPECT_EQ(b.order_count(), 2u);                // 10100 ask + 50-unit bid residual
}

// Market order on an empty book → 0 trades, order not posted.
static void test_market_order_on_empty_book() {
    SUITE("market_order_on_empty_book");
    OrderBook b;
    auto trades = b.add_market(market(Side::Buy, 100));
    EXPECT(trades.empty());
    EXPECT_EQ(b.order_count(), 0u);                // residual NOT posted to book
}

// Market order fully fills against a resting limit.
static void test_market_order_full_fill() {
    SUITE("market_order_full_fill");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 50));
    auto trades = b.add_market(market(Side::Buy, 50));
    EXPECT_EQ(total_qty(trades), 50u);
    EXPECT_EQ(b.order_count(), 0u);
}

// Market order that cannot fully fill: residual quantity is silently discarded.
// Market orders must never rest in the book — a resting market order would
// execute at any price, which is semantically undefined.
static void test_market_order_residual_discarded() {
    SUITE("market_order_residual_discarded");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 30));
    auto trades = b.add_market(market(Side::Buy, 50));
    EXPECT_EQ(total_qty(trades), 30u);             // only 30 filled
    EXPECT_EQ(b.order_count(), 0u);                // remaining 20 NOT in book
}

// Market order sweeps multiple price levels without a price limit.
static void test_market_order_sweeps_multiple_levels() {
    SUITE("market_sweeps_multiple_levels");
    OrderBook b;
    b.add_limit(limit(Side::Sell, 10'000, 50));
    b.add_limit(limit(Side::Sell, 10'500, 50));
    b.add_limit(limit(Side::Sell, 11'000, 50));
    auto trades = b.add_market(market(Side::Buy, 150));
    EXPECT_EQ(total_qty(trades), 150u);
    EXPECT_EQ(b.order_count(), 0u);
}

// Cancelling a resting order removes it from the book.
static void test_cancel_resting_order() {
    SUITE("cancel_resting_order");
    OrderBook b;
    OrderId id = g_id;
    b.add_limit(limit(Side::Buy, 9'900, 100));
    EXPECT_EQ(b.order_count(), 1u);
    EXPECT_TRUE(b.cancel(id));
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_FALSE(b.best_bid().has_value());
}

// Cancelling an order that never existed returns false without crashing.
static void test_cancel_nonexistent_returns_false() {
    SUITE("cancel_nonexistent");
    OrderBook b;
    EXPECT_FALSE(b.cancel(99'999));
}

// After a full fill, the order is removed from the index.
// Attempting to cancel it should return false (not in book).
static void test_cancel_fully_matched_returns_false() {
    SUITE("cancel_fully_matched");
    OrderBook b;
    OrderId buy_id = g_id;
    b.add_limit(limit(Side::Buy,  10'000, 100));
    b.add_limit(limit(Side::Sell, 10'000, 100));   // crosses fully
    EXPECT_FALSE(b.cancel(buy_id));                // already removed from index
}

// An order that is partially filled still exists in the index.
// Cancelling it removes the residual quantity.
static void test_cancel_after_partial_fill() {
    SUITE("cancel_after_partial_fill");
    OrderBook b;
    OrderId buy_id = g_id;
    b.add_limit(limit(Side::Buy,  10'000, 100));   // resting bid
    b.add_limit(limit(Side::Sell, 10'000,  40));   // partial fill: 40 traded
    // 60 units of the buy should still be resting
    EXPECT_EQ(b.order_count(), 1u);
    EXPECT_TRUE(b.cancel(buy_id));
    EXPECT_EQ(b.order_count(), 0u);
}

// best_bid / best_ask always reflect the current best prices.
static void test_best_price_tracking() {
    SUITE("best_price_tracking");
    OrderBook b;
    b.add_limit(limit(Side::Buy,  9'800, 10));
    b.add_limit(limit(Side::Buy,  9'900, 10));
    b.add_limit(limit(Side::Buy, 10'000, 10));     // best bid
    EXPECT_EQ(*b.best_bid(), 10'000);

    b.add_limit(limit(Side::Sell, 10'200, 10));
    b.add_limit(limit(Side::Sell, 10'100, 10));    // best ask
    b.add_limit(limit(Side::Sell, 10'300, 10));
    EXPECT_EQ(*b.best_ask(), 10'100);

    // Consume best ask; second-best becomes new best
    b.add_limit(limit(Side::Buy, 10'100, 10));
    EXPECT_EQ(*b.best_ask(), 10'200);
}

// Empty book returns nullopt for both sides.
static void test_empty_book_queries() {
    SUITE("empty_book_queries");
    OrderBook b;
    EXPECT_FALSE(b.best_bid().has_value());
    EXPECT_FALSE(b.best_ask().has_value());
    EXPECT_EQ(b.order_count(), 0u);
}

// order_count accurately tracks the number of resting orders through
// submissions, matches, and cancellations.
static void test_order_count_consistency() {
    SUITE("order_count_consistency");
    OrderBook b;
    EXPECT_EQ(b.order_count(), 0u);

    OrderId id1 = g_id; b.add_limit(limit(Side::Buy, 9'900, 10)); // rests
    OrderId id2 = g_id; b.add_limit(limit(Side::Buy, 9'950, 10)); // rests
    EXPECT_EQ(b.order_count(), 2u);

    b.cancel(id1);
    EXPECT_EQ(b.order_count(), 1u);

    b.add_limit(limit(Side::Sell, 9'950, 10)); // matches id2 fully
    EXPECT_EQ(b.order_count(), 0u);

    b.cancel(id2); // already gone
    EXPECT_EQ(b.order_count(), 0u);
}

// Verifies maker_id and taker_id are correctly assigned in trade records.
static void test_maker_taker_ids_in_trade() {
    SUITE("maker_taker_ids");
    OrderBook b;
    OrderId maker_id = g_id;
    b.add_limit(limit(Side::Sell, 10'000, 100));   // resting = maker
    OrderId taker_id = g_id;
    auto trades = b.add_limit(limit(Side::Buy, 10'000, 100)); // aggressive = taker
    EXPECT_EQ(trades[0].maker_id, maker_id);
    EXPECT_EQ(trades[0].taker_id, taker_id);
}

// ─── Runner ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\nMatchCore — Unit Tests (OrderBook)\n"
              << "===================================\n\n";

    test_no_match_open_spread();
    test_crossing_limit_generates_trade();
    test_execution_price_is_maker_price();
    test_time_priority_within_price_level();
    test_price_priority_across_levels();
    test_taker_partial_fill_rests_in_book();
    test_maker_partial_fill_stays_in_book();
    test_multi_level_sweep();
    test_limit_order_stops_at_price_boundary();
    test_market_order_on_empty_book();
    test_market_order_full_fill();
    test_market_order_residual_discarded();
    test_market_order_sweeps_multiple_levels();
    test_cancel_resting_order();
    test_cancel_nonexistent_returns_false();
    test_cancel_fully_matched_returns_false();
    test_cancel_after_partial_fill();
    test_best_price_tracking();
    test_empty_book_queries();
    test_order_count_consistency();
    test_maker_taker_ids_in_trade();

    std::cout << "\n";
    if (g_fail == 0)
        std::cout << "ALL " << g_run << " CHECKS PASSED\n\n";
    else
        std::cout << g_fail << " of " << g_run << " CHECKS FAILED\n\n";

    return g_fail == 0 ? 0 : 1;
}
