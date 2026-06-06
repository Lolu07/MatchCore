cmake_minimum_required(VERSION 3.20)
project(MatchCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# ── Core library ───────────────────────────────────────────────────────────────
add_library(matchcore
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/ConcurrentOrderBook.cpp
)
target_include_directories(matchcore PUBLIC include)
target_link_libraries(matchcore PUBLIC Threads::Threads)

# ── Demo ───────────────────────────────────────────────────────────────────────
add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE matchcore)

# ── Tests ──────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(unit_test test/unit_test.cpp)
target_link_libraries(unit_test PRIVATE matchcore)
add_test(NAME unit_test COMMAND unit_test)

add_executable(consistency_test test/consistency_test.cpp)
target_link_libraries(consistency_test PRIVATE matchcore)
add_test(NAME consistency_test COMMAND consistency_test)

# ── Benchmarks ─────────────────────────────────────────────────────────────────
add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE matchcore)

add_executable(phase3_bench bench/phase3_bench.cpp)
target_link_libraries(phase3_bench PRIVATE matchcore)
