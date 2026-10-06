#include "graph_downsizing/connectivity.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace downsizing {
namespace {
constexpr auto unreachable = std::numeric_limits<std::size_t>::max();
}

VertexSet ordered_subset(const Graph& graph, VertexSet vertices) {
    std::sort(vertices.begin(), vertices.end());
    if (std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end()) {
        throw std::invalid_argument("A vertex subset must not contain duplicates");
    }
    for (const Vertex vertex : vertices) {
        if (!graph.contains(vertex)) {
            throw std::invalid_argument("A vertex subset contains an unknown vertex");
        }
    }
    return vertices;
}

VertexFlow::VertexFlow(const Graph& graph, VertexSet vertices, EdgeCapacity edge_capacity)
    : vertices_(ordered_subset(graph, std::move(vertices))) {
    if (vertices_.size() > network_.max_size() / 2) {
        throw std::length_error("Flow network has too many vertices");
    }
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        indices_.emplace(vertices_[i], i);
    }
    network_.resize(2 * vertices_.size());
    level_.resize(network_.size());
    next_edge_.resize(network_.size());
    queue_.reserve(network_.size());
    path_.reserve(network_.size());
    const auto capacity = edge_capacity == EdgeCapacity::Unit ? std::size_t{1} : vertices_.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        add_arc(2 * i, 2 * i + 1, 1);
        for (const Vertex neighbor : graph.neighbors(vertices_[i])) {
            const auto found = indices_.find(neighbor);
            if (found != indices_.end() && i < found->second) {
                add_arc(2 * i + 1, 2 * found->second, capacity);
                add_arc(2 * found->second + 1, 2 * i, capacity);
            }
        }
    }
}

void VertexFlow::add_arc(std::size_t from, std::size_t to, std::size_t capacity) {
    const auto reverse = network_[to].size();
    network_[to].push_back({from, network_[from].size(), 0, 0});
    network_[from].push_back({to, reverse, capacity, capacity});
}

bool VertexFlow::build_levels(std::size_t source, std::size_t target) {
    std::fill(level_.begin(), level_.end(), unreachable);
    queue_.clear();
    queue_.push_back(source);
    level_[source] = 0;
    for (std::size_t head = 0; head < queue_.size(); ++head) {
        const auto current = queue_[head];
        for (const auto& edge : network_[current]) {
            if (edge.capacity > 0 && level_[edge.to] == unreachable) {
                level_[edge.to] = level_[current] + 1;
                queue_.push_back(edge.to);
            }
        }
    }
    return level_[target] != unreachable;
}

// Iterative DFS avoids recursion proportional to the length of a path in a large graph.
std::size_t VertexFlow::augment(std::size_t source, std::size_t target, std::size_t limit) {
    path_.clear();
    auto current = source;
    while (true) {
        if (current == target) {
            auto amount = limit;
            for (const auto& [from, index] : path_) {
                amount = std::min(amount, network_[from][index].capacity);
            }
            for (const auto& [from, index] : path_) {
                auto& edge = network_[from][index];
                edge.capacity -= amount;
                network_[edge.to][edge.reverse].capacity += amount;
            }
            return amount;
        }
        auto& index = next_edge_[current];
        while (index < network_[current].size()) {
            const auto& edge = network_[current][index];
            if (edge.capacity > 0 && level_[edge.to] == level_[current] + 1) {
                break;
            }
            ++index;
        }
        if (index < network_[current].size()) {
            path_.emplace_back(current, index);
            current = network_[current][index].to;
        } else {
            if (path_.empty()) {
                return 0;
            }
            current = path_.back().first;
            path_.pop_back();
            ++next_edge_[current];
        }
    }
}

PairFlow VertexFlow::between(Vertex source, Vertex target, std::size_t limit) {
    const auto left = indices_.find(source);
    const auto right = indices_.find(target);
    if (left == indices_.end() || right == indices_.end() || source == target) {
        throw std::invalid_argument("Flow endpoints must be distinct vertices of the subset");
    }
    // n-1 paths suffice for any threshold; a larger limit also obtains a minimum cut.
    limit = std::min(limit, vertices_.size());
    for (auto& edges : network_) {
        for (auto& edge : edges) {
            edge.capacity = edge.initial_capacity;
        }
    }
    const auto start = 2 * left->second + 1;
    const auto finish = 2 * right->second;
    bool adjacent = false;
    for (auto& edge : network_[start]) {
        if (edge.to == finish && edge.initial_capacity > 0) {
            edge.capacity = 1; // The direct edge contributes exactly one path.
            adjacent = true;
            break;
        }
    }
    PairFlow result{0, {}};
    while (result.paths < limit && build_levels(start, finish)) {
        std::fill(next_edge_.begin(), next_edge_.end(), 0);
        while (result.paths < limit) {
            const auto amount = augment(start, finish, limit - result.paths);
            if (amount == 0) {
                break;
            }
            result.paths += amount;
        }
    }
    if (!adjacent && result.paths < limit) {
        // The final unsuccessful BFS marks the source side of a minimum cut.
        // Unit-capacity cuts can cross original-edge arcs as well as vertex arcs.
        // Select the head vertex, or the tail if the head is the terminal target.
        // Every crossing arc is hit without deleting either flow endpoint.
        std::vector<bool> cut(vertices_.size(), false);
        for (std::size_t from = 0; from < network_.size(); ++from) {
            if (level_[from] == unreachable) {
                continue;
            }
            for (const auto& edge : network_[from]) {
                if (edge.initial_capacity > 0 && level_[edge.to] == unreachable) {
                    const auto vertex = edge.to / 2 == right->second ? from / 2 : edge.to / 2;
                    cut[vertex] = true;
                }
            }
        }
        for (std::size_t i = 0; i < vertices_.size(); ++i) {
            if (cut[i]) {
                result.separator.push_back(vertices_[i]);
            }
        }
    }
    return result;
}

bool is_k_connected(const Graph& graph, const VertexSet& vertices, std::size_t k) {
    VertexFlow flow(graph, vertices);
    if (k == 0) {
        return true;
    }
    if (vertices.size() <= k) {
        return false;
    }
    const auto& ordered = flow.vertices();
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        for (std::size_t j = i + 1; j < ordered.size(); ++j) {
            if (flow.between(ordered[i], ordered[j], k).paths < k) {
                return false;
            }
        }
    }
    return true;
}

std::size_t vertex_connectivity(const Graph& graph, const VertexSet& vertices) {
    VertexFlow flow(graph, vertices);
    if (vertices.size() < 2) {
        return 0;
    }
    auto best = vertices.size() - 1;
    const auto& ordered = flow.vertices();
    for (std::size_t i = 0; i < ordered.size() && best > 0; ++i) {
        for (std::size_t j = i + 1; j < ordered.size() && best > 0; ++j) {
            best = std::min(best, flow.between(ordered[i], ordered[j], best).paths);
        }
    }
    return best;
}

} // namespace downsizing
