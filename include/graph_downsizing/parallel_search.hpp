#pragma once

#include "graph_downsizing/combinations.hpp"
#include "graph_downsizing/connectivity.hpp"
#include "graph_downsizing/worker_pool.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace downsizing {
class Checkpoint;
struct NodeProgress {
    Checkpoint& checkpoint;
    std::string key;
};
struct SearchStats {
    std::size_t nodes_started = 0;
    BigInt subsets_checked = 0;
};

// Enumerate r-subsets in lexicographic order; return the first witness with connectivity >= k.
[[nodiscard]] std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                                std::size_t r, std::size_t k,
                                                const std::atomic<bool>* stopped = nullptr, BigInt* checked = nullptr,
                                                NodeProgress* progress = nullptr);

// Each task processes one entire node sequentially. All nodes belong to one level k.
// References remain alive until every queued/running task has finished.
// Stops on the first success; the witness can vary with scheduling, the optimum cannot.
[[nodiscard]] std::optional<VertexSet> parallel_p_downsize(
    const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
    std::size_t r, std::size_t k, WorkerPool& pool,
    SearchStats* stats = nullptr, Checkpoint* checkpoint = nullptr,
    const std::string& prefix = {});
} // namespace downsizing
