#pragma once

#include "graph_downsizing/combinations.hpp"
#include "graph_downsizing/connectivity.hpp"
#include "graph_downsizing/worker_pool.hpp"

#include <memory>

namespace downsizing {
struct SearchStats {
    BigInt ranges_issued = 0;
    BigInt subsets_checked = 0;
};

// All nodes belong to one level k. References remain alive until every worker has stopped.
// Stops on the first success; the witness can vary with scheduling, the optimum cannot.
[[nodiscard]] std::optional<VertexSet> parallel_p_downsize(
    const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
    std::size_t r, std::size_t k, WorkerPool& pool, std::size_t chunk_size,
    SearchStats* stats = nullptr);
} // namespace downsizing
