#include "graph_downsizing/graph.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace downsizing {
namespace {

std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

RequestedSizes parse_requested_sizes(std::string_view line) {
    line = trim(line);
    const bool sizes = line.starts_with("r:");
    if (!sizes && !line.starts_with("alpha:")) {
        return {};
    }
    line.remove_prefix(sizes ? 2 : 6);
    RequestedSizes result;
    while (true) {
        const auto comma = line.find(',');
        const auto token = trim(line.substr(0, comma));
        if (token.empty()) {
            throw std::invalid_argument("Empty value in requested sizes header");
        }
        if (sizes) {
            std::size_t value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc{} || end != token.data() + token.size() || value == 0) {
                throw std::invalid_argument("Requested r must be a positive integer");
            }
            result.sizes.push_back(value);
        } else {
            double value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc{} || end != token.data() + token.size() || !(value > 0 && value <= 1)) {
                throw std::invalid_argument("Requested alpha must be in (0, 1]");
            }
            result.alphas.push_back(value);
        }
        if (comma == std::string_view::npos) {
            return result;
        }
        line.remove_prefix(comma + 1);
    }
}

VertexId parse_id(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-') {
            throw std::invalid_argument("Expected two signed 64-bit vertex IDs");
        }
    }
    VertexId value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("Expected two signed 64-bit vertex IDs");
    }
    return value;
}

} // namespace

Graph Graph::from_edges(std::vector<std::pair<VertexId, VertexId>> edges) {
    if (edges.empty()) {
        throw std::invalid_argument("Empty graph: an edge-list input must contain at least one edge");
    }
    Graph graph;
    for (const auto& [u, v] : edges) {
        if (u == v) {
            throw std::invalid_argument("Self-loops are not supported");
        }
        graph.adjacency_[u].push_back(v);
        graph.adjacency_[v].push_back(u);
    }
    std::size_t degree_sum = 0;
    for (auto& [vertex, neighbors] : graph.adjacency_) {
        (void)vertex;
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        degree_sum += neighbors.size();
    }
    graph.edge_count_ = degree_sum / 2;

    const Vertex start = graph.adjacency_.begin()->first;
    std::set<Vertex> visited{start};
    std::vector<Vertex> queue{start};
    for (std::size_t head = 0; head < queue.size(); ++head) {
        for (const Vertex next : graph.neighbors(queue[head])) {
            if (visited.insert(next).second) {
                queue.push_back(next);
            }
        }
    }
    if (queue.size() != graph.vertex_count()) {
        throw std::invalid_argument("Input graph is disconnected; Algorithm 1 requires a connected graph");
    }
    return graph;
}

GraphInput read_graph_input(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Graph file does not exist or is not a regular file: " + path.string());
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open graph file: " + path.string());
    }
    std::vector<std::pair<VertexId, VertexId>> edges;
    std::string line;
    std::size_t line_number = 0;
    RequestedSizes requested;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream tokens(line);
        std::string first;
        if (!(tokens >> first) || first.front() == '#' || first.front() == '%') {
            continue;
        }
        try {
            if (line_number == 1) {
                requested = parse_requested_sizes(line);
                if (!requested.sizes.empty() || !requested.alphas.empty()) {
                    continue;
                }
            }
            std::string second;
            std::string extra;
            if (!(tokens >> second) || (tokens >> extra)) {
                throw std::invalid_argument("Expected exactly two vertex IDs per edge line");
            }
            const auto u = parse_id(first);
            const auto v = parse_id(second);
            if (u == v) {
                throw std::invalid_argument("Self-loops are not supported");
            }
            edges.emplace_back(u, v);
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (input.bad()) {
        throw std::runtime_error("Failed to read graph file: " + path.string());
    }
    try {
        auto graph = Graph::from_edges(std::move(edges));
        // Validate explicit r against the graph size before any calculation starts.
        static_cast<void>(make_sizes(1, graph.vertex_count(), requested));
        return {std::move(graph), std::move(requested)};
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(path.string() + ": " + error.what());
    }
}

Graph read_graph(const std::filesystem::path& path) {
    return read_graph_input(path).graph;
}

} // namespace downsizing
