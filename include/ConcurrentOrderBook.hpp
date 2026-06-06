#pragma once
#include "OrderBook.hpp"
#include <shared_mutex>
#include <vector>

// ============================================================================
// Concurrency design rationale — read before changing the locking strategy
// ============================================================================
//
// STRATEGY CHOSEN: single std::shared_mutex (reader-writer lock)
//   • Write operations (add_limit, add_market, cancel) → std::unique_lock
//     Only one writer active at a time; all readers block during a write.
//   • Read operations (best_bid, best_ask, order_count) → std::shared_lock
//     Any number of readers proceed concurrently when no writer is active.
//
// This is the right tradeoff for an order book because:
//   - Market-data consumers (best_bid/ask queries) vastly outnumber order
//     submitters in most real workloads. shared_mutex lets them all run in
//     parallel during gaps between writes.
//   - The write path (matching) is inherently sequential: you cannot safely
//     run two concurrent match operations because they share and mutate the
//     same price levels. Any correct solution must serialize them.
//
// ============================================================================
// WHY PER-SIDE LOCKING (bid_mu_ + ask_mu_) DOES NOT HELP
// ============================================================================
//
// At first glance it seems attractive: protect bids separately from asks,
// so a buy thread and a sell thread could run concurrently. Here is why
// that reasoning fails:
//
//   Thread A — incoming buy order:
//     Step 1: must WRITE asks (consume resting sells during matching)
//     Step 2: must WRITE bids (insert unfilled remainder as resting buy)
//     → needs BOTH locks to complete the operation atomically.
//
//   Thread B — incoming sell order:
//     Step 1: must WRITE bids (consume resting buys during matching)
//     Step 2: must WRITE asks (insert unfilled remainder as resting sell)
//     → needs BOTH locks.
//
//   Naive acquisition:
//     Thread A: lock(ask_mu_) … lock(bid_mu_)   ← wants bid
//     Thread B: lock(bid_mu_) … lock(ask_mu_)   ← wants ask
//     → DEADLOCK.
//
//   Fix 1 — fixed lock order (always bid then ask):
//     Thread A: lock(bid_mu_), lock(ask_mu_) — must hold BOTH for the match.
//     Thread B: lock(bid_mu_), lock(ask_mu_) — same.
//     Both contend on bid_mu_ first → full serialization, same as one mutex.
//
//   Fix 2 — std::scoped_lock (deadlock-avoidance via try-lock loop):
//     scoped_lock(bid_mu_, ask_mu_) for both threads.
//     Still acquires both locks; still serializes on the first lock acquired.
//
//   Conclusion: per-side locking provides zero throughput improvement on the
//   matching hot path because matching inherently needs both sides atomically.
//   It only adds complexity and deadlock risk.
//
// ============================================================================
// WHY NOT LOCK-FREE
// ============================================================================
//
// A lock-free order book requires replacing std::map and std::list with
// concurrent structures: typically a lock-free skip list for price levels
// and hazard-pointer-protected linked lists for price level queues.
//
// The fundamental difficulty is atomicity: matching an order requires
// atomically reading the best price, decrementing a quantity, and possibly
// removing the price level — three separate memory locations. Without locks,
// you need multi-word CAS or careful use of epoch-based reclamation, both of
// which are notoriously hard to prove correct.
//
// More importantly, the MatchingEngine's MPSC queue model already eliminates
// lock contention on the book by making it single-threaded. ConcurrentOrderBook
// trades that off to allow direct multi-thread access, which is useful when you
// need the book to be a shared data structure rather than engine-owned.
//
// ============================================================================
// TRADE DISPATCH DESIGN
// ============================================================================
//
// Trades are returned by value, never dispatched inside the lock.
// If we held the lock and called a callback, any callback that called even
// a read-only method (best_bid) would deadlock: std::shared_mutex is not
// recursive, so a thread that holds unique_lock cannot re-enter for shared_lock.
// Returning by value lets callers dispatch trades after the lock is released.

namespace matchcore {

class ConcurrentOrderBook {
public:
    // Write operations — exclusive ownership of the book for their duration.
    // Returns the trades generated so the caller can dispatch them after
    // releasing the lock (see trade dispatch design note above).
    std::vector<Trade> add_limit(Order order);
    std::vector<Trade> add_market(Order order);
    bool               cancel(OrderId id);

    // Read operations — shared ownership; concurrent with other readers.
    std::optional<Price> best_bid()    const;
    std::optional<Price> best_ask()    const;
    size_t               order_count() const;

private:
    mutable std::shared_mutex mu_;  // mutable so const methods can take shared_lock
    OrderBook                 book_;
};

} // namespace matchcore
