#include "graph_downsizing/parallel_search.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace downsizing;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void invalid(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}

void combinations() {
    for (std::size_t n = 0; n <= 12; ++n) {
        for (std::size_t r = 0; r <= n; ++r) {
            std::vector<std::vector<std::size_t>> expected;
            for (unsigned mask = 0; mask < (1U << n); ++mask) {
                if (static_cast<std::size_t>(std::popcount(mask)) != r) continue;
                std::vector<std::size_t> subset;
                for (std::size_t i = 0; i < n; ++i) if (mask & (1U << i)) subset.push_back(i);
                expected.push_back(std::move(subset));
            }
            std::sort(expected.begin(), expected.end());
            check(combination_count(n, r) == expected.size(), "Wrong binomial count");
            auto indices = expected.front();
            for (std::size_t rank = 0; rank < expected.size(); ++rank) {
                check(unrank_combination(n, r, BigInt{rank}) == expected[rank], "Unranking differs from bitmask oracle");
                check(indices == expected[rank], "Combination iterator skipped or duplicated a subset");
                check(next_combination(indices, n) == (rank + 1 < expected.size()), "Wrong iterator termination");
            }
        }
    }
    const BigInt count("100891344545564193334812497256");
    check(combination_count(100, 50) == count, "Binomial count overflowed beyond 64 bits");
    const BigInt rank = BigInt{1} << 80;
    auto indices = unrank_combination(100, 50, rank);
    check(next_combination(indices, 100) && indices == unrank_combination(100, 50, rank + 1), "Large rank mismatch");
    auto last = unrank_combination(100, 50, count - 1);
    check(last.front() == 50 && last.back() == 99 && !next_combination(last, 100), "Wrong final big combination");
    invalid([&] { static_cast<void>(unrank_combination(100, 50, count)); });
    invalid([] { static_cast<void>(unrank_combination(5, 3, -1)); });
    invalid([] { static_cast<void>(unrank_combination(2, 3, 0)); });
    for (const std::size_t chunk : {1U,4U,100U}) {
        CombinationRanges ranges({5,7,2}, 3, chunk);
        std::vector<BigInt> next(3);
        const std::vector<BigInt> counts{10,35,0};
        while (const auto range = ranges.next()) {
            check(range->begin == next[range->node], "Missing or overlapping range");
            check(range->end > range->begin && range->end <= counts[range->node] && range->end - range->begin <= chunk,
                  "Range bounds wrong");
            next[range->node] = range->end;
        }
        check(next == counts && !ranges.next(), "Ranges did not cover all nodes");
    }
    CombinationRanges huge({100,101}, 50, 7);
    check(huge.next()->node == 0 && huge.next()->node == 1 && huge.next()->begin == 7, "Nodes must be scheduled round robin");
    invalid([] { CombinationRanges ranges({3}, 2, 0); });
}

void pool_and_search() {
    invalid([] { WorkerPool pool(0); });
    WorkerPool pool(4);
    std::barrier barrier(4);
    std::atomic<std::size_t> calls{0};
    for (int generation = 0; generation < 30; ++generation) {
        pool.run([&] { barrier.arrive_and_wait(); ++calls; });
    }
    check(calls == 120, "Pool lost jobs between generations");
    std::atomic<bool> fail{true};
    bool caught = false;
    try {
        pool.run([&] { barrier.arrive_and_wait(); if (fail.exchange(false)) throw std::runtime_error("worker failure"); });
    } catch (const std::runtime_error&) { caught = true; }
    check(caught, "Worker exception must reach the coordinator");
    pool.run([&] { ++calls; });
    check(calls == 124, "Pool must remain usable after an exception");

    std::vector<std::pair<Vertex,Vertex>> edges;
    VertexSet vertices;
    for (Vertex i = 0; i < 12; ++i) { edges.emplace_back(i, (i + 1) % 12); vertices.push_back(i); }
    const auto cycle = Graph::from_edges(edges);
    const auto node = std::make_shared<const VertexSet>(vertices);
    SearchStats stats;
    check(!parallel_p_downsize(cycle, {node}, 5, 2, pool, 7, &stats), "Proper induced cycle subset cannot be 2-connected");
    check(stats.subsets_checked == combination_count(12, 5) && stats.ranges_issued == 114,
          "One large node must be exhausted without missing or repeating subsets");
    const auto left = std::make_shared<const VertexSet>(VertexSet{0,1,2,3,4,5});
    const auto right = std::make_shared<const VertexSet>(VertexSet{6,7,8,9,10,11});
    check(!parallel_p_downsize(cycle, {left,right}, 3, 2, pool, 7, &stats) && stats.subsets_checked == 40,
          "All nodes of an unsuccessful level must be exhausted");
    edges.clear();
    for (Vertex i = 0; i < 12; ++i) for (Vertex j = i + 1; j < 12; ++j) edges.emplace_back(i,j);
    const auto clique = Graph::from_edges(edges);
    const auto answer = parallel_p_downsize(clique, {node}, 6, 5, pool, 1, &stats);
    check(answer && answer->size() == 6 && is_k_connected(clique, *answer, 5), "Invalid parallel witness");
    check(stats.ranges_issued <= pool.size() && stats.subsets_checked <= pool.size(), "Success must stop issuing ranges");
    check(!parallel_p_downsize(cycle, {node}, 5, 2, pool, 19, &stats) && stats.subsets_checked == 792,
          "Cancellation must not leak to the next level/search");
    check(!parallel_p_downsize(cycle, {node}, 5, 5, pool, 1, &stats) && stats.ranges_issued == 0,
          "Impossible levels must be skipped");
    check(!parallel_p_downsize(cycle, {}, 3, 2, pool, 1), "Empty level should finish");
    invalid([&] { static_cast<void>(parallel_p_downsize(cycle, {node}, 3, 2, pool, 0)); });
    WorkerPool single(1);
    const auto singleton = parallel_p_downsize(cycle, {node}, 1, 0, single, 1);
    check(singleton == std::optional<VertexSet>(VertexSet{0}), "Single worker singleton search");
}
} // namespace
int main() {
    try { combinations(); pool_and_search(); std::cout << "Combination ranges, arbitrary-precision ranks, workers, cancellation: passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
