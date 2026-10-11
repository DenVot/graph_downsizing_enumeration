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

BigInt combination_rank(const std::vector<std::size_t>& indices, std::size_t n) {
    BigInt rank = 0;
    std::size_t start = 0;
    const auto r = indices.size();
    if (r > n) {
        throw std::invalid_argument("Invalid combination cursor");
    }
    for (std::size_t i = 0; i < r; ++i) {
        const auto value = indices[i];
        if (value < start || value > n - (r - i)) {
            throw std::invalid_argument("Invalid combination cursor");
        }
        rank += combination_count(n - start, r - i) - combination_count(n - value, r - i);
        start = value + 1;
    }
    return rank;
}

std::vector<std::size_t> combination_at(std::size_t n, std::size_t r, BigInt rank) {
    if (r > n || rank < 0 || rank >= combination_count(n, r)) {
        throw std::invalid_argument("Combination rank is outside enumeration");
    }
    std::vector<std::size_t> indices;
    indices.reserve(r);
    std::size_t start = 0;
    for (std::size_t i = 0; i < r; ++i) {
        const auto remaining = r - i;
        const BigInt total = combination_count(n - start, remaining);
        auto low = start;
        auto high = n - remaining;
        // Find the last candidate with no more than rank preceding combinations.
        while (low < high) {
            const auto middle = low + (high - low) / 2 + 1;
            const BigInt skipped = total - combination_count(n - middle, remaining);
            if (skipped <= rank) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        rank -= total - combination_count(n - low, remaining);
        indices.push_back(low);
        start = low + 1;
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

} // namespace downsizing
