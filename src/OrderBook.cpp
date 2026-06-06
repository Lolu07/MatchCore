#include "OrderBook.hpp"

namespace matchcore {

// Price-time priority:
//   1. Price: best (most aggressive) price matches first.
//   2. Time: within a price level, earliest order matches first (FIFO via list::front).
//
// Execution price is always the maker's (resting) price. This is the standard
// convention: the passive order sets the price, the aggressive order accepts it.

std::vector<Trade> OrderBook::add_limit(Order order) {
    std::vector<Trade> trades;

    if (order.side == Side::Buy) {
        // Walk asks from lowest price upward; stop when ask > limit bid price.
        auto it = asks_.begin();
        while (it != asks_.end() && order.leaves > 0) {
            if (it->first > order.price) break;
            PriceLevel& lvl = it->second;
            while (!lvl.empty() && order.leaves > 0) {
                Order& maker = lvl.front();
                Quantity fill = std::min(order.leaves, maker.leaves);
                trades.push_back({maker.id, order.id, maker.price, fill, now_ns()});
                order.leaves -= fill;
                maker.leaves -= fill;
                if (maker.leaves == 0) {
                    index_.erase(maker.id);
                    lvl.pop_front();
                }
            }
            it = lvl.empty() ? asks_.erase(it) : std::next(it);
        }
        // Post unfilled remainder as a resting bid.
        if (order.leaves > 0) {
            auto& lvl = bids_[order.price];
            lvl.push_back(order);
            index_[order.id] = {Side::Buy, order.price, std::prev(lvl.end())};
        }
    } else {
        // Walk bids from highest price downward; stop when bid < limit ask price.
        auto it = bids_.begin();
        while (it != bids_.end() && order.leaves > 0) {
            if (it->first < order.price) break;
            PriceLevel& lvl = it->second;
            while (!lvl.empty() && order.leaves > 0) {
                Order& maker = lvl.front();
                Quantity fill = std::min(order.leaves, maker.leaves);
                trades.push_back({maker.id, order.id, maker.price, fill, now_ns()});
                order.leaves -= fill;
                maker.leaves -= fill;
                if (maker.leaves == 0) {
                    index_.erase(maker.id);
                    lvl.pop_front();
                }
            }
            it = lvl.empty() ? bids_.erase(it) : std::next(it);
        }
        if (order.leaves > 0) {
            auto& lvl = asks_[order.price];
            lvl.push_back(order);
            index_[order.id] = {Side::Sell, order.price, std::prev(lvl.end())};
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::add_market(Order order) {
    std::vector<Trade> trades;

    // Generic lambda works across the two map types (different comparators).
    // Market orders have no price limit, so we never break on price — we consume
    // until filled or liquidity is exhausted. Residual is silently discarded;
    // market orders must never rest in the book.
    auto fill_from = [&](auto& passive) {
        auto it = passive.begin();
        while (it != passive.end() && order.leaves > 0) {
            PriceLevel& lvl = it->second;
            while (!lvl.empty() && order.leaves > 0) {
                Order& maker = lvl.front();
                Quantity fill = std::min(order.leaves, maker.leaves);
                trades.push_back({maker.id, order.id, maker.price, fill, now_ns()});
                order.leaves -= fill;
                maker.leaves -= fill;
                if (maker.leaves == 0) {
                    index_.erase(maker.id);
                    lvl.pop_front();
                }
            }
            it = lvl.empty() ? passive.erase(it) : std::next(it);
        }
    };

    if (order.side == Side::Buy) fill_from(asks_);
    else                         fill_from(bids_);

    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto idx = index_.find(id);
    if (idx == index_.end()) return false;

    const Locator& loc = idx->second;

    // Erase the list node in O(1) using the stored iterator.
    // Then clean up the price level if it's now empty.
    if (loc.side == Side::Buy) {
        auto it = bids_.find(loc.price);
        it->second.erase(loc.pos);
        if (it->second.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(loc.price);
        it->second.erase(loc.pos);
        if (it->second.empty()) asks_.erase(it);
    }

    index_.erase(idx);
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return {};
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return {};
    return asks_.begin()->first;
}

} // namespace matchcore
