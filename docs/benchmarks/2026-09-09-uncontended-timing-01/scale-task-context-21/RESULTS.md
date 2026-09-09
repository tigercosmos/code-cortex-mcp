# Controlled scale and complete-task result

The optimized Code Cortex path passed every preregistered gate on `sim1`.
It used 33.81% of the shell-only pooled time and reduced pooled time by 66.19%.
The paired geometric ratio was 0.3783, which represents a 62.17% reduction.
All 24 pairs favored Code Cortex, and all 48 answers passed exact source checks.

The [primary analyzer report](sim1-evidence-03/attempt-04/analysis-02/report.json)
and [independent cross-check](sim1-evidence-03/attempt-04/analysis-02/independent-cross-check.json)
agree on every reported statistic.

## Primary result

| Scope | Pairs | Geometric ratio | Time reduction |
|---|---:|---:|---:|
| All tasks | 24 | 0.3783 | 62.17% |
| 10K lines of code | 8 | 0.3974 | 60.26% |
| 1M lines of code | 8 | 0.3928 | 60.72% |
| 100M lines of code | 8 | 0.3468 | 65.32% |
| One-shot lookup | 12 | 0.5531 | 44.69% |
| Multi-step call chain | 12 | 0.2587 | 74.13% |

The shell arm used 551.696 seconds across all tasks.
The Code Cortex arm used 186.545 seconds, including MCP retrieval.
The pooled ratio was 0.3381.
The exact one-sided sign-test result was `p = 5.960464477539063e-08`.

Every primary condition was true:

- all 48 scheduled rows were present;
- all 48 answers were source-correct;
- all 24 pairs passed the environment gate;
- both overall ratios were at most 0.90;
- every scale ratio was at most 0.90;
- both task-type ratios were at most 0.90; and
- the exact sign-test result was below 0.05.

## Comparison protocol

The [frozen protocol](sim1-evidence-03/attempt-04/protocol/protocol.json)
used deterministic synthetic C++ repositories at 10K, 1M, and 100M physical lines.
Each scale supplied four defining-signature tasks and four three-hop direct-call tasks.
The protocol contained 24 paired tasks and 48 fresh `gpt-5.6-sol` sessions at medium effort.

Each pair compared two complete task paths.
The shell arm gave the model no retrieval context and required repository shell search.
The Code Cortex arm queried the production MCP server through standard input and output.
The client then supplied the source-audited result to an otherwise identical model session.
The model had no MCP access in either arm.

Task time started before MCP retrieval and ended at the recorded model terminal time.
The time therefore includes retrieval, model startup, reasoning, tool use, and answer generation.
Fixture generation, indexing, source-oracle checks, and the exposure smoke were setup costs.

The schedule placed both arms next to each other for each task.
Twelve pairs ran the shell arm first, and twelve pairs ran the Code Cortex arm first.
Only one timed model session ran at a time.

## Source and tool correctness

The pre-run and post-run source oracles both passed 24 of 24 tasks.
Their files were byte-identical.
The manifest audit checked all 101,010 generated files after the run and found no changes.

All 48 model answers matched the exact expected output.
All 48 sessions reached a terminal state with exit code zero.
The 24 Code Cortex rows received 34,797 context bytes and made no model-side tool calls.
The 24 shell rows received zero context bytes and made 47 shell command calls.

The run retained 886 process and load samples.
The maximum one-minute load was 7.294 against the admitted limit of 8.
No unrelated process reached the 50% CPU exclusion threshold.

The source tree was dirty before validation.
The exact dirty state stayed stable across setup and execution.
Git `HEAD` remained `18002a43cea79ff5396bead762aa05dbba58c4ee`.
The candidate binary SHA-256 remained
`bbfeb274225eaabeea8fac20ccb183647cae33b46f28bf9bd01dfa25307f2e60`.
The [provenance evidence](sim1-evidence-03/attempt-04/analysis-evidence-02/provenance-gate.txt)
also confirms the protocol, harness, setup, and generated-source identities.

## Scale behavior

