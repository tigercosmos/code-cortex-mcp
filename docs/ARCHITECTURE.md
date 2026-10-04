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

The 2026-10-03 run measured full Claude Code sessions, not isolated graph calls: a fresh
session per task, the task as the only prompt, the agent free to use any tool, and the timer
around the whole process (startup, hooks, MCP connection, every model turn).

On real repositories the installed product measured 0.65 (Opus 5.5) and 0.66 (Fable 5.1) of
the shell-only time over 32 tasks in seven languages, and 0.73 and 0.60 over 34 C/C++ tasks, with
the agent answering from the prompt block without a tool in most sessions. On the
synthetic C++ task set, where every symbol resolves exactly, Opus 5.5 finished in 0.557 of the shell-only time
(48 pairs) and Fable 5.1 in 0.622 (38 exact-match pairs). The agent called no MCP tool; the time
came from the UserPromptSubmit hook that puts verified definition locations and call chains into
the context before the first turn, and from a SessionStart brief that is stored at index time so
it costs the same on a 2.2M-node index as on a 200-node one.

The benchmark covers warm synthetic C++ indexes, exact lookups, and fixed three-hop chains at
10,000, 1 million, and 100 million lines. It does not cover edits, cold indexing, every language,
or every task shape.
