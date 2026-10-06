#include "graph_downsizing/connectivity.hpp"
#include "graph_downsizing/global_cut.hpp"
#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/solver.hpp"
#include "graph_downsizing/maximal_kvcc_enumerator.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace downsizing;
using Mask = unsigned;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function>
void expect_error(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected invalid_argument");
}

// Independent oracle: bit masks, vertex deletions, reachability; no flow code.
struct Oracle {
    std::vector<Mask> adjacency;

    Mask reachable(Mask remaining, unsigned source) const {
        Mask reached = Mask{1} << source;
        Mask frontier = reached;
        while (frontier != 0) {
            const auto vertex = static_cast<unsigned>(std::countr_zero(frontier));
            frontier &= frontier - 1;
            const auto next = adjacency[vertex] & remaining & ~reached;
            reached |= next;
            frontier |= next;
        }
        return reached;
    }

    bool connected(Mask remaining) const {
        return remaining != 0 && reachable(remaining, static_cast<unsigned>(std::countr_zero(remaining))) == remaining;
    }

    std::size_t connectivity(Mask subset) const {
        const auto count = static_cast<std::size_t>(std::popcount(subset));
        if (count < 2) {
            return 0;
        }
        auto best = count - 1;
        for (auto cut = subset;; cut = (cut - 1) & subset) {
            const auto size = static_cast<std::size_t>(std::popcount(cut));
            const auto remaining = subset & ~cut;
            if (size < best && std::popcount(remaining) >= 2 && !connected(remaining)) {
                best = size;
            }
            if (cut == 0) {
                return best;
            }
        }
    }

    std::size_t pair_paths(unsigned source, unsigned target) const {
        auto without_edge = *this;
        const bool adjacent = (adjacency[source] & (Mask{1} << target)) != 0;
        without_edge.adjacency[source] &= ~(Mask{1} << target);
        without_edge.adjacency[target] &= ~(Mask{1} << source);
        const auto full = (Mask{1} << adjacency.size()) - 1;
        const auto interior = full & ~(Mask{1} << source) & ~(Mask{1} << target);
        auto best = adjacency.size();
        for (auto cut = interior;; cut = (cut - 1) & interior) {
            const auto size = static_cast<std::size_t>(std::popcount(cut));
            if (size < best && (without_edge.reachable(full & ~cut, source) & (Mask{1} << target)) == 0) {
                best = size;
            }
            if (cut == 0) {
                return best + (adjacent ? 1U : 0U);
            }
        }
    }
};

VertexSet labels(std::size_t n) {
    VertexSet result;
    for (std::size_t i = 0; i < n; ++i) {
        result.push_back(-17 + static_cast<Vertex>(i) * 19);
    }
    return result;
}

VertexSet subset_vertices(const VertexSet& vertices, Mask mask) {
    VertexSet subset;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if ((mask & (Mask{1} << i)) != 0) {
            subset.push_back(vertices[i]);
        }
    }
    return subset;
}

std::vector<std::pair<Vertex, Vertex>> edges(const Oracle& oracle, const VertexSet& vertices) {
    std::vector<std::pair<Vertex, Vertex>> result;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            if ((oracle.adjacency[i] & (Mask{1} << j)) != 0) {
                result.emplace_back(vertices[i], vertices[j]);
            }
        }
    }
    return result;
}

// Embed arbitrary graphs in a connected input; the extra vertex is excluded from tests.
Graph embed(const Oracle& oracle, const VertexSet& vertices) {
    auto result = edges(oracle, vertices);
    for (const Vertex vertex : vertices) {
        result.emplace_back(vertex, 1000);
    }
    return Graph::from_edges(std::move(result));
}

std::vector<std::size_t> oracle_table(const Oracle& oracle) {
    std::vector<std::size_t> table(Mask{1} << oracle.adjacency.size());
    for (Mask subset = 0; subset < table.size(); ++subset) {
        table[subset] = oracle.connectivity(subset);
    }
    return table;
}

std::set<VertexSet> oracle_maximal(const VertexSet& vertices, const std::vector<std::size_t>& table,
                                 std::size_t k, std::size_t minimum_size = 1) {
    std::vector<Mask> candidates;
    for (Mask subset = 0; subset < table.size(); ++subset) {
        if (table[subset] >= k && static_cast<std::size_t>(std::popcount(subset)) > k) {
            candidates.push_back(subset);
        }
    }
    std::set<VertexSet> result;
    for (const auto subset : candidates) {
        const bool contained = std::any_of(candidates.begin(), candidates.end(), [&](Mask other) {
            return other != subset && (subset & other) == subset;
        });
        if (!contained && static_cast<std::size_t>(std::popcount(subset)) >= minimum_size) {
            result.insert(subset_vertices(vertices, subset));
        }
    }
    return result;
}

template <class Function>
void all_small_graphs(Function function) {
    for (std::size_t n = 1; n <= 5; ++n) {
        const auto edge_count = n * (n - 1) / 2;
        for (Mask mask = 0; mask < (Mask{1} << edge_count); ++mask) {
            Oracle oracle{std::vector<Mask>(n, 0)};
            unsigned bit = 0;
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j, ++bit) {
                    if ((mask & (Mask{1} << bit)) != 0) {
                        oracle.adjacency[i] |= Mask{1} << j;
                        oracle.adjacency[j] |= Mask{1} << i;
                    }
                }
            }
            function(oracle);
        }
    }
}

