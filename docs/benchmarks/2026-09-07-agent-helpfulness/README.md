# Making the graph helpful to an agent: what changed and what it measured

Two studies on 2026-09-06 (243 and 504 Claude Code sessions, 64 Codex sessions) found that
installing code-cortex-mcp changes nothing measurable, that forcing its use costs 1.3-2.1x,
and that multi-hop call chains were the one task where the graph won. This branch applies
the recommendations from both write-ups and measures each one. The headline: the graph is now
delivered mostly as context rather than as a tool the model must choose, one call answers a
caller or impact question with verifiable evidence, and the availability defect that hid six
tools from Codex is fixed. Task-level wall time and accuracy on read-only questions remain a
tie with shell search, as before; the wins are concentrated where grep cannot go.

## Availability (Codex)

Codex CLI 0.153.4 exposes an MCP server's tools through an `ALL_TOOLS` catalog. A live
session showed exactly eight cortex tools in it: the first page of our `tools/list`, which
paged at 8 with a `nextCursor` that Codex never requests. `list_projects`, `index_status`,
`detect_changes`, `delete_project`, `manage_adr` and `ingest_traces` were unreachable, which
matches the "empty catalog" results in the multi-project study. Server startup was not the
cause (spawn to `initialize` is under 10 ms).

Probe: fresh `CODEX_HOME`, `codex exec --json`, prompt "call `list_projects` once", four
configurations x four repetitions.

| Binary | optional | required=true | grace 0 ms | required + grace 0 | Total |
|---|---:|---:|---:|---:|---:|
| Before (page of 8) | 0/4 | 0/4 | 0/4 | 0/4 | **0/16** |
| After (one page) | 3/4 | 4/4 | | | **7/8** |
| After, binary warmed once | 6/6 | | | | **6/6** |

The one miss after the fix was the first execution of a freshly built binary, where macOS
code-signing validation takes 2-3 s and exceeds Codex's 1000 ms grace for optional servers.
`install` already executes the binary once for that reason. `required=true` and
`mcp_optional_startup_grace_ms=0` changed nothing before the fix and are not needed after it.
`code-cortex-mcp doctor` now checks that one page holds every tool. Codex names the tools
`mcp__code_cortex_mcp__<tool>` (underscores); the Codex instruction file says so.

## One call with evidence: `inspect_symbol`

`inspect_symbol(symbol)` returns the definition (file, lines, signature, source head), direct
callers with call-site lines and the resolver's confidence and strategy, related tests, callers
from other languages, subclasses, a complete `caller_files` rollup that is never paged, header
declarations found by scanning the obvious headers, and trust signals (`index.file_modified_after_index`,
`coverage_note`). It accepts bare names, `Class::method`, and qualified names, and it fits a
`max_bytes` budget with a `continuation` hint when cut. `trace_path` entries now carry
`file`/`start_line`/`end_line`, and its edges carry `from_file`, `line`, `strategy` and the
real `confidence` (the store used to hardcode 1.0).

Tool-level precision and recall against the studies' source-backed ground truth (file sets,
one call per task, no agent):

| Task | Symbol | Precision | Recall | Note |
|---|---|---:|---:|---|
| PcapPlusPlus callers | `fnvHash` | 1.00 | 1.00 | |
| PcapPlusPlus signature change | `hexStringToByteArray` | 1.00 | 1.00 | header found via `declared_in` |
| modmesh callers | `ReflectionSession.advance` | 1.00 | 1.00 | Python method |
| modmesh signature change | `world_from_screen` | 1.00 | 1.00 | 4 C++ + 2 Python + 3 test files, one call |
| code-cortex-mcp callers | `cbm_validate_shell_arg` | 1.00 | 1.00 | |
| code-cortex-mcp signature change | `cbm_json_escape` | 1.00 | 1.00 | header found via `declared_in` |

Before the header scan the two signature-change rows scored recall 0.93: prototypes are not
graph nodes. Script: `fixtures_eval.py`.

## Trust: the C++ class extraction gap

