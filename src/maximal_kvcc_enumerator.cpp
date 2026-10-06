#include "graph_downsizing/maximal_kvcc_enumerator.hpp"
#include "graph_downsizing/global_cut.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>

namespace downsizing {

VertexSet MaximalKVCCEnumerator::k_core(const VertexSet& vertices, std::size_t k) const {
    std::set<Vertex> active(vertices.begin(), vertices.end());
    std::map<Vertex, std::size_t> degree;
    VertexSet queue;
    for (const Vertex vertex : vertices) {
        auto& count = degree[vertex];
        for (const Vertex neighbor : graph_.neighbors(vertex)) {
            count += active.contains(neighbor) ? 1U : 0U;
        }
        if (count < k) {
            queue.push_back(vertex);
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const Vertex vertex = queue[head];
        active.erase(vertex);
        for (const Vertex neighbor : graph_.neighbors(vertex)) {
            if (active.contains(neighbor) && --degree[neighbor] == k - 1) {
                queue.push_back(neighbor);
            }
        }
    }
    return {active.begin(), active.end()};
}

std::vector<VertexSet> MaximalKVCCEnumerator::connected_components(const VertexSet& vertices) const {
    std::set<Vertex> remaining(vertices.begin(), vertices.end());
    std::vector<VertexSet> parts;
    while (!remaining.empty()) {
        VertexSet queue{*remaining.begin()};
        remaining.erase(queue.front());
        for (std::size_t head = 0; head < queue.size(); ++head) {
            for (const Vertex neighbor : graph_.neighbors(queue[head])) {
                if (remaining.erase(neighbor) > 0) {
                    queue.push_back(neighbor);
                }
            }
        }
        std::sort(queue.begin(), queue.end());
        parts.push_back(std::move(queue));
    }
    return parts;
}

std::vector<VertexSet> MaximalKVCCEnumerator::overlap_partition(
    const VertexSet& vertices, const VertexSet& separator) const {
    VertexSet remaining;
    std::set_difference(vertices.begin(), vertices.end(), separator.begin(), separator.end(),
                        std::back_inserter(remaining));
    auto parts = connected_components(remaining);
    if (parts.size() < 2) {
        throw std::logic_error("GLOBAL_CUT returned a set that does not disconnect the component");
    }
    for (auto& part : parts) {
        part.insert(part.end(), separator.begin(), separator.end());
        std::sort(part.begin(), part.end());
    }
    return parts;
}

VertexSet MaximalKVCCEnumerator::global_cut(const VertexSet& vertices, std::size_t k,
                                           StrongSideCache& cache) const {
    GlobalCutStats stats;
    auto separator = ordered_subset(graph_, global_cut_ ? global_cut_(graph_, vertices, k)
                                                       : find_global_cut_reusing(graph_, vertices, k, cache, stats));
    if (separator.size() >= k ||
        !std::includes(vertices.begin(), vertices.end(), separator.begin(), separator.end())) {
        throw std::logic_error("GLOBAL_CUT must return a subset of the component with size < k");
    }
    return separator;
}

std::vector<VertexSet> MaximalKVCCEnumerator::enumerate(const VertexSet& vertices, std::size_t k) const {
    if (k == 0) {
        throw std::invalid_argument("The maximal-subgraph enumerator requires k >= 1");
    }
    // Algorithm 1 (KVCC-ENUM), using a work stack instead of recursive calls.
    struct Task {
        VertexSet vertices;
        StrongSideCache parent;
    };
    std::vector<Task> pending{{ordered_subset(graph_, vertices), {}}};
    std::vector<VertexSet> found;
    while (!pending.empty()) {
        auto task = std::move(pending.back());
        pending.pop_back();
        auto core = k_core(task.vertices, k);
        // Lines 3-4: split connected components before invoking GLOBAL_CUT.
        for (auto& component : connected_components(core)) {
            auto cache = task.parent;
            const auto separator = global_cut(component, k, cache);
            if (separator.empty()) {
                found.push_back(std::move(component));
            } else {
                // Lines 9-11: every component receives the separator and is processed again.
                auto parts = overlap_partition(component, separator);
                // LIFO: finish smaller siblings first to release inherited parent information earlier.
                std::stable_sort(parts.begin(), parts.end(), [](const auto& left, const auto& right) {
                    return left.size() > right.size();
                });
                for (auto& part : parts) {
                    pending.push_back({std::move(part), cache});
                }
            }
        }
    }
    // Valid cuts of size < k make the leaves maximal and duplicate-free (Lemma 3).
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace downsizing
