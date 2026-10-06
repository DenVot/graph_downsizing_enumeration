#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/maximal_kvcc_enumerator.hpp"

#include <stdexcept>

namespace downsizing {

NestingTree build_tree(const Graph& graph, std::size_t minimum_size,
                       MaximalKVCCEnumerator::GlobalCut global_cut) {
    if (minimum_size == 0 || minimum_size > graph.vertex_count()) {
        throw std::invalid_argument("Minimum tree-node size must be between 1 and the graph size");
    }
    VertexSet all;
    all.reserve(graph.vertex_count());
    for (const auto& entry : graph.adjacency()) {
        all.push_back(entry.first);
    }
    std::map<VertexSet, std::shared_ptr<const VertexSet>> sets;
    auto intern = [&](const VertexSet& vertices) {
        auto& stored = sets[vertices];
        if (!stored) {
            stored = std::make_shared<const VertexSet>(vertices);
        }
        return stored;
    };
    const MaximalKVCCEnumerator enumerator(graph, std::move(global_cut));
    NestingTree tree;
    tree.nodes.push_back({1, 0, intern(all), {}});
    tree.levels.resize(2);
    tree.levels[1].push_back(0);
    for (std::size_t parent = 0; parent < tree.nodes.size(); ++parent) {
        const auto k = tree.nodes[parent].level + 1;
        const auto children = enumerator.enumerate(*tree.nodes[parent].vertices, k);
        for (const auto& vertices : children) {
            if (vertices.size() < minimum_size) {
                continue;
            }
            const auto id = tree.nodes.size();
            tree.nodes[parent].children.push_back(id);
            tree.nodes.push_back({k, parent, intern(vertices), {}});
            if (tree.levels.size() <= k) {
                tree.levels.resize(k + 1);
            }
            tree.levels[k].push_back(id);
        }
    }
    return tree;
}

} // namespace downsizing
