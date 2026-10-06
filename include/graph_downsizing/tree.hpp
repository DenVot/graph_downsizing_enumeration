#pragma once

#include "graph_downsizing/maximal_kvcc_enumerator.hpp"

#include <memory>

namespace downsizing {

struct TreeNode {
    std::size_t level;
    std::size_t parent;
    std::shared_ptr<const VertexSet> vertices;
    std::vector<std::size_t> children;
};

struct NestingTree {
    std::vector<TreeNode> nodes;
    // Node IDs grouped by k. Index 0 is unused, the connected graph is at index 1.
    std::vector<std::vector<std::size_t>> levels;
};

// The root has parent == 0; identical sets at different levels share their storage.
[[nodiscard]] NestingTree build_tree(const Graph& graph, std::size_t minimum_size,
                                    MaximalKVCCEnumerator::GlobalCut global_cut = {});

} // namespace downsizing
