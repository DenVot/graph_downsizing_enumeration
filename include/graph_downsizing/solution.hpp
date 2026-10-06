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
// Emits the completed prefix of the alpha grid, reusing results for repeated r values.
[[nodiscard]] std::size_t write_solution_blocks(std::ostream& output, const Graph& graph,
                                         double alpha_step,
                                         const SolutionsBySize& solutions);

// Replace the output only after the complete prefix has been written successfully.
void save_solution_file(const std::filesystem::path& path, const Graph& graph, double alpha_step,
                        const SolutionsBySize& solutions);

} // namespace downsizing
