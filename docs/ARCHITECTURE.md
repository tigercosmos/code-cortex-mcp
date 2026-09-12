# Architecture and scale

Code Cortex separates expensive repository analysis from fast task-time retrieval. The indexer
creates a local SQLite graph. The Model Context Protocol (MCP) server then reads that graph for
agent requests.

This design targets repositories from small services to very large monorepos. The 100-million-line
result uses a synthetic C++ project, so it does not prove equal behavior for every language.

## System data flow

```text
repository
    |
    v
file discovery -> tree-sitter extraction -> registries -> call and type resolution
    |                                                       |
    +---------------- graph nodes and edges ----------------+
                                                            |
                                                            v
                                                      SQLite graph
                                                            |
                    +-------------------+-------------------+
                    |                   |                   |
                    v                   v                   v
              search_graph       targeted trace_path    query_graph
                    |                   |                   |
                    +---------------- bounded MCP replies --+
                                        |
                                        v
                                   coding agent
```

The persistent tool worker keeps the graph database open between calls. A supervisor applies a
deadline and contains a failed tool process.

## Scale redesign

| Area | Previous path | Current bounded path |
|---|---|---|
| Extraction state | Full results stayed in memory until resolution ended. | `CBM_RESULT_STORE=1` stores full results in temporary storage. |
| Registry state | Resolution shared the full extraction objects. | Compact owned summaries remain in memory. |
| Known endpoint chain | A broad neighborhood trace could spend work on unrelated callers. | `from_function` starts a forward search at the known entry point. |
| Traversal limit | Depth and response size bounded output, but not all search work. | `max_work` bounds examined `CALLS` edges from 1 to 100,000. |
| Result state | An empty path did not identify the limiting condition. | `path_found` and `traversal_truncated` report the result state. |
| Release check | MCP initialization started the background check. | `CBM_UPDATE_CHECK=0` disables the check for controlled runs. |

The result store is optional and applies to the parallel indexing path. It uses a fresh temporary
stream and does not survive process exit. A complete write becomes visible after `fflush`; the
store does not call `fsync` and does not provide crash recovery.

The per-run temporary-storage limit is 64 GiB. Make sure that the temporary volume has enough
free space before you set `CBM_RESULT_STORE=1`. A failed store write, including an `ENOSPC`
out-of-space error, cancels the indexing run. Free temporary-volume space before retrying; a
partial run is not a reusable result store.

The configured memory budget provides back-pressure. It is not a hard resident-memory limit.
The graph, active parsing, allocator behavior, and in-flight work can exceed the target.

## Bounded endpoint tracing

Use endpoint tracing when both ends of a call chain are known:

```json
{
  "function_name": "B",
  "from_function": "A",
  "direction": "inbound",
  "mode": "calls",
  "depth": 3,
  "max_work": 10000
}
```

The server resolves both endpoints by identity. It then performs a bounded forward breadth-first
search from `A` across indexed `CALLS` edges. The reply contains one shortest observed path.

Check these fields before using the path:

| Field | Meaning |
|---|---|
| `path_found` | The bounded search reached the target. |
| `traversal_truncated` | The search stopped at `max_work` before it examined all reachable work. |
| `traversal_examined_edges` | The number of `CALLS` edges that the search examined. |
| `traversal_visited_nodes` | The number of unique graph nodes that the search visited. |

An absent path does not prove that the functions are disconnected. Depth, work, confidence,
test filters, stale indexes, or partial parsing can hide a path.

## Task-performance boundary

The controlled 2026-09-10 run measured full agent tasks, not isolated graph calls. It included
MCP startup, initialization, tool discovery, retrieval, topology checks, and model execution.

Code Cortex was 57.9% faster than shell tools on 22 source-correct matched pairs. It was 37.4%
faster than pinned upstream on 14 source-correct matched pairs. Two candidate answers failed the
exact-output contract and were excluded without retries or estimated times.

The benchmark covers warm synthetic C++ indexes, exact lookups, and fixed three-hop chains at
10,000, 1 million, and 100 million lines. It does not cover edits, cold indexing, every language,
or every task shape. Upstream has no 100-million-line task time because both setup attempts failed
within sealed memory budgets.
