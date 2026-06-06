#pragma once
#include "Types.hpp"
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace matchcore {

// Not thread-safe. Designed to be owned exclusively by the matching thread.
// Thread safety is the MatchingEngine's responsibility via a request queue.
class OrderBook {
public:
    std::vector<Trade> add_limit(Order order);
    std::vector<Trade> add_market(Order order);
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    size_t bid_levels()  const { return bids_.size(); }
    size_t ask_levels()  const { return asks_.size(); }
    size_t order_count() const { return index_.size(); }

private:
    // Each price level is a FIFO queue of resting orders.
    // std::list chosen over std::deque because:
    //   1. Iterators remain stable after insertions/deletions elsewhere in the list.
    //   2. We store an iterator per order in the index for O(1) cancel.
    //   3. Front removal (pop_front) is O(1).
    // A deque would invalidate iterators on insert/erase, breaking the index.
    using PriceLevel = std::list<Order>;

    // std::map (red-black tree) chosen over a priority queue because:
    //   - O(log n) insert/delete/lookup for all operations, including cancel.
    //   - A heap gives O(1) peek at the best price but O(n) for arbitrary removal,
    //     which would make cancel unacceptably slow.
    //   - Bidirectional iteration lets us sweep price levels during matching.

    // std::greater<Price> → begin() is the highest price (best bid).
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    // Default less<Price>   → begin() is the lowest price (best ask).
    std::map<Price, PriceLevel>                       asks_;

    // Locator enables O(1) cancel: given an OrderId, we jump directly to the
    // list node and erase it, without scanning the book.
    struct Locator {
        Side             side;
        Price            price;
        PriceLevel::iterator pos;  // stable iterator into the price level's list
    };
    std::unordered_map<OrderId, Locator> index_;
};

} // namespace matchcore
