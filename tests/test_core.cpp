#include "graph_downsizing/alpha.hpp"
#include "graph_downsizing/config.hpp"
#include "graph_downsizing/graph.hpp"
#include "graph_downsizing/solution.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template<class Function>
void expect_error(Function function, std::string_view expected) {
    try {
        function();
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(expected) != std::string_view::npos,
              "Exception does not contain expected diagnostic");
        return;
    }
    throw std::runtime_error("Expected an exception");
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() / ("graph-downsizing-test-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create temporary test directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path write(std::string_view name, std::string_view contents) const {
        const auto target = path_ / name;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target);
        output << contents;
        output.close();
        check(static_cast<bool>(output), "Cannot write test fixture");
        return target;
    }

private:
    std::filesystem::path path_;
};

void test_alpha() {
    downsizing::AlphaSchedule defaults(0.1, 70);
    check(defaults.task_count() == 10, "Default grid must have 10 tasks");
    for (std::size_t i = 1; i <= 10; ++i) {
        const auto task = defaults.next();
        check(task && task->r == 7 * i, "Wrong r at a floating-point boundary");
        check(std::abs(task->alpha - static_cast<double>(i) / 10) < 1e-14, "Wrong alpha");
    }
    check(!defaults.next() && !defaults.next(), "Exhausted grid must remain exhausted");

    downsizing::AlphaSchedule uneven(0.3, 9);
    const std::vector<double> alphas{0.3, 0.6, 0.9, 1};
    const std::vector<std::size_t> sizes{2, 5, 8, 9};
    check(uneven.task_count() == 4, "Wrong uneven task count");
    for (std::size_t i = 0; i < alphas.size(); ++i) {
        const auto task = uneven.next();
        check(task && std::abs(task->alpha - alphas[i]) < 1e-14 && task->r == sizes[i], "Wrong uneven grid");
    }
    check(!uneven.next(), "Final 1 must occur once");

    downsizing::AlphaSchedule duplicates(0.1, 3);
    for (const auto size : std::vector<std::size_t>{1, 1, 1, 1, 1, 1, 2, 2, 2, 3}) {
        const auto task = duplicates.next();
        check(task && task->r == size, "Small graph must retain every alpha with minimum r=1");
    }
    downsizing::AlphaSchedule one(1, 5);
    check(one.task_count() == 1 && one.next()->r == 5 && !one.next(), "Step 1 must produce only the full graph");
    downsizing::AlphaSchedule tiny(1e-6, 100);
    check(tiny.task_count() == 1000000 && tiny.next()->r == 1, "Small steps must be generated lazily");
    downsizing::AlphaSchedule boundary(0.29, 100);
    check(boundary.next()->r == 29, "0.29 times 100 must not lose a vertex to roundoff");
    // Independent integer reference for ordinary decimal steps and graph sizes.
    for (std::size_t numerator = 1; numerator <= 100; ++numerator) {
        for (std::size_t n = 1; n <= 80; ++n) {
            downsizing::AlphaSchedule grid(static_cast<double>(numerator) / 100, n);
            std::size_t fraction = 0;
            while (const auto task = grid.next()) {
                fraction = std::min(std::size_t{100}, fraction + numerator);
                check(task->r == std::max(std::size_t{1}, fraction * n / 100), "Grid differs from integer reference");
            }
            check(fraction == 100, "Reference grid did not reach 1");
        }
    }
    for (const double invalid : {0.0, -0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(), 1e-30}) {
        expect_error([&] { downsizing::AlphaSchedule schedule(invalid, 3); }, "alpha_step");
    }
    expect_error([] { downsizing::AlphaSchedule schedule(0.1, 0); }, "nonempty graph");
}

