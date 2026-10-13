#include "graph_downsizing/sizes.hpp"
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

void test_sizes() {
    using downsizing::make_sizes;
    check(make_sizes(0.3, 9) == std::vector<std::size_t>({2, 5, 8, 9}), "Uneven grid");
    check(make_sizes(0.1, 3) == std::vector<std::size_t>({1, 2, 3}), "Duplicate sizes must collapse");
    check(make_sizes(1, 5) == std::vector<std::size_t>({5}), "Full graph only");
    check(make_sizes(1e-15, 100).size() == 100, "Tiny grid must skip repeated sizes");
    check(make_sizes(0.29, 100).front() == 29, "Floating boundary");
    check(make_sizes(0.1, 10, {{7, 3, 7}, {}}) == std::vector<std::size_t>({3, 7}), "Explicit r");
    check(make_sizes(0.1, 10, {{}, {0.31, 0.7, 0.39}}) == std::vector<std::size_t>({3, 7}), "Equivalent alpha sizes");
    for (std::size_t numerator = 1; numerator <= 100; ++numerator) {
        for (std::size_t n = 1; n <= 80; ++n) {
            std::vector<std::size_t> expected;
            for (std::size_t fraction = numerator; fraction < 100; fraction += numerator) {
                expected.push_back(std::max(std::size_t{1}, fraction * n / 100));
            }
            expected.push_back(n);
            expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
            check(make_sizes(static_cast<double>(numerator) / 100, n) == expected, "Grid differs from integer reference");
        }
    }
    for (const double invalid : {0.0, -0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(), 1e-30}) {
        expect_error([&] { (void)make_sizes(invalid, 3); }, "alpha_step");
    }
    expect_error([] { (void)make_sizes(0.1, 0); }, "nonempty graph");
}

