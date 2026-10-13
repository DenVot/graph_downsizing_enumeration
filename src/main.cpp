#include "graph_downsizing/alpha.hpp"
#include "graph_downsizing/checkpoint.hpp"
#include "graph_downsizing/logging.hpp"
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
#include <sstream>
#include <string_view>

namespace {

void help() {
    std::cout << "graph_downsizing " GRAPH_DOWNSIZING_VERSION " (stage 5)\n"
                 "Usage:\n"
                 "  graph_downsizing validate --config FILE\n"
                 "  graph_downsizing tree --config FILE\n"
                 "  graph_downsizing run --config FILE\n"
                 "  graph_downsizing resume --checkpoint DIRECTORY --config FILE\n"
                 "  graph_downsizing --help\n"
                 "  graph_downsizing --version\n\n"
                 "validate reads inputs and previews up to 20 alpha/r tasks per graph.\n"
                 "tree builds the exact hierarchy and previews up to 20 nodes per graph.\n"
                 "run solves alpha/r tasks using a shared worker pool and writes solution files.\n"
                 "resume continues from a JSON checkpoint; active nodes resume at their saved subset.\n";
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
              << " range_size=" << config.range_size
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

void run(const downsizing::Config& config, const std::filesystem::path& resume,
         const std::filesystem::path& config_path) {
    const bool restoring = !resume.empty();
    const auto checkpoint_path = restoring ? resume : config.working_directory;
    std::vector<std::filesystem::path> reserved{checkpoint_path / ".checkpoint.lock", checkpoint_path / "run.log"};
    for (std::size_t i = 0; i < config.inputs.size(); ++i) {
        const auto folder = checkpoint_path / config.inputs[i].stem();
        for (const auto name : {"tree.json", "tree.json.tmp", "progress.json", "progress.json.tmp"}) {
            reserved.push_back(folder / name);
        }
    }
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
    for (const auto& file : reserved) {
        const auto canonical = std::filesystem::weakly_canonical(file);
        if (!destinations.insert(canonical).second || canonical == std::filesystem::weakly_canonical(config_path)) {
            throw std::invalid_argument("Checkpoint paths must not overwrite configuration or solution files");
        }
        for (const auto& input : config.inputs) {
            if (canonical == std::filesystem::weakly_canonical(input)) {
                throw std::invalid_argument("Checkpoint must not overwrite an input graph");
            }
        }
    }
    if (destinations.contains(std::filesystem::weakly_canonical(config_path))) {
        throw std::invalid_argument("Output must not overwrite configuration");
    }
    downsizing::install_stop_handlers();
    // Input graphs remain external and must be unchanged when resuming.
    std::vector<nlohmann::json> metadata;
    for (const auto& input : config.inputs) {
        metadata.push_back({
            {"algorithm", "node-cursor-v1"},
            {"input", input.filename().string()},
            {"alpha_step", config.alpha_step}
        });
    }
    downsizing::Checkpoint checkpoint(checkpoint_path, restoring, config.checkpoint_interval, metadata);
    downsizing::WorkerPool pool(config.threads);
    downsizing::RunLogger logger(checkpoint.path() / "run.log", pool, config.log_interval);
    try {
        logger.info(std::string("event=run_started mode=") + (restoring ? "resume" : "run") +
                    " checkpoint=" + checkpoint.path().string() + " workers=" + std::to_string(pool.size()) +
                    " range_size=" + std::to_string(config.range_size) +
                    " log_interval_s=" + std::to_string(config.log_interval.count()) +
                    " checkpoint_interval_s=" + std::to_string(config.checkpoint_interval.count()));
        for (std::size_t i = 0; i < config.inputs.size(); ++i) {
            const auto graph_id = config.inputs[i].stem().string();
            logger.context(graph_id, "read_graph");
            const auto graph = downsizing::read_graph(config.inputs[i]);
            logger.info("event=graph_loaded vertices=" + std::to_string(graph.vertex_count()) +
                        " edges=" + std::to_string(graph.edge_count()));
            downsizing::AlphaSchedule schedule(config.alpha_step, graph.vertex_count());
            auto task = schedule.next();
            logger.context(graph_id, "build_tree");
            logger.info("event=tree_started minimum_r=" + std::to_string(task->r));
            downsizing::ExactSolver solver(graph, task->r, pool, &checkpoint, graph_id, config.range_size);
            logger.info("event=tree_ready nodes=" + std::to_string(solver.tree().nodes.size()) +
                        " maximum_k=" + std::to_string(solver.tree().levels.size() - 1));
            const auto destination = downsizing::solution_path(config, i);
            if (restoring) {
                // A killed process may have left an unfinished atomic output replacement.
                auto temporary = destination;
                temporary += ".tmp";
                std::filesystem::remove(temporary);
            }
            do {
                const bool reused = solver.solutions().contains(task->r);
                logger.search(graph_id, task->alpha, task->r);
                std::ostringstream result;
                result << std::setprecision(15) << "event=solution alpha=" << task->alpha << " r=" << task->r;
                const auto& solution = solver.solve(task->r, [&](const downsizing::SolveProgress& progress) {
                    logger.search_progress(progress);
                });
                if (!reused) {
                    downsizing::save_solution_file(destination, graph, config.alpha_step, solver.solutions());
                }
                result << " k=" << solution.connectivity << (reused ? " reused" : " computed");
                if (task->r == 1) {
                    result << " vertex=" << solution.vertices.front();
                }
                logger.info(result.str());
            } while ((task = schedule.next()));
            logger.info("event=graph_finished output=" + destination.string() +
                        " unique_sizes=" + std::to_string(solver.solutions().size()));
        }
        logger.context("", "save_checkpoint");
        checkpoint.flush();
        logger.info("event=run_finished checkpoint_saved=true");
    } catch (const downsizing::Interrupted&) {
        checkpoint.flush();
        logger.info("event=run_interrupted checkpoint_saved=true");
        throw;
    } catch (const std::exception& error) {
        try {
            logger.error(error.what());
        } catch (...) {
            // Logging failure must not prevent saving computation state.
        }
        checkpoint.flush();
        throw;
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
    try {
        const auto config = downsizing::read_config(arguments.config);
        if (arguments.command == "run" || arguments.command == "resume") {
            run(config, arguments.checkpoint, arguments.config);
        } else if (arguments.command == "tree") {
            inspect_tree(config);
        } else {
            validate(config);
        }
        return 0;
    } catch (const downsizing::Interrupted& error) {
        std::cerr << error.what() << '\n';
        return 130;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
