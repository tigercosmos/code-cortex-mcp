/* tool_server — one persistent, supervised worker process for MCP tool calls.
 *
 * Why: `83973674` moved every tool call into a worker subprocess so a crash
 * or hang cannot take the stdio server down and every call has a hard
 * deadline. It spawned a NEW process per call, which threw away the open
 * store, its prepared statements and the integrity memo each time: a 5ms
 * search_graph cost 145ms on the wire, 460ms on a 100MB database.
 *
 * This keeps the isolation and the deadline but reuses one worker:
 *
 *   parent (stdio MCP server)              worker (`<self> cli --tool-server`)
 *   cbm_tool_server_call(tool, args)  ──►  reads frame, dispatches through
 *        waits ≤ cbm_tool_timeout_ms       cbm_mcp_handle_tool on ONE srv,
 *   ◄──  reply frame                        writes reply, loops.
 *
 * Framing (both directions, over the worker's stdin/stdout pipes):
 *   request:  "<tool_len> <args_len>\n" + tool bytes + args bytes
 *   reply:    "<len>\n" + len bytes (the tool result JSON, unchanged)
 *
 * Supervision: a reply that misses its deadline gets the worker SIGKILLed and
 * the call reports CBM_PROC_HANG; a worker that dies mid-call reports the
 * crash outcome. Either way the NEXT call spawns a fresh worker, so one bad
 * query costs one process, never the server. A worker that exits cleanly
 * between calls (it recycles itself after CBM_TOOL_SERVER_MAX_REQUESTS
 * requests so RSS cannot ratchet) is respawned transparently and the call is
 * retried once.
 *
 * The worker revalidates its cached store before every dispatch: a re-index
 * deletes and recreates the .db, delete_project unlinks it, so a store whose
 * file changed generation (inode/size/mtime) is closed and reopened by the
 * normal resolve path. Nothing is ever served from a replaced file.
 *
 * Policy: only consulted when cbm_tool_supervisor_should_wrap() is true.
 * CBM_TOOL_SERVER=0 falls back to the one-shot worker per call. Windows has no
 * piped spawn yet and always uses the one-shot worker. */
#ifndef CBM_TOOL_SERVER_H
#define CBM_TOOL_SERVER_H

#include <stdbool.h>
#include <stdio.h>

#include "mcp/index_supervisor.h" /* cbm_index_worker_result_t */

#ifdef __cplusplus
extern "C" {
#endif

/* True when tool calls should go through the persistent worker (POSIX and
 * CBM_TOOL_SERVER is not "0"). Callers still gate on
 * cbm_tool_supervisor_should_wrap() first. */
bool cbm_tool_server_enabled(void);

/* Run `tool_name` with `args_json` in the persistent worker, spawning it if
 * needed. Fills *result exactly like cbm_tool_spawn_worker (outcome, exit
 * code, signal, heap `response` or NULL). Returns 0 when a worker ran the
 * request to some outcome, -1 when no worker could be spawned. */
int cbm_tool_server_call(const char *tool_name, const char *args_json,
                         cbm_index_worker_result_t *result);

/* Stop the worker (EOF, short grace, then SIGKILL). Idempotent. Called by the
 * server on shutdown; a parent that dies without calling it is covered by the
 * worker's parent-death watchdog and by EOF on its stdin. */
void cbm_tool_server_shutdown(void);

/* Worker side: serve frames from `in` until EOF (or the recycle limit), replying
 * on `out`. Returns the process exit code. Used by `cli --tool-server`. */
int cbm_tool_server_serve(FILE *in, FILE *out);

/* Number of workers spawned by this process so far (test hook). */
int cbm_tool_server_spawn_count(void);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Replace the worker command line (NULL-terminated argv; argv[0] is the
 * executable). NULL restores `<self> cli --tool-server`. Lets tests stand up a
 * fake worker (e.g. a shell script) without a production binary. */
void cbm_tool_server_set_argv_for_testing(const char *const *argv);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_TOOL_SERVER_H */
