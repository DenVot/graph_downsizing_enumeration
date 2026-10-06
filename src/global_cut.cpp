#include "graph_downsizing/global_cut.hpp"

#include <algorithm>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace downsizing {

namespace {
struct Certificate {
    Graph graph;
    std::vector<VertexSet> groups;
};

Certificate build_certificate(const Graph& graph, const VertexSet& vertices, std::size_t k) {
    const auto ordered = ordered_subset(graph, vertices);
    if (k == 0 || ordered.size() < 2) {
        throw std::invalid_argument("Sparse certificate requires k >= 1 and at least two vertices");
    }
    const std::unordered_set<Vertex> selected(ordered.begin(), ordered.end());
    std::map<Vertex, std::set<Vertex>> remaining;
    for (const auto vertex : ordered) {
        auto& neighbors = remaining[vertex];
        for (const auto neighbor : graph.neighbors(vertex)) {
            if (selected.contains(neighbor)) {
                neighbors.insert(neighbor);
            }
        }
    }
    std::vector<VertexSet> groups;
    for (const auto vertex : ordered) groups.push_back({vertex});
    std::vector<std::pair<Vertex, Vertex>> certificate_edges;
    for (std::size_t round = 0; round < k; ++round) {
        std::vector<VertexSet> round_groups;
        std::set<Vertex> marked;
        std::vector<std::pair<Vertex, Vertex>> forest;
        // BFS is scan-first: mark ALL unseen neighbors when scanning a vertex.
        // Removing earlier forests can leave multiple residual components.
        for (const auto root : ordered) {
            if (!marked.insert(root).second) {
                continue;
            }
            round_groups.push_back({});
            std::queue<Vertex> queue;
            queue.push(root);
            while (!queue.empty()) {
                const auto vertex = queue.front();
                queue.pop();
                round_groups.back().push_back(vertex);
                for (const auto neighbor : remaining.at(vertex)) {
                    if (marked.insert(neighbor).second) {
                        forest.emplace_back(vertex, neighbor);
                        queue.push(neighbor);
                    }
                }
            }
        }
        if (round + 1 == k) groups = std::move(round_groups);
        if (forest.empty()) {
            break;
        }
        for (const auto& [u, v] : forest) {
            remaining.at(u).erase(v);
            remaining.at(v).erase(u);
        }
        certificate_edges.insert(certificate_edges.end(), forest.begin(), forest.end());
    }
    // The first forest spans a connected input. Graph validates connectivity of the union.
    auto certificate = Graph::from_edges(std::move(certificate_edges));
    if (certificate.vertex_count() != ordered.size()) {
        throw std::invalid_argument("Sparse certificate requires a connected induced subset");
    }
    return {std::move(certificate), std::move(groups)};
}

// LOC-CUT: reuse the topology; between() resets residuals and stops at k paths.
VertexSet local_cut(const Graph& certificate, VertexFlow& flow, Vertex u, Vertex v,
                    std::size_t k, GlobalCutStats& stats) {
    const auto& neighbors = certificate.neighbors(u);
    if (u == v || std::binary_search(neighbors.begin(), neighbors.end(), v)) return {};
    ++stats.flow_calls;
    return flow.between(u, v, k).separator;
}

Vertex minimum_degree_vertex(const Graph& graph, const VertexSet& ordered) {
    auto source = ordered.front();
    auto minimum_degree = ordered.size();
    for (const auto vertex : ordered) {
        std::size_t degree = 0;
        for (const auto neighbor : graph.neighbors(vertex)) {
            degree += std::binary_search(ordered.begin(), ordered.end(), neighbor) ? 1U : 0U;
        }
        if (degree < minimum_degree) {
            minimum_degree = degree;
            source = vertex;
        }
    }
    return source;
}

std::set<Vertex> strong_side_vertices(const Graph& certificate, const VertexSet& ordered,
                                           std::size_t k, const StrongSideCache& parent,
                                           GlobalCutStats& stats) {
    std::set<Vertex> strong;
    // Theorem 8: each nonadjacent pair of neighbors has >= k common neighbors.
    for (const auto vertex : ordered) {
        if (parent && parent->k == k &&
            std::binary_search(parent->vertices.begin(), parent->vertices.end(), vertex) &&
            !parent->recheck.contains(vertex)) {
            ++stats.strong_reused;
            if (parent->strong.contains(vertex)) strong.insert(vertex);
            continue;
        }
        ++stats.strong_checks;
        bool side = true;
        const auto& neighbors = certificate.neighbors(vertex);
        for (std::size_t i = 0; side && i < neighbors.size(); ++i) {
            const auto& left = certificate.neighbors(neighbors[i]);
            for (std::size_t j = i + 1; side && j < neighbors.size(); ++j) {
                if (std::binary_search(left.begin(), left.end(), neighbors[j])) continue;
                const auto& right = certificate.neighbors(neighbors[j]);
                std::size_t common = 0;
                auto a = left.begin();
                auto b = right.begin();
                while (a != left.end() && b != right.end() && common < k) {
                    if (*a < *b) ++a;
                    else if (*b < *a) ++b;
                    else { ++common; ++a; ++b; }
                }
                side = common >= k;
            }
        }
        if (side) strong.insert(vertex);
    }
    return strong;
}

struct SweepState {
    std::map<Vertex, std::size_t> group_of;
    std::set<Vertex> verified;
    std::map<Vertex, std::size_t> deposit;
    std::vector<std::size_t> group_deposit;
    std::vector<bool> group_done;

