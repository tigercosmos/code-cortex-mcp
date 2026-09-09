#!/usr/bin/env python3
"""Generate deterministic C++ scale fixtures; all oracle metadata stays outside source."""

import argparse
import hashlib
import json
import pathlib
import shutil
import time

LINES_PER_FUNCTION = 50
FUNCTIONS_PER_FILE = 20
LINES_PER_FILE = LINES_PER_FUNCTION * FUNCTIONS_PER_FILE


def source_file(number):
    lines = []
    for local in range(FUNCTIONS_PER_FILE):
        symbol = number * FUNCTIONS_PER_FILE + local
        lines.append(f"int capacity_function_{symbol}(int value) {{\n")
        for step in range(47):
            lines.append(f"    value = (value ^ {step + 1}) + 1;\n")
        if local:
            lines.append(f"    return capacity_function_{symbol - 1}(value);\n")
        else:
            lines.append("    return value;\n")
        lines.append("}\n")
    return "".join(lines).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--lines", type=int, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.lines <= 0 or args.lines % LINES_PER_FILE:
        raise ValueError("lines must be a positive multiple of 1000")
    if args.root.exists() or args.manifest.exists():
        raise FileExistsError("root and manifest must be new paths")
    files = args.lines // LINES_PER_FILE
    upper_bound = len(source_file(files - 1)) * files
    args.root.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(args.root.parent).free < upper_bound + 10 * 1024**3:
        raise OSError("less than source upper bound plus 10 GiB reserve")
    args.root.mkdir()
    started = time.monotonic()
    aggregate = hashlib.sha256()
    total_bytes = 0
    with args.manifest.open("x") as manifest:
        for number in range(files):
            relative = pathlib.Path(f"part_{number // 1000:04d}") / f"unit_{number:06d}.cpp"
            path = args.root / relative
            path.parent.mkdir(exist_ok=True)
            data = source_file(number)
            path.write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            record = {"file": str(relative), "lines": LINES_PER_FILE,
                      "bytes": len(data), "sha256": digest}
            encoded = (json.dumps(record, sort_keys=True) + "\n").encode()
            manifest.write(encoded.decode())
            aggregate.update(encoded)
            total_bytes += len(data)
            if (number + 1) % 1000 == 0:
                manifest.flush()
                print(json.dumps({"completed_files": number + 1, "total_files": files,
                                  "elapsed_seconds": time.monotonic() - started}), flush=True)
    report = {"state": "complete", "requested_code_lines": args.lines, "files": files,
              "functions": files * FUNCTIONS_PER_FILE, "source_bytes": total_bytes,
              "manifest_sha256": aggregate.hexdigest(),
              "elapsed_seconds": time.monotonic() - started}
    print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
