#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace downsizing {
// A single coordinator invokes run(). Each worker executes the supplied job once per run.
// run() waits for every worker, including on exception; the pool is reusable afterward.
class WorkerPool {
public:
    explicit WorkerPool(std::size_t threads);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    void run(std::function<void()> job);
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
private:
    void worker();
    void stop();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable finished_;
    bool stopping_ = false;
    std::size_t generation_ = 0;
    std::size_t remaining_ = 0;
    std::function<void()> job_;
    std::exception_ptr error_;
    std::vector<std::thread> workers_;
};
} // namespace downsizing