template <class Function>
void random_graphs(Function function) {
    std::mt19937 random(20260927);
    for (std::size_t n = 6; n <= 8; ++n) {
        for (unsigned trial = 0; trial < 12; ++trial) {
            Oracle oracle{std::vector<Mask>(n, 0)};
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (random() % 4 <= trial % 4) {
                        oracle.adjacency[i] |= Mask{1} << j;
                        oracle.adjacency[j] |= Mask{1} << i;
                    }
                }
            }
            function(oracle);
        }
    }
}

void verify_flow(const Oracle& oracle) {
    const auto vertices = labels(oracle.adjacency.size());
    const auto graph = embed(oracle, vertices);
    const auto full = (Mask{1} << vertices.size()) - 1;
    const auto expected = oracle.connectivity(full);
    check(vertex_connectivity(graph, vertices) == expected, "Connectivity disagrees with deletion oracle");
    for (std::size_t k = 0; k <= vertices.size(); ++k) {
        check(is_k_connected(graph, vertices, k) == (expected >= k), "Wrong connectivity threshold");
    }
    for (const auto mode : {VertexFlow::EdgeCapacity::VertexBound, VertexFlow::EdgeCapacity::Unit}) {
        VertexFlow flow(graph, vertices, mode);
        for (unsigned i = 0; i < vertices.size(); ++i) {
            for (unsigned j = i + 1; j < vertices.size(); ++j) {
                const auto paths = oracle.pair_paths(i, j);
                for (std::size_t limit = 0; limit <= vertices.size(); ++limit) {
                    const auto result = flow.between(vertices[i], vertices[j], limit);
                    check(result.paths == std::min(paths, limit), "Pair flow disagrees with deletion oracle");
                    check(flow.between(vertices[j], vertices[i], limit).paths == result.paths, "Residual reset or reverse flow failed");
                    if ((oracle.adjacency[i] & (Mask{1} << j)) == 0 && result.paths < limit) {
                        check(result.separator.size() == result.paths, "Separator cardinality differs from flow");
                        Mask cut = 0;
                        for (const Vertex vertex : result.separator) {
                            const auto found = std::find(vertices.begin(), vertices.end(), vertex);
                            check(found != vertices.end() && vertex != vertices[i] && vertex != vertices[j], "Invalid separator vertex");
                            cut |= Mask{1} << static_cast<std::size_t>(found - vertices.begin());
                        }
                        check((oracle.reachable(full & ~cut, i) & (Mask{1} << j)) == 0, "Separator does not separate endpoints");
                    }
                }
            }
        }
    }
}

void test_flow() {
    std::size_t count = 0;
    all_small_graphs([&](const Oracle& oracle) { verify_flow(oracle); ++count; });
    random_graphs(verify_flow);
    const auto graph = Graph::from_edges({{1, 2}, {2, 3}});
    check(vertex_connectivity(graph, {}) == 0 && vertex_connectivity(graph, {2}) == 0, "Empty/singleton connectivity");
    check(vertex_connectivity(graph, {3, 1}) == 0, "Subset must be induced, exclude vertex 2");
    expect_error([&] { VertexFlow flow(graph, {1, 1}); });
    expect_error([&] { VertexFlow flow(graph, {99}); });
    VertexFlow flow(graph, {3, 2, 1});
    expect_error([&] { static_cast<void>(flow.between(1, 1, 1)); });
    expect_error([&] { static_cast<void>(flow.between(1, 99, 1)); });

    std::vector<std::pair<Vertex, Vertex>> path;
    VertexSet vertices;
    for (Vertex i = 0; i < 20000; ++i) {
        vertices.push_back(i);
        if (i > 0) {
            path.emplace_back(i - 1, i);
        }
    }
    const auto large = Graph::from_edges(std::move(path));
    VertexFlow long_flow(large, vertices);
    check(long_flow.between(0, 19999, 2).paths == 1, "Long path without recursive stack overflow");
    std::cout << count << " exhaustive graphs + 36 random graphs; all pairs, both directions, all thresholds; 20000-vertex path\n";
}

// Test-only GLOBAL_CUT substitute: exhaustively delete vertices, without any flow code.
VertexSet deletion_global_cut(const Graph& graph, const VertexSet& vertices, std::size_t k) {
    check(!vertices.empty() && vertices.size() <= 10, "Deletion substitute supports 1..10 vertices");
    check(std::is_sorted(vertices.begin(), vertices.end()), "GLOBAL_CUT input must be sorted");
    Oracle oracle{std::vector<Mask>(vertices.size(), 0)};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        for (const auto neighbor : graph.neighbors(vertices[i])) {
            const auto found = std::find(vertices.begin(), vertices.end(), neighbor);
            if (found != vertices.end()) {
                oracle.adjacency[i] |= Mask{1} << static_cast<std::size_t>(found - vertices.begin());
            }
        }
        check(static_cast<std::size_t>(std::popcount(oracle.adjacency[i])) >= k,
              "GLOBAL_CUT must only receive a k-core");
    }
    const auto full = (Mask{1} << vertices.size()) - 1;
    check(oracle.connected(full), "GLOBAL_CUT must only receive connected components");
    for (Mask cut = 1; cut < full; ++cut) {
        if (static_cast<std::size_t>(std::popcount(cut)) < k &&
            std::popcount(full & ~cut) >= 2 && !oracle.connected(full & ~cut)) {
            return subset_vertices(vertices, cut);
        }
    }
    return {};
}

