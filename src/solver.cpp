#include "graph_downsizing/solver.hpp"
#include "graph_downsizing/checkpoint.hpp"
#include "graph_downsizing/parallel_search.hpp"

#include <algorithm>
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

ExactSolver::ExactSolver(const Graph& graph, std::size_t minimum_size, WorkerPool& pool,
                         Checkpoint* checkpoint, const std::string& graph_id)
    : graph_(graph), minimum_size_(minimum_size), tree_(build_tree(graph, minimum_size, checkpoint, graph_id)),
      checkpoint_(checkpoint), graph_id_(graph_id), pool_(pool) {
}

const Solution& ExactSolver::remember(std::size_t r, Solution solution) {
    return solutions_.emplace(r, std::move(solution)).first->second;
}

const Solution& ExactSolver::solve(std::size_t r) {
    if (r < minimum_size_ || r > graph_.vertex_count()) {
        throw std::invalid_argument("Requested r is outside the solver's tree size range");
    }
    if (checkpoint_) checkpoint_->check();
    if (const auto found = solutions_.find(r); found != solutions_.end()) return found->second;
    if (r > 1) {
        const auto maximum_k = std::min(r - 1, tree_.levels.size() - 1);
        for (std::size_t k = maximum_k; k >= 2; --k) {
            std::vector<std::shared_ptr<const VertexSet>> nodes;
            for (const auto id : tree_.levels[k]) {
                if (tree_.nodes[id].vertices->size() >= r) nodes.push_back(tree_.nodes[id].vertices);
            }
            if (auto witness = parallel_p_downsize(graph_, nodes, r, k, pool_, nullptr, checkpoint_,
                    "search/" + graph_id_ + "/" + std::to_string(r) + "/" + std::to_string(k))) {
                return remember(r, Solution{k, std::move(*witness)});
            }
        }
    }
    // A BFS prefix is connected. Having exhausted k >= 2, its connectivity is optimal.
    return remember(r, Solution{r == 1 ? 0U : 1U, connected_subset(graph_, r)});
}

} // namespace downsizing
