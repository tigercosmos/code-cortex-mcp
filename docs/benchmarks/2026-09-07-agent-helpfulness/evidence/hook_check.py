#!/usr/bin/env python3
"""Feed sample Claude Code hook payloads to `hook-augment` and time them."""
import json, subprocess, sys, time
binary = sys.argv[1]
repo = '/Users/tigercosmos/code-cortex-mcp'
cases = [
    ('PreToolUse Bash grep exact', {"hook_event_name": "PreToolUse", "tool_name": "Bash", "cwd": repo,
                                   "tool_input": {"command": "grep -rn cbm_json_escape src/"}}),
    ('PreToolUse Grep fuzzy', {"hook_event_name": "PreToolUse", "tool_name": "Grep", "cwd": repo,
                               "tool_input": {"pattern": "getenv"}}),
    ('PreToolUse Bash rg class', {"hook_event_name": "PreToolUse", "tool_name": "Bash", "cwd": "/Users/tigercosmos/PcapPlusPlus",
                                 "tool_input": {"command": "rg 'class Layer' Packet++"}}),
    ('PreToolUse Bash cross-lang', {"hook_event_name": "PreToolUse", "tool_name": "Bash", "cwd": "/Users/tigercosmos/modmesh",
                                   "tool_input": {"command": "grep -rn world_from_screen cpp/ solvcon/"}}),
    ('PostToolUse Edit', {"hook_event_name": "PostToolUse", "tool_name": "Edit", "cwd": repo,
                          "tool_input": {"file_path": repo + "/src/foundation/str_util.cpp", "old_string": "a", "new_string": "b"},
                          "tool_response": {"success": True}}),
    ('PostToolUse Edit modmesh', {"hook_event_name": "PostToolUse", "tool_name": "Write", "cwd": "/Users/tigercosmos/modmesh",
                          "tool_input": {"file_path": "/Users/tigercosmos/modmesh/cpp/solvcon/universe/ViewTransform2d.hpp", "content": "x"}}),
    ('SessionStart', {"hook_event_name": "SessionStart", "source": "startup", "cwd": repo}),
    ('SessionStart not indexed', {"hook_event_name": "SessionStart", "source": "startup", "cwd": "/private/tmp"}),
]
for name, payload in cases:
    t0 = time.time()
    p = subprocess.run([binary, 'hook-augment'], input=json.dumps(payload), capture_output=True, text=True, timeout=30)
    dt = (time.time() - t0) * 1000
    out = p.stdout.strip()
    try:
        j = json.loads(out); out = j['hookSpecificOutput']['hookEventName'] + ': ' + j['hookSpecificOutput']['additionalContext']
    except Exception:
        pass
    print(f'== {name} [{dt:.0f} ms, rc={p.returncode}, {len(p.stdout)} bytes]\n{out}\n')
