#!/usr/bin/env python3
"""Verify every frozen answer directly from generated source, without graph data."""

import argparse
import json
import pathlib
import re


def location(symbol):
    number = int(re.fullmatch(r"capacity_function_(\d+)", symbol).group(1))
    file_number, local = divmod(number, 20)
    relative = pathlib.Path(f"part_{file_number // 1000:04d}") / f"unit_{file_number:06d}.cpp"
    return number, relative, local * 50 + 1


def verify_task(task, root):
    if task["archetype"] == "one-shot":
        number, relative, line = location(task["symbol"])
        lines = (root / relative).read_text().splitlines()
        expected_source = f"int capacity_function_{number}(int value) {{"
        observed = f"{relative}:{line}"
        passed = lines[line - 1] == expected_source and observed == task["expected"]
        evidence = {"definition": observed, "source": lines[line - 1]}
    else:
        chain = task["expected"]
        evidence = []
        passed = chain[0] == task["from_symbol"] and chain[-1] == task["symbol"]
        for caller, callee in zip(chain, chain[1:]):
            caller_number, relative, line = location(caller)
            callee_number, callee_relative, callee_line = location(callee)
            lines = (root / relative).read_text().splitlines()
            definition = lines[line - 1]
            call = lines[line + 47]
            edge_ok = (callee_number == caller_number - 1 and relative == callee_relative and
                       definition == f"int capacity_function_{caller_number}(int value) {{" and
                       call == f"    return capacity_function_{callee_number}(value);" and
                       lines[callee_line - 1] ==
                       f"int capacity_function_{callee_number}(int value) {{")
            passed = passed and edge_ok
            evidence.append({"caller": caller, "caller_definition": f"{relative}:{line}",
                             "call": f"{relative}:{line + 48}", "callee": callee,
                             "callee_definition": f"{callee_relative}:{callee_line}",
                             "source": call, "correct": edge_ok})
    return {"task": task["id"], "passed": passed, "evidence": evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup", type=pathlib.Path, required=True)
    parser.add_argument("--tasks", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    setup = json.loads(args.setup.read_text())
    tasks = json.loads(args.tasks.read_text())
    results = [verify_task(task, pathlib.Path(setup["projects"][task["scale"]]["repo_root"]))
               for task in tasks]
    report = {"tasks": len(results), "passed": all(row["passed"] for row in results),
              "results": results}
    with args.output.open("x") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