void verify_global_cut(const Oracle& oracle) {
    const auto n = oracle.adjacency.size();
    const auto full = (Mask{1} << n) - 1;
    if (n < 2 || !oracle.connected(full)) {
        return;
    }
    const auto vertices = labels(n);
    const auto graph = embed(oracle, vertices); // Auxiliary edges must stay outside the certificate.
    const auto expected = oracle.connectivity(full);
    for (std::size_t k = 1; k < n; ++k) {
        const auto certificate = sparse_certificate(graph, vertices, k);
        check(certificate.vertex_count() == n && certificate.edge_count() <= k * (n - 1),
              "Certificate vertex set or edge bound failed");
        Oracle sparse{std::vector<Mask>(n, 0)};
        for (std::size_t i = 0; i < n; ++i) {
            for (const auto neighbor : certificate.neighbors(vertices[i])) {
                const auto found = std::find(vertices.begin(), vertices.end(), neighbor);
                check(found != vertices.end(), "Certificate added an external vertex");
                const auto j = static_cast<std::size_t>(found - vertices.begin());
                check((oracle.adjacency[i] & (Mask{1} << j)) != 0, "Certificate added an edge");
                sparse.adjacency[i] |= Mask{1} << j;
            }
        }
        // Strong certificate property needed to use a cut of SC to partition the original graph.
        for (Mask cut = 0; cut < full; ++cut) {
            if (static_cast<std::size_t>(std::popcount(cut)) >= k) {
                continue;
            }
            const auto remaining = full & ~cut;
            for (unsigned i = 0; i < n; ++i) {
                if ((remaining & (Mask{1} << i)) != 0) {
                    check(sparse.reachable(remaining, i) == oracle.reachable(remaining, i),
                          "Certificate changed components after deletion of fewer than k vertices");
                }
            }
        }
        GlobalCutStats baseline_stats;
        const auto baseline = find_global_cut_with_options(graph, vertices, k, false, baseline_stats);
        GlobalCutStats sweep_stats;
        const auto separator = find_global_cut_with_options(graph, vertices, k, true, sweep_stats);
        check(baseline.empty() == separator.empty(), "Sweep and baseline disagree");
        StrongSideCache parent;
        GlobalCutStats inherited_stats;
        check(find_global_cut_reusing(graph, vertices, k, parent, inherited_stats) == separator,
              "Cold inherited search differs from regular search");
        check(inherited_stats.strong_checks == n && inherited_stats.strong_reused == 0, "Cold cache counts");
        std::set<Vertex> expected_recheck(separator.begin(), separator.end());
        for (const auto vertex : parent->strong) {
            for (const auto cut_vertex : separator) {
                const auto& neighbors = graph.neighbors(vertex);
                if (std::binary_search(neighbors.begin(), neighbors.end(), cut_vertex)) expected_recheck.insert(vertex);
            }
        }
        check(parent->recheck == expected_recheck, "Lemma 16 uses neighbors in the graph being partitioned");
        // Apply inheritance only to actual OVERLAP-PARTITION children.
        Mask separator_mask = 0;
        for (const auto vertex : separator) {
            separator_mask |= Mask{1} << static_cast<std::size_t>(std::find(vertices.begin(), vertices.end(), vertex) - vertices.begin());
        }
        Mask remaining = full & ~separator_mask;
        while (!separator.empty() && remaining != 0) {
            const auto component = oracle.reachable(remaining, static_cast<unsigned>(std::countr_zero(remaining)));
            remaining &= ~component;
            VertexSet child;
            for (std::size_t i = 0; i < n; ++i) {
                if (((component | separator_mask) & (Mask{1} << i)) != 0) child.push_back(vertices[i]);
            }
            if (child.size() <= k) continue;
            auto inherited = parent;
            const auto cached_cut = find_global_cut_reusing(graph, child, k, inherited, inherited_stats);
            check(cached_cut.empty() == is_k_connected(graph, child, k), "Inherited search misclassified a partition child");
            if (!cached_cut.empty()) {
                Mask removed = 0;
                for (const auto vertex : cached_cut) {
                    check(std::binary_search(child.begin(), child.end(), vertex), "Inherited cut must stay inside child");
                    removed |= Mask{1} << static_cast<std::size_t>(std::find(vertices.begin(), vertices.end(), vertex) - vertices.begin());
                }
                check(cached_cut.size() < k && !oracle.connected((component | separator_mask) & ~removed),
                      "Inherited cut must disconnect child in independent oracle");
            }
            std::size_t expected_checks = 0;
            for (const auto vertex : child) expected_checks += parent->recheck.contains(vertex) ? 1U : 0U;
            check(inherited_stats.strong_checks == expected_checks &&
                  inherited_stats.strong_reused == child.size() - expected_checks,
                  "Must recheck separator vertices and adjacent parent strong vertices");
        }
        if (k + 1 < n) {
            auto changed_k = parent;
            static_cast<void>(find_global_cut_reusing(graph, vertices, k + 1, changed_k, inherited_stats));
            check(inherited_stats.strong_reused == 0 && inherited_stats.strong_checks == n,
                  "A different k must invalidate inherited classifications");
        }
        check(separator.empty() == (expected >= k), "GLOBAL_CUT disagrees with deletion oracle");
        if (!separator.empty()) {
            check(separator.size() < k, "GLOBAL_CUT cut is too large");
            Mask cut = 0;
            for (const auto vertex : separator) {
                const auto found = std::find(vertices.begin(), vertices.end(), vertex);
                check(found != vertices.end(), "GLOBAL_CUT returned an external vertex");
                cut |= Mask{1} << static_cast<std::size_t>(found - vertices.begin());
            }
            check(static_cast<std::size_t>(std::popcount(cut)) == separator.size() &&
                  std::popcount(full & ~cut) >= 2 && !oracle.connected(full & ~cut),
                  "GLOBAL_CUT must disconnect the original induced graph");
        }
    }
}

