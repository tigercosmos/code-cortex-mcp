# code-cortex-mcp

[![Latest release](https://img.shields.io/github/v/release/tigercosmos/code-cortex-mcp)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://github.com/tigercosmos/code-cortex-mcp)
[![CI](https://img.shields.io/github/actions/workflow/status/tigercosmos/code-cortex-mcp/dry-run.yml?label=CI)](https://github.com/tigercosmos/code-cortex-mcp/actions)
[![Languages](https://img.shields.io/badge/languages-155-orange)](#language-support)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/tigercosmos/code-cortex-mcp/releases/latest)

**code-cortex-mcp** is a local [MCP](https://modelcontextprotocol.io) server for AI coding
agents. It builds a knowledge graph of your codebase: functions, classes, call graphs, HTTP
routes, and cross-service links. One graph query replaces dozens of grep-and-read cycles.
It ships as a single static binary with no runtime dependencies.

It parses 155 languages with [tree-sitter](https://tree-sitter.github.io/tree-sitter/)
and resolves types for Go, C, C++, TypeScript/JavaScript, Java, Kotlin, Rust, Python, PHP,
and C#. It indexes the Linux kernel (28M LOC) in about 3 minutes. It indexes a repository
3.2× to 5.9× faster than [codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp),
the project it forked from. See
[Compared to codebase-memory-mcp](#compared-to-codebase-memory-mcp).

- [Quick Start](#quick-start)
- [MCP Tools](#mcp-tools)
- [Features](#features)
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
and other MCP clients: server entries, instruction files, and pre-tool hooks. Restart your
agent and say "Index this project."

Other subcommands: `config set auto_index true`, `update`, `uninstall`.

It needs no LLM and no API key. The server is the structural backend; your agent is the
language layer. All data stays in `~/.cache/code-cortex-mcp/` as SQLite.

## MCP Tools

| Tool | Purpose |
|------|---------|
| `index_repository`, `index_status`, `list_projects`, `delete_project` | Index and manage projects |
| `search_graph` | Search by label, name pattern, file pattern, or degree |
| `trace_path` | Callers, callees, data flow, and cross-service chains |
| `query_graph` | Read-only Cypher-subset queries |
| `get_code_snippet` | Source of a symbol by qualified name |
| `get_architecture` | Languages, packages, routes, hotspots, clusters, cycles, ADRs |
| `get_graph_schema` | Node and edge counts, property shapes |
| `search_code` | Graph-augmented grep over indexed files |
| `detect_changes` | Blast radius of a git diff |
| `manage_adr` | Architecture Decision Records (CRUD) |
| `ingest_traces` | Runtime traces (stub: counts spans only) |

Every tool also runs from the CLI:

```bash
code-cortex-mcp cli search_graph '{"name_pattern": ".*Handler.*", "label": "Function"}'
code-cortex-mcp cli query_graph  '{"query": "MATCH (f:Function) RETURN f.name LIMIT 5"}'
```

## Features

- **Static analysis** — import-aware, type-inferred call graph; dead code; Leiden clusters;
  circular dependencies; complexity metrics; git-diff blast radius.
- **Deterministic indexing** — the same tree always produces a byte-identical graph.
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
- **Team artifact** — commit `.code-cortex/graph.db.zst` (a zstd snapshot, about 10:1)
  and teammates import it instead of a full reindex. A `.gitattributes` `merge=ours` rule
  prevents merge conflicts. Gitignore `.code-cortex/` to opt out.

## Performance

Apple Silicon, release build.

| Operation | Time |
|-----------|------|
| RocksDB full index (C++, 62K nodes, 346K edges) | ~7 s |
| Django full index (Python, 55K nodes, 371K edges) | ~4 s |
| Redis full index (C, 38K nodes, 148K edges) | ~2.7 s |
| etcd full index (Go, 15K nodes, 94K edges) | ~1.4 s |
| Linux kernel full index (28M LOC, 2.1M nodes) | ~3 min |
| `search_graph`, `trace_path` (warm) | 0.1–0.5 ms |
| `get_code_snippet` (warm) | ~2 ms |
| PreToolUse hook (`Grep` or `Read`) | ~10 ms |

Two mechanisms keep calls fast. A persistent worker process serves tool calls, so each call
skips a process exec and a database open. A memo in `_config.db` records each database's
integrity verdict against its `(size, mtime)`, so a cold process such as a hook does not
verify databases again.

| Variable | Effect |
|----------|--------|
| `CBM_TOOL_SERVER=0` | One worker process per tool call (the Windows default) |
| `CBM_TOOL_SUPERVISOR=0` | Run tools in-process, without isolation |
| `CBM_STORE_META=0` | Disable the memo; every lookup verifies again |

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
```

- **Storage** — `~/.cache/code-cortex-mcp/`; override with `CBM_CACHE_DIR`.
- **Parallelism** — auto-detected (cgroup-aware); override with `CBM_WORKERS` (1–256).
- **Ignore rules** — `.gitignore`, then `.cbmignore` (gitignore syntax). The indexer skips symlinks.
- **Custom extensions** — `.code-cortex.json`: `{"extra_extensions": {".mjs": "javascript"}}`.

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
its on-disk graph format. It indexes faster and answers most tool calls faster.
codebase-memory-mcp has features that code-cortex-mcp does not; the feature table names them.

Test conditions for every number in this section:

- **Date and machine** — 2026-08-25, Apple Silicon, macOS.
- **Versions** — code-cortex-mcp at `7a3196c4`, codebase-memory-mcp at `010569fa`.
- **Binaries** — each project's own `scripts/build.sh`, without the codebase-memory-mcp web UI.
- **Cache** — one empty cache directory per engine.

### Indexing speed

Median of three full-index runs from an empty cache.

| Repository | Language | code-cortex-mcp | codebase-memory-mcp | Ratio |
|---|---|---|---|---|
| Redis | C | **2.46 s** | 9.48 s | 3.9× |
| etcd | Go | **1.31 s** | 7.74 s | 5.9× |
| Django | Python | **4.03 s** | 12.95 s | 3.2× |
| this repository | C++ | **2.46 s** | 9.56 s | 3.9× |

Both engines build graphs of nearly the same size, so the times measure comparable work.

| Repository | code-cortex-mcp (nodes / edges) | codebase-memory-mcp (nodes / edges) |
|---|---|---|
| Redis | 38,438 / 146,074 | 38,658 / 135,969 |
| etcd | 14,836 / 94,615 | 15,475 / 108,103 |
| Django | 55,458 / 371,363 | 55,461 / 344,047 |
| this repository | 13,418 / 47,157 | 13,419 / 47,085 |

Embeddings do not explain the gap. In `fast` mode, which writes no similarity or semantic
edges, Django takes 2.9 s in code-cortex-mcp and 12.0 s in codebase-memory-mcp. The gap is
in extraction and resolution: codebase-memory-mcp runs the `CALL_REFERENCE` and `USAGE`
precision passes, and code-cortex-mcp carries its own resolver and pipeline optimizations.

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
| MCP tools | 14 | 15 (adds `check_index_coverage`) |
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
