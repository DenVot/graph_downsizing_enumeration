#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/checkpoint.hpp"
#include "graph_downsizing/maximal_kvcc_enumerator.hpp"

#include <algorithm>

#include <stdexcept>

namespace downsizing {

NestingTree build_tree(const Graph& graph, std::size_t minimum_size,
                       Checkpoint* checkpoint, const std::string& graph_id) {
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
    const MaximalKVCCEnumerator enumerator(graph, {}, [checkpoint] {
        if (checkpoint) {
            checkpoint->check();
        }
    });
    NestingTree tree;
    tree.nodes.push_back({1, 0, intern(all), {}});
    tree.levels.resize(2);
    tree.levels[1].push_back(0);
    std::size_t next_parent = 0;
    const auto key = "tree/" + graph_id;
    if (checkpoint) {
        const auto saved = checkpoint->get(key);
        if (!saved.is_null()) {
            if (saved.at("minimum_size").get<std::size_t>() != minimum_size) {
                throw std::runtime_error("Checkpoint tree minimum size mismatch");
            }
            tree = {};
            std::map<std::size_t, std::shared_ptr<const VertexSet>> restored_sets;
            const auto count = saved.at("node_count").get<std::size_t>();
            for (std::size_t id = 0; id < count; ++id) {
                const auto item = checkpoint->get(key + "/node/" + std::to_string(id));
                const auto level = item.at("level").get<std::size_t>();
                const auto parent = item.at("parent").get<std::size_t>();
                const auto set_id = item.at("set").get<std::size_t>();
                if (set_id > id) {
                    throw std::runtime_error("Invalid checkpoint vertex-set reference");
                }
                if (!restored_sets.contains(set_id)) {
                    const auto vertices = checkpoint->get(key + "/set/" + std::to_string(set_id)).get<VertexSet>();
                    if (vertices.empty() || ordered_subset(graph, vertices) != vertices) {
                        throw std::runtime_error("Invalid checkpoint tree vertices");
                    }
                    restored_sets[set_id] = intern(vertices);
                }
                const auto vertices = restored_sets.at(set_id);
                if (id == 0) {
                    if (level != 1 || parent != 0 || *vertices != all) {
                        throw std::runtime_error("Invalid checkpoint root");
                    }
                } else {
                    if (parent >= id || level != tree.nodes[parent].level + 1 || vertices->size() < minimum_size ||
                        !std::includes(tree.nodes[parent].vertices->begin(), tree.nodes[parent].vertices->end(),
                                       vertices->begin(), vertices->end())) {
                        throw std::runtime_error("Invalid checkpoint tree node");
                    }
                    tree.nodes[parent].children.push_back(id);
                }
                tree.nodes.push_back({level, parent, vertices, {}});
                if (tree.levels.size() <= level) {
                    tree.levels.resize(level + 1);
                }
                tree.levels[level].push_back(id);
            }
            next_parent = saved.at("next_parent").get<std::size_t>();
            if (tree.nodes.empty() || next_parent > tree.nodes.size()) {
                throw std::runtime_error("Invalid tree cursor");
            }
        }
    }
    // Append only new nodes/sets. Publishing the entire growing tree after each
    // parent would cost quadratic copying for graphs with many small components.
    std::map<const VertexSet*, std::size_t> ids;
    std::size_t published = 0;
    if (checkpoint && !checkpoint->get(key).is_null()) {
        published = tree.nodes.size();
        for (std::size_t id = 0; id < published; ++id) {
            ids.emplace(tree.nodes[id].vertices.get(), id);
        }
    }
    const auto publish = [&] {
        if (!checkpoint) {
            return;
        }
        std::map<std::string, nlohmann::json> updates;
        for (std::size_t id = published; id < tree.nodes.size(); ++id) {
            const auto& node = tree.nodes[id];
            const auto [it, added] = ids.emplace(node.vertices.get(), id);
            if (added) {
                updates[key + "/set/" + std::to_string(it->second)] = *node.vertices;
            }
            updates[key + "/node/" + std::to_string(id)] =
                {{"level", node.level}, {"parent", node.parent}, {"set", it->second}};
        }
        updates[key] = {{"minimum_size", minimum_size}, {"next_parent", next_parent}, {"node_count", tree.nodes.size()}};
        checkpoint->put_many(std::move(updates));
        published = tree.nodes.size();
    };
    publish();
    for (std::size_t parent = next_parent; parent < tree.nodes.size(); ++parent) {
        if (checkpoint) {
            checkpoint->check();
        }
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
        next_parent = parent + 1;
        publish();
    }
    // Persist the complete tree before any node search.
    if (checkpoint) {
        checkpoint->flush();
    }
    return tree;
}

} // namespace downsizing
