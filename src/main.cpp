#include "graph_downsizing/alpha.hpp"
#include "graph_downsizing/config.hpp"
#include "graph_downsizing/graph.hpp"
#include "graph_downsizing/tree.hpp"
#include "graph_downsizing/solver.hpp"

#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void help() {
    std::cout << "graph_downsizing " GRAPH_DOWNSIZING_VERSION " (stage 4)\n"
                 "Usage:\n"
                 "  graph_downsizing validate --config FILE\n"
                 "  graph_downsizing tree --config FILE\n"
                 "  graph_downsizing run --config FILE\n"
                 "  graph_downsizing resume --checkpoint FILE --config FILE\n"
                 "  graph_downsizing --help\n"
                 "  graph_downsizing --version\n\n"
                 "validate reads inputs and previews up to 20 alpha/r tasks per graph.\n"
                 "tree builds the exact hierarchy and previews up to 20 nodes per graph.\n"
                 "run solves alpha/r tasks using a shared worker pool and writes solution files.\n"
                 "resume is reserved for the checkpoint stage.\n";
}

struct Arguments {
    std::string command;
    std::filesystem::path config;
    std::filesystem::path checkpoint;
};

Arguments parse_arguments(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument("Expected validate, tree, run, or resume; use --help");
    }
    Arguments arguments;
    arguments.command = argv[1];
    if (arguments.command != "validate" && arguments.command != "tree" &&
        arguments.command != "run" && arguments.command != "resume") {
        throw std::invalid_argument("Unknown command; use --help");
    }
    for (int i = 2; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option != "--config" && option != "--checkpoint") {
            throw std::invalid_argument("Unknown option; use --help");
        }
        if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--") || std::string_view(argv[i + 1]).empty()) {
            throw std::invalid_argument("An option requires a file path");
        }
        auto& destination = option == "--config" ? arguments.config : arguments.checkpoint;
        if (!destination.empty()) {
            throw std::invalid_argument("Duplicate option; use --help");
        }
        destination = argv[++i];
    }
    if (arguments.config.empty()) {
        throw std::invalid_argument("--config is required");
    }
    if (arguments.command == "resume" && arguments.checkpoint.empty()) {
        throw std::invalid_argument("resume requires --checkpoint");
    }
    if (arguments.command != "resume" && !arguments.checkpoint.empty()) {
        throw std::invalid_argument("--checkpoint is supported only by resume");
    }
    return arguments;
}

void validate(const downsizing::Config& config) {
    std::cout << "Validation only: no enumeration, output files, or GitHub uploads.\n"
              << std::setprecision(15) << "alpha_step=" << config.alpha_step << " threads=" << config.threads
              << " checkpoint_interval_seconds=" << config.checkpoint_interval.count()
              << " github=" << (config.github.enabled ? "configured" : "disabled") << '\n';
    for (std::size_t i = 0; i < config.inputs.size(); ++i) {
        const auto graph = downsizing::read_graph(config.inputs[i]);
        downsizing::AlphaSchedule schedule(config.alpha_step, graph.vertex_count());
        std::cout << "graph=" << config.inputs[i].string() << " vertices=" << graph.vertex_count()
                  << " edges=" << graph.edge_count() << " alpha_tasks=" << schedule.task_count()
                  << " output=" << downsizing::solution_path(config, i).string() << '\n';
        std::size_t previous_r = 0;
        for (std::size_t preview = 0; preview < 20; ++preview) {
            const auto task = schedule.next();
            if (!task) {
                break;
            }
            std::cout << "  alpha=" << task->alpha << " r=" << task->r;
            if (task->r == previous_r) {
                std::cout << " (reuse previous r)";
            }
            std::cout << '\n';
            previous_r = task->r;
        }
        if (schedule.task_count() > 20) {
            std::cout << "  ... preview limited to 20 tasks; alpha=1 is always included.\n";
        }
    }
}

