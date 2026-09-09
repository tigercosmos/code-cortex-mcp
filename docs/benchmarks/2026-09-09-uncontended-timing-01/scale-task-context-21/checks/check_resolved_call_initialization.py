#!/usr/bin/env python3
"""Reject non-trivial or non-value-initialized CBMResolvedCall records."""

import argparse
import pathlib
import re


FIELD_DEFAULT = re.compile(r"uint32_t\s+(?:source_byte|binary_operator_line)\s*=\s*0\s*;")
PLAIN_LOCAL = re.compile(r"\bCBMResolvedCall\s+([A-Za-z_]\w*)\s*;")
MEMSET_RECORD = re.compile(r"\bmemset\s*\([^;]*\bCBMResolvedCall\b")


def violations(root):
    result = []
    for scope in (root / "internal" / "cbm", root / "src" / "pipeline"):
        for path in sorted(scope.rglob("*")):
            if path.suffix not in {".cpp", ".h"}:
                continue
            for lineno, line in enumerate(path.read_text(errors="surrogateescape").splitlines(), 1):
                if FIELD_DEFAULT.search(line):
                    result.append((path.relative_to(root), lineno, "in-class field initializer"))
                if PLAIN_LOCAL.search(line):
                    result.append((path.relative_to(root), lineno, "local is not value-initialized"))
                if MEMSET_RECORD.search(line):
                    result.append((path.relative_to(root), lineno, "record is initialized by memset"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=pathlib.Path)
    args = parser.parse_args()
    found = violations(args.root.resolve())
    for path, lineno, message in found:
        print(f"{path}:{lineno} -- {message}")
    print(f"VIOLATIONS: {len(found)}")
    if found:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
