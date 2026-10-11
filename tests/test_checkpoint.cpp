#include "graph_downsizing/checkpoint.hpp"
#include "graph_downsizing/parallel_search.hpp"
#include "graph_downsizing/solver.hpp"
#include <barrier>
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
            const auto unfinished = [](std::size_t next) {
                return nlohmann::json{
                    {"status", "IN_PROGRESS"}, {"next_unassigned", "20"},
                    {"unfinished_ranges", {{{"begin", "0"}, {"next", std::to_string(next)}, {"end", "20"}}}}
                };
            };
            for (std::size_t rank = 0; rank < 20; ++rank) {
                const auto key = "search/test/3/2/" + std::to_string(rank);
                state.put(key, unfinished(rank));
                NodeProgress progress{state, key};
                BigInt checked;
                check(!p_downsize(cycle, vertices, 3, 2, nullptr, &checked, &progress), "C6 has no induced triangle");
                check(checked == 20 - rank, "Resume skipped or repeated completed subsets");
                check(state.get(key).at("status") == "FINISHED_NOT_FOUND", "Exhaustion status lost");
            }
            state.put("search/test/3/2/30", unfinished(10));
            state.put("search/test/3/2/31", unfinished(21));
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
        // Out-of-order completion leaves holes; neither completed intervals nor prefixes repeat.
        for (const auto workers : {1U, 4U}) {
            const auto ranges_path = directory / ("ranges-" + std::to_string(workers));
            {
                Checkpoint state(ranges_path, false, std::chrono::seconds(60), metadata);
                state.put("search/test/3/2/0", {
                    {"status", "IN_PROGRESS"}, {"next_unassigned", "16"},
                    {"unfinished_ranges", {
                        {{"begin", "0"}, {"next", "1"}, {"end", "4"}},
                        {{"begin", "8"}, {"next", "10"}, {"end", "12"}}
                    }}
                });
                state.flush();
            }
            Checkpoint state(ranges_path, true, std::chrono::seconds(60), metadata);
            WorkerPool pool(workers);
            SearchStats stats;
            const auto node = std::make_shared<const VertexSet>(vertices);
            check(!parallel_p_downsize(cycle, {node}, 3, 2, pool, &stats, &state, "search/test/3/2"),
                  "Resumed cycle must have no triangle");
            check(stats.subsets_checked == 9, "Completed range or prefix repeated, or an unfinished tail skipped");
            check(state.get("search/test/3/2/0").at("status") == "FINISHED_NOT_FOUND",
                  "Fully exhausted ranges must finish the node");
            check(stats.max_outstanding_ranges <= 2 * workers, "Resume exceeded its submission window");
        }
        {
            Checkpoint state(directory / "range-validation", false, std::chrono::seconds(60), metadata);
            WorkerPool pool(4);
            const auto node = std::make_shared<const VertexSet>(vertices);
            const auto key = "search/test/3/2/0";
            const nlohmann::json range{{"begin", "0"}, {"next", "2"}, {"end", "4"}};
            for (const auto& ranges : {nlohmann::json::array({range, range}),
                                      nlohmann::json::array({{{"begin", "0"}, {"next", "5"}, {"end", "4"}}})}) {
                state.put(key, {{"status", "IN_PROGRESS"}, {"next_unassigned", "4"}, {"unfinished_ranges", ranges}});
                fails([&] { static_cast<void>(parallel_p_downsize(cycle, {node}, 3, 2, pool, nullptr, &state, "search/test/3/2")); });
            }
            // Reserved but never submitted, as after a crash between reservation and submit.
            state.put(key, {{"status", "IN_PROGRESS"}, {"next_unassigned", "20"},
                            {"unfinished_ranges", {{{"begin", "16"}, {"next", "16"}, {"end", "20"}}}}});
            SearchStats stats;
            check(!parallel_p_downsize(cycle, {node}, 3, 2, pool, &stats, &state, "search/test/3/2") &&
                  stats.subsets_checked == 4, "Unsubmitted reserved work must be recovered");
            // Reuse the pool after validation errors, and retain a fully completed frontier.
            state.put(key, {{"status", "IN_PROGRESS"}, {"next_unassigned", "20"},
                            {"unfinished_ranges", nlohmann::json::array()}});
            check(!parallel_p_downsize(cycle, {node}, 3, 2, pool, &stats, &state, "search/test/3/2") &&
                  stats.subsets_checked == 0, "Empty terminal frontier must not restart enumeration");
        }
        {
            const auto legacy = directory / "legacy";
            std::filesystem::copy(path, legacy, std::filesystem::copy_options::recursive);
            for (const auto name : {"tree.json", "progress.json"}) {
                auto file = legacy / "test" / name;
                auto document = read(file);
                document["version"] = 4;
                std::ofstream(file) << document.dump();
            }
            fails([&] { Checkpoint state(legacy, true, std::chrono::seconds(60), metadata); });
        }
        {
            std::vector<std::pair<Vertex, Vertex>> edges;
            VertexSet large;
            for (Vertex i = 0; i < 100; ++i) {
                edges.emplace_back(i, (i + 1) % 100);
                large.push_back(i);
            }
            const auto large_cycle = Graph::from_edges(edges);
            const BigInt total = combination_count(100, 50);
            const BigInt begin = total - 3;
            Checkpoint state(directory / "big-rank", false, std::chrono::seconds(60), metadata);
            state.put("search/test/50/2/0", {{"status", "IN_PROGRESS"}, {"next_unassigned", total.str()},
                {"unfinished_ranges", {{{"begin", begin.str()}, {"next", begin.str()}, {"end", total.str()}}}}});
            WorkerPool pool(2);
            SearchStats stats;
            check(!parallel_p_downsize(large_cycle, {std::make_shared<const VertexSet>(large)}, 50, 2, pool,
                                      &stats, &state, "search/test/50/2") && stats.subsets_checked == 3,
                  "Resume lost a range with ranks beyond 64 bits");
        }
        {
            std::vector<std::pair<Vertex, Vertex>> edges;
            VertexSet labels;
            for (Vertex i = 0; i < 8; ++i) {
                labels.push_back(i);
                for (Vertex j = i + 1; j < 8; ++j) {
                    edges.emplace_back(i, j);
                }
            }
            const auto clique = Graph::from_edges(edges);
            const auto node = std::make_shared<const VertexSet>(labels);
            WorkerPool pool(4);
            Checkpoint state(directory / "range-success", false, std::chrono::seconds(60), metadata);
            const auto answer = parallel_p_downsize(clique, {node}, 4, 3, pool, nullptr, &state, "search/test/4/3");
            check(answer && answer->size() == 4, "Clique range must find a witness");
            const auto saved = read(state.path() / "test/progress.json").at("nodes").at("4/3/0");
            check(saved.at("status") == "FINISHED_FOUND" && saved.at("vertices").get<VertexSet>() == *answer,
                  "Late range updates overwrote the durable witness");
            SearchStats stats;
            check(parallel_p_downsize(clique, {node}, 4, 3, pool, &stats, &state, "search/test/4/3") == answer &&
                  stats.ranges_started == 0, "Saved success must return before submitting jobs");
            Checkpoint broken(directory / "range-disk-error", false, std::chrono::seconds(60), metadata);
            std::filesystem::create_directories(broken.path() / "test/progress.json.tmp/blocked");
            fails([&] { static_cast<void>(parallel_p_downsize(clique, {node}, 4, 3, pool, nullptr, &broken, "search/test/4/3")); });
            check(parallel_p_downsize(clique, {node}, 4, 3, pool).has_value(),
                  "Worker error must drain outstanding ranges and leave the pool reusable");
        }
        {
            Checkpoint state(directory / "live-cursors", false, std::chrono::seconds(60), metadata);
            const auto key = "search/test/live/0";
            auto first = std::make_shared<RangeCursor>(BigInt(0));
            auto second = std::make_shared<RangeCursor>(BigInt(20000));
            state.put(key, {{"status", "IN_PROGRESS"}, {"next_unassigned", "40000"},
                {"unfinished_ranges", {
                    {{"begin", "0"}, {"next", "0"}, {"end", "20000"}},
                    {{"begin", "20000"}, {"next", "20000"}, {"end", "40000"}}
                }}}, true, {{"0", first}, {"20000", second}});
            // No put/dirty update occurs in workers; flush must discover their progress.
            std::barrier gate(3);
            std::jthread left([&] {
                gate.arrive_and_wait();
                for (int i = 0; i < 10000; ++i) {
                    first->advance();
                }
            });
            std::jthread right([&] {
                gate.arrive_and_wait();
                for (int i = 0; i < 10000; ++i) {
                    second->advance();
                }
            });
            gate.arrive_and_wait();
            BigInt previous_first = 0;
            BigInt previous_second = 20000;
            for (int sample = 0; sample < 10; ++sample) {
                state.flush();
                const auto ranges = read(state.path() / "test/progress.json").at("nodes").at("live/0").at("unfinished_ranges");
                const BigInt current_first(ranges.at(0).at("next").get<std::string>());
                const BigInt current_second(ranges.at(1).at("next").get<std::string>());
                check(current_first >= previous_first && current_first <= 10000 &&
                      current_second >= previous_second && current_second <= 30000,
                      "Concurrent snapshot lost monotonicity or skipped unchecked subsets");
                previous_first = current_first;
                previous_second = current_second;
            }
            left.join();
            right.join();
            state.flush();
            const auto ranges = state.get(key).at("unfinished_ranges");
            check(ranges.at(0).at("next") == "10000" && ranges.at(1).at("next") == "30000",
                  "Flush did not collect the final live cursor positions");
            state.put(key, {{"status", "FINISHED_FOUND"}, {"vertices", {0,1,2}}}, true);
            first->advance();
            second->advance();
            state.flush();
            check(state.get(key).at("status") == "FINISHED_FOUND" && !state.get(key).contains("unfinished_ranges"),
                  "Detached live cursors overwrote a saved witness");
            const std::weak_ptr<RangeCursor> weak = first;
            first.reset();
            check(weak.expired(), "Finished node retained a live cursor registration");
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
