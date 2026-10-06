#pragma once

#include "graph_downsizing/graph.hpp"

#include <map>
#include <vector>

namespace downsizing {

using VertexSet = std::vector<Vertex>;

// Sort and reject duplicate or unknown IDs, without constructing a flow network.
[[nodiscard]] VertexSet ordered_subset(const Graph& graph, VertexSet vertices);

struct PairFlow {
    std::size_t paths;
    // Populated only for nonadjacent endpoints when paths < the requested limit.
    VertexSet separator;
};

// Dinic on an induced subgraph. The topology is reused; residuals reset for each pair.
class VertexFlow {
public:
    enum class EdgeCapacity { VertexBound, Unit };

    VertexFlow(const Graph& graph, VertexSet vertices,
               EdgeCapacity edge_capacity = EdgeCapacity::VertexBound);

    [[nodiscard]] PairFlow between(Vertex source, Vertex target, std::size_t limit);
    [[nodiscard]] const VertexSet& vertices() const noexcept { return vertices_; }

private:
    struct Edge {
        std::size_t to;
        std::size_t reverse;
        std::size_t capacity;
        std::size_t initial_capacity;
    };

    void add_arc(std::size_t from, std::size_t to, std::size_t capacity);
    bool build_levels(std::size_t source, std::size_t target);
    std::size_t augment(std::size_t source, std::size_t target, std::size_t limit);

    VertexSet vertices_;
    std::map<Vertex, std::size_t> indices_;
    std::vector<std::vector<Edge>> network_;
    std::vector<std::size_t> level_;
    std::vector<std::size_t> next_edge_;
    std::vector<std::size_t> queue_;
    std::vector<std::pair<std::size_t, std::size_t>> path_;
};

[[nodiscard]] bool is_k_connected(const Graph& graph, const VertexSet& vertices, std::size_t k);
[[nodiscard]] std::size_t vertex_connectivity(const Graph& graph, const VertexSet& vertices);

} // namespace downsizing
