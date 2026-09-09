#!/usr/bin/env python3
"""Create the deterministic, paired holdout protocol before any model session."""

import argparse
import json
import pathlib
import random

SEED = 2026090922
SCALES = (("10k", 10_000), ("1m", 1_000_000), ("100m", 100_000_000))


def save_new(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def tasks_for(scale, lines):
    randomizer = random.Random(SEED + lines)
    file_count = lines // 1000
    selected_files = randomizer.sample(range(file_count), 8)
    tasks = []
    for ordinal, file_number in enumerate(selected_files[:4]):
        local = randomizer.randrange(2, 20)
        symbol = file_number * 20 + local
        relative = f"part_{file_number // 1000:04d}/unit_{file_number:06d}.cpp"
        tasks.append({
            "id": f"{scale}-lookup-{ordinal + 1}", "scale": scale, "lines": lines,
            "archetype": "one-shot", "symbol": f"capacity_function_{symbol}",
            "question":
                f"Find the defining signature of capacity_function_{symbol}. Return exactly "
                "one repository-relative path:line and no other text.",
            "expected": f"{relative}:{local * 50 + 1}",
        })
    for ordinal, file_number in enumerate(selected_files[4:]):
        start_local = 19 - ordinal
        target_local = start_local - 3
        start = file_number * 20 + start_local
        target = file_number * 20 + target_local
        tasks.append({
            "id": f"{scale}-chain-{ordinal + 1}", "scale": scale, "lines": lines,
            "archetype": "multi-step", "from_symbol": f"capacity_function_{start}",
            "symbol": f"capacity_function_{target}",
            "question":
                f"Find the shortest direct-call chain from capacity_function_{start} to "
                f"capacity_function_{target}, including both endpoints. Return exactly one bare "
                "function name per line in call order and no other text.",
            "expected": [f"capacity_function_{number}" for number in range(start, target - 1, -1)],
        })
    return tasks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    tasks = [task for scale, lines in SCALES for task in tasks_for(scale, lines)]
    blocks = list(tasks)
    random.Random(SEED).shuffle(blocks)
    rows = []
    for block_id, task in enumerate(blocks):
        arms = ["shell", "mcp-context"]
        if block_id % 2:
            arms.reverse()
        for arm in arms:
            rows.append({"block_id": block_id, "task": task["id"], "arm": arm,
                         "model": "gpt-5.6-sol", "effort": "medium",
                         "run": f"{task['id']}-{arm}"})
    protocol = {
        "schema": 1,
        "seed": SEED,
        "project_sizes_code_lines": [lines for _, lines in SCALES],
        "tasks": len(tasks),
        "independent_paired_blocks": len(blocks),
        "arms": ["shell", "mcp-context"],
        "task_wall": "retrieval start through the fresh Codex session's recorded terminal timestamp",
        "terminal_observation": "poll-observation latency is retained separately and excluded from task_wall",
        "setup_excluded": "fixture generation, indexing, correctness oracle and exposure smoke",
        "mcp_lifecycle": "one production stdio server is initialized before the timed schedule; each tools/call remains inside its task timer",
        "source_correctness_gate": "exact answer, read-only source unchanged, audited command evidence",
        "primary_pass": {
            "all_rows_source_correct": True,
            "overall_total_ratio_at_most": 0.90,
            "overall_geometric_ratio_at_most": 0.90,
            "one_sided_exact_sign_test_graph_faster_p_below": 0.05,
            "each_scale_geometric_ratio_at_most": 0.90,
            "each_archetype_geometric_ratio_at_most": 0.90,
        },
        "pairing": "adjacent task pairs; 12 shell-first and 12 graph-first after seeded task shuffle",
        "context_budget_bytes": 6000,
        "retrieval_timeout_seconds": 20,
        "session_wall_timeout_seconds": 300,
        "contention": "one timed session at a time on a host; one-second process/load samples retained",
        "limits": [
            "Synthetic C++ scale fixtures test deterministic lookup and call-chain discovery, not language diversity.",
            "Warm source and graph caches are established before the frozen timed schedule.",
            "The optimized arm invokes the production MCP server over stdio JSON-RPC before the identical shell-only model session.",
        ],
    }
    save_new(args.output / "tasks.json", tasks)
    save_new(args.output / "schedule.json", {"seed": SEED, "rows": rows})
    save_new(args.output / "protocol.json", protocol)


if __name__ == "__main__":
    main()