The earlier note that "only the first class per file survives in some headers" had a concrete
cause: a macro invocation whose arguments are statements, such as modmesh's
`SC_DECL_SERIALIZABLE(register_member("id", m_id); ...)`, cannot be parsed by tree-sitter-cpp,
and its error recovery swallows every declaration after it. Extraction now parses a copy of
C/C++ sources with such invocations blanked (same byte offsets, so lines are unchanged). On
`World.hpp` the classes went from 3 (`WorldShapeState`, plus two nested) to all 10, with zero
partial-parse ranges instead of 27. On the whole of modmesh the class count rose from 1059 to
1074 and the `SerializableItem` direct-subclass query returns 13 instead of 4, matching the
ground truth the earlier study graded at F1 0.42.

## Project resolution and reply size

10-12% of graph calls in both studies failed on a guessed project name. Query tools now
resolve a missing `project` from the working directory (walking up eight parents), and answer
an unknown name for the cwd project with a `project_note`. Destructive tools stay strict.

`search_graph` omits the 14 complexity metrics unless `include_metrics=true` (1127 vs 1425
bytes per node here; the studies measured 3.4-7 KB per call). `search_code` full mode returns a
window around the matches (at most 80 lines) and both `search_code` and `inspect_symbol`
honor `max_bytes`; the ten-result full-mode reply that measured 55 KB is now 38 KB at the
default budget and 10 KB at `max_bytes=6000`, with `truncated`, `shown` and `continuation`.
`is_test_file` is now case-insensitive, so PcapPlusPlus's `Tests/` suites no longer appear as
production callers when `include_tests=false`.

## Context instead of directives

The "ALWAYS use graph tools FIRST" SessionStart text, the `~500 tokens vs ~80K` claim, and the
symbol-list PreToolUse output (which duplicated grep's own output and measured null) are gone.
The hook binary now dispatches on the hook event:

- **SessionStart**: a ~1.2 KB brief for the cwd project (size, languages, largest modules, the
  eight most-called functions with file paths) plus three sentences on when the graph beats
  grep, or a one-line "not indexed" pointer. 130 ms.
- **PreToolUse** (Grep, Glob, Bash searches): only for an exact symbol in the pattern, what grep
  cannot show: definition versus declaration, caller/test/file counts with call-site lines,
  cross-language callers, subclasses, freshness and coverage notes. 10-25 ms; silent otherwise.
- **PostToolUse** (Edit, Write, MultiEdit): the blast radius of the edited file: direct callers
  of the symbols it defines, by file, tests separated. 10 ms. New matcher in `settings.json`.

Guidance in the skill, the agent instruction files, and the Codex/Gemini one-liners is now
scoped to callers, impact, call chains, tests and cross-language users, names grep's territory
explicitly, and every example matches the current schemas (`project` optional, `direction`
not `mode`).

## Agent-level A/B (Claude Code, Sonnet 5, medium effort)

18 audited read-only tasks from the earlier study (3 repositories x locate/callers/callchain/
impact/hierarchy/textcfg), 3 arms, 2 repetitions, 108 sessions, paired by (task, rep), exact
sign test. `off`: no MCP, hooks disabled. `old`: the installed v0.20 binary with the previous
directive hooks. `new`: this branch's binary with the context hooks. Harness: `runner.sh`,
`orchestrate.sh`, `grade.py`, `analysis.py`; per-session metrics in `evidence/`.

| Arm | Median wall | Median context tokens | Median cost | Mean F1 | Sessions using MCP | MCP calls |
|---|---:|---:|---:|---:|---:|---:|
| off | 16.3 s | 173.6k | $0.079 | 1.000 | 0/36 | 0 |
| old | 14.6 s | 140.3k | $0.068 | 0.998 | 10/36 | 32 (3.2 per using session) |
| new | 14.8 s | 163.7k | $0.080 | 0.995 | 16/36 | 16 (1.0 per using session) |

No paired difference reaches significance (all sign-test p >= 0.40). By archetype, median
wall time / context:

| Archetype | off | old | new |
|---|---:|---:|---:|
| callchain | 32.8 s / 371k | 18.5 s / 194k | **12.9 s / 149k** |
| callers | **8.7 s / 98k** | 10.8 s / 136k | 14.1 s / 140k |
| impact | **11.9 s / 135k** | 14.6 s / 155k | 17.8 s / 187k |
| hierarchy | 16.9 s / 204k | 30.8 s / 183k | 25.6 s / 200k |
| locate | 9.4 s / 128k | 8.2 s / 101k | 9.0 s / 102k |
| textcfg | 18.1 s / 200k | 16.9 s / 176k | 16.5 s / 174k |

What the sessions show: on call-chain tasks the new arm makes exactly one `trace_path` call and
finishes in three turns (four of six sessions in 8.6-13.3 s against 28-47 s for shell). On
callers and impact tasks the baseline is a single grep, while the graph path costs a schema-
loading `ToolSearch` turn, the `inspect_symbol` call, and one or two verification greps; that
is the whole penalty, and it is why the guidance no longer pushes the graph for questions a
single grep answers. Adoption went from 10 to 16 sessions and the number of graph calls per
using session from 3.2 to 1.0. Accuracy was a tie (two answers below F1 1.0 in `new`, one in
`old`, both model errors without a graph call involved in one of them).

The two hooks that fire on edits could not matter in a read-only task set; measuring them needs
edit tasks with graded file sets, which this run did not include.

## Size ladder rerun (Fable 5.1 and Opus 5, 19k to 1.8M LOC)

The 2026-09-06 size study found that the graph gets relatively worse in bigger repositories
(Spearman of log LOC against the forced/off wall ratio: +0.22 and +0.11) and that adoption at
medium effort was near zero. Rerun with this branch: the same seven C/C++ repositories
(jansson 19k, lz4 28k, elfuse 62k, PcapPlusPlus 175k, Yatagarasu 314k, CGAL 837k, OpenCV
1.8M LOC), the same four audited archetypes per repository (locate, callers, callchain,
impact), off/old/new arms, two repetitions, `claude-fable-5-1` and `claude-opus-5` at medium
effort: 336 sessions per round. Two rounds were run; the first is a pilot because the
hook's grep-pattern extractor was fixed while it ran (it missed `cd dir; grep ...` prefixes
and `\b` word boundaries, which is how agents write most of their searches). Round two is on
the final binary. Every repository was re-indexed under its path-derived name first (OpenCV
in 15 s). No session failed in any round.

