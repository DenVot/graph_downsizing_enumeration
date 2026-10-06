#pragma once

#include <cstddef>
#include <optional>

namespace downsizing {

struct AlphaTask {
    double alpha;
    std::size_t r;
};

// Lazy grid. The final alpha is always 1, even when step does not divide 1.
class AlphaSchedule {
public:
    AlphaSchedule(double step, std::size_t vertex_count);

    [[nodiscard]] std::optional<AlphaTask> next();
    [[nodiscard]] std::size_t task_count() const noexcept { return task_count_; }

private:
    double step_;
    std::size_t index_ = 0;
    std::size_t task_count_;
    std::size_t vertex_count_;
};

} // namespace downsizing
