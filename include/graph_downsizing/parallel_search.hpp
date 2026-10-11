#pragma once

#include "graph_downsizing/combinations.hpp"
#include "graph_downsizing/connectivity.hpp"
#include "graph_downsizing/worker_pool.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace downsizing {
class Checkpoint;
// Called at level start and node completion, never for individual combinations.
using SearchProgressCallback = std::function<void(std::size_t k, std::size_t completed, std::size_t total)>;
struct NodeProgress {
    Checkpoint& checkpoint;
    std::string key;
};
struct SearchStats {
    std::size_t nodes_started = 0;
    BigInt subsets_checked = 0;
    std::size_t ranges_started = 0;
    std::size_t max_outstanding_ranges = 0;
};

// Enumerate r-subsets in lexicographic order; return the first witness with connectivity >= k.
[[nodiscard]] std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                                std::size_t r, std::size_t k,
                                                const std::atomic<bool>* stopped = nullptr, BigInt* checked = nullptr,
                                                NodeProgress* progress = nullptr);

// Each task processes a bounded range of combinations. All nodes belong to one level k.
// The coordinator keeps at most 2 * pool.size() accepted, unfinished tasks.
// References remain alive until every queued/running task has finished.
// Stops on the first success; the witness can vary with scheduling, the optimum cannot.
[[nodiscard]] std::optional<VertexSet> parallel_p_downsize(
    const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
    std::size_t r, std::size_t k, WorkerPool& pool,
    SearchStats* stats = nullptr, Checkpoint* checkpoint = nullptr,
    const std::string& prefix = {}, SearchProgressCallback progress = {});
} // namespace downsizing
