#include "MatchingEngine.hpp"

namespace matchcore {

MatchingEngine::MatchingEngine(TradeHandler on_trade)
    : handler_(std::move(on_trade)) {}

MatchingEngine::~MatchingEngine() { stop(); }

void MatchingEngine::start() {
    stopping_ = false;
    thread_   = std::thread(&MatchingEngine::run, this);
}

void MatchingEngine::stop() {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    // join() returns only after the matching thread has processed all
    // remaining queued requests and exited run(), so the caller is guaranteed
    // all trades have been dispatched before stop() returns.
}

OrderId MatchingEngine::submit_limit(Side side, Price price, Quantity qty) {
    // fetch_add with relaxed ordering is safe here: we only need uniqueness,
    // not synchronisation with any other memory location.
    OrderId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    enqueue(NewOrder{Order{id, side, OrderType::Limit, price, qty, qty, now_ns()}});
    return id;
}

OrderId MatchingEngine::submit_market(Side side, Quantity qty) {
    OrderId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    enqueue(NewOrder{Order{id, side, OrderType::Market, 0, qty, qty, now_ns()}});
    return id;
}

void MatchingEngine::cancel(OrderId id) {
    enqueue(CancelReq{id});
}

void MatchingEngine::enqueue(Request req) {
    {
        std::lock_guard lk(mu_);
        queue_.push_back(std::move(req));
    }
    cv_.notify_one();
}

void MatchingEngine::run() {
    std::unique_lock lk(mu_);
    while (true) {
        cv_.wait(lk, [&] { return !queue_.empty() || stopping_; });

        // Batch drain: swap the entire queue out while holding the lock
        // for just this O(1) swap, then release before doing any real work.
        // Producers can enqueue new requests immediately — they never block
        // for the duration of a match.
        std::deque<Request> batch;
        batch.swap(queue_);
        lk.unlock();

        for (auto& req : batch) {
            std::visit([&](auto& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, NewOrder>) {
                    auto& o     = r.order;
                    auto trades = (o.type == OrderType::Market)
                                  ? book_.add_market(o)
                                  : book_.add_limit(o);
                    if (handler_) {
                        for (const auto& t : trades) handler_(t);
                    }
                } else {
                    book_.cancel(r.id);
                }
            }, req);
        }

        lk.lock();
        if (stopping_ && queue_.empty()) break;
    }
}

} // namespace matchcore
