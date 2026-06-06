#pragma once
#include <chrono>
#include <cstdint>

namespace matchcore {

// Prices stored as integer ticks (e.g. cents) — eliminates float rounding.
// $100.50 → 10050. This is how every real exchange represents prices.
using OrderId  = uint64_t;
using Price    = int64_t;
using Quantity = uint64_t;

enum class Side      : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;   // 0 for Market orders
    Quantity  qty;     // original quantity
    Quantity  leaves;  // remaining unfilled quantity
    uint64_t  ts_ns;   // submission timestamp — breaks price-level ties (time priority)
};

struct Trade {
    OrderId  maker_id;  // resting (passive) order
    OrderId  taker_id;  // incoming (aggressive) order
    Price    price;     // executes at the maker's price — standard exchange convention
    Quantity qty;
    uint64_t ts_ns;
};

inline uint64_t now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace matchcore
