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
