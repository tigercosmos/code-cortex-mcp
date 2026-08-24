#!/usr/bin/env python3
"""benchmark-tool-latency.py — time MCP tool calls over stdio JSON-RPC.

Drives one server process the way an MCP client does (initialize,
tools/list, tools/call ...) and prints the wall-clock latency of each call as
seen by the client. Use it to compare binaries or environment settings:

    scripts/benchmark-tool-latency.py --project <name>
    scripts/benchmark-tool-latency.py --bin build/c/code-cortex-mcp --project <name>
    scripts/benchmark-tool-latency.py --project <name> --env CBM_TOOL_SUPERVISOR=0
    scripts/benchmark-tool-latency.py --project <name> --repeat 5 --json

The project must already be indexed (run `index_repository` first). Every
tool call is issued --repeat times; the first sample is reported separately
as "cold" (store open + integrity check) and the rest as the warm median.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time


def default_bin():
    for cand in ("build/c/code-cortex-mcp", "build/nosan/code-cortex-mcp"):
        if os.path.exists(cand):
            return cand
    return os.path.expanduser("~/.local/bin/code-cortex-mcp")


class Client:
    def __init__(self, bin_path, env, cwd):
        self.proc = subprocess.Popen(
            [bin_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            cwd=cwd,
        )
        self.next_id = 0

    def rpc(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        t0 = time.perf_counter()
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        dt_ms = (time.perf_counter() - t0) * 1000.0
        if not line:
            raise RuntimeError(f"server closed the pipe during {method}")
        return dt_ms, json.loads(line)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

    def call(self, name, args):
        dt_ms, resp = self.rpc("tools/call", {"name": name, "arguments": args})
        result = resp.get("result", {})
        is_error = bool(result.get("isError"))
        text = ""
        content = result.get("content") or []
        if content and isinstance(content[0], dict):
            text = content[0].get("text", "")
        return dt_ms, is_error, text

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def tool_calls(project):
    """The fixed call set. Each entry: (label, tool, args)."""
    return [
        ("index_status", "index_status", {"project": project}),
        ("search_graph name glob", "search_graph",
         {"project": project, "name_pattern": "*main*", "limit": 10}),
        ("search_graph label", "search_graph",
         {"project": project, "label": "Function", "limit": 10}),
        ("trace_path calls", "trace_path",
         {"project": project, "function_name": "main", "mode": "calls"}),
        ("get_code_snippet", "get_code_snippet",
         {"project": project, "qualified_name": "main"}),
        ("query_graph count", "query_graph",
         {"project": project, "query": "MATCH (n:Function) RETURN count(n)"}),
        ("get_graph_schema", "get_graph_schema", {"project": project}),
        ("get_architecture", "get_architecture", {"project": project}),
        ("search_code", "search_code",
         {"project": project, "pattern": "main", "limit": 5}),
        ("arg error (no project)", "search_graph", {"name_pattern": "x"}),
        ("list_projects", "list_projects", {"limit": 50}),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=default_bin(), help="server binary (default: newest build)")
    ap.add_argument("--project", required=True, help="indexed project name")
    ap.add_argument("--repeat", type=int, default=4, help="samples per call (default 4)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra environment variable for the server (repeatable)")
    ap.add_argument("--cwd", default=os.getcwd(), help="server working directory")
    ap.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = ap.parse_args()

    env = dict(os.environ)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    spawn_t0 = time.perf_counter()
    client = Client(args.bin, env, args.cwd)
    init_ms, _ = client.rpc("initialize", {
        "protocolVersion": "2024-11-05", "capabilities": {},
        "clientInfo": {"name": "benchmark-tool-latency", "version": "1"}})
    spawn_to_init_ms = (time.perf_counter() - spawn_t0) * 1000.0
    client.notify("notifications/initialized")
    list_ms, _ = client.rpc("tools/list")

    rows = []
    for label, tool, targs in tool_calls(args.project):
        samples = []
        errors = 0
        last_text = ""
        for _ in range(max(1, args.repeat)):
            dt, is_err, text = client.call(tool, targs)
            samples.append(dt)
            errors += int(is_err)
            last_text = text
        warm = samples[1:] if len(samples) > 1 else samples
        rows.append({
            "label": label,
            "tool": tool,
            "cold_ms": samples[0],
            "warm_median_ms": statistics.median(warm),
            "warm_min_ms": min(warm),
            "errors": errors,
            "preview": last_text[:60].replace("\n", " "),
        })
    client.close()

    summary = {
        "bin": args.bin,
        "project": args.project,
        "env": args.env,
        "spawn_to_initialize_ms": spawn_to_init_ms,
        "initialize_ms": init_ms,
        "tools_list_ms": list_ms,
        "calls": rows,
    }
    if args.json:
        json.dump(summary, sys.stdout, indent=2)
        print()
        return 0

    print(f"bin: {args.bin}")
    print(f"project: {args.project}   env: {' '.join(args.env) or '(none)'}")
    print(f"spawn→initialize: {spawn_to_init_ms:.1f} ms   initialize: {init_ms:.1f} ms   "
          f"tools/list: {list_ms:.1f} ms")
    print()
    print(f"{'call':<26} {'cold ms':>9} {'warm med':>9} {'warm min':>9} {'err':>4}  preview")
    for r in rows:
        print(f"{r['label']:<26} {r['cold_ms']:>9.1f} {r['warm_median_ms']:>9.1f} "
              f"{r['warm_min_ms']:>9.1f} {r['errors']:>4}  {r['preview']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
