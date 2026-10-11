#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace downsizing {

struct Interrupted : std::runtime_error {
    Interrupted() : std::runtime_error("Calculation interrupted; checkpoint saved") {}
};

void install_stop_handlers();

// One tree.json and progress.json per graph. Published state is held in memory;
// flush replaces changed files atomically, always saving the tree before progress.
class Checkpoint {
public:
    Checkpoint(const std::filesystem::path& directory, bool resume, std::chrono::seconds interval,
               const std::vector<nlohmann::json>& metadata);
    ~Checkpoint();
    Checkpoint(const Checkpoint&) = delete;
    Checkpoint& operator=(const Checkpoint&) = delete;

    [[nodiscard]] nlohmann::json get(const std::string& key) const;
    void put(const std::string& key, nlohmann::json value, bool immediate = false);
    void put_many(std::map<std::string, nlohmann::json> values);
    void flush();
    void check() const;
    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    struct GraphState {
        nlohmann::json tree;
        nlohmann::json progress;
        bool tree_dirty = false;
        bool progress_dirty = false;
    };

    struct Location {
        std::string graph;
        bool tree;
        nlohmann::json::json_pointer pointer;
    };

    [[nodiscard]] Location locate(const std::string& key) const;
    void update(const std::string& key, nlohmann::json value);
    void flush_locked();
    void background(std::chrono::seconds interval);

    std::filesystem::path path_;
    int lock_fd_ = -1;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, GraphState> graphs_;
    std::exception_ptr error_;
    bool closing_ = false;
    std::thread saver_;
};

} // namespace downsizing
