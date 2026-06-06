#include "MatchingEngine.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace matchcore;
using namespace std::chrono;

// Usage: ./bench [num_threads] [orders_per_thread]
// Example: ./bench 4 500000
int main(int argc, char** argv) {
    const int num_threads       = (argc > 1) ? std::atoi(argv[1]) : 4;
    const int orders_per_thread = (argc > 2) ? std::atoi(argv[2]) : 500'000;
    const long total            = static_cast<long>(num_threads) * orders_per_thread;

    std::atomic<uint64_t> trade_count{0};

    MatchingEngine engine([&](const Trade&) {
        // relaxed: we only need a final total, not ordering guarantees
        trade_count.fetch_add(1, std::memory_order_relaxed);
    });
    engine.start();

    auto t0 = steady_clock::now();

    // Each producer thread submits a mix of random buy/sell limit orders
    // in a narrow price band to generate lots of crosses.
    std::vector<std::thread> producers;
    producers.reserve(num_threads);
    for (int tid = 0; tid < num_threads; ++tid) {
        producers.emplace_back([&, tid] {
            std::mt19937_64 rng(static_cast<uint64_t>(tid));
            // $99.00–$101.00: narrow enough to generate plentiful matches
            std::uniform_int_distribution<Price> px_dist(9'900, 10'100);
            std::uniform_int_distribution<int>   side_dist(0, 1);

            for (int i = 0; i < orders_per_thread; ++i) {
                engine.submit_limit(
                    side_dist(rng) ? Side::Buy : Side::Sell,
                    px_dist(rng),
                    10);
            }
        });
    }

    for (auto& t : producers) t.join();

    // stop() waits until the matching thread has processed every queued request.
    engine.stop();

    auto   t1      = steady_clock::now();
    double elapsed = duration<double>(t1 - t0).count();
    uint64_t trades = trade_count.load();

    std::cout << "Threads:    " << num_threads        << "\n"
              << "Orders:     " << total               << "\n"
              << "Trades:     " << trades              << "\n"
              << "Elapsed:    " << elapsed             << " s\n"
              << "Throughput: " << static_cast<long>(total / elapsed) << " orders/s\n";
}
