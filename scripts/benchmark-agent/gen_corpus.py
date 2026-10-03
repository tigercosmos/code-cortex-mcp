#!/usr/bin/env python3
"""Generate the synthetic C++ corpora used by the agent benchmark.

Layout (identical to the frozen v743 fixtures): every file is 1000 lines and
holds 20 functions of 50 lines. Function N is `int capacity_function_N(int
value)`, applies 47 xor-and-add steps, and returns `capacity_function_{N-1}`;
the first function of each file returns `value`, so call chains stay inside a
file. Files are `part_{unit // 1000:04d}/unit_{unit:06d}.cpp`.

Usage:
  gen_corpus.py <out_root> <scale>     scale: 10k | 1m | 100m
Creates <out_root>/source-<scale>/ (10, 1000 or 100000 files).
"""
import os
import sys

FILES = {"10k": 10, "1m": 1000, "100m": 100000}
FUNCS_PER_FILE = 20
STEPS = 47


def function_text(n, first):
    lines = [f"int capacity_function_{n}(int value) {{"]
    lines.extend(f"    value = (value ^ {k}) + 1;" for k in range(1, STEPS + 1))
    lines.append("    return value;" if n == first else f"    return capacity_function_{n - 1}(value);")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in FILES:
        sys.exit(__doc__)
    root = os.path.join(sys.argv[1], f"source-{sys.argv[2]}")
    for unit in range(FILES[sys.argv[2]]):
        part = os.path.join(root, f"part_{unit // 1000:04d}")
        os.makedirs(part, exist_ok=True)
        first = unit * FUNCS_PER_FILE
        body = "".join(function_text(n, first) for n in range(first, first + FUNCS_PER_FILE))
        with open(os.path.join(part, f"unit_{unit:06d}.cpp"), "w") as f:
            f.write(body)
    print(root)


if __name__ == "__main__":
    main()
