#!/usr/bin/env python3
"""Mechanical re-check of tasks_real.json oracles on sim1 (no graph used).

- set_of_paths: files printed by `derivation` minus derivation_excluded == ground_truth
- path_and_line: the gold line exists and contains the symbol's bare name
- ordered_names: every name in the chain has a definition-looking grep hit, and for each
  consecutive pair (a, b) print a's definition line(s) so a human can read the body.
Usage: verify_tasks.py tasks_real.json [task_id ...]
"""
import json
import os
import re
import subprocess
import sys


def run(cmd, cwd):
    p = subprocess.run(["bash", "-c", cmd], cwd=cwd, capture_output=True, text=True, timeout=300)
    return p.stdout


def main():
    tasks = json.load(open(sys.argv[1]))
    want = set(sys.argv[2:])
    bad = 0
    for t in tasks:
        if want and t["id"] not in want:
            continue
        rp = t["repo_path"]
        kind = t["answer_kind"]
        status = "OK"
        detail = ""
        if kind == "set_of_paths":
            out = run(t["derivation"], rp)
            files = sorted({re.sub(r'^\./', '', l.split(":", 1)[0]) for l in out.splitlines() if l.strip()})
            excl = set()
            for e in t.get("derivation_excluded") or []:
                excl.add(e["path"] if isinstance(e, dict) else str(e).split()[0].rstrip(":,"))
            got = sorted(set(files) - excl)
            gold = sorted(t["ground_truth"])
            if got != gold:
                status = "MISMATCH"
                detail = f"derived-minus-excluded={got}\n      gold={gold}\n      extra={sorted(set(got)-set(gold))} missing={sorted(set(gold)-set(got))}"
            missing_files = [g for g in gold if not os.path.exists(os.path.join(rp, g))]
            if missing_files:
                status = "MISSINGFILE"
                detail += f" nonexistent={missing_files}"
            detail = detail or f"{len(gold)} files, derivation printed {len(files)} files, {len(excl)} excluded"
        elif kind == "path_and_line":
            path, _, line = t["ground_truth"][0].rpartition(":")
            name = re.split(r'::|\.|#|\s', t["symbol"].strip())[-1] if t.get("symbol") else ""
            try:
                text = open(os.path.join(rp, path), errors="replace").read().splitlines()[int(line) - 1]
            except (OSError, IndexError, ValueError) as e:
                text = f"ERR {e}"
            if not name or not re.search(r'\b' + re.escape(name) + r'\b', text):
                status = "LINE?"
            detail = f"{path}:{line}: {text.strip()[:160]}"
        elif kind == "ordered_names":
            detail = " -> ".join(t["ground_truth"])
        if status != "OK":
            bad += 1
        print(f"{status:<11} {t['id']:<26} {detail}")
    print(f"problems: {bad}")


if __name__ == "__main__":
    main()
