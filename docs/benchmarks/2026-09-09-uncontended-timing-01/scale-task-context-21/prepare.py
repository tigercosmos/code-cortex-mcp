#!/usr/bin/env python3
"""Generate, index, and source-audit the frozen scale fixtures without model calls."""

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
SCALES = (("10k", 10_000), ("1m", 1_000_000), ("100m", 100_000_000))
MEMORY_BUDGET_ENV = "CBM_MEM_BUDGET_MB"


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


def command(argv, output, stem, env, timeout):
    save_new(output / f"{stem}.argv.json", argv)
    started = time.monotonic()
    try:
        result = subprocess.run(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout)
        code = result.returncode
        stdout = result.stdout
        stderr = result.stderr
        error = None
    except subprocess.TimeoutExpired as exc:
        code = None
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
        error = "timeout"
    (output / f"{stem}.stdout").write_bytes(stdout)
    (output / f"{stem}.stderr").write_bytes(stderr)
    save_new(output / f"{stem}.result.json",
             {"returncode": code, "error": error,
              "wall_seconds": time.monotonic() - started})
    if code != 0:
        raise RuntimeError(f"{stem} failed")
    return stdout


def unwrap(raw):
    envelope = json.loads(raw)
    if envelope.get("isError"):
        raise ValueError("MCP error: " + str(envelope))
    body = envelope.get("structuredContent")
    if isinstance(body, dict):
        return body
    for block in envelope.get("content", []):
        if block.get("type") == "text":
            value = json.loads(block["text"])
            if isinstance(value, dict):
                return value
    raise ValueError("missing object payload")


def verify_source(root, manifest, expected_lines):
    aggregate = hashlib.sha256()
    files = 0
    lines = 0
    bytes_total = 0
    for encoded in manifest.read_bytes().splitlines(keepends=True):
        aggregate.update(encoded)
        record = json.loads(encoded)
        path = root / record["file"]
        if not path.is_file() or sha256(path) != record["sha256"]:
            raise ValueError("source hash mismatch: " + record["file"])
        if path.stat().st_size != record["bytes"]:
            raise ValueError("source byte mismatch: " + record["file"])
        files += 1
        lines += record["lines"]
        bytes_total += record["bytes"]
    actual_paths = sum(1 for _ in root.glob("part_*/*.cpp"))
    if files != actual_paths or lines != expected_lines:
        raise ValueError("source inventory or line-count mismatch")
    return {"files": files, "code_lines": lines, "source_bytes": bytes_total,
            "manifest_sha256": aggregate.hexdigest()}


def tool(binary, env, output, stem, name, args):
    args_path = output / f"{stem}.args.json"
    save_new(args_path, args)
    raw = command([sys.executable, str(HERE / "mcp_once.py"), "--binary", str(binary),
                   "--tool", name, "--args-file", str(args_path)],
                  output, stem, env, 60)
    return unwrap(raw)


