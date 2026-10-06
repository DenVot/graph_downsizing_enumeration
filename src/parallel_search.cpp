#include "graph_downsizing/parallel_search.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>

namespace downsizing {
std::optional<VertexSet> parallel_p_downsize(
    const Graph& graph, const std::vector<std::shared_ptr<const VertexSet>>& nodes,
    std::size_t r, std::size_t k, WorkerPool& pool, std::size_t chunk_size, SearchStats* stats) {
    if (r == 0 || chunk_size == 0) throw std::invalid_argument("Search requires positive r and chunk size");
    if (stats) *stats = {};
    if (k >= r) return std::nullopt;
    std::vector<std::size_t> sizes;
    for (const auto& node : nodes) {
        if (!node || ordered_subset(graph, *node) != *node) {
            throw std::invalid_argument("Parallel nodes must contain sorted unique vertex IDs");
        }
        sizes.push_back(node->size());
    }
    CombinationRanges ranges(sizes, r, chunk_size);
    std::mutex mutex;
    std::atomic<bool> stopped{false};
    std::optional<VertexSet> result;
    pool.run([&] {
        try {
            for (;;) {
                std::optional<CombinationRange> range;
                {
                    std::lock_guard lock(mutex);
                    if (stopped.load()) return;
                    range = ranges.next();
                    if (!range) return;
                    if (stats) ++stats->ranges_issued;
                }
                const auto& vertices = *nodes[range->node];
                auto indices = unrank_combination(vertices.size(), r, range->begin);
                VertexSet subset(r);
                BigInt checked = 0;
                for (BigInt rank = range->begin; rank < range->end && !stopped.load(); ++rank) {
                    for (std::size_t i = 0; i < r; ++i) subset[i] = vertices[indices[i]];
                    VertexFlow flow(graph, subset);
                    bool found = true;
                    for (std::size_t i = 0; i < r && found; ++i) {
                        for (std::size_t j = i + 1; j < r; ++j) {
                            // A running flow finishes; no further pair is started after cancellation is observed.
                            if (stopped.load()) { found = false; break; }
                            if (flow.between(subset[i], subset[j], k).paths < k) { found = false; break; }
                        }
                    }
                    if (stopped.load()) break;
                    ++checked;
                    if (found) {
                        std::lock_guard lock(mutex);
                        if (!stopped.load()) {
                            result = std::move(subset);
                            stopped.store(true);
                        }
                        break;
                    }
                    if (rank + 1 < range->end) next_combination(indices, vertices.size());
                }
                if (stats) {
                    std::lock_guard lock(mutex);
                    stats->subsets_checked += checked;
                }
            }
        } catch (...) {
            stopped.store(true);
            throw;
        }
    });
    return result;
}
} // namespace downsizing
