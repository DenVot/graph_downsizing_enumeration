#include "graph_downsizing/parallel_search.hpp"
#include "graph_downsizing/checkpoint.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>
#include <stdexcept>

namespace downsizing {

std::optional<VertexSet> p_downsize(const Graph& graph, const VertexSet& vertices,
                                   std::size_t r, std::size_t k, const std::atomic<bool>* stopped,
                                   BigInt* checked, NodeProgress* progress) {
    if (checked) {
        *checked = 0;
    }
    const auto ordered = ordered_subset(graph, vertices);
    if (r == 0 || r > ordered.size()) {
        throw std::invalid_argument("PDownsize requires 1 <= r <= node size");
    }
    if (k >= r) {
        return std::nullopt;
    }
    std::vector<std::size_t> indices(r);
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    if (progress) {
        progress->checkpoint.check();
        const auto saved = progress->checkpoint.get(progress->key);
        if (!saved.is_null()) {
            const auto status = saved.at("status").get<std::string>();
            if (status == "FINISHED_NOT_FOUND") {
                return std::nullopt;
            }
            if (status == "FINISHED_FOUND") {
                const auto vertices = saved.at("vertices").get<VertexSet>();
                if (vertices.size() != r || ordered_subset(graph, vertices) != vertices ||
                    !std::includes(ordered.begin(), ordered.end(), vertices.begin(), vertices.end())) {
                    throw std::runtime_error("Invalid saved node witness");
                }
                return vertices;
            }
            if (status == "IN_PROGRESS") {
                indices = saved.at("next").get<std::vector<std::size_t>>();
                if (indices.size() != r || !std::is_sorted(indices.begin(), indices.end()) ||
                    std::adjacent_find(indices.begin(), indices.end()) != indices.end() ||
                    indices.back() >= ordered.size()) {
                    throw std::runtime_error("Invalid checkpoint combination cursor");
                }
            } else if (status != "NOT_PROCESSED") {
                throw std::runtime_error("Unknown checkpoint node status");
            }
        }
    }
    const auto publish_cursor = [&] {
        if (progress) {
            progress->checkpoint.put(progress->key, {{"status", "IN_PROGRESS"}, {"next", indices}});
        }
    };
    publish_cursor();
    VertexSet subset(r);
    for (;;) {
        if (progress) {
            progress->checkpoint.check();
        }
        for (std::size_t i = 0; i < r; ++i) {
            subset[i] = ordered[indices[i]];
        }
        if (stopped && stopped->load()) {
            return std::nullopt;
        }
        VertexFlow flow(graph, subset);
        bool found = true;
        for (std::size_t i = 0; i < r && found; ++i) {
            for (std::size_t j = i + 1; j < r; ++j) {
                if (progress) {
                    progress->checkpoint.check();
                }
                if (stopped && stopped->load()) {
                    return std::nullopt;
                }
                if (flow.between(subset[i], subset[j], k).paths < k) {
                    found = false;
                    break;
                }
            }
        }
        if (checked) {
            ++*checked;
        }
        if (found) {
            if (progress) {
                progress->checkpoint.put(progress->key,
                                         {{"status", "FINISHED_FOUND"}, {"vertices", subset}}, true);
            }
            return subset;
        }
        if (!next_combination(indices, ordered.size())) {
            break;
        }
        publish_cursor();
    }
    if (progress) {
        progress->checkpoint.put(progress->key, {{"status", "FINISHED_NOT_FOUND"}});
    }
    return std::nullopt;
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
    for (const auto& node : nodes) {
        if (!node || ordered_subset(graph, *node) != *node) {
            throw std::invalid_argument("Parallel nodes must contain sorted unique vertex IDs");
        }
    }
    // Check saved successes before submitting any work. Ordinals are stable within
    // the filtered level, since the tree and r are immutable across resume.
    for (std::size_t i = 0; checkpoint && i < nodes.size(); ++i) {
        checkpoint->check();
        const auto key = prefix + "/" + std::to_string(i);
        const auto state = checkpoint->get(key);
        if (state.is_null()) {
            checkpoint->put(key, {{"status", "NOT_PROCESSED"}});
        } else if (state.at("status") == "FINISHED_FOUND") {
            NodeProgress progress{*checkpoint, key};
            return p_downsize(graph, *nodes[i], r, k, nullptr, nullptr, &progress);
        }
    }
    std::mutex mutex;
    std::atomic<bool> stopped{false};
    std::optional<VertexSet> result;
    try {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (checkpoint) {
                checkpoint->check();
            }
            const auto key = prefix + "/" + std::to_string(i);
            if (checkpoint && checkpoint->get(key).at("status") == "FINISHED_NOT_FOUND") {
                continue;
            }
            if (stopped.load()) {
                break;
            }
            if (node->size() < r) {
                continue;
            }
            // One queued task owns the entire sequential enumeration of one node.
            pool.submit([&, node, key] {
                if (stopped.load()) {
                    return;
                }
                BigInt checked = 0;
                try {
                    if (stats) {
                        std::lock_guard lock(mutex);
                        ++stats->nodes_started;
                    }
                    std::optional<NodeProgress> progress;
                    if (checkpoint) {
                        progress.emplace(NodeProgress{*checkpoint, key});
                    }
                    auto witness = p_downsize(graph, *node, r, k, &stopped, &checked,
                                             progress ? &*progress : nullptr);
                    std::lock_guard lock(mutex);
                    if (stats) {
                        stats->subsets_checked += checked;
                    }
                    if (witness && !stopped.load()) {
                        result = std::move(witness);
                        stopped.store(true);
                    }
                } catch (...) {
                    stopped.store(true);
                    throw;
                }
            });
        }
        // References captured by tasks stay alive until BOTH queued and running tasks finish.
        pool.wait();
        if (checkpoint) {
            checkpoint->check();
        }
    } catch (...) {
        const auto error = std::current_exception();
        stopped.store(true);
        // submit itself can fail after earlier tasks were accepted. Drain those tasks too.
        try {
            pool.wait();
        } catch (...) {
        }
        std::rethrow_exception(error);
    }
    return result;
}

} // namespace downsizing
