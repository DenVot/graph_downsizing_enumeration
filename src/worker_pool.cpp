#include "graph_downsizing/worker_pool.hpp"

#include <stdexcept>

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
    std::size_t generation = 0;
    std::unique_lock lock(mutex_);
    for (;;) {
        ready_.wait(lock, [&] { return stopping_ || generation != generation_; });
        if (stopping_) return;
        generation = generation_;
        lock.unlock();
        std::exception_ptr error;
        try { job_(); } catch (...) { error = std::current_exception(); }
        lock.lock();
        if (error && !error_) error_ = error;
        if (--remaining_ == 0) finished_.notify_one();
    }
}

void WorkerPool::run(std::function<void()> job) {
    std::unique_lock lock(mutex_);
    if (!job || remaining_ != 0) throw std::invalid_argument("Pool requires one nonempty coordinator job at a time");
    job_ = std::move(job);
    error_ = nullptr;
    remaining_ = workers_.size();
    ++generation_;
    ready_.notify_all();
    finished_.wait(lock, [&] { return remaining_ == 0; });
    job_ = {};
    if (error_) std::rethrow_exception(error_);
}
} // namespace downsizing