void test_global_cut() {
    all_small_graphs(verify_global_cut);
    random_graphs(verify_global_cut);

    // Source 0 lies in the separator {0,1}. All source-pair tests succeed for k=3;
    // only phase 2, testing its neighbors across the two cliques, can find the cut.
    std::vector<std::pair<Vertex, Vertex>> input;
    for (const auto& clique : {VertexSet{2,3,4,5,6}, VertexSet{7,8,9,10,11}}) {
        for (std::size_t i = 0; i < clique.size(); ++i) {
            input.emplace_back(1, clique[i]);
            for (std::size_t j = i + 1; j < clique.size(); ++j) {
                input.emplace_back(clique[i], clique[j]);
            }
        }
    }
    for (const auto v : {2,3,7,8}) {
        input.emplace_back(0, v);
    }
    const auto graph = Graph::from_edges(std::move(input));
    const VertexSet vertices{0,1,2,3,4,5,6,7,8,9,10,11};
    const auto certificate = sparse_certificate(graph, vertices, 3);
    check(certificate.edge_count() < graph.edge_count(), "Dense fixture should actually be sparsified");
    VertexFlow flow(certificate, vertices, VertexFlow::EdgeCapacity::Unit);
    for (Vertex v = 1; v < 12; ++v) {
        check(graph.neighbors(v).size() > graph.neighbors(0).size(), "Source must be unique minimum-degree vertex");
        check(flow.between(0, v, 3).paths == 3, "Phase 1 must not find a cut in this fixture");
    }
    GlobalCutStats baseline_stats;
    check(find_global_cut_with_options(graph, vertices, 3, false, baseline_stats) == VertexSet({0,1}),
          "Baseline phase 2 must detect a cut containing the source");
    check(find_global_cut(graph, vertices, 3) == VertexSet({0,1}), "Sweeps must retain the separator");

    // A separator vertex can become strong after losing neighbors from another part.
    const auto bow_tie = Graph::from_edges({{0,1},{0,2},{1,2},{0,3},{0,4},{3,4}});
    StrongSideCache bow_tie_parent;
    GlobalCutStats bow_tie_stats;
    check(find_global_cut_reusing(bow_tie, {0,1,2,3,4}, 2, bow_tie_parent, bow_tie_stats) == VertexSet{0},
          "The shared triangle vertex must be the separator");
    check(!bow_tie_parent->strong.contains(0) && bow_tie_parent->recheck.contains(0),
          "A non-strong separator vertex must still be scheduled for rechecking");
    for (const auto& triangle : {VertexSet{0,1,2}, VertexSet{0,3,4}}) {
        auto child_cache = bow_tie_parent;
        check(find_global_cut_reusing(bow_tie, triangle, 2, child_cache, bow_tie_stats).empty(),
              "Each child triangle must be 2-connected");
        check(child_cache->strong.contains(0), "Separator vertex must become strong in each child");
    }
    check(!bow_tie_parent->strong.contains(0), "Children must not modify parent classifications");

    // A genuine split retains the remote clique's unchanged certificate neighborhoods.
    std::vector<std::pair<Vertex, Vertex>> chain_edges;
    for (Vertex start : {0,4,8}) {
        for (Vertex u = start; u < start + 4; ++u) {
            for (Vertex v = u + 1; v < start + 4; ++v) chain_edges.emplace_back(u, v);
        }
    }
    chain_edges.emplace_back(3,4);
    chain_edges.emplace_back(7,8);
    const auto chain = Graph::from_edges(std::move(chain_edges));
    StrongSideCache chain_cache;
    GlobalCutStats chain_stats;
    const auto chain_cut = find_global_cut_reusing(chain, vertices, 2, chain_cache, chain_stats);
    check(chain_cut == VertexSet{3}, "Fixture child must come from the actual overlap partition");
    auto child_cache = chain_cache;
    const VertexSet child_vertices{3,4,5,6,7,8,9,10,11};
    const auto inherited_cut = find_global_cut_reusing(chain, child_vertices, 2, child_cache, chain_stats);
    check(chain_stats.strong_reused > 0,
          "A smaller child must inherit parent classifications");
    check(!inherited_cut.empty() && inherited_cut.size() < 2, "Split child must retain a small cut");
    std::set<Vertex> reachable{child_vertices.front()};
    if (inherited_cut.front() == child_vertices.front()) reachable = {child_vertices.back()};
    VertexSet queue(reachable.begin(), reachable.end());
    for (std::size_t head = 0; head < queue.size(); ++head) {
        for (const auto neighbor : chain.neighbors(queue[head])) {
            if (neighbor != inherited_cut.front() && std::binary_search(child_vertices.begin(), child_vertices.end(), neighbor) &&
                reachable.insert(neighbor).second) queue.push_back(neighbor);
        }
    }
    check(reachable.size() < child_vertices.size() - 1, "Inherited separator must disconnect the child");
    check(chain_cache->vertices.size() == 12, "Child update must not mutate parent shared by siblings");
    auto sibling_cache = chain_cache;
    check(find_global_cut_reusing(chain, {0,1,2,3}, 2, sibling_cache, chain_stats).empty(), "Sibling clique must remain connected");
    check(chain_stats.strong_checks > 0, "Strong vertices adjacent to the parent separator must be rechecked");

    // A cycle at k=2 has no strong side vertices: deposits and side groups do the work.
    const auto cycle = Graph::from_edges({{0,1},{1,2},{2,3},{3,4},{4,5},{5,0}});
    GlobalCutStats cycle_base, cycle_sweep;
    check(find_global_cut_with_options(cycle, {0,1,2,3,4,5}, 2, false, cycle_base).empty(), "Cycle baseline");
    check(find_global_cut_with_options(cycle, {0,1,2,3,4,5}, 2, true, cycle_sweep).empty(), "Cycle sweeps");
    check(cycle_sweep.neighbor_sweeps > 0,
          "Cycle must exercise neighbor deposits");
    const auto bipartite = Graph::from_edges({{0,2},{0,3},{0,4},{0,5},{1,2},{1,3},{1,4},{1,5}});
    GlobalCutStats bipartite_base, bipartite_sweep;
    check(find_global_cut_with_options(bipartite, {0,1,2,3,4,5}, 2, false, bipartite_base).empty(), "K2,4 baseline");
    check(find_global_cut_with_options(bipartite, {0,1,2,3,4,5}, 2, true, bipartite_sweep).empty(), "K2,4 sweeps");
    check(bipartite_sweep.flow_calls == 0 && bipartite_base.flow_calls > 0,
          "Strong-side sweeps must eliminate K2,4 flows");
    const auto phase_two = Graph::from_edges({{0,2},{0,4},{0,5},{1,2},{1,3},{1,5},{2,3},{2,4},{2,5},{3,4}});
    GlobalCutStats pair_stats;
    const auto pair_cut = find_global_cut_with_options(phase_two, {0,1,2,3,4,5}, 3, true, pair_stats);
    check(pair_stats.group_pair_skips > 0, "Phase 2 must skip pairs within a side group");
    check(pair_cut.empty() == is_k_connected(phase_two, {0,1,2,3,4,5}, 3), "Phase-2 fixture optimum");
    // At k=1 the first forest is one group, swept from the source without any flows.
    GlobalCutStats group_stats;
    check(find_global_cut_with_options(cycle, {0,1,2,3,4,5}, 1, true, group_stats).empty(), "Group sweep");
    check(group_stats.group_sweeps > 0 && group_stats.flow_calls == 0, "Whole group must be swept");
    // Exhausted forests before round k must produce singleton groups, never stale components.
    GlobalCutStats exhausted;
    check(find_global_cut_with_options(Graph::from_edges({{0,1},{1,2},{2,3}}), {0,1,2,3}, 3,
                                       true, exhausted).size() == 1, "Exhausted forest cannot certify a path");
    std::cout << "K2,4 flows: baseline=" << bipartite_base.flow_calls << ", sweep=" << bipartite_sweep.flow_calls << '\n';

    const auto small = Graph::from_edges({{1,2},{2,3},{3,4}});
    expect_error([&] { static_cast<void>(sparse_certificate(small, {1,2}, 0)); });
    expect_error([&] { static_cast<void>(sparse_certificate(small, {1}, 1)); });
    expect_error([&] { static_cast<void>(sparse_certificate(small, {1,2,4}, 1)); });
    expect_error([&] { static_cast<void>(sparse_certificate(small, {1,1}, 1)); });
    expect_error([&] { static_cast<void>(find_global_cut(small, {1,2}, 2)); });
    expect_error([&] { static_cast<void>(find_global_cut(small, {1,99}, 1)); });
    check(find_global_cut(small, {3,2,1}, 2) == VertexSet({2}), "Unsorted IDs and a one-vertex cut");
    std::cout << "Connected graphs through 5 vertices + random through 8; certificate cuts; two-phase fixture\n";
}