void test_config() {
    TemporaryDirectory directory;
    auto file = directory.write("config.json", R"({"inputs":["data/g.txt"]})");
    const auto config = downsizing::read_config(file);
    check(config.combination_chunk_size == 256, "Default chunk size failed");
    check(config.alpha_step == 0.1 && config.threads >= 1, "Default step or threads failed");
    check(config.checkpoint_interval.count() == 60 && config.github.upload_interval.count() == 300, "Default intervals failed");
    check(config.inputs[0] == directory.path() / "data/g.txt", "Input paths must be relative to the config file");
    check(config.working_directory == directory.path() / "runs", "Working directory default failed");
    check(!config.github.enabled && config.github.branch == "runs", "GitHub defaults failed");
    check(downsizing::solution_path(config, 0) == directory.path() / "runs/graph_0/g_sol.txt", "Result path failed");

    file = directory.write("configured.json", R"({
        "inputs": ["data/g.txt", "other/g.txt"], "alpha_step": 0.3,
        "threads": 32, "combination_chunk_size": 7, "working_directory": "work/../answers",
        "checkpoint_interval_seconds": 7,
        "github": {"enabled":true, "repository_url":"https://github.com/DenVot/graph_downsizing_enumeration.git",
                   "branch":"runs/test", "token_env":"CUSTOM_TOKEN", "upload_interval_seconds":19}
    })");
    const auto configured = downsizing::read_config(file);
    check(configured.combination_chunk_size == 7, "Configured chunk size failed");
    check(configured.alpha_step == 0.3 && configured.threads == 32, "Configured values failed");
    check(configured.working_directory == directory.path() / "answers", "Path normalization failed");
    check(configured.github.enabled && configured.github.token.empty() && configured.github.token_env == "CUSTOM_TOKEN",
          "Credential configuration failed");
    check(configured.checkpoint_interval.count() == 7 && configured.github.upload_interval.count() == 19,
          "Configured intervals failed");
    check(downsizing::solution_path(configured, 0) != downsizing::solution_path(configured, 1),
          "Equal basenames in different directories must not collide");

    const std::vector<std::pair<std::string, std::string>> invalid{
        {"{", "valid JSON"}, {"[]", "JSON object"}, {"{}", "inputs"},
        {R"({"inputs":[]})", "inputs"}, {R"({"inputs":[12]})", "input path"},
        {R"({"inputs":[""]})", "input path"},
        {R"({"inputs":["a.txt","./a.txt"]})", "duplicate paths"},
        {R"({"inputs":["a.txt"],"alpha_step":"0.1"})", "alpha_step"},
        {R"({"inputs":["a.txt"],"alpha_step":0})", "alpha_step"},
        {R"({"inputs":["a.txt"],"alpha_step":true})", "alpha_step"},
        {R"({"inputs":["a.txt"],"combination_chunk_size":0})", "combination_chunk_size"},
        {R"({"inputs":["a.txt"],"combination_chunk_size":1.5})", "combination_chunk_size"},
        {R"({"inputs":["a.txt"],"threads":0})", "threads"},
        {R"({"inputs":["a.txt"],"threads":-1})", "threads"},
        {R"({"inputs":["a.txt"],"threads":1.5})", "threads"},
        {R"({"inputs":["a.txt"],"checkpoint_interval_seconds":0})", "checkpoint_interval_seconds"},
        {R"({"inputs":["a.txt"],"checkpoint_interval_seconds":18446744073709551615})", "checkpoint_interval_seconds"},
        {R"({"inputs":["a.txt"],"working_directory":false})", "working_directory"},
        {R"({"inputs":["a.txt"],"alpha_stpe":"0.1"})", "Unknown field"},
        {R"({"inputs":["a.txt"],"threads":1,"threads":2})", "Duplicate field"},
        {R"({"inputs":["a.txt"],"github":{"enabled":false,"enabled":true}})", "Duplicate field"},
        {R"({"inputs":["a.txt"],"github":[]})", "JSON object"},
        {R"({"inputs":["a.txt"],"github":{"enabled":true}})", "repository_url"},
        {R"({"inputs":["a.txt"],"github":{"enabled":"true"}})", "enabled"},
        {R"({"inputs":["a.txt"],"github":{"repository_url":"https://token@github.com/a/b"}})", "repository_url"},
        {R"({"inputs":["a.txt"],"github":{"branch":"runs/../main"}})", "branch"},
        {R"({"inputs":["a.txt"],"github":{"branch":"runs/test.lock"}})", "branch"},
        {R"({"inputs":["a.txt"],"github":{"upload_interval_seconds":0}})", "upload_interval_seconds"},
        {R"({"inputs":["a.txt"],"github":{"token":"secret","token_env":"TOKEN"}})", "only one"},
        {R"({"inputs":["a\u0000.txt"]})", "NUL"},
    };
    for (const auto& [contents, expected] : invalid) {
        file = directory.write("invalid.json", contents);
        expect_error([&] { (void)downsizing::read_config(file); }, expected);
    }
    file = directory.write("token.json", R"({"inputs":["g.txt"],"github":{"token":"fake-secret-for-test"}})");
    const auto literal = downsizing::read_config(file);
    check(literal.github.token == "fake-secret-for-test" && literal.github.token_env.empty(), "Literal token option failed");
    for (const std::string_view contents : {
        R"({"inputs":["g.txt"],"github":{"token":"fake-secret-for-test" broken}})",
        R"({"inputs":["g.txt"],"github":{"token":"fake-secret-for-test","token_env":"TOKEN"}})",
        R"({"inputs":["g.txt"],"fake-secret-for-test":true})"}) {
        file = directory.write("secret-invalid.json", contents);
        try {
            (void)downsizing::read_config(file);
            throw std::logic_error("Expected invalid secret fixture to fail");
        } catch (const std::invalid_argument& error) {
            check(std::string_view(error.what()).find("fake-secret-for-test") == std::string_view::npos,
                  "Diagnostics must not expose credentials");
        }
    }
    expect_error([&] { (void)downsizing::read_config(directory.path() / "missing.json"); }, "Cannot open");
}