Round two, pooled over both models (112 pairs, paired by model, task, rep):

| Comparison | Wall | Context tokens | Cost | Shell searches | Accuracy |
|---|---:|---:|---:|---:|---:|
| new vs off | x0.98 (p=0.78) | x1.03 (p=0.07) | x0.96 (p=0.07) | fewer in 48 of 59 differing pairs (p<0.0001) | 112/112 vs 110/112 at F1 1.0 |
| new vs old | x0.98 (p=0.40) | x1.01 (p=0.22) | x0.97 (p=0.30) | fewer in 46 of 63 (p=0.0003) | 112/112 vs 112/112 |

Shell searches per arm over 112 sessions: off 327, old 315, new 249. Sessions that called a
graph tool: Fable 3 of 56, Opus 6 of 56; the symbol brief from the PreToolUse hook reached 53
of 56 Fable and 50 of 56 Opus new-arm sessions, so the hook is the channel that carries the
graph to these models at medium effort.

Per model, round two:

| Model | Arm | Median wall | Median context | Median cost | F1 = 1.0 | Using MCP |
|---|---|---:|---:|---:|---:|---:|
| Fable 5.1 | off | 15.8 s | 73.9k | $0.122 | 54/56 | 0 |
| | old | 15.1 s | 71.7k | $0.121 | 56/56 | 0 |
| | new | 16.7 s | 71.4k | $0.119 | 56/56 | 3 |
| Opus 5 | off | 14.6 s | 90.7k | $0.102 | 56/56 | 0 |
| | old | 15.4 s | 91.3k | $0.112 | 56/56 | 0 |
| | new | **13.4 s** | **82.6k** | $0.106 | 56/56 | 6 |

Opus new versus old: wall x0.95 with new faster in 36 of 56 pairs (p=0.044); new versus off:
context x0.85 at the median and cost lower in 36 of 56 pairs (p=0.044). Fable: all
comparisons within noise (p >= 0.50 for wall and cost).