void test_enumeration_framework() {
    // Initial peeling removes 8 and 9. After the cut, peeling must remove 7 from each side.
    const auto graph = Graph::from_edges({{1,2},{2,3},{3,1},{4,5},{5,6},{6,4},
                                          {3,7},{7,4},{1,8},{8,9}});
    std::vector<VertexSet> calls;
    const MaximalKVCCEnumerator enumerator(graph,
        [&](const Graph& source, const VertexSet& vertices, std::size_t k) -> VertexSet {
            check(&source == &graph && k == 2, "Wrong graph or k passed to GLOBAL_CUT");
            calls.push_back(vertices);
            if (vertices == VertexSet{1,2,3,4,5,6,7}) {
                return {7};
            }
            check(vertices == VertexSet{1,2,3} || vertices == VertexSet{4,5,6},
                  "Peeling must run initially and after partitioning");
            return {};
        });
    const auto result = enumerator.enumerate({9,8,7,6,5,4,3,2,1}, 2);
    check(result == std::vector<VertexSet>{{1,2,3},{4,5,6}} && calls.size() == 3,
          "Framework must process both cut branches");
    calls.clear();
    check(enumerator.enumerate({1,2,3,4,5,6,7,8,9}, 2) == result && calls.size() == 3,
          "Repeated enumeration must not retain traversal state");

    // Peeling a degree-2 bridge separates two K4s before the first GLOBAL_CUT call.
    std::vector<std::pair<Vertex, Vertex>> disjoint_edges;
    for (const auto& clique : {VertexSet{1,2,3,4}, VertexSet{5,6,7,8}}) {
        for (std::size_t i = 0; i < clique.size(); ++i) {
            for (std::size_t j = i + 1; j < clique.size(); ++j) {
                disjoint_edges.emplace_back(clique[i], clique[j]);
            }
        }
    }
    disjoint_edges.emplace_back(4,9);
    disjoint_edges.emplace_back(9,5);
    const auto disjoint = Graph::from_edges(std::move(disjoint_edges));
    std::size_t component_calls = 0;
    const MaximalKVCCEnumerator components(disjoint,
        [&](const Graph&, const VertexSet& vertices, std::size_t k) -> VertexSet {
            ++component_calls;
            check(k == 3 && (vertices == VertexSet{1,2,3,4} || vertices == VertexSet{5,6,7,8}),
                  "Connected components must be separated after k-core peeling");
            return {};
        });
    check(components.enumerate({1,2,3,4,5,6,7,8,9}, 3) ==
          std::vector<VertexSet>{{1,2,3,4},{5,6,7,8}} && component_calls == 2,
          "Every initial connected component must be processed");

    // One cut produces three branches. Both separator vertices must be in every leaf.
    const std::vector<VertexSet> cliques{{1,2,3,4},{1,2,5,6},{1,2,7,8}};
    std::vector<std::pair<Vertex, Vertex>> overlapping_edges;
    for (const auto& clique : cliques) {
        for (std::size_t i = 0; i < clique.size(); ++i) {
            for (std::size_t j = i + 1; j < clique.size(); ++j) {
                overlapping_edges.emplace_back(clique[i], clique[j]);
            }
        }
    }
    const auto overlapping = Graph::from_edges(std::move(overlapping_edges));
    std::size_t overlap_calls = 0;
    const MaximalKVCCEnumerator overlap(overlapping,
        [&](const Graph&, const VertexSet& vertices, std::size_t k) -> VertexSet {
            ++overlap_calls;
            check(k == 3, "Wrong overlapping-component threshold");
            if (vertices == VertexSet{1,2,3,4,5,6,7,8}) {
                return {2,1};
            }
            check(std::find(cliques.begin(), cliques.end(), vertices) != cliques.end(),
                  "Separator must be restored to every component");
            return {};
        });
    check(overlap.enumerate({1,2,3,4,5,6,7,8}, 3) == cliques && overlap_calls == 4,
          "Overlap partition must preserve all three maximal components");

    // A non-cut would otherwise enqueue the same component forever.
    const MaximalKVCCEnumerator bad_cut(graph,
        [](const Graph&, const VertexSet&, std::size_t) { return VertexSet{1}; });
    try {
        static_cast<void>(bad_cut.enumerate({1,2,3}, 2));
        throw std::runtime_error("Expected invalid partition to throw");
    } catch (const std::logic_error& error) {
        check(std::string(error.what()).find("does not disconnect") != std::string::npos,
              "Invalid partition must fail instead of looping");
    }
}

