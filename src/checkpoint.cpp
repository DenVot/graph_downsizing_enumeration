#include "graph_downsizing/checkpoint.hpp"

#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

namespace downsizing {
namespace {

std::atomic<int> signal_number{0};
static_assert(std::atomic<int>::is_always_lock_free);

extern "C" void stop_handler(int number) {
    signal_number.store(number);
}

void sync_file(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("Cannot open checkpoint for fsync: " + path.string());
    }

    const int code = ::fsync(fd);
    ::close(fd);
    if (code != 0) {
        throw std::runtime_error("Cannot fsync checkpoint: " + path.string());
    }
}

void write_json(const std::filesystem::path& path, const nlohmann::json& document) {
    auto temporary = path;
    temporary += ".tmp";
    // Remove a private leftover from an interrupted write; never follow its symlink.
    std::filesystem::remove(temporary);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << document.dump(2) << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot write checkpoint: " + temporary.string());
    }

    sync_file(temporary);
    std::filesystem::rename(temporary, path);
    sync_file(path.parent_path());
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open checkpoint: " + path.string());
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(input);
    } catch (...) {
        throw std::runtime_error("Invalid checkpoint JSON: " + path.string());
    }

    if (!document.is_object() || document.value("version", 0) != 5) {
        throw std::runtime_error("Unsupported checkpoint format version: " + path.string());
    }

    return document;
}

} // namespace

void RangeCursor::advance() {
    std::lock_guard lock(mutex_);
    ++next_;
}

BigInt RangeCursor::snapshot() const {
    std::lock_guard lock(mutex_);
    return next_;
}

void install_stop_handlers() {
    signal_number.store(0);
    struct sigaction action{};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        throw std::runtime_error("Cannot install stop handlers");
    }
}

Checkpoint::Checkpoint(const std::filesystem::path& directory, bool resume, std::chrono::seconds interval,
                       const std::vector<nlohmann::json>& metadata)
    : path_(std::filesystem::weakly_canonical(std::filesystem::absolute(directory))) {
    if (interval.count() <= 0 || metadata.empty()) {
        throw std::invalid_argument("Checkpoint requires graphs and a positive interval");
    }
    if (resume && !std::filesystem::is_directory(path_)) {
        throw std::runtime_error("Checkpoint directory does not exist");
    }

    std::filesystem::create_directories(path_);
    lock_fd_ = ::open((path_ / ".checkpoint.lock").c_str(), O_RDWR | O_CREAT, 0600);
    if (lock_fd_ < 0) {
        throw std::runtime_error("Cannot open checkpoint lock");
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
        throw std::runtime_error("Checkpoint is already in use by another process");
    }

    try {
        bool any_tree = false;
        for (const auto& item : metadata) {
            const auto graph_id = std::filesystem::path(item.at("input").get<std::string>()).stem().string();
            if (graph_id.empty() || graph_id == "." || graph_id == ".." ||
                std::filesystem::path(graph_id).filename().string() != graph_id ||
                graph_id.find('\0') != std::string::npos || graphs_.contains(graph_id)) {
                throw std::invalid_argument("Checkpoint requires unique input filename stems as graph IDs");
            }
            const auto folder = path_ / graph_id;
            if (std::filesystem::is_symlink(folder)) {
                throw std::runtime_error("Checkpoint graph directories must not be symlinks");
            }

            const auto tree_file = folder / "tree.json";
            const auto progress_file = folder / "progress.json";
            GraphState state;
            if (std::filesystem::exists(tree_file)) {
                if (!resume) {
                    throw std::runtime_error("Checkpoint already exists; use resume or another working_directory");
                }
                state.tree = read_json(tree_file);
                if (state.tree.at("metadata") != item || state.tree.at("graph_id") != graph_id) {
                    throw std::runtime_error("Checkpoint parameters or input graph do not match configuration");
                }
                any_tree = true;
            } else {
                state.tree = {
                    {"version", 5},
                    {"graph_id", graph_id},
                    {"metadata", item},
                    {"state", nullptr},
                    {"nodes", nlohmann::json::object()},
                    {"sets", nlohmann::json::object()}
                };
                state.tree_dirty = true;
            }

            if (std::filesystem::exists(progress_file)) {
                if (!resume) {
                    throw std::runtime_error("Checkpoint already exists; use resume or another working_directory");
                }
                state.progress = read_json(progress_file);
                if (state.tree_dirty) {
                    throw std::runtime_error("Checkpoint progress is missing its tree");
                }
                if (state.progress.at("metadata") != item || state.progress.at("graph_id") != graph_id) {
                    throw std::runtime_error("Checkpoint progress parameters do not match its tree");
                }
            } else {
                state.progress = {
                    {"version", 5},
                    {"graph_id", graph_id},
                    {"metadata", item},
                    {"nodes", nlohmann::json::object()}
                };
                state.progress_dirty = true;
            }
            graphs_.emplace(graph_id, std::move(state));
        }

        if (resume && !any_tree) {
            throw std::runtime_error("Checkpoint contains no tree.json files");
        }
        flush_locked();
        saver_ = std::thread([this, interval] { background(interval); });
    } catch (...) {
        ::close(lock_fd_);
        lock_fd_ = -1;
        throw;
    }
}

