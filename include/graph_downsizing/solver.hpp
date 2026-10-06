#pragma once

#include "graph_downsizing/solution.hpp"
#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/worker_pool.hpp"

#include <optional>

namespace downsizing {

// Enumerate r-subsets in lexicographic order; return the first witness with connectivity >= k.
[[nodiscard]] std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                                std::size_t r, std::size_t k);

// Graph must outlive the solver. Builds its hierarchy once, then caches results by r.
class ExactSolver {
public:
    ExactSolver(const Graph& graph, std::size_t minimum_size);
    ExactSolver(const Graph& graph, std::size_t minimum_size, WorkerPool& pool, std::size_t chunk_size = 256);
    [[nodiscard]] const Solution& solve(std::size_t r);
    [[nodiscard]] const SolutionsBySize& solutions() const noexcept { return solutions_; }
    [[nodiscard]] const NestingTree& tree() const noexcept { return tree_; }

private:
    const Graph& graph_;
    std::size_t minimum_size_;
    NestingTree tree_;
    SolutionsBySize solutions_;
    WorkerPool* pool_ = nullptr; // Non-owning; shared across graphs and r values by the coordinator.
    std::size_t chunk_size_ = 256;
};

} // namespace downsizing
