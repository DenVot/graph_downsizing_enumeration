#include "graph_downsizing/solver.hpp"
#include "graph_downsizing/parallel_search.hpp"

#include <algorithm>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>

namespace downsizing {
namespace {

VertexSet connected_subset(const Graph& graph, std::size_t r) {
    const auto root = graph.adjacency().begin()->first;
    VertexSet vertices{root};
    std::set<Vertex> seen{root};
    std::queue<Vertex> pending;
    pending.push(root);
    while (vertices.size() < r && !pending.empty()) {
        const auto vertex = pending.front();
        pending.pop();
        for (const auto neighbor : graph.neighbors(vertex)) {
            if (seen.insert(neighbor).second) {
                vertices.push_back(neighbor);
                pending.push(neighbor);
                if (vertices.size() == r) break;
            }
        }
    }
    if (vertices.size() != r) throw std::logic_error("Connected graph cannot supply r vertices");
    std::sort(vertices.begin(), vertices.end());
    return vertices;
}

} // namespace

std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                   std::size_t r, std::size_t k) {
    const auto ordered = ordered_subset(graph, vertices);
    if (r == 0 || r > ordered.size()) throw std::invalid_argument("PDownsize requires 1 <= r <= node size");
    if (k >= r) return std::nullopt;
    std::vector<std::size_t> indices(r);
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    VertexSet subset(r);
    do {
        for (std::size_t i = 0; i < r; ++i) subset[i] = ordered[indices[i]];
        // Each pair is checked in the induced subset, stopping on the first failed flow.
        if (is_k_connected(graph, subset, k)) return subset;
    } while (next_combination(indices, ordered.size()));
    return std::nullopt;
}

ExactSolver::ExactSolver(const Graph& graph, std::size_t minimum_size)
    : graph_(graph), minimum_size_(minimum_size), tree_(build_tree(graph, minimum_size)) {}

ExactSolver::ExactSolver(const Graph& graph, std::size_t minimum_size, WorkerPool& pool, std::size_t chunk_size)
    : ExactSolver(graph, minimum_size) {
    if (chunk_size == 0) throw std::invalid_argument("Chunk size must be positive");
    pool_ = &pool;
    chunk_size_ = chunk_size;
}

const Solution& ExactSolver::solve(std::size_t r) {
    if (r < minimum_size_ || r > graph_.vertex_count()) {
        throw std::invalid_argument("Requested r is outside the solver's tree size range");
    }
    if (const auto found = solutions_.find(r); found != solutions_.end()) return found->second;
    if (r > 1) {
        const auto maximum_k = std::min(r - 1, tree_.levels.size() - 1);
        for (std::size_t k = maximum_k; k >= 2; --k) {
            if (pool_) {
                std::vector<std::shared_ptr<const VertexSet>> nodes;
                for (const auto id : tree_.levels[k]) {
                    if (tree_.nodes[id].vertices->size() >= r) nodes.push_back(tree_.nodes[id].vertices);
                }
                if (auto witness = parallel_p_downsize(graph_, nodes, r, k, *pool_, chunk_size_)) {
                    return solutions_.emplace(r, Solution{k, std::move(*witness)}).first->second;
                }
                continue;
            }
            for (const auto id : tree_.levels[k]) {
                const auto& vertices = *tree_.nodes[id].vertices;
                if (vertices.size() < r) continue;
                if (auto witness = p_downsize(graph_, vertices, r, k)) {
                    return solutions_.emplace(r, Solution{k, std::move(*witness)}).first->second;
                }
            }
        }
    }
    // A BFS prefix is connected. Having exhausted k >= 2, its connectivity is optimal.
    return solutions_.emplace(r, Solution{r == 1 ? 0U : 1U, connected_subset(graph_, r)}).first->second;
}

} // namespace downsizing
