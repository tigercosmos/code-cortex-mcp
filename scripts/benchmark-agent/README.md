# Agent-level complete-task benchmark

This harness measures what a user actually gets: a fresh Claude Code session, the
task as the only prompt, and the agent free to use whatever tools it has. The two
arms differ only in what is installed.

| Arm | Claude Code config | MCP servers |
|---|---|---|
| `off` | empty `settings.json`: no hooks, no skill | none (`--strict-mcp-config`) |
| `mcp` | `code-cortex-mcp install` output: UserPromptSubmit, PreToolUse, PostToolUse, SessionStart and SubagentStart hooks plus the skill | code-cortex-mcp over stdio |

The prompt is identical in both arms. The agent is never told to use or avoid the graph.

Earlier "complete-task" numbers (the v743 run of 2026-09-10) were a different
measurement: the harness itself made one MCP call and pasted the normalized answer into
the prompt. That measures a model that has the answer against a model that has to grep.
The results here replace them.

## Tasks

`tasks.json` holds the 24 frozen tasks: three synthetic C++ corpora (10 thousand, 1
million and 100 million lines) × four definition lookups ("return exactly one
`path:line`") × four shortest call chains ("one bare function name per line"). Grading is
exact string match; `analyze.py` also reports a lenient grade that accepts a prose line
before the answer, because one model tends to add one.

`gen_corpus.py` regenerates the corpora byte-for-byte (verified against the frozen
sha256 manifests for 10k and 1m; 100m follows the same rule):

```bash
python3 gen_corpus.py "$BENCH_ROOT/src" 10k
python3 gen_corpus.py "$BENCH_ROOT/src" 1m
python3 gen_corpus.py "$BENCH_ROOT/src" 100m     # 100 000 files, 3 GB
```

## Running

Requirements on the benchmark host: a logged-in `claude` CLI (2.1.287 was used), the
binary under test, python3. Run sessions serially on an otherwise idle machine.

```bash
export BENCH_ROOT=$HOME/bench-agent            # everything lives here
export BENCH_BIN=/path/to/code-cortex-mcp      # binary under test
mkdir -p $BENCH_ROOT && cd $BENCH_ROOT
cp /path/to/repo/scripts/benchmark-agent/*.{py,sh,json} .

./index.sh $BENCH_ROOT/src                     # index the three corpora into $BENCH_ROOT/cache
./mkcfg.sh                                     # build cfg-off and cfg-mcp (see warning inside)

nohup python3 run.py --model claude-fable-5-1 --src-root $BENCH_ROOT/src --out $BENCH_ROOT/run-fable --reps 2 &
python3 analyze.py run-fable/results.jsonl
```

`run.py` writes one JSON row per session (wall seconds, API time, turns, cost, tokens,
tool calls, hook events, grade) and keeps the full `stream.jsonl` of every session so a
claim can be checked against what the agent said and ran. Rows already present are
skipped, so a rerun resumes. `stop.sh <out_dir>` stops a run by its pid file.

`analyze.py` prints, per model, the paired wall-time ratio mcp/off over task pairs where
both arms answered correctly (geometric mean and sum ratio), broken down by corpus size and
by task shape, with an exact sign test, plus accuracy, cost, turns and tool adoption.

## What was measured on 2026-10-03

See "Complete-task performance" in the top-level README for the table and conditions.
Raw rows from that run are not committed (`docs/benchmarks/` is gitignored).