void verify_enumeration(const Oracle& oracle) {
    const auto vertices = labels(oracle.adjacency.size());
    const auto graph = embed(oracle, vertices);
    const auto table = oracle_table(oracle);
    const MaximalKVCCEnumerator enumerator(graph);
    const MaximalKVCCEnumerator mocked(graph, deletion_global_cut);
    for (std::size_t k = 1; k <= vertices.size(); ++k) {
        const auto actual = enumerator.enumerate(vertices, k);
        check(actual == mocked.enumerate(vertices, k), "Production GLOBAL_CUT differs from deletion substitution");
        check(std::set<VertexSet>(actual.begin(), actual.end()) == oracle_maximal(vertices, table, k), "Incomplete or nonmaximal enumeration");
        check(std::set<VertexSet>(actual.begin(), actual.end()).size() == actual.size(), "Duplicate enumerated set");
    }
}

void test_enumeration() {
    std::size_t count = 0;
    all_small_graphs([&](const Oracle& oracle) { verify_enumeration(oracle); ++count; });
    random_graphs(verify_enumeration);
    const auto graph = Graph::from_edges({{1, 2}, {2, 3}});
    const MaximalKVCCEnumerator enumerator(graph);
    check(enumerator.enumerate({}, 1).empty(), "Empty subset has no positive-connected components");
    check(enumerator.enumerate({1}, 1).empty(), "Singleton has connectivity 0");
    expect_error([&] { static_cast<void>(enumerator.enumerate({1, 2}, 0)); });
    expect_error([&] { static_cast<void>(enumerator.enumerate({1, 1}, 2)); });
    expect_error([&] { static_cast<void>(enumerator.enumerate({99}, 2)); });
    std::cout << count << " exhaustive graphs + 36 random graphs; all k; independent maximality oracle\n";
}

