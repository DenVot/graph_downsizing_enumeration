#include "graph_downsizing/parallel_search.hpp"
#include "graph_downsizing/checkpoint.hpp"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>

namespace downsizing {
namespace {

struct Range {
    Range(BigInt next, BigInt limit, bool accepted = false)
        : cursor(std::make_shared<RangeCursor>(std::move(next))), end(std::move(limit)), submitted(accepted) {}

    std::shared_ptr<RangeCursor> cursor;
    BigInt end;
    bool submitted = false;
};

struct Node {
    std::shared_ptr<const VertexSet> vertices;
    std::string key;
    BigInt total;
    BigInt next_unassigned = 0;
    std::map<BigInt, Range> ranges;
    std::string status = "NOT_PROCESSED";
    VertexSet witness;
    bool started = false;
};

BigInt read_rank(const nlohmann::json& value) {
    const auto text = value.get<std::string>();
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("Invalid checkpoint range rank");
    }
    BigInt result = 0;
    for (const auto digit : text) {
        result = result * 10 + (digit - '0');
    }
    return result;
}

void restore_node(Node& node, const nlohmann::json& saved, std::size_t r) {
    if (saved.is_null()) {
        return;
    }
    node.status = saved.at("status").get<std::string>();
    if (node.status == "FINISHED_FOUND") {
        node.witness = saved.at("vertices").get<VertexSet>();
        if (node.witness.size() != r || !std::is_sorted(node.witness.begin(), node.witness.end()) ||
            std::adjacent_find(node.witness.begin(), node.witness.end()) != node.witness.end() ||
            !std::includes(node.vertices->begin(), node.vertices->end(), node.witness.begin(), node.witness.end())) {
            throw std::runtime_error("Invalid saved node witness");
        }
    } else if (node.status == "IN_PROGRESS") {
        node.next_unassigned = read_rank(saved.at("next_unassigned"));
        if (node.next_unassigned > node.total || !saved.at("unfinished_ranges").is_array()) {
            throw std::runtime_error("Invalid checkpoint range frontier");
        }
        BigInt previous_end = 0;
        for (const auto& item : saved.at("unfinished_ranges")) {
            const auto begin = read_rank(item.at("begin"));
            Range range{read_rank(item.at("next")), read_rank(item.at("end"))};
            const auto next = range.cursor->snapshot();
            if (begin < previous_end || begin >= range.end || next < begin ||
                next > range.end || range.end > node.next_unassigned) {
                throw std::runtime_error("Invalid or overlapping checkpoint ranges");
            }
            previous_end = range.end;
            if (next < range.end) {
                node.ranges.emplace(begin, std::move(range));
            }
        }
    } else if (node.status != "NOT_PROCESSED" && node.status != "FINISHED_NOT_FOUND") {
        throw std::runtime_error("Unknown checkpoint node status");
    }
}

// Range lifecycle changes share the coordinator mutex. Each worker advances only its
// private cursor; checkpoint reads these cursors without calling back into the search.
class RangeSearch {
public:
    RangeSearch(const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
                std::size_t r, std::size_t k, WorkerPool* pool, SearchStats* stats,
                Checkpoint* checkpoint, const std::string& prefix,
                const std::atomic<bool>* external_stop = nullptr, bool exact_key = false)
        : graph_(graph), r_(r), k_(k), pool_(pool), stats_(stats), checkpoint_(checkpoint),
          external_stop_(external_stop), window_(pool ? 2 * pool->size() : 1) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i] || ordered_subset(graph, *nodes[i]) != *nodes[i]) {
                throw std::invalid_argument("Parallel nodes must contain sorted unique vertex IDs");
            }
            if (nodes[i]->size() < r) {
                continue;
            }
            Node node;
            node.vertices = nodes[i];
            node.key = exact_key ? prefix : prefix + "/" + std::to_string(i);
            node.total = combination_count(nodes[i]->size(), r);
            if (checkpoint_) {
                checkpoint_->check();
                restore_node(node, checkpoint_->get(node.key), r);
            }
            if (node.status == "FINISHED_FOUND") {
                result_ = node.witness;
            }
            nodes_.push_back(std::move(node));
        }
    }

    std::optional<VertexSet> run() {
        if (result_) {
            return result_;
        }
        try {
            std::unique_lock lock(mutex_);
            while (!cancelled()) {
                if (checkpoint_) {
                    checkpoint_->check();
                }
                while (outstanding_ < window_ && !cancelled()) {
                    const auto job = reserve();
                    if (!job) {
                        break;
                    }
                    const auto [node, begin] = *job;
                    ++outstanding_;
                    if (pool_) {
                        pool_->submit([this, node, begin] { work(node, begin); });
                    } else {
                        // The standalone sequential API uses the caller, without creating a pool.
                        lock.unlock();
                        work(node, begin);
                        lock.lock();
                    }
                    if (stats_) {
                        stats_->max_outstanding_ranges = std::max(stats_->max_outstanding_ranges, outstanding_);
                    }
                }
                if (outstanding_ == 0) {
                    break;
                }
                const auto completed = completed_;
                completed_cv_.wait(lock, [&] { return completed_ != completed || stopped_.load(); });
            }
            lock.unlock();
            // Keep all captures and synchronization objects alive until accepted tasks have exited.
            if (pool_) {
                pool_->wait();
            }
            release_cursors();
            if (error_) {
                std::rethrow_exception(error_);
            }
            if (checkpoint_) {
                checkpoint_->check();
            }
        } catch (...) {
            const auto error = std::current_exception();
            stopped_.store(true);
            try {
                if (pool_) {
                    pool_->wait();
                }
            } catch (...) {
            }
            try {
                release_cursors();
            } catch (...) {
                // Preserve the original error if checkpoint publication has failed.
            }
            std::rethrow_exception(error);
        }
        return result_;
    }

