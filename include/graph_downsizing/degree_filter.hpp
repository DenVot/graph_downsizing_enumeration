#pragma once

#include "graph_downsizing/graph.hpp"

#include <span>

namespace downsizing {

// Degree bound on induced candidates, independent of a flow network.
class DegreeFilter {
public:
    DegreeFilter(const Graph& graph, std::span<const Vertex> vertices);

    // Sorted, distinct positions in the constructor's vertex list.
    [[nodiscard]] bool has_minimum_degree(std::span<const std::size_t> indices, std::size_t k);

private:
    std::size_t size_;
    std::vector<std::uint64_t> neighbor_masks_;
    std::vector<std::vector<std::size_t>> neighbors_;
    std::vector<unsigned char> selected_;
};

} // namespace downsizing
