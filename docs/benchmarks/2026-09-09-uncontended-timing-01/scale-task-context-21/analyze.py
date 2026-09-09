#!/usr/bin/env python3
"""Audit source, tools, contention, and the preregistered paired speed result."""

import argparse
import hashlib
import json
import math
import pathlib


FORBIDDEN_SHELL = (
    "code-cortex", "cbm_", ".db", "sqlite", "curl ", "wget ", "ssh ",
    "nc ", "ncat ", "netcat ", "tasks.json", "schedule.json", "source-oracle",
    "source-manifest", "../",
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_new(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def source_audit(setup):
    projects = {}
    for scale, project in setup["projects"].items():
        root = pathlib.Path(project["repo_root"])
        manifest = pathlib.Path(project["manifest"])
        failures = []
        files = 0
        for line in manifest.read_text().splitlines():
            record = json.loads(line)
            path = root / record["file"]
            files += 1
            if not path.is_file() or path.stat().st_size != record["bytes"] or sha256(path) != record["sha256"]:
                failures.append(record["file"])
                if len(failures) == 20:
                    break
        actual_files = sum(1 for _ in root.glob("part_*/*.cpp"))
        projects[scale] = {"expected_files": project["source_audit"]["files"],
                           "checked_files": files, "actual_files": actual_files,
                           "failures": failures,
                           "unchanged": not failures and files == actual_files ==
                                        project["source_audit"]["files"]}
    return {"unchanged": all(row["unchanged"] for row in projects.values()),
            "projects": projects}


def snapshots(directory):
    result = []
    for name in ("contention-pre.json", "contention-post.json"):
        path = directory / name
        if path.is_file():
            result.append(json.loads(path.read_text()))
    samples = directory / "contention-samples.jsonl"
    if samples.is_file():
        result.extend(json.loads(line) for line in samples.read_text().splitlines() if line)
    return result


def contention_audit(directory):
    samples = snapshots(directory)
    max_load = max((sample["loadavg"][0] for sample in samples), default=math.inf)
    foreign = []
    for sample in samples:
        processes = []
        for line in sample.get("processes", []):
            fields = line.strip().split(None, 5)
            if len(fields) < 6:
                continue
            pid, ppid, cpu, rss, command, argv = fields
            try:
                process = {"pid": int(pid), "ppid": int(ppid), "cpu_percent": float(cpu),
                           "rss_kib": int(rss), "command": command, "argv": argv}
            except ValueError:
                continue
            processes.append(process)
        related = set(sample.get("related_root_pids", []))
        changed = True
        while changed:
            changed = False
            for process in processes:
                if process["pid"] not in related and process["ppid"] in related:
                    related.add(process["pid"])
                    changed = True
        for process in processes:
            if process["cpu_percent"] >= 50.0 and process["pid"] not in related:
                foreign.append({"utc": sample["utc"], **process})
    return {"samples": len(samples), "max_load_1m": max_load,
            "foreign_processes_at_least_50_percent_cpu": foreign,
            "admissible": len(samples) >= 3 and max_load <= 8.0 and not foreign,
            "thresholds": {"max_load_1m": 8.0, "foreign_process_cpu_percent": 50.0,
                           "logical_cpu_context": 32}}


def geometric(values):
    return math.exp(sum(math.log(value) for value in values) / len(values)) if values else None


def one_sided_sign(wins, n):
    return sum(math.comb(n, k) for k in range(wins, n + 1)) / (2 ** n) if n else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup", type=pathlib.Path, required=True)
    parser.add_argument("--protocol", type=pathlib.Path, required=True)
    parser.add_argument("--runs", type=pathlib.Path, required=True)
    parser.add_argument("--source-oracle", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    setup = json.loads(args.setup.read_text())
    protocol = json.loads((args.protocol / "protocol.json").read_text())
    tasks = {task["id"]: task for task in json.loads((args.protocol / "tasks.json").read_text())}
    schedule = json.loads((args.protocol / "schedule.json").read_text())["rows"]
    oracle = json.loads(args.source_oracle.read_text())
    sources = source_audit(setup)
    rows = []
    for row in schedule:
        directory = args.runs / row["run"]
        summary_path = directory / "summary.json"
        summary = json.loads(summary_path.read_text()) if summary_path.is_file() else {}
        grade_path = directory / "grade.json"
        grade = json.loads(grade_path.read_text()) if grade_path.is_file() else {}
        tools_path = directory / "tool-audit.json"
        tools = json.loads(tools_path.read_text()) if tools_path.is_file() else {}
        commands = [value if isinstance(value, str) else json.dumps(value, sort_keys=True)
                    for value in tools.get("commands", [])]
        forbidden = [value for value in commands if any(token in value.lower()
                                                        for token in FORBIDDEN_SHELL)]
        unapproved_items = [kind for kind in tools.get("item_types", [])
                            if kind not in ("agent_message", "command_execution", "reasoning",
                                            "todo_list")]
        environment = contention_audit(directory)
        retrieval_compliant = (
            (row["arm"] == "mcp-context" and
             summary.get("retrieval_backend") == "production_mcp_stdio_jsonrpc" and
             summary.get("retrieval_context_bytes", 0) > 0 and
             summary.get("source_oracle_correct") is True) or
            (row["arm"] == "shell" and summary.get("retrieval_backend") == "none" and
             summary.get("retrieval_context_bytes") == 0)
        )
        audited = {**row, "task_wall_seconds": summary.get("task_wall_seconds"),
                   "state": summary.get("state", "not_attempted"),
                   "agent_exit_code": summary.get("agent_exit_code"),
                   "answer_correct": grade.get("correct") is True,
                   "no_session_mcp_calls": tools.get("mcp_calls") == 0,
                   "forbidden_shell_commands": forbidden,
                   "unapproved_item_types": unapproved_items,
                   "retrieval_protocol_compliant": retrieval_compliant,
                   "shell_arm_used_shell": row["arm"] != "shell" or tools.get("command_calls", 0) > 0,
                   "tool_protocol_compliant": (tools.get("mcp_calls") == 0 and not forbidden and
                                               not unapproved_items and
                                               (row["arm"] != "shell" or
                                                tools.get("command_calls", 0) > 0)),
                   "environment": environment,
                   "source_unchanged": sources["unchanged"]}
        audited["eligible"] = (audited["state"] == "completed" and
                               audited["agent_exit_code"] == 0 and audited["answer_correct"] and
                               audited["tool_protocol_compliant"] and
                               audited["retrieval_protocol_compliant"] and
                               environment["admissible"] and
                               sources["unchanged"] and oracle.get("passed") is True)
        rows.append(audited)
    groups = {}
    for row in rows:
        groups.setdefault(row["task"], {})[row["arm"]] = row
    pairs = []
    for task_id, arms in groups.items():
        graph = arms.get("mcp-context")
        shell = arms.get("shell")
        eligible = bool(graph and shell and graph["eligible"] and shell["eligible"])
        ratio = graph["task_wall_seconds"] / shell["task_wall_seconds"] if eligible else None
        pairs.append({"task": task_id, "scale": tasks[task_id]["scale"],
                      "archetype": tasks[task_id]["archetype"], "eligible": eligible,
                      "ratio": ratio,
                      "mcp_context_seconds": graph.get("task_wall_seconds") if graph else None,
                      "shell_seconds": shell.get("task_wall_seconds") if shell else None})
    admitted = [pair for pair in pairs if pair["eligible"]]
    ratios = [pair["ratio"] for pair in admitted]
    graph_total = sum(pair["mcp_context_seconds"] for pair in admitted)
    shell_total = sum(pair["shell_seconds"] for pair in admitted)
    total_ratio = graph_total / shell_total if shell_total else None
    wins = sum(ratio < 1.0 for ratio in ratios)
    by_scale = {scale: geometric([pair["ratio"] for pair in admitted if pair["scale"] == scale])
                for scale in ("10k", "1m", "100m")}
    by_archetype = {kind: geometric([pair["ratio"] for pair in admitted
                                     if pair["archetype"] == kind])
                    for kind in ("one-shot", "multi-step")}
    primary = {"all_scheduled_rows_present": len(rows) == len(schedule) and
                                              all(row["state"] != "not_attempted" for row in rows),
               "all_rows_source_correct": all(row["answer_correct"] for row in rows) and
                                           oracle.get("passed") is True and sources["unchanged"],
               "all_pairs_environment_admissible": len(admitted) == 24,
               "mcp_context_total_seconds": graph_total,
               "shell_total_seconds": shell_total,
               "overall_total_ratio": total_ratio,
               "overall_total_at_most_0_90": total_ratio is not None and total_ratio <= 0.90,
               "overall_geometric_ratio": geometric(ratios),
               "overall_at_most_0_90": geometric(ratios) is not None and geometric(ratios) <= 0.90,
               "graph_faster_wins": wins, "sign_pairs": len(ratios),
               "one_sided_sign_p": one_sided_sign(wins, len(ratios)),
               "sign_p_below_0_05": one_sided_sign(wins, len(ratios)) is not None and
                                    one_sided_sign(wins, len(ratios)) < 0.05,
               "by_scale_geometric_ratio": by_scale,
               "each_scale_at_most_0_90": all(value is not None and value <= 0.90
                                               for value in by_scale.values()),
               "by_archetype_geometric_ratio": by_archetype,
               "each_archetype_at_most_0_90": all(value is not None and value <= 0.90
                                                   for value in by_archetype.values())}
    primary["passed"] = all(value for key, value in primary.items()
                            if key in ("all_scheduled_rows_present", "all_rows_source_correct",
                                       "all_pairs_environment_admissible", "overall_at_most_0_90",
                                       "overall_total_at_most_0_90",
                                       "sign_p_below_0_05", "each_scale_at_most_0_90",
                                       "each_archetype_at_most_0_90"))
    report = {"primary": primary, "pairs": sorted(pairs, key=lambda pair: pair["task"]),
              "rows": rows, "source_audit": sources,
              "source_oracle_sha256": sha256(args.source_oracle),
              "protocol_sha256": sha256(args.protocol / "protocol.json"),
              "setup_sha256": sha256(args.setup),
              "limits": protocol["limits"]}
    save_new(args.output, report)
    if not primary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