Checkpoint::~Checkpoint() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    wake_.notify_one();
    if (saver_.joinable()) {
        saver_.join();
    }
    if (lock_fd_ >= 0) {
        ::close(lock_fd_);
    }
}

Checkpoint::Location Checkpoint::locate(const std::string& key) const {
    const auto slash = key.find('/');
    const auto end = key.find('/', slash + 1);
    const auto kind = key.substr(0, slash);
    if (slash == std::string::npos || (kind != "tree" && kind != "search")) {
        throw std::invalid_argument("Invalid checkpoint key");
    }

    const auto graph = key.substr(slash + 1, end == std::string::npos ? end : end - slash - 1);
    if (!graphs_.contains(graph)) {
        throw std::invalid_argument("Invalid checkpoint graph ID");
    }

    std::string pointer;
    if (kind == "tree") {
        if (end == std::string::npos) {
            pointer = "/state";
        } else if (key.substr(end).starts_with("/node/")) {
            pointer = "/nodes/" + key.substr(end + 6);
        } else if (key.substr(end).starts_with("/set/")) {
            pointer = "/sets/" + key.substr(end + 5);
        } else {
            throw std::invalid_argument("Invalid checkpoint tree key");
        }
    } else {
        if (end == std::string::npos) {
            throw std::invalid_argument("Invalid checkpoint search key");
        }
        pointer = "/nodes/";
        for (const char c : key.substr(end + 1)) {
            pointer += c == '/' ? "~1" : std::string(1, c);
        }
    }
    return {graph, kind == "tree", nlohmann::json::json_pointer(pointer)};
}

nlohmann::json Checkpoint::get(const std::string& key) const {
    std::lock_guard lock(mutex_);
    if (error_) {
        std::rethrow_exception(error_);
    }
    const auto location = locate(key);
    const auto& state = graphs_.at(location.graph);
    const auto& document = location.tree ? state.tree : state.progress;
    return document.contains(location.pointer) ? document.at(location.pointer) : nlohmann::json{};
}

void Checkpoint::check() const {
    // The hot path touches atomics only. Publish the exception before setting failed_.
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex_);
        std::rethrow_exception(error_);
    }
    if (signal_number.load(std::memory_order_relaxed) != 0) {
        throw Interrupted();
    }
}

void Checkpoint::update(const std::string& key, nlohmann::json value) {
    cursors_.erase(key);
    const auto location = locate(key);
    auto& state = graphs_.at(location.graph);
    auto& document = location.tree ? state.tree : state.progress;
    if (document.contains(location.pointer) && document.at(location.pointer) == value) {
        return;
    }
    document[location.pointer] = std::move(value);
    (location.tree ? state.tree_dirty : state.progress_dirty) = true;
}

void Checkpoint::put(const std::string& key, nlohmann::json value, bool immediate, RangeCursors cursors) {
    std::lock_guard lock(mutex_);
    if (error_) {
        std::rethrow_exception(error_);
    }
    try {
        update(key, std::move(value));
        if (!cursors.empty()) {
            cursors_.emplace(key, std::move(cursors));
        }
        if (immediate) {
            flush_locked();
        }
    } catch (...) {
        error_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
        throw;
    }
}

void Checkpoint::put_many(std::map<std::string, nlohmann::json> values) {
    std::lock_guard lock(mutex_);
    if (error_) {
        std::rethrow_exception(error_);
    }
    try {
        for (auto& [key, value] : values) {
            update(key, std::move(value));
        }
    } catch (...) {
        error_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
        throw;
    }
}

void Checkpoint::flush() {
    std::lock_guard lock(mutex_);
    if (error_) {
        std::rethrow_exception(error_);
    }
    try {
        flush_locked();
    } catch (...) {
        error_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
        throw;
    }
}

void Checkpoint::snapshot_cursors_locked() {
    for (const auto& [key, cursors] : cursors_) {
        const auto location = locate(key);
        auto& state = graphs_.at(location.graph);
        auto& node = state.progress.at(location.pointer);
        for (auto& range : node.at("unfinished_ranges")) {
            const auto& cursor = cursors.at(range.at("begin").get<std::string>());
            const auto next = cursor->snapshot().str();
            if (range.at("next") != next) {
                range["next"] = next;
                state.progress_dirty = true;
            }
        }
    }
}

void Checkpoint::flush_locked() {
    snapshot_cursors_locked();
    for (auto& [graph_id, state] : graphs_) {
        const auto folder = path_ / graph_id;
        if (state.tree_dirty) {
            // Tree changes occur only before search, when progress is still empty.
            if (!state.progress.at("nodes").empty()) {
                throw std::logic_error("Cannot change a tree after search has started");
            }
            std::filesystem::create_directories(folder);
            sync_file(path_);
            write_json(folder / "tree.json", state.tree);
            state.tree_dirty = false;
        }
        if (state.progress_dirty) {
            write_json(folder / "progress.json", state.progress);
            state.progress_dirty = false;
        }
    }
}

void Checkpoint::background(std::chrono::seconds interval) {
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, interval, [&] { return closing_; })) {
        try {
            flush_locked();
        } catch (...) {
            error_ = std::current_exception();
            failed_.store(true, std::memory_order_release);
            return;
        }
    }
}

} // namespace downsizing
