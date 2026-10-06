#pragma once

#include "graph_downsizing/connectivity.hpp"

#include <memory>
#include <set>

namespace downsizing {

// Union of k scan-first forests (BFS order), removing each forest before the next.
// Requires k >= 1 and a connected induced subset with at least two vertices.
[[nodiscard]] Graph sparse_certificate(const Graph& graph, const VertexSet& vertices, std::size_t k);

// Wen Algorithm 3 with neighbor/group sweeps: a cut of size < k, or empty if the induced graph is k-connected.
// Requires a connected induced subset and 1 <= k < vertices.size().
[[nodiscard]] VertexSet find_global_cut(const Graph& graph, const VertexSet& vertices, std::size_t k);

// Diagnostic counters are reset for each call. Disable sweeps to compare Algorithm 2.
struct GlobalCutStats {
    std::size_t strong_checks = 0;
    std::size_t strong_reused = 0;
    std::size_t flow_calls = 0;
    std::size_t neighbor_sweeps = 0;
    std::size_t group_sweeps = 0;
    std::size_t group_pair_skips = 0;
};
[[nodiscard]] VertexSet find_global_cut_with_options(const Graph& graph, const VertexSet& vertices,
                                                    std::size_t k, bool sweeps, GlobalCutStats& stats);

// Immutable parent information shared by overlap-partition siblings.
struct StrongSideInfo {
    std::size_t k;
    VertexSet vertices;
    std::set<Vertex> strong;
    std::set<Vertex> recheck;
};
using StrongSideCache = std::shared_ptr<const StrongSideInfo>;

// Reads parent information and replaces it with information for this component.
// Parent must come from the overlap partition that produced this child.
// Recheck every separator vertex, plus parent strong vertices adjacent to the separator.
[[nodiscard]] VertexSet find_global_cut_reusing(const Graph& graph, const VertexSet& vertices,
                                               std::size_t k, StrongSideCache& cache,
                                               GlobalCutStats& stats);

} // namespace downsizing
