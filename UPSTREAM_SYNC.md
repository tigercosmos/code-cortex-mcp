# Upstream Sync

How to keep this fork (`tigercosmos/cpp-codebase-memory-mcp`, a C++23 port) in
sync with upstream **[`DeusData/codebase-memory-mcp`](https://github.com/DeusData/codebase-memory-mcp)** (C11).

## Last synced

| | |
|---|---|
| **Upstream commit** | `e599df1` (post-v0.8.2, 2026-06) |
| **Synced on** | 2026-06-13 |

Update both fields whenever you pull new upstream work (see the log at the bottom).

## Why you can't just `git merge upstream/main`

Git's merge-base for the two histories is an old commit
(`a0e809d`), so a raw merge tries to replay **hundreds** of commits we already
have (via earlier syncs) and conflicts massively with this fork's structural
changes. **Do not run `git merge upstream/main` or a blind `git cherry-pick`.**
Sync by porting the *genuinely new* upstream commits by hand (procedure below).

### Structural divergences from upstream (the conflict landscape)

These are intentional and permanent; expect every upstream commit that touches
them to need translation, not a clean apply:

- **Language: C11 → C++23.** Every first-party `.c` was renamed to `.cpp`
  (e.g. upstream `src/pipeline/pass_parallel.c` ⇄ our `…/pass_parallel.cpp`).
  An upstream diff to a `.c` file applies to our same-named `.cpp`.
- **Build system: `Makefile.cbm` (deleted) → CMake** (`CMakeLists.txt`,
  `build/c/…`). Upstream Makefile/CI changes have no direct counterpart; the CI
  wrapper scripts (`scripts/{build,test,lint,security}.sh`) drive CMake.
- **C++ idioms** in some TUs (e.g. `std::array`/IIFE tables in `language.cpp`,
  `lang_specs.cpp`; `cbm_type_args()` instead of compound literals in
  `ts_lsp.cpp`/`cs_lsp.cpp`/`generated/*.cpp`) — context around upstream edits
  there will differ.
- **Vendored libraries stay C.** sqlite3, mongoose, yyjson, lz4, zstd, tre,
  ts_runtime, and the tree-sitter `grammar_*.c` are compiled as C (they don't
  port to C++ without forking generated/upstream code). The `grammar_*.c` and
  `ts_runtime.c` are zero-logic `#include` shims.
- **`-Werror` is GCC-only** for first-party C++ (clang's extra warnings aren't
  promoted to errors). Lint uses clang-format **20.1.8** + cppcheck in **C++**
  mode (with C-idiom checks suppressed). Run `scripts/lint.sh` before pushing.
- **Fork rebrand.** All repo URLs point to `tigercosmos/cpp-codebase-memory-mcp`
  (README credits upstream). Skip upstream's release/versioning/`pkg/*` and
  registry (`server.json`) changes unless intentionally re-adopting them.

## Sync procedure

```bash
git fetch upstream main

# 1. List upstream commits NEWER than our last-synced marker that we DON'T
#    already have (compare by message — SHAs diverge across the fork).
MARKER=e599df1   # <- the "Last synced" SHA above
git log --format='%s' "$MARKER"..upstream/main | while IFS= read -r m; do
  git log --format='%s' main | grep -qxF "$m" || echo "NEW: $m"
done
```

For each genuinely-new commit:

1. **Triage.** Skip upstream-only machinery: release/version bumps, `RELEASE_NOTES_*`,
   `pkg/*` publishing, `Makefile.cbm`, CI workflow internals that don't apply.
   Keep substantive **bug fixes**, **perf**, **features**, **test** changes.
2. **Inspect** the diff: `git show <sha>`.
3. **Apply by hand** to our tree — map `.c` → our `.cpp`; adapt context for the
   C++ idioms / `-Werror` / vendored-stays-C rules above. (Cherry-pick usually
   fails on the rename + context drift; manual is more reliable.)
4. **Verify**: `scripts/test.sh` (expect `3629 passed, 1 failed` under ASan —
   the lone failure is the environment-sensitive `test_incremental` RSS check)
   and `scripts/lint.sh` (or at least `clang-format-20.1.8 --dry-run --Werror`
   over the changed files).
5. **Commit** crediting the upstream SHA(s) in the message
   (`merge(upstream): … from DeusData/codebase-memory-mcp@<sha>`).
6. **Update** the "Last synced" SHA/date above and add a row to the log.

## Sync log

| Date | Upstream → | Ported | Skipped |
|------|-----------|--------|---------|
| 2026-06-13 | `e599df1` (post-v0.8.2) | **151 commits triaged → ~90 ported** (net-per-file, partitioned across a 56-agent workflow). **Features:** Java (`a600e80`), Kotlin (`fd6c003`), Rust (`31f7438`) hybrid LSP resolvers (new `.cpp` TUs in `cbm_lsp_tscs`; compound-literal→`cbm_type_args`, grammar externs `extern "C"`); first-party graph-UI `httpd.cpp` replacing mongoose (`630bd40`, `cbm_mongoose` target dropped); computation-bottleneck/complexity metrics + `pass_complexity` (`12683b8`); cross-language IMPORTS/IMPLEMENTS/HANDLES/DECORATES extraction (`7275be3` `6afb454` `65ee5ef` `3c4e7b4` `eb47f86` `35ac60c` `de2c54a` `b30fe13` `5410809` `632af52` `2d772b0` `0d87a86` `861b9d9`). **Correctness/perf:** OOB/UAF fixes (`fb17573` `1d81652` `bb7341d` `0600bc7` `0314a21`); bounded recursion/param/candidate (`9f981a1` `0442cce` `66bf33a`); indexed arch_boundaries + overload resolution (`b904cf0` `02d2774`); edge-props JSON escaping/atomic/overflow (`6c04ab7` `359f37c` `9f465e5` `4b317c8` `8666a99` `7f78f93` `036a80e`); two-phase def registration (`3951786`); gbuf interning + streaming SQLite writer + page reclaim/back-pressure (`6ed58bc` `894932c` `2d82022` `22743b0` `7b67cf9`); excluded-subtree report (`ba52d6b`); parent-death watchdog + CBM_LOG_LEVEL (`de1d58a`); workspaces IMPORTS (`586729b`); get_architecture counts (`08115fd`); Windows path/temp fixes. **Vendored (verbatim):** grammar refresh (`0956f37`), mimalloc 3.2.8→3.3.2 (`eab4148`), nim drop (`3ee9d5d`), license coverage (`d57bbe6` `73f859a` `4451ca0` `f714bf3`). 26 new test suites + fork-divergent test merges. **Verified:** non-sanitizer build, **5606 passed / 0 failed**, clang-format-20.1.8 clean. **Deviations:** CFML/QML/CFScript still deferred (grammars + MANIFEST rows + test cases stripped); the `#283` search_code regex-validation deviation from the prior sync stands. **Post-review fork fixes (intentional deviations from upstream — re-porting upstream's version of these files regresses them):** (1) synthesized topic-Route `nodes.properties` now valid JSON `{"broker":..,"topic":..}` not a bare broker token (`pipeline.cpp`); (2) env-access EnvVar/CONFIGURES also materialized in the parallel path (new `create_env_configures` in `pass_parallel.cpp`, twin of the sequential `create_env_configures_for_file`); (3) leading YAML `---` document marker skipped rather than treated as end-of-document in the k8s label scan (`pass_k8s.cpp`); (4) out-of-line / cross-file method parents (Go methods, C++ .h/.cpp split) resolved via a unique-by-name fallback `cbm_pipeline_resolve_method_parent` (`pass_definitions.cpp` + `pass_parallel.cpp`); (5) all members of a mutual-recursion cycle marked `recursive` via a DFS path-stack, not just the on-stack node (`pass_complexity.cpp`). The watcher `git -C` popen review finding needed no change — `cbm_validate_shell_arg` already rejects shell metacharacters at the `cbm_watcher_watch` entry point. | Release/version bumps + `pkg/*` publishing (`972577145` `3bbb6bb` `83eb998` `eb748cb` `92f38b6` `324b6fc` `f0c9be1`); MCP-Registry/Glama (`9507c4b` `fa10216` `16103d9` `9dbed50` `f714bf3` glama parts); CI workflow internals (`953844` `49bb9f6` `93e4f03` `0240d89` `e5ea9ea` `2003821` `bd6d617` `4630af6` `54c0b87` `eb2e8d5` `578ac55` `3bbb6bb`); license/DCO/community gates (`e93d86d` `c72d856` `952eea9` `1b383e3` `2a0ec32` `b129aab` `3d9917f` `487f3f9` `4fa71d9` `e599df1` `3b0ce55`); rebrand-divergent README/docs (`0338685` `4b46639` `e46d512` `16034f21` `244039e` `ad5d45a` `20bf70e`); smoke-test-only tweaks (`7990c6f` `f67799` `e83bd73` `bb4af38` `8d6dfc5` `4ce0305`) |
| 2026-06-03 | `64be280` (post-v0.7.0) | **~40 commits.** Cypher engine: WITH DISTINCT (`3014867`), `WHERE n:Label` (`b30e6d6`), label alternation `(n:A\|B)` (`2a5515e`), `COUNT(DISTINCT)` (`869341c`), resync-on-unsupported (`a2cd1e7`), dead-store cleanup (`3be9ace`), scalar/entity fns (`71a6c57`), string fns + fail-loud (`67a7334`), multi-arg scalar fns (`c160465`), `EXISTS{}` predicate (`08b62f0`). Arch/store: Louvain→multi-level **Leiden** (`955b87d`) + surface clusters in get_architecture (`d87cffe`), drive-letter integrity (`c1dabc9`), file_pattern path-substring #200 (`53be053`). MCP/search: JSON-RPC string ids #253 (`4abcaa4`), manage_adr→SQLite #256 (`70e3ed7`), search_code `&`/literal-`\|`/timing (`09e3f71`), `--ui` warn #350 (`6657044`). Extract: JS/TS arrow factory methods (`eb50569`), C/C++ macros→Macro nodes (`10ce652`), GDScript/PS/Luau + PS fn names (`f42d772`), HCL block labels (`2cad2fe`), R `box::use` imports (`1a636f1`). Helm: named templates + include calls (`a8fc901`), Chart.yaml DEPENDS_ON + values.yaml top-level (`1b308ec`) — both via the existing gotemplate grammar. Pipeline/discover/fqn/cli: gRPC Route stub-suffix #294 (`5f3847c`), validator-safe project names #349 (`5595ce4`), `.mjs/.cjs/.mts/.cts` #197 (`2de984b`), `.blade.php`→Blade #258 (`db6e99c`), Windows PATHEXT #221 (`0485d3f`). Install/cli: `install --plan` receipt #388 (`4c38c5a`), Cursor IDE detect #222 (`f88d031`), fish PATH #319 (`30dfa95`), Antigravity paths (`13bd108`), SessionStart reminders #330 (`6c71244`). Tests: regression guards (`7e4745d` `bf3fc71` `48a9b2c` `6840457` `f5573f1`). **Deviations:** (1) **#283 search_code invalid-regex error NOT ported** — upstream validates the user regex up front with TRE-backed `cbm_regcomp` (same dialect as `grep`); this fork backs `cbm_regcomp` with `std::regex::extended` (strict POSIX ERE, rejects `\s`/`\w`/`\d`) while the real search shells out to system `grep`, so the up-front check would falsely reject valid GNU-grep patterns. Dropped the validation + its test, kept the independent `path_regex` leak fix. (2) **CFML/QML languages deferred** (user scope decision) — ~1M lines of vendored generated grammars; `cbm.h`/`lang_specs.c`/`language.c`/`extract_defs.c`/test CFML/QML hunks intentionally not applied. Verified: g++-15 unavailable locally (Homebrew ld/SDK breakage), built+tested under Apple-clang non-sanitizer (CBM_SANITIZE=OFF) — **3703 passed, 0 failed**; clang-format-20.1.8 clean. | CFML (`1e1d408`) + QML (`f16f9dc`) languages + `600fe90` lang-count doc; `efec2ce`/`023d92c` README/landing-page (rebrand-divergent — fork README lacks the section); `64be280` vitest bump (graph-ui); `d67329e` smoke-test Antigravity; `7fad5bc` CI dry-run; `d198403`+`9dcc0e3` net-zero Windows CI repro harness; `61453e9` hooks-gate test (shell); scripts/smoke-test.sh hunks |
| 2026-05-31 | `d0e1e77` (post-v0.7.0) | **22 commits**: c-lsp crash/eval-cap fixes (`05f0a26` `926eb7f` `d895c16`); install `$CLAUDE_CONFIG_DIR` (`dedd33d`); security/correctness hardening (`21c73b5`); store WAL checkpoint (`a6ad401`); MCP ping (`9d6e28b`); platform `CBM_WORKERS` + cgroup detection (`d952238` `a5a3d1d`); pkgmap workspace-imports + SvelteKit routes (`9bcfaab` `fcf98ac` `bcf73b2`); arena NULL-guard + vendored-skip (`9e2bb92`); extract_channels growable stack (`4d84406`); CLOCK_MONOTONIC (`2e110fc`); Windows path/git-log fixes (`2389d82` `df38e33`); tests (`7dafde4` `ba49137` `de069d1` `d848347` `34864c8`); CI bumps (`5c743e8` `694652f` `4fd6e1b`); stale-doc deletions. **Deviation:** repo-wide pkgmap manifest walk is POSIX-only (Windows no-op) — matches upstream `bcf73b2` (the unconditional walk hung Windows CI on directory-junction cycles); Windows workspace-import resolution awaits a junction-safe rewrite upstream. | `afd98bf` README rebrand-divergent; `c6c1d77`+`6c6b8c8`+`d0e1e77` net-zero temp bucket-B CI harness; pkg/version bumps |
| (earlier) | `9a9488f`-era | full upstream history up to the "pad source read buffers / gate LSP benchmarks" era, brought in before the C++ migration | — |
