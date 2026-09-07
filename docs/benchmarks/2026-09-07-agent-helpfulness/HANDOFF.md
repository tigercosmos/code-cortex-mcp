# Hand-off: continue the agent-helpfulness work

Read `README.md` in this directory first; it holds every number referenced below and the
review round. The code is on `main` (commit "agent helpfulness: ...").

## State of things

- Build: `scripts/build.sh` -> `build/c/code-cortex-mcp` (reports version `dev`; CI stamps
  real versions). Tests: `scripts/test.sh` (ASan) or the fast loop
  `cmake -S . -B build/nosan -DCBM_SANITIZE=OFF -DCBM_TEST_SEAMS=ON && cmake --build build/nosan
  --target test-runner && ./build/nosan/test-runner` from the repo root. 5747 pass.
  The suite now isolates HOME; running an OLDER checkout's suite deletes databases in
  `~/.cache/code-cortex-mcp` (that happened once on this machine).
- Install for Claude Code / Codex: `build/c/code-cortex-mcp install -y`; check with
  `build/c/code-cortex-mcp doctor`. Codex names the tools `mcp__code_cortex_mcp__<tool>`.
- Indexed on this machine under path-derived names: code-cortex-mcp, PcapPlusPlus, modmesh,
  and the ladder repos `_bench/jansson`, `_bench/lz4`, `elfuse`, `Yatagarasu`, `cgal`,
  `_bench/opencv` (`reindex_ladder.sh` re-creates them in under a minute).
- Harness in this directory: `runner.sh` (Claude Code arms off/old/new; edit `OLD_BIN`/`NEW_BIN`
  at the top — `NEW_BIN` still points at the worktree build), `orchestrate.sh`, `grade.py`,
  `extract.py`, `analysis.py`, `ladder_analysis.py`, `pooled.py`, `tasks_main.json` (18 tasks,
  3 repos), `tasks_ladder.json` (28 tasks, 7 repos, 19k-1.8M LOC), `fixtures_eval.py`
  (tool-level P/R), `evidence/probe.py` (Codex availability), `evidence/hook_check.py`.
  The Claude arms need the env switches `setup_global_hooks.py` installs into
  `~/.claude/hooks/cbm-*` (`CORTEX_AB_OFF`, `CORTEX_AB_ARM`, `CORTEX_AB_BIN`); they are in
  place on this machine, backups in the session scratchpad.
- The earlier Codex runners are `docs/benchmarks/2026-09-06-codex-ab/run_benchmark.py` and
  `docs/benchmarks/2026-09-06-codex-medium-multiproject/run_benchmark.py` (fresh
  `CODEX_HOME`, codexmon, frozen rubrics, anonymous grading).

## What is measured and what is not

Measured: Codex tool availability (0/16 -> 6/6), tool-level caller/impact P/R (1.00/1.00 on
six fixtures), C++ class recall (SerializableItem 4 -> 13 of 13), Claude Code A/B on Sonnet
(108 sessions), Fable and Opus size ladder (336 sessions clean + 336 pilot). Result: never
worse, ~24% fewer shell searches, Opus cheaper, call chains 2.5x faster, tasks a single grep
answers stay a tie.

Not measured: any task that EDITS code (so the PostToolUse blast-radius hook has no data),
Codex agent-level behaviour with the fixed catalog and the new tools, long sessions, and
non-C/C++ repositories at scale.

## Next work, in priority order

1. Codex agent-level experiment with verified tool access: three arms (shell only; old
   binary; this build), fresh `CODEX_HOME` per session, a smoke call to a tool from the END of
   the catalog (`list_projects`) before every timed session, and a recorded exposure check.
   Include the 18 `tasks_main.json` tasks (their graders are deterministic) plus new EDIT tasks.
2. Edit tasks with graded file sets: "add a required parameter to X and update every caller",
   graded by the set of files changed (git status) against `inspect_symbol`'s `caller_files`
   plus `declared_in`, and by the project's own tests where they exist. Reset the repo between
   sessions (`git checkout -- . && git clean -fd`), one copy per arm to allow parallel runs
   (each copy must be indexed; names derive from the path). This measures the PostToolUse hook.
3. Header prototypes as graph nodes. `inspect_find_header_decls` is a text scan over the
   obvious headers; the extraction pipeline should emit a `Declaration` node (or a `declares`
   property) so `caller_files` is complete for any layout.
4. The PreToolUse brief fires only for an exact symbol. Consider a second tier for a unique
   prefix/suffix match, measured for noise (the old five-symbol list measured null).
5. Reduce the tool-schema round trip: in Claude Code the graph path costs a `ToolSearch` turn
   before the first call. Investigate whether a smaller tool set or a combined tool
   description changes that; measure with `tasks_main.json`.
6. Opus arm of the 18-task study, and Sonnet on the ladder, to complete the model matrix.
7. Follow-ups from review: remove the temporary test HOME on exit; share the quoting guard
   between the gate and session scripts; consider `caller_files` pagination instead of a cap.

## Gotchas

- `tools/list` must stay one page (Codex ignores `nextCursor`). `doctor` checks it.
- Project names are the full path encoded (`Users-me-repo`), not the basename; a custom name
  resolves from cwd through the memo's `root_path`.
- A freshly built binary's first exec costs 2-3 s on macOS (code signing) and exceeds Codex's
  1 s grace for optional servers; `install` warms it, a probe must too (`--version` once).
- The A/B runs need `--strict-mcp-config --mcp-config` per arm and the hook env switches;
  `--settings '{"hooks":{}}'` does not disable user hooks.
- The grader drops sessions with `is_error`/`rc!=0`; an account session limit turns every run
  into an error silently — check `result_text`.
