---
name: sync-upstream
description: Check DeusData/codebase-memory-mcp (upstream C11) for new commits, triage them down to the ones that matter for this C++23 fork — C++/Python/Go indexing, performance, optimization, bug fixes — confirm the shortlist with the user, then port them. Use when asked to "check upstream", "sync upstream", "pull upstream changes", "what's new upstream", or "port the latest upstream fixes".
---

# Upstream sync (curated)

Port genuinely-new upstream work into this fork **selectively**. The upstream
range between syncs is typically 100–400 commits; only a small fraction is worth
porting. This skill is triage-first and **gated on user approval** — never start
editing source before the user has confirmed the shortlist.

Background on the fork's structural divergences lives in `UPSTREAM_SYNC.md`
(read it — it is the source of truth for the last-synced marker and the sync
log). This skill is the *procedure*; that file is the *state*.

## Scope filter

**Port (in scope):**

| Theme | Examples |
|---|---|
| **C++ / Python / Go** indexing correctness | LSP resolvers, extraction, FQN/module naming, import-alias resolution, header/source node identity, preprocessor handling |
| **Bug fixes** | crashes, hangs, SIGABRT, UAF/OOB, data loss, dropped edges, wrong query results |
| **Performance / optimization** | allocator, interning, parallel passes, streaming writers, caps and bounds, quantization, memoization |
| **Security hardening** | injection/validation, path escapes, overflow guards |

**Skip (out of scope) — do not port, just list as skipped:**

- UI (`src/ui`, `graph-ui`), CI (`.github/workflows`), release/version bumps,
  `pkg/*`, `server.json`, `Makefile.cbm`, dependabot, docs/rebrand churn.
- **New languages the fork does not carry** (cfml, cfscript, qml, ObjectScript,
  Perl, Mojo grammar) — see "Permanent fork deferrals" below.
- Large unrelated features: the shared-coordination daemon and its Windows/DACL
  /IPC follow-ups, Windows launcher/install-transaction work, MCP output-format
  migrations, coverage-signal/agent-integration features, test-infra/VM/sharding.
- Vendored-library churn unless it fixes something we actually hit.

When a commit is genuinely borderline, put it in the shortlist marked
`?` and let the user decide — do not silently drop it.

## Procedure

### 1. Enumerate what's new

```bash
git fetch upstream main
.claude/skills/sync-upstream/list-new.sh            # marker auto-read from UPSTREAM_SYNC.md
.claude/skills/sync-upstream/list-new.sh <marker>   # or pass one explicitly
```

The helper dedups **by commit subject**, not SHA — SHAs diverge across the fork,
so a commit we already ported shows up in `MARKER..upstream/main` anyway.

Note the marker may be flagged *partial* in `UPSTREAM_SYNC.md` (it is as of
`97ce23f`). If so, also re-triage the areas the previous pass explicitly skipped
— read the "Skipped" cell of the most recent sync-log row and fold anything now
in scope back into this pass.

### 2. Triage — commit message first

Read **subjects and bodies**, not diffs, for the first pass:

```bash
git log --format='%h %s%n%b%n---' <marker>..upstream/main -- <path>
git show --stat <sha>          # size/shape only, at this stage
```

Group by theme (crash/hang safety → correctness → perf → security). Use
`--stat` to see whether a commit touches first-party source at all; anything
confined to `src/ui`, `graph-ui`, `.github`, `pkg`, `docs` is out by definition.

Fan out to subagents for reading if the range is large, but keep the *decision*
yourself — the point of this skill is curation, not bulk translation.

### 3. Check with the user — REQUIRED GATE

Present a compact table: theme, upstream SHA, one-line intent, why it matters
*here*, rough size (files/LOC). Then a short "skipped" summary grouped by
reason. Ask the user to confirm or amend the shortlist (AskUserQuestion or a
plain question, whichever fits) and **wait**. Do not begin porting until they
answer.

### 4. Port

For each approved commit:

1. **Read the commit message first**, and the linked PR/issue when the message
   is thin: `gh pr view <n> --repo DeusData/codebase-memory-mcp --json title,body`
   (also `gh issue view`). Upstream issue numbers appear as `#NNN` in subjects.
2. **Small, mechanical diff** → apply it: `git diff <sha>^ <sha> -- <path>` and
   port hunk-by-hunk onto our `.cpp`.
3. **Large or heavily-refactored diff → do NOT line-translate.** Understand the
   intent from the message/PR body and **implement it directly** in this fork's
   own C++23 idioms. A faithful re-implementation beats a mechanical port that
   fights context drift.
4. Map paths: upstream `foo/bar.c` → our `foo/bar.cpp`; headers stay `.h`.
5. Keep commits themed — one fork commit per coherent group, crediting the
   upstream SHA(s):
   `merge(upstream): <what> from DeusData/codebase-memory-mcp@<sha>`

#### C11 → C++23 idiom rules

