#include "graph_downsizing/sizes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace downsizing {

std::vector<std::size_t> make_sizes(double step, std::size_t vertex_count, const RequestedSizes& requested) {
    if (vertex_count == 0) {
        throw std::invalid_argument("A size schedule requires a nonempty graph");
    }
    const auto to_size = [vertex_count](double alpha) {
        if (!std::isfinite(alpha) || alpha <= 0 || alpha > 1) {
            throw std::invalid_argument("Requested alpha must be in (0, 1]");
        }
        const double size = std::nextafter(alpha * static_cast<double>(vertex_count),
                                          std::numeric_limits<double>::infinity());
        return size >= static_cast<double>(vertex_count) ? vertex_count
            : std::max(std::size_t{1}, static_cast<std::size_t>(std::floor(size)));
    };
    if (!requested.sizes.empty() && !requested.alphas.empty()) {
        throw std::invalid_argument("Specify either r or alpha, not both");
    }
    auto sizes = requested.sizes;
    for (const auto r : sizes) {
        if (r == 0 || r > vertex_count) {
            throw std::invalid_argument("Requested r must be in [1, vertex count]");
        }
    }
    for (const auto alpha : requested.alphas) {
        sizes.push_back(to_size(alpha));
    }
    if (sizes.empty()) {
        if (!std::isfinite(step) || step <= 0 || step > 1) {
            throw std::invalid_argument("alpha_step must be a number in (0, 1]");
        }
        const double count = std::ceil(1.0 / step);
        if (!std::isfinite(count) || count >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
            throw std::invalid_argument("alpha_step is too small: task count exceeds the supported range");
        }
        const auto grid_count = static_cast<std::size_t>(count);
        const auto at = [&](std::size_t index) {
            return to_size(index == grid_count - 1 ? 1.0
                : std::min(1.0, static_cast<double>(index + 1) * step));
        };
        for (std::size_t index = 0; index < grid_count;) {
            const auto r = at(index);
            sizes.push_back(r);
            // Skip all grid points yielding this r, even for a very small step.
            auto low = index + 1;
            auto high = grid_count;
            while (low < high) {
                const auto middle = low + (high - low) / 2;
                if (at(middle) <= r) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }
            index = low;
        }
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

} // namespace downsizing
