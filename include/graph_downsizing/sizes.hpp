#pragma once

#include <cstddef>
#include <vector>

namespace downsizing {

struct RequestedSizes {
    std::vector<std::size_t> sizes;
    std::vector<double> alphas;
};

// Normalize explicit input or the default alpha grid into unique target sizes.
[[nodiscard]] std::vector<std::size_t> make_sizes(double step, std::size_t vertex_count,
                                                const RequestedSizes& requested = {});

} // namespace downsizing
