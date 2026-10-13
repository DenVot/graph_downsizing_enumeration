#include "graph_downsizing/degree_filter.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace downsizing {

DegreeFilter::DegreeFilter(const Graph& graph, std::span<const Vertex> vertices)
    : size_(vertices.size()) {
    if (!std::is_sorted(vertices.begin(), vertices.end())
        || std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end()) {
        throw std::invalid_argument("Degree filter vertices must be sorted and distinct");
    }
    if (size_ <= 64) {
        neighbor_masks_.resize(size_);
    } else {
        neighbors_.resize(size_);
        selected_.resize(size_);
    }
    for (std::size_t i = 0; i < size_; ++i) {
        for (const auto neighbor : graph.neighbors(vertices[i])) {
            const auto found = std::lower_bound(vertices.begin(), vertices.end(), neighbor);
            if (found == vertices.end() || *found != neighbor) {
                continue;
            }
            const auto index = static_cast<std::size_t>(found - vertices.begin());
            if (size_ <= 64) {
                neighbor_masks_[i] |= std::uint64_t{1} << index;
            } else {
                neighbors_[i].push_back(index);
            }
        }
    }
}

bool DegreeFilter::has_minimum_degree(std::span<const std::size_t> indices, std::size_t k) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= size_ || (i > 0 && indices[i - 1] >= indices[i])) {
            throw std::invalid_argument("Candidate positions must be sorted, distinct and inside the node");
        }
    }
    if (k == 0) {
        return true;
    }
    if (indices.size() <= k) {
        return false;
    }
    if (size_ <= 64) {
        std::uint64_t mask = 0;
        for (const auto vertex : indices) {
            mask |= std::uint64_t{1} << vertex;
        }
        for (const auto vertex : indices) {
            if (static_cast<std::size_t>(std::popcount(neighbor_masks_[vertex] & mask)) < k) {
                return false;
            }
        }
        return true;
    }
    for (const auto vertex : indices) {
        selected_[vertex] = 1;
    }
    bool passed = true;
    for (const auto vertex : indices) {
        std::size_t degree = 0;
        for (const auto neighbor : neighbors_[vertex]) {
            if (selected_[neighbor] && ++degree >= k) {
                break;
            }
        }
        if (degree < k) {
            passed = false;
            break;
        }
    }
    for (const auto vertex : indices) {
        selected_[vertex] = 0;
    }
    return passed;
}

} // namespace downsizing
