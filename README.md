# cpp-codebase-memory-mcp

[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://github.com/tigercosmos/cpp-codebase-memory-mcp)
[![CI](https://img.shields.io/github/actions/workflow/status/tigercosmos/cpp-codebase-memory-mcp/dry-run.yml?label=CI)](https://github.com/tigercosmos/cpp-codebase-memory-mcp/actions)
[![Languages](https://img.shields.io/badge/languages-155-orange)](https://github.com/tigercosmos/cpp-codebase-memory-mcp)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/tigercosmos/cpp-codebase-memory-mcp)

> **A C++23 port of [`codebase-memory-mcp`](https://github.com/DeusData/codebase-memory-mcp).**
> Same engine and on-disk format; the entire first-party codebase is migrated from C11 to
> C++23 and built with CMake. Maintained standalone at
> **<https://github.com/tigercosmos/cpp-codebase-memory-mcp>**.

A fast, dependency-free **code-intelligence engine for AI coding agents**. It builds a
persistent **knowledge graph** of your codebase — functions, classes, call chains, HTTP
routes, cross-service links — and answers structural queries over MCP in well under a
millisecond. It full-indexes an average repo in milliseconds and the Linux kernel (28M LOC,
75K files) in ~3 minutes. Ships as a single static binary for macOS, Linux, and Windows.

Parsing is [tree-sitter](https://tree-sitter.github.io/tree-sitter/) AST analysis across 155
languages, enhanced with LSP-style hybrid type resolution for Go, C, C++,
TypeScript/JavaScript/JSX/TSX, Java, Kotlin, Rust, Python, PHP, and C#. 14 MCP
tools, zero runtime dependencies.

## Why

- **No LLM, no API keys** — it's the structural backend; your MCP agent (Claude Code, etc.)
  is the language layer. One graph query replaces dozens of grep/read cycles (~120× fewer
  tokens on typical exploration).
- **Single static binary** — 155 tree-sitter grammars compiled in. No Docker, no runtime deps.
- **Everything local** — SQLite-backed, persists to `~/.cache/codebase-memory-mcp/`. Your code
  never leaves the machine.
- **Plug and play across agents** — `install` auto-detects and configures Claude Code, Codex
  CLI, Gemini CLI, Zed, OpenCode, Aider, VS Code, and more (MCP entries + instruction files +
  non-blocking pre-tool hooks).

## Build from Source

The build system is **CMake** (C++23). You need a C/C++ compiler (gcc or clang) and zlib.

```bash
git clone https://github.com/tigercosmos/cpp-codebase-memory-mcp.git
cd cpp-codebase-memory-mcp
scripts/build.sh                 # standard binary  → build/c/codebase-memory-mcp
scripts/build.sh --with-ui       # with 3D graph visualization UI
./build/c/codebase-memory-mcp install   # configure your installed agents
```

Or build, place the binary on your `PATH`, and install the skill in one step:

```bash
make install PREFIX=$HOME/.local # no sudo; binary → ~/.local/bin
sudo make install                # system-wide; binary → /usr/local/bin
```

`make install` builds the binary, installs it under `$(PREFIX)/bin`, then runs the binary's own
`install` to deploy the embedded skill. `/usr/local` needs root, so use `sudo` (the skill step
runs as `$SUDO_USER`, landing in *your* `~/.claude`, not root's) — or set `PREFIX=$HOME/.local` to
skip sudo. `install` auto-detects your coding agents and wires up MCP server
entries, instruction files, and hooks. Then restart your agent and say **“Index this project.”**

Run the test suite with `scripts/test.sh` (CMake + ASan/UBSan) and the linters with
`scripts/lint.sh`.

## Install Script

Once a release is published, the binary can also be installed directly:

```bash
# macOS / Linux
curl -fsSL https://raw.githubusercontent.com/tigercosmos/cpp-codebase-memory-mcp/main/install.sh | bash
# add --ui for the graph-visualization variant
```

```powershell
# Windows (PowerShell)
Invoke-WebRequest -Uri https://raw.githubusercontent.com/tigercosmos/cpp-codebase-memory-mcp/main/install.ps1 -OutFile install.ps1
.\install.ps1
```

Useful subcommands: `config set auto_index true` (index on session start),
`--ui=true --port=9749` (graph UI at `http://localhost:9749`), `update`, `uninstall`.

## How It Works

```
You:    "what calls ProcessOrder?"
Agent:  trace_path(function_name="ProcessOrder", mode="calls")
Engine: runs the graph traversal, returns structured results
Agent:  explains the call chain in plain English
```

The engine indexes in a RAM-first pipeline (LZ4-compressed reads, in-memory SQLite, single
dump at the end; memory is released afterward), then serves queries from a persistent SQLite
graph store.

## MCP Tools

| Tool | Purpose |
|------|---------|
| `index_repository` / `index_status` / `list_projects` / `delete_project` | Index and manage projects (auto-sync keeps them fresh) |
| `search_graph` | Structured search by label, name/file pattern, degree filters |
| `trace_path` | Call-chain traversal (callers / callees / data flow / cross-service) |
| `query_graph` | Read-only Cypher-subset queries |
| `get_code_snippet` | Source for a symbol by qualified name |
| `get_architecture` | Languages, packages, routes, hotspots, clusters, ADRs in one call |
| `get_graph_schema` | Node/edge counts and property shapes (run first) |
| `search_code` | Graph-augmented grep over indexed files |
| `detect_changes` | Map a git diff to affected symbols + blast radius |
| `manage_adr` | Architecture Decision Records (CRUD) |
| `ingest_traces` | Ingest runtime traces to validate `HTTP_CALLS` edges |

Every tool is also available from the CLI:

```bash
codebase-memory-mcp cli search_graph '{"name_pattern": ".*Handler.*", "label": "Function"}'
codebase-memory-mcp cli query_graph  '{"query": "MATCH (f:Function) RETURN f.name LIMIT 5"}'
```

## Features

- **Graph & analysis** — import-aware, type-inferred call graph; dead-code detection; Leiden
  community clusters; complexity / bottleneck metrics; git-diff impact mapping; Cypher-like
  queries.
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
- **Search** — semantic vector search (bundled `nomic-embed-code` embeddings, no API key),
  BM25 full-text (FTS5, camelCase/snake_case aware), and structural/code search.
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

## Team-Shared Graph Artifact

Commit `.codebase-memory/graph.db.zst` (a zstd-compressed graph snapshot, typically 8–13:1)
and teammates skip the full reindex: on first run the artifact is imported and incremental
indexing fills in their local diff. A `.gitattributes` `merge=ours` rule is auto-created so the
binary artifact never causes merge conflicts. Optional — gitignore `.codebase-memory/` to opt out.

## Performance

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

## Language Support

155 languages via vendored tree-sitter grammars. Strongest call/type resolution (LSP-style
hybrid) for **Go, C, C++, TypeScript/JavaScript/JSX/TSX, Java, Kotlin, Rust, Python, PHP,
C#**. Benchmarked tiers:

- **Excellent (≥90%)** — C, C++, Lua, Kotlin, Perl, Objective-C, Groovy, Bash, Zig, Swift,
  CSS, YAML, TOML, HTML, SCSS, HCL, Dockerfile
- **Good (75–89%)** — Python, TypeScript, TSX, Go, Rust, Java, R, Dart, JavaScript, Erlang,
  Elixir, Scala, Ruby, PHP, C#, SQL

Plus ~110 more (config, data, and niche languages) parsed structurally.

## Graph Data Model

- **Nodes** — `Project`, `Package`, `Folder`, `File`, `Module`, `Class`, `Function`, `Method`,
  `Interface`, `Enum`, `Type`, `Route`, `Resource`
- **Qualified names** — `get_code_snippet` uses `<project>.<path_parts>.<name>`; discover them
  with `search_graph` first.
- **Cypher subset** — `MATCH` / `OPTIONAL MATCH` (labels, relationship types, variable-length
  paths), `WHERE` (comparisons / regex / `CONTAINS` / `EXISTS{}`), `WITH` (+ `DISTINCT`),
  `RETURN` (+ `COUNT`/`COUNT(DISTINCT)` / aggregates), `ORDER BY`, `LIMIT`. Read-only; no
  mutations. Queries are bounded by a 100k-row ceiling and a 30 s wall-clock deadline, so an
  unbounded `OPTIONAL MATCH` fails with an actionable error instead of hanging.

## Configuration

```bash
codebase-memory-mcp config list
codebase-memory-mcp config set auto_index true        # index on MCP session start
codebase-memory-mcp config set auto_index_limit 50000 # max files for auto-index
```

- **Index storage** — `~/.cache/codebase-memory-mcp/` (override with `CBM_CACHE_DIR`). WAL-mode
  SQLite, ACID-safe across restarts. Reset with `rm -rf ~/.cache/codebase-memory-mcp/`.
- **Indexing parallelism** — auto-detected (cgroup-aware on Linux); override with `CBM_WORKERS`
  (range 1–256, invalid values ignored). Useful in containers where the host CPU count differs
  from the effective quota.
- **Ignore rules** — hardcoded patterns → `.gitignore` hierarchy → `.cbmignore` (gitignore
  syntax). Symlinks always skipped.
- **Custom extensions** — map extra extensions to languages via `.codebase-memory.json`
  (`{"extra_extensions": {".mjs": "javascript"}}`).

## Credits & License

This is a community C++23 port. The original engine, design, and research are by
**[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)** — see the
preprint *Codebase-Memory: Tree-Sitter-Based Knowledge Graphs for LLM Code Exploration via MCP*
([arXiv:2603.27277](https://arxiv.org/abs/2603.27277)).

MIT — see [LICENSE](LICENSE). Security policy: [SECURITY.md](SECURITY.md).
