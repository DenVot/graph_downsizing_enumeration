#pragma once

#include "graph_downsizing/graph.hpp"

#include <filesystem>
#include <map>
#include <ostream>
#include <vector>

namespace downsizing {

struct Solution {
    std::size_t connectivity;
    std::vector<Vertex> vertices;
};

using SolutionsBySize = std::map<std::size_t, Solution>;

// The solver supplies proven results; this writer checks witness shape, not connectivity.
// Emits one block per r in the completed prefix of the sorted unique size list.
[[nodiscard]] std::size_t write_solution_blocks(std::ostream& output, const Graph& graph,
                                         const std::vector<std::size_t>& sizes,
                                         const SolutionsBySize& solutions);

// Replace the output only after the complete prefix has been written successfully.
void save_solution_file(const std::filesystem::path& path, const Graph& graph, const std::vector<std::size_t>& sizes,
                        const SolutionsBySize& solutions);

} // namespace downsizing