| Scale | Files | Graph nodes | Graph edges | Index time |
|---|---:|---:|---:|---:|
| 10K | 10 | 223 | 412 | 0.833 s |
| 1M | 1,000 | 22,003 | 41,002 | 5.440 s |
| 100M | 100,000 | 2,200,102 | 4,100,101 | 505.525 s |

All three indexes reported the expected node and edge counts.
No index reported a skipped, partial, or unindexed source file.
The complete setup took 593.524 seconds and ran no model session.
The [setup record](sim1-evidence-03/attempt-04/setup/setup.json) contains each source manifest and index response.

The setup passed `CBM_MEM_BUDGET_MB=4096` to each worker.
The obsolete `CBM_MEMORY_BUDGET_MB` name was absent.
This setting did not impose a 4 GiB process resident-set limit.
The complete setup reached 10,276,764 KiB maximum resident memory, or approximately 9.80 GiB.

## MCP redesign under test

The scale path stores complete extraction results in bounded, run-local snapshot records.
Later resolution passes lease one complete result at a time.
The cache keeps smaller owned registry summaries instead of every complete extraction arena.

`trace_path` now accepts a known source endpoint through `from_function`.
It uses a deterministic targeted forward search and stops at the requested destination.
The `max_work` parameter bounds examined work and reports truncation explicitly.

Traversal passes visited identifiers to SQLite through `json_each`.
This removes the former fixed-size identifier interpolation buffer.
That buffer truncated broad traversals near 4,100 visited nodes.

Empty store lookups now return a null result instead of an allocated zero-length array.
This removes fallback-resolution leaks found by LeakSanitizer.
Project listing also skips count and root queries unless the caller requests details.

The production MCP server exposes the new traversal parameters in `tools/list`.
The benchmark asserted this schema before the timed schedule.

## Validation

All code builds and tests ran on `sim1`, not on the local Mac.

- The production build completed in 24.68 seconds with exit code zero.
- The sanitizer suite reported 5,864 passed tests and one skipped test.
- AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer reported no finding.
- The resolved-call initialization checker reported zero violations.
- Both harness runs passed nine tests, including warnings as errors.
- `git diff --check` passed in the sanitizer, build, and harness gates.

The [sanitizer log](sim1-evidence-02/validation-sanitized-04/test.combined.log),
[production build record](sim1-evidence-02/production-build-01/build.result), and
[final harness record](sim1-evidence-03/harness-validation-05/unittest-resourcewarning.combined.log)
retain the exact outputs.

## Recovery and retained evidence

The resumed work did not modify the interrupted evidence.
The four required input hashes still match the
[preservation record](preserved-input-evidence.json).
The previous draft remains at
[task-context-20](../task-context-20/doc-draft-01/RESULTS-DRAFT.md).

The ENOSPC recovery used new homes, caches, scratch paths, and evidence paths on `sim1`.
The recovery kept every prior temporary directory.
Attempt 01 stopped after an MCP client file-descriptor warning.
Attempt 02 stopped before generation because the controller pre-created the scratch path.
Attempt 03 passed, but its harness used the wrong memory-budget variable name.
Attempt 04 used a corrected variable and a fresh task seed.

The first attempt 04 analyzer invocation also stopped before output creation.
Its output parent did not exist.
The failed invocation remains in `analysis-evidence-01`, and the authorized run remains in `analysis-evidence-02`.

The copied attempt 04 bundle contains 4,572 selected files and 109,084,952 bytes.
Its source and destination manifests match with zero differences.
The [transfer manifest](sim1-evidence-03/DESTINATION_SELECTED_SHA256SUMS) excludes generated scratch data, credentials, private sessions, build objects, and binaries.

## Limits

The benchmark uses synthetic C++ and does not establish results for other languages or real repository structures.
It covers exact definition lookup and a three-hop direct-call chain.
It does not cover open-ended architecture analysis or edits.

The operating-system source cache and graph cache were warm before the schedule.
Complete-task timing excludes index time; the scale table reports it separately.
The Code Cortex arm used client-side MCP retrieval, not a model-side MCP tool call.

Only `sim1` produced results.
All repeated `sim4` SSH probes timed out before a TCP connection completed.
The final six probes also timed out, so no parallel validation ran there.
