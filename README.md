# code-cortex-mcp — Codebase Knowledge Graph MCP Server for AI Coding Agents (C++23)

[![Latest release](https://img.shields.io/github/v/release/tigercosmos/code-cortex-mcp)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://github.com/tigercosmos/code-cortex-mcp)
[![CI](https://img.shields.io/github/actions/workflow/status/tigercosmos/code-cortex-mcp/dry-run.yml?label=CI)](https://github.com/tigercosmos/code-cortex-mcp/actions)
[![Languages](https://img.shields.io/badge/languages-155-orange)](#language-support-155-languages)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)

**code-cortex-mcp** is a fast, local-first **MCP server** ([Model Context
Protocol](https://modelcontextprotocol.io)) that gives AI coding agents — **Claude Code,
Codex CLI, Gemini CLI, Zed, OpenCode, Aider, VS Code**, and any other MCP client — a
persistent **knowledge graph of your codebase**: functions, classes, call graphs, HTTP
routes, and cross-service links, queryable in well under a millisecond. One structural
graph query replaces dozens of grep-and-read cycles (**~120× fewer tokens** on typical
code exploration), and the whole thing ships as a **single static binary** with zero
runtime dependencies.

It full-indexes an average repository in milliseconds and the **Linux kernel (28M LOC,
75K files) in ~3 minutes**, using [tree-sitter](https://tree-sitter.github.io/tree-sitter/)
AST parsing across **155 languages** with LSP-style hybrid type resolution for Go, C, C++,
TypeScript/JavaScript/JSX/TSX, Java, Kotlin, Rust, Python, PHP, and C#.

- [Quick Start](#quick-start)
- [Why code-cortex-mcp?](#why-code-cortex-mcp)
- [How It Works](#how-it-works)
- [MCP Tools](#mcp-tools)
- [Features](#features)
- [Performance Benchmarks](#performance-benchmarks)
- [Language Support](#language-support-155-languages)
- [Graph Data Model & Cypher Queries](#graph-data-model--cypher-queries)
- [Team-Shared Graph Artifact](#team-shared-graph-artifact)
- [Configuration](#configuration)
- [Build from Source](#build-from-source)
- [What This Fork Adds](#what-this-fork-adds)
- [Credits, Citation & License](#credits-citation--license)

## Quick Start

Install the latest release binary and auto-configure every MCP coding agent on your
machine in one step:

```bash
# macOS / Linux
curl -fsSL https://raw.githubusercontent.com/tigercosmos/code-cortex-mcp/main/install.sh | bash
```

```powershell
# Windows (PowerShell)
Invoke-WebRequest -Uri https://raw.githubusercontent.com/tigercosmos/code-cortex-mcp/main/install.ps1 -OutFile install.ps1
.\install.ps1
```

The installer detects **Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Aider,
VS Code**, and more, and wires up MCP server entries, instruction files, and
non-blocking pre-tool hooks. Restart your agent and say **“Index this project.”**

Useful subcommands: `config set auto_index true` (index on session start), `update`,
`uninstall`.

## Why code-cortex-mcp?

- **No LLM, no API keys** — it is the structural backend; your MCP agent (Claude Code,
  etc.) is the language layer. One graph query replaces dozens of grep/read cycles
  (~120× fewer tokens on typical exploration).
- **Single static binary** — 155 tree-sitter grammars compiled in. No Docker, no Node,
  no Python, no runtime dependencies.
- **Everything local & private** — SQLite-backed, persists to
  `~/.cache/code-cortex-mcp/`. Your code never leaves the machine.
- **Plug and play across agents** — `install` auto-detects and configures every
  supported MCP client in one pass.

## How It Works

```
You:    "what calls ProcessOrder?"
Agent:  trace_path(function_name="ProcessOrder", mode="calls")
Engine: runs the graph traversal, returns structured results
Agent:  explains the call chain in plain English
```

The engine indexes in a RAM-first pipeline (LZ4-compressed reads, in-memory SQLite,
single dump at the end; memory is released afterward), then serves queries from a
persistent SQLite graph store.

## MCP Tools

| Tool | Purpose |
|------|---------|
| `index_repository` / `index_status` / `list_projects` / `delete_project` | Index and manage projects (auto-sync keeps them fresh) |
| `search_graph` | Structured search by label, name/file pattern, degree filters |
| `trace_path` | Call-chain traversal (callers / callees / data flow / cross-service) |
| `query_graph` | Read-only Cypher-subset queries |
| `get_code_snippet` | Source for a symbol by qualified name |
| `get_architecture` | Languages, packages, routes, hotspots, clusters, ADRs in one call; opt-in `cycles` aspect reports circular call dependencies |
| `get_graph_schema` | Node/edge counts and property shapes (run first) |
| `search_code` | Graph-augmented grep over indexed files |
| `detect_changes` | Map a git diff to its blast radius — one multi-source traversal to the transitive impact set, plus a module rollup |
| `manage_adr` | Architecture Decision Records (CRUD) |
| `ingest_traces` | Accept runtime traces (stub — counts spans; edge creation not yet implemented) |

Every tool is also available from the CLI:

```bash
code-cortex-mcp cli search_graph '{"name_pattern": ".*Handler.*", "label": "Function"}'
code-cortex-mcp cli query_graph  '{"query": "MATCH (f:Function) RETURN f.name LIMIT 5"}'
```

## Features

- **Call graph & static analysis** — import-aware, type-inferred call graph; dead-code
  detection; Leiden community clusters; circular-dependency detection (SCC condensation
  over the call graph); complexity / bottleneck metrics; git-diff blast-radius analysis;
  Cypher-like queries.
- **Deterministic indexing** — re-indexing the same tree produces a byte-identical graph
  (nodes, labels, edges, and edge directions), independent of worker scheduling. Diff two
  snapshots and only real code changes show up.
- **Crash-isolated indexing** — an index supervisor contains per-file crashes and hangs so
  one bad file cannot take down the whole index run.
- **Preprocessor-aware C/C++** — a second pass over preprocessed source recovers definitions
  whose braces are split across `#ifdef`/`#else` branches (unparseable in raw form) and
  remaps them to their original lines, verifying every line belongs to the main file. Headers
  get their own `File` nodes, `#include` resolves to the header, and benign function-like
  macro calls are not reported as parse gaps.
- **Code search** — semantic similarity edges (algorithmic random-indexing embeddings, no
  API key), BM25 full-text (FTS5, camelCase/snake_case aware), and structural/code search.
- **Cross-service linking** — HTTP route ↔ call-site matching; gRPC/GraphQL/tRPC detection;
  channel detection (`EMITS`/`LISTENS_ON`) for Socket.IO, EventEmitter, and pub-sub.
- **Cross-repo** — `CROSS_*` edges and a combined architecture view across repos in one store.
- **Infrastructure-as-code** — Dockerfiles, Kubernetes manifests, and Kustomize overlays as
  first-class graph nodes.
- **Selected edge types** — `CALLS`, `IMPORTS`, `DEFINES`, `IMPLEMENTS`, `INHERITS`,
  `OVERRIDE`, `HTTP_CALLS`, `ASYNC_CALLS`, `DATA_FLOWS`, `SIMILAR_TO` (MinHash/LSH),
  `SEMANTICALLY_RELATED`. `IMPLEMENTS` vs `INHERITS` is decided by the resolved base's label
  (an `Interface` target means `implements`), and `OVERRIDE` links a method to the base
  method it redefines — for both explicit `implements`/`extends` languages and Go's implicit
  interface satisfaction.
- **Identical graphs from either pipeline** — the sequential and parallel (>50 files) paths
  resolve calls and base-class relations through the same decision points, so a project's
  graph does not depend on which path its size selected.

## Performance Benchmarks

Benchmarked on Apple Silicon (M-series), release build. Full-index rows were re-measured on
v0.12.0 against each project's current `main`:

| Operation | Time |
|-----------|------|
| RocksDB full index (C++, 62K nodes, 346K edges) | ~7 s |
| Django full index (Python, 55K nodes, 371K edges) | ~4 s |
| Redis full index (C, 38K nodes, 148K edges) | ~2.7 s |
| etcd full index (Go, 15K nodes, 94K edges) | ~1.4 s |
| Linux kernel full index (28M LOC, 75K files → 2.1M nodes) | ~3 min |
| Cypher query / trace path | <1–10 ms |
| Dead-code detection (full graph) | ~150 ms |

Edge counts rose substantially for the C/C++ projects in v0.12.0 (RocksDB 218K → 346K, Redis
95K → 148K) as the header `File` nodes, `#include` targets, recovered `#ifdef`-split
definitions, and restored function-pointer/destructor call resolution all landed. Node counts
are essentially unchanged. These are not strict before/after deltas — each project is indexed
at a newer commit than the earlier figures were — but the direction is the corrected graph, not
drift. The Linux kernel row has not been re-measured on v0.12.0.

## Language Support (155 Languages)

155 languages via vendored tree-sitter grammars. Strongest call/type resolution (LSP-style
hybrid) for **Go, C, C++, TypeScript/JavaScript/JSX/TSX, Java, Kotlin, Rust, Python, PHP,
C#**. Benchmarked tiers:

- **Excellent (≥90%)** — C, C++, Lua, Kotlin, Perl, Objective-C, Groovy, Bash, Zig, Swift,
  CSS, YAML, TOML, HTML, SCSS, HCL, Dockerfile
- **Good (75–89%)** — Python, TypeScript, TSX, Go, Rust, Java, R, Dart, JavaScript, Erlang,
  Elixir, Scala, Ruby, PHP, C#, SQL

Plus ~110 more (config, data, and niche languages) parsed structurally.

## Graph Data Model & Cypher Queries

- **Nodes** — `Project`, `Package`, `Folder`, `File`, `Module`, `Class`, `Function`, `Method`,
  `Interface`, `Enum`, `Type`, `Route`, `Resource`
- **Qualified names** — `get_code_snippet` uses `<project>.<path_parts>.<name>`; discover them
  with `search_graph` first.
- **Cypher subset** — `MATCH` / `OPTIONAL MATCH` (labels, relationship types, variable-length
  paths), `WHERE` (comparisons / regex / `CONTAINS` / `EXISTS{}`), `WITH` (+ `DISTINCT`),
  `RETURN` (+ `COUNT`/`COUNT(DISTINCT)` / aggregates), `ORDER BY`, `LIMIT`. Read-only; no
  mutations. Queries are bounded by a 100k-row ceiling and a 30 s wall-clock deadline, so an
  unbounded `OPTIONAL MATCH` fails with an actionable error instead of hanging.

## Team-Shared Graph Artifact

Commit `.code-cortex/graph.db.zst` (a zstd-compressed graph snapshot, typically 8–13:1)
and teammates skip the full reindex: on first run the artifact is imported and incremental
indexing fills in their local diff. A `.gitattributes` `merge=ours` rule is auto-created so the
binary artifact never causes merge conflicts. Optional — gitignore `.code-cortex/` to opt out.

Both export quality levels snapshot the store with `VACUUM INTO` before compressing. The store
runs in WAL mode, so copying its raw bytes could miss committed rows still in the `-wal` or
capture a mid-checkpoint torn page — an artifact that imported as a corrupt cache. The fast
path therefore pays a full consistent copy; correctness beats speed for something teammates
pull. Import additionally runs `PRAGMA quick_check` and refuses a page-corrupted artifact
outright rather than installing it.

## Configuration

```bash
code-cortex-mcp config list
code-cortex-mcp config set auto_index true        # index on MCP session start
code-cortex-mcp config set auto_index_limit 50000 # max files for auto-index
```

- **Index storage** — `~/.cache/code-cortex-mcp/` (override with `CBM_CACHE_DIR`). WAL-mode
  SQLite, ACID-safe across restarts. Reset with `rm -rf ~/.cache/code-cortex-mcp/`.
- **Indexing parallelism** — auto-detected (cgroup-aware on Linux); override with `CBM_WORKERS`
  (range 1–256, invalid values ignored). Useful in containers where the host CPU count differs
  from the effective quota.
- **Ignore rules** — hardcoded patterns → `.gitignore` hierarchy → `.cbmignore` (gitignore
  syntax). Symlinks always skipped.
- **Custom extensions** — map extra extensions to languages via `.code-cortex.json`
  (`{"extra_extensions": {".mjs": "javascript"}}`).

## Build from Source

The build system is **CMake** (C++23). You need a C/C++ compiler (gcc or clang) and zlib.

```bash
git clone https://github.com/tigercosmos/code-cortex-mcp.git
cd code-cortex-mcp
scripts/build.sh                 # production binary → build/c/code-cortex-mcp
./build/c/code-cortex-mcp install   # configure your installed agents
```

Or build, place the binary on your `PATH`, and install the skill in one step:

```bash
make install PREFIX=$HOME/.local # no sudo; binary → ~/.local/bin
sudo make install                # system-wide; binary → /usr/local/bin
```

`make install` builds the binary, installs it under `$(PREFIX)/bin`, then runs the binary's own
`install` to deploy the embedded skill. `/usr/local` needs root, so use `sudo` (the skill step
runs as `$SUDO_USER`, landing in *your* `~/.claude`, not root's) — or set `PREFIX=$HOME/.local` to
skip sudo.

Run the test suite with `scripts/test.sh` (CMake + ASan/UBSan) and the linters with
`scripts/lint.sh`.

## What This Fork Adds

Beyond tracking upstream's indexing, performance, and bug-fix work, this codebase is
maintained independently with its own direction:

- **Modern C++23 codebase, CMake build** — the entire first-party engine is migrated from
  C11 to C++23 (`std::unordered_map`-backed tables, RAII, standard atomics) and built with
  CMake, while staying byte-compatible with the upstream on-disk graph format.
- **Leaner distribution** — every release binary is ~30 MB smaller: the embedded
  pretrained-embedding blob was removed in favor of pure algorithmic random-indexing
  embeddings (no model weights, no API keys), and the optional web UI was retired so there
  is exactly one binary per platform.
- **Tighter supply chain** — no GPL-licensed vendored code, a machine-independent vendored
  checksum audit, a stricter "no network calls in vendored code" rule, and an SBOM that
  matches what actually ships.
- **Release CI that proves the matrix** — build, full test suite, and end-to-end smoke
  (install / update / uninstall, MCP handshake, signature checks, malware scans) genuinely
  run on linux-amd64, linux-arm64, macOS-arm64, macOS-amd64, and Windows for every release.

## Credits, Citation & License

This project was originally forked from
**[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)** (C11),
whose engine, design, and research it builds on — see the preprint *Codebase-Memory:
Tree-Sitter-Based Knowledge Graphs for LLM Code Exploration via MCP*
([arXiv:2603.27277](https://arxiv.org/abs/2603.27277)). The installed binary keeps the
upstream-compatible name `code-cortex-mcp`.

MIT — see [LICENSE](LICENSE). Security policy: [SECURITY.md](SECURITY.md).