void verify_tree(const Oracle& oracle) {
    const auto n = oracle.adjacency.size();
    const auto full = (Mask{1} << n) - 1;
    if (n < 2 || !oracle.connected(full)) {
        return;
    }
    const auto vertices = labels(n);
    const auto graph = Graph::from_edges(edges(oracle, vertices));
    const auto table = oracle_table(oracle);
    for (std::size_t minimum = 1; minimum <= n; ++minimum) {
        const auto tree = build_tree(graph, minimum);
        check(tree.nodes[0].parent == 0 && tree.nodes[0].level == 1 && *tree.nodes[0].vertices == vertices, "Wrong root");
        std::size_t indexed = 0;
        for (std::size_t k = 1; k <= n; ++k) {
            std::set<VertexSet> actual;
            if (k < tree.levels.size()) {
                for (const auto id : tree.levels[k]) {
                    check(id < tree.nodes.size() && tree.nodes[id].level == k, "Invalid level index");
                    check(actual.insert(*tree.nodes[id].vertices).second, "Same set duplicated at one level");
                    ++indexed;
                }
            }
            check(actual == oracle_maximal(vertices, table, k, minimum), "Tree level incomplete after size pruning");
        }
        check(indexed == tree.nodes.size(), "A node is missing from level indices");
        std::vector<unsigned> child_occurrences(tree.nodes.size(), 0);
        for (std::size_t id = 0; id < tree.nodes.size(); ++id) {
            const auto& node = tree.nodes[id];
            for (const auto child : node.children) {
                check(child > id && child < tree.nodes.size() && tree.nodes[child].parent == id, "Invalid child relation");
                ++child_occurrences[child];
            }
            if (id > 0) {
                const auto& parent = tree.nodes[node.parent];
                check(node.level == parent.level + 1, "Levels must be consecutive");
                check(std::includes(parent.vertices->begin(), parent.vertices->end(), node.vertices->begin(), node.vertices->end()), "Child not contained in parent");
            }
            for (std::size_t other = 0; other < id; ++other) {
                if (*node.vertices == *tree.nodes[other].vertices) {
                    check(node.vertices == tree.nodes[other].vertices, "Identical sets must share storage");
                }
            }
        }
        check(child_occurrences[0] == 0, "Root cannot be a child");
        for (std::size_t id = 1; id < tree.nodes.size(); ++id) {
            check(child_occurrences[id] == 1, "Each nonroot node must occur once among children");
        }
    }
}

void test_tree() {
    std::size_t count = 0;
    all_small_graphs([&](const Oracle& oracle) {
        const auto full = (Mask{1} << oracle.adjacency.size()) - 1;
        if (oracle.adjacency.size() >= 2 && oracle.connected(full)) {
            verify_tree(oracle);
            ++count;
        }
    });
    random_graphs(verify_tree);
    const auto graph = Graph::from_edges({{10, 20}, {20, 30}, {30, 40}, {40, 50}, {50, 60}, {60, 10},
                                         {70, 80}, {80, 90}, {90, 70}, {10, 70}});
    const auto tree = build_tree(graph, 1);
    check(tree.nodes.size() == 3 && tree.levels.size() == 3, "Cycle plus triangle: expected root and two level-2 nodes");
    check(*tree.nodes[1].vertices == VertexSet({10, 20, 30, 40, 50, 60}), "Cycle was lost");
    check(*tree.nodes[2].vertices == VertexSet({70, 80, 90}), "Smaller triangle was lost");
    check(build_tree(graph, 4).nodes.size() == 2, "Minimum-size pruning must remove only the triangle");
    // Two K4 graphs sharing an edge: maximal 3-connected sets overlap in two vertices.
    std::vector<std::pair<Vertex, Vertex>> overlapping_edges;
    for (const auto& clique : {VertexSet{1, 2, 3, 4}, VertexSet{3, 4, 5, 6}}) {
        for (std::size_t i = 0; i < clique.size(); ++i) {
            for (std::size_t j = i + 1; j < clique.size(); ++j) {
                overlapping_edges.emplace_back(clique[i], clique[j]);
            }
        }
    }
    const auto overlapping = Graph::from_edges(std::move(overlapping_edges));
    const auto overlapping_tree = build_tree(overlapping, 1);
    check(overlapping_tree.nodes.size() == 4 && overlapping_tree.levels.size() == 4, "Overlapping K4 hierarchy");
    check(overlapping_tree.nodes[0].vertices == overlapping_tree.nodes[1].vertices, "Levels 1 and 2 must share storage");
    check(*overlapping_tree.nodes[2].vertices == VertexSet({1, 2, 3, 4}) &&
          *overlapping_tree.nodes[3].vertices == VertexSet({3, 4, 5, 6}), "Separator vertices must return to both components");
    expect_error([&] { static_cast<void>(build_tree(graph, 0)); });
    expect_error([&] { static_cast<void>(build_tree(graph, 10)); });
    std::cout << count << " connected exhaustive graphs; every minimum size; random connected cases; cycle plus triangle; overlapping K4\n";
}

