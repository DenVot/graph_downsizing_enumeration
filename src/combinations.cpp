#include "graph_downsizing/combinations.hpp"

#include <algorithm>
#include <stdexcept>

namespace downsizing {
BigInt combination_count(std::size_t n, std::size_t r) {
    if (r > n) return 0;
    r = std::min(r, n - r);
    BigInt count = 1;
    for (std::size_t i = 1; i <= r; ++i) {
        count *= n - r + i;
        count /= i;
    }
    return count;
}

std::vector<std::size_t> unrank_combination(std::size_t n, std::size_t r, BigInt rank) {
    if (rank < 0 || rank >= combination_count(n, r)) throw std::invalid_argument("Combination rank is out of range");
    std::vector<std::size_t> indices;
    indices.reserve(r);
    if (r == 0) return indices;
    auto block = combination_count(n - 1, r - 1);
    std::size_t candidate = 0;
    for (std::size_t position = 0; position < r; ++position) {
        while (rank >= block) {
            rank -= block;
            // C(n-c-2, remaining-1) from C(n-c-1, remaining-1).
            block *= n - candidate - (r - position);
            block /= n - candidate - 1;
            ++candidate;
        }
        indices.push_back(candidate);
        if (position + 1 < r) {
            block *= r - position - 1;
            block /= n - candidate - 1;
        }
        ++candidate;
    }
    return indices;
}

bool next_combination(std::vector<std::size_t>& indices, std::size_t n) {
    for (std::size_t i = indices.size(); i > 0; --i) {
        const auto position = i - 1;
        if (indices[position] < n - indices.size() + position) {
            ++indices[position];
            for (std::size_t j = i; j < indices.size(); ++j) indices[j] = indices[j - 1] + 1;
            return true;
        }
    }
    return false;
}

CombinationRanges::CombinationRanges(const std::vector<std::size_t>& node_sizes,
                                     std::size_t r, std::size_t chunk_size) : chunk_size_(chunk_size) {
    if (r == 0 || chunk_size == 0) throw std::invalid_argument("Range size and r must be positive");
    for (const auto n : node_sizes) {
        cursors_.push_back({0, combination_count(n, r)});
        if (cursors_.back().end > 0) active_.push_back(cursors_.size() - 1);
    }
}

std::optional<CombinationRange> CombinationRanges::next() {
    if (active_.empty()) return std::nullopt;
    const auto node = active_.front();
    active_.pop_front();
    auto& cursor = cursors_[node];
    BigInt end = cursor.next + chunk_size_;
    if (end > cursor.end) end = cursor.end;
    CombinationRange result{node, cursor.next, end};
    cursor.next = end;
    if (end < cursor.end) active_.push_back(node);
    return result;
}
} // namespace downsizing
