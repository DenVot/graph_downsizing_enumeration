#pragma once

#include "graph_downsizing/global_cut.hpp"

#include <functional>
#include <utility>

namespace downsizing {

class MaximalKVCCEnumerator {
public:
    // Input: a sorted connected induced k-core. Return a cut of size < k,
    // or an empty set if it is k-connected. Optional substitution for framework tests.
    using GlobalCut = std::function<VertexSet(const Graph&, const VertexSet&, std::size_t)>;

    // The graph must outlive the enumerator.
    // Without a substitution, uses Wen Algorithm 3 (GLOBAL-CUT*) with inherited strong-side information.
    explicit MaximalKVCCEnumerator(const Graph& graph, GlobalCut global_cut = {})
        : graph_(graph), global_cut_(std::move(global_cut)) {}

    // All inclusion-maximal induced k-  subsets, sorted lexicographically.
    [[nodiscard]] std::vector<VertexSet> enumerate(const VertexSet& vertices, std::size_t k) const;

private:
    [[nodiscard]] VertexSet k_core(const VertexSet& vertices, std::size_t k) const;
    [[nodiscard]] std::vector<VertexSet> connected_components(const VertexSet& vertices) const;
    [[nodiscard]] std::vector<VertexSet> overlap_partition(
        const VertexSet& vertices, const VertexSet& separator) const;
    [[nodiscard]] VertexSet global_cut(const VertexSet& vertices, std::size_t k,
                                       StrongSideCache& cache) const;

    const Graph& graph_;
    GlobalCut global_cut_;
};

} // namespace downsizing
