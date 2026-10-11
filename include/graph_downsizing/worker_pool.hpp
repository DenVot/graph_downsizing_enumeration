#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace downsizing {
// One coordinator submits independent tasks and waits at level boundaries.
// The task queue has no size limit.
class WorkerPool {
public:
    struct Stats {
        std::size_t active;
        std::size_t queued;
        std::size_t finished;
    };

    [[nodiscard]] Stats stats();
    explicit WorkerPool(std::size_t threads);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    void submit(std::function<void()> job);
    // Waits for queued and running tasks, then rethrows the first task error and clears it.
    void wait();
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
private:
    void worker();
    void stop();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable finished_;
    bool stopping_ = false;
    std::size_t active_ = 0;
    std::size_t finished_tasks_ = 0;
    std::deque<std::function<void()>> jobs_;
    std::exception_ptr error_;
    std::vector<std::thread> workers_;
};
} // namespace downsizing
