#!/usr/bin/env python3
"""Issue one production MCP tool call over stdio JSON-RPC."""

import argparse
import json
import pathlib
import subprocess
import sys


def response_with_id(raw, request_id):
    matches = []
    for line in raw.splitlines():
        if not line.strip():
            continue
        value = json.loads(line)
        if value.get("id") == request_id:
            matches.append(value)
    if len(matches) != 1:
        raise ValueError(f"expected one response for id {request_id}, got {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--args-file", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=float, default=50.0)
    args = parser.parse_args()
    arguments = json.loads(args.args_file.read_text())
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "scale-task-context-21", "version": "1"},
        }},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
            "name": args.tool, "arguments": arguments,
        }},
    ]
    payload = "".join(json.dumps(message, separators=(",", ":")) + "\n"
                      for message in messages).encode()
    result = subprocess.run([str(args.binary)], input=payload, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False, timeout=args.timeout)
    sys.stderr.buffer.write(result.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"MCP server exited {result.returncode}")
    initialize = response_with_id(result.stdout, 1)
    response = response_with_id(result.stdout, 2)
    if "error" in initialize:
        raise RuntimeError("MCP initialize failed: " + json.dumps(initialize["error"]))
    if "error" in response:
        raise RuntimeError("MCP tools/call failed: " + json.dumps(response["error"]))
    sys.stdout.write(json.dumps(response.get("result"), sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
