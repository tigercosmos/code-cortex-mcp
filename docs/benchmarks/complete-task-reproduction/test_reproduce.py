#!/usr/bin/env python3
"""Tests for the public complete-task reproduction harness."""

import importlib.util
import json
import pathlib
import tempfile
import unittest


HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("complete_task_reproduce", HERE / "reproduce.py")
REPRODUCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPRODUCE)


class ReproductionHarnessTests(unittest.TestCase):
    def setUp(self):
        self.config_path, self.config = REPRODUCE.load_config(HERE / "config.json")

    def test_configuration_references_complete_frozen_protocol(self):
        tasks_path = REPRODUCE.resolve_config_path(
            self.config_path, self.config["fixtures"]["protocol_tasks"])
        schedule_path = REPRODUCE.resolve_config_path(
            self.config_path, self.config["fixtures"]["protocol_schedule"])
        tasks = json.loads(tasks_path.read_text())
        rows = json.loads(schedule_path.read_text())["rows"]
        self.assertEqual(len(tasks), 24)
        self.assertEqual(len(rows), 64)
        self.assertEqual({row["arm"] for row in rows},
                         {"candidate-mcp", "shell", "upstream-mcp"})
        self.assertEqual(sum(row["arm"] == "candidate-mcp" for row in rows), 24)
        self.assertEqual(sum(row["arm"] == "shell" for row in rows), 24)
        self.assertEqual(sum(row["arm"] == "upstream-mcp" for row in rows), 16)
        self.assertFalse(any(row["arm"] == "upstream-mcp" and
                             row["task"].startswith("100m-") for row in rows))
        self.assertEqual(len({row["run"] for row in rows}), 64)
        self.assertEqual(len({(row["task"], row["arm"]) for row in rows}), 64)
        self.assertTrue(all(item["sha256_matches"] for item in
                            REPRODUCE.fixture_receipt(
                                self.config_path, self.config).values()))
        self.assertTrue(all(item["sha256_matches"] for item in
                            REPRODUCE.harness_receipt(
                                self.config_path, self.config).values()))
        self.assertEqual(self.config["backends"]["upstream"]["build"],
                         ["bash", "scripts/build.sh"])
        self.assertEqual(
            self.config["backends"]["upstream"]["build_arguments"],
            ["BUILD_DIR=build/benchmark-upstream"])
        self.assertEqual(self.config["backends"]["upstream"]["build_environment"], {})

    def test_hash_manifest_detects_tampering(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "record.txt").write_text("sealed\n")
            REPRODUCE.write_hash_manifest(root)
            self.assertEqual(REPRODUCE.verify_hash_manifest(root), 1)
            (root / "record.txt").write_text("changed\n")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                REPRODUCE.verify_hash_manifest(root)
            (root / "record.txt").write_text("sealed\n")
            (root / "unlisted.txt").write_text("not sealed\n")
            with self.assertRaisesRegex(ValueError, "closed file tree"):
                REPRODUCE.verify_hash_manifest(root)

    def test_setup_requires_an_external_ready_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            REPRODUCE.save_new(root / "SETUP-READY.json", {"state": "ready"})
            with self.assertRaisesRegex(ValueError, "external setup READY"):
                REPRODUCE.verify_setup(
                    self.config_path, self.config, root, "0" * 64)

    def make_run(self, root):
        protocol = root / "protocol"
        protocol.mkdir()
        tasks_source = REPRODUCE.resolve_config_path(
            self.config_path, self.config["fixtures"]["protocol_tasks"])
        schedule_source = REPRODUCE.resolve_config_path(
            self.config_path, self.config["fixtures"]["protocol_schedule"])
        (protocol / "tasks.json").write_bytes(tasks_source.read_bytes())
        (protocol / "schedule.json").write_bytes(schedule_source.read_bytes())
        rows = json.loads((protocol / "schedule.json").read_text())["rows"]
        output = root / "run"
        output.mkdir()
        oracle = {"passed": True, "tasks": 24}
        source_manifest = {"passed": True, "projects": {}}
        REPRODUCE.save_new(output / "source-oracle-pre.json", oracle)
        REPRODUCE.save_new(output / "source-oracle-post.json", oracle)
        REPRODUCE.save_new(output / "source-manifest-pre.json", source_manifest)
        REPRODUCE.save_new(output / "source-manifest-post.json", source_manifest)
        REPRODUCE.save_new(output / "run-finalization.json", {"passed": True})
        for row in rows:
            directory = output / row["run"]
            directory.mkdir()
            backend = {
                "candidate-mcp": "candidate_production_mcp_stdio_jsonrpc",
                "shell": "none",
                "upstream-mcp": "upstream_production_mcp_stdio_jsonrpc",
            }[row["arm"]]
            seconds = {"candidate-mcp": 4.0, "shell": 10.0, "upstream-mcp": 7.0}[row["arm"]]
            REPRODUCE.save_new(directory / "summary.json", {**row,
                "answer_correct": True,
                "no_session_mcp_calls": True,
                "retrieval_backend": backend,
                "retrieval_context_bytes": 0 if row["arm"] == "shell" else 32,
                "source_oracle_correct": True,
                "state": "completed",
                "task_wall_seconds": seconds,
                "terminal_observed": True,
                "runtime_uncontended": True,
            })
            REPRODUCE.save_new(directory / "grade.json", {"correct": True})
            REPRODUCE.save_new(directory / "contention-admission.json", {"admitted": True})
            REPRODUCE.save_new(directory / "contention-runtime.json", {"passed": True})
            REPRODUCE.save_new(directory / "retrieval-receipt.json", {
                "backend": backend,
                "source_oracle_correct": None if row["arm"] == "shell" else True,
            })
            REPRODUCE.save_new(directory / "terminal-status.json", {
                "exit_code": 0, "state": "completed",
            })
            REPRODUCE.append_json(
                output / "results.jsonl",
                json.loads((directory / "summary.json").read_text()))
        return {"protocol": str(protocol)}, output, rows

    def test_analysis_requires_complete_rows_and_minimum_exact_pairs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            setup, output, rows = self.make_run(root)
            report = REPRODUCE.analyze_data(self.config, setup, output)
            self.assertTrue(report["passed"])
            self.assertEqual(report["comparisons"]["candidate_to_shell"]["all"]
                             ["eligible_pairs"], 24)
            self.assertAlmostEqual(report["comparisons"]["candidate_to_shell"]["all"]
                                   ["geometric_ratio"], 0.4)
            candidate_rows = [row for row in rows if row["arm"] == "candidate-mcp"]
            for row in candidate_rows[:3]:
                grade = output / row["run"] / "grade.json"
                grade.write_text(json.dumps({"correct": False}) + "\n")
            failed = REPRODUCE.analyze_data(self.config, setup, output)
            self.assertFalse(failed["passed"])
            self.assertFalse(failed["gates"]["candidate_shell_pair_count"])

            (output / "source-manifest-post.json").write_text(
                json.dumps({"passed": False, "projects": {}}) + "\n")
            source_failed = REPRODUCE.analyze_data(self.config, setup, output)
            self.assertFalse(source_failed["passed"])
            self.assertFalse(source_failed["gates"]["source_oracle"])


if __name__ == "__main__":
    unittest.main()