    explicit SweepState(const std::vector<VertexSet>& groups)
        : group_deposit(groups.size()), group_done(groups.size()) {
        for (std::size_t i = 0; i < groups.size(); ++i) {
            for (const auto vertex : groups[i]) group_of[vertex] = i;
        }
    }
};

// Algorithm 4: iterative SWEEP, sharing deposits across calls for the same source.
void sweep(Vertex seed, const Certificate& data, const std::set<Vertex>& strong,
           std::size_t k, SweepState& state, GlobalCutStats& stats) {
    const auto& certificate = data.graph;
    std::queue<Vertex> pending;
    const auto mark = [&](Vertex vertex) {
        if (state.verified.insert(vertex).second) pending.push(vertex);
    };
    mark(seed);
    while (!pending.empty()) {
        const auto vertex = pending.front();
        pending.pop();
        for (const auto neighbor : certificate.neighbors(vertex)) {
            if (state.verified.contains(neighbor)) continue;
            ++state.deposit[neighbor];
            if (strong.contains(vertex) || state.deposit[neighbor] >= k) {
                ++stats.neighbor_sweeps;
                mark(neighbor);
            }
        }
        const auto group = state.group_of.at(vertex);
        if (!state.group_done[group] && (strong.contains(vertex) || ++state.group_deposit[group] >= k)) {
            state.group_done[group] = true;
            for (const auto member : data.groups[group]) {
                if (!state.verified.contains(member)) { ++stats.group_sweeps; mark(member); }
            }
        }
    }
}

VertexSet decreasing_distance_order(const Graph& graph, const VertexSet& ordered, Vertex source) {
    // Reverse BFS discovery order gives non-increasing distance in the input graph.
    VertexSet order;
    std::set<Vertex> seen{source};
    const std::unordered_set<Vertex> selected(ordered.begin(), ordered.end());
    std::queue<Vertex> pending;
    pending.push(source);
    while (!pending.empty()) {
        const auto vertex = pending.front(); pending.pop();
        order.push_back(vertex);
        for (const auto neighbor : graph.neighbors(vertex)) {
            if (selected.contains(neighbor) && seen.insert(neighbor).second) pending.push(neighbor);
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// Algorithm 2, retained as a baseline for tests.
VertexSet global_cut(const Graph& graph, const Certificate& data, VertexFlow& flow,
                     std::size_t k, GlobalCutStats& stats) {
    const auto source = minimum_degree_vertex(graph, flow.vertices());
    for (const auto vertex : flow.vertices()) {
        auto cut = local_cut(data.graph, flow, source, vertex, k, stats);
        if (!cut.empty()) return cut;
    }
    const auto& neighbors = data.graph.neighbors(source);
    for (std::size_t i = 0; i < neighbors.size(); ++i) {
        for (std::size_t j = i + 1; j < neighbors.size(); ++j) {
            auto cut = local_cut(data.graph, flow, neighbors[i], neighbors[j], k, stats);
            if (!cut.empty()) return cut;
        }
    }
    return {};
}

// Algorithm 3: GLOBAL-CUT*.
VertexSet global_cut_star(const Graph& graph, const Certificate& data, VertexFlow& flow,
                          std::size_t k, const std::set<Vertex>& strong, GlobalCutStats& stats) {
    const auto& ordered = flow.vertices();
    const auto source = strong.empty() ? minimum_degree_vertex(graph, ordered) : *strong.begin();
    SweepState state(data.groups);
    sweep(source, data, strong, k, state, stats);
    for (const auto vertex : decreasing_distance_order(graph, ordered, source)) {
        if (state.verified.contains(vertex)) continue;
        auto cut = local_cut(data.graph, flow, source, vertex, k, stats);
        if (!cut.empty()) return cut;
        sweep(vertex, data, strong, k, state, stats);
    }
    // A strong side source cannot belong to a separator of size < k.
    if (strong.contains(source)) return {};
    const auto& neighbors = data.graph.neighbors(source);
    for (std::size_t i = 0; i < neighbors.size(); ++i) {
        for (std::size_t j = i + 1; j < neighbors.size(); ++j) {
            if (state.group_of.at(neighbors[i]) == state.group_of.at(neighbors[j])) {
                ++stats.group_pair_skips;
                continue;
            }
            auto cut = local_cut(data.graph, flow, neighbors[i], neighbors[j], k, stats);
            if (!cut.empty()) return cut;
        }
    }
    return {};
}
} // namespace

Graph sparse_certificate(const Graph& graph, const VertexSet& vertices, std::size_t k) {
    return build_certificate(graph, vertices, k).graph;
}

VertexSet find_global_cut_with_options(const Graph& graph, const VertexSet& vertices, std::size_t k,
                                       bool sweeps, GlobalCutStats& stats) {
    stats = {};
    if (k == 0 || k >= vertices.size()) {
        throw std::invalid_argument("GLOBAL_CUT requires 1 <= k < the number of vertices");
    }
    const auto data = build_certificate(graph, vertices, k);
    VertexFlow flow(data.graph, vertices, VertexFlow::EdgeCapacity::Unit);
    if (!sweeps) return global_cut(graph, data, flow, k, stats);
    const auto strong = strong_side_vertices(data.graph, flow.vertices(), k, {}, stats);
    return global_cut_star(graph, data, flow, k, strong, stats);
}

VertexSet find_global_cut_reusing(const Graph& graph, const VertexSet& vertices, std::size_t k,
                                 StrongSideCache& cache, GlobalCutStats& stats) {
    stats = {};
    if (k == 0 || k >= vertices.size()) {
        throw std::invalid_argument("GLOBAL_CUT requires 1 <= k < the number of vertices");
    }
    auto data = build_certificate(graph, vertices, k);
    VertexFlow flow(data.graph, vertices, VertexFlow::EdgeCapacity::Unit);
    auto strong = strong_side_vertices(data.graph, flow.vertices(), k, cache, stats);
    auto cut = global_cut_star(graph, data, flow, k, strong, stats);
    // Separator vertices lose neighbors after partitioning and can become strong.
    std::set<Vertex> recheck(cut.begin(), cut.end());
    for (const auto vertex : strong) {
        // Lemma 16 uses the graph being partitioned, not its sparse certificate.
        const auto& neighbors = graph.neighbors(vertex);
        if (std::any_of(cut.begin(), cut.end(), [&](Vertex separator_vertex) {
                return std::binary_search(neighbors.begin(), neighbors.end(), separator_vertex);
            })) {
            recheck.insert(vertex);
        }
    }
    cache = std::make_shared<StrongSideInfo>(StrongSideInfo{k, flow.vertices(), std::move(strong), std::move(recheck)});
    return cut;
}

VertexSet find_global_cut(const Graph& graph, const VertexSet& vertices, std::size_t k) {
    GlobalCutStats stats;
    return find_global_cut_with_options(graph, vertices, k, true, stats);
}

} // namespace downsizing
