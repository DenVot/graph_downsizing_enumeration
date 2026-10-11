#include "graph_downsizing/worker_pool.hpp"

#include <stdexcept>
#include <utility>

namespace downsizing {
WorkerPool::WorkerPool(std::size_t threads) {
    if (threads == 0) throw std::invalid_argument("Worker count must be positive");
    try {
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker(); });
    } catch (...) {
        stop();
        throw;
    }
}

void WorkerPool::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
}
WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::worker() {
    std::unique_lock lock(mutex_);
    for (;;) {
        ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        auto job = std::move(jobs_.front());
        jobs_.pop_front();
        ++active_;
        lock.unlock();
        std::exception_ptr error;
        try { job(); } catch (...) { error = std::current_exception(); }
        job = {};
        lock.lock();
        if (error && !error_) error_ = error;
        --active_;
        if (jobs_.empty() && active_ == 0) finished_.notify_one();
    }
}

void WorkerPool::submit(std::function<void()> job) {
    if (!job) throw std::invalid_argument("Cannot submit an empty task");
    std::unique_lock lock(mutex_);
    if (stopping_) throw std::logic_error("Cannot submit to a stopped pool");
    jobs_.push_back(std::move(job));
    ready_.notify_one();
}

void WorkerPool::wait() {
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [&] { return jobs_.empty() && active_ == 0; });
    const auto error = std::exchange(error_, nullptr);
    if (error) std::rethrow_exception(error);
}
} // namespace downsizing