private:
    bool cancelled() const {
        return stopped_.load() || (external_stop_ && external_stop_->load());
    }

    void publish(const Node& node, bool immediate = false, bool live = true) {
        if (!checkpoint_) {
            return;
        }
        nlohmann::json state{{"status", node.status}};
        RangeCursors cursors;
        if (node.status == "FINISHED_FOUND") {
            state["vertices"] = node.witness;
        } else if (node.status == "IN_PROGRESS") {
            state["next_unassigned"] = node.next_unassigned.str();
            state["unfinished_ranges"] = nlohmann::json::array();
            for (const auto& [begin, range] : node.ranges) {
                state["unfinished_ranges"].push_back({
                    {"begin", begin.str()}, {"next", range.cursor->snapshot().str()}, {"end", range.end.str()}
                });
                if (live) {
                    cursors.emplace(begin.str(), range.cursor);
                }
            }
        }
        checkpoint_->put(node.key, std::move(state), immediate, std::move(cursors));
    }

    void release_cursors() {
        // Called only after all accepted workers have exited. No cursor can advance again.
        if (checkpoint_) {
            std::lock_guard lock(mutex_);
            for (const auto& node : nodes_) {
                if (!node.ranges.empty() && node.status == "IN_PROGRESS") {
                    publish(node, false, false);
                }
            }
        }
    }

    std::optional<std::pair<std::size_t, BigInt>> reserve() {
        for (std::size_t attempt = 0; attempt < nodes_.size(); ++attempt) {
            const auto id = next_node_;
            next_node_ = (next_node_ + 1) % nodes_.size();
            auto& node = nodes_[id];
            if (node.status == "FINISHED_NOT_FOUND") {
                continue;
            }
            for (auto& [begin, range] : node.ranges) {
                if (!range.submitted) {
                    range.submitted = true;
                    return std::pair{id, begin};
                }
            }
            if (node.next_unassigned < node.total) {
                const BigInt begin = node.next_unassigned;
                // Small nodes also split across workers; large nodes use bounded 256-subset jobs.
                const BigInt balanced = (node.total + window_ - 1) / window_;
                const BigInt length = std::min(BigInt(256), balanced);
                const BigInt end = std::min(node.total, BigInt(begin + length));
                node.ranges.emplace(begin, Range{begin, end, true});
                node.next_unassigned = end;
                node.status = "IN_PROGRESS";
                publish(node);
                return std::pair{id, begin};
            }
            if (node.ranges.empty() && node.status != "FINISHED_NOT_FOUND") {
                node.status = "FINISHED_NOT_FOUND";
                publish(node);
            }
        }
        return std::nullopt;
    }

    void enumerate(std::size_t id, const BigInt& begin, BigInt& checked) {
        BigInt next;
        BigInt end;
        std::shared_ptr<RangeCursor> cursor;
        {
            std::lock_guard lock(mutex_);
            if (cancelled()) {
                return;
            }
            auto& node = nodes_[id];
            const auto& range = node.ranges.at(begin);
            cursor = range.cursor;
            next = cursor->snapshot();
            end = range.end;
            if (stats_) {
                ++stats_->ranges_started;
                if (!node.started) {
                    ++stats_->nodes_started;
                }
            }
            node.started = true;
        }
        const auto& vertices = *nodes_[id].vertices;
        auto indices = combination_at(vertices.size(), r_, next);
        VertexSet subset(r_);
        while (next < end && !cancelled()) {
            if (checkpoint_) {
                checkpoint_->check();
            }
            for (std::size_t i = 0; i < r_; ++i) {
                subset[i] = vertices[indices[i]];
            }
            VertexFlow flow(graph_, subset);
            bool found = true;
            for (std::size_t i = 0; i < r_ && found; ++i) {
                for (std::size_t j = i + 1; j < r_; ++j) {
                    if (checkpoint_) {
                        checkpoint_->check();
                    }
                    if (cancelled()) {
                        return;
                    }
                    if (flow.between(subset[i], subset[j], k_).paths < k_) {
                        found = false;
                        break;
                    }
                }
            }
            ++checked;
            if (found) {
                std::lock_guard lock(mutex_);
                if (!cancelled()) {
                    auto& node = nodes_[id];
                    node.status = "FINISHED_FOUND";
                    node.witness = subset;
                    publish(node, true);
                    result_ = std::move(subset);
                    stopped_.store(true);
                }
                return;
            }
            ++next;
            cursor->advance();
            if (next < end) {
                next_combination(indices, vertices.size());
            }
        }
        if (next == end) {
            std::lock_guard lock(mutex_);
            if (!cancelled()) {
                auto& node = nodes_[id];
                node.ranges.erase(begin);
                if (node.next_unassigned == node.total && node.ranges.empty()) {
                    node.status = "FINISHED_NOT_FOUND";
                }
                publish(node);
            }
        }
    }

    void work(std::size_t node, const BigInt& begin) {
        BigInt checked = 0;
        try {
            enumerate(node, begin, checked);
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!error_) {
                error_ = std::current_exception();
            }
            stopped_.store(true);
        }
        {
            std::lock_guard lock(mutex_);
            if (stats_) {
                stats_->subsets_checked += checked;
            }
            --outstanding_;
            ++completed_;
        }
        completed_cv_.notify_one();
    }

    const Graph& graph_;
    std::size_t r_;
    std::size_t k_;
    WorkerPool* pool_;
    SearchStats* stats_;
    Checkpoint* checkpoint_;
    const std::atomic<bool>* external_stop_;
    std::size_t window_;
    std::vector<Node> nodes_;
    std::size_t next_node_ = 0;
    std::size_t outstanding_ = 0;
    std::size_t completed_ = 0;
    std::mutex mutex_;
    std::condition_variable completed_cv_;
    std::atomic<bool> stopped_{false};
    std::exception_ptr error_;
    std::optional<VertexSet> result_;
};

} // namespace