def audit_task(binary, env, output, project, task):
    if task["archetype"] == "one-shot":
        body = tool(binary, env, output, task["id"], "inspect_symbol",
                    {"project": project, "symbol": task["symbol"], "source_lines": 2,
                     "callers_limit": 0, "callees_limit": 0, "max_bytes": 4000})
        symbol = body.get("symbol", {})
        observed = f"{symbol.get('file')}:{symbol.get('start_line')}"
        correct = observed == task["expected"]
    else:
        body = tool(binary, env, output, task["id"], "trace_path",
                    {"project": project, "function_name": task["symbol"],
                     "from_function": task["from_symbol"], "direction": "inbound",
                     "mode": "calls", "depth": 3, "max_work": 128,
                     "source_context": 1, "max_bytes": 6000})
        observed = [node.get("name") for node in body.get("path", [])]
        correct = (body.get("path_found") is True and
                   body.get("traversal_strategy") == "targeted_forward_bfs" and
                   body.get("traversal_truncated") is False and observed == task["expected"])
    if not correct:
        raise ValueError("graph oracle mismatch for " + task["id"])
    return {"task": task["id"], "correct": True, "observed": observed,
            "payload_sha256": hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--scratch", type=pathlib.Path, required=True)
    parser.add_argument("--protocol", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.scratch.exists() or args.output.exists():
        raise FileExistsError("scratch and output must be new paths")
    args.output.mkdir(parents=True)
    args.scratch.mkdir(parents=True)
    report = {"state": "started", "model_sessions": 0}
    started = time.monotonic()
    try:
        binary = args.binary.resolve(strict=True)
        tasks = json.loads((args.protocol / "tasks.json").read_text())
        cache = args.scratch / "cache"
        home = args.scratch / "home"
        temporary = args.scratch / "tmp"
        for path in (cache, home, temporary):
            path.mkdir()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("CBM_", "CODEX_"))}
        env.update({"HOME": str(home), "XDG_CACHE_HOME": str(home / ".cache"),
                    "TMPDIR": str(temporary), "CBM_CACHE_DIR": str(cache),
                    "CBM_WORKERS": "4", MEMORY_BUDGET_ENV: "4096",
                    "CBM_RESULT_STORE": "1", "CBM_MEM_PROFILE": "1",
                    "CBM_INDEX_TOOL_TIMEOUT_S": "7200",
                    "CBM_INDEX_WORKER_TOTAL_TIMEOUT_S": "7200"})
        projects = {}
        for scale, lines in SCALES:
            phase = args.output / scale
            phase.mkdir()
            root = args.scratch / f"source-{scale}"
            manifest = phase / "source-manifest.jsonl"
            command([sys.executable, str(HERE / "generate_fixture.py"), "--root", str(root),
                     "--lines", str(lines), "--manifest", str(manifest)],
                    phase, "generate", env, 900)
            source_audit = verify_source(root, manifest, lines)
            save_new(phase / "source-audit.json", source_audit)
            project = f"scale-task-context-21-{scale}"
            index_args = {"repo_path": str(root), "name": project, "mode": "fast",
                          "persistence": False}
            index_args_path = phase / "index.args.json"
            save_new(index_args_path, index_args)
            raw = command(["/usr/bin/time", "-v", str(binary), "cli", "--json",
                           "index_repository", "--args-file", str(index_args_path)],
                          phase, "index", env, 7500)
            indexed = unwrap(raw)
            if indexed.get("status") != "indexed" or indexed.get("project") != project:
                raise ValueError("index result mismatch for " + scale)
            after = verify_source(root, manifest, lines)
            if after != source_audit:
                raise ValueError("source changed during indexing")
            projects[scale] = {"scale": scale, "code_lines": lines,
                               "repo_root": str(root), "project": project,
                               "manifest": str(manifest), "source_audit": source_audit,
                               "index_payload": indexed}
        audits = []
        for task in tasks:
            scale_output = args.output / task["scale"] / "task-oracle"
            scale_output.mkdir(exist_ok=True)
            audits.append(audit_task(binary, env, scale_output,
                                      projects[task["scale"]]["project"], task))
        setup = {"state": "ready", "host": os.uname().nodename,
                 "binary": str(binary), "binary_sha256": sha256(binary),
                 "cache": str(cache), "home": str(home), "tmp": str(temporary),
                 "projects": projects, "task_oracle": audits,
                 "task_oracle_passed": len(audits) == len(tasks),
                 "model_sessions": 0,
                 "index_configuration": {"workers": 4, "memory_budget_mb": 4096,
                                         "tool_timeout_seconds": 7200,
                                         "worker_total_timeout_seconds": 7200,
                                         "outer_timeout_seconds": 7500},
                 "protocol_hashes": {path.name: sha256(path) for path in args.protocol.iterdir()
                                     if path.is_file()},
                 "harness_hashes": {path.name: sha256(path) for path in HERE.glob("*.py")},
                 "setup_wall_seconds": time.monotonic() - started}
        save_new(args.output / "setup.json", setup)
        report.update(state="ready", setup_sha256=sha256(args.output / "setup.json"))
    except Exception as exc:
        report.update(state="failed", error=type(exc).__name__ + ": " + str(exc))
    finally:
        report["wall_seconds"] = time.monotonic() - started
        save_new(args.output / "report.json", report)
    if report["state"] != "ready":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
