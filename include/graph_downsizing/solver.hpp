#pragma once

#include "graph_downsizing/solution.hpp"
#include "graph_downsizing/parallel_search.hpp"
#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/worker_pool.hpp"

#include <string>

namespace downsizing {

struct SolveProgress {
    std::size_t k = 0;
    std::size_t level_done = 0;
    std::size_t level_total = 0;
    std::size_t overall_done = 0;
    std::size_t overall_total = 0;
    bool solved = false;
};
using SolveProgressCallback = std::function<void(const SolveProgress&)>;

// Graph and pool must outlive the solver. Builds its hierarchy once, then caches results by r.
class ExactSolver {
public:
    ExactSolver(const Graph& graph, std::size_t minimum_size, WorkerPool& pool,
                Checkpoint* checkpoint = nullptr, const std::string& graph_id = "",
                std::size_t range_size = default_range_size);
    [[nodiscard]] const Solution& solve(std::size_t r, SolveProgressCallback progress = {});
    [[nodiscard]] const SolutionsBySize& solutions() const noexcept { return solutions_; }
    [[nodiscard]] const NestingTree& tree() const noexcept { return tree_; }

private:
    const Graph& graph_;
    std::size_t minimum_size_;
    std::size_t range_size_;
    NestingTree tree_;
    SolutionsBySize solutions_;
    Checkpoint* checkpoint_;
    std::string graph_id_;
    const Solution& remember(std::size_t r, Solution solution);
    WorkerPool& pool_; // Non-owning; shared across graphs and r values by the coordinator.
};

} // namespace downsizing
