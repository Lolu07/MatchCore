#include "MatchingEngine.hpp"
#include <iomanip>
#include <iostream>

using namespace matchcore;

int main() {
    // Prices in cents: 10000 = $100.00
    MatchingEngine engine([](const Trade& t) {
        std::cout << "TRADE  maker=" << std::setw(3) << t.maker_id
                  << "  taker=" << std::setw(3) << t.taker_id
                  << "  price=" << t.price
                  << "  qty="   << t.qty << "\n";
    });
    engine.start();

    // Build a bid stack
    auto b1 = engine.submit_limit(Side::Buy,  9'800, 100);  // $98.00
    auto b2 = engine.submit_limit(Side::Buy,  9'900, 200);  // $99.00
    auto b3 = engine.submit_limit(Side::Buy, 10'000, 150);  // $100.00 ← best bid

    // Build an ask stack
    auto a1 = engine.submit_limit(Side::Sell, 10'200,  80); // $102.00
    auto a2 = engine.submit_limit(Side::Sell, 10'100, 120); // $101.00 ← best ask

    // Aggressive sell at $100.00 — crosses b3, should generate a trade
    engine.submit_limit(Side::Sell, 10'000, 100);

    // Market buy — sweeps all available asks (a2 then a1)
    engine.submit_market(Side::Buy, 200);

    // Cancel resting bids
    engine.cancel(b1);
    engine.cancel(b2);

    // stop() drains the queue and joins the matching thread, so all
    // trade callbacks are guaranteed to have fired before this returns.
    engine.stop();

    std::cout << "\nAll orders processed. Unused IDs: b3=" << b3
              << " a1=" << a1 << " a2=" << a2 << "\n";
}