void verify_solver(const Oracle& oracle, WorkerPool* pool = nullptr) {
    const auto n = oracle.adjacency.size();
    const auto vertices = labels(n);
    const auto table = oracle_table(oracle);
    const auto embedded = embed(oracle, vertices);
    // PDownsize includes disconnected nodes, rejects missing witnesses and visits the last combination.
    for (std::size_t r = 1; r <= n; ++r) {
        for (std::size_t k = 0; k <= r; ++k) {
            std::optional<VertexSet> expected;
            for (Mask subset = 1; subset < table.size(); ++subset) {
                if (static_cast<std::size_t>(std::popcount(subset)) != r || table[subset] < k || k >= r) continue;
                const auto candidate = subset_vertices(vertices, subset);
                if (!expected || candidate < *expected) expected = candidate;
            }
            check(p_downsize(embedded, vertices, r, k) == expected, "PDownsize differs from independent subset/cut oracle");
        }
    }
    const auto full = (Mask{1} << n) - 1;
    if (n < 2 || !oracle.connected(full)) return;
    const auto graph = Graph::from_edges(edges(oracle, vertices));
    ExactSolver solver(graph, 1);
    ExactSolver pruned(graph, (n + 1) / 2);
    std::unique_ptr<ExactSolver> parallel;
    if (pool) parallel = std::make_unique<ExactSolver>(graph, 1, *pool, 1);
    for (std::size_t r = n; r > 0; --r) {
        std::size_t optimum = 0;
        for (Mask subset = 1; subset < table.size(); ++subset) {
            if (static_cast<std::size_t>(std::popcount(subset)) == r) optimum = std::max(optimum, table[subset]);
        }
        const auto& answer = solver.solve(r);
        check(answer.connectivity == optimum && answer.vertices.size() == r, "Wrong downsizing optimum/size");
        Mask witness = 0;
        for (const auto vertex : answer.vertices) {
            const auto found = std::find(vertices.begin(), vertices.end(), vertex);
            check(found != vertices.end(), "Solver returned an external vertex");
            witness |= Mask{1} << static_cast<std::size_t>(found - vertices.begin());
        }
        check(static_cast<std::size_t>(std::popcount(witness)) == r && table[witness] == optimum,
              "Witness fails independent connectivity oracle");
        const auto count = solver.solutions().size();
        check(&solver.solve(r) == &answer && solver.solutions().size() == count, "Repeated r must reuse stored answer");
        if (r >= (n + 1) / 2) check(pruned.solve(r).connectivity == optimum, "Pruned hierarchy lost the optimum");
        if (parallel) {
            const auto& result = parallel->solve(r);
            Mask mask = 0;
            for (const auto vertex : result.vertices) {
                const auto found = std::find(vertices.begin(), vertices.end(), vertex);
                check(found != vertices.end(), "Parallel witness contains an external vertex");
                mask |= Mask{1} << static_cast<std::size_t>(found - vertices.begin());
            }
            check(result.vertices.size() == r && static_cast<std::size_t>(std::popcount(mask)) == r &&
                  result.connectivity == optimum && table[mask] == optimum, "Parallel optimum/witness differs from oracle");
        }
    }
}

void test_solver() {
    WorkerPool pool(4);
    all_small_graphs([&](const Oracle& oracle) { verify_solver(oracle, &pool); });
    random_graphs([&](const Oracle& oracle) { verify_solver(oracle, &pool); });
    const auto graph = Graph::from_edges({{10,20},{20,30},{30,40},{40,50},{50,60},{60,10},
                                          {70,80},{80,90},{90,70},{10,70}});
    ExactSolver solver(graph, 1);
    WorkerPool single_pool(1);
    ExactSolver one_worker(graph, 1, single_pool, 2);
    ExactSolver four_workers(graph, 1, pool, 1);
    for (std::size_t r = 1; r <= graph.vertex_count(); ++r) {
        check(one_worker.solve(r).connectivity == solver.solve(r).connectivity &&
              four_workers.solve(r).connectivity == solver.solve(r).connectivity,
              "One and four workers must give the same optimum");
    }
    check(solver.solve(3).vertices == VertexSet({70,80,90}), "Must try the smaller triangle after exhausting the cycle node");
    check(solver.solve(4).connectivity == 1 && solver.solve(6).connectivity == 2 && solver.solve(9).connectivity == 1,
          "Cycle/triangle fallback and full-size cases");
    check(p_downsize(graph, {90,80,70}, 3, 2) == std::optional<VertexSet>({70,80,90}), "Node ordering must be normalized");
    expect_error([&] { static_cast<void>(solver.solve(0)); });
    expect_error([&] { static_cast<void>(solver.solve(10)); });
    ExactSolver pruned(graph, 4);
    expect_error([&] { static_cast<void>(pruned.solve(3)); });
    expect_error([&] { static_cast<void>(p_downsize(graph, {10,20}, 0, 1)); });
    expect_error([&] { static_cast<void>(p_downsize(graph, {10,20}, 3, 1)); });
    expect_error([&] { static_cast<void>(p_downsize(graph, {10,10}, 1, 0)); });
    std::cout << "All graphs through 5 vertices + random through 8: PDownsize all r/k; connected solver all r; repeated r; pruned tree\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("Expected flow, global_cut, enumeration_framework, enumeration, tree, or solver test group");
        }
        const std::string group = argv[1];
        if (group == "flow") {
            test_flow();
        } else if (group == "global_cut") {
            test_global_cut();
        } else if (group == "enumeration_framework") {
            test_enumeration_framework();
        } else if (group == "enumeration") {
            test_enumeration();
        } else if (group == "solver") {
            test_solver();
        } else if (group == "tree") {
            test_tree();
        } else {
            throw std::invalid_argument("Unknown test group");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
