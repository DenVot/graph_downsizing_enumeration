#pragma once

#include "graph_downsizing/worker_pool.hpp"
#include "graph_downsizing/solver.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace downsizing {

// The pool must outlive the logger. Resource sampling never runs on pool workers.
class RunLogger {
public:
    RunLogger(const std::filesystem::path& file, WorkerPool& pool, std::chrono::seconds interval);
    ~RunLogger();
    RunLogger(const RunLogger&) = delete;
    RunLogger& operator=(const RunLogger&) = delete;

    void search_progress(const SolveProgress& progress);
    void search(const std::string& graph, std::size_t r);
    void info(const std::string& message);
    void error(const std::string& message);
    void context(const std::string& graph, const std::string& phase, std::size_t r = 0);

private:
    [[nodiscard]] std::string progress_fields() const;
    void write(const char* level, const std::string& message);
    void sample();
    void monitor(std::chrono::seconds interval);

    WorkerPool& pool_;
    std::filesystem::path directory_;
    std::ofstream output_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::string context_ = "phase=startup";
    SolveProgress progress_;
    bool searching_ = false;
    std::thread monitor_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previous_time_;
    double previous_cpu_ = -1;
};

} // namespace downsizing
