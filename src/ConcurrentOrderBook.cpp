#include "ConcurrentOrderBook.hpp"

namespace matchcore {

// ---- write operations (exclusive lock) ----

std::vector<Trade> ConcurrentOrderBook::add_limit(Order order) {
    std::unique_lock lk(mu_);
    return book_.add_limit(std::move(order));
    // Lock released here, before caller dispatches the returned trades.
}

std::vector<Trade> ConcurrentOrderBook::add_market(Order order) {
    std::unique_lock lk(mu_);
    return book_.add_market(std::move(order));
}

bool ConcurrentOrderBook::cancel(OrderId id) {
    std::unique_lock lk(mu_);
    return book_.cancel(id);
}

// ---- read operations (shared lock) ----

std::optional<Price> ConcurrentOrderBook::best_bid() const {
    std::shared_lock lk(mu_);
    return book_.best_bid();
}

std::optional<Price> ConcurrentOrderBook::best_ask() const {
    std::shared_lock lk(mu_);
    return book_.best_ask();
}

size_t ConcurrentOrderBook::order_count() const {
    std::shared_lock lk(mu_);
    return book_.order_count();
}

} // namespace matchcore
