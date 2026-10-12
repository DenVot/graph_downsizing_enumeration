#include "graph_downsizing/connectivity.hpp"

#include <algorithm>
#include <bit>
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
    selected_.reserve(vertices_.size());
    active_.assign(vertices_.size(), 1);
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        selected_.push_back(i);
    }
    network_.resize(2 * vertices_.size());
    level_.resize(network_.size());
    next_edge_.resize(network_.size());
    queue_.reserve(network_.size());
    path_.reserve(network_.size());
    if (vertices_.size() <= 64) {
        neighbor_masks_.resize(vertices_.size());
    }
    const auto capacity = edge_capacity == EdgeCapacity::Unit ? std::size_t{1} : vertices_.size();
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        add_arc(2 * i, 2 * i + 1, 1);
        for (const Vertex neighbor : graph.neighbors(vertices_[i])) {
            const auto found = indices_.find(neighbor);
            if (found != indices_.end() && i < found->second) {
                if (!neighbor_masks_.empty()) {
                    neighbor_masks_[i] |= std::uint64_t{1} << found->second;
                    neighbor_masks_[found->second] |= std::uint64_t{1} << i;
                }
                add_arc(2 * i + 1, 2 * found->second, capacity);
                add_arc(2 * found->second + 1, 2 * i, capacity);
            }
        }
    }
}

void VertexFlow::select_indices(std::span<const std::size_t> indices) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertices_.size() || (i > 0 && indices[i - 1] >= indices[i])) {
            throw std::invalid_argument("Selected positions must be sorted, distinct and inside the flow network");
        }
    }
    reset_capacities();
    for (const auto vertex : selected_) {
        active_[vertex] = 0;
    }
    selected_.assign(indices.begin(), indices.end());
    for (const auto vertex : selected_) {
        active_[vertex] = 1;
    }
}

bool VertexFlow::has_minimum_degree(std::size_t k) const {
    if (k == 0) {
        return true;
    }
    if (selected_.size() <= k) {
        return false;
    }
    if (!neighbor_masks_.empty()) {
        std::uint64_t mask = 0;
        for (const auto vertex : selected_) {
            mask |= std::uint64_t{1} << vertex;
        }
        for (const auto vertex : selected_) {
            if (static_cast<std::size_t>(std::popcount(neighbor_masks_[vertex] & mask)) < k) {
                return false;
            }
        }
    } else {
        for (const auto vertex : selected_) {
            std::size_t degree = 0;
            for (const auto& edge : network_[2 * vertex + 1]) {
                // Only original outgoing arcs, not residual reverse arcs.
                if (edge.initial_capacity > 0 && active_[edge.to / 2]) {
                    if (++degree >= k) {
                        break;
                    }
                }
            }
            if (degree < k) {
                return false;
            }
        }
    }
    return true;
}

void VertexFlow::reset_capacities() {
    for (const auto& [from, index] : changed_) {
        auto& edge = network_[from][index];
        auto& reverse = network_[edge.to][edge.reverse];
        edge.capacity = edge.initial_capacity;
        reverse.capacity = reverse.initial_capacity;
    }
    changed_.clear();
}

void VertexFlow::add_arc(std::size_t from, std::size_t to, std::size_t capacity) {
    const auto reverse = network_[to].size();
    network_[to].push_back({from, network_[from].size(), 0, 0});
    network_[from].push_back({to, reverse, capacity, capacity});
}

bool VertexFlow::build_levels(std::size_t source, std::size_t target) {
    for (const auto vertex : selected_) {
        level_[2 * vertex] = unreachable;
        level_[2 * vertex + 1] = unreachable;
    }
    queue_.clear();
    queue_.push_back(source);
    level_[source] = 0;
    for (std::size_t head = 0; head < queue_.size(); ++head) {
        const auto current = queue_[head];
        // Finish the preceding level so all shortest paths remain available.
        // If target is unreachable, exhaust BFS for minimum-cut extraction.
        if (level_[target] != unreachable && level_[current] >= level_[target]) {
            break;
        }
        for (const auto& edge : network_[current]) {
            if (active_[edge.to / 2] && edge.capacity > 0 && level_[edge.to] == unreachable) {
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
            // Record before mutation; repeated entries are harmless during reset.
            changed_.insert(changed_.end(), path_.begin(), path_.end());
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
            if (active_[edge.to / 2] && edge.capacity > 0 && level_[edge.to] == level_[current] + 1) {
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
    if (left == indices_.end() || right == indices_.end() || source == target
        || !active_[left->second] || !active_[right->second]) {
        throw std::invalid_argument("Flow endpoints must be distinct vertices of the subset");
    }
    // n-1 paths suffice for any threshold; a larger limit also obtains a minimum cut.
    limit = std::min(limit, selected_.size());
    reset_capacities();
    const auto start = 2 * left->second + 1;
    const auto finish = 2 * right->second;
    bool adjacent = false;
    for (std::size_t index = 0; index < network_[start].size(); ++index) {
        auto& edge = network_[start][index];
        if (edge.to == finish && edge.initial_capacity > 0) {
            if (edge.capacity != 1) {
                changed_.emplace_back(start, index);
            }
            edge.capacity = 1; // The direct edge contributes exactly one path.
            adjacent = true;
            break;
        }
    }
    PairFlow result{0, {}};
    while (result.paths < limit && build_levels(start, finish)) {
        for (const auto vertex : selected_) {
            next_edge_[2 * vertex] = 0;
            next_edge_[2 * vertex + 1] = 0;
        }
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
        for (const auto selected : selected_) {
            for (const auto from : {2 * selected, 2 * selected + 1}) {
                if (level_[from] == unreachable) {
                    continue;
                }
                for (const auto& edge : network_[from]) {
                    if (active_[edge.to / 2] && edge.initial_capacity > 0 && level_[edge.to] == unreachable) {
                        const auto vertex = edge.to / 2 == right->second ? from / 2 : edge.to / 2;
                        result.separator.push_back(vertices_[vertex]);
                    }
                }
            }
        }
        std::sort(result.separator.begin(), result.separator.end());
        result.separator.erase(std::unique(result.separator.begin(), result.separator.end()), result.separator.end());
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
