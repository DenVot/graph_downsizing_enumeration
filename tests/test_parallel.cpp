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
                check(indices == expected[rank], "Combination iterator skipped or duplicated a subset");
                check(next_combination(indices, n) == (rank + 1 < expected.size()), "Wrong iterator termination");
            }
        }
    }
    const BigInt count("100891344545564193334812497256");
    check(combination_count(100, 50) == count, "Binomial count overflowed beyond 64 bits");

}

void pool_and_search() {
    invalid([] { WorkerPool pool(0); });
    WorkerPool pool(4);
    std::barrier barrier(4);
    std::atomic<std::size_t> calls{0};
    for (int generation = 0; generation < 30; ++generation) {
        for (int i = 0; i < 4; ++i) pool.submit([&] { barrier.arrive_and_wait(); ++calls; });
        pool.wait();
    }
    check(calls == 120, "Pool lost jobs between generations");
    std::atomic<bool> fail{true};
    bool caught = false;
    try {
        for (int i = 0; i < 4; ++i) pool.submit([&] { barrier.arrive_and_wait(); if (fail.exchange(false)) throw std::runtime_error("worker failure"); });
        pool.wait();
    } catch (const std::runtime_error&) { caught = true; }
    check(caught, "Worker exception must reach the coordinator");
    for (int i = 0; i < 4; ++i) pool.submit([&] { ++calls; });
    pool.wait();
    check(calls == 124, "Pool must remain usable after an exception");
    std::vector<int> visits(1000);
    for (std::size_t i = 0; i < visits.size(); ++i) pool.submit([&, i] { ++visits[i]; });
    pool.wait();
    check(std::all_of(visits.begin(), visits.end(), [](int n) { return n == 1; }), "Tasks must execute exactly once");
    { WorkerPool draining(2); for (int i = 0; i < 20; ++i) draining.submit([&] { ++calls; }); }
    check(calls == 144, "Destructor must drain accepted tasks");


    std::vector<std::pair<Vertex,Vertex>> edges;
    VertexSet vertices;
    for (Vertex i = 0; i < 12; ++i) { edges.emplace_back(i, (i + 1) % 12); vertices.push_back(i); }
    const auto cycle = Graph::from_edges(edges);
    const auto node = std::make_shared<const VertexSet>(vertices);
    SearchStats stats;
    check(!parallel_p_downsize(cycle, {node}, 5, 2, pool, &stats), "Proper induced cycle subset cannot be 2-connected");
    check(stats.subsets_checked == combination_count(12, 5) && stats.nodes_started == 1,
          "One large node must be exhausted without missing or repeating subsets");
    const auto left = std::make_shared<const VertexSet>(VertexSet{0,1,2,3,4,5});
    const auto right = std::make_shared<const VertexSet>(VertexSet{6,7,8,9,10,11});
    check(!parallel_p_downsize(cycle, {left,right}, 3, 2, pool, &stats) && stats.subsets_checked == 40 && stats.nodes_started == 2,
          "All nodes of an unsuccessful level must be exhausted");
    edges.clear();
    for (Vertex i = 0; i < 12; ++i) for (Vertex j = i + 1; j < 12; ++j) edges.emplace_back(i,j);
    const auto clique = Graph::from_edges(edges);
    const auto answer = parallel_p_downsize(clique, {node}, 6, 5, pool, &stats);
    check(answer && answer->size() == 6 && is_k_connected(clique, *answer, 5), "Invalid parallel witness");
    check(stats.nodes_started == 1 && stats.subsets_checked == 1, "One node must be one sequential task");
    std::vector<std::shared_ptr<const VertexSet>> many_nodes(100, node);
    check(parallel_p_downsize(clique, many_nodes, 6, 5, pool, &stats).has_value()
          && stats.nodes_started <= pool.size(), "Success must cancel remaining nodes");
    check(!parallel_p_downsize(cycle, {node}, 5, 2, pool, &stats) && stats.subsets_checked == 792,
          "Cancellation must not leak to the next level/search");
    check(!parallel_p_downsize(cycle, {node}, 5, 5, pool, &stats) && stats.nodes_started == 0,
          "Impossible levels must be skipped");
    check(!parallel_p_downsize(cycle, {}, 3, 2, pool), "Empty level should finish");
    invalid([&] { static_cast<void>(parallel_p_downsize(cycle, {node}, 0, 2, pool)); });
    WorkerPool single(1);
    const auto singleton = parallel_p_downsize(cycle, {node}, 1, 0, single);
    check(singleton == std::optional<VertexSet>(VertexSet{0}), "Single worker singleton search");
}
} // namespace
int main() {
    try { combinations(); pool_and_search(); std::cout << "Combinations, queued workers, sequential nodes, cancellation: passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
