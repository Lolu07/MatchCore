# MatchCore

A high-performance, multithreaded limit order book matching engine in C++20.

**Live demo:** [matchcore.onrender.com](https://matchcore.onrender.com) — submit, cross and cancel orders against the C++ engine in the browser. *(Free hosting: the first load after a quiet period takes ~30 s while the server wakes up.)*

---

## What is a matching engine?

Every financial exchange — NYSE, Nasdaq, CME — runs a matching engine at its core. It maintains two sorted lists of resting orders: buyers willing to pay up to some price (the *bid* side) and sellers willing to accept at least some price (the *ask* side). When a new order arrives, the engine checks whether it crosses the opposite side and executes trades at the best available price. The correctness and speed of this system directly determine how fair and efficient the market is.

This project implements the full matching lifecycle: submission, price-time priority matching, partial fills, cancellation, and concurrent access — the same building blocks used in production exchange infrastructure.

---

## Features

- **Price-time priority** — best price first; FIFO within a price level
- **Limit and market orders** — limit orders rest in the book; market orders sweep and discard residual
- **O(1) order cancellation** — via a stable iterator stored in a hash-map index
- **Two thread-safety models** — MPSC queue engine or shared-mutex direct access (see Architecture)
- **~10 million orders/second** throughput on a single matching thread
- **Interactive order-book terminal** — a thin C++ HTTP server over `MatchingEngine` plus a React UI to submit, cross and cancel orders and watch the book react (see [Interactive simulator](#interactive-simulator))

---

## Architecture

### Order book data structures

```
BID side — sorted high→low           ASK side — sorted low→high
─────────────────────────────────    ─────────────────────────────────
std::map<Price, list<Order>,         std::map<Price, list<Order>>
         std::greater<Price>>
─────────────────────────────────    ─────────────────────────────────
$100.00 → [Order A] → [Order B]      $100.50 → [Order C]     ← best ask
 $99.50 → [Order D]                  $101.00 → [Order E] → [Order F]
 $99.00 → [Order G] → [Order H]

index_: unordered_map<OrderId, {side, price, list::iterator}>
         ↑ O(1) cancel lookup — jumps directly to the list node
```

**Why `std::map` over a heap?** A heap gives O(1) peek at the best price but O(n) arbitrary removal. Since cancel must remove any order in the book, O(n) cancel is unacceptable. `std::map` (red-black tree) gives O(log n) for every operation: insert, erase, lookup, and price-sorted iteration.

**Why `std::list` over `std::deque`?** `std::list` iterators are *stable* — they remain valid across any other insertions or deletions in the same list. This lets the cancel index store a live iterator per order for O(1) erase. `std::deque` invalidates iterators on structural modifications, which would corrupt the index.

### Concurrency models

Two designs are provided, optimised for different use cases:

```
  ── MatchingEngine (MPSC queue) ──────────────────────────────────────────

  Producer 1 ──┐
  Producer 2 ──┤ submit_limit()    ┌─ Matching Thread ──────────────────┐
  Producer N ──┘ ──→ [queue] ──────┤  batch drain (O(1) swap)           │
                   (mutex, brief)  │  OrderBook.add_limit() / cancel()  │
                                   │  ← no lock on the book itself       │
                                   └──────────────── Trade callback ─────┘

  ── ConcurrentOrderBook (shared_mutex) ───────────────────────────────────

  Thread 1 ──┐
  Thread 2 ──┤ add_limit()   ─→  unique_lock (write) ─→ OrderBook
  Thread N ──┘ best_bid()   ─→  shared_lock  (read)  ─→ OrderBook
```

**MatchingEngine** is the higher-throughput option. Multiple producer threads push requests into a mutex-protected deque; a single dedicated matching thread drains the queue using a *batch drain* — it swaps the entire deque out atomically (O(1) while holding the lock) then processes the batch without holding the lock. (The queue itself is mutex-protected, not lock-free.) The `OrderBook` itself is never locked, because only one thread ever touches it.

**ConcurrentOrderBook** wraps `OrderBook` with a `std::shared_mutex`. Write operations (`add_limit`, `add_market`, `cancel`) take an exclusive `unique_lock`; read queries (`best_bid`, `best_ask`) take a `shared_lock`, allowing concurrent reads during write-free intervals. Trades are returned by value and dispatched *after* releasing the lock — if they were dispatched inside the lock, any callback that queried the book would deadlock (non-recursive mutex).

---

## Benchmark results

**Workload:** 300,000 ops/thread · 80% limit / 10% market / 10% cancel · prices $99.00–$101.00 · qty 1–50  
**Hardware:** Apple Silicon (ARM64) · Release build (`-O2`) · `std::chrono::steady_clock`

![MatchCore benchmark — throughput and latency across thread counts](docs/benchmark.png)

*Left: MatchingEngine throughput stays near its single-thread ceiling regardless of producer count — the book is never locked. ConcurrentOrderBook throughput halves with each doubling of threads as writers serialise on `unique_lock`. Right: p50 latency barely moves (84→167 ns) while p99 explodes 450× (333 ns→155 µs) — the lock-convoy effect.*

### A) MatchingEngine — MPSC queue, dedicated matching thread

| Threads | Throughput (ops/s) | Trades | Trades/op |
|--------:|-------------------:|-------:|----------:|
| 1 | 10,311,965 | 229,047 | 0.763 |
| 2 | 10,385,931 | 458,193 | 0.764 |
| 4 | 7,211,507 | 917,329 | 0.764 |
| 8 | 6,556,677 | 1,836,971 | 0.765 |

Throughput is nearly flat at 1–2 threads, bounded by the single matching thread. The modest drop at 4–8 threads reflects contention on the *queue* mutex (not the book) as more producers compete to enqueue.

### B) ConcurrentOrderBook — `shared_mutex`, direct multi-thread access

| Threads | Throughput (ops/s) | Mean lat | p50 lat | p99 lat | Trades/op |
|--------:|-------------------:|---------:|--------:|--------:|----------:|
| 1 | 7,906,874 | 110 ns | 84 ns | 333 ns | 0.763 |
| 2 | 4,329,282 | 445 ns | 125 ns | 8.9 µs | 0.764 |
| 4 | 1,639,248 | 2.4 µs | 125 ns | 51.0 µs | 0.765 |
| 8 | 829,067 | 9.6 µs | 167 ns | 155.3 µs | 0.766 |

Throughput falls roughly in half with each doubling of threads — every writer serializes on `unique_lock`. The p99 column tells the more important story: tail latency grows **450× from 1 to 8 threads** while p50 grows only **2×**. This is the *lock-convoy effect*: one thread sweeping a large order across multiple price levels holds the lock while all other threads queue up, then they all re-contend simultaneously when it releases.

`Trades/op` is stable across all configurations (0.763–0.766). Because the workload is pre-generated from a fixed seed, the aggregate match rate is a property of the price distribution and must be concurrency-model-independent. Drift here would indicate a correctness bug (double-fill or lost order).

---

## Interactive simulator

A small web terminal for demonstrating the engine. **All matching still happens in C++** — the browser only sends commands and renders what the engine returns.

```
 Browser (React + TS, frontend/)
   │  fetch /api/...            prices as integer ticks; $ ↔ ticks only in the UI
   ▼
 Vite dev server  ── proxies /api ──►  matchcore_server  (server/, C++20, POSIX sockets)
                                        single HTTP thread = one producer
                                          │ submit_limit / submit_market
                                          │ cancel_with_ack   → future<bool>
                                          │ snapshot(depth)   → future<BookSnapshot>
                                          ▼
                                  MatchingEngine request queue  (mutex + deque, FIFO)
                                          ▼
                                  matching thread ── owns ──► OrderBook
                                          │ TradeHandler
                                          ▼
                                  TradeLog (mutex, bounded) ──► GET /api/trades
```

**How the adapter stays out of the engine's way**

- Orders go through the existing `submit_limit` / `submit_market`. Two additions were made to `MatchingEngine`, both as new request types on the *same* queue: `cancel_with_ack(id)` (returns whether the cancel hit a resting order) and `snapshot(max_levels)` (a copy of the top of the book). Because they are queued, they observe every earlier request (read-your-writes), and the `OrderBook` is still only ever touched by the matching thread — no lock is added to the book.
- `OrderBook::snapshot()` is a read-only copy of the top N levels, including each level's FIFO queue of `(order id, leaves)`. It is not on the matching hot path.
- After a `POST /api/orders`, the server waits on a snapshot future as a barrier: when it resolves, the order has been matched and all its trades delivered, so the response contains that order's fills and the post-trade book.
- The HTTP server is deliberately single-threaded (one request at a time, `Connection: close`) and binds to `127.0.0.1` by default. It is not part of the performance story — the engine is benchmarked without any network layer.

**Run it** (two terminals):

```bash
# 1. Engine + API server  (defaults: --host 127.0.0.1 --port 8080)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
./build/matchcore_server

# 2. Frontend  (Node 20+)
cd frontend
npm install
npm run dev            # open http://localhost:5173
```

If the server runs on another port, copy `frontend/.env.example` to `frontend/.env` and set `MATCHCORE_API`. `npm run build` type-checks and produces a static bundle in `frontend/dist/`; `npm run preview` serves it with the same proxy.

End-to-end check against a fresh server (standard-library Python, no packages):

```bash
python3 scripts/api_smoke_test.py            # resting orders → cross → fills → cancel (needs an unseeded server)
```

**API** — prices are integer ticks (`100 ticks = $1.00`), the engine's native representation.

| Method | Path | Body / query | Returns |
|--------|------|--------------|---------|
| `POST` | `/api/orders` | `{"side":"buy"\|"sell", "type":"limit"\|"market", "price":10050, "quantity":10}` (`price` for limit only) | order id, status, fills, post-trade book |
| `DELETE` | `/api/orders/<id>` | — | `cancelled: true/false`, book |
| `GET` | `/api/book` | `?depth=20` (max 100) | best bid/ask, spread, levels with FIFO order queues |
| `GET` | `/api/trades` | `?limit=50` (max 500) | recent fills, newest first: seq, price, qty, maker/taker id, aggressor side |
| `GET` | `/api/metrics` | — | orders submitted, fills, volume, resting orders, cancel counts |

### Deploying a public demo

The `Dockerfile` builds the frontend, builds and tests the C++ code, and produces one small image in which `matchcore_server` serves both the UI and `/api` from a single origin (`--static public`).

```bash
docker build -t matchcore .
docker run --rm -p 8080:8080 matchcore      # → http://localhost:8080
```

**Render (free tier):** push to GitHub → Render dashboard → **New → Blueprint** → pick this repo. `render.yaml` configures a Docker web service with a health check on `/api/health`. Render builds on every push and gives a `https://<name>.onrender.com` URL. Free instances sleep when idle, so the first visit after a quiet period takes ~30 s to wake up, and waking starts with an empty book.

Guards for running on the public internet (they bound resource use; matching is unaffected):

| Guard | Behaviour |
|-------|-----------|
| Write rate limit | 20-request burst, 5/s sustained per client IP on `POST`/`DELETE` → `429` |
| Resting-order cap | New limit orders rejected at 2,000 resting orders → `409` (market orders and cancels still work) |
| Response size | Each level lists at most the first 50 orders of its FIFO queue (`order_count` has the true total) |
| Reset | `POST /api/admin/reset` with header `X-Reset-Token: $MATCHCORE_RESET_TOKEN` restarts the engine with an empty book; disabled when the variable is unset |
| Static files | Paths containing `..` are rejected and every resolved file must lie inside the web root |

| Seeded book | With `--seed` (set in the Dockerfile), the server submits a fixed 24-order book — 5 levels a side around $100.00, 10 ticks apart — through `submit_limit` at startup and after a reset, so visitors land on a two-sided market |

Server options: `--host`, `--port`, `--static <dir>`, `--trust-proxy`, `--seed` (use `X-Forwarded-For` for the client IP; only set this behind a proxy you trust), plus env `HOST`, `PORT`, `MATCHCORE_RESET_TOKEN`.

The benchmark table in the UI is **static** — the recorded numbers from this README (`frontend/src/data/benchmarks.json`), labelled as such. Nothing in the UI is a live throughput measurement.

---

## Design decisions

### Integer prices

`Price = int64_t` in tick units (e.g. cents: `$100.50 → 10050`). Floating-point prices accumulate rounding errors across millions of operations and differ between hardware and compilers. Every real exchange uses an integer tick representation.

### Maker/taker pricing

Trades execute at the *maker's* (resting order's) price, not the taker's. This is the universal exchange convention: the passive side sets the price; the aggressive side accepts it. A buy limit at $101 that crosses a resting ask at $100 executes at $100, giving the taker price improvement.

### Why per-side locking (bid lock + ask lock) doesn't improve throughput

It looks attractive: separate the two sides, let buys and sells run in parallel. It fails because matching always touches *both* sides atomically — a buy must write the ask side (consume resting sells) and then write the bid side (insert any residual). Both operations need both locks simultaneously. Acquiring them in a fixed order to prevent deadlock forces both buy threads and sell threads to contend on the same first lock, serializing them identically to a single global mutex — with added complexity and no benefit.

### Batch drain pattern

The matching thread swaps the entire request queue out with a fresh empty deque while holding the lock for that O(1) swap, then releases before doing any real work. Producers can immediately resume enqueuing. This decouples submission latency from match latency: no producer ever waits for a complex multi-level match to complete.

### Market order residual handling

Unfilled market quantity is silently discarded. A resting market order would execute against the next incoming order at any price, which is semantically undefined. All real exchanges handle this the same way.

---

## Project structure

```
matchcore/
├── include/
│   ├── Types.hpp               — Order, Trade, integer Price, now_ns()
│   ├── OrderBook.hpp           — Core book: map + list + cancel index
│   ├── MatchingEngine.hpp      — MPSC queue + dedicated matching thread
│   └── ConcurrentOrderBook.hpp — shared_mutex wrapper; design rationale
├── src/
│   ├── OrderBook.cpp           — Price-time priority matching loops
│   ├── MatchingEngine.cpp      — Batch drain, std::visit dispatch
│   ├── ConcurrentOrderBook.cpp — Thin lock wrappers
│   └── main.cpp                — Demo: build a book, cross, sweep, cancel
├── server/
│   ├── main.cpp                — matchcore_server: HTTP API adapter over MatchingEngine
│   ├── HttpServer.{hpp,cpp}    — Minimal single-threaded HTTP/1.1 over POSIX sockets
│   ├── StaticFiles.hpp         — Serves the built UI (single-origin deployment)
│   └── Json.hpp                — Output escaping + flat request-object parser
├── frontend/                   — React + TypeScript + Vite order-book terminal
├── Dockerfile / render.yaml    — One-image deployment (UI + API) and Render blueprint
├── scripts/
│   └── api_smoke_test.py       — End-to-end API check (order → cross → cancel)
├── test/
│   ├── unit_test.cpp           — 21 single-threaded correctness tests (62 checks)
│   ├── consistency_test.cpp    — 4 multithreaded invariant tests
│   └── engine_query_test.cpp   — Snapshot / acknowledged-cancel API tests
└── bench/
    ├── bench.cpp               — Quick MatchingEngine throughput sweep
    └── phase3_bench.cpp        — Full throughput + latency comparison
```

---

## Build and run

**Requirements:** CMake ≥ 3.20, a C++20 compiler (GCC 13+ or Clang 16+), POSIX threads.

```bash
# Configure and build (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# Run all tests
ctest --test-dir build --output-on-failure

# Run tests individually
./build/unit_test           # single-threaded correctness (62 checks)
./build/consistency_test    # multithreaded invariants
./build/engine_query_test   # snapshot + acknowledged cancel

# Run the demo
./build/demo

# Run benchmarks
./build/phase3_bench              # full throughput + latency table (default: 300k ops/thread)
./build/phase3_bench 1000000      # higher N for more stable numbers
./build/bench 4 500000            # quick MatchingEngine sweep: 4 threads, 500k ops each
```

---

## Tech stack

- **Language:** C++20 (concepts, `std::variant`, designated initialisers, `if constexpr`)
- **Concurrency:** `std::thread`, `std::mutex`, `std::shared_mutex`, `std::atomic`
- **Build:** CMake 3.20+ with CTest integration
- **Dependencies:** C++ standard library only (plus POSIX sockets for the API server) — no third-party libraries
- **Frontend (optional):** React + TypeScript + Vite, no UI libraries

---

## Future improvements

| Area | Description |
|------|-------------|
| Lock-free queue | Replace the mutex-protected `std::deque` in `MatchingEngine` with a lock-free MPSC queue (e.g. Dmitry Vyukov's intrusive queue). Eliminates the queue-contention drop seen at 4+ producer threads. |
| Memory pools | Allocate `std::list` and `std::map` nodes from a slab allocator. Eliminates per-node `malloc` on the hot path, which can dominate at high order rates. |
| Multiple instruments | Run one `MatchingEngine` per instrument in parallel. The single-threaded book-per-instrument model is how most production systems achieve horizontal scaling. |
| IOC / FOK order types | Immediate-or-cancel (fill what you can, cancel the rest) and fill-or-kill (fill everything or cancel the entire order) are standard exchange order types. |
| Order book snapshots | Periodic consistent snapshots of the full depth for market data distribution, without blocking the matching thread. |