void test_graph() {
    TemporaryDirectory directory;
    auto file = directory.write("graph.txt", "\n # comment\r\n\t% metadata\n +70 -9\n-9 70\n70 400\n400 -9\n");
    const auto graph = downsizing::read_graph(file);
    check(graph.vertex_count() == 3 && graph.edge_count() == 3, "Graph parsing or edge deduplication failed");
    check(graph.contains(-9) && graph.contains(70) && graph.contains(400) && !graph.contains(0), "Original IDs must be adjacency keys");
    for (const auto& [v, neighbors] : graph.adjacency()) {
        check(neighbors.size() == 2 && std::is_sorted(neighbors.begin(), neighbors.end()), "Adjacency lists must be sorted");
        for (const auto u : neighbors) {
            const auto reverse = graph.neighbors(u);
            check(std::binary_search(reverse.begin(), reverse.end(), v), "Undirected adjacency must be symmetric");
        }
    }
    file = directory.write("boundaries.txt", "-9223372036854775808 9223372036854775807\n");
    const auto extremes = downsizing::read_graph(file);
    check(extremes.contains(std::numeric_limits<downsizing::VertexId>::min()) &&
          extremes.contains(std::numeric_limits<downsizing::VertexId>::max()), "64-bit ID boundaries failed");

    const std::vector<std::pair<std::string, std::string>> invalid{
        {"", "Empty graph"}, {"# only comments\n% metadata\n", "Empty graph"},
        {"1 1\n", ":1:"}, {"1\n", ":1:"}, {"1 2 3\n", ":1:"},
        {"1 two\n", ":1:"}, {"1.0 2\n", ":1:"}, {"1 2 # extra\n", ":1:"},
        {"1 9223372036854775808\n", ":1:"}, {"1 -9223372036854775809\n", ":1:"},
        {"1 +-2\n", ":1:"}, {"1 2\n\n# comment\n3\n", ":4:"},
        {"1 2\n3 4\n", "disconnected"},
    };
    for (const auto& [contents, expected] : invalid) {
        file = directory.write("invalid.txt", contents);
        expect_error([&] { (void)downsizing::read_graph(file); }, expected);
    }
    expect_error([&] { (void)downsizing::read_graph(directory.path()); }, "regular file");
    expect_error([&] { (void)downsizing::read_graph(directory.path() / "missing.txt"); }, "regular file");
    expect_error([&] { (void)graph.neighbors(3); }, "map::at");
    expect_error([] { (void)downsizing::Graph::from_edges({{1, 1}}); }, "Self-loops");

    // Exercise graph storage with a larger sparse input, independently counting degrees.
    std::vector<std::pair<downsizing::VertexId, downsizing::VertexId>> edges;
    constexpr std::size_t n = 20000;
    for (std::size_t i = 1; i < n; ++i) {
        edges.emplace_back(static_cast<downsizing::VertexId>(i - 1), static_cast<downsizing::VertexId>(i));
    }
    const auto path = downsizing::Graph::from_edges(std::move(edges));
    check(path.vertex_count() == n && path.edge_count() == n - 1, "Sparse graph counts failed");
    std::size_t degrees = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& neighbors = path.neighbors(static_cast<downsizing::Vertex>(i));
        check(neighbors.size() == ((i == 0 || i + 1 == n) ? 1U : 2U), "Path degree failed");
        degrees += neighbors.size();
    }
    check(degrees == 2 * path.edge_count(), "Sum of degrees must equal twice the edge count");
}