- Compound literals `(const CBMType*[]){a,b,NULL}` are invalid C++ → use
  `cbm_type_args(arena, a, b, NULL)`. Study an already-ported resolver
  (`internal/cbm/lsp/cs_lsp.cpp`, `ts_lsp.cpp`) and mirror it exactly.
- Arena OOM must stay graceful (return NULL / degrade). **Never** introduce
  exceptions or let allocation terminate.
- `_Alignof`→`alignof`, `_Static_assert`→`static_assert`, `_Thread_local`→
  `thread_local`, drop `restrict`, VLAs → fixed/arena buffers, implicit `void*`
  casts → explicit casts, `_Generic` → drop/rewrite.
- Headers consumed by the C-compiled test TUs need `extern "C"` guards — copy
  the guard pattern from sibling headers.
- Vendored code (sqlite3, mongoose, yyjson, lz4, zstd, tre, ts_runtime,
  `grammar_*.c`) **stays C**. Don't C++-ify it.
- Tests under `tests/` compile as C11. Keep them C.

#### Fork-side pitfalls (all have bitten a previous sync)

- **Parallel-path twins.** This fork has fork-only twins of sequential pass
  logic (e.g. `create_env_configures` in `pass_parallel.cpp` vs
  `create_env_configures_for_file`). Parallel is the default above 50 files, so
  an upstream fix applied only to the sequential path is *inert in practice*.
  After porting any `src/pipeline/pass_*.cpp` change, grep `pass_parallel.cpp`
  for the twin and apply it there too.
- **Divergent internals can make an upstream fix a no-op.** The fork's
  authoritative per-file LSP index (`3eed547`) silently swallowed several
  upstream strategies. Before declaring a ported fix "done", prove the new code
  path actually executes here — build the binary and index a fixture.
- **Deliberate absences are not omissions.** If an upstream hunk's context
  contains cfml/cfscript/qml entries missing from our file, preserve the
  absence.
- **A new `#include` can break Windows even when the code is fine.** Upstream's
  portability macros (`CBM_TLS`, …) live in headers that pull `<windows.h>` on
  MinGW, which `#define`s `far`, `near`, `min`, `max`, `small`, `IN`, `OUT`,
  `DELETE`, `interface`. Adding such an include to a TU that has a local named
  `far` breaks a file you never edited — and macOS/Linux CI stays green, so it
  only surfaces in the Windows release job. **Prefer the file's existing
  idiom over upstream's macro**: this tree is C++23, so write `thread_local`
  directly rather than importing `CBM_TLS`. Before adding any foundation
  header to a TU, diff its `#include` block against the last released commit —
  if the set is unchanged, the exposure is unchanged.
- Don't hand-edit `lsp_all.cpp` / `CMakeLists.txt` from a fan-out — central
  build wiring is a single deliberate step.

#### Permanent fork deferrals

No `CBM_LANG_CFML` / `CBM_LANG_CFSCRIPT` / `CBM_LANG_QML`; ObjectScript, Perl,
and the Mojo grammar are likewise not carried. Never re-introduce them, and strip
their rows/tests from anything ported.

### 5. Verify

```bash
scripts/build.sh          # or a non-sanitizer CMake build
scripts/test.sh
```

- **ASan is broken on this Mac** (Apple clang 17 / macOS 26 — `scripts/test.sh`
  hangs at init). Build and validate with `CBM_SANITIZE=OFF` instead.
- **`scripts/lint.sh` needs bash 4+** (`mapfile`; macOS ships 3.2). Reproduce
  its three gates by hand over the changed files:
  `clang-format-20.1.8 --dry-run --Werror`, `cppcheck` in C++ mode, and
  `scripts/check-nolint-whitelist.sh`.
- Record the pass/fail counts and compare against the pre-sync baseline. Port
  the upstream regression tests for the fixes you took — a fix without its guard
  is half-ported.
- For anything claiming a behavior change, **functionally verify against the
  built binary** on a purpose-built fixture (index it, query the graph, show the
  edge/node counts before and after).

### 6. Record

Update `UPSTREAM_SYNC.md`: bump the "Last synced" SHA + date, and add a sync-log
row with (a) what was ported and why, (b) fork-side adaptations and deviations,
(c) **what was skipped and on what grounds** — the skipped list is what makes
the next sync tractable. If the pass was curated (it will be), mark the marker
`partial` so the skipped areas get re-triaged rather than assumed absorbed.

## Anti-patterns

- Starting to edit code before the user approves the shortlist.
- `git merge upstream/main` or blind `git cherry-pick` — the merge base is
  ancient (`a0e809d`); a raw merge replays hundreds of already-ported commits.
- Mechanically line-translating a large diff instead of implementing the intent.
- A giant undifferentiated agent fan-out over every upstream commit. Curate
  first; delegate only well-scoped chunks; hand-implement the big ones.
- Declaring a port done on "it compiles" — verify the path executes.
