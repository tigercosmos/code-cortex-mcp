#!/bin/bash
# stop.sh <out_dir> : stop a run.py started with --out <out_dir> and its current claude session
P=$(cat "$1/run.pid")
KIDS=$(ps -o pid= --ppid "$P")
kill "$P"
for c in $KIDS; do kill -- -"$c" 2>/dev/null; done