void test_solution() {
    const auto graph = downsizing::Graph::from_edges({{-8, 42}, {42, 900}, {900, -8}});
    const downsizing::SolutionsBySize solutions{{1, {0, {42}}}, {2, {1, {900, -8}}}, {3, {2, {900, 42, -8}}}};
    std::ostringstream output;
    output << std::hex; // Caller numeric flags must not affect the output format.
    check(downsizing::write_solution_blocks(output, graph, 0.2, solutions) == 5, "Missing duplicate-r output blocks");
    const std::string expected = "0.2 1 0 0\n0.4 1 0 0\n0.6 1 0 0\n0.8 2 1 1\n-8 900\n1 3 2 3\n-8 42\n-8 900\n42 900\n";
    check(output.str() == expected, "Block header, original IDs, induced edges or order failed");

    // Parse the result as a consumer would: m controls the exact number of following edge lines.
    std::istringstream reader(output.str());
    std::size_t blocks = 0;
    double alpha = 0;
    std::size_t r = 0;
    std::size_t k = 0;
    std::size_t m = 0;
    while (reader >> alpha >> r >> k >> m) {
        check(r > 0 && k < r, "Output must include a valid r");
        for (std::size_t i = 0; i < m; ++i) {
            downsizing::VertexId u = 0;
            downsizing::VertexId v = 0;
            check(static_cast<bool>(reader >> u >> v) && u < v, "Cannot consume an output edge");
        }
        ++blocks;
    }
    check(reader.eof() && blocks == 5, "Output blocks are not self-delimiting");

    std::ostringstream partial;
    const downsizing::SolutionsBySize prefix{{1, {0, {42}}}, {3, {2, {-8, 42, 900}}}};
    check(downsizing::write_solution_blocks(partial, graph, 0.2, prefix) == 3, "Incomplete output must stop at first missing r");
    check(partial.str() == "0.2 1 0 0\n0.4 1 0 0\n0.6 1 0 0\n", "Later answers must not bypass an unfinished earlier r");

    std::ostringstream empty;
    check(downsizing::write_solution_blocks(empty, graph, 0.1, {}) == 0 && empty.str().empty(), "No results must produce no blocks");

    const std::vector<downsizing::SolutionsBySize> invalid{
        {{0, {0, {}}}}, {{1, {0, {}}}}, {{2, {1, {0}}}},
        {{2, {2, {-8, 42}}}}, {{2, {1, {-8, -8}}}}, {{2, {1, {-8, 999}}}},
    };
    for (const auto& answers : invalid) {
        std::ostringstream destination;
        expect_error([&] { (void)downsizing::write_solution_blocks(destination, graph, 0.1, answers); }, "Solution");
        check(destination.str().empty(), "Invalid witness must be rejected before writing");
    }
    TemporaryDirectory directory;
    const auto destination = directory.write("answer.txt", "previous result");
    const auto read_saved = [&]() {
        std::ifstream input(destination);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    expect_error([&] { downsizing::save_solution_file(destination, graph, 0.2, invalid.front()); }, "Solution");
    check(read_saved() == "previous result", "Failed write must preserve previous output");
    check(!std::filesystem::exists(destination.string() + ".tmp"), "Failed write must remove its temporary file");
    downsizing::save_solution_file(destination, graph, 0.2, solutions);
    std::ostringstream expected_file;
    static_cast<void>(downsizing::write_solution_blocks(expected_file, graph, 0.2, solutions));
    check(read_saved() == expected_file.str(), "Saved file must contain the complete formatted answer");
    static_cast<void>(directory.write("answer.txt.tmp", "unfinished earlier write"));
    expect_error([&] { downsizing::save_solution_file(destination, graph, 0.2, solutions); }, "already exists");
    check(read_saved() == expected_file.str(), "Temporary-file conflict must not change the saved answer");

    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    expect_error([&] { (void)downsizing::write_solution_blocks(broken, graph, 0.1, solutions); }, "Failed to write");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("Expected one test group");
        }
        const std::string_view group(argv[1]);
        if (group == "alpha") {
            test_alpha();
        } else if (group == "config") {
            test_config();
        } else if (group == "graph") {
            test_graph();
        } else if (group == "solution") {
            test_solution();
        } else {
            throw std::invalid_argument("Unknown test group");
        }
        std::cout << group << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