By repository size, median wall ratio new/off (Fable, Opus): jansson 1.07, 0.92; lz4 1.01,
0.94; elfuse 0.84, 0.82; PcapPlusPlus 1.01, 0.82; Yatagarasu 0.97, 0.99; CGAL 1.15, 1.01;
OpenCV 0.94, 1.13. Spearman(log LOC, new/off): -0.14 (Fable), +0.71 (Opus); the pilot round
gave +0.25 and +0.14. The sign is not stable across rounds or models, and no repository shows
a consistent direction: as in the earlier study, repository size does not decide whether the
graph pays, because the shell baseline answers these four archetypes in two to four targeted
greps at every size. CGAL remains the slowest repository for every arm (21-32 s).

What changed against the earlier size study is therefore not speed at scale but the shape of
the cost: the graph no longer costs anything when the model ignores it (old and new arms are
never significantly slower than off, whereas forcing use cost 1.7-2.0x before), the model
runs about a quarter fewer shell searches with the context hooks on, accuracy is never worse,
and Opus's cost and context drop measurably. Scripts and per-session data:
`run_ladder.sh`, `ladder_analysis.py`, `pooled.py`, `evidence/ladder*_graded.json`.

## Independent review

Two reviewers read the full diff without prior context: a fresh Claude Fable 5.1 agent and
Codex (`gpt-6-astra`, high reasoning, via codexmon). Both asked for changes. Findings and
what was done:

| Finding | Raised by | Outcome |
|---|---|---|
| The cwd project memo cached a miss for the process lifetime, so a worker started in an unindexed repo never saw a later `index_repository` | both | Fixed: only hits are memoized, and only while their db exists |
| `inspect_symbol` returned the oversized payload after its shrink rounds; `caller_files`, tests and subclasses never shrank | Codex | Fixed: those lists shrink too, 24 rounds, 914 B at `max_bytes=1500` on a 257-caller symbol |
| `search_code` skipped its byte budget when hits mapped to no graph node (only raw matches) | Codex | Fixed: raw matches shrink after enriched results |
| `cd <dir>; grep` prefix was recognized but the augment resolved the project from the payload cwd, not `<dir>` | Codex | Fixed: the walk-up starts at `<dir>` (absolute or joined onto cwd) |
| Per-symbol caller counts in the edit-impact note were undercounted by the global dedup | Codex | Fixed: per-symbol distinct callers; the global set only dedups the file rollup |
| Freshness compared file mtime with db mtime; a restored db hides stale relationships | Codex | Fixed: compared against the project's `indexed_at`, db mtime as fallback |
| Macro blanking mis-scanned C++ raw strings `R"(...)"` containing `"` or `;` | Codex | Fixed, with a test; also stops at a preprocessor line inside the argument list (Fable) |
| Hook JSON buffer (352 B) truncates long project names into malformed JSON, silently disabling the augment | both | Fixed: sized for the longest legal name, and the request is dropped rather than sent malformed |
| `json_extract` raises on a malformed `properties` row and empties the whole BFS edge list | Fable | Fixed: `json_valid` guard |
| `project_note` could leak from an error reply into the next call | Fable | Fixed: cleared on every reply path |
| A memo row that outlives its db made the name "known" | Fable | Fixed: file existence checked |
| Windows test build: `realpath` in the new cwd test; `compat.h` include inside a POSIX-only block | both | Fixed: test guarded, include hoisted |
| Same-basename repositories at different roots would collide | Fable | Not a bug: project names encode the whole path |
| Session-reminder script embeds the binary path in a double-quoted bash string | Fable | Pre-existing pattern shared with the gate script; left as is |
| Temporary test HOME is not removed | Fable | Left as is |

Both reviewers verified the memory/lifetime handling (yyjson borrowing, ownership moves,
error-path frees), the path containment of every file read, and that destructive tools stay
strict. Suite after the fixes: 5747 passed, 1 skipped; fixture precision/recall unchanged at
1.00/1.00.

## Reproduce

- Availability: `evidence/probe.py <binary> <repo> <out> <reps> optional required ...`
- Fixtures: `fixtures_eval.py <binary> tasks_main.json`
- A/B: `setup_global_hooks.py` (adds env switches to the installed hooks, backs up first),
  then `orchestrate.sh tasks_main.json sonnet 2 <out> 3`, `grade.py`, `analysis.py`.
- Hooks: `hook_check.py <binary>` feeds sample Claude Code payloads to `hook-augment`.
