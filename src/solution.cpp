#include "graph_downsizing/solution.hpp"
#include "graph_downsizing/sizes.hpp"

#include <algorithm>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace downsizing {
namespace {

void check_solution(const Graph& graph, std::size_t r, const Solution& solution) {
    if (r == 0 || r > graph.vertex_count() || solution.vertices.size() != r) {
        throw std::invalid_argument("Solution witness must contain exactly r vertices");
    }
    if (solution.connectivity >= r) {
        throw std::invalid_argument("Solution connectivity must be in [0, r-1]");
    }
    auto vertices = solution.vertices;
    std::sort(vertices.begin(), vertices.end());
    if (std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end() ||
        std::any_of(vertices.begin(), vertices.end(), [&](Vertex vertex) { return !graph.contains(vertex); })) {
        throw std::invalid_argument("Solution witness contains invalid or duplicate vertices");
    }
}

} // namespace

std::size_t write_solution_blocks(std::ostream& output, const Graph& graph, const std::vector<std::size_t>& sizes,
                             const SolutionsBySize& solutions) {
    for (const auto& [r, solution] : solutions) {
        check_solution(graph, r, solution);
    }
    std::size_t blocks = 0;
    for (const auto r : sizes) {
        const auto found = solutions.find(r);
        if (found == solutions.end()) {
            break;
        }
        const auto& solution = found->second;
        const std::set<Vertex> vertices(solution.vertices.begin(), solution.vertices.end());
        std::size_t edges = 0;
        for (const auto u : vertices) {
            for (const auto v : graph.neighbors(u)) {
                if (u < v && vertices.contains(v)) {
                    ++edges;
                }
            }
        }
        // A separate header stream keeps caller locale/base flags out of the file format.
        std::ostringstream header;
        header.imbue(std::locale::classic());
        header << r << ' '
               << solution.connectivity << ' ' << edges << '\n';
        output << header.str();
        for (const auto u : vertices) {
            for (const auto v : graph.neighbors(u)) {
                if (u < v && vertices.contains(v)) {
                    output << std::to_string(u) << ' ' << std::to_string(v) << '\n';
                }
            }
        }
        if (!output) {
            throw std::runtime_error("Failed to write solution blocks");
        }
        ++blocks;
    }
    return blocks;
}

void save_solution_file(const std::filesystem::path& path, const Graph& graph, const std::vector<std::size_t>& sizes,
                        const SolutionsBySize& solutions) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    if (std::filesystem::exists(temporary)) {
        throw std::runtime_error("Temporary output already exists: " + temporary.string());
    }
    try {
        std::ofstream output(temporary);
        if (!output) throw std::runtime_error("Cannot open temporary solution file: " + temporary.string());
        static_cast<void>(write_solution_blocks(output, graph, sizes, solutions));
        output.close();
        if (!output) throw std::runtime_error("Failed to save solution file: " + temporary.string());
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace downsizing
