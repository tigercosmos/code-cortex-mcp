# code-cortex-mcp

[![Latest release](https://img.shields.io/github/v/release/tigercosmos/code-cortex-mcp)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://github.com/tigercosmos/code-cortex-mcp)
[![CI](https://img.shields.io/github/actions/workflow/status/tigercosmos/code-cortex-mcp/dry-run.yml?label=CI)](https://github.com/tigercosmos/code-cortex-mcp/actions)
[![Languages](https://img.shields.io/badge/languages-155-orange)](#language-support)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)

**Complete AI coding tasks ran 57.7% to 59.4% faster than shell tools across
measured codebases from 10K to 100M lines. They ran 36.9% to 37.8% faster than
upstream codebase-memory-mcp at 10K and 1M lines.**

| Codebase size | Faster than shell | Faster than upstream |
|---:|---:|---:|
| 10K lines | **57.7%** (8/8 pairs) | **37.8%** (8/8 pairs) |
| 1M lines | **56.1%** (6/8 pairs) | **36.9%** (6/8 pairs) |
| 100M lines | **59.4%** (8/8 pairs) | Unavailable: upstream exceeded the 4 GiB and 32 GiB memory budgets |

The values include only terminal, source-correct, exact task pairs. Two Code Cortex answers
were not exact, so the 1M results contain six of eight pairs. See
[Complete-task performance](#complete-task-performance) for the method and limits.

**code-cortex-mcp** is a local [MCP](https://modelcontextprotocol.io) server for AI coding
agents. It builds a knowledge graph of your codebase: functions, classes, call graphs, HTTP
routes, and cross-service links. Graph queries can replace repeated grep-and-read cycles when
the task depends on indexed relationships. It ships as a single static binary with no runtime
dependencies.

It parses 155 languages with [tree-sitter](https://tree-sitter.github.io/tree-sitter/)
and resolves types for Go, C, C++, TypeScript/JavaScript, Java, Kotlin, Rust, Python, PHP,
and C#. It indexes llvm-project (47M lines) in 6.4 minutes and the Linux kernel (44M lines)
in 4.4 minutes on a 32-core machine. On 11 other repositories it indexes 1.1× to 5.7× faster
(median 2.4×) than [codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp),
the project it forked from. See
[Compared to codebase-memory-mcp](#compared-to-codebase-memory-mcp).

- [Quick Start](#quick-start)
- [MCP Tools](#mcp-tools)
- [Features](#features)
- [Architecture](#architecture)
- [Performance](#performance)
- [Language Support](#language-support)
- [Graph Data Model](#graph-data-model)
- [Configuration](#configuration)
- [Build from Source](#build-from-source)
- [Compared to codebase-memory-mcp](#compared-to-codebase-memory-mcp)
- [Credits & License](#credits--license)

## Quick Start

```bash
# macOS / Linux
curl -fsSL https://raw.githubusercontent.com/tigercosmos/code-cortex-mcp/main/install.sh | bash
```

```powershell
# Windows (PowerShell)
Invoke-WebRequest -Uri https://raw.githubusercontent.com/tigercosmos/code-cortex-mcp/main/install.ps1 -OutFile install.ps1
.\install.ps1
```

The installer configures Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Aider, VS Code,
and other MCP clients: server entries, instruction files, and hooks. Restart your agent and
say "Index this project."

In Claude Code the graph mostly arrives as context rather than as a tool the agent has to
choose: a SessionStart hook prints a short architecture brief for the repository you are in,
a PreToolUse hook on searches adds what grep cannot show for an exact symbol (definition vs
declaration, caller and test counts with call-site lines, callers from other languages), and
a PostToolUse hook on edits reports the blast radius of the file you just changed. All hooks
are non-blocking and bounded (300 ms for searches, 1.5 s after edits, 3 s at session start).

Other subcommands: `doctor` (checks the install, hooks, and whether the current directory is
indexed), `config set auto_index true`, `update`, `uninstall`.

It needs no LLM and no API key. The server is the structural backend; your agent is the
language layer. All data stays in `~/.cache/code-cortex-mcp/` as SQLite.

## MCP Tools

| Tool | Purpose |
|------|---------|
| `index_repository`, `index_status`, `list_projects`, `delete_project` | Index and manage projects |
| `inspect_symbol` | One call: definition with source head, direct callers with call-site lines and resolver confidence, related tests, cross-language callers, subclasses, a complete caller-file list, and trust signals (index freshness, partial-parse coverage) |
| `search_graph` | Search by label, name pattern, file pattern, or degree |
| `trace_path` | Callers, callees, data flow, and cross-service chains; entries carry file, lines, and call sites |
| `query_graph` | Read-only Cypher-subset queries |
| `get_code_snippet` | Source of a symbol by qualified name |
| `get_architecture` | Languages, packages, routes, hotspots, clusters, cycles, ADRs |
| `get_graph_schema` | Node and edge counts, property shapes |
| `search_code` | Graph-augmented grep over indexed files |
| `detect_changes` | Blast radius of a git diff |
| `manage_adr` | Read or replace Architecture Decision Records (`get`, `update`, `sections`) |
| `ingest_traces` | Runtime traces (stub: counts spans only) |

The `project` argument is optional for every query tool: inside an indexed repository it is
inferred from the working directory, and a name that matches nothing is answered for that
project with a `project_note` saying so. Names may be bare (`parse`), scoped
(`Packet::addLayer`), or fully qualified. Replies are bounded (`max_bytes` on
`inspect_symbol`, `trace_path`, and `search_code`). Each bounded reply says when it was cut
and how to continue.

Every tool also runs from the CLI:

```bash
code-cortex-mcp cli inspect_symbol --symbol hexStringToByteArray
code-cortex-mcp cli search_graph --name-pattern '.*Handler.*' --label Function
code-cortex-mcp cli trace_path --function-name B --from-function A --direction inbound --depth 3 --max-work 10000
code-cortex-mcp cli query_graph --query 'MATCH (f:Function) RETURN f.name LIMIT 5'
```

For a known call chain from `A` to `B`, use the bounded `trace_path` form above. Check
`path_found` before using the returned path. If `traversal_truncated` is true, increase
`max_work` or narrow the endpoint names. The targeted form supports `CALLS` edges,
`mode="calls"`, and `direction="inbound"`.

## Features

- **Static analysis** — import-aware, type-inferred call graph; dead code; Leiden clusters;
  circular dependencies; complexity metrics; git-diff blast radius.
- **Crash isolation** — a supervisor contains per-file crashes and hangs during indexing,
  and a persistent worker process answers tool calls with a per-tool deadline.
- **Preprocessor-aware C/C++** — recovers definitions split across `#ifdef` branches,
  and gives headers their own `File` nodes with resolved `#include` edges.
- **Code search** — BM25 full text (FTS5), structural search, and semantic similarity
  edges from algorithmic embeddings (no API key).
- **Cross-service links** — HTTP route ↔ call site; gRPC, GraphQL, tRPC; Socket.IO and
  pub-sub channels (`EMITS` / `LISTENS_ON`); `CROSS_*` edges across repositories.
- **Infrastructure as code** — Dockerfiles, Kubernetes manifests, and Kustomize overlays
  as graph nodes.
- **Reproducible index** — two full runs over the same tree produce the same graph: same
  nodes, edges, IDs and properties, byte for byte. Verified on 11 of the benchmark
  repositories.
- **Team artifact** — commit `.code-cortex/graph.db.zst` (a zstd snapshot, about 10:1)
  and teammates import it instead of a full reindex. A `.gitattributes` `merge=ours` rule
  prevents merge conflicts. Gitignore `.code-cortex/` to opt out.

## Architecture

Code Cortex separates indexing from task-time retrieval. The indexing pipeline discovers
files, parses syntax, resolves calls and types, builds graph edges, and writes SQLite. The
MCP server then serves bounded graph queries from the indexed database.

The scale redesign adds two bounded paths. `CBM_RESULT_STORE=1` moves full extraction
results to temporary storage and keeps compact summaries in memory. Targeted `trace_path`
queries search from one known endpoint to another with an explicit edge-work limit. See
[Architecture and scale](docs/ARCHITECTURE.md) for the data flow, limits, and before/after
comparison.

## Performance

### Complete-task performance

A controlled, uncontended Linux run on 2026-09-10 compared complete tasks with the current
Code Cortex MCP, pure shell tools, and pinned upstream codebase-memory-mcp. Lower ratios are
faster. Each total contains only matched pairs for which both answers were terminal,
source-correct, and exact; failed answers were not retried or assigned an estimated time.

| Comparison | Eligible pairs | Matched task time | Geometric time ratio | Reduction |
|---|---:|---:|---:|---:|
| Code Cortex / shell | 22 / 24 | 213.806 s / 505.603 s | **0.421** | **57.9%** |
| Code Cortex / upstream | 14 / 16 | 142.740 s / 207.004 s | **0.626** | **37.4%** |
| Upstream / shell | 16 / 16 | 253.507 s / 376.071 s | **0.712** | **28.8%** |

| Comparison | 10K lines | 1M lines | 100M lines | One-shot | Multi-step |
|---|---:|---:|---:|---:|---:|
| Code Cortex / shell | 57.7% (8/8) | 56.1% (6/8) | 59.4% (8/8) | 33.3% (12/12) | 75.7% (10/12) |
| Code Cortex / upstream | 37.8% (8/8) | 36.9% (6/8) | unavailable | 33.3% (8/8) | 42.6% (6/8) |
| Upstream / shell | 31.9% (8/8) | 25.6% (8/8) | unavailable | 2.2% (8/8) | 48.2% (8/8) |

The run used fresh `gpt-5.6-sol` sessions at medium reasoning effort and warm synthetic C++
indexes. It serialized all 64 sessions on the same 32-core host and included MCP startup,
initialization, tool discovery, retrieval, the candidate's post-retrieval topology scan, and
the model session in complete-task time. Fixture generation and indexing were outside the task
timer. The candidate was `0aa36e53`; upstream was `1db8bace`. Candidate and upstream used
task-equivalent one-call adapters and supplied byte-identical normalized context on all 16
shared tasks.

Code Cortex completed 22 of 24 answers exactly; its two incorrect 1M multi-step responses had
source-correct retrieval but added text to required symbol names. Shell completed 24/24 and
upstream completed 16/16. Upstream has no 100M timing: indexing exceeded both sealed 4 GiB and
32 GiB memory budgets, and no time was imputed. These results establish the stated benefit for
the measured warm-index lookup and three-hop-chain tasks, not for edits, cold indexing, every
language, or every task shape.

### Full-index performance

Full-index wall clock, median of three runs from an empty cache, on an Apple M3 Max (14 cores,
36 GB) and a 32-core Linux machine (62 GB).

| Repository | Lines | Nodes / edges | M3 Max | 32-core Linux | Peak RAM |
|-----------|------:|---|------:|------:|------:|
| etcd (Go) | 0.3M | 15K / 95K | 1.4 s | 1.2 s | 0.7 GB |
| Django (Python) | 1.1M | 55K / 372K | 4.8 s | 3.8 s | 1.9 GB |
| CPython (C, Python) | 3.3M | 137K / 1.0M | 14 s | 12 s | 4.3 GB |
| Kubernetes (Go) | 7.3M | 288K / 3.3M | 56 s | 57 s | 4.9 GB |
| Elasticsearch (Java) | 8.8M | 692K / 5.2M | 94 s | 63 s | 7.9 GB |
| llvm-project (C++) | 46.8M | 2.2M / 7.8M | 487 s | 382 s | 12 GB |
| Linux kernel (C) | 43.8M | 4.7M / 11.6M | 368 s | 264 s | 13.4 GB |

### Memory

Peak RAM is the peak resident set size on the M3 Max at 14 workers, median of three runs
(llvm-project two, the kernel one). It follows how much the extractors produce rather than
repository size — CPython needs 4.3 GB against Kubernetes' 4.9 GB with less than half the
lines — and stays between 0.7 and 13.4 GB across every repository measured. Indexing is the expensive
phase; answering queries afterwards reads the SQLite file and needs almost none of it.

The peak is not the graph. By default, the pipeline keeps each file's definitions, calls, and
usages from extraction until call resolution ends: about six times the graph buffer on
Elasticsearch. On top of that sits the parse working set of the files being read at that
moment, which is what `CBM_WORKERS` moves. Set `CBM_RESULT_STORE=1` to serialize full per-file
results to bounded system temporary storage after extraction and keep compact registry
summaries in memory; resolution loads each full result on demand. This store is not durable.
The per-run storage limit is 64 GiB. Make sure that the system temporary volume has enough
free space before you enable the store. A failed store write, including an `ENOSPC`
out-of-space error, cancels the indexing run. Free temporary-volume space before retrying.

The indexer throttles workers when resident memory passes a budget derived from total RAM
(25–50%, with a larger share on larger machines; `CBM_MEM_BUDGET_MB` overrides it in MiB).
The budget is a soft back-pressure target, not a hard RSS limit: the resident graph, in-flight
work, or allocator behavior can exceed it. `CBM_MEM_PROFILE=1` logs a byte-level breakdown at
every phase boundary.

Query latency:

| Operation | Time |
|-----------|------|
| `search_graph`, `trace_path` (warm) | 0.1–0.5 ms |
| `get_code_snippet` (warm) | ~2 ms |
| `inspect_symbol` (warm, 40 callers) | ~10 ms |
| PreToolUse hook (`Grep`, `Bash` search, or `Read`) | 10–25 ms |
| PostToolUse hook (`Edit`/`Write`) | ~10 ms |
| SessionStart brief | ~130 ms |

Two mechanisms keep calls fast. A persistent worker process serves tool calls, so each call
skips a process exec and a database open. A memo in `_config.db` records each database's
integrity verdict against its `(size, mtime)`, so a cold process such as a hook does not
verify databases again.

| Variable | Effect |
|----------|--------|
| `CBM_TOOL_SERVER=0` | One worker process per tool call (the Windows default) |
| `CBM_TOOL_SUPERVISOR=0` | Run tools in-process, without isolation |
| `CBM_STORE_META=0` | Disable the memo; every lookup verifies again |
| `CBM_RESULT_STORE=1` | Spill full extraction results to bounded system temporary storage |
| `CBM_UPDATE_CHECK=0` | Disable the background GitHub release check at MCP initialization |

## Language Support

155 languages via vendored tree-sitter grammars. Benchmarked tiers:

- **Excellent (≥90%)** — C, C++, Lua, Kotlin, Perl, Objective-C, Groovy, Bash, Zig, Swift,
  CSS, YAML, TOML, HTML, SCSS, HCL, Dockerfile
- **Good (75–89%)** — Python, TypeScript, TSX, Go, Rust, Java, R, Dart, JavaScript, Erlang,
  Elixir, Scala, Ruby, PHP, C#, SQL

The other 110 languages get structural parsing only.

## Graph Data Model

- **Nodes** — `Project`, `Package`, `Folder`, `File`, `Module`, `Class`, `Function`, `Method`,
  `Interface`, `Enum`, `Type`, `Route`, `Resource`
- **Edges** — `CALLS`, `IMPORTS`, `DEFINES`, `IMPLEMENTS`, `INHERITS`, `OVERRIDE`,
  `HTTP_CALLS`, `ASYNC_CALLS`, `DATA_FLOWS`, `SIMILAR_TO`, `SEMANTICALLY_RELATED`, and more
- **Qualified names** — `<project>.<path_parts>.<name>`; find them with `search_graph`.
- **Cypher subset** — `MATCH` / `OPTIONAL MATCH`, `WHERE`, `WITH`, `RETURN` with aggregates,
  `ORDER BY`, `LIMIT`. Read-only. Queries stop at 100k rows or 30 s.

## Configuration

```bash
code-cortex-mcp config list
code-cortex-mcp config set auto_index true        # index on MCP session start
code-cortex-mcp config set auto_index_limit 50000 # max files for auto-index
code-cortex-mcp config set auto_watch false       # default: true
code-cortex-mcp config set ui-lang en              # auto, en, or zh; default: auto
```

- **Storage** — `~/.cache/code-cortex-mcp/`; override with `CBM_CACHE_DIR`.
- **Parallelism** — auto-detected (cgroup-aware); override with `CBM_WORKERS` (1–256).
- **Ignore rules** — `.gitignore`, then `.cbmignore` (gitignore syntax). The indexer skips symlinks.
- **Custom extensions** — `.code-cortex.json`: `{"extra_extensions": {".mjs": "javascript"}}`.
- **Memory** — indexing is the memory-hungry phase; see [Memory](#memory). `CBM_MEM_BUDGET_MB`
  caps the budget the indexer throttles against, and `CBM_MEM_PROFILE=1` logs a byte-level
  breakdown at each phase boundary.

## Build from Source

Requires CMake, a C++23 compiler, and zlib.

```bash
git clone https://github.com/tigercosmos/code-cortex-mcp.git
cd code-cortex-mcp
scripts/build.sh                    # → build/c/code-cortex-mcp
./build/c/code-cortex-mcp install   # configure your agents
```

Or install the binary and the skill in one step:

```bash
make install PREFIX=$HOME/.local    # binary → ~/.local/bin
sudo make install                   # binary → /usr/local/bin
```

Run tests with `scripts/test.sh` (ASan/UBSan) and linters with `scripts/lint.sh`.

## Compared to codebase-memory-mcp

code-cortex-mcp forked from
**[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)** and keeps
its on-disk graph format. Historical cold-index and warm-tool measurements show lower
times for most measured operations. The [complete-task comparison](#complete-task-performance)
measures the newer agent-task boundary.
codebase-memory-mcp has features that code-cortex-mcp does not; the feature table names them.

Test conditions for every number in this section:

- **Date and machines** — 2026-08-25; an Apple M3 Max (14 cores, 36 GB, macOS) and a
  32-core x86-64 Linux machine (62 GB, Ubuntu 24.04, gcc 13.3). The tool-call and hook
  latency rows come from the Mac only.
- **Versions** — code-cortex-mcp at `af4579de` plus the llvm-project crash fix committed with
  these results; codebase-memory-mcp at `010569fa`.
- **Binaries** — each project's own `scripts/build.sh`, without the codebase-memory-mcp web UI.
- **Cache** — one empty cache directory per run.

### Indexing speed

Median of three full-index runs from an empty cache on 13 repositories, from Redis (0.6M lines)
to llvm-project (46.8M lines).

| | Apple M3 Max (14 cores, 36 GB) | 32-core Linux (62 GB) |
|---|---|---|
| Repositories both engines complete | 11 of 13 | 11 of 13 |
| Speedup, median | **2.4×** | **2.4×** |
| Speedup, range | 1.1× (Kubernetes) – 5.7× (etcd) | 1.1× (Kubernetes) – 4.4× (etcd) |
| etcd (Go, 0.3M lines) | **1.42 s** vs 8.11 s | **1.15 s** vs 5.08 s |
| CPython (C, Python, 3.3M lines) | **14.4 s** vs 47.6 s | **11.9 s** vs 28.2 s |
| PyTorch (C++, Python, 5.1M lines) | **31.9 s** vs 77.8 s | **25.8 s** vs 70.3 s |
| Elasticsearch (Java, 8.8M lines) | **94.0 s** vs 112.8 s | **63.1 s** vs 116.2 s |
| llvm-project (C++, 46.8M lines) | **487 s** vs crash | **382 s** vs crash |
| Linux kernel (C, 43.8M lines) | **368 s** vs stopped | **264 s** vs out of memory |

Both engines build graphs of nearly the same node count on every repository except Rails
(100,649 nodes against 64,354). Edge counts differ more on Kubernetes (3.3M against 2.0M) and
Elasticsearch (5.2M against 5.7M), so read those ratios with that in mind.

Both engines crashed on llvm-project with SIGSEGV in the index worker. The cause is an
out-of-bounds read in structured-binding decomposition in the C/C++ resolver, inherited from
the common code. A NULL dereference in the preprocessor wrapper crashes the same run. This
release fixes both in code-cortex-mcp. On the Linux kernel, codebase-memory-mcp's worker grew
to 57 GB resident in 90 s, and the 62 GB machine killed it. On the Mac, we stopped the run
after 10 minutes with 19 GB of swap in use.

Embeddings do not explain the gap. In `fast` mode, which writes no similarity or semantic
edges, Django takes 2.9 s in code-cortex-mcp and 12.0 s in codebase-memory-mcp (Mac, measured
at `7a3196c4`). The gap is in extraction and resolution: codebase-memory-mcp runs the
`CALL_REFERENCE` and `USAGE` precision passes, and code-cortex-mcp carries its own resolver
and pipeline optimizations.

### Tool-call latency

Median of 20 warm calls over MCP stdio against the indexed Django graph (55K nodes, 371K
edges). The response column is the JSON-RPC response size.

| Tool | code-cortex-mcp | Response | codebase-memory-mcp | Response |
|---|---|---|---|---|
| `search_graph` | **5.1 ms** | 20.4 KB | 18.2 ms | 1.8 KB |
| `query_graph` | **1.6 ms** | 0.4 KB | 15.8 ms | 0.2 KB |
| `list_projects` | **0.2 ms** | 0.5 KB | 13.7 ms | 1.3 KB |
| `search_code` | **239.8 ms** | 5.6 KB | 271.3 ms | 1.5 KB |
| `get_graph_schema` | **406.3 ms** | 10.9 KB | 480.9 ms | 11.0 KB |
| `index_status` | 60.6 ms | 31.2 KB | **20.0 ms** | 30.1 KB |
| `get_architecture` | 251.0 ms | 117.1 KB | **54.7 ms** | 1.7 KB |

codebase-memory-mcp returns a compact tree format; code-cortex-mcp returns JSON. The
`get_architecture` row therefore measures different amounts of output (117 KB against
1.7 KB). `index_status` returns the same amount on both sides and is slower in
code-cortex-mcp.

code-cortex-mcp serves each call from a supervised worker process; codebase-memory-mcp
serves it in-process. The worker adds crash isolation and a per-tool deadline for about half
a millisecond. `search_graph` measures 4.2 ms with `CBM_TOOL_SUPERVISOR=0` and 4.8 ms
through the worker.

### Cold call from an agent hook

A hook has no MCP session, so it starts one CLI process per call. Median of five sequential
`cli search_graph` calls against a cache that holds the 176 MB Django database.

| Path | Time |
|---|---|
| code-cortex-mcp, memo hit | **0.02 s** |
| code-cortex-mcp, first call after the database changes | 0.74 s |
| codebase-memory-mcp, default | 4.4 s |
| codebase-memory-mcp, after `daemon start` | 1.6 s |

The `_config.db` memo stores each database's integrity verdict against its `(size, mtime)`,
so only the first call after a change verifies again. codebase-memory-mcp starts a
coordination daemon for every CLI command unless `daemon start` keeps one warm.

### Features

| Area | code-cortex-mcp | codebase-memory-mcp |
|---|---|---|
| Language and build system | C++23, CMake | C11, Make |
| Binary size (macOS arm64, no UI) | 218 MB | 283 MB |
| Semantic embeddings | algorithmic random indexing | 31 MB pretrained vector blob, random indexing as fallback |
| Tool-call isolation | persistent supervised worker, per-tool deadline | in-process |
| Integrity memo for cold starts | `_config.db` | none |
| Languages | 155 | 158 (adds CFML, CFScript, QML, ObjectScript) |
| Hybrid LSP resolvers | 10 languages | 11 languages (adds Perl) |
| MCP tools | 15 | 15 |
| Reference precision (`CALL_REFERENCE` / `USAGE`) | no | yes |
| Incremental reindex | yes | yes, plus delta staging (clone, patch, rename) |
| Session coordination daemon | no | yes |
| 3D graph web UI | no | yes, on `localhost:9749` |
| Compact tree output format | no, JSON | yes |
| Agent surfaces configured by `install` | 13 | 43 |

Both engines share the tree-sitter frontend, graph schema, on-disk format, Cypher subset,
C/C++ preprocessor pass, crash-isolated index supervisor, infrastructure-as-code nodes,
cross-service linking, and team artifact export.

code-cortex-mcp also audits vendored checksums and ships an SBOM that matches each release.
Its release CI runs the build, the tests, and an end-to-end smoke test on linux-amd64,
linux-arm64, macOS-arm64, macOS-amd64, and Windows.

## Credits & License

Forked from **[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)**
(C11), whose engine, design, and research it builds on. See the preprint *Codebase-Memory:
Tree-Sitter-Based Knowledge Graphs for LLM Code Exploration via MCP*
([arXiv:2603.27277](https://arxiv.org/abs/2603.27277)).

MIT — see [LICENSE](LICENSE). Security policy: [SECURITY.md](SECURITY.md).
