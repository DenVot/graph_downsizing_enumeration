#include "graph_downsizing/alpha.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace downsizing {

AlphaSchedule::AlphaSchedule(double step, std::size_t vertex_count)
    : step_(step), vertex_count_(vertex_count) {
    if (vertex_count == 0) {
        throw std::invalid_argument("An alpha schedule requires a nonempty graph");
    }
    if (!std::isfinite(step) || step <= 0 || step > 1) {
        throw std::invalid_argument("alpha_step must be a number in (0, 1]");
    }
    const double count = std::ceil(1.0 / step);
    if (!std::isfinite(count) || count >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("alpha_step is too small: task count exceeds the supported range");
    }
    task_count_ = static_cast<std::size_t>(count);
}

std::optional<AlphaTask> AlphaSchedule::next() {
    if (index_ == task_count_) {
        return std::nullopt;
    }
    ++index_;
    const double alpha = index_ == task_count_ ? 1.0 : std::min(1.0, static_cast<double>(index_) * step_);
    const double size = std::nextafter(alpha * static_cast<double>(vertex_count_),
                                       std::numeric_limits<double>::infinity());
    const auto r = size >= static_cast<double>(vertex_count_) ? vertex_count_
        : std::max(std::size_t{1}, static_cast<std::size_t>(std::floor(size)));
    return AlphaTask{alpha, r};
}

} // namespace downsizing