std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                   std::size_t r, std::size_t k, const std::atomic<bool>* stopped,
                                   BigInt* checked, NodeProgress* progress) {
    if (checked) {
        *checked = 0;
    }
    if (r == 0 || r > vertices.size()) {
        throw std::invalid_argument("PDownsize requires 1 <= r <= node size");
    }
    if (k >= r) {
        return std::nullopt;
    }
    SearchStats stats;
    RangeSearch search(graph, {std::make_shared<const VertexSet>(ordered_subset(graph, vertices))},
                       r, k, nullptr, &stats, progress ? &progress->checkpoint : nullptr,
                       progress ? progress->key : "", stopped, true);
    auto witness = search.run();
    if (checked) {
        *checked = stats.subsets_checked;
    }
    return witness;
}

std::optional<VertexSet> parallel_p_downsize(
    const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
    std::size_t r, std::size_t k, WorkerPool& pool, SearchStats* stats,
    Checkpoint* checkpoint, const std::string& prefix) {
    if (r == 0) {
        throw std::invalid_argument("Search requires positive r");
    }
    if (stats) {
        *stats = {};
    }
    if (k >= r) {
        return std::nullopt;
    }
    return RangeSearch(graph, nodes, r, k, &pool, stats, checkpoint, prefix).run();
}

} // namespace downsizing
