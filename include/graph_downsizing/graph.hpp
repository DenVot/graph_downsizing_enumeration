#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>

namespace downsizing {

using Vertex = std::int64_t;
using VertexId = Vertex;

// Simple connected graph; adjacency keys and neighbors are original vertex IDs.
class Graph {
public:
    [[nodiscard]] static Graph from_edges(std::vector<std::pair<VertexId, VertexId>> edges);

    [[nodiscard]] std::size_t vertex_count() const noexcept { return adjacency_.size(); }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edge_count_; }
    [[nodiscard]] bool contains(Vertex vertex) const { return adjacency_.contains(vertex); }
    [[nodiscard]] const std::vector<Vertex>& neighbors(Vertex vertex) const { return adjacency_.at(vertex); }
    [[nodiscard]] const std::map<Vertex, std::vector<Vertex>>& adjacency() const noexcept { return adjacency_; }

private:
    Graph() = default;
    std::map<Vertex, std::vector<Vertex>> adjacency_;
    std::size_t edge_count_ = 0;
};

[[nodiscard]] Graph read_graph(const std::filesystem::path& path);

} // namespace downsizing
