#include "graph_downsizing/checkpoint.hpp"
#include "graph_downsizing/parallel_search.hpp"
#include "graph_downsizing/solver.hpp"
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace downsizing;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void fails(F function) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    check(caught, "Expected a checkpoint failure");
}
const std::vector<nlohmann::json> metadata{{{"input","test.txt"},{"alpha_step",0.1}}};
nlohmann::json read(const std::filesystem::path& path) {
    std::ifstream stream(path); return nlohmann::json::parse(stream);
}
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("downsizing-json-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    try {
        install_stop_handlers();
        const auto path = directory / "state";
        const auto cycle = Graph::from_edges({{0,1},{1,2},{2,3},{3,4},{4,5},{5,0}});
        const VertexSet vertices{0,1,2,3,4,5};
        {
            Checkpoint state(path, false, std::chrono::seconds(60), metadata);
            fails([&] { Checkpoint locked(path, true, std::chrono::seconds(60), metadata); });
            const auto alias = directory / "alias";
            std::filesystem::create_directory_symlink(path, alias);
            fails([&] { Checkpoint locked(alias, true, std::chrono::seconds(60), metadata); });
            std::vector<std::size_t> cursor{0,1,2};
            std::size_t rank = 0;
            do {
                const auto key = "search/test/3/2/" + std::to_string(rank);
                state.put(key, {{"status", "IN_PROGRESS"}, {"next", cursor}});
                NodeProgress progress{state, key};
                BigInt checked;
                check(!p_downsize(cycle, vertices, 3, 2, nullptr, &checked, &progress), "C6 has no induced triangle");
                check(checked == 20 - rank, "Resume skipped or repeated completed subsets");
                check(state.get(key).at("status") == "FINISHED_NOT_FOUND", "Exhaustion status lost");
                ++rank;
            } while (next_combination(cursor, 6));
            state.put("search/test/3/2/30", {{"status", "IN_PROGRESS"}, {"next", {1,2,3}}});
            state.put("search/test/3/2/31", {{"status", "IN_PROGRESS"}, {"next", {1,1,3}}});
            NodeProgress invalid{state, "search/test/3/2/31"};
            fails([&] { static_cast<void>(p_downsize(cycle, vertices, 3, 2, nullptr, nullptr, &invalid)); });
            state.flush();
        }
        {
            const auto portable = directory / "portable";
            std::filesystem::copy(path, portable, std::filesystem::copy_options::recursive);
            // A crashed temporary write must not replace the last complete document.
            std::ofstream(portable / "test/progress.json.tmp") << "{unfinished";
            Checkpoint state(portable, true, std::chrono::seconds(1), metadata);
            NodeProgress progress{state, "search/test/3/2/30"};
            BigInt checked;
            check(!p_downsize(cycle, vertices, 3, 2, nullptr, &checked, &progress) && checked == 10,
                  "Copied JSON checkpoint did not preserve cursor");
            WorkerPool pool(4);
            auto node = std::make_shared<const VertexSet>(vertices);
            SearchStats stats;
            check(!parallel_p_downsize(cycle, {node}, 3, 2, pool, &stats, &state, "search/test/3/2") && stats.nodes_started == 0,
                  "Finished node was executed again");
            state.flush();
            check(read(portable / "test/progress.json").at("nodes").at("3/2/30").at("status") == "FINISHED_NOT_FOUND", "Final status not saved");
        }
        const auto graph = Graph::from_edges({{10,20},{20,30},{30,40},{40,50},{50,60},{60,10},
                                             {70,80},{80,90},{90,70},{10,70}});
        const auto solver_path = directory / "solver";
        {
            Checkpoint state(solver_path, false, std::chrono::seconds(60), metadata);
            state.put_many({
                {"tree/test", {{"minimum_size",1}, {"next_parent",1}, {"node_count",3}}},
                {"tree/test/set/0", {10,20,30,40,50,60,70,80,90}},
                {"tree/test/set/1", {10,20,30,40,50,60}}, {"tree/test/set/2", {70,80,90}},
                {"tree/test/node/0", {{"level",1},{"parent",0},{"set",0}}},
                {"tree/test/node/1", {{"level",2},{"parent",0},{"set",1}}},
                {"tree/test/node/2", {{"level",2},{"parent",0},{"set",2}}}});
            state.flush();
        }
        {
            Checkpoint state(solver_path, true, std::chrono::seconds(60), metadata);
            WorkerPool pool(1);
            ExactSolver solver(graph, 1, pool, &state, "test");
            check(solver.tree().nodes.size() == 3 && state.get("tree/test").at("next_parent") == 3, "Partial tree resume failed");
            check(solver.solve(3).connectivity == 2 && solver.solve(3).vertices == VertexSet({70,80,90}), "Wrong resumed optimum");
            check(solver.solve(4).connectivity == 1, "Wrong resumed fallback");
            state.flush();
        }
        const auto tree_time = std::filesystem::last_write_time(solver_path / "test/tree.json");
        {
            Checkpoint state(solver_path, true, std::chrono::seconds(5), metadata);
            WorkerPool pool(4);
            ExactSolver solver(graph, 1, pool, &state, "test");
            check(solver.solutions().empty(), "Results must not be stored separately");
            check(solver.solve(3).vertices == VertexSet({70,80,90}) && solver.solve(4).connectivity == 1, "Node states did not recover answers");
            state.flush();
            check(std::filesystem::last_write_time(solver_path / "test/tree.json") == tree_time, "Completed tree was rewritten");
        }
        // A crash between tree and empty-progress renames must be recoverable.
        {
            const auto split = directory / "split";
            Checkpoint state(split, false, std::chrono::seconds(60), metadata);
            const auto old_progress = read(split / "test/progress.json");
            state.put("tree/test", {{"test", 42}}); state.flush();
            std::ofstream(split / "test/progress.json") << old_progress.dump();
        }
        { Checkpoint restored(directory / "split", true, std::chrono::seconds(60), metadata); }
        // Progress with different calculation parameters is rejected.
        {
            const auto mixed = directory / "mixed";
            std::filesystem::copy(solver_path, mixed, std::filesystem::copy_options::recursive);
            auto progress = read(mixed / "test/progress.json");
            progress["metadata"]["alpha_step"] = 0.5;
            std::ofstream(mixed / "test/progress.json") << progress.dump();
            fails([&] { Checkpoint state(mixed, true, std::chrono::seconds(60), metadata); });
        }
        {
            Checkpoint state(directory / "disk", false, std::chrono::seconds(60), metadata);
            state.put("search/test/3/2/0", {{"status","NOT_PROCESSED"}});
            // A nonempty directory cannot be removed as a stale temporary file.
            std::filesystem::create_directories(state.path() / "test/progress.json.tmp/blocked");
            fails([&] { state.flush(); }); fails([&] { state.check(); });
        }
        {
            Checkpoint state(directory / "signal", false, std::chrono::seconds(60), metadata);
            std::raise(SIGINT);
            bool interrupted = false;
            try { state.check(); } catch (const Interrupted&) { interrupted = true; }
            check(interrupted, "SIGINT was not handled");
            state.flush(); install_stop_handlers();
        }
        std::filesystem::remove_all(directory);
        std::cout << "JSON cursors, node statuses, partial trees, copies, locks, disk errors: passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
