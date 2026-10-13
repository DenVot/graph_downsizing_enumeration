"""Real-process recovery with fixed cycle/clique answers and two JSON files per graph."""
import json
import pathlib
import math
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

exe = str(pathlib.Path(sys.argv[1]).resolve())
children = []


def document(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def completed_subsets(state):
    if state.get("status") != "IN_PROGRESS":
        return 0
    return int(state["next_unassigned"]) - sum(
        int(item["end"]) - int(item["next"]) for item in state["unfinished_ranges"])


def wait_for(process, function, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = function()
        if result:
            return result
        if process.poll() is not None:
            raise AssertionError(f"Process ended too early: {process.returncode}: {captured_error(process)}")
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for a durable checkpoint boundary")


def captured_error(process):
    process.communicate()
    process.log_output.seek(0)
    return process.log_output.read()


def start(config, checkpoint=None):
    command = [exe, "resume" if checkpoint else "run", "--config", str(config)]
    if checkpoint:
        command += ["--checkpoint", str(checkpoint)]
    log_output = tempfile.TemporaryFile(mode="w+t")
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=log_output, text=True)
    process.log_output = log_output
    children.append(process)
    return process


def finish(process, code=0, timeout=120):
    process.communicate(timeout=timeout)
    error = captured_error(process)
    assert process.returncode == code, (process.returncode, error)
    return error


def fails(config, checkpoint, message):
    error = finish(start(config, checkpoint), 1)
    assert message in error, error


try:
    with tempfile.TemporaryDirectory(prefix="downsizing-process-") as temp:
        root = pathlib.Path(temp)
        inputs = root / "graphs"
        inputs.mkdir()
        graph = inputs / "cycle.txt"
        n = 34  # Keep enumeration alive through periodic saves with atomic cursors.
        graph.write_text("".join(f"{i} {(i + 1) % n}\n" for i in range(n)))
        triangle = inputs / "a_triangle.txt"
        triangle.write_text("100 200\n200 300\n100 300\n")
        config = root / "config.json"
        options = dict(input_directory=str(inputs), alpha_step=0.5, threads=1,
                       checkpoint_interval_seconds=1, log_interval_seconds=1, working_directory=str(root / "run"),
                       github=dict(enabled=False, token="SECRET_MUST_NOT_BE_SAVED"))
        config.write_text(json.dumps(options))
        checkpoint = root / "run"
        progress = checkpoint / "cycle/progress.json"
        key = f"{n // 2}/2/0"
        process = start(config)
        saved = wait_for(process, lambda: document(progress).get("nodes", {}).get(key))
        assert saved["status"] == "IN_PROGRESS", saved
        # Periodic sampling must work while a node is still being enumerated.
        resource_log = checkpoint / "run.log"
        wait_for(process, lambda: len(resource_log.read_text().split("resources cpu_percent=")) >= 3)
        first_cursor = wait_for(process, lambda: completed_subsets(document(progress).get("nodes", {}).get(key, {})))
        assert first_cursor > 0, first_cursor
        fails(config, checkpoint, "already in use")
        process.kill()
        process.communicate(timeout=10)
        assert completed_subsets(document(progress)["nodes"][key]) >= first_cursor

        # Garbage left by an interrupted write is never read as committed progress.
        pathlib.Path(str(progress) + ".tmp").write_text("{incomplete")
        options["threads"] = 4
        options["range_size"] = 4096
        config.write_text(json.dumps(options))
        process = start(config, checkpoint)
        wait_for(process, lambda: completed_subsets(document(progress).get("nodes", {}).get(key, {})) > first_cursor)
        process.send_signal(signal.SIGTERM)
        finish(process, 130)
        log = resource_log.read_text()
        assert "mode=run" in log and "mode=resume" in log
        assert "event=run_interrupted checkpoint_saved=true" in log
        assert "SECRET_MUST_NOT_BE_SAVED" not in log
        for field in ("cpu_percent", "rss_mib", "peak_rss_mib", "disk_available_gib"):
            values = [float(v) for v in re.findall(rf"\b{field}=([0-9.]+)", log)]
            assert values and all(math.isfinite(v) and v >= 0 for v in values), (field, log)
        assert "active_tasks=1" in log and "queued_tasks=" in log and "finished_tasks=" in log
        search_lines = [line for line in log.splitlines()
                        if 'resources ' in line and f'graph="cycle" phase=search alpha=0.5 r={n // 2} k=2' in line]
        assert search_lines, log
        assert all('level_nodes_done=0 level_nodes_total=1 level_nodes_percent=0.00' in line
                   and 'overall_nodes_done=0 overall_nodes_total=1 overall_percent=0.00' in line
                   for line in search_lines), search_lines
        second_cursor = completed_subsets(document(progress)["nodes"][key])
        assert second_cursor > first_cursor
        process = start(config, checkpoint)
        time.sleep(0.2)
        process.send_signal(signal.SIGINT)
        finish(process, 130)

        portable = root / "portable"
        shutil.copytree(checkpoint, portable)
        # Original TXT is deliberately required: graph contents are not in the checkpoint.
        graph.rename(root / "hidden.txt")
        missing_options = dict(options)
        missing_options.pop("input_directory")
        missing_options["inputs"] = [str(triangle), str(graph)]
        config.write_text(json.dumps(missing_options))
        fails(config, portable, "Graph file does not exist")
        config.write_text(json.dumps(options))
        (root / "hidden.txt").rename(graph)
        options["working_directory"] = str(root / "resumed")
        config.write_text(json.dumps(options))
        finish(start(config, portable))
        solved_lines = [line for line in (portable / "run.log").read_text().splitlines()
                        if 'event=solution' in line and 'graph="cycle"' in line]
        assert solved_lines and all('overall_percent=100.00 search_status=SOLVED' in line
                                    for line in solved_lines), solved_lines
        answer = root / "resumed/cycle/cycle_sol.txt"
        lines = answer.read_text().splitlines()
        assert lines[0] == f"0.5 {n // 2} 1 {n // 2 - 1}", lines[:1]
        assert lines[n // 2] == f"1 {n} 2 {n}", lines[n // 2:n // 2 + 1]
        assert (root / "resumed/a_triangle/a_triangle_sol.txt").read_text() == "0.5 1 0 0\n1 3 2 3\n100 200\n100 300\n200 300\n"
        before = answer.read_bytes()
        triangle_answer = root / "resumed/a_triangle/a_triangle_sol.txt"
        triangle_before = triangle_answer.read_bytes()
        triangle_answer.unlink() # Reconstruct a finished graph solely from node states.
        tree_before = (portable / "a_triangle/tree.json").stat().st_mtime_ns
        finish(start(config, portable))
        assert answer.read_bytes() == before
        assert triangle_answer.read_bytes() == triangle_before
        assert (portable / "a_triangle/tree.json").stat().st_mtime_ns == tree_before
        assert document(portable / "cycle/progress.json")["nodes"][key]["status"] == "FINISHED_NOT_FOUND"
        for folder in (portable / "a_triangle", portable / "cycle"):
            assert {p.name for p in folder.glob("*.json")} == {"tree.json", "progress.json"}
            for file in folder.glob("*.json"):
                assert "SECRET_MUST_NOT_BE_SAVED" not in file.read_text()
                assert "results" not in document(file)
                assert "checksum" not in document(file)
                assert "tree_checksum" not in document(file)
                assert "graph_sha256" not in document(file).get("metadata", {})

        changed = dict(options, alpha_step=0.25)
        config.write_text(json.dumps(changed))
        fails(config, portable, "parameters or input graph")
        config.write_text(json.dumps(options))
        corrupt = root / "corrupt"
        shutil.copytree(portable, corrupt)
        file = corrupt / "cycle/progress.json"
        file.write_text("{bad json")
        fails(config, corrupt, "Invalid checkpoint JSON")
        file.write_text(json.dumps({"version": 999}))
        fails(config, corrupt, "format version")

        clique = root / "clique.txt"
        size = 140
        clique.write_text("".join(f"{i} {j}\n" for i in range(size) for j in range(i + 1, size)))
        tree_config = root / "tree-config.json"
        tree_config.write_text(json.dumps(dict(inputs=[str(clique)], alpha_step=1, threads=2,
                                               checkpoint_interval_seconds=1,
                                               working_directory=str(root / "tree-run"))))
        tree_checkpoint = root / "tree-run"
        tree_file = tree_checkpoint / "clique/tree.json"
        process = start(tree_config)
        partial = wait_for(process, lambda: document(tree_file).get("state"))
        assert 0 < partial["next_parent"] < partial["node_count"], partial
        process.kill()
        process.communicate(timeout=10)
        finish(start(tree_config, tree_checkpoint))
        state = document(tree_file)
        assert state["state"]["next_parent"] == size - 1
        assert len(state["sets"]) == 1
        assert (tree_checkpoint / "clique/clique_sol.txt").read_text().splitlines()[0] == f"1 {size} {size - 1} {size * (size - 1) // 2}"
        print("JSON SIGKILL/SIGTERM/SIGINT, tree/cursor resume, two graphs, copies, corruption: passed")
finally:
    for process in children:
        if process.poll() is None:
            process.kill()
        process.communicate()
        process.log_output.close()
