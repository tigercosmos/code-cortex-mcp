#!/usr/bin/env python3
"""Tests for the public complete-task reproduction harness."""

import importlib.util
import io
import json
import os
import pathlib
import tempfile
import unittest
from contextlib import redirect_stdout


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

    def test_binary_hash_mismatch_is_recorded_as_reference_metadata(self):
        receipt = REPRODUCE.binary_hash_receipt("new-path-hash", "published-hash")
        self.assertEqual(receipt["binary_sha256"], "new-path-hash")
        self.assertEqual(receipt["published_binary_sha256"], "published-hash")
        self.assertFalse(receipt["binary_sha256_matches_published"])

    def test_runtime_contention_uses_sample_related_roots(self):
        sample = {
            "processes": [
                "100 1 0.0 10 worker worker",
                "101 100 80.0 10 rg rg symbol",
                "200 300 80.0 10 daemon-worker daemon-worker build",
                "300 1 0.0 10 daemon daemon",
                "400 1 80.0 10 foreign foreign build",
                f"{os.getpid()} 1 80.0 10 analyzer analyzer offline",
            ],
            "related_root_pids": [100],
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "contention-samples.jsonl"
            path.write_text(json.dumps(sample) + "\n")
            report = REPRODUCE.runtime_contention_report(path)
        self.assertFalse(report["passed"])
        self.assertEqual(
            [row["pid"] for row in report["foreign_processes"]],
            [200, 400, os.getpid()])

        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "contention-samples.jsonl"
            path.write_text(json.dumps(sample) + "\n")
            report = REPRODUCE.runtime_contention_report(path, (300,))
        self.assertFalse(report["passed"])
        self.assertEqual(
            [row["pid"] for row in report["foreign_processes"]],
            [400, os.getpid()])
        self.assertEqual(
            [row["pid"] for row in report["related_processes_at_or_above_threshold"]],
            [101, 200])
        self.assertEqual(report["threshold_percent_cpu"], 50.0)

        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "contention-samples.jsonl"
            path.write_text(json.dumps(sample) + "\n")
            report = REPRODUCE.runtime_contention_report(path, (300,), 90.0)
        self.assertTrue(report["passed"])
        self.assertEqual(report["foreign_processes"], [])
        self.assertEqual(report["related_processes_at_or_above_threshold"], [])
        self.assertEqual(report["threshold_percent_cpu"], 90.0)

    def test_mcp_roots_include_owned_processes(self):
        roots = REPRODUCE.mcp_related_root_pids({
            "ownership": {
                "candidate_owned_active": [{"pid": 11}],
                "foreground_client_pid": 10,
            },
        })
        self.assertEqual(roots, [10, 11])

    def test_mcp_row_requires_retrieval(self):
        with self.assertRaisesRegex(RuntimeError, "MCP arm skipped retrieval"):
            REPRODUCE.require_mcp_retrieval({"arm": "candidate-mcp"}, {})
        REPRODUCE.require_mcp_retrieval(
            {"arm": "candidate-mcp"}, {"retrieval_complete": True})
        REPRODUCE.require_mcp_retrieval({"arm": "shell"}, {})

    def test_admission_rejection_records_row_and_raises(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary)
            row_output = output / "row-01"
            row_output.mkdir()
            rows = []
            with self.assertRaisesRegex(
                    REPRODUCE.AdmissionRejected, "contention admission rejected"):
                with redirect_stdout(io.StringIO()):
                    REPRODUCE.reject_admission(
                        {"arm": "shell", "run": "row-01"}, row_output, output, rows)
            self.assertEqual(rows[0]["state"], "admission_rejected")
            self.assertEqual(json.loads((row_output / "summary.json").read_text()), rows[0])
            self.assertEqual(
                json.loads((output / "results.jsonl").read_text()), rows[0])

    def test_finalization_state_distinguishes_every_failure_class(self):
        class FatalMonitor(RuntimeError):
            pass

        self.assertEqual(
            REPRODUCE.finalization_state(None, [], True, FatalMonitor), "completed")
        self.assertEqual(
            REPRODUCE.finalization_state(None, [], False, FatalMonitor), "row_failure")
        self.assertEqual(REPRODUCE.finalization_state(
            REPRODUCE.AdmissionRejected(), [], False, FatalMonitor),
            "admission_rejected")
        self.assertEqual(REPRODUCE.finalization_state(
            FatalMonitor(), [], False, FatalMonitor), "fatal_codexmon_survivor")
        self.assertEqual(REPRODUCE.finalization_state(
            REPRODUCE.AdmissionRejected(), ["cleanup"], False, FatalMonitor),
            "admission_rejected")
        with self.assertRaisesRegex(RuntimeError, "row_failure.*row-17.*model failed"):
            REPRODUCE.require_completed_run(
                "row_failure", None, [], FatalMonitor,
                {"error": "model failed", "run": "row-17", "state": "controller_failure"})
        with self.assertRaisesRegex(
                RuntimeError, "cleanup failed.*row-17.*model failed"):
            REPRODUCE.require_completed_run(
                "cleanup_failure", None, ["cleanup failed"], FatalMonitor,
                {"error": "model failed", "run": "row-17", "state": "row_failure"})
        REPRODUCE.require_completed_run("completed", None, [], FatalMonitor)

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
        setup = {
            "backends": {
                "candidate": {
                    "binary_sha256": "candidate-observed",
                    "binary_sha256_matches_published": False,
                    "published_binary_sha256": "candidate-reference",
                },
                "upstream": {
                    "binary_sha256": "upstream-observed",
                    "binary_sha256_matches_published": True,
                    "published_binary_sha256": "upstream-reference",
                },
            },
            "protocol": str(protocol),
        }
        return setup, output, rows

    def test_analysis_requires_complete_rows_and_minimum_exact_pairs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            setup, output, rows = self.make_run(root)
            report = REPRODUCE.analyze_data(self.config, setup, output)
            self.assertTrue(report["passed"])
            self.assertFalse(
                report["binaries"]["candidate"]["binary_sha256_matches_published"])
            rendered = REPRODUCE.markdown_results(report)
            self.assertIn("candidate-observed", rendered)
            self.assertIn("candidate-reference", rendered)
            self.assertIn("| false |", rendered)
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