void inspect_tree(const downsizing::Config& config) {
    for (const auto& input : config.inputs) {
        const auto graph = downsizing::read_graph(input);
        downsizing::AlphaSchedule schedule(config.alpha_step, graph.vertex_count());
        const auto minimum_size = schedule.next()->r;
        std::cout << "Building tree: graph=" << input.string() << " minimum_r=" << minimum_size
                  << std::endl;
        const auto tree = downsizing::build_tree(graph, minimum_size);
        std::cout << "nodes=" << tree.nodes.size() << " maximum_k=" << tree.levels.size() - 1 << '\n';
        for (std::size_t id = 0; id < tree.nodes.size() && id < 20; ++id) {
            const auto& node = tree.nodes[id];
            std::cout << "  node=" << id << " parent=" << node.parent << " k=" << node.level
                      << " size=" << node.vertices->size() << " vertices=";
            for (std::size_t i = 0; i < node.vertices->size() && i < 20; ++i) {
                std::cout << (i == 0 ? "" : ",") << (*node.vertices)[i];
            }
            if (node.vertices->size() > 20) {
                std::cout << ",...";
            }
            std::cout << '\n';
        }
        if (tree.nodes.size() > 20) {
            std::cout << "  ... preview limited to 20 nodes.\n";
        }
    }
}

void run(const downsizing::Config& config) {
    if (config.github.enabled) throw std::invalid_argument("GitHub upload is not implemented yet; set github.enabled=false");
    // Check every destination before writing, including collisions with a later input.
    std::set<std::filesystem::path> destinations;
    for (std::size_t i = 0; i < config.inputs.size(); ++i) {
        const auto destination = downsizing::solution_path(config, i);
        auto temporary = destination;
        temporary += ".tmp";
        for (const auto& candidate : {destination, temporary}) {
            if (!destinations.insert(std::filesystem::weakly_canonical(candidate)).second) {
                throw std::invalid_argument("Solution output paths must be distinct");
            }
        }
        for (const auto& input : config.inputs) {
            for (const auto& candidate : {destination, temporary}) {
                if (std::filesystem::weakly_canonical(candidate) == std::filesystem::weakly_canonical(input)) {
                    throw std::invalid_argument("Solution output must not overwrite an input graph");
                }
            }
        }
    }
    downsizing::WorkerPool pool(config.threads);
    std::cout << "Parallel solver: workers=" << pool.size()
              << "; checkpoints/resume are not implemented yet.\n";
    for (std::size_t i = 0; i < config.inputs.size(); ++i) {
        const auto graph = downsizing::read_graph(config.inputs[i]);
        downsizing::AlphaSchedule schedule(config.alpha_step, graph.vertex_count());
        auto task = schedule.next();
        std::cout << "Building tree: graph=" << config.inputs[i].string() << " minimum_r=" << task->r << std::endl;
        downsizing::ExactSolver solver(graph, task->r, pool, config.combination_chunk_size);
        std::cout << "nodes=" << solver.tree().nodes.size() << " maximum_k=" << solver.tree().levels.size() - 1 << std::endl;
        const auto destination = downsizing::solution_path(config, i);
        do {
            const bool reused = solver.solutions().contains(task->r);
            std::cout << std::setprecision(15) << "alpha=" << task->alpha << " r=" << task->r << std::flush;
            const auto& solution = solver.solve(task->r);
            if (!reused) downsizing::save_solution_file(destination, graph, config.alpha_step, solver.solutions());
            std::cout << " k=" << solution.connectivity << (reused ? " reused" : " computed");
            if (task->r == 1) std::cout << " vertex=" << solution.vertices.front();
            std::cout << std::endl;
        } while ((task = schedule.next()));
        std::cout << "output=" << destination.string() << " unique_sizes=" << solver.solutions().size() << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << GRAPH_DOWNSIZING_VERSION << '\n';
        return 0;
    }
    Arguments arguments;
    try {
        arguments = parse_arguments(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Usage error: " << error.what() << '\n';
        return 2;
    }
    if (arguments.command == "resume") {
        std::cerr << "resume is not implemented in stage 4.\n";
        return 3;
    }
    try {
        const auto config = downsizing::read_config(arguments.config);
        if (arguments.command == "run") {
            run(config);
        } else if (arguments.command == "tree") {
            inspect_tree(config);
        } else {
            validate(config);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