void test_config() {
    TemporaryDirectory directory;
    auto file = directory.write("config.json", R"({"inputs":["data/g.txt"]})");
    const auto config = downsizing::read_config(file);
    check(config.range_size == 131072, "Default range size failed");
    check(config.log_interval.count() == 10, "Default log interval failed");
    check(config.alpha_step == 0.1 && config.threads >= 1, "Default step or threads failed");
    check(config.checkpoint_interval.count() == 60 && config.github.upload_interval.count() == 300, "Default intervals failed");
    check(config.inputs[0] == directory.path() / "data/g.txt", "Input paths must be relative to the config file");
    check(config.working_directory == directory.path() / "runs", "Working directory default failed");
    check(!config.github.enabled && config.github.branch == "runs", "GitHub defaults failed");
    check(downsizing::solution_path(config, 0) == directory.path() / "runs/g/g_sol.txt", "Result path failed");

    file = directory.write("configured.json", R"({
        "inputs": ["data/g.txt", "other/h.txt"], "alpha_step": 0.3,
        "threads": 32, "range_size": 4096, "working_directory": "work/../answers",
        "checkpoint_interval_seconds": 7, "log_interval_seconds": 2,
        "github": {"enabled":true, "repository_url":"https://github.com/DenVot/graph_downsizing_enumeration.git",
                   "branch":"runs/test", "token_env":"CUSTOM_TOKEN", "upload_interval_seconds":19}
    })");
    const auto configured = downsizing::read_config(file);
    check(configured.range_size == 4096, "Configured range size failed");
    check(configured.log_interval.count() == 2, "Configured log interval failed");
    check(configured.alpha_step == 0.3 && configured.threads == 32, "Configured values failed");
    check(configured.working_directory == directory.path() / "answers", "Path normalization failed");
    check(configured.github.enabled && configured.github.token.empty() && configured.github.token_env == "CUSTOM_TOKEN",
          "Credential configuration failed");
    check(configured.checkpoint_interval.count() == 7 && configured.github.upload_interval.count() == 19,
          "Configured intervals failed");
    check(downsizing::solution_path(configured, 0) != downsizing::solution_path(configured, 1),
          "Different graph filenames must have separate output directories");

    (void)directory.write("graphs/z.txt", "1 2\n");
    (void)directory.write("graphs/a ~.txt", "1 2\n");
    (void)directory.write("graphs/ignore.csv", "not a graph");
    (void)directory.write("graphs/nested/skip.txt", "1 2\n");
    std::filesystem::create_directories(directory.path() / "graphs/folder.txt");
    file = directory.write("folder.json", R"({"input_directory":"graphs"})");
    const auto scanned = downsizing::read_config(file);
    check(scanned.inputs == std::vector<std::filesystem::path>{
              directory.path() / "graphs/a ~.txt", directory.path() / "graphs/z.txt"},
          "Directory inputs must be sorted, nonrecursive, regular .txt files");
    std::filesystem::create_directories(directory.path() / "empty");

    const std::vector<std::pair<std::string, std::string>> invalid{
        {R"({"input_directory":"missing"})", "existing directory"},
        {R"({"input_directory":"empty"})", "no .txt files"},
        {R"({"input_directory":12})", "nonempty string"},
        {R"({"input_directory":"graphs","inputs":["g.txt"]})", "exactly one"},
        {R"({"inputs":["one/g.txt","two/g.txt"]})", "filenames must be unique"},
        {R"({"inputs":["one/g.txt","two/g.dat"]})", "filenames must be unique"},
        {"{", "valid JSON"}, {"[]", "JSON object"}, {"{}", "inputs"},
        {R"({"inputs":[]})", "inputs"}, {R"({"inputs":[12]})", "input path"},
        {R"({"inputs":[""]})", "input path"},
        {R"({"inputs":["a.txt","./a.txt"]})", "duplicate paths"},
        {R"({"inputs":["a.txt"],"alpha_step":"0.1"})", "alpha_step"},
        {R"({"inputs":["a.txt"],"alpha_step":0})", "alpha_step"},
        {R"({"inputs":["a.txt"],"alpha_step":true})", "alpha_step"},
        {R"({"inputs":["a.txt"],"combination_chunk_size":256})", "Unknown field"},
        {R"({"inputs":["a.txt"],"log_interval_seconds":0})", "log_interval_seconds"},
        {R"({"inputs":["a.txt"],"range_size":0})", "range_size"},
        {R"({"inputs":["a.txt"],"range_size":-1})", "range_size"},
        {R"({"inputs":["a.txt"],"range_size":1.5})", "range_size"},
        {R"({"inputs":["a.txt"],"range_size":true})", "range_size"},
        {R"({"inputs":["a.txt"],"range_size":"4096"})", "range_size"},
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
    for (const auto header : {"r: 3, 1, 3", "alpha: 1, 0.2, 0.3, 0.2"}) {
        const auto input = directory.write("selected.txt", std::string(header) + "\r\n1 2\n2 3\n3 1\n");
        const auto loaded = downsizing::read_graph_input(input);
        const auto sizes = downsizing::make_sizes(0.1, loaded.graph.vertex_count(), loaded.requested);
        check(sizes == std::vector<std::size_t>({1, 3}), "Header must yield unique sorted sizes");
    }
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
        {"r:\n1 2\n", "Empty value"}, {"r: 1,\n1 2\n", "Empty value"},
        {"r: 0\n1 2\n", "positive integer"}, {"r: -1\n1 2\n", "positive integer"},
        {"r: 1.5\n1 2\n", "positive integer"}, {"r: 3\n1 2\n", "vertex count"},
        {"alpha: nan\n1 2\n", "(0, 1]"}, {"alpha: inf\n1 2\n", "(0, 1]"},
        {"alpha: 0\n1 2\n", "(0, 1]"}, {"alpha: 1.1\n1 2\n", "(0, 1]"},
        {"alpha: 0.1,,0.2\n1 2\n", "Empty value"},
        {"r: 1\nalpha: 0.5\n1 2\n", ":2:"},
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
    check(downsizing::write_solution_blocks(output, graph, {1, 2, 3}, solutions) == 3, "One block per r");
    const std::string expected = "1 0 0\n2 1 1\n-8 900\n3 2 3\n-8 42\n-8 900\n42 900\n";
    check(output.str() == expected, "Block header, original IDs, induced edges or order failed");

    // Parse the result as a consumer would: m controls the exact number of following edge lines.
    std::istringstream reader(output.str());
    std::size_t blocks = 0;
    std::size_t r = 0;
    std::size_t k = 0;
    std::size_t m = 0;
    while (reader >> r >> k >> m) {
        check(r > 0 && k < r, "Output must include a valid r");
        for (std::size_t i = 0; i < m; ++i) {
            downsizing::VertexId u = 0;
            downsizing::VertexId v = 0;
            check(static_cast<bool>(reader >> u >> v) && u < v, "Cannot consume an output edge");
        }
        ++blocks;
    }
    check(reader.eof() && blocks == 3, "Output blocks are not self-delimiting");

    std::ostringstream partial;
    const downsizing::SolutionsBySize prefix{{1, {0, {42}}}, {3, {2, {-8, 42, 900}}}};
    check(downsizing::write_solution_blocks(partial, graph, {1, 2, 3}, prefix) == 1, "Incomplete output must stop at first missing r");
    check(partial.str() == "1 0 0\n", "Later answers must not bypass an unfinished earlier r");

    std::ostringstream empty;
    check(downsizing::write_solution_blocks(empty, graph, {1, 2, 3}, {}) == 0 && empty.str().empty(), "No results must produce no blocks");

    const std::vector<downsizing::SolutionsBySize> invalid{
        {{0, {0, {}}}}, {{1, {0, {}}}}, {{2, {1, {0}}}},
        {{2, {2, {-8, 42}}}}, {{2, {1, {-8, -8}}}}, {{2, {1, {-8, 999}}}},
    };
    for (const auto& answers : invalid) {
        std::ostringstream destination;
        expect_error([&] { (void)downsizing::write_solution_blocks(destination, graph, {1, 2, 3}, answers); }, "Solution");
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
    expect_error([&] { downsizing::save_solution_file(destination, graph, {1, 2, 3}, invalid.front()); }, "Solution");
    check(read_saved() == "previous result", "Failed write must preserve previous output");
    check(!std::filesystem::exists(destination.string() + ".tmp"), "Failed write must remove its temporary file");
    downsizing::save_solution_file(destination, graph, {1, 2, 3}, solutions);
    std::ostringstream expected_file;
    static_cast<void>(downsizing::write_solution_blocks(expected_file, graph, {1, 2, 3}, solutions));
    check(read_saved() == expected_file.str(), "Saved file must contain the complete formatted answer");
    static_cast<void>(directory.write("answer.txt.tmp", "unfinished earlier write"));
    expect_error([&] { downsizing::save_solution_file(destination, graph, {1, 2, 3}, solutions); }, "already exists");
    check(read_saved() == expected_file.str(), "Temporary-file conflict must not change the saved answer");

    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    expect_error([&] { (void)downsizing::write_solution_blocks(broken, graph, {1, 2, 3}, solutions); }, "Failed to write");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("Expected one test group");
        }
        const std::string_view group(argv[1]);
        if (group == "sizes") {
            test_sizes();
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
