#pragma once
#include "OrderBook.hpp"
#include "Types.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>

namespace matchcore {

// Thread-safety model: MPSC (multi-producer, single-consumer).
//
// Multiple threads submit orders to a mutex-protected queue.
// A single dedicated matching thread drains the queue and runs the order book.
// This means the OrderBook itself needs no locks — it is accessed exclusively
// by the matching thread. Contention only occurs on the queue, and only briefly.
//
// Alternative considered: mutex directly on the OrderBook.
// Rejected because matching can be slow (O(k) trades), so a book-level lock
// would block producers for the entire duration of a match, killing throughput.
// The queue approach decouples submission latency from match latency.
class MatchingEngine {
public:
    using TradeHandler = std::function<void(const Trade&)>;

    explicit MatchingEngine(TradeHandler on_trade = nullptr);
    ~MatchingEngine();

    void start();
    void stop();   // drains the queue, then joins the matching thread

    // Thread-safe. OrderId is assigned atomically before enqueuing,
    // so callers get the ID immediately without waiting for matching.
    OrderId submit_limit(Side side, Price price, Quantity qty);
    OrderId submit_market(Side side, Quantity qty);

    // Async: the cancel is queued alongside orders to preserve ordering.
    // In a real exchange, you'd send an ack; we omit that here for brevity.
    void cancel(OrderId id);

private:
    struct NewOrder  { Order order; };
    struct CancelReq { OrderId id;  };
    using  Request = std::variant<NewOrder, CancelReq>;

    void enqueue(Request req);
    void run();

    TradeHandler handler_;
    OrderBook    book_;     // only touched by matching thread

    // Queue — mutex guards this deque plus the stopping flag.
    // The matching thread swaps the whole deque out while holding the lock
    // (an O(1) swap), then processes the batch without the lock.
    // This "batch drain" minimises the time producers are blocked.
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<Request>     queue_;
    bool                    stopping_{false};

    std::thread          thread_;
    std::atomic<OrderId> next_id_{1};
};

} // namespace matchcore
