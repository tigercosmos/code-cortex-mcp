#!/usr/bin/env python3
"""Focused deterministic tests for the scale-task protocol and oracle."""

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import analyze
import build_protocol
import generate_fixture
import mcp_once
import prepare
import run
import source_oracle


class HarnessTests(unittest.TestCase):
    def test_source_file_contract(self):
        source = generate_fixture.source_file(7).decode().splitlines()
        self.assertEqual(len(source), 1000)
        self.assertEqual(source[0], "int capacity_function_140(int value) {")
        self.assertEqual(source[49], "}")
        self.assertEqual(source[50], "int capacity_function_141(int value) {")
        self.assertEqual(source[98], "    return capacity_function_140(value);")

    def test_protocol_has_distinct_balanced_pairs(self):
        tasks = [task for scale, lines in build_protocol.SCALES
                 for task in build_protocol.tasks_for(scale, lines)]
        self.assertEqual(len(tasks), 24)
        self.assertEqual(len({task["id"] for task in tasks}), 24)
        for scale, _ in build_protocol.SCALES:
            selected = [task for task in tasks if task["scale"] == scale]
            self.assertEqual(sum(task["archetype"] == "one-shot" for task in selected), 4)
            self.assertEqual(sum(task["archetype"] == "multi-step" for task in selected), 4)

    def test_source_oracle_rejects_broken_edge(self):
        task = build_protocol.tasks_for("10k", 10_000)[4]
        number, relative, _ = source_oracle.location(task["from_symbol"])
        file_number = number // 20
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / relative
            path.parent.mkdir(parents=True)
            path.write_bytes(generate_fixture.source_file(file_number))
            self.assertTrue(source_oracle.verify_task(task, root)["passed"])
            text = path.read_text().replace(
                f"return capacity_function_{number - 1}(value);", "return value;", 1)
            path.write_text(text)
            self.assertFalse(source_oracle.verify_task(task, root)["passed"])

    def test_exact_grade(self):
        lookup = build_protocol.tasks_for("10k", 10_000)[0]
        self.assertTrue(run.grade(lookup, lookup["expected"] + "\n")["correct"])
        self.assertFalse(run.grade(lookup, "answer: " + lookup["expected"])["correct"])
        chain = build_protocol.tasks_for("10k", 10_000)[4]
        self.assertTrue(run.grade(chain, "\n".join(chain["expected"]))["correct"])

    def test_statistics(self):
        self.assertAlmostEqual(analyze.geometric([0.8, 1.0125]), 0.9)
        self.assertEqual(analyze.one_sided_sign(24, 24), 1 / 2**24)

    def test_terminal_timestamp(self):
        self.assertEqual(run.timestamp_seconds("1970-01-01T09:00:01+09:00"), 1.0)

    def test_prepare_uses_supported_memory_budget_variable(self):
        self.assertEqual(prepare.MEMORY_BUDGET_ENV, "CBM_MEM_BUDGET_MB")

    def test_mcp_response_selection(self):
        raw = (b'{"jsonrpc":"2.0","id":1,"result":{}}\n'
               b'{"jsonrpc":"2.0","id":2,"result":{"isError":false}}\n')
        self.assertFalse(mcp_once.response_with_id(raw, 2)["result"]["isError"])
        with self.assertRaises(ValueError):
            mcp_once.response_with_id(raw, 3)

    def test_persistent_mcp_client(self):
        program = """#!/usr/bin/env python3
import json
import sys
for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    if request["method"] == "initialize":
        result = {"protocolVersion": "2024-11-05"}
    elif request["method"] == "tools/list":
        result = {"tools": []}
    else:
        result = {"isError": False, "structuredContent": {"ok": True}}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            server = root / "fake-mcp"
            server.write_text(program)
            server.chmod(0o755)
            output = root / "evidence"
            output.mkdir()
            client = run.MCPClient(server, os.environ.copy(), output, root)
            try:
                result = client.call("probe", {"value": 1})
                self.assertTrue(result["structuredContent"]["ok"])
            finally:
                client.close()
            self.assertTrue(client.proc.stdout.closed)
            self.assertIn("notifications/initialized", (output / "mcp-transcript.jsonl").read_text())


if __name__ == "__main__":
    unittest.main()
