#!/usr/bin/env python3
"""Prepare, run, analyze, and verify the public complete-task benchmark."""

import argparse
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time


sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parents[2]
V743 = (REPOSITORY_ROOT / "docs/benchmarks/2026-09-09-uncontended-timing-01/"
        "scale-task-context-21/upstream-comparison-v743")
V743_HARNESS = V743 / "frozen/harness"
sys.path.insert(0, str(V743_HARNESS))
BOUND = 64 * 1024 * 1024
TRI = None
BASE = None
SOURCE_ORACLE = None


class AdmissionRejected(RuntimeError):
    """The host failed the pre-row uncontended admission gate."""


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load frozen module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HARNESS_PATHS = {
    "base_runner": V743_HARNESS / "base-run-v743.py",
    "cache_integrity": V743_HARNESS / "cache_integrity_v743.py",
    "candidate_lifecycle": V743_HARNESS / "candidate_lifecycle_v743.py",
    "source_oracle": V743_HARNESS / "source_oracle.py",
    "triarm_adapter": V743_HARNESS / "triarm_run_v743.py",
}


def sha256(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_new(path, value):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def append_json(path, value):
    with pathlib.Path(path).open("a") as stream:
        stream.write(json.dumps(value, sort_keys=True) + "\n")


def binary_hash_receipt(actual, reference):
    return {
        "binary_sha256": actual,
        "binary_sha256_matches_published": (
            actual == reference if reference is not None else None),
        "published_binary_sha256": reference,
    }


def binary_report(setup):
    return {
        name: {
            "binary_sha256": item["binary_sha256"],
            "binary_sha256_matches_published":
                item.get("binary_sha256_matches_published"),
            "published_binary_sha256": item.get("published_binary_sha256"),
        }
        for name, item in sorted(setup["backends"].items())
    }


def resolve_config_path(config_path, value):
    path = pathlib.Path(value)
    return path if path.is_absolute() else (config_path.parent / path).resolve()


def activate_harness(config_path, config):
    global BASE, SOURCE_ORACLE, TRI
    for name, expected in HARNESS_PATHS.items():
        item = config["harness"][name]
        path = resolve_config_path(config_path, item["path"])
        if path != expected.resolve() or not path.is_file():
            raise ValueError(f"frozen harness path differs for {name}")
        if sha256(path) != item["sha256"]:
            raise ValueError(f"frozen harness hash differs for {name}")
    TRI = load_module("public_triarm_v743", HARNESS_PATHS["triarm_adapter"])
    BASE = TRI.base
    SOURCE_ORACLE = load_module("public_source_oracle", HARNESS_PATHS["source_oracle"])


def load_config(path):
    path = pathlib.Path(path).resolve(strict=True)
    config = json.loads(path.read_text())
    if config.get("schema") != "code-cortex-complete-task-reproduction-v1":
        raise ValueError("unsupported configuration schema")
    if config.get("execution", {}).get("serial") is not True:
        raise ValueError("the benchmark requires serial execution")
    if config.get("analysis", {}).get("retry_failed_rows") is not False:
        raise ValueError("the benchmark forbids row retries")
    if config.get("analysis", {}).get("impute_missing_times") is not False:
        raise ValueError("the benchmark forbids time imputation")
    if config.get("analysis", {}).get("exclude_incorrect_pairs") is not True:
        raise ValueError("the benchmark requires exact-answer pair filtering")
    if set(config.get("backends", {})) != {"candidate", "upstream"}:
        raise ValueError("configuration must define candidate and upstream")
    if set(config.get("harness", {})) != set(HARNESS_PATHS):
        raise ValueError("configuration must identify every frozen harness file")
    runner = config.get("runner", {})
    runner_path = resolve_config_path(path, runner.get("path", ""))
    if (runner_path != pathlib.Path(__file__).resolve() or not runner_path.is_file() or
            sha256(runner_path) != runner.get("sha256")):
        raise ValueError("runner path or hash differs from the pinned configuration")
    fixed = {
        "context_budget_bytes": 6000,
        "timeout_seconds": 120,
    }
    if {key: config.get("retrieval", {}).get(key) for key in fixed} != fixed:
        raise ValueError("retrieval limits differ from the frozen v743 adapter")
    if config.get("model", {}).get("session_timeout_seconds") != 300:
        raise ValueError("session timeout differs from the frozen v743 runner")
    thresholds = config.get("model", {}).get("codexmon_thresholds_seconds")
    if thresholds != {"heartbeat": 10, "idle": 120, "slow": 30,
                      "stalled": 120, "tool": 90, "wall": 300}:
        raise ValueError("codexmon thresholds differ from the frozen v743 runner")
    execution_contract = {
        "foreign_process_cpu_percent": 50.0,
        "max_load_1m": 8.0,
        "sample_count": 3,
        "sample_interval_seconds": 1.0,
        "systemd_scope_unit_pattern": "cbm-v743-run-<32 lowercase hex>.service",
    }
    if {key: config["execution"].get(key) for key in execution_contract} != execution_contract:
        raise ValueError("execution controls differ from the frozen v743 runner")
    expected_retrieval = {
        "candidate": {
            "chain": {"depth": 3, "direction": "inbound", "max_bytes": 6000,
                      "max_work": 128, "mode": "calls", "source_context": 1,
                      "tool": "trace_path"},
            "lookup": {"callees_limit": 0, "callers_limit": 0, "max_bytes": 4000,
                       "source_lines": 2, "tool": "inspect_symbol"},
        },
        "upstream": {
            "chain": {"format": "json", "max_rows": 2, "tool": "query_graph"},
            "lookup": {"format": "json", "label": "Function", "limit": 2,
                       "max_output_tokens": 128, "tool": "search_graph"},
        },
    }
    observed_retrieval = {key: config["retrieval"].get(key)
                          for key in ("candidate", "upstream")}
    if observed_retrieval != expected_retrieval:
        raise ValueError("retrieval arguments differ from the frozen v743 adapter")
    activate_harness(path, config)
    return path, config


def existing_parent(path):
    path = pathlib.Path(path).resolve()
    while not path.exists():
        if path.parent == path:
            raise FileNotFoundError("no existing parent for work path")
        path = path.parent
    return path


def command(argv, output, stem, *, cwd=None, env=None, timeout=30, check=True):
    output = pathlib.Path(output)
    output.mkdir(parents=True, exist_ok=True)
    save_new(output / f"{stem}.argv.json", [str(value) for value in argv])
    started = time.monotonic()
    try:
        result = subprocess.run(
            [str(value) for value in argv], cwd=cwd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
            check=False)
        returncode, stdout, stderr, error = (
            result.returncode, result.stdout, result.stderr, None)
    except subprocess.TimeoutExpired as exc:
        returncode, stdout, stderr, error = (
            None, exc.stdout or b"", exc.stderr or b"", "timeout")
    if len(stdout) > BOUND or len(stderr) > BOUND:
        raise RuntimeError(f"{stem} output exceeds {BOUND} bytes")
    (output / f"{stem}.stdout").write_bytes(stdout)
    (output / f"{stem}.stderr").write_bytes(stderr)
    save_new(output / f"{stem}.result.json", {
        "error": error,
        "returncode": returncode,
        "wall_seconds": time.monotonic() - started,
    })
    if check and returncode != 0:
        raise RuntimeError(f"{stem} failed with return code {returncode}")
    return stdout, returncode


def clean_environment(extra=None):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CBM_", "CODEX_"))}
    if extra:
        env.update({key: str(value) for key, value in extra.items()})
    return env


def memory_gib():
    path = pathlib.Path("/proc/meminfo")
    if not path.is_file():
        return 0.0
    match = re.search(r"^MemTotal:\s+(\d+)\s+kB$", path.read_text(), re.MULTILINE)
    return int(match.group(1)) / 1024**2 if match else 0.0


def cpu_model():
    path = pathlib.Path("/proc/cpuinfo")
    if not path.is_file():
        return ""
    match = re.search(r"^model name\s*:\s*(.+)$", path.read_text(), re.MULTILINE)
    return match.group(1).strip() if match else ""


def os_release():
    path = pathlib.Path("/etc/os-release")
    values = {}
    if path.is_file():
        for line in path.read_text().splitlines():
            if "=" not in line or line.startswith("#"):
                continue
            key, value = line.split("=", 1)
            values[key] = value.strip().strip('"')
    return {"id": values.get("ID", ""), "version_id": values.get("VERSION_ID", "")}


def probe_output(argv):
    try:
        probe = subprocess.run(argv, capture_output=True, text=True, timeout=15,
                               check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"{type(exc).__name__}: {exc}"
    return (probe.stdout + probe.stderr).strip()


def development_headers_present(compiler):
    if not compiler:
        return False
    probe = subprocess.run(
        [compiler, "-E", "-x", "c", "-"],
        input="#include <zlib.h>\n#include <lz4.h>\n",
        capture_output=True, text=True, timeout=30, check=False)
    return probe.returncode == 0


def harness_receipt(config_path, config):
    rows = {}
    for name, item in sorted(config["harness"].items()):
        path = resolve_config_path(config_path, item["path"])
        actual = sha256(path) if path.is_file() else None
        rows[name] = {
            "path": str(path),
            "sha256": actual,
            "sha256_expected": item["sha256"],
            "sha256_matches": actual == item["sha256"],
        }
    return rows


def fixture_receipt(config_path, config):
    rows = {}
    for name in ("generator", "protocol_schedule", "protocol_tasks"):
        path = resolve_config_path(config_path, config["fixtures"][name])
        actual = sha256(path) if path.is_file() else None
        expected = config["fixtures"]["sha256"][name]
        rows[name] = {
            "path": str(path), "sha256": actual, "sha256_expected": expected,
            "sha256_matches": actual == expected,
        }
    return rows


def verify_fixture_inputs(config_path, config, names=None):
    receipts = fixture_receipt(config_path, config)
    selected = receipts if names is None else {name: receipts[name] for name in names}
    if not all(item["sha256_matches"] for item in selected.values()):
        raise ValueError("frozen fixture or protocol input changed")
    return selected


def published_evidence_receipt(config_path, config):
    published = config["published_comparison"]
    root = resolve_config_path(config_path, published["evidence"])
    manifest = root / "SHA256SUMS"
    metrics = root / "METRICS.json"
    return {
        "manifest": str(manifest),
        "manifest_sha256": sha256(manifest) if manifest.is_file() else None,
        "manifest_sha256_expected": published["evidence_manifest_sha256"],
        "metrics": str(metrics),
        "metrics_sha256": sha256(metrics) if metrics.is_file() else None,
        "metrics_sha256_expected": published["metrics_sha256"],
        "root": str(root),
    }


def executable_receipt(path, expected_sha256):
    path = pathlib.Path(path).resolve(strict=True)
    metadata = path.stat()
    return {
        "device": metadata.st_dev,
        "inode": metadata.st_ino,
        "mode": metadata.st_mode,
        "path": str(path),
        "sha256": sha256(path),
        "sha256_expected": expected_sha256,
    }


def verify_executable(receipt):
    path = pathlib.Path(receipt["path"])
    if path.is_symlink() or not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"executable identity changed: {path}")
    metadata = path.stat()
    if (metadata.st_dev != receipt["device"] or metadata.st_ino != receipt["inode"] or
            metadata.st_mode != receipt["mode"] or sha256(path) != receipt["sha256"]):
        raise ValueError(f"executable identity changed: {path}")
    return path


def toolchain_receipt(config):
    result = {}
    for name, expected in sorted(config["toolchain"].items()):
        found = shutil.which(expected["executable"])
        if not found:
            result[name] = {"path": None, "passed": False}
            continue
        receipt = executable_receipt(found, expected["sha256"])
        version = probe_output([receipt["path"], "--version"])
        receipt.update({
            "executable": expected["executable"],
            "passed": (receipt["sha256"] == expected["sha256"] and
                       expected["version_contains"] in version),
            "version": version,
            "version_expected": expected["version_contains"],
        })
        result[name] = receipt
    return result


def verify_toolchain(receipts):
    for receipt in receipts.values():
        if not receipt.get("path"):
            raise ValueError("toolchain executable is missing")
        if pathlib.Path(shutil.which(receipt["executable"]) or "").resolve() != \
                pathlib.Path(receipt["path"]):
            raise ValueError(f'toolchain PATH changed: {receipt["executable"]}')
        verify_executable(receipt)


def doctor(config_path, config, work, allow_version_drift=False):
    required = ("bash", "c++", "cc", "cmake", "git", "make", "ps", "python3",
                "systemctl", "systemd-run", "time")
    tools = {name: shutil.which(name) for name in required}
    codex_found = shutil.which(config["model"]["codex_executable"])
    codexmon_found = shutil.which(config["model"]["codexmon_executable"])
    codex = str(pathlib.Path(codex_found).resolve()) if codex_found else None
    codexmon = str(pathlib.Path(codexmon_found).resolve()) if codexmon_found else None
    version = probe_output([codex, "--version"]) if codex else ""
    codexmon_help = probe_output([codexmon, "--help"]) if codexmon else ""
    expected_version = config["model"]["codex_version"]
    version_matches = expected_version in version
    codex_hash = sha256(codex) if codex else None
    codexmon_hash = sha256(codexmon) if codexmon else None
    codex_hash_matches = codex_hash == config["model"]["codex_sha256"]
    codexmon_hash_matches = codexmon_hash == config["model"]["codexmon_sha256"]
    parent = existing_parent(work)
    disk_gib = shutil.disk_usage(parent).free / 1024**3
    cpu_count = os.cpu_count() or 0
    repository_commit = probe_output(
        [tools["git"], "-C", str(REPOSITORY_ROOT), "rev-parse", "HEAD"]) \
        if tools["git"] else ""
    repository_status = probe_output(
        [tools["git"], "-C", str(REPOSITORY_ROOT), "status", "--short"]) \
        if tools["git"] else ""
    distribution = os_release()
    report = {
        "authentication_file": str(pathlib.Path.home() / ".codex/auth.json"),
        "authentication_file_present": (pathlib.Path.home() / ".codex/auth.json").is_file(),
        "codex": codex,
        "codex_sha256": codex_hash,
        "codex_sha256_matches": codex_hash_matches,
        "codex_version": version,
        "codex_version_expected": expected_version,
        "codex_version_matches": version_matches,
        "codexmon": codexmon,
        "codexmon_version": codexmon_help.splitlines()[0] if codexmon_help else "",
        "codexmon_version_expected": config["model"]["codexmon_version"],
        "codexmon_version_matches": config["model"]["codexmon_version"] in codexmon_help,
        "codexmon_sha256": codexmon_hash,
        "codexmon_sha256_matches": codexmon_hash_matches,
        "configuration": str(config_path),
        "configuration_sha256": sha256(config_path),
        "free_disk_gib": disk_gib,
        "logical_cpus": cpu_count,
        "cpu_model": cpu_model(),
        "memory_gib": memory_gib(),
        "machine": platform.machine(),
        "kernel": platform.release(),
        "operating_system": platform.system(),
        "distribution": distribution,
        "reproduction_repository_commit": repository_commit,
        "reproduction_repository_status": repository_status.splitlines(),
        "runner": str(pathlib.Path(__file__).resolve()),
        "runner_sha256": sha256(pathlib.Path(__file__).resolve()),
        "tools": tools,
        "tool_versions": {
            "c++": probe_output([tools["c++"], "--version"]) if tools["c++"] else "",
            "cc": probe_output([tools["cc"], "--version"]) if tools["cc"] else "",
            "cmake": probe_output([tools["cmake"], "--version"]) if tools["cmake"] else "",
            "git": probe_output([tools["git"], "--version"]) if tools["git"] else "",
            "python3": probe_output([tools["python3"], "--version"])
            if tools["python3"] else "",
            "time": probe_output([tools["time"], "--version"]) if tools["time"] else "",
        },
        "work_parent": str(parent),
    }
    report["fixtures"] = fixture_receipt(config_path, config)
    report["toolchain"] = toolchain_receipt(config)
    report["published_evidence"] = published_evidence_receipt(config_path, config)
    try:
        report["published_evidence"]["files_checked"] = verify_hash_manifest(
            report["published_evidence"]["root"], closed=False)
        report["published_evidence"]["files_valid"] = True
    except (OSError, ValueError) as exc:
        report["published_evidence"]["files_valid"] = False
        report["published_evidence"]["verification_error"] = f"{type(exc).__name__}: {exc}"
    report["harness"] = harness_receipt(config_path, config)
    report["development_headers_present"] = development_headers_present(tools["cc"])
    time_verbose = False
    if tools["time"] and tools["python3"]:
        time_verbose = subprocess.run(
            [tools["time"], "-v", tools["python3"], "-c", "pass"],
            capture_output=True, timeout=15, check=False).returncode == 0
    report["gnu_time_verbose_present"] = time_verbose
    systemd_user = False
    if tools["systemctl"]:
        systemd_user = subprocess.run(
            [tools["systemctl"], "--user", "show-environment"], capture_output=True,
            timeout=15, check=False).returncode == 0
    report["systemd_user_present"] = systemd_user
    limits = config["host"]
    checks = {
        "authentication": report["authentication_file_present"],
        "codex": bool(codex),
        "codex_identity": codex_hash_matches or allow_version_drift,
        "codex_version": version_matches or allow_version_drift,
        "codexmon": bool(codexmon),
        "codexmon_identity": codexmon_hash_matches or allow_version_drift,
        "codexmon_version": report["codexmon_version_matches"] or allow_version_drift,
        "cpu": cpu_count >= limits["minimum_logical_cpus"],
        "development_headers": report["development_headers_present"],
        "disk": disk_gib >= limits["minimum_free_disk_gib"],
        "fixtures": all(item["sha256_matches"] for item in report["fixtures"].values()),
        "gnu_time": report["gnu_time_verbose_present"],
        "harness": all(item["sha256_matches"] for item in report["harness"].values()),
        "machine": platform.machine() == config["model"]["architecture"].split("-", 1)[0],
        "memory": report["memory_gib"] >= limits["minimum_memory_gib"],
        "operating_system": platform.system() == limits["operating_system"],
        "distribution": distribution == limits["distribution"],
        "published_evidence": (
            report["published_evidence"]["manifest_sha256"] ==
            report["published_evidence"]["manifest_sha256_expected"] and
            report["published_evidence"]["metrics_sha256"] ==
            report["published_evidence"]["metrics_sha256_expected"] and
            report["published_evidence"]["files_valid"]),
        "systemd_user": report["systemd_user_present"],
        "toolchain": (all(item.get("passed") for item in report["toolchain"].values()) or
                      allow_version_drift),
        "tools": all(tools.values()),
    }
    report["checks"] = checks
    report["passed"] = all(checks.values())
    return report


def unwrap(raw):
    return BASE.unwrap(raw)


def clone_and_build(name, backend, runtime, evidence, timeout, toolchain):
    repository = runtime / "repositories" / name
    command(["git", "clone", "--no-checkout", backend["repository"], repository],
            evidence, f"clone-{name}", timeout=timeout)
    command(["git", "checkout", "--detach", backend["revision"]], evidence,
            f"checkout-{name}", cwd=repository, timeout=300)
    head_raw, _ = command(["git", "rev-parse", "HEAD"], evidence,
                          f"head-{name}", cwd=repository)
    head = head_raw.decode().strip()
    if head != backend["revision"]:
        raise ValueError(f"{name} checkout does not match the pinned revision")
    verify_toolchain(toolchain)
    build_environment = dict(backend.get("build_environment", {}))
    build_environment.update({
        "CC": toolchain["c_compiler"]["path"],
        "CXX": toolchain["cxx_compiler"]["path"],
    })
    command([*backend["build"], *backend.get("build_arguments", [])], evidence,
            f"build-{name}", cwd=repository,
            env=clean_environment(build_environment), timeout=timeout)
    verify_toolchain(toolchain)
    binary = (repository / backend["binary"]).resolve(strict=True)
    binary_hash = sha256(binary)
    return {
        "binary": str(binary),
        **binary_hash_receipt(binary_hash, backend.get("published_binary_sha256")),
        "commit": head,
        "repository": str(repository),
        "source_repository": backend["repository"],
    }


def verify_source(root, manifest, expected_lines):
    aggregate = hashlib.sha256()
    files = 0
    lines = 0
    total_bytes = 0
    failures = []
    for encoded in pathlib.Path(manifest).read_bytes().splitlines(keepends=True):
        aggregate.update(encoded)
        record = json.loads(encoded)
        path = pathlib.Path(root) / record["file"]
        if (not path.is_file() or path.stat().st_size != record["bytes"] or
                sha256(path) != record["sha256"]):
            failures.append(record["file"])
            if len(failures) >= 20:
                break
        files += 1
        lines += record["lines"]
        total_bytes += record["bytes"]
    actual = sum(1 for _ in pathlib.Path(root).glob("part_*/*.cpp"))
    passed = not failures and files == actual and lines == expected_lines
    return {
        "actual_files": actual,
        "code_lines": lines,
        "failures": failures,
        "files": files,
        "manifest_sha256": aggregate.hexdigest(),
        "passed": passed,
        "source_bytes": total_bytes,
    }


def generate_sources(config_path, config, runtime, setup_output):
    verify_fixture_inputs(config_path, config, ("generator",))
    generator_source = resolve_config_path(config_path, config["fixtures"]["generator"])
    generator = setup_output / "fixture-generator.py"
    shutil.copy2(generator_source, generator)
    if sha256(generator) != config["fixtures"]["sha256"]["generator"]:
        raise ValueError("copied fixture generator hash changed")
    projects = {}
    for scale, lines in config["fixtures"]["scales"].items():
        root = runtime / "sources" / scale
        manifest = setup_output / "source-manifests" / f"{scale}.jsonl"
        phase = setup_output / "generation" / scale
        command([sys.executable, generator, "--root", root, "--lines", lines,
                 "--manifest", manifest], phase, "generate",
                timeout=config["execution"]["generation_timeout_seconds"])
        audit = verify_source(root, manifest, lines)
        if not audit["passed"]:
            raise ValueError(f"source verification failed for {scale}")
        projects[scale] = {
            "code_lines": lines,
            "manifest": str(manifest),
            "repo_root": str(root),
            "scale": scale,
            "source_audit": audit,
        }
    return projects


def backend_environment(item, configured):
    env = clean_environment(configured.get("environment"))
    env.update({
        "CBM_CACHE_DIR": item["cache"],
        "HOME": item["home"],
        "TMPDIR": item["tmp"],
        "XDG_CACHE_HOME": str(pathlib.Path(item["home"]) / ".cache"),
    })
    return env


def index_backend(name, item, configured, sources, setup_output, timeout):
    projects = {}
    env = backend_environment(item, configured)
    for scale in configured["scales"]:
        source = sources[scale]
        project_name = f"complete-task-reproduction-{name}-{scale}"
        phase = setup_output / "index" / name / scale
        args = {
            "mode": "fast",
            "name": project_name,
            "persistence": False,
            "repo_path": source["repo_root"],
        }
        args_file = phase / "index.args.json"
        save_new(args_file, args)
        raw, _ = command(
            [shutil.which("time"), "-v", item["binary"], "cli", "--json",
             "index_repository", "--args-file", args_file],
            phase, "index", env=env, timeout=timeout)
        body = unwrap(raw)
        if body.get("status") != "indexed" or body.get("project") != project_name:
            raise ValueError(f"{name} index result mismatch for {scale}")
        if verify_source(source["repo_root"], source["manifest"],
                         source["code_lines"]) != source["source_audit"]:
            raise ValueError(f"{name} changed the {scale} source")
        projects[scale] = {**source, "project": project_name}
    return projects


def stop_backend(name, item, output, label, roots=()):
    if name == "candidate":
        return TRI.candidate_zero(item, output, label, roots)
    return TRI.backend_cleanup(name, item, output, label, roots)


def retrieval_preflight(config, setup, tasks, output):
    results = {}
    for name in ("candidate", "upstream"):
        item = setup["backends"][name]
        selected = [task for task in tasks if task["scale"] in item["projects"]]
        backend_output = output / name
        backend_output.mkdir(parents=True)
        client = None
        root_pid = None
        try:
            client, _ = TRI.open_row(name, setup, backend_output)
            root_pid = client.proc.pid
            records = []
            for task in selected:
                task_output = backend_output / task["id"]
                task_output.mkdir()
                context, receipt, _ = TRI.retrieve_then_scan(
                    task, item["projects"][task["scale"]], client, name, item,
                    task_output, root_pid)
                records.append({
                    "context_bytes": len(context.encode()),
                    "context_sha256": hashlib.sha256(context.encode()).hexdigest(),
                    "receipt": receipt,
                    "task": task["id"],
                })
            results[name] = records
        finally:
            if client is not None:
                client.close()
            stop_backend(name, item, backend_output, f"cleanup-{name}",
                         (root_pid,) if root_pid else ())
    candidate = {row["task"]: row for row in results["candidate"]}
    upstream = {row["task"]: row for row in results["upstream"]}
    shared = []
    for task_id, upstream_row in sorted(upstream.items()):
        candidate_row = candidate.get(task_id)
        if not candidate_row:
            raise ValueError(f"candidate context is missing for {task_id}")
        if (candidate_row["context_sha256"] != upstream_row["context_sha256"] or
                candidate_row["context_bytes"] != upstream_row["context_bytes"]):
            raise ValueError(f"normalized MCP contexts differ for {task_id}")
        shared.append(task_id)
    report = {
        "candidate_tasks": len(candidate),
        "passed": len(candidate) == 24 and len(upstream) == 16,
        "shared_context_tasks": shared,
        "upstream_tasks": len(upstream),
    }
    save_new(output / "report.json", report)
    if not report["passed"]:
        raise ValueError("retrieval preflight task count failed")
    return report


def source_oracle_report(tasks, sources):
    rows = [SOURCE_ORACLE.verify_task(task, pathlib.Path(sources[task["scale"]]["repo_root"]))
            for task in tasks]
    return {"passed": all(row["passed"] for row in rows), "results": rows,
            "tasks": len(rows)}


def write_hash_manifest(root):
    root = pathlib.Path(root)
    target = root / "SHA256SUMS"
    if target.exists():
        raise FileExistsError(f"hash manifest already exists: {target}")
    rows = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file() or path == target:
            continue
        rows.append(f"{sha256(path)}  {path.relative_to(root)}")
    target.write_text("\n".join(rows) + "\n")
    return target


def verify_hash_manifest(root, closed=True):
    root = pathlib.Path(root)
    manifest = root / "SHA256SUMS"
    if not manifest.is_file():
        raise FileNotFoundError(f"missing {manifest}")
    checked = 0
    expected_files = set()
    for line in manifest.read_text().splitlines():
        expected, relative = line.split("  ", 1)
        relative_path = pathlib.PurePosixPath(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise ValueError(f"unsafe manifest path: {relative}")
        if relative in expected_files:
            raise ValueError(f"duplicate manifest path: {relative}")
        expected_files.add(relative)
        path = root / relative
        if not path.is_file() or path.is_symlink() or sha256(path) != expected:
            raise ValueError(f"hash mismatch: {path}")
        checked += 1
    actual_files = {
        str(path.relative_to(root))
        for path in root.rglob("*")
        if path != manifest and (path.is_file() or path.is_symlink())
    }
    if closed and actual_files != expected_files:
        raise ValueError("hash manifest does not describe the closed file tree")
    return checked


def prepare(config_path, config, work, allow_version_drift=False):
    if allow_version_drift:
        raise ValueError("version drift is doctor-only; update a copied configuration")
    work = pathlib.Path(work).resolve()
    if work.exists():
        raise FileExistsError("work path must not exist")
    report = doctor(config_path, config, work, allow_version_drift)
    if not report["passed"]:
        raise RuntimeError("doctor checks failed: " + json.dumps(report["checks"], sort_keys=True))
    work.mkdir(parents=True)
    runtime = work / "runtime"
    setup_output = work / "setup"
    runtime.mkdir()
    setup_output.mkdir()
    save_new(setup_output / "host.json", report)
    shutil.copy2(config_path, setup_output / "config.json")
    evidence = setup_output / "build"
    backends = {}
    for name in ("candidate", "upstream"):
        configured = config["backends"][name]
        item = clone_and_build(name, configured, runtime, evidence,
                               config["execution"]["build_timeout_seconds"],
                               report["toolchain"])
        backend_root = runtime / "backends" / name
        for child in ("cache", "home", "tmp"):
            (backend_root / child).mkdir(parents=True, exist_ok=True)
        item.update({
            "cache": str(backend_root / "cache"),
            "enforce_exact_topology": name == "candidate",
            "home": str(backend_root / "home"),
            "tmp": str(backend_root / "tmp"),
            "update_check_disabled": name == "candidate",
        })
        backends[name] = item
    sources = generate_sources(config_path, config, runtime, setup_output)
    for name in ("candidate", "upstream"):
        backends[name]["projects"] = index_backend(
            name, backends[name], config["backends"][name], sources,
            setup_output, config["execution"]["index_timeout_seconds"])
    tasks_path = resolve_config_path(config_path, config["fixtures"]["protocol_tasks"])
    schedule_path = resolve_config_path(config_path, config["fixtures"]["protocol_schedule"])
    verify_fixture_inputs(config_path, config, ("protocol_tasks", "protocol_schedule"))
    protocol_output = setup_output / "protocol"
    protocol_output.mkdir()
    shutil.copy2(tasks_path, protocol_output / "tasks.json")
    shutil.copy2(schedule_path, protocol_output / "schedule.json")
    if (sha256(protocol_output / "tasks.json") != config["fixtures"]["sha256"]["protocol_tasks"] or
            sha256(protocol_output / "schedule.json") !=
            config["fixtures"]["sha256"]["protocol_schedule"]):
        raise ValueError("copied frozen protocol hash changed")
    tasks = json.loads((protocol_output / "tasks.json").read_text())
    schedule = json.loads((protocol_output / "schedule.json").read_text())
    if len(tasks) != 24 or len(schedule.get("rows", [])) != 64:
        raise ValueError("frozen protocol task or row count changed")
    if ({row["model"] for row in schedule["rows"]} != {config["model"]["name"]} or
            {row["effort"] for row in schedule["rows"]} != {config["model"]["effort"]}):
        raise ValueError("model configuration differs from the frozen schedule")
    oracle = source_oracle_report(tasks, sources)
    save_new(setup_output / "source-oracle.json", oracle)
    if not oracle["passed"]:
        raise ValueError("source oracle failed")
    session_home = runtime / "codex-home"
    session_home.mkdir()
    setup = {
        "backends": backends,
        "config": str(setup_output / "config.json"),
        "config_sha256": sha256(setup_output / "config.json"),
        "home": str(session_home),
        "executables": {
            "codex": executable_receipt(report["codex"], config["model"]["codex_sha256"]),
            "codexmon": executable_receipt(
                report["codexmon"], config["model"]["codexmon_sha256"]),
        },
        "model": config["model"],
        "protocol": str(protocol_output),
        "protocol_hashes": {
            "schedule.json": sha256(protocol_output / "schedule.json"),
            "tasks.json": sha256(protocol_output / "tasks.json"),
        },
        "sources": sources,
        "state": "preflight",
        "toolchain": report["toolchain"],
    }
    preflight = retrieval_preflight(config, setup, tasks, setup_output / "retrieval-preflight")
    setup["retrieval_preflight"] = preflight
    cache_seal = setup_output / "runtime-cache-manifest.json"
    TRI.cache_integrity.write(setup, cache_seal)
    setup["runtime_cache_seal"] = {"path": str(cache_seal), "sha256": sha256(cache_seal)}
    setup["state"] = "ready"
    save_new(setup_output / "setup.json", setup)
    manifest = write_hash_manifest(setup_output)
    ready_path = work / "SETUP-READY.json"
    save_new(ready_path, {
        "manifest": str(manifest),
        "manifest_sha256": sha256(manifest),
        "setup": str(setup_output / "setup.json"),
        "setup_sha256": sha256(setup_output / "setup.json"),
        "state": "ready",
    })
    print(json.dumps({"setup": str(setup_output / "setup.json"),
                      "setup_ready_sha256": sha256(ready_path), "state": "ready"}))


def verify_setup(config_path, config, work, setup_ready_sha256, immutable_only=False):
    setup_output = pathlib.Path(work) / "setup"
    ready_path = pathlib.Path(work) / "SETUP-READY.json"
    if not re.fullmatch(r"[0-9a-f]{64}", setup_ready_sha256 or ""):
        raise ValueError("an externally recorded setup READY SHA-256 is required")
    if sha256(ready_path) != setup_ready_sha256:
        raise ValueError("external setup READY SHA-256 does not match")
    ready = json.loads(ready_path.read_text())
    if ready.get("state") != "ready":
        raise ValueError("detached setup receipt is not ready")
    if sha256(ready["manifest"]) != ready["manifest_sha256"]:
        raise ValueError("detached setup manifest seal changed")
    if sha256(ready["setup"]) != ready["setup_sha256"]:
        raise ValueError("detached setup receipt seal changed")
    verify_hash_manifest(setup_output)
    setup = json.loads((setup_output / "setup.json").read_text())
    if setup.get("state") != "ready":
        raise ValueError("setup is not ready")
    if sha256(setup["config"]) != setup["config_sha256"]:
        raise ValueError("prepared configuration changed")
    if sha256(config_path) != setup["config_sha256"]:
        raise ValueError("selected configuration differs from prepared configuration")
    for name, item in setup["backends"].items():
        if sha256(item["binary"]) != item["binary_sha256"]:
            raise ValueError(f"{name} binary changed")
    for name, digest in setup["protocol_hashes"].items():
        if sha256(pathlib.Path(setup["protocol"]) / name) != digest:
            raise ValueError(f"protocol changed: {name}")
    for scale, source in setup["sources"].items():
        actual = verify_source(source["repo_root"], source["manifest"], source["code_lines"])
        if actual != source["source_audit"]:
            raise ValueError(f"source changed: {scale}")
    if not all(item["sha256_matches"] for item in harness_receipt(config_path, config).values()):
        raise ValueError("frozen harness changed")
    published = published_evidence_receipt(config_path, config)
    if (published["manifest_sha256"] != published["manifest_sha256_expected"] or
            published["metrics_sha256"] != published["metrics_sha256_expected"]):
        raise ValueError("published reference evidence changed")
    verify_hash_manifest(published["root"], closed=False)
    for receipt in setup["executables"].values():
        verify_executable(receipt)
    verify_toolchain(setup["toolchain"])
    seal = setup["runtime_cache_seal"]
    TRI.cache_integrity.verify(setup, seal["path"], seal["sha256"],
                               immutable_only=immutable_only)
    return setup


def parse_processes(sample):
    processes = []
    for line in sample.get("processes", []):
        fields = line.strip().split(None, 5)
        if len(fields) != 6:
            continue
        try:
            processes.append({
                "argv": fields[5], "command": fields[4], "cpu_percent": float(fields[2]),
                "pid": int(fields[0]), "ppid": int(fields[1]), "rss_kib": int(fields[3]),
            })
        except ValueError:
            continue
    return processes


def contention_admission(samples, config):
    threshold = config["execution"]["foreign_process_cpu_percent"]
    foreign = []
    for sample in samples:
        processes = parse_processes(sample)
        related = set(sample.get("related_root_pids", []))
        changed = True
        while changed:
            changed = False
            for process in processes:
                if process["pid"] not in related and process["ppid"] in related:
                    related.add(process["pid"])
                    changed = True
        foreign.extend(process for process in processes
                       if process["cpu_percent"] >= threshold and process["pid"] not in related)
    max_load = max(sample["loadavg"][0] for sample in samples)
    return {
        "admitted": (len(samples) == config["execution"]["sample_count"] and
                     max_load <= config["execution"]["max_load_1m"] and not foreign),
        "foreign_processes": foreign,
        "max_load_1m": max_load,
        "samples": len(samples),
        "thresholds": {
            "foreign_process_cpu_percent": threshold,
            "max_load_1m": config["execution"]["max_load_1m"],
        },
    }


def runtime_contention_report(path, related_root_pids=(), threshold=50.0):
    path = pathlib.Path(path)
    if not path.is_file():
        return {"foreign_processes": [], "passed": False, "samples": 0}
    samples = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    foreign = []
    related_high_cpu = []
    for sample in samples:
        roots = {*sample.get("related_root_pids", ()), *related_root_pids}
        related = descendant_process_pids(sample, roots)
        for process in parse_processes(sample):
            if process["cpu_percent"] < threshold:
                continue
            target = related_high_cpu if process["pid"] in related else foreign
            target.append({**process, "utc": sample.get("utc")})
    return {"foreign_processes": foreign,
            "related_processes_at_or_above_threshold": related_high_cpu,
            "related_root_pids": sorted(set(related_root_pids)),
            "passed": bool(samples) and not foreign, "samples": len(samples),
            "threshold_percent_cpu": threshold}


def require_mcp_retrieval(row, opened):
    if row["arm"] != "shell" and not opened.get("retrieval_complete"):
        raise RuntimeError("MCP arm skipped retrieval")


def descendant_process_pids(sample, roots):
    related = set(roots)
    processes = parse_processes(sample)
    changed = True
    while changed:
        changed = False
        for process in processes:
            if process["pid"] not in related and process["ppid"] in related:
                related.add(process["pid"])
                changed = True
    return related


def mcp_related_root_pids(opened):
    ownership = opened.get("ownership", {})
    roots = {
        value for key, value in ownership.items()
        if key in {"daemon_pid", "foreground_client_pid"} and isinstance(value, int)
    }
    roots.update(item["pid"] for item in ownership.get("candidate_owned_active", [])
                 if isinstance(item.get("pid"), int))
    return sorted(roots)


def finalization_state(primary, errors, schedule_complete, fatal_type):
    if isinstance(primary, fatal_type):
        return "fatal_codexmon_survivor"
    if isinstance(primary, AdmissionRejected):
        return "admission_rejected"
    if primary is not None:
        return "controller_failure"
    if errors:
        return "cleanup_failure"
    return "completed" if schedule_complete else "row_failure"


def require_completed_run(state, primary, errors, fatal_type, last_result=None):
    if state == "completed":
        return
    if isinstance(primary, fatal_type):
        raise primary
    details = "; ".join(errors)
    if primary is not None:
        details = f"{type(primary).__name__}: {primary}" + \
                  (f"; {details}" if details else "")
    if not details:
        details = f"run ended with state: {state}"
    if last_result and last_result.get("terminal_observed") is not True:
        details += f'; row: {last_result.get("run")}; ' \
                   f'row state: {last_result.get("state")}; ' \
                   f'error: {last_result.get("error")}'
    raise RuntimeError(details) from primary


def reject_admission(row, row_output, output, results):
    result = {**row, "answer_correct": False, "state": "admission_rejected",
              "terminal_observed": False}
    save_new(row_output / "summary.json", result)
    results.append(result)
    append_json(output / "results.jsonl", result)
    print(json.dumps(result, sort_keys=True), flush=True)
    raise AdmissionRejected(f'contention admission rejected: {row["run"]}')


def remove_auth_link(setup, row):
    namespace = hashlib.sha256(str(pathlib.Path(setup["run_output"]).resolve()).encode()).hexdigest()[:16]
    path = pathlib.Path(setup["home"]) / "sessions" / namespace / row["run"] / "auth.json"
    if path.is_symlink() or path.is_file():
        path.unlink()


def run_row(task, row, project, setup, output, codex, codexmon):
    original = BASE.graph_context
    opened = {}

    def supplied(*unused):
        backend = {"candidate-mcp": "candidate", "upstream-mcp": "upstream"}[row["arm"]]
        if opened:
            raise RuntimeError("MCP row attempted more than one retrieval")
        item = setup["backends"][backend]
        client, ownership = TRI.open_row(backend, setup, output)
        opened.update({"backend": backend, "client": client,
                       "ownership": ownership, "pid": client.proc.pid})
        context, receipt, _ = TRI.retrieve_then_scan(
            task, item["projects"][task["scale"]], client, backend, item,
            output, client.proc.pid)
        if receipt.get("source_oracle_correct") is not True:
            raise RuntimeError("MCP retrieval did not pass the source oracle")
        opened["retrieval_complete"] = True
        opened["runtime_related_root_pids"] = mcp_related_root_pids(opened)
        return context, receipt

    BASE.graph_context = supplied
    bridged = {**row, "triarm_arm": row["arm"],
               "arm": "mcp-context" if row["arm"] != "shell" else "shell"}
    primary = None
    result = None
    cleanup = []
    try:
        result = BASE.run_session(task, bridged, project, setup, output,
                                  pathlib.Path(codex), pathlib.Path(codexmon), None)
        require_mcp_retrieval(row, opened)
        result["mcp_related_root_pids"] = opened.get("runtime_related_root_pids", [])
        result["arm"] = row["arm"]
        result["triarm_arm"] = row["arm"]
    except BaseException as exc:
        primary = exc
    finally:
        BASE.graph_context = original
        if opened:
            try:
                opened["client"].close()
            except BaseException as exc:
                cleanup.append(f"client close: {type(exc).__name__}: {exc}")
            try:
                stop_backend(opened["backend"], setup["backends"][opened["backend"]],
                             output, f"cleanup-{opened['backend']}",
                             (opened["pid"],))
            except BaseException as exc:
                cleanup.append(f"backend cleanup: {type(exc).__name__}: {exc}")
        try:
            remove_auth_link(setup, row)
        except BaseException as exc:
            cleanup.append(f"auth cleanup: {type(exc).__name__}: {exc}")
    if primary or cleanup:
        if cleanup:
            save_new(output / "mcp-teardown-failure.json", {
                "primary": (f"{type(primary).__name__}: {primary}" if primary else None),
                "teardown_errors": cleanup,
            })
        if isinstance(primary, BASE.FatalCodexmonSurvivor):
            raise primary
        details = "; ".join(cleanup)
        if primary:
            details = f"{type(primary).__name__}: {primary}" + (f"; {details}" if details else "")
        raise RuntimeError(details)
    summary_path = output / "summary.json"
    summary_path.unlink()
    BASE.save_new(summary_path, result)
    return result


def run_stage(config_path, config, work, setup_ready_sha256):
    work = pathlib.Path(work).resolve(strict=True)
    setup = verify_setup(config_path, config, work, setup_ready_sha256)
    output = work / "run"
    if output.exists():
        raise FileExistsError("run output already exists")
    scope = TRI.require_unique_v743_scope("run")
    output.mkdir()
    save_new(output / "runner-scope-receipt.json", scope)
    setup["run_output"] = str(output)
    tasks = {task["id"]: task for task in
             json.loads((pathlib.Path(setup["protocol"]) / "tasks.json").read_text())}
    schedule = json.loads((pathlib.Path(setup["protocol"]) / "schedule.json").read_text())["rows"]
    results = []
    oracle_pre = None
    source_pre = None
    primary = None
    finalization_errors = []
    try:
        oracle_pre = source_oracle_report(list(tasks.values()), setup["sources"])
        save_new(output / "source-oracle-pre.json", oracle_pre)
        if not oracle_pre["passed"]:
            raise ValueError("pre-run source oracle failed")
        source_pre = TRI.source_manifest_audit(setup["sources"])
        save_new(output / "source-manifest-pre.json", source_pre)
        if not source_pre["passed"]:
            raise ValueError("pre-run source manifest audit failed")
        codex = verify_executable(setup["executables"]["codex"])
        codexmon = verify_executable(setup["executables"]["codexmon"])
        TRI.zero(setup, output, "daemon-zero-before-schedule")
        for row in schedule:
            row_output = output / row["run"]
            row_output.mkdir()
            TRI.zero(setup, row_output, "daemon-zero-before-admission")
            samples = []
            for ordinal in range(config["execution"]["sample_count"]):
                samples.append(BASE.snapshot(f"admission-{ordinal}", ()))
                if ordinal + 1 < config["execution"]["sample_count"]:
                    time.sleep(config["execution"]["sample_interval_seconds"])
            admission = contention_admission(samples, config)
            save_new(row_output / "contention-admission-samples.json", samples)
            save_new(row_output / "contention-admission.json", admission)
            if not admission["admitted"]:
                reject_admission(row, row_output, output, results)
            backend = {"candidate-mcp": "candidate",
                       "upstream-mcp": "upstream"}.get(row["arm"])
            project = (setup["backends"][backend]["projects"][tasks[row["task"]]["scale"]]
                       if backend else setup["sources"][tasks[row["task"]]["scale"]])
            try:
                result = run_row(tasks[row["task"]], row, project, setup, row_output,
                                 codex, codexmon)
            except BASE.FatalCodexmonSurvivor as exc:
                fatal = {**row, "accepted_task_wall_seconds": None,
                         "answer_correct": False,
                         "error": f"{type(exc).__name__}: {exc}",
                         "state": "fatal_codexmon_survivor", "terminal_observed": False}
                save_new(row_output / "fatal-survivor.json", fatal)
                raise
            except Exception as exc:
                result = {**row, "answer_correct": False,
                          "error": f"{type(exc).__name__}: {exc}",
                          "state": "controller_failure", "terminal_observed": False}
                summary_path = row_output / "summary.json"
                if summary_path.exists():
                    summary_path.unlink()
                save_new(summary_path, result)
            runtime_contention = runtime_contention_report(
                row_output / "contention-samples.jsonl",
                result.get("mcp_related_root_pids", ()),
                config["execution"]["foreign_process_cpu_percent"])
            save_new(row_output / "contention-runtime.json", runtime_contention)
            result["runtime_uncontended"] = runtime_contention["passed"]
            summary_path = row_output / "summary.json"
            if summary_path.exists():
                summary_path.unlink()
            save_new(summary_path, result)
            save_new(row_output / "contention-post.json", BASE.snapshot("task-post"))
            results.append(result)
            append_json(output / "results.jsonl", result)
            print(json.dumps(result, sort_keys=True), flush=True)
            if not result.get("terminal_observed"):
                break
    except BaseException as exc:
        primary = exc
    finally:
        try:
            oracle_post = source_oracle_report(list(tasks.values()), setup["sources"])
            save_new(output / "source-oracle-post.json", oracle_post)
            if oracle_pre is None or oracle_post != oracle_pre:
                raise ValueError("source oracle changed during the run")
        except BaseException as exc:
            finalization_errors.append(f"source oracle: {type(exc).__name__}: {exc}")
        try:
            source_post = TRI.source_manifest_audit(setup["sources"])
            save_new(output / "source-manifest-post.json", source_post)
            if source_pre is None or source_post != source_pre or not source_post["passed"]:
                raise ValueError("source manifest changed during the run")
        except BaseException as exc:
            finalization_errors.append(f"source manifest: {type(exc).__name__}: {exc}")
        for name in ("candidate", "upstream"):
            try:
                stop_backend(name, setup["backends"][name], output,
                             f"final-backend-cleanup-{name}")
            except BaseException as exc:
                finalization_errors.append(
                    f"{name} cleanup: {type(exc).__name__}: {exc}")
        try:
            TRI.zero(setup, output, "daemon-zero-after-schedule")
        except BaseException as exc:
            finalization_errors.append(f"backend cleanup: {type(exc).__name__}: {exc}")
        try:
            seal = setup["runtime_cache_seal"]
            cache_post = TRI.cache_integrity.verify(
                setup, seal["path"], seal["sha256"], immutable_only=True)
            save_new(output / "cache-integrity-post.json", cache_post)
        except BaseException as exc:
            finalization_errors.append(f"cache integrity: {type(exc).__name__}: {exc}")
        schedule_complete = (len(results) == len(schedule) and
                             all(row.get("terminal_observed") is True for row in results))
        state = finalization_state(
            primary, finalization_errors, schedule_complete,
            BASE.FatalCodexmonSurvivor)
        save_new(output / "run-finalization.json", {
            "attempted": len(results),
            "cleanup_failed": bool(finalization_errors),
            "errors": finalization_errors,
            "passed": state == "completed",
            "primary": f"{type(primary).__name__}: {primary}" if primary else None,
            "schedule_complete": schedule_complete,
            "scheduled": len(schedule),
            "state": state,
        })
    require_completed_run(
        state, primary, finalization_errors, BASE.FatalCodexmonSurvivor,
        results[-1] if results else None)
    save_new(output / "run-report.json", {
        "attempted": len(results),
        "correct": sum(row.get("answer_correct") is True for row in results),
        "scheduled": len(schedule),
        "terminal": sum(row.get("terminal_observed") is True for row in results),
    })
    write_hash_manifest(output)


def geometric(values):
    return math.exp(sum(math.log(value) for value in values) / len(values)) if values else None


def comparison_report(pairs):
    groups = {"all": pairs}
    for field in ("scale", "archetype"):
        for key in sorted({pair[field] for pair in pairs}):
            groups[f"{field}:{key}"] = [pair for pair in pairs if pair[field] == key]
    report = {}
    for name, rows in groups.items():
        eligible = [row for row in rows if row["eligible"]]
        ratio = geometric([row["ratio"] for row in eligible])
        report[name] = {
            "eligible_pairs": len(eligible),
            "geometric_percent_reduction": (1.0 - ratio) * 100 if ratio is not None else None,
            "geometric_ratio": ratio,
            "left_seconds_sum": sum(row["left_seconds"] for row in eligible),
            "planned_pairs": len(rows),
            "right_seconds_sum": sum(row["right_seconds"] for row in eligible),
        }
    return report


def analyze_data(config, setup, run_output):
    tasks = {task["id"]: task for task in
             json.loads((pathlib.Path(setup["protocol"]) / "tasks.json").read_text())}
    schedule = json.loads((pathlib.Path(setup["protocol"]) / "schedule.json").read_text())["rows"]
    pre = json.loads((run_output / "source-oracle-pre.json").read_text())
    post = json.loads((run_output / "source-oracle-post.json").read_text())
    source_manifest_pre = json.loads((run_output / "source-manifest-pre.json").read_text())
    source_manifest_post = json.loads((run_output / "source-manifest-post.json").read_text())
    finalization = json.loads((run_output / "run-finalization.json").read_text())
    source_ok = (pre.get("passed") is True and pre == post and
                 source_manifest_pre.get("passed") is True and
                 source_manifest_pre == source_manifest_post and
                 finalization.get("passed") is True)
    recorded_rows = {}
    results_file = run_output / "results.jsonl"
    if results_file.is_file():
        for encoded in results_file.read_text().splitlines():
            record = json.loads(encoded)
            if record.get("run") in recorded_rows:
                raise ValueError(f'duplicate results row: {record.get("run")}')
            recorded_rows[record.get("run")] = record
    rows = []
    for planned in schedule:
        directory = run_output / planned["run"]
        summary = json.loads((directory / "summary.json").read_text()) \
            if (directory / "summary.json").is_file() else {}
        grade = json.loads((directory / "grade.json").read_text()) \
            if (directory / "grade.json").is_file() else {}
        admission = json.loads((directory / "contention-admission.json").read_text()) \
            if (directory / "contention-admission.json").is_file() else {}
        runtime_contention = json.loads((directory / "contention-runtime.json").read_text()) \
            if (directory / "contention-runtime.json").is_file() else {}
        retrieval = json.loads((directory / "retrieval-receipt.json").read_text()) \
            if (directory / "retrieval-receipt.json").is_file() else {}
        terminal = json.loads((directory / "terminal-status.json").read_text()) \
            if (directory / "terminal-status.json").is_file() else {}
        backend = {"candidate-mcp": "candidate_production_mcp_stdio_jsonrpc",
                   "upstream-mcp": "upstream_production_mcp_stdio_jsonrpc"}.get(
                       planned["arm"], "none")
        eligible = (
            summary.get("state") == "completed" and
            summary.get("task_wall_seconds") is not None and
            grade.get("correct") is True and
            summary.get("answer_correct") is True and
            summary.get("no_session_mcp_calls") is True and
            summary.get("retrieval_backend") == backend and
            retrieval.get("backend") == backend and
            recorded_rows.get(planned["run"]) == summary and
            terminal.get("state") == "completed" and
            terminal.get("exit_code") == 0 and
            (planned["arm"] == "shell" or
             (summary.get("retrieval_context_bytes", 0) > 0 and
              summary.get("source_oracle_correct") is True and
              retrieval.get("source_oracle_correct") is True)) and
            admission.get("admitted") is True and
            runtime_contention.get("passed") is True and
            summary.get("runtime_uncontended") is True and source_ok)
        rows.append({**planned, "admitted": admission.get("admitted") is True,
                     "archetype": tasks[planned["task"]]["archetype"],
                     "eligible": eligible, "scale": tasks[planned["task"]]["scale"],
                     "seconds": summary.get("task_wall_seconds"),
                     "state": summary.get("state", "not_attempted"),
                     "terminal": summary.get("terminal_observed") is True})
    by_task = {}
    for row in rows:
        by_task.setdefault(row["task"], {})[row["arm"]] = row

    def pairs(left, right):
        result = []
        for task_id, arms in sorted(by_task.items()):
            left_row, right_row = arms.get(left), arms.get(right)
            if not left_row or not right_row:
                continue
            eligible = bool(left_row["eligible"] and right_row["eligible"])
            result.append({
                "archetype": tasks[task_id]["archetype"],
                "eligible": eligible,
                "left_seconds": left_row["seconds"] if eligible else None,
                "ratio": left_row["seconds"] / right_row["seconds"] if eligible else None,
                "right_seconds": right_row["seconds"] if eligible else None,
                "scale": tasks[task_id]["scale"],
                "task": task_id,
            })
        return result

    comparisons = {
        "candidate_to_shell": comparison_report(pairs("candidate-mcp", "shell")),
        "candidate_to_upstream": comparison_report(pairs("candidate-mcp", "upstream-mcp")),
        "upstream_to_shell": comparison_report(pairs("upstream-mcp", "shell")),
    }
    target = config["analysis"]["primary_geometric_ratio_at_most"]
    primary = comparisons["candidate_to_shell"]["all"]
    gates = {
        "all_rows_admitted": sum(row["admitted"] for row in rows) >=
                             config["analysis"]["required_admitted_rows"],
        "all_rows_terminal": sum(row["terminal"] for row in rows) >=
                             config["analysis"]["required_terminal_rows"],
        "candidate_shell_pair_count": primary["eligible_pairs"] >=
                                      config["analysis"]["minimum_candidate_shell_eligible_pairs"],
        "candidate_upstream_pair_count":
            comparisons["candidate_to_upstream"]["all"]["eligible_pairs"] >=
            config["analysis"]["minimum_candidate_upstream_eligible_pairs"],
        "source_oracle": source_ok,
        "speed": primary["geometric_ratio"] is not None and
                 primary["geometric_ratio"] <= target,
        "upstream_shell_pair_count": comparisons["upstream_to_shell"]["all"]["eligible_pairs"] >=
                                     config["analysis"]["minimum_upstream_shell_eligible_pairs"],
    }
    passed = all(gates.values())
    return {"binaries": binary_report(setup), "comparisons": comparisons,
            "gates": gates, "passed": passed, "rows": rows,
            "source_oracle_passed": source_ok,
            "rules": config["analysis"]}


def markdown_results(report):
    lines = ["# Complete-task reproduction results", "",
             "## Binary identities", "",
             "| Backend | Observed SHA-256 | Published SHA-256 | Matches reference |",
             "|---|---|---|---|"]
    for name, row in report["binaries"].items():
        reference = row["published_binary_sha256"] or "unavailable"
        matches = ("unavailable" if row["binary_sha256_matches_published"] is None else
                   str(row["binary_sha256_matches_published"]).lower())
        lines.append(f'| {name} | `{row["binary_sha256"]}` | `{reference}` | {matches} |')
    lines.extend(["", "A mismatch is informational because build paths can change binary bytes.",
                  "Source, toolchain, and runtime seals remain required.", ""])
    labels = {
        "candidate_to_shell": "Code Cortex / shell",
        "candidate_to_upstream": "Code Cortex / upstream",
        "upstream_to_shell": "Upstream / shell",
    }
    for key, label in labels.items():
        lines.extend([f"## {label}", "",
                      "| Group | Eligible | Planned | Geometric ratio | Reduction |",
                      "|---|---:|---:|---:|---:|"])
        for group, row in report["comparisons"][key].items():
            ratio = "unavailable" if row["geometric_ratio"] is None else f'{row["geometric_ratio"]:.6f}'
            reduction = ("unavailable" if row["geometric_percent_reduction"] is None else
                         f'{row["geometric_percent_reduction"]:.3f}%')
            lines.append(f'| {group} | {row["eligible_pairs"]} | {row["planned_pairs"]} | '
                         f'{ratio} | {reduction} |')
        lines.append("")
    lines.extend(["Failed or incorrect rows remain in the evidence and do not receive an estimated time.", ""])
    return "\n".join(lines)


def analyze_stage(config_path, config, work, setup_ready_sha256):
    work = pathlib.Path(work).resolve(strict=True)
    setup = verify_setup(config_path, config, work, setup_ready_sha256,
                         immutable_only=True)
    run_output = work / "run"
    verify_hash_manifest(run_output)
    output = work / "analysis"
    if output.exists():
        raise FileExistsError("analysis output already exists")
    output.mkdir()
    report = analyze_data(config, setup, run_output)
    save_new(output / "report.json", report)
    (output / "RESULTS.md").write_text(markdown_results(report))
    write_hash_manifest(output)
    if not report["passed"]:
        raise SystemExit(1)


def verify_stage(config_path, config, work, setup_ready_sha256):
    work = pathlib.Path(work).resolve(strict=True)
    setup = verify_setup(config_path, config, work, setup_ready_sha256,
                         immutable_only=True)
    counts = {"setup": verify_hash_manifest(work / "setup")}
    for name in ("run", "analysis"):
        counts[name] = verify_hash_manifest(work / name)
    seal = setup["runtime_cache_seal"]
    TRI.cache_integrity.verify(setup, seal["path"], seal["sha256"], immutable_only=True)
    print(json.dumps({"files_checked": counts, "passed": True}, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("doctor", "prepare", "run", "analyze", "verify"))
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--allow-version-drift", action="store_true")
    parser.add_argument("--setup-ready-sha256")
    args = parser.parse_args()
    config_path, config = load_config(args.config)
    if args.stage == "doctor":
        report = doctor(config_path, config, args.work, args.allow_version_drift)
        print(json.dumps(report, indent=2, sort_keys=True))
        raise SystemExit(0 if report["passed"] else 1)
    if args.stage == "prepare":
        prepare(config_path, config, args.work, args.allow_version_drift)
    else:
        if not args.setup_ready_sha256:
            parser.error(f"{args.stage} requires --setup-ready-sha256")
        if args.stage == "run":
            run_stage(config_path, config, args.work, args.setup_ready_sha256)
        elif args.stage == "analyze":
            analyze_stage(config_path, config, args.work, args.setup_ready_sha256)
        else:
            verify_stage(config_path, config, args.work, args.setup_ready_sha256)


if __name__ == "__main__":
    main()
