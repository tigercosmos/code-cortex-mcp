/*
 * mcp.c — MCP server: JSON-RPC 2.0 over stdio with 14 graph tools.
 *
 * Uses yyjson for fast JSON parsing/building.
 * Single-threaded event loop: read line → parse → dispatch → respond.
 */

// operations

#include "foundation/constants.h"

enum {
    MCP_FIELD_SIZE = 1040,
    MCP_TIMEOUT_MS = 1000,
    MCP_HALF_SEC_US = 500000,
    MCP_MAX_ROWS = 100,
    MCP_COL_2 = 2,
    MCP_COL_3 = 3,
    MCP_COL_4 = 4,
    MCP_COL_7 = 7,
    MCP_COL_10 = 10,
    MCP_COL_16 = 16,
    MCP_DB_EXT = 3,    /* strlen(".db") */
    MCP_SEPARATOR = 2, /* space for separator chars */
    MCP_DEFAULT_DEPTH = 3,
    MCP_DEFAULT_BFS_DEPTH = 2,
    MCP_DEFAULT_LIMIT = 10,
    MCP_BFS_LIMIT = 100,           /* default per-direction trace budget (limit param raises) */
    MCP_BFS_SCAN_LIMIT = 1000,     /* max ordered candidate rows before test filtering */
    MCP_BFS_LIMIT_MAX = 5000,      /* hard ceiling for the limit param (context-bomb guard) */
    MCP_PATH_WORK_DEFAULT = 10000, /* CALLS edges examined by targeted from_function search */
    MCP_PATH_WORK_MAX = 100000,    /* hard ceiling independent of repository size */
    MCP_PATH_HASH_FILE_MAX = 2 * 1024 * 1024,
    MCP_PATH_HASH_REQUEST_MAX = 32 * 1024 * 1024,
    MCP_PATH_HASH_TIMEOUT_MS = 200,
    MCP_DEFAULT_IMPACT_LIMIT = 200, /* detect_changes per-symbol rows; rollup stays complete */
    MCP_SNIPPET_MAX_LINES = 500,    /* get_code_snippet line cap (whole-file Module guard) */
    /* search_code `context`: lines of surrounding source per hit. Clamped at
     * the handler and again where the window is computed, so the arithmetic
     * never overflows. */
    MCP_SEARCH_CONTEXT_MAX_LINES = 200,
    MCP_N_DEFAULTS_2 = 2,
    MCP_URI_PREFIX = 7,      /* strlen("file://") */
    MCP_CONTENT_PREFIX = 15, /* strlen("Content-Length:") */
    MCP_RETURN_2 = 2,
    /* tools/list page size. The MCP cursor is optional for clients and at
     * least one (Codex CLI 0.153) never follows nextCursor: with the old page
     * of 8 it saw only the first eight tools, so list_projects, index_status
     * and detect_changes were unreachable from Codex. One page holds every
     * tool; the cursor code stays for clients that do send one. */
    MCP_TOOLS_PAGE_SIZE = 64,
    MCP_FALLBACK_SCAN_TIMEOUT_MS = 5000,
    MCP_FALLBACK_DB_BUSY_MS = 500,
    MCP_INDEX_LOCK_WAIT_MS = 5000,
    MCP_INDEX_LOCK_POLL_MS = 50,
};
#define MCP_MS_TO_US 1000LL
#define MCP_S_TO_US 1000000LL

#define SLEN(s) (sizeof(s) - 1)
#include "mcp/mcp.h"
#include "store/store.h"
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <new>
#include <vector>
#include "cypher/cypher.h"
#include "pipeline/pipeline.h"
#include "mcp/mcp_internal.h"
#include "pipeline/pass_cross_repo.h"
#include "git/git_context.h"
#include "cli/cli.h"
#include "watcher/watcher.h"
#include "discover/discover.h" /* cbm_discover_count_bounded: auto_index admission (#713) */
#include "helpers.h"           /* cbm_kind_in_set_free_cache: auto-index thread teardown */
#include "foundation/mem.h"
#include "foundation/diagnostics.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/log.h"
#include "foundation/sha256.h"
#include "foundation/limits.h"
#include "mcp/index_supervisor.h"
#include "mcp/store_meta.h"
#include "mcp/tool_server.h"
#include "foundation/str_util.h"
#include "foundation/dump_verify.h"
#include "foundation/compat_regex.h"
#include "pipeline/artifact.h"
#include "pipeline/branch_slots.h"
#include "mcp/cache_expiry.h"
#include "pipeline/pipeline_internal.h" /* cbm_pipeline_repo_path */

static size_t trace_hash_file_max = MCP_PATH_HASH_FILE_MAX;
static size_t trace_hash_request_max = MCP_PATH_HASH_REQUEST_MAX;

#ifdef CBM_ENABLE_TEST_SEAMS
void cbm_mcp_test_set_trace_hash_limits(size_t per_file_bytes, size_t per_request_bytes) {
    trace_hash_file_max = per_file_bytes ? per_file_bytes : (size_t)MCP_PATH_HASH_FILE_MAX;
    trace_hash_request_max =
        per_request_bytes ? per_request_bytes : (size_t)MCP_PATH_HASH_REQUEST_MAX;
}
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <langinfo.h>
#include <locale.h>
#include <regex.h> /* REG_NOTBOL for cbm_regexec's pass-through exec flags */
#ifdef __APPLE__
#include <xlocale.h>
#endif
#endif
#include <yyjson/yyjson.h>
#include <limits.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

/* ── Constants ────────────────────────────────────────────────── */

/* Default snippet fallback line count */
#define SNIPPET_DEFAULT_LINES 50

/* Idle store eviction: close cached project store after this many seconds
 * of inactivity to free SQLite memory during idle periods. */
#define STORE_IDLE_TIMEOUT_S 60

/* Directory permissions: rwxr-xr-x */
#define ADR_DIR_PERMS 0755

/* JSON-RPC 2.0 standard error codes */
#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_METHOD_NOT_FOUND (-32601)

/* ── Helpers ────────────────────────────────────────────────────── */

static char *heap_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = (char *)malloc(len + SKIP_ONE);
    if (d) {
        memcpy(d, s, len + SKIP_ONE);
    }
    return d;
}

/* Write yyjson_mut_doc to heap-allocated JSON string.
 * ALLOW_INVALID_UNICODE: some database strings may contain non-UTF-8 bytes
 * from older indexing runs — don't fail serialization over it. */
static char *yy_doc_to_str(yyjson_mut_doc *doc) {
    size_t len = 0;
    char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    return s;
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

int cbm_jsonrpc_parse(const char *line, cbm_jsonrpc_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = CBM_NOT_FOUND;

    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (!doc) {
        return CBM_NOT_FOUND;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return CBM_NOT_FOUND;
    }

    yyjson_val *v_jsonrpc = yyjson_obj_get(root, "jsonrpc");
    yyjson_val *v_method = yyjson_obj_get(root, "method");
    yyjson_val *v_id = yyjson_obj_get(root, "id");
    yyjson_val *v_params = yyjson_obj_get(root, "params");

    if (!v_method || !yyjson_is_str(v_method)) {
        yyjson_doc_free(doc);
        return CBM_NOT_FOUND;
    }

    out->jsonrpc =
        heap_strdup(v_jsonrpc && yyjson_is_str(v_jsonrpc) ? yyjson_get_str(v_jsonrpc) : "2.0");
    out->method = heap_strdup(yyjson_get_str(v_method));

    if (v_id) {
        out->has_id = true;
        if (yyjson_is_int(v_id)) {
            out->id = yyjson_get_int(v_id);
        } else if (yyjson_is_str(v_id)) {
            /* JSON-RPC 2.0 §4 permits string ids (Claude Desktop uses them).
             * Preserve verbatim instead of coercing via strtol (issue #253). */
            out->id_str = heap_strdup(yyjson_get_str(v_id));
        }
    }

    if (v_params) {
        out->params_raw = yyjson_val_write(v_params, 0, NULL);
    }

    yyjson_doc_free(doc);
    return 0;
}

void cbm_jsonrpc_request_free(cbm_jsonrpc_request_t *r) {
    if (!r) {
        return;
    }
    safe_str_free(&r->jsonrpc);
    safe_str_free(&r->method);
    safe_str_free(&r->id_str);
    safe_str_free(&r->params_raw);
    memset(r, 0, sizeof(*r));
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_jsonrpc_format_response(const cbm_jsonrpc_response_t *resp) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    if (resp->id_str) {
        yyjson_mut_obj_add_str(doc, root, "id", resp->id_str);
    } else {
        yyjson_mut_obj_add_int(doc, root, "id", resp->id);
    }

    if (resp->error_json) {
        /* Parse the error JSON and embed */
        yyjson_doc *err_doc = yyjson_read(resp->error_json, strlen(resp->error_json), 0);
        if (err_doc) {
            yyjson_mut_val *err_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(err_doc));
            yyjson_mut_obj_add_val(doc, root, "error", err_val);
            yyjson_doc_free(err_doc);
        }
    } else if (resp->result_json) {
        /* Parse the result JSON and embed */
        yyjson_doc *res_doc = yyjson_read(resp->result_json, strlen(resp->result_json), 0);
        if (res_doc) {
            yyjson_mut_val *res_val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(res_doc));
            yyjson_mut_obj_add_val(doc, root, "result", res_val);
            yyjson_doc_free(res_doc);
        }
    } else {
        /* JSON-RPC 2.0 spec: response MUST contain "result" or "error" */
        yyjson_mut_obj_add_null(doc, root, "result");
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

char *cbm_jsonrpc_format_error(int64_t id, int code, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", id);

    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_str(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, root, "error", err);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

/* One-shot note attached to the next object-shaped tool result. Set by
 * get_project_arg_or_cwd when it answers for a project other than the one the
 * caller named; cleared here and at the top of cbm_mcp_handle_tool. */
static char g_project_note[CBM_SZ_512];

char *cbm_mcp_text_result(const char *text, bool is_error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    yyjson_mut_obj_add_str(doc, item, "text", text ? text : "");
    yyjson_mut_arr_add_val(content, item);
    yyjson_mut_obj_add_val(doc, root, "content", content);

    /* structuredContent has been wrong in both directions upstream, so the rule
     * is spelled out here in full:
     *
     *   - JSON-object payload -> structuredContent = the PARSED object (the
     *     spec's structured+serialized pattern).
     *   - error               -> structuredContent = {"error": <text>} —
     *     bounded, small, and the only machine-readable form of a failure a
     *     client has.
     *   - anything else       -> NO structuredContent key at all.
     *
     * Duplicating a non-object payload into {"text": <payload>} beside an
     * identical content[0].text measured 2.05x the bytes on a 20k-node
     * query_graph, and an EMPTY object is worse still: clients that honor a
     * declared outputSchema treat structuredContent as THE authoritative
     * result, so an empty object beside a non-empty payload renders the reply
     * as literally "{}". Which is why no tool declares an outputSchema anymore
     * — see mcp_add_tool_def. */
    if (!is_error && text) {
        yyjson_doc *structured_doc = yyjson_read(text, strlen(text), 0);
        if (structured_doc) {
            yyjson_val *structured_root = yyjson_doc_get_root(structured_doc);
            if (yyjson_is_obj(structured_root)) {
                yyjson_mut_val *structured = yyjson_val_mut_copy(doc, structured_root);
                /* A pending project substitution note (see
                 * get_project_arg_or_cwd) rides on the object payload, in
                 * both the text and the structured copy, so the agent learns
                 * which project actually answered. */
                if (g_project_note[0]) {
                    yyjson_mut_obj_add_strcpy(doc, structured, "project_note", g_project_note);
                    char *retext = yyjson_mut_val_write(structured, 0, NULL);
                    if (retext) {
                        yyjson_mut_obj_replace(item, yyjson_mut_str(doc, "text"),
                                               yyjson_mut_strcpy(doc, retext));
                        free(retext);
                    }
                    g_project_note[0] = '\0';
                }
                yyjson_mut_obj_add_val(doc, root, "structuredContent", structured);
            }
            yyjson_doc_free(structured_doc);
        }
    } else if (is_error) {
        yyjson_mut_val *structured = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, structured, "error", text ? text : "");
        yyjson_mut_obj_add_val(doc, root, "structuredContent", structured);
    }
    /* One reply consumes the note whatever its shape; it must never leak
     * into the next tool call. */
    g_project_note[0] = '\0';
    yyjson_mut_obj_add_bool(doc, root, "isError", is_error);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* Wrap a heap-owned payload in the MCP text result envelope and release it.
 * Keeping this ownership step in one place prevents handler tails from
 * accidentally leaking serialized JSON or freeing it before it is copied. */
static char *mcp_text_result_take_owned(char *text, bool is_error) {
    char *result = cbm_mcp_text_result(text, is_error);
    free(text);
    return result;
}

bool cbm_mcp_cancel_request_matches(const char *params_json, int64_t active_id,
                                    const char *active_id_str) {
    if (!params_json) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *request_id = yyjson_obj_get(root, "requestId");
    bool matches = false;
    if (request_id) {
        if (active_id_str) {
            matches =
                yyjson_is_str(request_id) && strcmp(yyjson_get_str(request_id), active_id_str) == 0;
        } else {
            matches = yyjson_is_int(request_id) && yyjson_get_int(request_id) == active_id;
        }
    }

    yyjson_doc_free(doc);
    return matches;
}

/* ── Tool definitions ─────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *input_schema; /* JSON string */
} tool_def_t;

static const tool_def_t TOOLS[] = {
    {"index_repository", "Index repository",
     "Index a repository into the knowledge graph. "
     "Special mode 'cross-repo-intelligence': skip extraction, only match Routes/Channels "
     "across projects to create CROSS_HTTP_CALLS/CROSS_ASYNC_CALLS/CROSS_CHANNEL edges. "
     "Requires target_projects param. Ensure target projects have fresh indexes first. "
     "COVERAGE: the response reports files that were NOT fully indexed — 'skipped' (not "
     "indexed at all: oversized/read/parse failures) and 'parse_partial' (indexed, but "
     "constructs inside the listed line ranges could not be parsed and MAY be missing from "
     "the graph). Query the persisted signal any time via index_status or "
     "structurally via query_graph(graph=\"missed\"). Both signals are best-effort: absence "
     "of a flag is NOT a completeness guarantee; prefer grep inside flagged ranges. "
     "Separately, 'excluded' + 'not_indexed_files' list what was deliberately NOT indexed "
     "(gitignore/.cbmignore/skip-lists) — by design, not failures.",
     "{\"type\":\"object\",\"properties\":{\"repo_path\":{\"type\":\"string\",\"description\":"
     "\"Path to the repository\"},"
     "\"mode\":{\"type\":\"string\","
     "\"enum\":[\"full\",\"moderate\",\"fast\",\"cross-repo-intelligence\"],"
     "\"default\":\"full\",\"description\":\"All modes run type-aware LSP call/usage "
     "resolution (per-file + cross-file). full: all files + similarity/semantic edges. "
     "moderate: filtered files + similarity/semantic. fast: filtered files, no "
     "similarity/semantic. cross-repo-intelligence: match Routes/Channels across projects.\"},"
     "\"target_projects\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
     "\"description\":\"Projects to search for cross-repo links (cross-repo-intelligence mode). "
     "Use [\\\"*\\\"] for all indexed projects. Run list_projects to see available projects.\"},"
     "\"name\":{\"type\":\"string\",\"description\":"
     "\"Override the derived project name. Non-ASCII bytes are encoded and unsafe path characters "
     "are normalized.\"},"
     "\"persistence\":{\"type\":\"boolean\",\"default\":false,\"description\":"
     "\"Write compressed artifact to .code-cortex/graph.db.zst for team sharing. "
     "Teammates can bootstrap from the artifact instead of full re-indexing.\"}"
     "},\"required\":[\"repo_path\"]}"},

    {"search_graph", "Search graph",
     "Search the code knowledge graph for functions, classes, routes, and variables. Use INSTEAD "
     "OF grep/glob when finding code definitions, implementations, or relationships. Three search "
     "modes: (1) query='update settings' for BM25 ranked full-text search with camelCase "
     "splitting and structural label boosting — recommended for natural-language discovery; "
     "(2) name_pattern='.*regex.*' for exact pattern matching; (3) semantic_query=[...] for "
     "vector cosine search that bridges vocabulary (finds 'publish' when you search 'send'). "
     "The three modes are independent and can be combined in a single call. "
     "PAGINATION: results are capped at limit (default 200) — broader queries are silently "
     "truncated. The response always includes 'total' (full match count before limit) and "
     "'has_more' (true when total > offset+returned). Detect truncation with has_more, then "
     "page by re-calling with offset=offset+limit until has_more is false. Narrow first via "
     "label/file_pattern/min_degree before paginating large result sets.",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},"
     "\"query\":{\"type\":\"string\",\"description\":\"Natural-language or keyword full-text "
     "search using BM25 ranking. Tokens are split on whitespace; camelCase identifiers are "
     "indexed as individual words (updateCloudClient → update, cloud, client). Results are "
     "ranked with structural boosting: Functions/Methods +10, Routes +8, Classes/Interfaces +5. "
     "Noise labels (File/Folder/Module/Variable) are filtered out. When provided, name_pattern "
     "is ignored.\"},"
     "\"label\":{\"type\":\"string\"},\"name_pattern\":{\"type\":\"string\"},\"qn_pattern\":{"
     "\"type\":\"string\"},\"file_pattern\":{\"type\":\"string\"},"
     "\"relationship\":{\"type\":\"string\"},\"min_degree\":{\"type\":\"integer\"},"
     "\"max_degree\":{\"type\":\"integer\"},\"exclude_entry_points\":{\"type\":\"boolean\"},"
     "\"include_connected\":{\"type\":\"boolean\"},\"include_metrics\":{\"type\":\"boolean\","
     "\"default\":false,\"description\":\"Also return per-function complexity metrics "
     "(complexity, cognitive, loop_*, param_count, ...). Off by default to keep replies "
     "small.\"},\"semantic_query\":{"
     "\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"MUST be an ARRAY of "
     "keyword strings (e.g. [\\\"send\\\",\\\"pubsub\\\",\\\"publish\\\"]) — NOT a single string. "
     "Each keyword is scored independently via per-keyword min-cosine; results reflect functions "
     "that score well on ALL keywords. Requires moderate/full index mode. Results appear in the "
     "'semantic_results' field (separate from 'results').\"},\"limit\":{\"type\":"
     "\"integer\",\"description\":\"Max results per call. Default 200. Response carries "
     "'total' (full match count) and 'has_more' (true if truncated) so callers can "
     "detect the limit and paginate.\"},\"offset\":{\"type\":\"integer\",\"default\":0,"
     "\"description\":\"Skip the first N matching nodes. Combine with 'limit' to page: "
     "increment offset by limit and re-call while has_more is true.\"}},"
     "\"required\":[]}"},

    {"query_graph", "Query graph",
     "Execute a Cypher query against the knowledge graph for complex multi-hop patterns, "
     "aggregations, and cross-service analysis. The response includes 'total' (returned "
     "row count). There is a hard 100k row ceiling — for broad queries add LIMIT in the "
     "Cypher itself or use search_graph + offset/limit pagination instead. "
     "COMPLEXITY / BOTTLENECKS: every Function and Method node carries queryable complexity "
     "properties — cyclomatic (complexity), cognitive, loop_count, loop_depth (max nested-loop "
     "depth, a polynomial-degree proxy), plus interprocedural transitive_loop_depth (worst-case "
     "nested-loop degree propagated along CALLS edges) and a recursive flag. Additional "
     "hot-path signals: linear_scan_in_loop (count of find/contains/indexOf-style scans inside a "
     "loop — the hidden O(n^2) that loop_depth misses), alloc_in_loop (allocations/appends inside "
     "a loop), recursion_in_loop (a self-call inside a loop), unguarded_recursion (recursion with "
     "no conditionally-guarded base case), param_count and max_access_depth (structure smells). "
     "Find all hot-path candidates in one query, e.g. MATCH (f:Function) WHERE "
     "f.transitive_loop_depth >= 3 OR f.linear_scan_in_loop >= 1 RETURN f.qualified_name, "
     "f.transitive_loop_depth, f.linear_scan_in_loop ORDER BY f.transitive_loop_depth DESC. "
     "MISSED GRAPH: pass graph=\"missed\" to query the best-effort miss graph instead — the "
     "file structure of ONLY the files the indexer could NOT fully index (Project → Folder → "
     "File nodes with CONTAINS_FOLDER/CONTAINS_FILE edges; each File carries kind "
     "(\"parse_partial\" = indexed but constructs in the flagged line ranges MAY be missing; "
     "or a skip phase) and detail (the line ranges / reason)). Example: MATCH (f:File) WHERE "
     "f.kind = \\\"parse_partial\\\" RETURN f.file_path, f.detail. Absence from this graph is "
     "NOT a completeness guarantee.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Cypher "
     "query\"},\"project\":{\"type\":\"string\"},"
     "\"graph\":{\"type\":\"string\",\"enum\":[\"code\",\"missed\"],\"default\":\"code\","
     "\"description\":\"Which graph to query: the code knowledge graph (default) or the "
     "missed graph (only files not fully indexed, laid out as their file structure).\"},"
     "\"max_rows\":{\"type\":\"integer\","
     "\"description\":"
     "\"Optional row limit. Default: unlimited up to a 100k row "
     "ceiling. No offset support — use search_graph for paginated browsing.\"}},"
     "\"required\":[\"query\"]}"},

    {"trace_path", "Trace path",
     "Trace paths through the code graph. Modes: calls (callers/callees), data_flow (value "
     "propagation with args at each hop), cross_service (through HTTP/async Route nodes). "
     "Use INSTEAD OF grep for callers, dependencies, impact analysis, or data flow tracing.",
     "{\"type\":\"object\",\"properties\":{\"function_name\":{\"type\":\"string\"},\"project\":{"
     "\"type\":\"string\"},\"direction\":{\"type\":\"string\",\"enum\":[\"inbound\",\"outbound\","
     "\"both\"],\"default\":\"both\"},\"depth\":{\"type\":\"integer\",\"default\":3},\"mode\":{"
     "\"type\":\"string\",\"enum\":[\"calls\",\"data_flow\",\"cross_service\"],\"default\":"
     "\"calls\",\"description\":\"calls: follow CALLS edges. data_flow: follow CALLS+DATA_FLOWS "
     "with arg expressions. cross_service: follow HTTP_CALLS+ASYNC_CALLS+DATA_FLOWS through "
     "Routes, plus CROSS_* cross-repo edges (CROSS_HTTP_CALLS/ASYNC_CALLS/CHANNEL/GRPC_CALLS/"
     "GRAPHQL_CALLS/TRPC_CALLS) to hop into other services.\"},\"parameter_name\":{\"type\":"
     "\"string\",\"description\":\"For data_flow mode: "
     "scope trace to a specific parameter name\"},\"edge_types\":{\"type\":\"array\",\"items\":{"
     "\"type\":\"string\"}},\"risk_labels\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Add risk classification (CRITICAL/HIGH/MEDIUM/LOW) based on hop distance"
     "\"},\"include_tests\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Include test files in results. When false (default), test files are "
     "filtered out. When true, test nodes are included with is_test=true marker."
     "\"},\"min_confidence\":{\"type\":\"number\",\"default\":0,"
     "\"description\":\"Filter CALLS-edge results by min resolution confidence "
     "(0..1). Default 0 returns all edges. Try 0.8 to keep precise-tier and same-module "
     "resolutions while dropping fuzzy-tail noise. Response also includes caller_edges/"
     "callee_edges arrays with "
     "{from,to,type,confidence}.\"},\"source_lines\":{\"type\":\"integer\",\"default\":0,"
     "\"description\":\"Include current source for each returned symbol, up to this many lines "
     "(maximum 500). Indexed locations can be stale; verify the included source.\"},"
     "\"source_context\":{\"type\":\"integer\",\"default\":0,\"description\":\"Include current "
     "source around each recorded call-site line with this many context lines (maximum 10). "
     "Use 2 for compact call-chain evidence; missing locations are explicit.\"},"
     "\"from_function\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512,"
     "\"description\":\"For inbound calls mode, return one shortest observed path from this "
     "entry point to function_name. Uses a bounded forward search from the entry point, resolves "
     "endpoints by identity, and respects depth, confidence and test filters. This is not an "
     "exhaustive path search.\"},"
     "\"freshness_mode\":{\"type\":\"string\",\"enum\":[\"strict\",\"index_only\"],"
     "\"default\":\"strict\",\"description\":\"For from_function paths, strict withholds an "
     "indexed chain unless every selected path file matches its indexed SHA-256 and metadata. "
     "index_only explicitly returns historical indexed relationships without a current-source "
     "or current-shortest-path claim.\"},"
     "\"max_work\":{\"type\":\"integer\",\"default\":10000,\"minimum\":1,\"maximum\":100000,"
     "\"description\":\"Maximum CALLS edges examined by a from_function path search. The reply "
     "reports the actual work and whether this bound truncated the search.\"},"
     "\"max_bytes\":{\"type\":\"integer\",\"default\":24000,\"minimum\":1500,"
     "\"description\":\"Text payload budget. Source may be omitted; an oversized graph returns "
     "an explicit error asking for a narrower trace.\"}},\"required\":[\"function_name\"]}"},

    {"get_code_snippet", "Get code snippet",
     "Read source code for a function/class/symbol. IMPORTANT: First call search_graph to find the "
     "exact qualified_name, then pass it here. This is a read tool, not a search tool. Accepts "
     "full qualified_name (exact match) or short function name (returns suggestions if ambiguous). "
     "If the response carries a 'coverage_note', the file was only partially indexed — constructs "
     "in the noted line ranges may be missing from the graph (best-effort signal); prefer grep "
     "there and treat the returned source as ground truth.",
     "{\"type\":\"object\",\"properties\":{\"qualified_name\":{\"type\":\"string\",\"description\":"
     "\"Full qualified_name from search_graph, or short function name\"},\"project\":{"
     "\"type\":\"string\"},\"include_neighbors\":{"
     "\"type\":\"boolean\",\"default\":false}},\"required\":[\"qualified_name\"]}"},

    {"inspect_symbol", "Inspect symbol",
     "One call for everything about a function, method, class or variable: its definition "
     "(file, lines, signature, source head), DIRECT CALLERS with call-site lines and resolver "
     "confidence (heuristic name matches are flagged), callees, the tests that call it, "
     "cross-language callers (e.g. Python callers of a C++ binding), subclasses for classes, "
     "and trust signals (index freshness, partial-parse coverage). Use this FIRST for 'who calls "
     "X', 'what does X depend on', 'where is X used across languages', 'which tests cover X', "
     "and impact-of-change questions — it replaces a search + read + trace sequence. Accepts a "
     "bare name, Class::method, or a qualified name; returns suggestions when ambiguous. "
     "Bounded by max_bytes; continuation covers callers, caller files and declarations.",
     "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":"
     "\"Name, Class::method, or qualified_name\"},\"project\":{\"type\":\"string\",\"description\":"
     "\"Optional: defaults to the indexed project containing the working directory\"},"
     "\"callers_limit\":{\"type\":\"integer\",\"default\":25},\"callers_offset\":{"
     "\"type\":\"integer\",\"default\":0},"
     "\"caller_files_offset\":{\"type\":\"integer\",\"default\":0},"
     "\"declarations_offset\":{\"type\":\"integer\",\"default\":0},"
     "\"callees_limit\":{\"type\":\"integer\",\"default\":15},"
     "\"source_lines\":{\"type\":\"integer\",\"default\":60,\"description\":\"Lines of the "
     "definition to include (0 for none)\"},\"max_bytes\":{\"type\":\"integer\",\"default\":12000}"
     "},\"required\":[\"symbol\"]}"},

    {"get_graph_schema", "Get graph schema",
     "Get the schema of the knowledge graph (node labels, edge types)",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "]}"},

    {"get_architecture", "Get architecture",
     "Get high-level architecture overview — packages, services, dependencies, and project "
     "structure at a glance. Includes 'clusters': Leiden community detection over the call/import "
     "graph, surfacing the de-facto modules (each with a label, member count, cohesion score, "
     "representative top_nodes, and the packages/edge_types that bind it) — use these to grasp "
     "the real architectural seams, which often cut across the folder layout. Optional path scopes "
     "analysis to nodes under that directory prefix (file_path).",
     /* The aspects enum mirrors VALID_ASPECTS (see aspect_is_valid) — update both together. */
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"path\":{\"type\":"
     "\"string\",\"description\":\"Optional directory prefix to scope architecture (e.g. "
     "apps/hoa)\"},"
     "\"aspects\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"all\","
     "\"overview\",\"structure\",\"dependencies\",\"routes\",\"languages\",\"packages\","
     "\"entry_points\",\"hotspots\",\"boundaries\",\"layers\",\"file_tree\",\"clusters\","
     "\"cycles\"]},"
     "\"description\":\"Aspects to include. 'all' = everything; 'overview' = compact summary "
     "(all except file_tree); omit = all. 'cycles' is opt-in ONLY (never via all/overview): it "
     "scans the whole call graph for circular CALLS dependencies (SCCs of size > 1).\"}},"
     "\"required\":[]}"},

    {"search_code", "Search code",
     "Graph-augmented code search. Finds text patterns via grep, then enriches results with "
     "the knowledge graph: deduplicates matches into containing functions, ranks by structural "
     "importance (definitions first, popular functions next, tests last). "
     "Modes: compact (default, metadata + match lines — token efficient), full (source window "
     "around the matches, at most 80 lines per result), files (just file paths). Use "
     "path_filter regex to scope results. "
     "TRUNCATION: enriched results are capped at limit (default 10) AND at max_bytes (default "
     "24000): when the reply is cut it carries 'truncated', 'shown' and a 'continuation' hint. "
     "'total_grep_matches' (raw grep hit count) and 'total_results' (deduplicated function "
     "count) are always exact. For an exact identifier you already know, plain grep is just as "
     "good; this tool pays off when you want hits grouped by function and ranked.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"project\":{\"type\":"
     "\"string\"},\"file_pattern\":{\"type\":\"string\",\"description\":\"Glob on the file "
     "name (e.g. *.go); with '/', on the repo-relative path (e.g. src/**/*.c, ** = any "
     "dirs)\"},\"path_filter\":{\"type\":\"string\",\"description\":\"Regex "
     "filter on result file paths (e.g. ^src/ or \\\\.(go|ts)$)\"},\"mode\":{\"type\":\"string\","
     "\"enum\":[\"compact\",\"full\",\"files\"],\"default\":\"compact\",\"description\":\"compact: "
     "signatures+metadata (default). full: with source. files: just file list.\"},"
     "\"context\":{\"type\":\"integer\",\"default\":0,\"minimum\":0,\"maximum\":200,"
     "\"description\":\"Lines of context around each match (like grep -C). Only used in "
     "compact mode.\"},"
     "\"regex\":{\"type\":\"boolean\",\"default\":false},\"limit\":{\"type\":\"integer\","
     "\"description\":\"Max enriched results per call. Default 10. Response includes "
     "'total_grep_matches' and 'total_results' so callers can detect truncation. No "
     "offset parameter — raise limit or narrow with file_pattern / path_filter to see more."
     "\",\"default\":10},\"max_bytes\":{\"type\":\"integer\",\"default\":24000,\"description\":"
     "\"Reply byte budget; results are dropped (and 'truncated' set) to fit. 0 disables.\"}},"
     "\"required\":[\"pattern\"]}"},

    {"list_projects", "List projects", "List indexed projects with deterministic pagination",
     "{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\","
     "\"minimum\":0,\"default\":0},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"default\":50},"
     "\"include_details\":{\"type\":\"boolean\",\"default\":false,"
     "\"description\":\"Include root_path, git context, node/edge counts and database "
     "size. Slower: it opens every listed project's database.\"}}}"},
    {"delete_project", "Delete project", "Delete a project from the index",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "\"project\"]}"},

    {"index_status", "Index status",
     "Get the indexing status of a project: node/edge counts, root path, git context, and the "
     "indexing-COVERAGE report — which files the indexer could NOT fully cover (best-effort "
     "signal): 'parse_partial' files WERE indexed but contain line ranges tree-sitter could not "
     "parse — constructs there MAY be missing from the graph (some are still recovered); "
     "'skipped' files were not indexed at all (oversized/read/parse failure). Use this before "
     "trusting graph completeness on a file: if a file is listed, ALSO grep it (especially the "
     "flagged ranges). IMPORTANT: absence from these lists is NOT a completeness guarantee — the "
     "signal only marks what the indexer can detect. For structural queries over the misses use "
     "query_graph(graph=\"missed\"). The report also carries 'not_indexed' — files/dirs excluded "
     "BY DESIGN (gitignore/.cbmignore/skip-lists): deliberate and deterministic, not failures; "
     "change the ignore rules and re-index to include them.",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"}},\"required\":["
     "]}"},

    {"detect_changes", "Detect changes",
     "Map a git diff to its BLAST RADIUS. Resolves changed files to the symbols they define, then "
     "runs ONE multi-source graph traversal to the transitive impact set. RESPONSE: base + "
     "merge_base SHA, changed_files list, impacted_symbols (name label hop, nearest hops first) + "
     "an impacted_modules rollup; impacted_total + truncated are exact. Seeds (the changed "
     "symbols) are excluded from impacted; a changed file reached from another changed file is "
     "not counted as extra impact.",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"scope\":{\"type\":"
     "\"string\",\"enum\":[\"files\",\"symbols\",\"impact\"],\"description\":\"files: changed "
     "files only (no traversal). impact/symbols (default): files + the transitive impact set.\"},"
     "\"direction\":{\"type\":\"string\",\"enum\":[\"inbound\",\"outbound\",\"both\"],\"default\":"
     "\"inbound\",\"description\":\"inbound (default) = the blast radius: transitive CALLERS of "
     "the changed symbols. outbound = what the changed code depends on. both = union.\"},"
     "\"depth\":{\"type\":\"integer\",\"default\":2,\"description\":\"Max traversal hops from the "
     "changed symbols.\"},\"limit\":{\"type\":\"integer\",\"default\":200,\"description\":"
     "\"Max impacted_symbols rows (nearest hops first); the module rollup stays complete.\"},"
     "\"base_branch\":{\"type\":\"string\",\"description\":\"Default: origin/HEAD, else "
     "main, else master, else the upstream.\"},\"since\":{\"type\":\"string\",\"description\":"
     "\"Git ref or tag to compare from (e.g. HEAD~5, v0.5.0). Diffs <ref>...HEAD.\"}},"
     "\"required\":"
     "[\"project\"]}"},

    {"manage_adr", "Manage ADR", "Create or update Architecture Decision Records",
     "{\"type\":\"object\",\"properties\":{\"project\":{\"type\":\"string\"},\"mode\":{\"type\":"
     "\"string\",\"enum\":[\"get\",\"update\",\"sections\"],\"description\":\"update replaces "
     "the entire ADR document; sections only lists existing headings\"},\"content\":{\"type\":"
     "\"string\",\"description\":\"Complete replacement document required by update\"}},"
     "\"additionalProperties\":false,\"required\":[]}"},

    {"ingest_traces", "Ingest traces", "Ingest runtime traces to enhance the knowledge graph",
     "{\"type\":\"object\",\"properties\":{\"traces\":{\"type\":\"array\",\"items\":{\"type\":"
     "\"object\",\"properties\":{\"caller\":{\"type\":\"string\"},\"callee\":{\"type\":\"string\"},"
     "\"count\":{\"type\":\"integer\"}},\"additionalProperties\":false}},\"project\":{\"type\":"
     "\"string\"}},\"required\":[\"traces\",\"project\"]}"},
};

static const int TOOL_COUNT = sizeof(TOOLS) / sizeof(TOOLS[0]);

static void mcp_add_json_schema(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                const char *schema_json) {
    yyjson_doc *schema_doc = yyjson_read(schema_json, strlen(schema_json), 0);
    if (schema_doc) {
        yyjson_mut_val *schema = yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema_doc));
        if (schema) {
            yyjson_mut_obj_add_val(doc, obj, key, schema);
        }
        yyjson_doc_free(schema_doc);
    }
}

static void mcp_add_tool_def(yyjson_mut_doc *doc, yyjson_mut_val *tools, int i) {
    yyjson_mut_val *tool = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, tool, "name", TOOLS[i].name);
    yyjson_mut_obj_add_str(doc, tool, "title", TOOLS[i].title);
    yyjson_mut_obj_add_str(doc, tool, "description", TOOLS[i].description);

    mcp_add_json_schema(doc, tool, "inputSchema", TOOLS[i].input_schema);
    /* Deliberately NO outputSchema. A declared schema makes spec-honoring
     * clients read structuredContent as the authoritative result, and a tool
     * whose payload is not always a JSON object (an error envelope, a plain
     * text answer) then renders as whatever structuredContent happens to hold.
     * The blanket {"type":"object","additionalProperties":true} this replaced
     * validated anything and informed nobody. See cbm_mcp_text_result. */

    yyjson_mut_arr_add_val(tools, tool);
}

static char *cbm_mcp_tools_list_range(int offset, int limit, bool include_next_cursor) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *tools = yyjson_mut_arr(doc);

    if (offset < 0) {
        offset = 0;
    }
    if (offset > TOOL_COUNT) {
        offset = TOOL_COUNT;
    }
    if (limit < 0 || limit > TOOL_COUNT) {
        limit = TOOL_COUNT;
    }

    int end = offset + limit;
    if (end > TOOL_COUNT) {
        end = TOOL_COUNT;
    }

    for (int i = offset; i < end; i++) {
        mcp_add_tool_def(doc, tools, i);
    }

    yyjson_mut_obj_add_val(doc, root, "tools", tools);
    if (include_next_cursor && end < TOOL_COUNT) {
        char cursor[32];
        snprintf(cursor, sizeof(cursor), "%d", end);
        yyjson_mut_obj_add_strcpy(doc, root, "nextCursor", cursor);
    }

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

char *cbm_mcp_tools_list(void) {
    return cbm_mcp_tools_list_range(0, TOOL_COUNT, false);
}

/* Return the JSON input_schema string for a tool by name, or NULL if unknown.
 * Used by the CLI to build --flag arguments and per-tool --help from the same
 * source of truth the MCP tools/list advertises. Static lifetime; do not free. */
const char *cbm_mcp_tool_input_schema(const char *tool_name) {
    if (!tool_name) {
        return NULL;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(TOOLS[i].name, tool_name) == 0) {
            return TOOLS[i].input_schema;
        }
    }
    return NULL;
}

static bool mcp_tool_known(const char *tool_name) {
    return cbm_mcp_tool_input_schema(tool_name) != NULL ||
           (tool_name && strcmp(tool_name, "trace_call_path") == 0);
}

bool cbm_mcp_tool_result_valid(const char *json, bool *is_error) {
    if (is_error) {
        *is_error = true;
    }
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_is_obj(root) ? yyjson_obj_get(root, "content") : NULL;
    yyjson_val *error = yyjson_is_obj(root) ? yyjson_obj_get(root, "isError") : NULL;
    bool valid = content && yyjson_is_arr(content) && error && yyjson_is_bool(error);
    if (valid && is_error) {
        *is_error = yyjson_get_bool(error);
    }
    yyjson_doc_free(doc);
    return valid;
}

bool cbm_mcp_result_is_index_busy(const char *json) {
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    yyjson_val *error = yyjson_obj_get(root, "isError");
    yyjson_val *payload = yyjson_obj_get(root, "structuredContent");
    const char *status = yyjson_get_str(yyjson_obj_get(payload, "status"));
    const char *code = yyjson_get_str(yyjson_obj_get(payload, "code"));
    bool busy = yyjson_is_arr(content) && yyjson_is_bool(error) && !yyjson_get_bool(error) &&
                status && strcmp(status, "busy") == 0 && code && strcmp(code, "index_busy") == 0;
    yyjson_doc_free(doc);
    return busy;
}

static char *index_busy_result(const char *project = NULL) {
    // A clean child exit prevents supervisor crash recovery/quarantine. Clients
    // and watcher must inspect this typed status instead of treating it as done.
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "busy");
    yyjson_mut_obj_add_str(doc, root, "code", "index_busy");
    yyjson_mut_obj_add_str(doc, root, "hint",
                           "Another process is using this database. Retry later.");
    if (project) {
        yyjson_mut_obj_add_str(doc, root, "project", project);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return mcp_text_result_take_owned(json, false);
}

static int mcp_tools_cursor_offset(const char *params_json) {
    if (!params_json) {
        return 0;
    }

    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return 0;
    }

    int offset = 0;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cursor = root ? yyjson_obj_get(root, "cursor") : NULL;
    if (cursor) {
        offset = TOOL_COUNT;
        if (yyjson_is_str(cursor)) {
            const char *cursor_str = yyjson_get_str(cursor);
            if (cursor_str && *cursor_str != '\0') {
                char *endptr = NULL;
                errno = 0;
                long parsed = strtol(cursor_str, &endptr, 10);
                if (endptr && *endptr == '\0' && errno == 0 && parsed >= 0) {
                    offset = parsed > TOOL_COUNT ? TOOL_COUNT : (int)parsed;
                }
            }
        }
    }

    yyjson_doc_free(doc);
    return offset;
}

static char *cbm_mcp_tools_list_page(const char *params_json) {
    return cbm_mcp_tools_list_range(mcp_tools_cursor_offset(params_json), MCP_TOOLS_PAGE_SIZE,
                                    true);
}

/* Supported protocol versions, newest first. The server picks the newest
 * version that it shares with the client (per MCP spec version negotiation). */
static const char *SUPPORTED_PROTOCOL_VERSIONS[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};
static const int SUPPORTED_VERSION_COUNT =
    (int)(sizeof(SUPPORTED_PROTOCOL_VERSIONS) / sizeof(SUPPORTED_PROTOCOL_VERSIONS[0]));

char *cbm_mcp_initialize_response(const char *params_json) {
    /* Determine protocol version: if client requests a version we support,
     * echo it back; otherwise respond with our latest. */
    const char *version = SUPPORTED_PROTOCOL_VERSIONS[0]; /* default: latest */
    if (params_json) {
        yyjson_doc *pdoc = yyjson_read(params_json, strlen(params_json), 0);
        if (pdoc) {
            yyjson_val *pv = yyjson_obj_get(yyjson_doc_get_root(pdoc), "protocolVersion");
            if (pv && yyjson_is_str(pv)) {
                const char *requested = yyjson_get_str(pv);
                for (int i = 0; i < SUPPORTED_VERSION_COUNT; i++) {
                    if (strcmp(requested, SUPPORTED_PROTOCOL_VERSIONS[i]) == 0) {
                        version = SUPPORTED_PROTOCOL_VERSIONS[i];
                        break;
                    }
                }
            }
            yyjson_doc_free(pdoc);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "protocolVersion", version);

    yyjson_mut_val *impl = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, impl, "name", "code-cortex-mcp");
    yyjson_mut_obj_add_str(doc, impl, "version", cbm_cli_get_version());
    yyjson_mut_obj_add_val(doc, root, "serverInfo", impl);

    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tools_cap = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, tools_cap, "listChanged", false);
    yyjson_mut_obj_add_val(doc, caps, "tools", tools_cap);
    yyjson_mut_obj_add_val(doc, root, "capabilities", caps);

    char *out = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return out;
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

char *cbm_mcp_get_tool_name(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *name = yyjson_obj_get(root, "name");
    char *result = NULL;
    if (name && yyjson_is_str(name)) {
        result = heap_strdup(yyjson_get_str(name));
    }
    yyjson_doc_free(doc);
    return result;
}

char *cbm_mcp_get_arguments(const char *params_json) {
    yyjson_doc *doc = yyjson_read(params_json, strlen(params_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *args = yyjson_obj_get(root, "arguments");
    char *result = NULL;
    if (args) {
        result = yyjson_val_write(args, 0, NULL);
    }
    yyjson_doc_free(doc);
    return result ? result : heap_strdup("{}");
}

char *cbm_mcp_get_string_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    char *result = NULL;
    if (val && yyjson_is_str(val)) {
        result = heap_strdup(yyjson_get_str(val));
    }
    yyjson_doc_free(doc);
    return result;
}

static char *canonicalize_repo_path_if_exists(char *repo_path) {
    if (!repo_path) {
        return NULL;
    }
    bool root_syntax = true;
    for (const char *p = repo_path; *p; p++) {
        if (*p != '/' && *p != '\\' && *p != ':') {
            root_syntax = false;
            break;
        }
    }
    if (root_syntax) {
        return repo_path;
    }

    char real[CBM_SZ_4K];
#ifdef _WIN32
    if (_access(repo_path, 0) == 0 && _fullpath(real, repo_path, sizeof(real))) {
        cbm_normalize_path_sep(real);
        char *canonical = heap_strdup(real);
        if (canonical) {
            free(repo_path);
            return canonical;
        }
    }
#else
    if (realpath(repo_path, real)) {
        cbm_normalize_path_sep(real);
        char *canonical = heap_strdup(real);
        if (canonical) {
            free(repo_path);
            return canonical;
        }
    }
#endif

    return repo_path;
}

static char *normalize_project_arg(char *project) {
    if (!project || (!strchr(project, '/') && !strchr(project, '\\'))) {
        return project;
    }

    project = canonicalize_repo_path_if_exists(project);
    char *normalized = cbm_project_name_from_path(project);
    if (normalized) {
        /* The path names the directory; the derived name alone may belong to
         * another one (see cbm_root_matches). */
        cbm_mcp_expect_project_root(project);
        free(project);
        return normalized;
    }
    return project;
}

/* Resolve the project argument, accepting the canonical "project" key plus the
 * aliases a caller naturally reaches for (#640): list_projects surfaces the
 * field as "name" and the not-found hint says "pass the project name", so
 * "project_name" is the usual guess; "project_id" / "projectName" are accepted
 * too. NOT bare "name" — index_repository uses "name" for an explicit
 * project-name override. Caller must free() the result. */
static char *get_project_arg(const char *args_json) {
    char *p = cbm_mcp_get_string_arg(args_json, "project");
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "project_name");
    }
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "project_id");
    }
    if (!p) {
        p = cbm_mcp_get_string_arg(args_json, "projectName");
    }
    return normalize_project_arg(p);
}

int cbm_mcp_get_int_arg(const char *args_json, const char *key, int default_val) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return default_val;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    int result = default_val;
    /* yyjson_get_int truncates 64-bit integers to int (2^32 + 1 became 1);
     * read the full width and treat anything outside int range like a
     * non-integer, i.e. fall back to the caller's default. */
    if (val && yyjson_is_sint(val)) {
        int64_t parsed = yyjson_get_sint(val);
        if (parsed >= INT_MIN && parsed <= INT_MAX) {
            result = (int)parsed;
        }
    } else if (val && yyjson_is_uint(val)) {
        uint64_t parsed = yyjson_get_uint(val);
        if (parsed <= (uint64_t)INT_MAX) {
            result = (int)parsed;
        }
    }
    yyjson_doc_free(doc);
    return result;
}

/* Parse a numeric (real or int) arg. Returns default_val when missing
 * or non-numeric. Mirrors cbm_mcp_get_int_arg. */
static double cbm_mcp_get_double_arg(const char *args_json, const char *key, double default_val) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return default_val;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    double result = default_val;
    if (val && yyjson_is_num(val)) {
        result = yyjson_get_num(val);
    }
    yyjson_doc_free(doc);
    return result;
}

bool cbm_mcp_get_bool_arg(const char *args_json, const char *key) {
    yyjson_doc *doc = yyjson_read(args_json, strlen(args_json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *val = yyjson_obj_get(root, key);
    bool result = false;
    if (val && yyjson_is_bool(val)) {
        result = yyjson_get_bool(val);
    }
    yyjson_doc_free(doc);
    return result;
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP SERVER
 * ══════════════════════════════════════════════════════════════════ */

struct cbm_mcp_server {
    cbm_store_t *store;               /* currently open project store (or NULL) */
    bool read_batch_tool;             /* this request's store reads run in a read batch */
    bool owns_store;                  /* true if we opened the store */
    char *current_project;            /* which project store is open for (heap) */
    time_t store_last_used;           /* last time resolve_store was called for a named project */
    bool store_resolution_incomplete; /* cache scan stopped on a busy DB or its deadline */
    /* Why the last resolve produced no store. The VERDICT reason is one field,
     * not a bool per outcome: two bools admitted a meaningless both-true state,
     * and the error builder was only correct because it happened to test them
     * in the right order. store_resolution_incomplete above stays separate — it
     * reports that the cache SCAN was truncated, which is a different fact from
     * what a scanned database turned out to be. */
    cbm_integrity_verdict_t store_resolution_state;
    /* Memo for the O(db size) integrity verdict, keyed on a file generation. */
    char verdict_path[CBM_SZ_1K];
    cbm_file_gen_t verdict_gen;
    cbm_integrity_verdict_t verdict_result;
    bool verdict_valid;
    /* Generation of the file the cached store was opened from; see
     * cbm_mcp_server_revalidate_store. */
    cbm_file_gen_t store_gen;
    bool store_gen_valid;
    /* When set, resolution stops at the direct open + the on-disk memo and
     * never walks the cache dir. See cbm_mcp_server_set_scan_fallback. */
    bool no_scan_fallback;
    char update_notice[CBM_SZ_256]; /* one-shot update notice, cleared after first injection */
    bool update_checked;            /* true after background check has been launched */
    cbm_thread_t update_tid;        /* background update check thread */
    bool update_thread_active;      /* true if update thread was started and needs joining */

    /* Session + auto-index state */
    char session_root[CBM_SZ_1K];     /* detected project root path */
    char session_project[CBM_SZ_256]; /* derived project name */
    bool session_detected;            /* true after first detection attempt */
    struct cbm_watcher *watcher;      /* external watcher ref (not owned) */
    struct cbm_config *config;        /* external config ref (not owned) */
    cbm_thread_t autoindex_tid;
    bool autoindex_active; /* true if auto-index thread was started */
    /* Cache expiry sweep (cache_expiry.h): at most one thread at a time,
     * joined before the next one starts, before the watcher changes
     * (cbm_mcp_server_set_watcher) and at cbm_mcp_server_join_threads/free.
     * The struct is calloc'd, so cbm_mcp_server_new constructs sweep_running
     * in place; zeroed bytes are not a constructed std::atomic. */
    cbm_thread_t sweep_tid;
    bool sweep_active;
    std::atomic<bool> sweep_running;
    int autoindex_file_limit; /* auto_index_limit, read before the thread starts */

    /* Active pipeline tracking for cancellation support */
    cbm_pipeline_t *active_pipeline; /* non-NULL while index_repository runs */
    int64_t active_request_id;       /* JSON-RPC id of the in-progress tool call */
    char *active_request_id_str;     /* string JSON-RPC id of the in-progress tool call */
};

/* The server is calloc'd and released with free(): every member must be
 * valid when zeroed except the ones cbm_mcp_server_new constructs in place,
 * and none may need a destructor. */
static_assert(std::is_trivially_destructible_v<cbm_mcp_server>,
              "cbm_mcp_server is released with free(); members must not need destructors");

cbm_mcp_server_t *cbm_mcp_server_new(const char *store_path) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)calloc(CBM_ALLOC_ONE, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    /* calloc gives zeroed bytes, not objects: begin the atomic's lifetime. */
    new (&srv->sweep_running) std::atomic<bool>(false);
    srv->sweep_active = false;

    /* If a store_path is given, open that project directly.
     * Otherwise, create an in-memory store for test/embedded use. */
    if (store_path) {
        srv->store = cbm_store_open(store_path);
        srv->current_project = heap_strdup(store_path);
    } else {
        srv->store = cbm_store_open_memory();
    }
    srv->owns_store = true;

    return srv;
}

cbm_store_t *cbm_mcp_server_store(cbm_mcp_server_t *srv) {
    return srv ? srv->store : NULL;
}

void cbm_mcp_server_set_project(cbm_mcp_server_t *srv, const char *project) {
    if (!srv) {
        return;
    }
    free(srv->current_project);
    srv->current_project = project ? heap_strdup(project) : NULL;
}

/* Join the cache sweep thread, if one was started. The sweep reads
 * srv->watcher (sweep_keep_watched), so it must be gone before the watcher
 * changes or is freed. */
static void join_sweep(cbm_mcp_server_t *srv) {
    if (srv->sweep_active) {
        cbm_thread_join(&srv->sweep_tid);
        srv->sweep_active = false;
    }
}

void cbm_mcp_server_set_watcher(cbm_mcp_server_t *srv, struct cbm_watcher *w) {
    if (srv) {
        join_sweep(srv);
        srv->watcher = w;
    }
}

void cbm_mcp_server_join_threads(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->update_thread_active) {
        cbm_thread_join(&srv->update_tid);
        srv->update_thread_active = false;
    }
    if (srv->autoindex_active) {
        cbm_thread_join(&srv->autoindex_tid);
        srv->autoindex_active = false;
    }
    join_sweep(srv);
}

void cbm_mcp_server_set_config(cbm_mcp_server_t *srv, struct cbm_config *cfg) {
    if (srv) {
        srv->config = cfg;
    }
}

void cbm_mcp_server_free(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    cbm_mcp_server_join_threads(srv);
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
    }
    free(srv->current_project);
    free(srv->active_request_id_str);
    free(srv);
}

/* ── Idle store eviction ──────────────────────────────────────── */

void cbm_mcp_server_evict_idle(cbm_mcp_server_t *srv, int timeout_s) {
    if (!srv || !srv->store) {
        return;
    }
    /* Protect initial in-memory stores that were never accessed via a named project.
     * store_last_used stays 0 until resolve_store is called with a non-NULL project. */
    if (srv->store_last_used == 0) {
        return;
    }

    time_t now = time(NULL);
    if ((now - srv->store_last_used) < timeout_s) {
        return;
    }

    if (srv->owns_store) {
        cbm_store_close(srv->store);
    }
    srv->store = NULL;
    free(srv->current_project);
    srv->current_project = NULL;
    srv->store_last_used = 0;
}

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*store_open_test_cb)(const char *, void *) = nullptr;
static void *store_open_test_data = nullptr;

void cbm_mcp_test_store_open_hook(void (*callback)(const char *db_path, void *data), void *data) {
    store_open_test_cb = callback;
    store_open_test_data = data;
}
#define STORE_OPEN_TEST_HOOK(path)                          \
    do {                                                    \
        if (store_open_test_cb) {                           \
            store_open_test_cb(path, store_open_test_data); \
        }                                                   \
    } while (0)
#else
#define STORE_OPEN_TEST_HOOK(path) ((void)0)
#endif

/* Record the generation of the file the cached store reads, for
 * cbm_mcp_server_revalidate_store. It is stat'ed by path after the open, the
 * integrity check and the project read, and an index run can replace the
 * file in that time (a full rebuild deletes and recreates it; a restore with
 * no live database renames one in). The path's generation would then be the
 * new file's while the connection reads the old one, and every later call
 * would find them "equal" and keep answering from the deleted file: the
 * multi-process stress test saw a server serve another branch's graph that
 * way, persistently. So the generation counts only when its inode is the
 * one the open saw; otherwise it is recorded as never equal, and the next
 * call reopens.
 *
 * The same holds when nothing is at the path any more: an incremental run
 * unlinks the file and only renames its re-dump in when the dump is done,
 * so the stat can fail while the connection reads the deleted file. An
 * unknown generation used to leave store_gen_valid false, which
 * cbm_mcp_server_revalidate_store reads as "never revalidate": the stress
 * test saw a tool worker answer from the deleted pre-switch graph for every
 * call until it exited. A failed stat is recorded as never equal too. */
static void note_store_generation(cbm_mcp_server_t *srv, const char *path) {
    srv->store_gen_valid = false;
    if (!path || !path[0]) {
        return;
    }
    bool at_path = cbm_file_generation(path, &srv->store_gen);
    uint64_t ino = 0;
    bool raced = !at_path || (srv->store_gen.ino != 0 && (!cbm_store_open_inode(srv->store, &ino) ||
                                                          ino != srv->store_gen.ino));
    if (raced) {
        cbm_log_info("store.generation_raced", "path", path, "reason",
                     at_path ? "replaced" : "file_gone");
        srv->store_gen.size = CBM_NOT_FOUND; /* cbm_file_gen_equal: never equal */
    }
    srv->store_gen_valid = true;
}

void cbm_mcp_server_revalidate_store(cbm_mcp_server_t *srv) {
    if (!srv || !srv->store) {
        return;
    }
    /* Same file or not, an index run may have written into it in place:
     * start this request on a fresh read snapshot. */
    cbm_store_end_reads(srv->store);
    if (!srv->owns_store || !srv->store_gen_valid) {
        return;
    }
    const char *path = cbm_store_db_path(srv->store);
    if (!path || !path[0]) {
        return;
    }
    cbm_file_gen_t now;
    bool exists = cbm_file_generation(path, &now);
    if (exists && cbm_file_gen_equal(&now, &srv->store_gen)) {
        return;
    }
    cbm_log_info("store.revalidate.reopen", "project",
                 srv->current_project ? srv->current_project : "", "reason",
                 exists ? "generation_changed" : "file_gone");
    cbm_store_close(srv->store);
    srv->store = NULL;
    free(srv->current_project);
    srv->current_project = NULL;
    srv->store_last_used = 0;
    srv->store_gen_valid = false;
}

bool cbm_mcp_server_has_cached_store(cbm_mcp_server_t *srv) {
    return (srv && srv->store != NULL) != 0;
}

void cbm_mcp_server_set_scan_fallback(cbm_mcp_server_t *srv, bool enabled) {
    if (srv) {
        srv->no_scan_fallback = !enabled;
    }
}

cbm_pipeline_t *cbm_mcp_server_active_pipeline(cbm_mcp_server_t *srv) {
    return srv ? srv->active_pipeline : NULL;
}

/* ── Cache dir + project DB path helpers ───────────────────────── */

/* Returns the cache directory. Writes to buf, returns buf for convenience. */
static const char *cache_dir(char *buf, size_t bufsz) {
    const char *dir = cbm_resolve_cache_dir();
    if (!dir) {
        dir = cbm_tmpdir();
    }
    snprintf(buf, bufsz, "%s", dir);
    return buf;
}

/* Returns full .db path for a project: <cache_dir>/<project>.db */
static const char *project_db_path(const char *project, char *buf, size_t bufsz) {
    if (!cbm_validate_project_name(project)) {
        buf[0] = '\0';
        return buf;
    }
    char dir[CBM_SZ_1K];
    cache_dir(dir, sizeof(dir));
    snprintf(buf, bufsz, "%s/%s.db", dir, project);
    return buf;
}

/* ── Implicit project resolution from the working directory ─────────
 *
 * Two agent studies (243 and 504 sessions) found that 10-12% of every graph
 * call failed with "project not found" because the agent guessed the project
 * name — while the server sat in a working directory that IS the indexed
 * repository. Query tools therefore accept a missing project and resolve it
 * from cwd (walking up to HA-style 8 parents), and when a caller names a
 * project that does not exist anywhere in the cache they get the cwd project
 * plus a project_note explaining the substitution. Destructive tools
 * (delete_project, index_repository's name override) keep the strict path. */

enum { CWD_PROJECT_MAX_WALKUP = 8 };

static char g_cwd_project[CBM_SZ_256];
static bool g_cwd_project_probed;
/* Cache dir the memo was computed against; a change (tests repoint
 * CBM_CACHE_DIR per fixture) invalidates it. */
static char g_cwd_project_cache_dir[CBM_SZ_1K];
/* Generation of the memoized project's database when the hit was accepted.
 * The ownership check behind the hit judged THAT file: a replacement (an
 * expiry and a re-index from a directory that derives the same name) is
 * another file, and is judged again. */
static cbm_file_gen_t g_cwd_project_gen;

static bool project_db_exists(const char *project) {
    if (!project || !project[0]) {
        return false;
    }
    char path[CBM_SZ_1K];
    project_db_path(project, path, sizeof(path));
    return path[0] != '\0' && cbm_file_exists(path);
}

/* ── Project ownership of a directory ───────────────────────────
 * A project name derived from a path is not unique: cbm_project_name_from_path
 * folds separators and unsafe characters to '-', so /x/a-b/repo and
 * /x/a/b-repo both derive the same name. A resolver that tries a directory's
 * derived name must therefore check that the database found under it was
 * indexed from THAT directory before answering from it -- otherwise an
 * unindexed checkout is silently answered from another one. */

/* The directory the current tool call's project must have been indexed from,
 * or "": set by a walk-up level (cbm_mcp_expect_project_root) or by a project
 * argument given as a path. Not cleared per call: cbm_mcp_handle_tool and
 * every other reader of a project argument outside a tool call save it and
 * restore it afterwards, so a path argument scopes only its own call and a
 * caller's walk-up level stays in force. Per process, like g_project_note. */
static char g_expected_root[CBM_SZ_4K];

void cbm_mcp_expect_project_root(const char *dir) {
    snprintf(g_expected_root, sizeof(g_expected_root), "%s", dir ? dir : "");
}

static void store_meta_note_project(const char *path, const char *project, const char *root_path);

/* Whether <project>.db was indexed from dir: the root recorded in the memo at
 * the file's current generation, else read from the database once (and
 * memoized, as resolve_store does). True when the file is absent or holds no
 * row for project. False when the database exists but cannot be read: an
 * owner that cannot be learned may be another checkout's, whose index a
 * watcher here would overwrite -- fail closed. */
static bool project_db_owned_by(const char *project, const char *dir, cbm_store_meta_db_t *meta) {
    char path[CBM_SZ_1K];
    project_db_path(project, path, sizeof(path));
    cbm_file_gen_t gen;
    if (!path[0] || !cbm_file_generation(path, &gen)) {
        return true;
    }
    cbm_store_meta_row_t row;
    if (meta && cbm_store_meta_get(meta, path, &gen, &row) && strcmp(row.project, project) == 0 &&
        strlen(row.root_path) < CBM_STORE_META_PATH_MAX - 1) { /* else maybe truncated */
        return cbm_root_matches(row.root_path, dir);
    }
    cbm_store_t *s = cbm_store_open_path_query(path);
    cbm_project_t proj = {0};
    int prc = s ? cbm_store_get_project(s, project, &proj) : CBM_STORE_ERR;
    bool owned = prc == CBM_STORE_NOT_FOUND;
    if (prc == CBM_STORE_OK) {
        owned = cbm_root_matches(proj.root_path, dir);
        store_meta_note_project(path, project, proj.root_path);
        cbm_project_free_fields(&proj);
    } else if (prc != CBM_STORE_NOT_FOUND) {
        cbm_log_info("project.owner_unreadable", "project", project, "path", path);
    }
    cbm_store_close(s);
    return owned;
}

/* Record the generation of <project>.db as the memo's; true when it exists. */
static bool cwd_project_note_gen(const char *project) {
    char path[CBM_SZ_1K];
    project_db_path(project, path, sizeof(path));
    return path[0] && cbm_file_generation(path, &g_cwd_project_gen);
}

/* Project name derived from cwd or one of its ancestors that has an indexed
 * db, or NULL. Cached for the life of the process (cwd does not change). */
static const char *cwd_project_name(void) {
    char cdir[CBM_SZ_1K];
    cache_dir(cdir, sizeof(cdir));
    /* Only a HIT is memoized, and only while its db is the same generation
     * that was checked: a persistent worker started in an unindexed repository
     * must pick the index up once index_repository has run, a deleted project
     * must not linger, and a database replaced by one indexed from another
     * directory (derived names collide) must be checked for ownership again.
     * A miss re-probes (eight stats and memo lookups), which is cheap; so does
     * a re-index of the project itself, once. */
    if (g_cwd_project_probed && strcmp(cdir, g_cwd_project_cache_dir) == 0 && g_cwd_project[0]) {
        char path[CBM_SZ_1K];
        project_db_path(g_cwd_project, path, sizeof(path));
        cbm_file_gen_t now;
        if (path[0] && cbm_file_generation(path, &now) &&
            cbm_file_gen_equal(&now, &g_cwd_project_gen)) {
            return g_cwd_project;
        }
    }
    g_cwd_project_probed = true;
    g_cwd_project[0] = '\0';
    snprintf(g_cwd_project_cache_dir, sizeof(g_cwd_project_cache_dir), "%s", cdir);
    char dir[CBM_SZ_1K];
    if (getcwd(dir, sizeof(dir)) == NULL) {
        return NULL;
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    for (int level = 0; level < CWD_PROJECT_MAX_WALKUP; level++) {
        char *pname = cbm_project_name_from_path(dir);
        if (pname) {
            /* The generation is taken before the checks: a file replaced
             * after it is a changed generation on the next call. */
            cwd_project_note_gen(pname);
            bool hit = project_db_exists(pname) && project_db_owned_by(pname, dir, meta);
            if (hit) {
                snprintf(g_cwd_project, sizeof(g_cwd_project), "%s", pname);
            }
            free(pname);
            if (hit) {
                cbm_store_meta_close(meta);
                return g_cwd_project;
            }
        }
        /* A project indexed under a custom name: the memo records its root.
         * The row may describe an earlier generation of the file, so the
         * root is confirmed against the file as it is now. */
        cbm_store_meta_row_t row;
        if (meta && cbm_store_meta_find_root(meta, dir, &row) && row.project[0] &&
            cwd_project_note_gen(row.project) && project_db_exists(row.project) &&
            project_db_owned_by(row.project, dir, meta)) {
            snprintf(g_cwd_project, sizeof(g_cwd_project), "%s", row.project);
            cbm_store_meta_close(meta);
            return g_cwd_project;
        }
        /* An unindexed git checkout root (a linked worktree, a nested repo,
         * one made with --separate-git-dir)
         * ends the walk: the enclosing checkout's index is other code. A
         * submodule's gitlink does not: discovery indexes its files into the
         * enclosing checkout's graph. */
        cbm_git_checkout_kind_t kind = cbm_git_checkout_root_kind(dir);
        if (kind == CBM_GIT_CHECKOUT_MAIN ||
            (kind == CBM_GIT_CHECKOUT_GITLINK && !cbm_git_gitlink_is_submodule(dir))) {
            break;
        }
        char *slash = strrchr(dir, '/');
        char *bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) {
            slash = bslash;
        }
        if (!slash || slash == dir) {
            break;
        }
        *slash = '\0';
    }
    cbm_store_meta_close(meta);
    return NULL;
}

/* Test seam: forget the cached cwd lookup (tests chdir between fixtures). */
void cbm_mcp_reset_cwd_project_cache(void) {
    g_cwd_project_probed = false;
    g_cwd_project[0] = '\0';
}

const char *cbm_mcp_cwd_project(void) {
    return cwd_project_name();
}

/* True when the name is served by SOME db in the cache: the direct file, or a
 * legacy db whose internal name the memo has already recorded. */
static bool project_known(const char *project) {
    if (project_db_exists(project)) {
        return true;
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    if (!meta) {
        return false;
    }
    cbm_store_meta_row_t row;
    bool known = cbm_store_meta_find_project(meta, project, &row);
    cbm_store_meta_close(meta);
    if (!known) {
        return false;
    }
    /* A memo row outlives its file (delete, quarantine): only count it when
     * the db it names is still there. */
    char dir[CBM_SZ_1K];
    cache_dir(dir, sizeof(dir));
    char db_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s", dir, row.db_name);
    return row.db_name[0] != '\0' && cbm_file_exists(db_path);
}

static cbm_store_t *resolve_store(cbm_mcp_server_t *srv, const char *project);

/* True when the caller passed a filesystem path rather than a name. */
static bool project_arg_is_path(const char *args_json) {
    static const char *keys[] = {"project", "project_name", "project_id", "projectName"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char *raw = cbm_mcp_get_string_arg(args_json, keys[i]);
        if (raw) {
            bool is_path = strchr(raw, '/') != NULL || strchr(raw, '\\') != NULL;
            free(raw);
            return is_path;
        }
    }
    return false;
}

/* get_project_arg for read-only tools. A missing argument resolves to the cwd
 * project. With allow_substitute, an unknown name ALSO falls back to the cwd
 * project and records a project_note for the reply, so an agent that guessed
 * "pcapplusplus" for /Users/me/PcapPlusPlus gets an answer instead of an error
 * list. Returns NULL only when nothing resolves (existing error paths apply). */
static char *get_project_arg_or_cwd(cbm_mcp_server_t *srv, const char *args_json,
                                    bool allow_substitute) {
    char *given = get_project_arg(args_json);
    /* The hook (no_scan_fallback) probes speculative names derived from the
     * payload's own cwd/file path; the process cwd must not answer for them. */
    if (srv && srv->no_scan_fallback) {
        return given;
    }
    /* The server's own open store (an in-memory store, or a project set via
     * cbm_mcp_server_set_project) is known by definition. */
    if (srv && srv->current_project && given && strcmp(given, srv->current_project) == 0) {
        return given;
    }
    if (!given) {
        const char *cwd_p = cwd_project_name();
        return cwd_p ? heap_strdup(cwd_p) : NULL;
    }
    if (!allow_substitute || project_arg_is_path(args_json)) {
        return given; /* an explicit location is never second-guessed */
    }
    /* "Known" means the real resolution succeeds — file name, memo, or the
     * cache-dir scan — not merely that <name>.db exists; a project indexed
     * under a custom name lives in a file the memo maps. */
    if (project_known(given) || (srv && resolve_store(srv, given) != NULL)) {
        return given;
    }
    const char *cwd_p = cwd_project_name();
    if (!cwd_p || strcmp(cwd_p, given) == 0) {
        return given;
    }
    snprintf(g_project_note, sizeof(g_project_note),
             "'%s' is not an indexed project; answered for '%s', the indexed project that "
             "contains the working directory. Omit the project argument or pass "
             "project='%s'.",
             given, cwd_p, cwd_p);
    free(given);
    return heap_strdup(cwd_p);
}

/* ── Store resolution ──────────────────────────────────────────── */

/* Read the sole INTERNAL project name from a .db file at full_path.
 * Opens the file query-mode (no create) and succeeds ONLY when the db holds
 * exactly one project row with a non-empty name — this filters ghost/empty
 * /corrupt dbs (0-byte file, missing `projects` table, or >1 row). On success
 * the internal name is copied into name_out; if out_store is non-NULL the open
 * handle is transferred to the caller (who must cbm_store_close it). On failure
 * the store is always closed. Defined below. */
typedef enum {
    DB_PROJECT_INVALID = 0,
    DB_PROJECT_FOUND,
    DB_PROJECT_BUSY,
    DB_PROJECT_DEADLINE,
} db_project_result_t;

static db_project_result_t db_internal_project_name(const char *full_path, char *name_out,
                                                    size_t name_sz, cbm_store_t **out_store,
                                                    uint64_t deadline_ms);

/* #704 fallback: scan the cache dir for the db whose sole internal project name
 * equals `project`, returning an open store handle (caller owns it) or NULL.
 * Used only when <project>.db is absent or its internal name differs from the
 * passed name (drifted filename). Defined below. */
static cbm_store_t *resolve_store_fallback_scan(const char *project, bool *incomplete,
                                                bool memo_only);

/* Integrity verdict for a store, memoized on the file's (path, size, mtime)
 * generation.
 *
 * The verdict walks the whole btree (PRAGMA quick_check), which measured ~160ms
 * on a 41MB cache database — and resolve_store runs on EVERY tool call, while
 * the store cache is evicted after STORE_IDLE_TIMEOUT_S. An agent client with
 * any pause between calls would therefore re-walk the entire database at the
 * start of every turn. Both the header and the implementation promise this
 * check stays off the hot path; the memo is what makes that true.
 *
 * Detection is unchanged: any rewrite of the file changes its size or mtime, so
 * a re-indexed or newly damaged generation is always re-examined.
 *
 * TRANSIENT is deliberately NOT memoized. It means "ask again" — caching it
 * would turn one unlucky lock collision into a sticky failure for the life of
 * the process. */
static cbm_integrity_verdict_t resolve_store_verdict(cbm_mcp_server_t *srv, cbm_store_t *store,
                                                     const char *path) {
    cbm_file_gen_t gen;
    (void)cbm_file_generation(path, &gen);
    bool signable = gen.size >= 0 && strlen(path) < sizeof(srv->verdict_path);
    if (signable && srv->verdict_valid && cbm_file_gen_equal(&srv->verdict_gen, &gen) &&
        strcmp(srv->verdict_path, path) == 0) {
        return srv->verdict_result;
    }

    /* Second level: the on-disk memo (store_meta.h). The in-memory memo above
     * dies with the process, and every tool worker and hook invocation IS a
     * fresh process, so without this every one of them re-walked the btree.
     * Only a recorded OK is trusted: a recorded CORRUPT means the file was
     * quarantined (renamed) at the time, so whatever sits at `path` now is a
     * different generation and gets checked for real. */
    cbm_store_meta_db_t *meta = signable ? cbm_store_meta_open() : NULL;
    cbm_store_meta_row_t row;
    bool from_disk = false;
    cbm_integrity_verdict_t verdict = CBM_INTEGRITY_OK;
    if (meta && cbm_store_meta_get(meta, path, &gen, &row) &&
        row.verdict == (int)CBM_INTEGRITY_OK) {
        from_disk = true;
    } else {
        verdict = cbm_store_check_integrity_verdict(store);
    }
    if (signable && verdict != CBM_INTEGRITY_TRANSIENT) {
        snprintf(srv->verdict_path, sizeof(srv->verdict_path), "%s", path);
        srv->verdict_gen = gen;
        srv->verdict_result = verdict;
        srv->verdict_valid = true;
    } else {
        srv->verdict_valid = false;
    }
    if (meta && !from_disk && verdict != CBM_INTEGRITY_TRANSIENT) {
        /* Keep whatever else the row already knows (project name, counts). */
        if (!cbm_store_meta_get(meta, path, &gen, &row)) {
            cbm_store_meta_row_init(&row, path, &gen);
        }
        row.verdict = (int)verdict;
        (void)cbm_store_meta_put(meta, &row);
    }
    cbm_store_meta_close(meta);
    return verdict;
}

/* Record a store's internal project name and root path in the on-disk memo so
 * the fallback scan and list_projects can answer "which file is project X" and
 * "what is in file Y" without opening Y. Skips the write when the row already
 * says the same thing. Best-effort; never affects the caller's result. */
static void store_meta_note_project(const char *path, const char *project, const char *root_path) {
    if (!path || !path[0] || !project || !project[0]) {
        return;
    }
    cbm_file_gen_t gen;
    if (!cbm_file_generation(path, &gen)) {
        return;
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    if (!meta) {
        return;
    }
    cbm_store_meta_row_t row;
    bool hit = cbm_store_meta_get(meta, path, &gen, &row);
    const char *root = root_path ? root_path : "";
    if (!hit || strcmp(row.project, project) != 0 || strcmp(row.root_path, root) != 0) {
        if (!hit) {
            cbm_store_meta_row_init(&row, path, &gen);
        }
        snprintf(row.project, sizeof(row.project), "%s", project);
        snprintf(row.root_path, sizeof(row.root_path), "%s", root);
        (void)cbm_store_meta_put(meta, &row);
    }
    cbm_store_meta_close(meta);
}

static cbm_store_t *resolve_store_open(cbm_mcp_server_t *srv, const char *project);

/* Query-only callers (every read tool). A tool listed in
 * mcp_tool_reads_in_batch runs its reads in one store read batch. */
static cbm_store_t *resolve_store(cbm_mcp_server_t *srv, const char *project) {
    cbm_store_t *store = resolve_store_open(srv, project);
    if (store && srv->read_batch_tool) {
        (void)cbm_store_read_batch_begin(store);
    }
    return store;
}

static cbm_store_t *resolve_store_by_name(cbm_mcp_server_t *srv, const char *project);

/* resolve_store_by_name, then the ownership check when the call expects a
 * root (g_expected_root): a database indexed from another directory under
 * the same derived name is "not found" here. One point query on the handle
 * the call needs anyway. */
static cbm_store_t *resolve_store_open(cbm_mcp_server_t *srv, const char *project) {
    cbm_store_t *store = resolve_store_by_name(srv, project);
    if (store && g_expected_root[0]) {
        cbm_project_t proj = {0};
        if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK) {
            bool owned = cbm_root_matches(proj.root_path, g_expected_root);
            if (!owned) {
                cbm_log_info("project.name_collision", "project", project, "indexed_root",
                             proj.root_path ? proj.root_path : "", "wanted", g_expected_root);
                store = NULL;
            }
            cbm_project_free_fields(&proj);
        }
    }
    if (store && cbm_validate_project_name(project)) {
        /* Usage for cache expiry (cache_expiry.h): throttled in process, so a
         * repeat call costs a map lookup. The file actually opened, which the
         * fallback scan may have found under another name than <project>.db. */
        const char *used = cbm_store_db_path(store);
        if (used && used[0] && cbm_file_exists(used)) {
            cbm_cache_usage_touch(used, NULL);
        }
    }
    return store;
}

/* Open the right project's .db file for query tools.
 * Caches the connection — reopens only when project changes.
 * Tracks last-access time so the event loop can evict idle stores. */
static cbm_store_t *resolve_store_by_name(cbm_mcp_server_t *srv, const char *project) {
    srv->store_resolution_incomplete = false;
    srv->store_resolution_state = CBM_INTEGRITY_OK;
    if (!project) {
        return NULL; /* project is required — no implicit fallback */
    }

    srv->store_last_used = time(NULL);

    /* Already open for this project? */
    if (srv->current_project && strcmp(srv->current_project, project) == 0 && srv->store) {
        return srv->store;
    }

    /* Close old store */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }

    /* Open project's .db file — query-only open (no SQLITE_OPEN_CREATE) to
     * prevent ghost .db file creation for unknown/unindexed projects.
     * #1425: a project name failing cbm_validate_project_name yields an EMPTY
     * path, and SQLite opens "" as an anonymous temp database. That healthy
     * temp db then fails the integrity check (it has no projects table) and
     * drives the corrupt-db backup below for a database that never existed —
     * rendering a bare relative ".corrupt" target in the process cwd and
     * logging a spurious store.auto_clean error on every such query, while the
     * caller only ever saw a clean 'project not found'. Skip the direct open
     * entirely; the fallback scan below still resolves legacy dbs whose
     * internal name predates validation. */
    char path[CBM_SZ_1K];
    project_db_path(project, path, sizeof(path));
    /* A reader open: it holds the database's reader gate while cached, so the
     * cache expiry sweep does not move the file under this connection. */
    srv->store = path[0] ? cbm_store_open_path_reader(path) : NULL;
    if (srv->store) {
        STORE_OPEN_TEST_HOOK(path);
        /* Check DB integrity — back up (never silently delete) a corrupt DB.
         *
         * #1206/#1037: the decision is tri-state, not a bool. The shallow
         * cbm_store_check_integrity() reports failure for ANY prepare error,
         * so a writer lock held by a second MCP instance (SQLITE_BUSY — a
         * transient condition, not damage) used to quarantine a perfectly
         * healthy database and rebuild a sparse replacement. It also never ran
         * quick_check, so the inverse case — a torn node/edge btree behind an
         * intact projects table — passed and kept being served. Only a
         * CONFIRMED corrupt verdict may rename a file; a transient one leaves
         * the DB untouched and lets the next resolve retry. */
        cbm_integrity_verdict_t verdict = resolve_store_verdict(srv, srv->store, path);
        if (verdict == CBM_INTEGRITY_TRANSIENT) {
            /* Report it as an incomplete lookup, NOT as "not found": the latter
             * reads to a user as "re-index me", which is the one action that
             * would destroy the graph this branch exists to protect. */
            cbm_log_error("store.busy", "project", project, "path", path, "action",
                          "integrity check inconclusive (db busy or locked) — not quarantining");
            cbm_store_close(srv->store);
            srv->store = NULL;
            srv->store_resolution_state = CBM_INTEGRITY_TRANSIENT;
            return NULL;
        }
        if (verdict == CBM_INTEGRITY_CORRUPT) {
            cbm_db_lease_t *lease = NULL;
            int claim_rc = cbm_db_lease_try_acquire(path, &lease);
            if (claim_rc != 0) {
                cbm_store_close(srv->store);
                srv->store = NULL;
                srv->verdict_valid = false;
                srv->store_resolution_state = CBM_INTEGRITY_TRANSIENT;
                cbm_log_warn("store.repair.deferred", "project", project, "reason",
                             claim_rc == CBM_INDEX_BUSY ? "index_busy" : "lease_error");
                return NULL;
            }
            cbm_store_close(srv->store);
            // The lease coordinates the referent, but repair must preserve the
            // requested filename: renaming a symlink must not delete its target.
            srv->store = cbm_store_open_path_query(path);
            srv->verdict_valid = false;
            cbm_integrity_verdict_t current = srv->store
                                                  ? cbm_store_check_integrity_verdict(srv->store)
                                                  : CBM_INTEGRITY_TRANSIENT;
            if (current != CBM_INTEGRITY_CORRUPT) {
                if (srv->store) {
                    cbm_store_close(srv->store);
                    srv->store = NULL;
                }
                cbm_db_lease_release(lease);
                // Even an OK replacement is reopened through normal resolution
                // on retry, rather than continuing with the old corrupt verdict.
                srv->store_resolution_state = CBM_INTEGRITY_TRANSIENT;
                return NULL;
            }
            cbm_log_error("store.auto_clean", "project", project, "path", path, "action",
                          "backing up corrupt db to .corrupt — re-index required");
            cbm_store_close(srv->store);
            srv->store = NULL;
            /* #557 (data loss): rename the corrupt DB to a .corrupt backup instead
             * of unlinking it, so the user's graph is recoverable / reportable.
             * Re-index rebuilds a fresh DB at `path`. WAL/SHM are transient. */
            char bak_path[MCP_FIELD_SIZE];
            snprintf(bak_path, sizeof(bak_path), "%s.corrupt", path);
            cbm_unlink(bak_path); /* clear any prior backup so rename succeeds on Windows */
            if (rename(path, bak_path) != 0) {
                cbm_unlink(path); /* rename failed (e.g. cross-device) — fall back to delete */
            }
            char wal_path[MCP_FIELD_SIZE];
            char shm_path[MCP_FIELD_SIZE];
            snprintf(wal_path, sizeof(wal_path), "%s-wal", path);
            snprintf(shm_path, sizeof(shm_path), "%s-shm", path);
            cbm_unlink(wal_path);
            cbm_unlink(shm_path);
            cbm_cache_usage_forget(path);
            cbm_db_lease_release(lease);
            srv->store_resolution_state = CBM_INTEGRITY_CORRUPT;
            return NULL;
        }

        /* Verify the project actually exists in this database.
         * A .db file may exist but be empty (e.g., after delete_project on
         * Linux where unlink defers actual removal). Opening an empty/deleted
         * store without closing it leaks the SQLite connection. */
        cbm_project_t proj_verify = {0};
        if (cbm_store_get_project(srv->store, project, &proj_verify) == CBM_STORE_OK) {
            store_meta_note_project(path, project, proj_verify.root_path);
            cbm_project_free_fields(&proj_verify);
            srv->owns_store = true;
            free(srv->current_project);
            srv->current_project = heap_strdup(project);
            note_store_generation(srv, path);
            return srv->store; /* fast path: filename == internal name */
        }
        /* #704: <project>.db exists but its INTERNAL project name differs from
         * the passed name (a copied/renamed db, or a legacy '.'-vs-'-' username
         * twin). Close it and fall through to the cache-dir scan below. */
        cbm_store_close(srv->store);
        srv->store = NULL;
    }

    /* #704 fallback: either <project>.db is absent or its internal name drifted
     * from its filename. Node rows are keyed on the INTERNAL name (== the passed
     * name, since list_projects now advertises internal names), so scan the
     * cache dir for the db whose sole internal project name equals `project` and
     * adopt it. Runs ONLY on the fallback — the common fast path is unchanged.
     * No match → NULL (a genuine typo stays not-found). */
    cbm_store_t *scanned = resolve_store_fallback_scan(project, &srv->store_resolution_incomplete,
                                                       srv->no_scan_fallback);
    if (scanned) {
        /* The scan only proves the `projects` row matches. Without this the
         * #1037 hole simply moves: a drifted-filename database with a torn
         * node/edge btree passes that row check and gets served, silently, as
         * an empty-but-successful result — exactly what the direct-open branch
         * above stopped doing.
         *
         * This branch REFUSES rather than quarantines. The scan resolves a file
         * whose name does not match the project, and it does not hand back the
         * path it used, so there is nothing here that can safely be renamed —
         * and renaming the wrong file is worse than refusing to answer. The
         * user's route out is delete_project or a re-index, which the
         * "not found or not indexed" hint already names. */
        /* Through the memoized path, not a raw check: this is the SAME
         * expensive quick_check the direct-open branch above learned to skip.
         * Left raw, every fresh tool worker and every hook invocation re-walked
         * the btree for a drifted-name database — 45ms on a 54MB store, ~400ms
         * on a 100MB one, which on its own blows the hook's 300ms alarm and
         * silently drops the augmentation it was about to emit. */
        const char *scanned_path = cbm_store_db_path(scanned);
        cbm_integrity_verdict_t scanned_verdict =
            scanned_path && scanned_path[0] ? resolve_store_verdict(srv, scanned, scanned_path)
                                            : cbm_store_check_integrity_verdict(scanned);
        if (scanned_verdict != CBM_INTEGRITY_OK) {
            cbm_log_error("store.scan_rejected", "project", project, "verdict",
                          scanned_verdict == CBM_INTEGRITY_CORRUPT ? "corrupt" : "inconclusive",
                          "action", "refusing to serve — re-index to rebuild");
            cbm_store_close(scanned);
            /* Report what actually happened. Marking confirmed damage as an
             * "incomplete lookup" would tell the user a database was BUSY when
             * it is broken, and would hide the re-index guidance that is the
             * only way out of it. */
            srv->store_resolution_state = scanned_verdict;
            return NULL;
        }
        srv->store = scanned;
        srv->owns_store = true;
        free(srv->current_project);
        srv->current_project = heap_strdup(project);
        note_store_generation(srv, cbm_store_db_path(scanned));
    }

    return srv->store;
}

/* True for the coverage kinds hook-augment reports as "not indexed at all".
 * not_indexed_dir / not_indexed_file describe paths outside the index scope
 * rather than files the indexer tried and failed on, and have never been
 * surfaced on Read; everything else in this bucket is a real per-file skip
 * (read / extract / oversized). */
static bool coverage_kind_is_skip(const char *kind) {
    return strcmp(kind, "parse_partial") != 0 && strcmp(kind, "parse_unusable") != 0 &&
           strcmp(kind, "walk_truncated") != 0 && strcmp(kind, "lsp_skipped") != 0 &&
           strcmp(kind, "not_indexed_dir") != 0 && strcmp(kind, "not_indexed_file") != 0;
}

char *cbm_mcp_symbol_nodes(cbm_mcp_server_t *srv, const char *project, const char *name,
                           bool *resolved, bool prefix) {
    if (resolved) {
        *resolved = false;
    }
    if (!srv || !project || !name || !name[0]) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) != CBM_STORE_OK) {
        return NULL;
    }
    if (resolved) {
        *resolved = true;
    }
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (prefix) {
        cbm_store_find_nodes_by_name_prefix(store, project, name, 40, &nodes, &count);
    } else {
        cbm_store_find_nodes_by_name(store, project, name, &nodes, &count);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "root", proj.root_path ? proj.root_path : "");
    cbm_project_free_fields(&proj);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    enum { SYMBOL_NODES_MAX = 1000 };
    for (int i = 0; i < count && i < SYMBOL_NODES_MAX; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "qualified_name",
                                  nodes[i].qualified_name ? nodes[i].qualified_name : "");
        yyjson_mut_obj_add_strcpy(doc, item, "name", nodes[i].name ? nodes[i].name : "");
        yyjson_mut_obj_add_strcpy(doc, item, "label", nodes[i].label ? nodes[i].label : "");
        yyjson_mut_obj_add_strcpy(doc, item, "file", nodes[i].file_path ? nodes[i].file_path : "");
        yyjson_mut_obj_add_int(doc, item, "start_line", nodes[i].start_line);
        yyjson_mut_obj_add_int(doc, item, "end_line", nodes[i].end_line);
        yyjson_mut_arr_append(arr, item);
    }
    yyjson_mut_obj_add_val(doc, root, "nodes", arr);
    yyjson_mut_obj_add_int(doc, root, "total", count);
    cbm_store_free_nodes(nodes, count);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

int cbm_mcp_project_files(cbm_mcp_server_t *srv, const char *project, char **root_out,
                          char ***files_out) {
    *root_out = NULL;
    *files_out = NULL;
    if (!srv || !project) {
        return -1;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return -1;
    }
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) != CBM_STORE_OK) {
        return -1;
    }
    *root_out = heap_strdup(proj.root_path ? proj.root_path : "");
    cbm_project_free_fields(&proj);
    cbm_file_hash_t *hashes = NULL;
    int count = 0;
    if (cbm_store_get_file_hashes(store, project, &hashes, &count) != CBM_STORE_OK) {
        count = 0;
    }
    char **files = count > 0 ? (char **)calloc((size_t)count, sizeof(char *)) : NULL;
    int n = 0;
    for (int i = 0; files && i < count; i++) {
        if (hashes[i].rel_path) {
            files[n++] = heap_strdup(hashes[i].rel_path);
        }
    }
    cbm_store_free_file_hashes(hashes, count);
    *files_out = files;
    return n;
}

void cbm_mcp_free_project_files(char *root, char **files, int count) {
    for (int i = 0; files && i < count; i++) {
        free(files[i]);
    }
    free(files);
    free(root);
}

char *cbm_mcp_session_brief_json(cbm_mcp_server_t *srv, const char *project, bool *resolved) {
    if (resolved) {
        *resolved = false;
    }
    if (!srv || !project) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) != CBM_STORE_OK) {
        return NULL;
    }
    cbm_project_free_fields(&proj);
    if (resolved) {
        *resolved = true;
    }
    char *json = NULL;
    if (cbm_store_session_brief_get(store, project, &json) == CBM_STORE_OK) {
        return json;
    }
    int64_t ceiling = cbm_store_node_id_ceiling(store);
    if (ceiling < CBM_MCP_BRIEF_LIVE_MAX_NODES &&
        cbm_store_session_brief_compute(store, project, &json) == CBM_STORE_OK) {
        return json;
    }
    free(json);
    char buf[CBM_SZ_128];
    snprintf(buf, sizeof(buf), "{\"nodes\":%lld,\"approximate\":true}", (long long)ceiling);
    return heap_strdup(buf);
}

char *cbm_mcp_coverage_note(cbm_mcp_server_t *srv, const char *project, const char *rel_path,
                            bool *resolved) {
    if (resolved) {
        *resolved = false;
    }
    if (!srv || !project || !rel_path || !rel_path[0]) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    if (resolved) {
        *resolved = true;
    }

    cbm_coverage_row_t *rows = NULL;
    int count = 0;
    if (cbm_store_coverage_get(store, project, &rows, &count) != CBM_STORE_OK) {
        return NULL;
    }
    char note[CBM_SZ_1K];
    note[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (!rows[i].rel_path || strcmp(rows[i].rel_path, rel_path) != 0) {
            continue;
        }
        const char *kind = rows[i].kind ? rows[i].kind : "";
        const char *detail = rows[i].detail ? rows[i].detail : "";
        if (strcmp(kind, "parse_unusable") == 0) {
            snprintf(note, sizeof(note),
                     "[code-cortex] Coverage note: the parse of this file failed across nearly "
                     "the whole of it, so most constructs are missing from the knowledge graph. "
                     "The file content you are reading is ground truth; graph queries will "
                     "under-report this file. (best-effort signal)");
            break;
        }
        if (strcmp(kind, "parse_partial") == 0) {
            snprintf(note, sizeof(note),
                     "[code-cortex] Coverage note: this file was only PARTIALLY indexed — line "
                     "range(s) %s could not be parsed, so constructs there may be missing from "
                     "the knowledge graph. The file content you are reading is ground truth; "
                     "graph queries may under-report this file. (best-effort signal)",
                     detail[0] ? detail : "?");
            break; /* the partial-parse note wins over any skip row */
        }
        if (strcmp(kind, "walk_truncated") == 0) {
            snprintf(note, sizeof(note),
                     "[code-cortex] Coverage note: extraction stopped partway through this file "
                     "(%s), so calls and usages past that point are missing from the knowledge "
                     "graph. The file content you are reading is ground truth; graph queries may "
                     "under-report this file. (best-effort signal)",
                     detail[0] ? detail : "?");
            break;
        }
        if (strcmp(kind, "lsp_skipped") == 0 && !note[0]) {
            snprintf(note, sizeof(note),
                     "[code-cortex] Coverage note: this file is too large or too deeply nested "
                     "for type-aware call resolution (%s), so its calls are resolved by name "
                     "only and some may be "
                     "missing or ambiguous in the knowledge graph. (best-effort signal)",
                     detail[0] ? detail : "?");
            continue;
        }
        if (coverage_kind_is_skip(kind) && !note[0]) {
            snprintf(note, sizeof(note),
                     "[code-cortex] Coverage note: this file was NOT indexed (%s%s%s) — the "
                     "knowledge graph has no data for it. (best-effort signal)",
                     kind[0] ? kind : "skipped", detail[0] ? ": " : "", detail);
        }
    }
    cbm_store_free_coverage(rows, count);
    return note[0] ? heap_strdup(note) : NULL;
}

/* Forward decl — definition lives below alongside list_projects. */

/* Forward decl — definition lives below in handle_trace_call_path's helpers. */
static void free_node_contents(cbm_node_t *n);

/* Scan cache dir for .db files, writing comma-separated quoted names into out.
 * Returns the number of projects found. */
static int collect_db_project_names(const char *dir_path, char *out, size_t out_sz,
                                    bool *incomplete) {
    *incomplete = false;
    int count = 0;
    int offset = 0;
    uint64_t deadline_ms = cbm_now_ms() + MCP_FALLBACK_SCAN_TIMEOUT_MS;
    cbm_dir_t *d = cbm_opendir(dir_path);
    if (!d) {
        return 0;
    }
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (cbm_now_ms() >= deadline_ms) {
            *incomplete = true;
            cbm_log_warn("store.project_list.incomplete", "reason", "deadline");
            break;
        }
        const char *n = entry->name;
        if (!cbm_is_project_index_db(n)) {
            continue;
        }
        /* #704: advertise the db's INTERNAL project name, not its filename, and
         * skip ghost/empty/corrupt dbs — so the hint lists names the user can
         * actually pass to resolve a store. */
        char full_path[CBM_SZ_2K];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, n);
        if (!cbm_is_regular_file(full_path)) {
            continue;
        }
        char iname[CBM_SZ_1K];
        db_project_result_t db_result =
            db_internal_project_name(full_path, iname, sizeof(iname), NULL, deadline_ms);
        if (db_result == DB_PROJECT_BUSY || db_result == DB_PROJECT_DEADLINE) {
            *incomplete = true;
            cbm_log_warn("store.project_list.incomplete", "path", full_path, "reason",
                         db_result == DB_PROJECT_BUSY ? "database_busy" : "deadline");
        }
        if (db_result != DB_PROJECT_FOUND) {
            continue;
        }
        /* Element-boundary write: only emit this name if the WHOLE element —
         * optional leading comma + "iname" — plus the NUL fits in what remains.
         * Never truncate mid-token; a partial name would corrupt the JSON array
         * (issue #235). Stop cleanly at the last name that fits: the array then
         * always holds complete names and `count` == its length. */
        size_t off = (size_t)offset;
        size_t need = strlen(iname) + 2 /* quotes */ + (count > 0 ? 1u : 0u) /* comma */;
        if (off + need + 1 > out_sz) {
            *incomplete = true;
            cbm_log_warn("store.project_list.incomplete", "reason", "response_buffer_full");
            break; /* would not fit entirely — stop at this element boundary */
        }
        if (count > 0) {
            out[offset++] = ',';
        }
        int wrote = snprintf(out + offset, out_sz - (size_t)offset, "\"%s\"", iname);
        if (wrote > 0) {
            offset += wrote; /* guaranteed to fit (checked above) — no truncation */
        }
        count++;
    }
    cbm_closedir(d);
    return count;
}

static void add_git_context_string(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                                   const char *value) {
    if (value) {
        yyjson_mut_obj_add_strcpy(doc, obj, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, obj, key);
    }
}

/* Per-branch index facts for one project (pipeline/branch_slots.h), added to
 * its git block: the branch the graph was built for (only when the caller has
 * the store open — list_projects answers from the store_meta memo and does
 * not), whether that matches the checkout, and the parked branch databases (a
 * directory read). A mismatch means the graph answers for another branch
 * until the next index run swaps it. */
typedef struct {
    int keep;
    int64_t budget;
} branch_limits_t;
static branch_limits_t branch_index_limits(const cbm_mcp_server_t *srv);

static void add_branch_index_json(yyjson_mut_doc *doc, yyjson_mut_val *git, cbm_store_t *store,
                                  const char *project, const char *root_path,
                                  const cbm_git_context_t *ctx) {
    /* The per-slot detail is index_status's (it has the store open);
     * list_projects keeps to the names. */
    bool detail = store != NULL;
    if (!project || !ctx->is_git) {
        return;
    }
    if (store) {
        char *name = NULL;
        if (cbm_store_get_branch_dup(store, project, NULL, &name, NULL) == CBM_STORE_OK) {
            yyjson_mut_obj_add_strcpy(doc, git, "indexed_branch", name ? name : "");
            /* Branch name and HEAD commit, not the slug two branches can
             * share; a graph with no recorded commit is not claimed current
             * (the strict form the content check uses). */
            cbm_branch_id_t indexed;
            cbm_branch_id_t current;
            cbm_branch_id_from_context(ctx, &current);
            if (cbm_branch_id_indexed(store, project, &indexed) && current.known) {
                yyjson_mut_obj_add_bool(doc, git, "index_matches_checkout",
                                        cbm_branch_id_same_strict(&indexed, &current));
            }
            cbm_branch_id_free(&indexed);
            cbm_branch_id_free(&current);
        }
        free(name);
    }
    char db_path[CBM_SZ_1K];
    project_db_path(project, db_path, sizeof(db_path));
    char **parked = NULL;
    /* This checkout's slots only: a derived name can be several roots'. */
    bool here = db_path[0] && root_path && root_path[0];
    int n = here ? cbm_branch_slots_list(db_path, project, root_path, &parked) : 0;
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < n; i++) {
        yyjson_mut_arr_add_strcpy(doc, arr, parked[i]);
    }
    cbm_branch_slots_free_list(parked, n);
    yyjson_mut_obj_add_val(doc, git, "parked_branches", arr);
    if (!detail) {
        return;
    }
    /* Each slot file with its size, their total, and the limits the next
     * park enforces. */
    cbm_branch_slot_info_t *info = NULL;
    int ni = here ? cbm_branch_slots_info(db_path, project, root_path, &info) : 0;
    yyjson_mut_val *files = yyjson_mut_arr(doc);
    int64_t total = 0;
    for (int i = 0; i < ni; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, o, "branch", info[i].name);
        yyjson_mut_obj_add_int(doc, o, "bytes", info[i].bytes);
        yyjson_mut_obj_add_bool(doc, o, "compressed", info[i].compressed);
        yyjson_mut_arr_append(files, o);
        total += info[i].bytes > 0 ? info[i].bytes : 0;
    }
    cbm_branch_slots_free_info(info, ni);
    yyjson_mut_obj_add_val(doc, git, "parked_slots", files);
    yyjson_mut_obj_add_int(doc, git, "parked_bytes", total);
    branch_limits_t lim = branch_index_limits(NULL);
    yyjson_mut_obj_add_int(doc, git, "branch_index_keep",
                           lim.keep >= 0 ? lim.keep : CBM_BRANCH_SLOTS_DEFAULT_KEEP);
    yyjson_mut_obj_add_int(doc, git, "branch_index_budget_bytes",
                           lim.budget >= 0 ? lim.budget : CBM_BRANCH_SLOTS_DEFAULT_BUDGET);
}

static void add_git_context_json(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *root_path,
                                 cbm_store_t *store = NULL, const char *project = NULL) {
    cbm_git_context_t ctx = {0};
    (void)cbm_git_context_resolve(root_path, &ctx);

    yyjson_mut_val *git = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, git, "is_git", ctx.is_git);
    yyjson_mut_obj_add_bool(doc, git, "is_worktree", ctx.is_worktree);
    yyjson_mut_obj_add_bool(doc, git, "is_detached", ctx.is_detached);
    yyjson_mut_obj_add_bool(doc, git, "root_exists", ctx.root_exists);
    add_git_context_string(doc, git, "worktree_root", ctx.worktree_root);
    add_git_context_string(doc, git, "git_dir", ctx.git_dir);
    add_git_context_string(doc, git, "git_common_dir", ctx.git_common_dir);
    add_git_context_string(doc, git, "canonical_root", ctx.canonical_root);
    add_git_context_string(doc, git, "branch", ctx.branch);
    add_git_context_string(doc, git, "branch_slug", ctx.branch_slug);
    add_git_context_string(doc, git, "head_sha", ctx.head_sha);
    add_git_context_string(doc, git, "base_sha", ctx.base_sha);
    add_branch_index_json(doc, git, store, project, root_path, &ctx);
    yyjson_mut_obj_add_val(doc, obj, "git", git);

    cbm_git_context_free(&ctx);
}

/* Build a helpful error listing available projects. Caller must free() result. */
static char *build_project_list_error(const char *reason) {
    char dir_path[CBM_SZ_1K];
    cache_dir(dir_path, sizeof(dir_path));

    char projects[CBM_SZ_4K] = "";
    bool projects_incomplete = false;
    int count =
        collect_db_project_names(dir_path, projects, sizeof(projects), &projects_incomplete);

    enum { ERR_BUF_SZ = 5120 };
    char buf[ERR_BUF_SZ];
    if (count > 0) {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"hint\":\"Use list_projects to see all indexed projects, "
                 "then pass it as the \\\"project\\\" "
                 "argument.\",\"available_projects\":[%s],\"count\":%d,"
                 "\"available_projects_incomplete\":%s}",
                 reason, projects, count, projects_incomplete ? "true" : "false");
    } else if (projects_incomplete) {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"hint\":\"Project discovery was incomplete because "
                 "a cache database was busy or the scan deadline expired. Retry the call.\","
                 "\"available_projects\":[],\"count\":0,"
                 "\"available_projects_incomplete\":true}",
                 reason);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"hint\":\"No projects indexed yet. "
                 "Call index_repository first.\"}",
                 reason);
    }
    return heap_strdup(buf);
}

/* Distinct from "unknown project": the caller omitted the project argument
 * entirely (no recognized key). Name the literal "project" key so the fix is
 * obvious (#640). Caller must free() result. */
static char *build_missing_project_error(void) {
    return heap_strdup("{\"error\":\"missing required argument: project\",\"hint\":\"The "
                       "working directory is not inside an indexed repository, so the project "
                       "could not be inferred. Pass it as the \\\"project\\\" argument, e.g. "
                       "{\\\"project\\\":\\\"<name from list_projects>\\\"}, or run "
                       "index_repository for this repository first.\"}");
}

/* Pick the right no-store error: a NULL project means the argument was missing
 * (clearer message); a non-NULL project that didn't resolve means it's
 * unknown/unindexed (list the available ones). */
static char *build_no_store_error(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return build_missing_project_error();
    }
    if (srv->store_resolution_state == CBM_INTEGRITY_CORRUPT) {
        return build_project_list_error(
            "project database failed its integrity check and was not served — "
            "re-run index_repository to rebuild it");
    }
    if (srv->store_resolution_state == CBM_INTEGRITY_TRANSIENT ||
        srv->store_resolution_incomplete) {
        return build_project_list_error(
            "project lookup incomplete: a cache database was busy or the scan deadline expired");
    }
    return build_project_list_error("project not found or not indexed");
}

/* Bail with the right error when no store is available. */
#define REQUIRE_STORE(store, project)                        \
    do {                                                     \
        if (!(store)) {                                      \
            char *_err = build_no_store_error(srv, project); \
            char *_res = cbm_mcp_text_result(_err, true);    \
            free(_err);                                      \
            free(project);                                   \
            return _res;                                     \
        }                                                    \
    } while (0)

static bool project_has_adr(cbm_store_t *store, const char *project, const char *root_path) {
    if (store && project) {
        cbm_adr_t adr;
        memset(&adr, 0, sizeof(adr));
        if (cbm_store_adr_get(store, project, &adr) == CBM_STORE_OK) {
            cbm_store_adr_free(&adr);
            return true;
        }
    }

    if (!root_path) {
        return false;
    }

    char adr_path[CBM_SZ_4K];
    snprintf(adr_path, sizeof(adr_path), "%s/.code-cortex/adr.md", root_path);
    return cbm_file_exists(adr_path);
}

/* ── Tool handler implementations ─────────────────────────────── */

/* db_internal_project_name — see forward declaration above resolve_store. */
/* `ctx` cannot be const-qualified: sqlite3_progress_handler fixes the callback
 * type as int(*)(void*), so constifying the parameter changes the function
 * pointer's type and requires a cast at the registration site — trading a real
 * type mismatch for a cosmetic one. The body already reads through a const
 * pointer, which is where the guarantee belongs. */
// cppcheck-suppress constParameterCallback
static int db_scan_deadline_expired(void *ctx) {
    uint64_t deadline_ms = *(const uint64_t *)ctx;
    return deadline_ms > 0 && cbm_now_ms() >= deadline_ms;
}

static db_project_result_t db_internal_project_name(const char *full_path, char *name_out,
                                                    size_t name_sz, cbm_store_t **out_store,
                                                    uint64_t deadline_ms) {
    if (out_store) {
        *out_store = NULL;
    }
    if (deadline_ms > 0 && cbm_now_ms() >= deadline_ms) {
        return DB_PROJECT_DEADLINE;
    }
    cbm_store_t *st = cbm_store_open_path_query(full_path);
    if (!st) {
        return DB_PROJECT_INVALID; /* nonexistent / unreadable */
    }
    sqlite3 *db = cbm_store_get_db(st);
    if (deadline_ms > 0 && db) {
        sqlite3_busy_timeout(db, MCP_FALLBACK_DB_BUSY_MS);
        sqlite3_progress_handler(db, 1000, db_scan_deadline_expired, &deadline_ms);
    }
    cbm_project_t *projs = NULL;
    int n = 0;
    int list_err = cbm_store_list_projects(st, &projs, &n);
    bool ok = false;
    /* Ignore internal shadow projects ("<name>::missed" miss-graph rows): they
     * live in the SAME db as the primary project, so ANY project that has ever
     * recorded a parse miss carries two rows. Requiring n == 1 over ALL rows
     * therefore made such a project unresolvable — reproduced here: a project
     * indexed with 8 partially-parsed files disappeared from list_projects
     * entirely, while a clean project beside it resolved fine.
     *
     * The single-primary requirement itself is KEPT: it is what proves this db
     * belongs to one project rather than being a shared or mislabelled store.
     * Only the shadow rows stop counting toward it. */
    int primary_count = 0;
    const char *primary_name = NULL;
    if (list_err == CBM_STORE_OK) {
        for (int i = 0; i < n; i++) {
            const char *name = projs[i].name;
            if (name && name[0] && !strstr(name, "::")) {
                primary_count++;
                primary_name = name;
            }
        }
    }
    if (primary_count == 1 && primary_name) {
        snprintf(name_out, name_sz, "%s", primary_name);
        ok = true;
    }
    if (deadline_ms > 0 && db) {
        sqlite3_progress_handler(db, 0, NULL, NULL);
        sqlite3_busy_timeout(db, 10000);
    }
    cbm_store_free_projects(projs, n);
    db_project_result_t result = DB_PROJECT_INVALID;
    if (ok) {
        result = DB_PROJECT_FOUND;
    } else if (deadline_ms > 0 && cbm_now_ms() >= deadline_ms) {
        result = DB_PROJECT_DEADLINE;
    } else if (db && (sqlite3_errcode(db) == SQLITE_BUSY || sqlite3_errcode(db) == SQLITE_LOCKED)) {
        result = DB_PROJECT_BUSY;
    }
    if (ok && out_store) {
        *out_store = st; /* transfer ownership to caller */
    } else {
        cbm_store_close(st);
    }
    return result;
}

/* resolve_store_fallback_scan — see forward declaration above resolve_store. */
static cbm_store_t *resolve_store_fallback_scan(const char *project, bool *incomplete,
                                                bool memo_only) {
    char dir_path[CBM_SZ_1K];
    cache_dir(dir_path, sizeof(dir_path));
    uint64_t deadline_ms = cbm_now_ms() + MCP_FALLBACK_SCAN_TIMEOUT_MS;
    cbm_store_meta_db_t *meta = cbm_store_meta_open();

    /* Memo shortcut: a previous scan (or list_projects, or a plain open) may
     * already have recorded which file carries this internal name. Trust it
     * only while the file is the same generation, and still verify the name by
     * opening that one file — the memo narrows the search, it never answers
     * for the database. */
    cbm_store_meta_row_t hint;
    if (meta && cbm_store_meta_find_project(meta, project, &hint) && hint.db_name[0]) {
        char hint_path[CBM_SZ_2K];
        snprintf(hint_path, sizeof(hint_path), "%s/%s", dir_path, hint.db_name);
        cbm_file_gen_t hgen;
        (void)cbm_file_generation(hint_path, &hgen);
        if (cbm_file_gen_equal(&hgen, &hint.gen) && cbm_is_regular_file(hint_path)) {
            char iname[CBM_SZ_1K];
            cbm_store_t *st = NULL;
            if (db_internal_project_name(hint_path, iname, sizeof(iname), &st, deadline_ms) ==
                DB_PROJECT_FOUND) {
                /* The gate comes after the read here: a file moved between
                 * the two reads as gone. A path still there either has its
                 * gate held (a move in progress) or holds another file than
                 * the one read (moved, then restored or re-indexed): the
                 * lookup is incomplete, not a miss, and a scan would only
                 * meet the same file again. */
                if (strcmp(iname, project) == 0) {
                    if (cbm_store_hold_read_gate(st)) {
                        cbm_store_meta_close(meta);
                        return st; /* caller takes ownership */
                    }
                    if (cbm_file_exists(hint_path)) {
                        cbm_store_close(st);
                        cbm_store_meta_close(meta);
                        *incomplete = true;
                        return NULL;
                    }
                }
                cbm_store_close(st);
            }
        }
    }

    if (memo_only) {
        /* The caller (the hook) runs under a hard deadline and would rather
         * report nothing than walk a cache dir holding hundreds of databases.
         * The memo shortcut above is all it gets; a miss is simply not-found. */
        cbm_store_meta_close(meta);
        return NULL;
    }

    cbm_dir_t *d = cbm_opendir(dir_path);
    if (!d) {
        cbm_store_meta_close(meta);
        return NULL;
    }
    cbm_store_t *found = NULL;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(d)) != NULL) {
        if (cbm_now_ms() >= deadline_ms) {
            cbm_log_warn("store.fallback.timeout", "project", project);
            *incomplete = true;
            break;
        }
        const char *n = entry->name;
        if (!cbm_is_project_index_db(n)) {
            continue;
        }
        char full_path[CBM_SZ_2K];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, n);
        if (!cbm_is_regular_file(full_path)) {
            continue;
        }
        /* A file whose internal name is already on record for this generation
         * does not need opening: it either is not this project (skip) or it is
         * (open to verify, as before). Unknown files are opened and recorded, so
         * the NEXT scan opens only files that changed since. This is what turns
         * a 500-database cache from ~400ms of sqlite opens into a stat loop. */
        cbm_file_gen_t fgen;
        (void)cbm_file_generation(full_path, &fgen);
        cbm_store_meta_row_t row;
        bool known = meta && cbm_store_meta_get(meta, full_path, &fgen, &row) && row.project[0];
        if (known && strcmp(row.project, project) != 0) {
            continue;
        }
        char iname[CBM_SZ_1K];
        cbm_store_t *st = NULL;
        db_project_result_t db_result =
            db_internal_project_name(full_path, iname, sizeof(iname), &st, deadline_ms);
        if (db_result == DB_PROJECT_BUSY || db_result == DB_PROJECT_DEADLINE) {
            *incomplete = true;
            cbm_log_warn("store.fallback.incomplete", "project", project, "path", full_path,
                         "reason", db_result == DB_PROJECT_BUSY ? "database_busy" : "deadline");
        }
        if (db_result == DB_PROJECT_FOUND) {
            if (meta && !known && fgen.size >= 0) {
                cbm_store_meta_row_init(&row, full_path, &fgen);
                snprintf(row.project, sizeof(row.project), "%s", iname);
                (void)cbm_store_meta_put(meta, &row);
            }
            if (strcmp(iname, project) == 0) {
                if (cbm_store_hold_read_gate(st)) {
                    found = st; /* adopt — caller takes ownership */
                    break;
                }
                if (cbm_file_exists(full_path)) {
                    /* Its gate stays held (a move in progress), or another
                     * file took the path since the read: the connection
                     * would serve the moved one. */
                    *incomplete = true;
                    cbm_log_warn("store.fallback.incomplete", "project", project, "path", full_path,
                                 "reason", "read_gate_busy_or_replaced");
                }
            }
            cbm_store_close(st);
        }
    }
    cbm_closedir(d);
    cbm_store_meta_close(meta);
    return found;
}

/* Open a .db file briefly, collect node/edge counts and root_path,
 * then append a JSON entry to arr. */
/* Append one list_projects entry. `include_details` adds root/git/counts. */
static void list_projects_emit_entry(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                     const char *project_name, const char *root_path, int nodes,
                                     int edges, int64_t size_bytes, bool include_details) {
    yyjson_mut_val *p = yyjson_mut_obj(doc);
    /* The NAME is always present — it is the identifier every other tool takes,
     * and the reason list_projects exists. The rest is opt-in. */
    yyjson_mut_obj_add_strcpy(doc, p, "name", project_name);
    if (include_details) {
        yyjson_mut_obj_add_strcpy(doc, p, "root_path", root_path);
        add_git_context_json(doc, p, root_path[0] ? root_path : NULL, NULL, project_name);
        yyjson_mut_obj_add_int(doc, p, "nodes", nodes);
        yyjson_mut_obj_add_int(doc, p, "edges", edges);
        yyjson_mut_obj_add_int(doc, p, "size_bytes", size_bytes);
    }
    yyjson_mut_arr_add_val(arr, p);
}

static db_project_result_t build_project_json_entry(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                                    const char *dir_path, const char *name,
                                                    size_t name_len, int64_t size_bytes,
                                                    uint64_t deadline_ms, bool include_details,
                                                    cbm_store_meta_db_t *meta) {
    (void)name_len;

    char full_path[CBM_SZ_2K];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);

    /* Memo hit for this generation of the file: answer without opening it. The
     * counts are only required when the caller asked for details; a row that
     * knows the name but not the counts still saves the open otherwise. */
    cbm_file_gen_t fgen;
    (void)cbm_file_generation(full_path, &fgen);
    cbm_store_meta_row_t memo;
    bool memo_hit = meta && cbm_store_meta_get(meta, full_path, &fgen, &memo) && memo.project[0];
    if (memo_hit && (!include_details || (memo.nodes >= 0 && memo.edges >= 0))) {
        list_projects_emit_entry(doc, arr, memo.project, memo.root_path, memo.nodes, memo.edges,
                                 size_bytes, include_details);
        return DB_PROJECT_FOUND;
    }

    /* #704: key on the db's INTERNAL project name, not its filename. Node/edge
     * rows are tagged with the internal name, so a drifted filename (copied or
     * renamed db, legacy '.'-vs-'-' username twin) would otherwise report 0
     * nodes/edges and be unresolvable. Skip ghost/empty/corrupt dbs entirely so
     * they don't appear as resolvable projects. */
    char project_name[CBM_SZ_1K];
    cbm_store_t *pstore = NULL;
    db_project_result_t project_result = db_internal_project_name(
        full_path, project_name, sizeof(project_name), &pstore, deadline_ms);
    if (project_result != DB_PROJECT_FOUND) {
        return project_result; /* ghost / unreadable — not a resolvable project */
    }

    /* The default response only needs the internal project name. Do not run
     * count queries or load project metadata that the response will discard.
     * Besides making the opt-in contract real, this keeps pagination bounded
     * on large databases and under instrumentation. Cache the name-only row so
     * subsequent calls do not need to reopen the database. */
    if (!include_details) {
        cbm_store_close(pstore);
        if (meta && fgen.size >= 0) {
            if (!memo_hit) {
                cbm_store_meta_row_init(&memo, full_path, &fgen);
            }
            snprintf(memo.project, sizeof(memo.project), "%s", project_name);
            (void)cbm_store_meta_put(meta, &memo);
        }
        list_projects_emit_entry(doc, arr, project_name, "", CBM_STORE_META_UNKNOWN,
                                 CBM_STORE_META_UNKNOWN, size_bytes, false);
        return DB_PROJECT_FOUND;
    }

    sqlite3 *db = cbm_store_get_db(pstore);
    if (db) {
        sqlite3_busy_timeout(db, MCP_FALLBACK_DB_BUSY_MS);
        sqlite3_progress_handler(db, 1000, db_scan_deadline_expired, &deadline_ms);
    }
    int nodes = cbm_store_count_nodes(pstore, project_name);
    int nodes_db_error = db ? sqlite3_errcode(db) : SQLITE_OK;
    int edges = cbm_store_count_edges(pstore, project_name);
    int edges_db_error = db ? sqlite3_errcode(db) : SQLITE_OK;
    char root_path_buf[CBM_SZ_1K] = "";
    cbm_project_t proj = {0};
    int project_store_result = cbm_store_get_project(pstore, project_name, &proj);
    int project_db_error = db ? sqlite3_errcode(db) : SQLITE_OK;
    if (project_store_result == CBM_STORE_OK) {
        if (proj.root_path) {
            snprintf(root_path_buf, sizeof(root_path_buf), "%s", proj.root_path);
        }
        cbm_project_free_fields(&proj);
    }
    bool deadline_expired = cbm_now_ms() >= deadline_ms;
    if (db) {
        sqlite3_progress_handler(db, 0, NULL, NULL);
        sqlite3_busy_timeout(db, 10000);
    }
    cbm_store_close(pstore);
    if (deadline_expired || nodes_db_error == SQLITE_INTERRUPT ||
        edges_db_error == SQLITE_INTERRUPT || project_db_error == SQLITE_INTERRUPT) {
        return DB_PROJECT_DEADLINE;
    }
    if (nodes_db_error == SQLITE_BUSY || nodes_db_error == SQLITE_LOCKED ||
        edges_db_error == SQLITE_BUSY || edges_db_error == SQLITE_LOCKED ||
        project_db_error == SQLITE_BUSY || project_db_error == SQLITE_LOCKED) {
        return DB_PROJECT_BUSY;
    }
    if (nodes < 0 || edges < 0 || project_store_result != CBM_STORE_OK) {
        return DB_PROJECT_INVALID;
    }

    if (meta && fgen.size >= 0) {
        if (!memo_hit) {
            cbm_store_meta_row_init(&memo, full_path, &fgen);
        }
        snprintf(memo.project, sizeof(memo.project), "%s", project_name);
        snprintf(memo.root_path, sizeof(memo.root_path), "%s", root_path_buf);
        memo.nodes = nodes;
        memo.edges = edges;
        (void)cbm_store_meta_put(meta, &memo);
    }

    list_projects_emit_entry(doc, arr, project_name, root_path_buf, nodes, edges, size_bytes,
                             include_details);
    return DB_PROJECT_FOUND;
}

static int project_db_name_cmp(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

/* list_projects: scan cache directory for .db files.
 * Each project is a single .db file — no central registry needed. */
static char *handle_list_projects(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;

    /* Pagination is DETERMINISTIC because the names are sorted below. Without
     * that, paging over readdir order is meaningless — the order is
     * filesystem-dependent, so page 2 could repeat or skip entries from page 1,
     * and even the unpaginated list differed across platforms. */
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int limit = cbm_mcp_get_int_arg(args, "limit", 50);
    bool include_details = cbm_mcp_get_bool_arg(args, "include_details");
    if (offset < 0) {
        offset = 0;
    }
    if (limit < 1) {
        limit = 1;
    } else if (limit > 100) {
        limit = 100;
    }

    char dir_path[CBM_SZ_1K];
    cache_dir(dir_path, sizeof(dir_path));

    cbm_dir_t *d = cbm_opendir(dir_path);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    uint64_t deadline_ms = cbm_now_ms() + MCP_FALLBACK_SCAN_TIMEOUT_MS;
    bool incomplete = false;

    /* A cache directory that does not exist yet is the normal state of a fresh
     * install — nothing has been indexed — not a failure. Reporting it as an
     * error made list_projects the one tool that cannot answer "nothing yet",
     * and it hid the difference from a directory that exists but cannot be
     * READ, which is a real permissions problem worth naming. Only the latter
     * is an error now; the former falls through to an empty list. */
    if (!d && cbm_is_dir(dir_path)) {
        char msg[CBM_SZ_1K];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"cannot read cache directory: %s\",\"hint\":"
                 "\"Check directory permissions or run index_repository first.\"}",
                 dir_path);
        yyjson_mut_doc_free(doc);
        return cbm_mcp_text_result(msg, true);
    }

    /* Collect names first, sort, THEN page and open only the slice's databases.
     * Opening every project's db to count nodes/edges is what made this slow on
     * a large install; now it happens for at most `limit` of them, and only
     * when details were asked for. */
    char **db_names = NULL;
    size_t db_count = 0;
    size_t db_cap = 0;
    bool name_collection_failed = false;

    cbm_dirent_t *entry;
    while (d && (entry = cbm_readdir(d)) != NULL) {
        if (cbm_now_ms() >= deadline_ms) {
            incomplete = true;
            break;
        }
        const char *name = entry->name;
        if (!cbm_is_project_index_db(name)) {
            continue;
        }
        char full_path[CBM_SZ_2K];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
        if (!cbm_is_regular_file(full_path)) {
            continue;
        }
        if (cbm_file_size(full_path) < 0) {
            continue;
        }
        if (db_count == db_cap) {
            size_t next_cap = db_cap ? db_cap * 2 : 64;
            char **grown = (char **)realloc(db_names, next_cap * sizeof(*db_names));
            if (!grown) {
                name_collection_failed = true;
                break;
            }
            db_names = grown;
            db_cap = next_cap;
        }
        db_names[db_count] = strdup(name);
        if (!db_names[db_count]) {
            name_collection_failed = true;
            break;
        }
        db_count++;
    }
    if (d) {
        cbm_closedir(d);
    }

    /* Fail ATOMICALLY rather than silently returning a truncated list that the
     * caller cannot distinguish from "these are all the projects". */
    if (name_collection_failed) {
        for (size_t i = 0; i < db_count; i++) {
            free(db_names[i]);
        }
        free(db_names);
        yyjson_mut_doc_free(doc);
        return cbm_mcp_text_result("{\"error\":\"out of memory while collecting indexed "
                                   "projects\"}",
                                   true);
    }

    if (db_count > 1) {
        qsort(db_names, db_count, sizeof(*db_names), project_db_name_cmp);
    }
    size_t start = (size_t)offset < db_count ? (size_t)offset : db_count;
    size_t end = start + (size_t)limit;
    if (end > db_count) {
        end = db_count;
    }
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    for (size_t i = start; i < end; i++) {
        const char *name = db_names[i];
        size_t len = strlen(name);
        char full_path[CBM_SZ_2K];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
        int64_t size_bytes = cbm_file_size(full_path);
        if (size_bytes < 0) {
            continue;
        }
        db_project_result_t entry_result = build_project_json_entry(
            doc, arr, dir_path, name, len, size_bytes, deadline_ms, include_details, meta);
        if (entry_result == DB_PROJECT_BUSY || entry_result == DB_PROJECT_DEADLINE) {
            incomplete = true;
        }
    }
    cbm_store_meta_close(meta);
    for (size_t i = 0; i < db_count; i++) {
        free(db_names[i]);
    }
    free(db_names);

    yyjson_mut_obj_add_val(doc, root, "projects", arr);
    yyjson_mut_obj_add_uint(doc, root, "total", db_count);
    yyjson_mut_obj_add_int(doc, root, "offset", offset);
    yyjson_mut_obj_add_int(doc, root, "limit", limit);
    yyjson_mut_obj_add_uint(doc, root, "returned", yyjson_mut_arr_size(arr));
    yyjson_mut_obj_add_bool(doc, root, "has_more", end < db_count);
    yyjson_mut_obj_add_bool(doc, root, "incomplete", incomplete);
    if (incomplete) {
        yyjson_mut_obj_add_str(doc, root, "incomplete_hint",
                               "Some cache databases were busy or the 5 second scan deadline "
                               "expired; retry list_projects.");
    }

    /* Guide user when no projects are indexed */
    if (yyjson_mut_arr_size(arr) == 0) {
        yyjson_mut_obj_add_str(doc, root, "hint",
                               "No projects indexed. Call index_repository(repo_path=...) first.");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    return mcp_text_result_take_owned(json, false);
}

/* verify_project_indexed — returns a heap-allocated error JSON string when the
 * named project has not been indexed yet, or NULL when the project exists.
 * resolve_store uses cbm_store_open_path_query (no SQLITE_OPEN_CREATE), so
 * store is NULL for missing .db files (REQUIRE_STORE fires first). This
 * function catches the remaining case: a .db file exists but has no indexed
 * nodes (e.g., an empty or half-initialised project).
 * Callers that receive a non-NULL return value must free(project) themselves
 * before returning the error string. */
static char *verify_project_indexed(cbm_store_t *store, const char *project) {
    cbm_project_t proj_check = {0};
    if (cbm_store_get_project(store, project, &proj_check) != CBM_STORE_OK) {
        char *err = build_project_list_error("project not indexed — run index_repository first");
        char *res = cbm_mcp_text_result(err, true);
        free(err);
        return res;
    }
    cbm_project_free_fields(&proj_check);
    return NULL;
}

static char *handle_get_graph_schema(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        return not_indexed;
    }

    cbm_schema_info_t schema = {0};
    cbm_store_get_schema(store, project, &schema);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *labels = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.node_label_count; i++) {
        yyjson_mut_val *lbl = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, lbl, "label", schema.node_labels[i].label);
        yyjson_mut_obj_add_int(doc, lbl, "count", schema.node_labels[i].count);
        yyjson_mut_val *props = yyjson_mut_arr(doc);
        for (int j = 0; j < schema.node_labels[i].property_count; j++) {
            yyjson_mut_arr_add_str(doc, props, schema.node_labels[i].properties[j]);
        }
        yyjson_mut_obj_add_val(doc, lbl, "properties", props);
        yyjson_mut_arr_add_val(labels, lbl);
    }
    yyjson_mut_obj_add_val(doc, root, "node_labels", labels);

    yyjson_mut_val *types = yyjson_mut_arr(doc);
    for (int i = 0; i < schema.edge_type_count; i++) {
        yyjson_mut_val *typ = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, typ, "type", schema.edge_types[i].type);
        yyjson_mut_obj_add_int(doc, typ, "count", schema.edge_types[i].count);
        yyjson_mut_val *eprops = yyjson_mut_arr(doc);
        for (int j = 0; j < schema.edge_types[i].property_count; j++) {
            yyjson_mut_arr_add_str(doc, eprops, schema.edge_types[i].properties[j]);
        }
        yyjson_mut_obj_add_val(doc, typ, "properties", eprops);
        yyjson_mut_arr_add_val(types, typ);
    }
    yyjson_mut_obj_add_val(doc, root, "edge_types", types);

    /* Check ADR presence */
    cbm_project_t proj_info = {0};
    if (cbm_store_get_project(store, project, &proj_info) == 0 && proj_info.root_path) {
        bool adr_exists = project_has_adr(store, project, proj_info.root_path);
        yyjson_mut_obj_add_bool(doc, root, "adr_present", adr_exists);
        if (!adr_exists) {
            yyjson_mut_obj_add_str(
                doc, root, "adr_hint",
                "No ADR found. Use manage_adr(mode='update') to persist architectural "
                "decisions across sessions. Run get_architecture(aspects=['all']) first.");
        }
        cbm_project_free_fields(&proj_info);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_schema_free(&schema);
    free(project);

    return mcp_text_result_take_owned(json, false);
}

/* Validate edge type: uppercase letters + underscore only, max 64 chars. */
static bool validate_edge_type(const char *s) {
    if (!s || strlen(s) > CBM_SZ_64) {
        return false;
    }
    for (const char *c = s; *c; c++) {
        if (!(*c >= 'A' && *c <= 'Z') && *c != '_') {
            return false;
        }
    }
    return true;
}

/* Enrich search result with 1-hop connected node names. */
/* Add BFS results to a yyjson array (deduped by name). */
static void enrich_add_bfs(yyjson_mut_doc *doc, yyjson_mut_val *arr, cbm_traverse_result_t *tr) {
    for (int j = 0; j < tr->visited_count; j++) {
        if (tr->visited[j].node.name) {
            yyjson_mut_arr_add_strcpy(doc, arr, tr->visited[j].node.name);
        }
    }
}

/* Enrich search result with 1-hop connected node names (inbound + outbound). */
static void enrich_connected(yyjson_mut_doc *doc, yyjson_mut_val *item, cbm_store_t *store,
                             int64_t node_id, const char *relationship) {
    const char *et[] = {relationship ? relationship : "CALLS"};
    yyjson_mut_val *conn = yyjson_mut_arr(doc);

    /* BFS doesn't support "both" — run inbound + outbound separately. */
    cbm_traverse_result_t tr_in = {0};
    cbm_store_bfs(store, node_id, "inbound", et, SKIP_ONE, SKIP_ONE, MCP_DEFAULT_LIMIT, &tr_in);
    enrich_add_bfs(doc, conn, &tr_in);
    cbm_store_traverse_free(&tr_in);

    cbm_traverse_result_t tr_out = {0};
    cbm_store_bfs(store, node_id, "outbound", et, SKIP_ONE, SKIP_ONE, MCP_DEFAULT_LIMIT, &tr_out);
    enrich_add_bfs(doc, conn, &tr_out);
    cbm_store_traverse_free(&tr_out);

    if (yyjson_mut_arr_size(conn) > 0) {
        yyjson_mut_obj_add_val(doc, item, "connected_names", conn);
    }
}

/* Build an FTS5 MATCH expression from a free-form query string by splitting
 * on whitespace and joining the terms with OR.  Each token is also sanitized:
 * anything that isn't alnum or underscore is dropped, so the caller can't
 * inject FTS5 operators or double-quoted phrases.  Returns the number of
 * tokens emitted (0 if the query contained no usable terms). */
enum {
    BM25_MIN_BUF = 2, /* minimum buffer size: at least NUL + one char */
    BM25_SEP_RESERVE = 1,
    BM25_QUERY_BUF = 1024,
    BM25_DEFAULT_LIMIT = 100,
    BM25_COL_ID = 0,
    BM25_COL_LABEL = 1,
    BM25_COL_NAME = 2,
    BM25_COL_QN = 3,
    BM25_COL_FILE = 4,
    BM25_COL_START = 5,
    BM25_COL_END = 6,
    BM25_COL_RANK = 7,
    BM25_BIND_QUERY = 1,
    BM25_BIND_PROJECT = 2,
    BM25_BIND_LIMIT = 3,
    BM25_BIND_OFFSET = 4,
    BM25_BIND_INNER = 5,
    BM25_BIND_FILE = 6,
    BM25_BIND_LABEL = 7,
    BM25_BIND_EXACT = 8,
    BM25_SQL_AUTO_LEN = -1,
    /* Inner FTS5 candidate cap.  SQLite can early-terminate a plain FTS5 query
     * (no JOIN/WHERE on outer table) of the form:
     *   SELECT rowid, bm25() FROM nodes_fts WHERE MATCH ? ORDER BY bm25() LIMIT N
     * By fetching only the top BM25_INNER_LIMIT candidates from the FTS5 index
     * and then joining/filtering/re-ranking those, we bound all work to O(N) where
     * N = BM25_INNER_LIMIT rather than the full match set size. */
    BM25_INNER_LIMIT = 2000,
};

/* Module-local SQLITE_TRANSIENT wrapper to dodge performance-no-int-to-ptr.
 * See the matching helper in src/store/store.c for the same pattern. */
static sqlite3_destructor_type mcp_sqlite_transient(void) {
    static const volatile intptr_t raw = -1;
    sqlite3_destructor_type dtor = NULL;
    memcpy(&dtor, (const void *)&raw, sizeof(dtor));
    return dtor;
}
#define MCP_SQLITE_TRANSIENT (mcp_sqlite_transient())

static int bm25_build_match(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size < BM25_MIN_BUF) {
        return 0;
    }
    size_t pos = 0;
    int tokens = 0;
    const char *p = query;
    while (*p) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *tok_start = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 0) {
            continue;
        }
        const char *sep = (tokens > 0) ? " OR " : "";
        size_t sep_len = strlen(sep);
        if (pos + sep_len + tok_len + BM25_SEP_RESERVE >= out_size) {
            break; /* out of room — stop cleanly, keep what we have */
        }
        memcpy(out + pos, sep, sep_len);
        pos += sep_len;
        memcpy(out + pos, tok_start, tok_len);
        pos += tok_len;
        tokens++;
    }
    out[pos] = '\0';
    return tokens;
}

/* A BM25 result is empty although nodes match when the label is spelled in a
 * case no node carries: labels are case-sensitive, so `label=class` finds
 * nothing while Class nodes match the query (upstream #2386). Writes a hint
 * naming the label this project actually holds into `out` and returns true,
 * or returns false when the label exists exactly (a true zero). The exact
 * probe is an index seek on (project, label); the case-insensitive scan runs
 * only when it misses. The caller's label is never echoed back, only the
 * stored spelling. */
static bool bm25_label_zero_hint(sqlite3 *db, const char *project, const char *label, char *out,
                                 size_t out_size) {
    enum { HINT_BIND_PROJECT = 1, HINT_BIND_LABEL = 2, HINT_COL_LABEL = 0, HINT_PROBE_EXACT = 0 };
    if (!db || !label || !label[0] || !out || out_size == 0) {
        return false;
    }
    static const char *const probes[] = {
        "SELECT label FROM nodes WHERE project = ?1 AND label = ?2 LIMIT 1",
        "SELECT label FROM nodes WHERE project = ?1 AND label = ?2 COLLATE NOCASE LIMIT 1",
    };
    char found[CBM_SZ_256] = "";
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]) && !found[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, probes[i], BM25_SQL_AUTO_LEN, &st, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(st, HINT_BIND_PROJECT, project, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
        sqlite3_bind_text(st, HINT_BIND_LABEL, label, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *stored = (const char *)sqlite3_column_text(st, HINT_COL_LABEL);
            snprintf(found, sizeof(found), "%s", stored ? stored : "");
        }
        sqlite3_finalize(st);
        if (found[0] && i == HINT_PROBE_EXACT) {
            return false;
        }
    }
    if (!found[0]) {
        snprintf(out, out_size,
                 "no node in this project carries this label; get_graph_schema lists the labels "
                 "this index holds");
        return true;
    }
    snprintf(out, out_size,
             "no node carries this label as spelled; labels are case-sensitive -- retry with "
             "label=\"%s\"",
             found);
    return true;
}

static char *bm25_file_pattern_like(const char *file_pattern) {
    if (!file_pattern) {
        return NULL;
    }
    char *like = cbm_glob_to_like(file_pattern);
    if (like && !strchr(file_pattern, '*') && !strchr(file_pattern, '?')) {
        size_t len = strlen(like);
        char *contains = (char *)malloc(len + MCP_SEPARATOR + SKIP_ONE);
        if (contains) {
            contains[0] = '%';
            memcpy(contains + SKIP_ONE, like, len);
            contains[len + SKIP_ONE] = '%';
            contains[len + MCP_SEPARATOR] = '\0';
            free(like);
            like = contains;
        }
    }
    return like;
}

/* Run the BM25 full-text search path and return the JSON result string.
 * Returns NULL if FTS5 is unavailable or the query produced no usable tokens,
 * in which case the caller falls back to the regex-based search path. */
/* Column weights for nodes_fts (name, qualified_name, label, file_path, body).
 * The four identifier columns stay at parity; prose sits well below them, so a
 * prose-only hit surfaces but never outranks a node whose IDENTIFIER matches.
 * FTS5 applies them to per-column term frequency before tf saturation
 * (BM25F-correct, not a post-hoc rescale).
 *
 * Used by BOTH the ranked query and the count query — they share an inner
 * candidate window, so different weights would desynchronise total from rows.
 *
 * Safe on a legacy four-column nodes_fts: bm25() reads a weight only for a
 * column an instance actually landed in, so the fifth is never consulted. */
#define BM25_WEIGHTS "bm25(nodes_fts, 1.0, 1.0, 1.0, 1.0, 0.3)"

/* Labels an UNLABELLED query never ranks: path-shaped File/Folder nodes, the
 * Project node and Variables match almost any token through their qualified
 * name and file path, and would crowd definitions out of the result. They are
 * still in nodes_fts, so an explicit `label` that names one of them is served
 * from its own label-scoped window like any other label (?7 is the caller's
 * label, NULL when none was given). Excluding them there too returned 0 rows
 * although such nodes match (upstream #2386). Section and Module are NOT
 * excluded (#518/#519): they carry the prose the body column indexes. Shared
 * by every candidate set and by the ranked and the count query, so they
 * cannot desynchronise. */
#define BM25_UNLABELLED_EXCLUSION(alias) \
    "(?7 IS NOT NULL OR " alias ".label NOT IN ('File','Folder','Variable','Project'))"

/* Candidate rows for BM25 search, shared by the ranked and the count query.
 * fts_top is the top BM25_INNER_LIMIT (?5) FTS hits. A filter applied after
 * that window only sees what survived it, so with thousands of partial
 * matches the requested label (?7) or path (?6), or the definition named like
 * the query (?8), could be missing entirely (`query=get label=Class` on
 * opencv: 16 classes in the window, 51 in the index). Three more sets close
 * that; each applies the outer query's project (?2), path and label filters
 * and excludes rows already collected:
 *   exact_extra: rows named exactly the query — an idx_nodes_name lookup,
 *     scored as good as the best FTS hit so the exact-name tier decides;
 *   nocase_extra: rows whose name equals the query case-insensitively (the
 *     ranking's second tier), found by an FTS scan of the name column only,
 *     without bm25(); only needed, and only run, when fts_top is full;
 *   filtered_extra: with a label or path filter, the best FTS hits that pass
 *     it. The FTS scan drives (CROSS JOIN) and bm25() is computed only for
 *     rows that pass the filters, so it costs no more than fts_top's own full
 *     scoring. Skipped without a filter, and when fts_top already holds a
 *     full window of rows that pass it (the extra rows would rank below).
 * On a synthetic 1M-match index the ranked query takes 1.8-2.1 s with or
 * without filters (the unfiltered fts_top scoring dominates); opencv ~80 ms. */
#define BM25_CANDIDATE_CTES                                                                        \
    "WITH fts_top AS MATERIALIZED ("                                                               \
    "    SELECT rowid, " BM25_WEIGHTS " AS base_rank"                                              \
    "    FROM nodes_fts WHERE nodes_fts MATCH ?1"                                                  \
    "    ORDER BY base_rank LIMIT ?5), "                                                           \
    "exact_extra AS MATERIALIZED ("                                                                \
    "    SELECT x.id AS rowid,"                                                                    \
    "           COALESCE((SELECT min(base_rank) FROM fts_top), 0.0) AS base_rank"                  \
    "    FROM nodes x WHERE x.project = ?2 AND x.name = ?8"                                        \
    "      AND " BM25_UNLABELLED_EXCLUSION(                                                        \
        "x") "      AND (?6 IS NULL OR x.file_path LIKE ?6)"                                       \
             "      AND (?7 IS NULL OR x.label = ?7)"                                              \
             "      AND x.id NOT IN (SELECT rowid FROM fts_top)"                                   \
             "    LIMIT ?5), "                                                                     \
             "nocase_extra AS MATERIALIZED ("                                                      \
             "    SELECT f.rowid AS rowid,"                                                        \
             "           COALESCE((SELECT min(base_rank) FROM fts_top), 0.0) AS base_rank"         \
             "    FROM nodes_fts f CROSS JOIN nodes w ON w.id = f.rowid"                           \
             "    WHERE (SELECT count(*) FROM fts_top) >= ?5"                                      \
             "      AND nodes_fts MATCH ('name : (' || ?1 || ')')"                                 \
             "      AND w.project = ?2 AND w.name != ?8 AND lower(w.name) = lower(?8)"             \
             "      AND " BM25_UNLABELLED_EXCLUSION(                                               \
                 "w") "      AND (?6 IS NULL OR w.file_path LIKE ?6)"                              \
                      "      AND (?7 IS NULL OR w.label = ?7)"                                     \
                      "      AND f.rowid NOT IN (SELECT rowid FROM fts_top)"                       \
                      "    LIMIT ?5), "                                                            \
                      "filtered_extra AS ("                                                        \
                      "    SELECT f.rowid AS rowid, " BM25_WEIGHTS " AS base_rank"                 \
                      "    FROM nodes_fts f CROSS JOIN nodes y ON y.id = f.rowid"                  \
                      "    WHERE (?7 IS NOT NULL OR ?6 IS NOT NULL)"                               \
                      "      AND (SELECT count(*) FROM fts_top t JOIN nodes z ON z.id = t.rowid"   \
                      "           WHERE z.project = ?2"                                            \
                      "             AND " BM25_UNLABELLED_EXCLUSION(                               \
                          "z") "             AND (?6 IS NULL OR z.file_path LIKE ?6)"              \
                               "             AND (?7 IS NULL OR z.label = ?7)) < ?5"               \
                               "      AND nodes_fts MATCH ?1"                                      \
                               "      AND y.project = ?2"                                          \
                               "      AND " BM25_UNLABELLED_EXCLUSION(                             \
                                   "y") "      AND (?6 IS NULL OR y.file_path LIKE ?6)"            \
                                        "      AND (?7 IS NULL OR y.label = ?7)"                   \
                                        "      AND f.rowid NOT IN (SELECT rowid FROM fts_top)"     \
                                        "      AND f.rowid NOT IN (SELECT rowid FROM exact_extra)" \
                                        "      AND f.rowid NOT IN (SELECT rowid FROM "             \
                                        "nocase_extra)"                                            \
                                        "    ORDER BY base_rank LIMIT ?5) "

static char *bm25_search(cbm_store_t *store, const char *project, const char *query,
                         const char *file_pattern, const char *label, int limit, int offset) {
    sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return NULL;
    }
    char fts_query[BM25_QUERY_BUF];
    int tok_count = bm25_build_match(query, fts_query, sizeof(fts_query));
    if (tok_count == 0) {
        return NULL;
    }
    char *file_like = bm25_file_pattern_like(file_pattern);

    /* BM25 ranked query using a two-step approach to enable FTS5 early termination.
     *
     * Flat queries of the form:
     *   SELECT ... FROM nodes_fts JOIN nodes WHERE MATCH ? AND n.project=? ORDER BY rank LIMIT N
     * block FTS5's WAND/MaxScore early-exit because the outer JOIN+WHERE conditions
     * are invisible to the FTS5 planner — it must score every matching document before
     * the project/label filter can discard any of them.  On a large codebase with 100K+
     * matches, this causes multi-minute queries.
     *
     * The fix: let FTS5 drive the inner subquery alone.  SQLite CAN early-terminate
     *   SELECT rowid, bm25(nodes_fts) FROM nodes_fts WHERE MATCH ? ORDER BY bm25() LIMIT N
     * because no outer predicate blocks it.  We fetch BM25_INNER_LIMIT top candidates
     * from the FTS5 index, then join/filter/boost only those rows.  bm25() returns a
     * NEGATIVE score (lower = more relevant). */
    /* Exact-name tier: a definition whose NAME is the query outranks every
     * partial hit. BM25 term frequency otherwise rewards a long test-method
     * name that repeats the token — `table references table with same name` —
     * over the `Table` class itself, and the label tiers below then push the
     * class's own methods above it (Method 10 > Class 5). The definition the
     * reader asked for by name comes first; case-insensitive exact spelling
     * comes next; everything else keeps its BM25 order. */
    const char *sql = BM25_CANDIDATE_CTES
        "SELECT n.id, n.label, n.name, n.qualified_name, n.file_path, n.start_line, n.end_line, "
        "       (fts.base_rank "
        "        - CASE WHEN n.name = ?8 THEN 30.0 "
        "               WHEN lower(n.name) = lower(?8) THEN 20.0 ELSE 0.0 END "
        "        - CASE WHEN n.label IN ('Function','Method') THEN 10.0 "
        "               WHEN n.label = 'Route' THEN 8.0 "
        "               WHEN n.label IN (" CBM_SQL_TYPE_LIKE_LABELS ") THEN 5.0 "
        /* Relations rank with the type tier: a table IS the schema container a
         * data question is looking for (findability-first). */
        "               WHEN n.label IN (" CBM_SQL_RELATION_LABELS ") THEN 5.0 "
        "               ELSE 0.0 END) AS rank "
        "FROM ("
        "    SELECT rowid, base_rank FROM fts_top"
        "    UNION ALL "
        "    SELECT rowid, base_rank FROM exact_extra"
        "    UNION ALL "
        "    SELECT rowid, base_rank FROM nocase_extra"
        "    UNION ALL "
        "    SELECT rowid, base_rank FROM filtered_extra"
        ") fts "
        "JOIN nodes n ON n.id = fts.rowid "
        "WHERE n.project = ?2 "
        /* Section and Module are NO LONGER excluded (#518/#519): they carry the
         * prose — a Markdown section's body, a config file's description.
         * MIRRORED in the count query below; change the two together. */
        "  AND " BM25_UNLABELLED_EXCLUSION(
            "n") " "
                 "  AND (?6 IS NULL OR n.file_path LIKE ?6) "
                 /* The caller's label filter applies in query mode exactly as it does
                  * in the structural mode (`label=Class` used to be ignored here and
                  * Methods came back). MIRRORED in the count query. */
                 "  AND (?7 IS NULL OR n.label = ?7) "
                 "ORDER BY rank "
                 "LIMIT ?3 OFFSET ?4";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, BM25_SQL_AUTO_LEN, &stmt, NULL) != SQLITE_OK) {
        free(file_like);
        return NULL;
    }
    sqlite3_bind_text(stmt, BM25_BIND_QUERY, fts_query, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, BM25_BIND_PROJECT, project, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, BM25_BIND_LIMIT, limit > 0 ? limit : BM25_DEFAULT_LIMIT);
    sqlite3_bind_int(stmt, BM25_BIND_OFFSET, offset > 0 ? offset : 0);
    sqlite3_bind_int(stmt, BM25_BIND_INNER, BM25_INNER_LIMIT);
    if (file_like) {
        sqlite3_bind_text(stmt, BM25_BIND_FILE, file_like, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, BM25_BIND_FILE);
    }
    if (label && label[0]) {
        sqlite3_bind_text(stmt, BM25_BIND_LABEL, label, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, BM25_BIND_LABEL);
    }
    sqlite3_bind_text(stmt, BM25_BIND_EXACT, query, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);

    /* Count hits within the same inner-limit window — capped at BM25_INNER_LIMIT.
     * Uses the identical subquery structure so the FTS5 early-exit applies here too. */
    int total = 0;
    {
        const char *count_sql = BM25_CANDIDATE_CTES
            "SELECT COUNT(*) FROM ("
            "    SELECT fts.rowid FROM ("
            "        SELECT rowid FROM fts_top UNION ALL SELECT rowid FROM exact_extra"
            "        UNION ALL SELECT rowid FROM nocase_extra"
            "        UNION ALL SELECT rowid FROM filtered_extra"
            "    ) fts "
            "    JOIN nodes n ON n.id = fts.rowid "
            "    WHERE n.project = ?2 "
            /* MIRRORS the ranked query verbatim: same weights, same exclusions. */
            "      AND " BM25_UNLABELLED_EXCLUSION(
                "n") " "
                     "      AND (?6 IS NULL OR n.file_path LIKE ?6)"
                     "      AND (?7 IS NULL OR n.label = ?7)"
                     ")";
        sqlite3_stmt *cs = NULL;
        if (sqlite3_prepare_v2(db, count_sql, BM25_SQL_AUTO_LEN, &cs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(cs, BM25_BIND_QUERY, fts_query, BM25_SQL_AUTO_LEN,
                              MCP_SQLITE_TRANSIENT);
            sqlite3_bind_text(cs, BM25_BIND_PROJECT, project, BM25_SQL_AUTO_LEN,
                              MCP_SQLITE_TRANSIENT);
            sqlite3_bind_int(cs, BM25_BIND_INNER, BM25_INNER_LIMIT);
            if (file_like) {
                sqlite3_bind_text(cs, BM25_BIND_FILE, file_like, BM25_SQL_AUTO_LEN,
                                  MCP_SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(cs, BM25_BIND_FILE);
            }
            if (label && label[0]) {
                sqlite3_bind_text(cs, BM25_BIND_LABEL, label, BM25_SQL_AUTO_LEN,
                                  MCP_SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(cs, BM25_BIND_LABEL);
            }
            sqlite3_bind_text(cs, BM25_BIND_EXACT, query, BM25_SQL_AUTO_LEN, MCP_SQLITE_TRANSIENT);
            if (sqlite3_step(cs) == SQLITE_ROW) {
                total = sqlite3_column_int(cs, 0);
            }
            sqlite3_finalize(cs);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "total", total);
    yyjson_mut_obj_add_str(doc, root, "search_mode", "bm25");

    yyjson_mut_val *results = yyjson_mut_arr(doc);
    int emitted = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "name",
                                  (const char *)sqlite3_column_text(stmt, BM25_COL_NAME));
        yyjson_mut_obj_add_strcpy(doc, item, "qualified_name",
                                  (const char *)sqlite3_column_text(stmt, BM25_COL_QN));
        yyjson_mut_obj_add_strcpy(doc, item, "label",
                                  (const char *)sqlite3_column_text(stmt, BM25_COL_LABEL));
        yyjson_mut_obj_add_strcpy(doc, item, "file_path",
                                  (const char *)sqlite3_column_text(stmt, BM25_COL_FILE));
        yyjson_mut_obj_add_int(doc, item, "start_line", sqlite3_column_int(stmt, BM25_COL_START));
        yyjson_mut_obj_add_int(doc, item, "end_line", sqlite3_column_int(stmt, BM25_COL_END));
        yyjson_mut_obj_add_real(doc, item, "rank", sqlite3_column_double(stmt, BM25_COL_RANK));
        yyjson_mut_arr_add_val(results, item);
        emitted++;
    }
    sqlite3_finalize(stmt);
    free(file_like);

    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", total > offset + emitted);
    /* Never a silent zero for a label the index spells differently. */
    char zero_hint[CBM_SZ_512];
    if (total == 0 && bm25_label_zero_hint(db, project, label, zero_hint, sizeof(zero_hint))) {
        yyjson_mut_obj_add_strcpy(doc, root, "hint", zero_hint);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return json;
}

/* Forward declaration — defined later. enrich_node_properties parses the
 * node's properties_json and grafts the parsed values onto the result item.
 * It returns the parsed yyjson_doc which must outlive the serialization
 * because yyjson_mut_obj_add_val uses zero-copy strings into that doc. */
static yyjson_doc *enrich_node_properties(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                          const char *properties_json);
static yyjson_doc *enrich_node_properties_ex(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                             const char *properties_json, bool skip_metrics,
                                             size_t docstring_cap);

/* A search listing carries at most this much of each docstring. The graph
 * stores the whole doc comment (upstream dab9187c dropped the 500-byte
 * extraction cut), but fifty rows of full comments doubled a min_degree
 * listing on redis (66 KB -> 122 KB); the single-symbol tools still return
 * the whole text. */
enum { MCP_SEARCH_DOCSTRING_CAP = 500 };

/* Emit the cbm_store_search results as a JSON "results" array on the doc.
 * Property docs created via enrich_node_properties are collected in
 * *out_pdocs (count in *out_pdoc_count) and must be freed by the caller
 * AFTER serializing doc, since yyjson_mut strings are zero-copy pointers
 * into those parsed docs. The caller also frees out_pdocs itself. */
static void emit_search_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                const cbm_search_output_t *out, cbm_store_t *store,
                                const char *relationship, bool include_connected, int offset,
                                bool include_metrics, yyjson_doc ***out_pdocs,
                                int *out_pdoc_count) {
    yyjson_doc **pdocs =
        out->count > 0 ? (yyjson_doc **)malloc((size_t)out->count * sizeof(yyjson_doc *)) : NULL;
    int pdoc_count = 0;
    yyjson_mut_obj_add_int(doc, root, "total", out->total);
    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (int i = 0; i < out->count; i++) {
        cbm_search_result_t *sr = &out->results[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "name", sr->node.name ? sr->node.name : "");
        yyjson_mut_obj_add_str(doc, item, "qualified_name",
                               sr->node.qualified_name ? sr->node.qualified_name : "");
        yyjson_mut_obj_add_str(doc, item, "label", sr->node.label ? sr->node.label : "");
        yyjson_mut_obj_add_str(doc, item, "file_path",
                               sr->node.file_path ? sr->node.file_path : "");
        if (sr->node.start_line > 0) {
            yyjson_mut_obj_add_int(doc, item, "start_line", sr->node.start_line);
            yyjson_mut_obj_add_int(doc, item, "end_line", sr->node.end_line);
        }
        yyjson_mut_obj_add_int(doc, item, "in_degree", sr->in_degree);
        yyjson_mut_obj_add_int(doc, item, "out_degree", sr->out_degree);
        if (include_connected && sr->node.id > 0) {
            enrich_connected(doc, item, store, sr->node.id, relationship);
        }
        yyjson_doc *pdoc = enrich_node_properties_ex(doc, item, sr->node.properties_json,
                                                     !include_metrics, MCP_SEARCH_DOCSTRING_CAP);
        if (pdoc && pdocs) {
            pdocs[pdoc_count++] = pdoc;
        }
        yyjson_mut_arr_add_val(results, item);
    }
    yyjson_mut_obj_add_val(doc, root, "results", results);
    yyjson_mut_obj_add_bool(doc, root, "has_more", out->total > offset + out->count);
    *out_pdocs = pdocs;
    *out_pdoc_count = pdoc_count;
}

/* Extract keyword strings from a yyjson array into `keywords`.  Returns the
 * number of strings copied (capped at `max_out`). */
static int extract_semantic_keywords(yyjson_val *sq_val, const char **keywords, int max_out) {
    int kw_count = (int)yyjson_arr_size(sq_val);
    if (kw_count > max_out) {
        kw_count = max_out;
    }
    size_t kw_idx = 0;
    size_t kw_max = 0;
    yyjson_val *kw_val;
    int ki = 0;
    yyjson_arr_foreach(sq_val, kw_idx, kw_max, kw_val) {
        if (ki < kw_count && yyjson_is_str(kw_val)) {
            keywords[ki++] = yyjson_get_str(kw_val);
        }
    }
    return ki;
}

/* Emit cbm_vector_result_t entries as a "semantic_results" array on the doc. */
static void emit_semantic_results(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                  cbm_vector_result_t *vresults, int vcount) {
    yyjson_mut_val *sem_results = yyjson_mut_arr(doc);
    for (int v = 0; v < vcount; v++) {
        yyjson_mut_val *vitem = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, vitem, "name", vresults[v].name);
        yyjson_mut_obj_add_strcpy(doc, vitem, "qualified_name", vresults[v].qualified_name);
        yyjson_mut_obj_add_strcpy(doc, vitem, "label", vresults[v].label);
        yyjson_mut_obj_add_strcpy(doc, vitem, "file_path", vresults[v].file_path);
        yyjson_mut_obj_add_real(doc, vitem, "score", vresults[v].score);
        yyjson_mut_arr_add_val(sem_results, vitem);
    }
    yyjson_mut_obj_add_val(doc, root, "semantic_results", sem_results);
}

/* Append the semantic_query vector-search results onto the doc.  Returns
 * true if semantic_query was provided as a non-array (type error — caller
 * should surface to the user). */
/* Run the semantic query and HAND BACK its results rather than emitting them, so
 * the caller can decide whether the structural search should run at all. Sets
 * *out_present when semantic_query was supplied (even if it matched nothing) —
 * that is what distinguishes "semantic-only request" from "no semantic query".
 * Returns true on a TYPE error (semantic_query present but not an array), which
 * the caller surfaces to the user.
 *
 * Ownership: on success *out_results must be released with
 * cbm_store_free_vector_results. */
static bool run_semantic_query_core(const char *args, cbm_store_t *store, const char *project,
                                    int limit, cbm_vector_result_t **out_results, int *out_count,
                                    bool *out_present) {
    enum { MAX_KW_SEARCH = 32 };
    *out_results = NULL;
    *out_count = 0;
    if (out_present) {
        *out_present = false;
    }
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    yyjson_val *sq_val = args_root ? yyjson_obj_get(args_root, "semantic_query") : NULL;
    bool type_error = false;
    if (sq_val && !yyjson_is_arr(sq_val)) {
        type_error = true;
    } else if (sq_val && yyjson_arr_size(sq_val) > 0) {
        if (out_present) {
            *out_present = true;
        }
        const char *keywords[MAX_KW_SEARCH];
        int ki = extract_semantic_keywords(sq_val, keywords, MAX_KW_SEARCH);
        cbm_vector_result_t *vresults = NULL;
        int vcount = 0;
        int sem_limit = limit > 0 ? limit : CBM_SZ_16;
        if (cbm_store_vector_search(store, project, keywords, ki, sem_limit, &vresults, &vcount) ==
                CBM_STORE_OK &&
            vcount > 0) {
            *out_results = vresults;
            *out_count = vcount;
        }
    }
    if (args_doc) {
        yyjson_doc_free(args_doc);
    }
    return type_error;
}

static char *handle_search_graph(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        return not_indexed;
    }

    /* BM25 path: if `query` is set, run FTS5 full-text search with ranking
     * and return early.  The regex/vector path below is untouched for all
     * other callers.  If FTS5 is unavailable or the query is empty after
     * tokenization, fall through to the regex path. */
    char *query = cbm_mcp_get_string_arg(args, "query");
    if (query && query[0]) {
        int q_limit = cbm_mcp_get_int_arg(args, "limit", BM25_DEFAULT_LIMIT);
        int q_offset = cbm_mcp_get_int_arg(args, "offset", 0);
        char *q_file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
        char *q_label = cbm_mcp_get_string_arg(args, "label");
        char *bm25_json =
            bm25_search(store, project, query, q_file_pattern, q_label, q_limit, q_offset);
        free(q_file_pattern);
        free(q_label);
        if (bm25_json) {
            free(query);
            free(project);
            char *result = cbm_mcp_text_result(bm25_json, false);
            free(bm25_json);
            return result;
        }
    }
    free(query);

    char *label = cbm_mcp_get_string_arg(args, "label");
    char *name_pattern = cbm_mcp_get_string_arg(args, "name_pattern");
    char *qn_pattern = cbm_mcp_get_string_arg(args, "qn_pattern");
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    char *relationship = cbm_mcp_get_string_arg(args, "relationship");
    bool exclude_entry_points = cbm_mcp_get_bool_arg(args, "exclude_entry_points");
    bool include_connected = cbm_mcp_get_bool_arg(args, "include_connected");
    bool include_metrics = cbm_mcp_get_bool_arg(args, "include_metrics");
    int limit = cbm_mcp_get_int_arg(args, "limit", CBM_DEFAULT_SEARCH_LIMIT);
    int offset = cbm_mcp_get_int_arg(args, "offset", 0);
    int min_degree = cbm_mcp_get_int_arg(args, "min_degree", CBM_NOT_FOUND);
    int max_degree = cbm_mcp_get_int_arg(args, "max_degree", CBM_NOT_FOUND);

    if (relationship && !validate_edge_type(relationship)) {
        free(project);
        free(label);
        free(name_pattern);
        free(qn_pattern);
        free(file_pattern);
        free(relationship);
        return cbm_mcp_text_result("relationship must be uppercase letters and underscores", true);
    }

    /* A semantic-only request must NOT also return the unfiltered graph. With no
     * structural filter, cbm_store_search matches everything, so appending
     * semantic hits to it produced a response byte-identical to "no filters at
     * all" — measured: 31 of 31 nodes returned for a two-keyword semantic
     * query. The semantic query then contributed nothing but noise. */
    bool has_structural_filters = label || name_pattern || qn_pattern || file_pattern ||
                                  relationship || exclude_entry_points ||
                                  min_degree != CBM_NOT_FOUND || max_degree != CBM_NOT_FOUND;

    cbm_vector_result_t *vresults = NULL;
    int vcount = 0;
    bool sq_present = false;
    bool sq_type_error =
        run_semantic_query_core(args, store, project, limit, &vresults, &vcount, &sq_present);
    if (sq_type_error) {
        free(project);
        free(label);
        free(name_pattern);
        free(qn_pattern);
        free(file_pattern);
        free(relationship);
        return cbm_mcp_text_result(
            "semantic_query must be an array of keyword strings, e.g. "
            "[\"send\",\"pubsub\",\"publish\"] — not a single string. Split your query "
            "into individual keywords; each is scored independently via per-keyword "
            "min-cosine.",
            true);
    }
    bool semantic_only = sq_present && !has_structural_filters;

    cbm_search_params_t params = {
        .project = project,
        .label = label,
        .name_pattern = name_pattern,
        .qn_pattern = qn_pattern,
        .file_pattern = file_pattern,
        .relationship = relationship,
        .min_degree = min_degree,
        .max_degree = max_degree,
        .limit = limit,
        .offset = offset,
        .exclude_entry_points = exclude_entry_points,
        .include_connected = include_connected,
    };

    cbm_search_output_t out = {0};
    char search_error[CBM_SZ_512] = "";
    if (!semantic_only && cbm_store_search(store, &params, &out) != CBM_STORE_OK) {
        /* A refused or invalid regex aborts the row scan inside SQLite; the
         * store's reason is the caller's only hint about the pattern. */
        snprintf(search_error, sizeof(search_error), "search_graph: %s", cbm_store_error(store));
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_doc **props_docs = NULL;
    int props_doc_count = 0;
    emit_search_results(doc, root, &out, store, relationship, include_connected, offset,
                        include_metrics, &props_docs, &props_doc_count);

    /* Add diagnostic hint when zero results */
    if (out.total == 0) {
        if (name_pattern && label) {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "No results. Try removing the label filter or broadening the name_pattern regex.");
        } else if (name_pattern) {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "No nodes match this pattern. Check spelling or try a broader regex.");
        } else if (label) {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "No nodes with this label. Available labels: Function, Method, Class, "
                "Interface, Route, Variable, Module, Package, File, Folder.");
        }
    }

    /* Semantic hits were fetched before the structural search (so the
     * semantic-only case could skip it) and are emitted here, in the same place
     * and order as before. */
    if (vcount > 0) {
        emit_semantic_results(doc, root, vresults, vcount);
        cbm_store_free_vector_results(vresults, vcount);
        vresults = NULL;
        vcount = 0;
    }

    char *json = yy_doc_to_str(doc);
    /* Property docs are zero-copy referenced by the mut doc — they must
     * outlive yy_doc_to_str. Free them once serialization is complete. */
    for (int pi = 0; pi < props_doc_count; pi++) {
        yyjson_doc_free(props_docs[pi]);
    }
    free(props_docs);
    yyjson_mut_doc_free(doc);
    cbm_store_search_free(&out);

    free(project);
    free(label);
    free(name_pattern);
    free(qn_pattern);
    free(file_pattern);
    free(relationship);

    if (search_error[0]) {
        free(json);
        return cbm_mcp_text_result(search_error, true);
    }
    return mcp_text_result_take_owned(json, false);
}

static char *handle_query_graph(cbm_mcp_server_t *srv, const char *args) {
    char *query = cbm_mcp_get_string_arg(args, "query");
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    int max_rows = cbm_mcp_get_int_arg(args, "max_rows", 0);

    /* graph="missed" (#963): run the SAME cypher against the derived
     * miss-graph view (shadow project "<project>::missed") instead of the
     * code graph — file structure of not-fully-indexed files only. */
    char *graph_arg = cbm_mcp_get_string_arg(args, "graph");
    bool missed_graph = graph_arg && strcmp(graph_arg, "missed") == 0;
    free(graph_arg);

    if (!query) {
        free(project);
        return cbm_mcp_text_result("query is required", true);
    }
    if (missed_graph && !project) {
        free(query);
        return cbm_mcp_text_result("project is required when graph=\"missed\"", true);
    }
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(query);
        return _res;
    }

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        free(query);
        return not_indexed;
    }

    char covproj[CBM_SZ_512];
    const char *cypher_project = project;
    if (missed_graph) {
        cbm_store_coverage_shadow_project(covproj, sizeof(covproj), project);
        cypher_project = covproj;
    }

    cbm_cypher_result_t result = {0};
    int rc = cbm_cypher_execute(store, query, cypher_project, max_rows, &result);

    if (rc < 0) {
        char *err_msg = result.error ? result.error : (char *)"query execution failed";
        char *resp = cbm_mcp_text_result(err_msg, true);
        cbm_cypher_result_free(&result);
        free(query);
        free(project);
        return resp;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    /* columns */
    yyjson_mut_val *cols = yyjson_mut_arr(doc);
    for (int i = 0; i < result.col_count; i++) {
        yyjson_mut_arr_add_str(doc, cols, result.columns[i]);
    }
    yyjson_mut_obj_add_val(doc, root, "columns", cols);

    /* rows */
    yyjson_mut_val *rows = yyjson_mut_arr(doc);
    for (int r = 0; r < result.row_count; r++) {
        yyjson_mut_val *row = yyjson_mut_arr(doc);
        for (int c = 0; c < result.col_count; c++) {
            yyjson_mut_arr_add_str(doc, row, result.rows[r][c]);
        }
        yyjson_mut_arr_add_val(rows, row);
    }
    yyjson_mut_obj_add_val(doc, root, "rows", rows);
    yyjson_mut_obj_add_int(doc, root, "total", result.row_count);
    if (result.warning) {
        yyjson_mut_obj_add_str(doc, root, "warning", result.warning);
    }

    if (result.row_count == 0) {
        yyjson_mut_obj_add_str(
            doc, root, "hint",
            "Query returned no results. Use get_graph_schema() to see available labels and "
            "edge types.");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_cypher_result_free(&result);
    free(query);
    free(project);

    return mcp_text_result_take_owned(json, false);
}

/* Indexing-coverage report (#963), attached to index_status: the best-effort
 * signal from the separate index_coverage table (coverage is metadata ABOUT
 * the graph, stored outside it). Full per-project list, capped generously. */
enum { COVERAGE_FILE_CAP = 500 };

/* Append one parse_unusable file entry, {path, whole_file:true, range_end}, to
 * `files`. `range` is the stored "start-end" text; "range_end" is the END of the
 * range, not the length of the file: a grammar can end an error node past the
 * last line. Shared by index_status and the index_repository summary. */
static void add_parse_unusable_file(yyjson_mut_doc *doc, yyjson_mut_val *files, const char *path,
                                    const char *range) {
    yyjson_mut_val *fe = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, fe, "path", path);
    yyjson_mut_obj_add_bool(doc, fe, "whole_file", true);
    const char *dash = range ? strchr(range, '-') : NULL;
    yyjson_mut_obj_add_int(doc, fe, "range_end", dash ? atoi(dash + 1) : 0);
    yyjson_mut_arr_add_val(files, fe);
}

/* Append one walk-coverage entry, {path, detail}, to `files`. detail is the
 * producer's "<visited>/<total> nodes walked" or "<rule>, <total> nodes". */
static void add_walk_coverage_file(yyjson_mut_doc *doc, yyjson_mut_val *files, const char *path,
                                   const char *detail) {
    yyjson_mut_val *fe = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, fe, "path", path ? path : "");
    yyjson_mut_obj_add_strcpy(doc, fe, "detail", detail ? detail : "");
    yyjson_mut_arr_add_val(files, fe);
}

static void add_coverage_report(yyjson_mut_doc *doc, yyjson_mut_val *root, cbm_store_t *store,
                                const char *project) {
    cbm_coverage_row_t *rows = NULL;
    int count = 0;
    (void)cbm_store_coverage_get(store, project, &rows, &count);

    yyjson_mut_val *pp_files = yyjson_mut_arr(doc);
    yyjson_mut_val *pu_files = yyjson_mut_arr(doc);
    yyjson_mut_val *wt_files = yyjson_mut_arr(doc);
    yyjson_mut_val *ls_files = yyjson_mut_arr(doc);
    yyjson_mut_val *sk_files = yyjson_mut_arr(doc);
    yyjson_mut_val *ni_dirs = yyjson_mut_arr(doc);
    yyjson_mut_val *ni_files = yyjson_mut_arr(doc);
    int pp_n = 0;
    int pu_n = 0;
    int wt_n = 0;
    int ls_n = 0;
    int sk_n = 0;
    int ni_dir_n = 0;
    int ni_file_n = 0;
    for (int i = 0; i < count; i++) {
        const char *kind = rows[i].kind ? rows[i].kind : "";
        if (strcmp(kind, "parse_partial") == 0) {
            if (pp_n < COVERAGE_FILE_CAP) {
                yyjson_mut_val *fe = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, fe, "path", rows[i].rel_path);
                yyjson_mut_obj_add_strcpy(doc, fe, "error_ranges",
                                          rows[i].detail ? rows[i].detail : "");
                yyjson_mut_arr_add_val(pp_files, fe);
            }
            pp_n++;
        } else if (strcmp(kind, "parse_unusable") == 0) {
            /* Needs its own branch: the catch-all below builds skipped[], and a
             * reader who finds a file there believes it was never indexed. */
            if (pu_n < COVERAGE_FILE_CAP) {
                add_parse_unusable_file(doc, pu_files, rows[i].rel_path, rows[i].detail);
            }
            pu_n++;
        } else if (strcmp(kind, "walk_truncated") == 0) {
            /* Indexed too — the walk stopped early. Kept out of skipped[] for
             * the same reason. */
            if (wt_n < COVERAGE_FILE_CAP) {
                add_walk_coverage_file(doc, wt_files, rows[i].rel_path, rows[i].detail);
            }
            wt_n++;
        } else if (strcmp(kind, "lsp_skipped") == 0) {
            /* Indexed, without the per-file and cross-file LSP walks. */
            if (ls_n < COVERAGE_FILE_CAP) {
                add_walk_coverage_file(doc, ls_files, rows[i].rel_path, rows[i].detail);
            }
            ls_n++;
        } else if (strcmp(kind, "not_indexed_dir") == 0) {
            if (ni_dir_n < COVERAGE_FILE_CAP) {
                yyjson_mut_arr_add_strcpy(doc, ni_dirs, rows[i].rel_path);
            }
            ni_dir_n++;
        } else if (strcmp(kind, "not_indexed_file") == 0) {
            if (ni_file_n < COVERAGE_FILE_CAP) {
                yyjson_mut_val *fe = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, fe, "path", rows[i].rel_path);
                yyjson_mut_obj_add_strcpy(doc, fe, "reason", rows[i].detail ? rows[i].detail : "");
                yyjson_mut_arr_add_val(ni_files, fe);
            }
            ni_file_n++;
        } else {
            if (sk_n < COVERAGE_FILE_CAP) {
                yyjson_mut_val *fe = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, fe, "path", rows[i].rel_path);
                yyjson_mut_obj_add_strcpy(doc, fe, "reason", rows[i].detail ? rows[i].detail : "");
                yyjson_mut_obj_add_strcpy(doc, fe, "phase", rows[i].kind ? rows[i].kind : "");
                yyjson_mut_arr_add_val(sk_files, fe);
            }
            sk_n++;
        }
    }
    cbm_store_free_coverage(rows, count);

    yyjson_mut_val *pp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, pp, "files", pp_files);
    yyjson_mut_obj_add_int(doc, pp, "count", pp_n);
    yyjson_mut_obj_add_bool(doc, pp, "truncated", pp_n > COVERAGE_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "parse_partial", pp);

    /* Indexed, but the parse failed across nearly the whole file, so naming
     * line ranges helps nobody — read the source instead. */
    yyjson_mut_val *pu = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, pu, "files", pu_files);
    yyjson_mut_obj_add_int(doc, pu, "count", pu_n);
    yyjson_mut_obj_add_bool(doc, pu, "truncated", pu_n > COVERAGE_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "parse_unusable", pu);

    /* Indexed, but the unified walk stopped at its budget ("N/M nodes walked"),
     * or the file ran without its per-file and cross-file LSP walks. */
    yyjson_mut_val *wt = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, wt, "files", wt_files);
    yyjson_mut_obj_add_int(doc, wt, "count", wt_n);
    yyjson_mut_obj_add_bool(doc, wt, "truncated", wt_n > COVERAGE_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "walk_truncated", wt);

    yyjson_mut_val *ls = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, ls, "files", ls_files);
    yyjson_mut_obj_add_int(doc, ls, "count", ls_n);
    yyjson_mut_obj_add_bool(doc, ls, "truncated", ls_n > COVERAGE_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "lsp_skipped", ls);

    yyjson_mut_val *sk = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, sk, "files", sk_files);
    yyjson_mut_obj_add_int(doc, sk, "count", sk_n);
    yyjson_mut_obj_add_bool(doc, sk, "truncated", sk_n > COVERAGE_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "skipped", sk);

    /* By-design exclusions (#963 "purposely not indexed"): a deliberate,
     * deterministic class — NOT a failure and NOT best-effort. Dirs are
     * exhaustive; per-file entries are capped in discovery (2000) with the
     * truncation explicit. */
    yyjson_mut_val *ni = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, ni, "dirs", ni_dirs);
    yyjson_mut_obj_add_int(doc, ni, "dirs_count", ni_dir_n);
    yyjson_mut_obj_add_val(doc, ni, "files", ni_files);
    yyjson_mut_obj_add_int(doc, ni, "files_count", ni_file_n);
    yyjson_mut_obj_add_bool(doc, ni, "truncated",
                            ni_dir_n > COVERAGE_FILE_CAP || ni_file_n > COVERAGE_FILE_CAP);
    if (ni_dir_n > 0 || ni_file_n > 0) {
        yyjson_mut_obj_add_str(doc, ni, "note",
                               "Purposely not indexed — excluded BY DESIGN via "
                               "gitignore/.cbmignore/skip-lists (see each file's reason). Not an "
                               "error: change the ignore rules and re-index to include them.");
    }
    yyjson_mut_obj_add_val(doc, root, "not_indexed", ni);

    if (pp_n > 0 || sk_n > 0 || wt_n > 0 || ls_n > 0) {
        yyjson_mut_obj_add_str(
            doc, root, "coverage_note",
            "Best-effort signal, not a completeness guarantee: parse_partial files WERE indexed, "
            "but constructs inside the listed line ranges (1-based) MAY be missing from the graph "
            "(tree-sitter error recovery still salvages some). walk_truncated files WERE indexed "
            "up to the node count shown, and lsp_skipped files without type-aware call "
            "resolution. skipped files were not indexed at "
            "all. Prefer text search (grep) for flagged files/ranges. Files absent from this list "
            "are NOT guaranteed to be fully indexed. (not_indexed entries are a separate, "
            "BY-DESIGN class — deliberate ignore rules, not failures.)");
    }
}

static char *handle_index_status(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    REQUIRE_STORE(store, project);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (project) {
        int nodes = cbm_store_count_nodes(store, project);
        int edges = cbm_store_count_edges(store, project);
        /* A negative count is a failed read (CBM_STORE_ERR), not a small
         * project. Reporting it as a count would put a bare -1 in the output,
         * and `nodes > 0 ? "ready" : "empty"` would answer "empty" for an
         * unreadable table. Say the read failed, and name the table. */
        const bool counts_unreadable = nodes < 0 || edges < 0;
        yyjson_mut_obj_add_str(doc, root, "project", project);
        yyjson_mut_obj_add_int(doc, root, "nodes", counts_unreadable ? 0 : nodes);
        yyjson_mut_obj_add_int(doc, root, "edges", counts_unreadable ? 0 : edges);
        const char *status = "empty";
        if (counts_unreadable) {
            status = "error";
        } else if (nodes > 0) {
            status = "ready";
        }
        yyjson_mut_obj_add_str(doc, root, "status", status);
        cbm_project_t proj_info = {0};
        if (cbm_store_get_project(store, project, &proj_info) == CBM_STORE_OK) {
            yyjson_mut_obj_add_strcpy(doc, root, "root_path",
                                      proj_info.root_path ? proj_info.root_path : "");
            add_git_context_json(doc, root, proj_info.root_path, store, project);
            safe_str_free(&proj_info.name);
            safe_str_free(&proj_info.indexed_at);
            safe_str_free(&proj_info.root_path);
        }
        add_coverage_report(doc, root, store, project);
        if (counts_unreadable) {
            const char *hint;
            if (nodes < 0 && edges < 0) {
                hint = "The nodes and edges tables could not be read; the database may be "
                       "corrupt. Re-run index_repository(repo_path=...) or remove the project "
                       "cache and re-index.";
            } else if (nodes < 0) {
                hint = "The nodes table could not be read; the database may be corrupt. Re-run "
                       "index_repository(repo_path=...) or remove the project cache and re-index.";
            } else {
                hint = "The edges table could not be read; the database may be corrupt. Re-run "
                       "index_repository(repo_path=...) or remove the project cache and re-index.";
            }
            yyjson_mut_obj_add_str(doc, root, "hint", hint);
        } else if (nodes == 0) {
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "Project is empty. Re-run index_repository(repo_path=...) to populate.");
        }
    } else {
        yyjson_mut_obj_add_str(doc, root, "status", "no_project");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(project);

    return mcp_text_result_take_owned(json, false);
}

/* delete_project: just erase the .db file (and WAL/SHM). */
static char *handle_delete_project(cbm_mcp_server_t *srv, const char *args) {
    char *name = get_project_arg(args);
    if (!name) {
        return cbm_mcp_text_result("project is required", true);
    }

    /* Claim before closing a cached SQLite handle (which can clean sidecars),
     * and before any DB/WAL/SHM mutation. Never substitute a guessed project. */
    char path[CBM_SZ_1K];
    project_db_path(name, path, sizeof(path));
    cbm_db_lease_t *lease = NULL;
    int claim_rc = cbm_db_lease_try_acquire(path, &lease);
    if (claim_rc != 0) {
        char *result = claim_rc == CBM_INDEX_BUSY
                           ? index_busy_result(name)
                           : cbm_mcp_text_result("failed to acquire database deletion lease", true);
        free(name);
        return result;
    }
    if (!cbm_pipeline_try_lock()) {
        cbm_db_lease_release(lease);
        char *result = index_busy_result(name);
        free(name);
        return result;
    }
    // Keep path as requested. Canonicalization chooses the lock identity only;
    // deleting a cache symlink must unlink that entry, not its backing database.
    /* The reader gate is not taken here, unlike the expiry sweep's move: a
     * server in another process holds it shared for as long as it caches
     * the store (until idle eviction), so an explicit delete would fail for
     * as long as any session had the project open. It does not need it:
     * every tool call starts with cbm_mcp_server_revalidate_store, which
     * finds the file gone and drops the cached handle, so no later call is
     * answered from the deleted inode -- only one already in flight
     * finishes on it. The sweep moves the file into the trash instead, where
     * a restore must not race a reader, hence its gate. */

    /* Which checkout the database is: the root it records, read under both
     * gates. Two directories can derive one project name (/x/a-b/repo,
     * /x/a/b-repo) and share <name>.db, while their parked slots are filed
     * per root. A path argument names one of them (g_expected_root): a
     * database the other indexed is not its to delete -- the ownership
     * check every read applies -- and only that path's slots go. Given a
     * bare name, the slots of the root the database records go with it;
     * every root's only when nothing records one (no database left). A
     * database whose owner cannot be read (it will not open, or its
     * projects table will not answer) is kept from a path argument as a
     * foreign one is: it may be the other directory's. A bare name still
     * deletes it -- the way to remove a corrupt index. */
    char owner[CBM_SZ_4K] = "";
    bool owner_unknown = false;
    if (cbm_file_exists(path)) {
        cbm_store_t *os = cbm_store_open_path_query(path);
        cbm_project_t proj = {0};
        int prc = os ? cbm_store_get_project(os, name, &proj) : CBM_STORE_ERR;
        if (prc == CBM_STORE_OK) {
            snprintf(owner, sizeof(owner), "%s", proj.root_path ? proj.root_path : "");
            cbm_project_free_fields(&proj);
        }
        owner_unknown = prc != CBM_STORE_OK && prc != CBM_STORE_NOT_FOUND;
        cbm_store_close(os);
    }
    bool foreign = g_expected_root[0] &&
                   (owner_unknown || (owner[0] && !cbm_root_matches(owner, g_expected_root)));
    const char *slot_root = g_expected_root[0] ? g_expected_root : owner;

    /* Close store if it's the project being deleted */
    if (!foreign && srv->current_project && strcmp(srv->current_project, name) == 0) {
        if (srv->owns_store && srv->store) {
            cbm_store_close(srv->store);
            srv->store = NULL;
        }
        free(srv->current_project);
        srv->current_project = NULL;
    }

    /* Delete the .db file + WAL/SHM, its parked branches and its memo and
     * usage rows while both gates are held -- the same removal cache expiry
     * uses (cbm_cache_remove_project). Parked branches can outlive a main
     * file that is already gone; they are kept when the main file could not
     * be removed: the project still exists, and its branches with it. A
     * database another directory indexed stays, as if absent; the slots of
     * the named directory still go. */
    const char *status = "not_found";
    const char *error_detail = NULL;
    bool is_error = false;
    int parked_deleted = 0;
    int removed = 1;
    if (foreign) {
        cbm_log_info("project.name_collision", "project", name, "indexed_root", owner, "wanted",
                     g_expected_root);
        parked_deleted = cbm_branch_slots_delete_root(path, name, g_expected_root);
    } else {
        removed = cbm_cache_remove_project_root(path, name, slot_root, &parked_deleted);
    }
    bool slots_left = removed == 2;
    if (removed == 0 || slots_left) {
        status = "deleted";
    } else if (removed < 0) {
        status = "delete_failed";
        error_detail = strerror(errno);
        is_error = true;
    } else if (parked_deleted > 0) {
        status = "deleted";
    } else {
        is_error = true;
    }

    cbm_pipeline_unlock();
    cbm_db_lease_release(lease);

    if (srv->watcher && !foreign) {
        cbm_watcher_unwatch(srv->watcher, name);
    }

    cbm_mem_collect(); /* return freed pages to OS after closing database */

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", name);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    if (foreign && owner_unknown) {
        yyjson_mut_obj_add_str(doc, root, "note",
                               "the database under this name could not be read to learn which "
                               "directory it was indexed from, so it is kept; delete it by "
                               "project name to remove it anyway");
    } else if (foreign) {
        yyjson_mut_obj_add_strcpy(doc, root, "kept_database_of", owner);
        yyjson_mut_obj_add_str(doc, root, "note",
                               "the database under this name was indexed from another directory "
                               "(a project-name collision); it is kept");
    }
    if (parked_deleted > 0) {
        yyjson_mut_obj_add_int(doc, root, "parked_branches_deleted", parked_deleted);
    }
    if (slots_left) {
        yyjson_mut_obj_add_str(doc, root, "warning",
                               "some parked branches could not be removed; the cache sweep "
                               "retries (cache.remove.slots_left in the log)");
    }
    if (error_detail) {
        yyjson_mut_obj_add_str(doc, root, "error", error_detail);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(name);

    return mcp_text_result_take_owned(json, is_error);
}

/* Canonical list of valid aspect tokens for get_architecture. Single source
 * of truth for the server-side validation (authoritative); the JSON-Schema
 * enum in the TOOLS entry above is the advisory client-side mirror — update
 * both together when the aspect set changes. */
static const char *VALID_ASPECTS[] = {"all",      "overview",   "structure", "dependencies",
                                      "routes",   "languages",  "packages",  "entry_points",
                                      "hotspots", "boundaries", "layers",    "file_tree",
                                      "clusters", "cycles",     NULL};

/* ── SCC / cycle condensation (get_architecture "cycles") ─────────
 * Iterative Tarjan over the CALLS call graph. Recursion would overflow on a
 * large graph, so the DFS state lives on explicit heap stacks. Reports the
 * strongly-connected components of size > 1 — the circular call dependencies,
 * the non-trivial content of the condensation quotient. */
typedef struct {
    int64_t *ids;  /* sorted unique node ids; index = position */
    int nverts;    /* |V| */
    int *adj_head; /* CSR row starts, length nverts+1 */
    int *adj;      /* CSR column (target vertex indices), length nedges */
    int nedges;    /* |E| within the vertex set */
} scc_graph_t;

static int scc_id_index(const int64_t *ids, int n, int64_t id) {
    int lo = 0;
    int hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ids[mid] < id) {
            lo = mid + 1;
        } else if (ids[mid] > id) {
            hi = mid - 1;
        } else {
            return mid;
        }
    }
    return -1;
}

static int cmp_int64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* Build the CSR call graph from parallel (src,tgt) edge arrays. Returns false
 * on OOM/empty. */
static bool scc_build(const int64_t *src, const int64_t *tgt, int ecount, scc_graph_t *g) {
    memset(g, 0, sizeof(*g));
    if (ecount <= 0) {
        return false;
    }
    int64_t *all = (int64_t *)malloc((size_t)ecount * 2 * sizeof(int64_t));
    if (!all) {
        return false;
    }
    for (int i = 0; i < ecount; i++) {
        all[2 * i] = src[i];
        all[2 * i + 1] = tgt[i];
    }
    qsort(all, (size_t)ecount * 2, sizeof(int64_t), cmp_int64);
    int nv = 0;
    for (int i = 0; i < ecount * 2; i++) {
        if (i == 0 || all[i] != all[i - 1]) {
            all[nv++] = all[i];
        }
    }
    g->ids = all;
    g->nverts = nv;
    g->adj_head = (int *)calloc((size_t)nv + 1, sizeof(int));
    g->adj = (int *)malloc((size_t)ecount * sizeof(int));
    if (!g->adj_head || !g->adj) {
        free(g->ids);
        free(g->adj_head);
        free(g->adj);
        memset(g, 0, sizeof(*g));
        return false;
    }
    /* two-pass CSR fill. Every endpoint is in ids[] by construction (ids is
     * built from these same edge arrays), so a negative lookup is unreachable
     * -- but that invariant is invisible to path-sensitive analysis, and a
     * guard keeps a future collection bug from becoming an OOB write. Both
     * passes must skip identically or the cursors desync. */
    for (int i = 0; i < ecount; i++) {
        int u = scc_id_index(g->ids, nv, src[i]);
        int v = scc_id_index(g->ids, nv, tgt[i]);
        if (u < 0 || v < 0) {
            continue;
        }
        g->adj_head[u + 1]++;
    }
    for (int i = 0; i < nv; i++) {
        g->adj_head[i + 1] += g->adj_head[i];
    }
    int *cursor = (int *)malloc((size_t)nv * sizeof(int));
    if (!cursor) {
        free(g->ids);
        free(g->adj_head);
        free(g->adj);
        memset(g, 0, sizeof(*g));
        return false;
    }
    for (int i = 0; i < nv; i++) {
        cursor[i] = g->adj_head[i];
    }
    int filled = 0;
    for (int i = 0; i < ecount; i++) {
        int u = scc_id_index(g->ids, nv, src[i]);
        int v = scc_id_index(g->ids, nv, tgt[i]);
        if (u < 0 || v < 0) {
            continue;
        }
        g->adj[cursor[u]++] = v;
        filled++;
    }
    free(cursor);
    g->nedges = filled;
    return true;
}

static void scc_free(scc_graph_t *g) {
    free(g->ids);
    free(g->adj_head);
    free(g->adj);
    memset(g, 0, sizeof(*g));
}

/* Iterative Tarjan. Fills comp[v] with a component id; components are numbered
 * in discovery order. Returns the component count, or -1 on OOM. */
static int scc_tarjan(const scc_graph_t *g, int *comp) {
    enum { SCC_UNVISITED = -1 };
    int nv = g->nverts;
    int *index = (int *)malloc((size_t)nv * sizeof(int));
    int *low = (int *)malloc((size_t)nv * sizeof(int));
    bool *on_stack = (bool *)calloc((size_t)nv, sizeof(bool));
    int *tstack = (int *)malloc((size_t)nv * sizeof(int)); /* Tarjan's node stack */
    int *dfs_v = (int *)malloc((size_t)nv * sizeof(int));  /* explicit DFS: vertex */
    int *dfs_i = (int *)malloc((size_t)nv * sizeof(int));  /* explicit DFS: adj cursor */
    if (!index || !low || !on_stack || !tstack || !dfs_v || !dfs_i) {
        free(index);
        free(low);
        free(on_stack);
        free(tstack);
        free(dfs_v);
        free(dfs_i);
        return -1;
    }
    for (int i = 0; i < nv; i++) {
        index[i] = SCC_UNVISITED;
        comp[i] = SCC_UNVISITED;
    }
    int counter = 0;
    int tsp = 0;   /* Tarjan stack pointer */
    int ncomp = 0; /* component id allocator */
    for (int s = 0; s < nv; s++) {
        if (index[s] != SCC_UNVISITED) {
            continue;
        }
        int dsp = 0; /* DFS stack pointer */
        dfs_v[dsp] = s;
        dfs_i[dsp] = g->adj_head[s];
        index[s] = low[s] = counter++;
        tstack[tsp++] = s;
        on_stack[s] = true;
        while (dsp >= 0) {
            int v = dfs_v[dsp];
            if (dfs_i[dsp] < g->adj_head[v + 1]) {
                int w = g->adj[dfs_i[dsp]++];
                if (index[w] == SCC_UNVISITED) {
                    index[w] = low[w] = counter++;
                    tstack[tsp++] = w;
                    on_stack[w] = true;
                    dsp++;
                    dfs_v[dsp] = w;
                    dfs_i[dsp] = g->adj_head[w];
                } else if (on_stack[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
            } else {
                /* v fully explored: it is a root iff low==index -> pop an SCC */
                if (low[v] == index[v]) {
                    int w;
                    do {
                        w = tstack[--tsp];
                        on_stack[w] = false;
                        comp[w] = ncomp;
                    } while (w != v);
                    ncomp++;
                }
                dsp--;
                if (dsp >= 0 && low[v] < low[dfs_v[dsp]]) {
                    low[dfs_v[dsp]] = low[v];
                }
            }
        }
    }
    free(index);
    free(low);
    free(on_stack);
    free(tstack);
    free(dfs_v);
    free(dfs_i);
    return ncomp;
}

static bool aspect_is_valid(const char *name) {
    if (!name) {
        return false;
    }
    for (int i = 0; VALID_ASPECTS[i]; i++) {
        if (strcmp(name, VALID_ASPECTS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Check if an aspect is requested. NULL aspects = all. The array can contain
 * "all" (everything), "overview" (everything except file_tree — see
 * cbm_store_arch_aspect_in_overview in store.c), or the aspect name itself. */
/* True ONLY when `name` is explicitly present in the aspects array — never via
 * the no-filter default, "all", or "overview". For expensive opt-in aspects
 * (cycles scans the whole call graph) that must not run on a bare call. */
static bool aspect_explicitly_named(yyjson_val *aspects_arr, const char *name) {
    if (!aspects_arr) {
        return false;
    }
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(aspects_arr, &iter);
    yyjson_val *val;
    while ((val = yyjson_arr_iter_next(&iter)) != NULL) {
        const char *s = yyjson_get_str(val);
        if (s && strcmp(s, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool aspect_wanted(yyjson_doc *aspects_doc, yyjson_val *aspects_arr, const char *name) {
    if (!aspects_arr) {
        return true; /* no filter = all */
    }
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(aspects_arr, &iter);
    yyjson_val *val;
    while ((val = yyjson_arr_iter_next(&iter)) != NULL) {
        const char *s = yyjson_get_str(val);
        if (!s) {
            continue;
        }
        if (strcmp(s, "all") == 0) {
            return true;
        }
        if (strcmp(s, "overview") == 0 && cbm_store_arch_aspect_in_overview(name)) {
            return true;
        }
        if (strcmp(s, name) == 0) {
            return true;
        }
    }
    (void)aspects_doc;
    return false;
}

/* Append cross_repo_links summary to architecture JSON if CROSS_* edges exist. */
static void append_cross_repo_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                      const cbm_schema_info_t *schema) {
    /* Scan edge types for any CROSS_* edges and sum them */
    int cross_total = 0;
    yyjson_mut_val *cr = yyjson_mut_obj(doc);
    static const char *cross_types[] = {"CROSS_HTTP_CALLS",    "CROSS_ASYNC_CALLS",
                                        "CROSS_CHANNEL",       "CROSS_GRPC_CALLS",
                                        "CROSS_GRAPHQL_CALLS", "CROSS_TRPC_CALLS"};
    for (int t = 0; t < (int)(sizeof(cross_types) / sizeof(cross_types[0])); t++) {
        for (int i = 0; i < schema->edge_type_count; i++) {
            if (strcmp(schema->edge_types[i].type, cross_types[t]) == 0) {
                yyjson_mut_obj_add_int(doc, cr, cross_types[t], schema->edge_types[i].count);
                cross_total += schema->edge_types[i].count;
                break;
            }
        }
    }
    if (cross_total > 0) {
        yyjson_mut_obj_add_int(doc, cr, "total", cross_total);
        yyjson_mut_obj_add_val(doc, root, "cross_repo_links", cr);
    }
}

/* Compute the circular-dependency SCCs (size > 1) of the CALLS graph. Returns
 * a malloc'd array of components, each a malloc'd int64 array of member node
 * ids, with sizes in *out_sizes and count in *out_ncycles; sets *scanned_edges
 * and *edges_truncated. Caller frees each component + the arrays. Returns
 * CBM_STORE_OK, or CBM_STORE_ERR on failure (all outs zeroed). */
enum { ARCH_SCC_MAX_EDGES = 400000, ARCH_SCC_MAX_CYCLES = 100, ARCH_SCC_MEMBERS_SHOWN = 20 };

static int arch_compute_cycles(cbm_store_t *store, const char *project, int64_t ***out_members,
                               int **out_sizes, int *out_ncycles, int *scanned_edges,
                               bool *edges_truncated) {
    *out_members = NULL;
    *out_sizes = NULL;
    *out_ncycles = 0;
    *scanned_edges = 0;
    *edges_truncated = false;

    int64_t *src = NULL;
    int64_t *tgt = NULL;
    int ecount = 0;
    if (cbm_store_fetch_call_edges(store, project, ARCH_SCC_MAX_EDGES, &src, &tgt, &ecount,
                                   edges_truncated) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    *scanned_edges = ecount;
    scc_graph_t g;
    if (!scc_build(src, tgt, ecount, &g)) {
        free(src);
        free(tgt);
        return CBM_STORE_OK; /* no edges = no cycles, not an error */
    }
    free(src);
    free(tgt);

    int *comp = (int *)malloc((size_t)g.nverts * sizeof(int));
    if (!comp) {
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    int ncomp = scc_tarjan(&g, comp);
    if (ncomp < 0) {
        free(comp);
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    /* Tarjan assigns every vertex a component, so ncomp >= 1 whenever
     * nverts >= 1 -- state it, so the zero-size-allocation path below is
     * provably confined to the empty graph. */
    if (g.nverts > 0 && ncomp <= 0) {
        free(comp);
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    /* size per component */
    int *csize = (int *)calloc((size_t)ncomp, sizeof(int));
    if (!csize) {
        free(comp);
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    for (int v = 0; v < g.nverts; v++) {
        csize[comp[v]]++;
    }
    /* collect components with size > 1 (the cycles) */
    int ncyc = 0;
    for (int c = 0; c < ncomp; c++) {
        if (csize[c] > 1) {
            ncyc++;
        }
    }
    if (ncyc > ARCH_SCC_MAX_CYCLES) {
        ncyc = ARCH_SCC_MAX_CYCLES;
    }
    int64_t **members = ncyc > 0 ? (int64_t **)calloc((size_t)ncyc, sizeof(int64_t *)) : NULL;
    int *sizes = ncyc > 0 ? (int *)calloc((size_t)ncyc, sizeof(int)) : NULL;
    /* map component id -> output slot (only for the first ARCH_SCC_MAX_CYCLES
     * size>1 comps, in component-id order) */
    int *slot = (int *)malloc((size_t)ncomp * sizeof(int));
    if ((ncyc > 0 && (!members || !sizes)) || !slot) {
        free(members);
        free(sizes);
        free(slot);
        free(csize);
        free(comp);
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    int next_slot = 0;
    for (int c = 0; c < ncomp; c++) {
        if (csize[c] > 1 && next_slot < ncyc) {
            slot[c] = next_slot;
            sizes[next_slot] = csize[c];
            members[next_slot] = (int64_t *)malloc((size_t)csize[c] * sizeof(int64_t));
            next_slot++;
        } else {
            slot[c] = -1;
        }
    }
    int *fill = (int *)calloc((size_t)ncyc, sizeof(int));
    if (ncyc > 0 && !fill) {
        for (int i = 0; i < ncyc; i++) {
            free(members[i]);
        }
        free(members);
        free(sizes);
        free(slot);
        free(csize);
        free(comp);
        scc_free(&g);
        return CBM_STORE_ERR;
    }
    /* With ncyc == 0 no slot is ever >= 0 (the slot loop can't assign one),
     * so members -- NULL in that case -- is never indexed; make that
     * invariant local instead of cross-loop so it is checkable. */
    if (ncyc > 0) {
        for (int v = 0; v < g.nverts; v++) {
            int sl = slot[comp[v]];
            if (sl >= 0) {
                members[sl][fill[sl]++] = g.ids[v];
            }
        }
    }
    free(fill);
    free(slot);
    free(csize);
    free(comp);
    scc_free(&g);
    *out_members = members;
    *out_sizes = sizes;
    *out_ncycles = ncyc;
    return CBM_STORE_OK;
}

/* Fetch the qualified_name for a node id, or a "#<id>" fallback. */
static void arch_node_qn(cbm_store_t *store, int64_t id, char *out, size_t outsz) {
    cbm_node_t n = {0};
    if (cbm_store_find_node_by_id(store, id, &n) == CBM_STORE_OK && n.qualified_name) {
        snprintf(out, outsz, "%s", n.qualified_name);
    } else {
        snprintf(out, outsz, "#%lld", (long long)id);
    }
    free_node_contents(&n);
}

static char *handle_get_architecture(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    /* REQUIRE_STORE returns without freeing anything but `project`, so every
     * other allocation must come after it (scope_path leaked here before —
     * caught by the clang-analyzer unix.Malloc lane). */
    REQUIRE_STORE(store, project);
    char *scope_path = cbm_mcp_get_string_arg(args, "path");

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(project);
        free(scope_path);
        return not_indexed;
    }

    /* Parse aspects array from args */
    yyjson_doc *aspects_doc = NULL;
    yyjson_val *aspects_arr = NULL;
    {
        yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
        if (args_doc) {
            yyjson_val *aval = yyjson_obj_get(yyjson_doc_get_root(args_doc), "aspects");
            if (yyjson_is_arr(aval)) {
                aspects_doc = args_doc; /* keep alive */
                aspects_arr = aval;
            } else {
                yyjson_doc_free(args_doc);
            }
        }
    }

    /* Build a C string array from aspects for cbm_store_get_architecture.
     * Strings point into aspects_doc memory so aspects_doc must outlive this array. */
    const char *aspects_strs[MCP_COL_16];
    int aspects_strs_count = 0;
    if (aspects_arr) {
        size_t aspect_idx;
        size_t aspect_max;
        yyjson_val *aspect_val;
        yyjson_arr_foreach(aspects_arr, aspect_idx, aspect_max, aspect_val) {
            const char *s = yyjson_get_str(aspect_val);
            if (s && aspects_strs_count < MCP_COL_16) {
                aspects_strs[aspects_strs_count++] = s;
            }
        }
    }

    /* Server-side validation: reject unknown aspect tokens with an isError
     * result listing the valid values. The JSON-Schema enum is advisory —
     * many MCP clients do not validate arguments against tool schemas — so
     * without this check a typo degraded to a silent near-empty payload. */
    for (int i = 0; i < aspects_strs_count; i++) {
        if (!aspect_is_valid(aspects_strs[i])) {
            char valid_list[CBM_SZ_256];
            size_t off = 0;
            for (int j = 0; VALID_ASPECTS[j] && off < sizeof(valid_list); j++) {
                int n = snprintf(valid_list + off, sizeof(valid_list) - off, "%s%s",
                                 j > 0 ? ", " : "", VALID_ASPECTS[j]);
                if (n < 0) {
                    break;
                }
                off += (size_t)n;
            }
            char msg[CBM_SZ_512];
            snprintf(msg, sizeof(msg), "Unknown aspect '%s'. Valid: %s.", aspects_strs[i],
                     valid_list);
            char *err = cbm_mcp_text_result(msg, true);
            free(project);
            free(scope_path);
            if (aspects_doc) {
                yyjson_doc_free(aspects_doc);
            }
            return err;
        }
    }

    cbm_schema_info_t schema = {0};
    /* Counts-only: this handler renders label/type counts but never property
     * keys, and full key discovery json_each-scans every row (seconds-to-
     * minutes on multi-million-node graphs). */
    cbm_store_get_schema_counts_scoped(store, project, scope_path, &schema);

    cbm_architecture_info_t arch = {0};
    cbm_store_get_architecture(store, project, scope_path,
                               aspects_strs_count > 0 ? aspects_strs : NULL, aspects_strs_count,
                               &arch);

    int node_count = cbm_store_count_nodes_scoped(store, project, scope_path);
    int edge_count = cbm_store_count_edges_scoped(store, project, scope_path);
    char norm_path[CBM_SZ_512];
    bool path_scoped = cbm_store_normalize_arch_path(scope_path, norm_path, sizeof(norm_path));

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (project) {
        yyjson_mut_obj_add_str(doc, root, "project", project);
    }
    if (path_scoped) {
        yyjson_mut_obj_add_str(doc, root, "path", norm_path);
        int root_nodes = cbm_store_count_nodes(store, project);
        int root_edges = cbm_store_count_edges(store, project);
        yyjson_mut_obj_add_int(doc, root, "root_total_nodes", root_nodes);
        yyjson_mut_obj_add_int(doc, root, "root_total_edges", root_edges);
        yyjson_mut_obj_add_int(doc, root, "scoped_total_nodes", node_count);
        yyjson_mut_obj_add_int(doc, root, "scoped_total_edges", edge_count);
    }
    yyjson_mut_obj_add_int(doc, root, "total_nodes", node_count);
    yyjson_mut_obj_add_int(doc, root, "total_edges", edge_count);

    /* Node label summary */
    if (aspect_wanted(aspects_doc, aspects_arr, "structure")) {
        yyjson_mut_val *labels = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.node_label_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "label", schema.node_labels[i].label);
            yyjson_mut_obj_add_int(doc, item, "count", schema.node_labels[i].count);
            yyjson_mut_arr_add_val(labels, item);
        }
        yyjson_mut_obj_add_val(doc, root, "node_labels", labels);
    }

    /* Edge type summary */
    if (aspect_wanted(aspects_doc, aspects_arr, "dependencies")) {
        yyjson_mut_val *types = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.edge_type_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "type", schema.edge_types[i].type);
            yyjson_mut_obj_add_int(doc, item, "count", schema.edge_types[i].count);
            yyjson_mut_arr_add_val(types, item);
        }
        yyjson_mut_obj_add_val(doc, root, "edge_types", types);
    }

    /* Relationship patterns */
    if (aspect_wanted(aspects_doc, aspects_arr, "routes") && schema.rel_pattern_count > 0) {
        yyjson_mut_val *pats = yyjson_mut_arr(doc);
        for (int i = 0; i < schema.rel_pattern_count; i++) {
            yyjson_mut_arr_add_str(doc, pats, schema.rel_patterns[i]);
        }
        yyjson_mut_obj_add_val(doc, root, "relationship_patterns", pats);
    }

    /* Languages */
    if (arch.language_count > 0) {
        yyjson_mut_val *langs = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.language_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "language",
                                   arch.languages[i].language ? arch.languages[i].language : "");
            yyjson_mut_obj_add_int(doc, item, "file_count", arch.languages[i].file_count);
            yyjson_mut_arr_add_val(langs, item);
        }
        yyjson_mut_obj_add_val(doc, root, "languages", langs);
    }

    /* Packages */
    if (arch.package_count > 0) {
        yyjson_mut_val *pkgs = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.package_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.packages[i].name ? arch.packages[i].name : "");
            yyjson_mut_obj_add_int(doc, item, "node_count", arch.packages[i].node_count);
            yyjson_mut_obj_add_int(doc, item, "fan_in", arch.packages[i].fan_in);
            yyjson_mut_obj_add_int(doc, item, "fan_out", arch.packages[i].fan_out);
            yyjson_mut_arr_add_val(pkgs, item);
        }
        yyjson_mut_obj_add_val(doc, root, "packages", pkgs);
    }

    /* Entry points */
    if (arch.entry_point_count > 0) {
        yyjson_mut_val *eps = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.entry_point_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.entry_points[i].name ? arch.entry_points[i].name : "");
            yyjson_mut_obj_add_str(
                doc, item, "qualified_name",
                arch.entry_points[i].qualified_name ? arch.entry_points[i].qualified_name : "");
            yyjson_mut_obj_add_str(doc, item, "file",
                                   arch.entry_points[i].file ? arch.entry_points[i].file : "");
            yyjson_mut_arr_add_val(eps, item);
        }
        yyjson_mut_obj_add_val(doc, root, "entry_points", eps);
    }

    /* HTTP routes */
    if (arch.route_count > 0) {
        yyjson_mut_val *routes = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.route_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "method",
                                   arch.routes[i].method ? arch.routes[i].method : "");
            yyjson_mut_obj_add_str(doc, item, "path",
                                   arch.routes[i].path ? arch.routes[i].path : "");
            yyjson_mut_obj_add_str(doc, item, "handler",
                                   arch.routes[i].handler ? arch.routes[i].handler : "");
            yyjson_mut_arr_add_val(routes, item);
        }
        yyjson_mut_obj_add_val(doc, root, "routes", routes);
    }

    /* Hotspots */
    if (arch.hotspot_count > 0) {
        yyjson_mut_val *hotspots = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.hotspot_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.hotspots[i].name ? arch.hotspots[i].name : "");
            yyjson_mut_obj_add_str(doc, item, "qualified_name",
                                   arch.hotspots[i].qualified_name ? arch.hotspots[i].qualified_name
                                                                   : "");
            yyjson_mut_obj_add_int(doc, item, "fan_in", arch.hotspots[i].fan_in);
            yyjson_mut_arr_add_val(hotspots, item);
        }
        yyjson_mut_obj_add_val(doc, root, "hotspots", hotspots);
    }

    /* Cross-package boundaries */
    if (arch.boundary_count > 0) {
        yyjson_mut_val *boundaries = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.boundary_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "from",
                                   arch.boundaries[i].from ? arch.boundaries[i].from : "");
            yyjson_mut_obj_add_str(doc, item, "to",
                                   arch.boundaries[i].to ? arch.boundaries[i].to : "");
            yyjson_mut_obj_add_int(doc, item, "call_count", arch.boundaries[i].call_count);
            yyjson_mut_arr_add_val(boundaries, item);
        }
        yyjson_mut_obj_add_val(doc, root, "boundaries", boundaries);
    }

    /* Cross-service links (HTTP/async between services) */
    if (arch.service_count > 0) {
        yyjson_mut_val *services = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.service_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "from",
                                   arch.services[i].from ? arch.services[i].from : "");
            yyjson_mut_obj_add_str(doc, item, "to", arch.services[i].to ? arch.services[i].to : "");
            yyjson_mut_obj_add_str(doc, item, "type",
                                   arch.services[i].type ? arch.services[i].type : "");
            yyjson_mut_obj_add_int(doc, item, "count", arch.services[i].count);
            yyjson_mut_arr_add_val(services, item);
        }
        yyjson_mut_obj_add_val(doc, root, "services", services);
    }

    /* Package layers */
    if (arch.layer_count > 0) {
        yyjson_mut_val *layers = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.layer_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "name",
                                   arch.layers[i].name ? arch.layers[i].name : "");
            yyjson_mut_obj_add_str(doc, item, "layer",
                                   arch.layers[i].layer ? arch.layers[i].layer : "");
            yyjson_mut_obj_add_str(doc, item, "reason",
                                   arch.layers[i].reason ? arch.layers[i].reason : "");
            yyjson_mut_arr_add_val(layers, item);
        }
        yyjson_mut_obj_add_val(doc, root, "layers", layers);
    }

    /* Clusters (community detection) */
    if (arch.cluster_count > 0) {
        yyjson_mut_val *clusters = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.cluster_count; i++) {
            const cbm_cluster_info_t *c = &arch.clusters[i];
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_int(doc, item, "id", c->id);
            yyjson_mut_obj_add_str(doc, item, "label", c->label ? c->label : "");
            yyjson_mut_obj_add_int(doc, item, "members", c->members);
            yyjson_mut_obj_add_real(doc, item, "cohesion", c->cohesion);
            yyjson_mut_val *top = yyjson_mut_arr(doc);
            for (int j = 0; j < c->top_node_count; j++) {
                yyjson_mut_arr_add_str(doc, top, c->top_nodes[j] ? c->top_nodes[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "top_nodes", top);
            yyjson_mut_val *pkgs = yyjson_mut_arr(doc);
            for (int j = 0; j < c->package_count; j++) {
                yyjson_mut_arr_add_str(doc, pkgs, c->packages[j] ? c->packages[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "packages", pkgs);
            yyjson_mut_val *etypes = yyjson_mut_arr(doc);
            for (int j = 0; j < c->edge_type_count; j++) {
                yyjson_mut_arr_add_str(doc, etypes, c->edge_types[j] ? c->edge_types[j] : "");
            }
            yyjson_mut_obj_add_val(doc, item, "edge_types", etypes);
            yyjson_mut_arr_add_val(clusters, item);
        }
        yyjson_mut_obj_add_val(doc, root, "clusters", clusters);
    }

    /* File tree */
    if (arch.file_tree_count > 0) {
        yyjson_mut_val *file_tree = yyjson_mut_arr(doc);
        for (int i = 0; i < arch.file_tree_count; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "path",
                                   arch.file_tree[i].path ? arch.file_tree[i].path : "");
            yyjson_mut_obj_add_str(doc, item, "type",
                                   arch.file_tree[i].type ? arch.file_tree[i].type : "");
            yyjson_mut_obj_add_int(doc, item, "children", arch.file_tree[i].children);
            yyjson_mut_arr_add_val(file_tree, item);
        }
        yyjson_mut_obj_add_val(doc, root, "file_tree", file_tree);
    }

    append_cross_repo_summary(doc, root, &schema);

    /* cycles: circular CALLS dependencies (SCCs of size > 1) — opt-in ONLY, it
     * scans the whole call graph. A quotient/condensation view. */
    if (aspect_explicitly_named(aspects_arr, "cycles")) {
        int64_t **members = NULL;
        int *sizes = NULL;
        int ncyc = 0;
        int scanned = 0;
        bool etrunc = false;
        if (arch_compute_cycles(store, project, &members, &sizes, &ncyc, &scanned, &etrunc) ==
            CBM_STORE_OK) {
            yyjson_mut_obj_add_int(doc, root, "call_edges_scanned", scanned);
            yyjson_mut_obj_add_bool(doc, root, "cycles_partial", etrunc);
            yyjson_mut_val *cyc = yyjson_mut_arr(doc);
            for (int c = 0; c < ncyc; c++) {
                yyjson_mut_val *o = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_int(doc, o, "size", sizes[c]);
                yyjson_mut_val *mem = yyjson_mut_arr(doc);
                int show = sizes[c] < ARCH_SCC_MEMBERS_SHOWN ? sizes[c] : ARCH_SCC_MEMBERS_SHOWN;
                for (int m = 0; m < show; m++) {
                    char qn[CBM_SZ_512];
                    arch_node_qn(store, members[c][m], qn, sizeof(qn));
                    yyjson_mut_arr_add_strcpy(doc, mem, qn);
                }
                yyjson_mut_obj_add_val(doc, o, "members", mem);
                yyjson_mut_arr_add_val(cyc, o);
                free(members[c]);
            }
            yyjson_mut_obj_add_val(doc, root, "cycles", cyc);
            free(members);
            free(sizes);
        }
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_architecture_free(&arch);
    cbm_store_schema_free(&schema);
    if (aspects_doc) {
        yyjson_doc_free(aspects_doc);
    }
    free(project);
    free(scope_path);

    return mcp_text_result_take_owned(json, false);
}

/* Resolve edge types from args: explicit array > mode-based > default ("CALLS").
 * Writes types into out_types (max 16). Returns the parsed yyjson_doc if explicit
 * edge_types were found (caller must keep alive until types are consumed), or NULL. */
static yyjson_doc *resolve_trace_edge_types(const char *args, const char *mode,
                                            const char **out_types, int *out_count) {
    static const char *mode_calls[] = {"CALLS"};
    static const char *mode_data_flow[] = {"CALLS", "DATA_FLOWS"};
    static const char *mode_cross_svc[] = {
        "HTTP_CALLS",          "ASYNC_CALLS",       "DATA_FLOWS",    "CALLS",
        "CROSS_HTTP_CALLS",    "CROSS_ASYNC_CALLS", "CROSS_CHANNEL", "CROSS_GRPC_CALLS",
        "CROSS_GRAPHQL_CALLS", "CROSS_TRPC_CALLS"};

    *out_count = 0;

    yyjson_doc *et_doc = yyjson_read(args, strlen(args), 0);
    if (et_doc) {
        yyjson_val *et_arr = yyjson_obj_get(yyjson_doc_get_root(et_doc), "edge_types");
        if (et_arr && yyjson_is_arr(et_arr)) {
            size_t idx2;
            size_t max2;
            yyjson_val *val2;
            yyjson_arr_foreach(et_arr, idx2, max2, val2) {
                if (yyjson_is_str(val2) && *out_count < MCP_COL_16) {
                    out_types[(*out_count)++] = yyjson_get_str(val2);
                }
            }
        }
    }

    if (*out_count > 0) {
        return et_doc; /* caller must keep alive — pointers reference doc memory */
    }

    yyjson_doc_free(et_doc); /* no explicit types found, free */

    const char **defaults = mode_calls;
    int n_defaults = SKIP_ONE;
    if (mode && strcmp(mode, "data_flow") == 0) {
        defaults = mode_data_flow;
        n_defaults = MCP_N_DEFAULTS_2;
    } else if (mode && strcmp(mode, "cross_service") == 0) {
        defaults = mode_cross_svc;
        n_defaults = (int)(sizeof(mode_cross_svc) / sizeof(mode_cross_svc[0]));
    }
    for (int i = 0; i < n_defaults; i++) {
        out_types[i] = defaults[i];
    }
    *out_count = n_defaults;
    return NULL;
}

/* Check if a file path looks like a test file. The substring checks below only
 * catch a tests/ directory nested under another path component
 * (".../tests/foo"); a PROJECT-ROOT-RELATIVE path like "tests/repro/foo.c" has
 * no leading slash before "tests" and fell through undetected, leaking whole
 * test subtrees into trace_path results with the default include_tests=false
 * (#1294). */
static bool is_test_file(const char *in_path) {
    if (!in_path) {
        return false;
    }
    /* Case-insensitive: PcapPlusPlus keeps its suites under "Tests/" and
     * "Packet++Test/", which the case-sensitive check let through as
     * production callers with include_tests=false. */
    char path[CBM_SZ_1K];
    size_t n = 0;
    for (; in_path[n] && n + 1 < sizeof(path); n++) {
        path[n] = (char)tolower((unsigned char)in_path[n]);
    }
    path[n] = '\0';
    return strstr(path, "/test") != NULL || strstr(path, "test_") != NULL ||
           strstr(path, "_test.") != NULL || strstr(path, "/tests/") != NULL ||
           strstr(path, "/spec/") != NULL || strstr(path, ".test.") != NULL ||
           strncmp(path, "tests/", SLEN("tests/")) == 0 ||
           strncmp(path, "test/", SLEN("test/")) == 0 ||
           strncmp(path, "spec/", SLEN("spec/")) == 0 ||
           strncmp(path, "__tests__/", SLEN("__tests__/")) == 0;
}

/* Convert BFS traversal results into a yyjson_mut array. */
/* True if `node_name` is endpoint of any edge in tr->edges with
 * confidence >= min_confidence. Linear scan; cheap for typical BFS sizes
 * (capped by MCP_BFS_LIMIT). */
static bool node_has_strong_edge(const cbm_traverse_result_t *tr, const char *node_name,
                                 double min_confidence) {
    if (!node_name) {
        return false;
    }
    for (int e = 0; e < tr->edge_count; e++) {
        if (tr->edges[e].confidence < min_confidence) {
            continue;
        }
        const char *fn = tr->edges[e].from_name;
        const char *tn = tr->edges[e].to_name;
        if ((fn && strcmp(fn, node_name) == 0) || (tn && strcmp(tn, node_name) == 0)) {
            return true;
        }
    }
    return false;
}

/* Find the CALLS-edge "args" JSON (the serialized arg expressions) on the edge
 * that leads to the given hop node, so data_flow mode can surface argument
 * expressions (#514). Returns the borrowed substring "[...]" inside the edge's
 * properties_json, with its length, or NULL when no args are recorded. */
static const char *bfs_edge_args_for_hop(cbm_traverse_result_t *tr, int64_t hop_node_id,
                                         size_t *out_len) {
    for (int e = 0; e < tr->edge_count; e++) {
        /* The hop node is the edge endpoint reached from the root side: for an
         * outbound trace it is the target, for inbound it is the source. Match
         * on either so both directions surface their args. */
        if (tr->edges[e].target_id != hop_node_id && tr->edges[e].source_id != hop_node_id) {
            continue;
        }
        const char *pj = tr->edges[e].properties_json;
        if (!pj) {
            continue;
        }
        const char *args = strstr(pj, "\"args\"");
        if (!args) {
            continue;
        }
        const char *open = strchr(args, '[');
        if (!open) {
            continue;
        }
        int depth = 0;
        const char *p = open;
        for (; *p; p++) {
            if (*p == '[') {
                depth++;
            } else if (*p == ']') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
        }
        *out_len = (size_t)(p - open);
        return open;
    }
    return NULL;
}

static yyjson_mut_val *bfs_to_json_array(yyjson_mut_doc *doc, cbm_traverse_result_t *tr,
                                         bool risk_labels, bool include_tests,
                                         double min_confidence, bool data_flow) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    const char *root_name = tr->root.name;
    for (int i = 0; i < tr->visited_count; i++) {
        const char *fp = tr->visited[i].node.file_path;
        bool test = is_test_file(fp);
        if (!include_tests && test) {
            continue;
        }
        /* Confidence filter: keep the root always; otherwise require at
         * least one connecting edge with confidence >= threshold. */
        if (min_confidence > 0.0) {
            const char *nname = tr->visited[i].node.name;
            bool is_root = (root_name && nname && strcmp(nname, root_name) == 0);
            if (!is_root && !node_has_strong_edge(tr, nname, min_confidence)) {
                continue;
            }
        }
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "name",
                               tr->visited[i].node.name ? tr->visited[i].node.name : "");
        yyjson_mut_obj_add_str(
            doc, item, "qualified_name",
            tr->visited[i].node.qualified_name ? tr->visited[i].node.qualified_name : "");
        yyjson_mut_obj_add_int(doc, item, "hop", tr->visited[i].hop);
        /* Definition location: an agent asked "who calls X, with file and
         * line" needed a second tool call per caller without these (Codex
         * pilot, T2). */
        if (fp && fp[0]) {
            yyjson_mut_obj_add_str(doc, item, "file", fp);
        }
        if (tr->visited[i].node.start_line > 0) {
            yyjson_mut_obj_add_int(doc, item, "start_line", tr->visited[i].node.start_line);
            yyjson_mut_obj_add_int(doc, item, "end_line", tr->visited[i].node.end_line);
        }
        if (risk_labels) {
            yyjson_mut_obj_add_str(doc, item, "risk",
                                   cbm_risk_label(cbm_hop_to_risk(tr->visited[i].hop)));
        }
        if (test) {
            yyjson_mut_obj_add_bool(doc, item, "is_test", true);
        }
        /* data_flow mode promises argument expressions at each call site; surface
         * the CALLS edge's serialized args array as a raw JSON value (#514). */
        if (data_flow) {
            size_t alen = 0;
            const char *args = bfs_edge_args_for_hop(tr, tr->visited[i].node.id, &alen);
            if (args && alen > 0) {
                yyjson_mut_val *av = yyjson_mut_rawn(doc, args, alen);
                if (av) {
                    yyjson_mut_obj_add_val(doc, item, "args", av);
                }
            }
        }
        yyjson_mut_arr_add_val(arr, item);
    }
    return arr;
}

/* True when every visited node carrying this NAME was removed by the
 * include_tests filter. Conservative on purpose: if any non-test node shares
 * the name, the edge is kept.
 *
 * FORK-LOCAL. This payload names callers in two places — the callers/callees
 * arrays (filtered by is_test_file) and the caller_edges/callee_edges arrays
 * (previously not). Before test-directory detection was fixed, both leaked a
 * tests/ caller and at least AGREED; filtering only the first would leave the
 * edge array naming a caller absent from the caller list, which is worse than
 * either. Edges are keyed by name here, exactly as the node arrays are, so
 * dropping by name keeps the two consistent. */
static bool bfs_name_is_filtered_test(const cbm_traverse_result_t *tr, const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    bool saw = false;
    for (int i = 0; i < tr->visited_count; i++) {
        const char *nn = tr->visited[i].node.name;
        if (!nn || strcmp(nn, name) != 0) {
            continue;
        }
        saw = true;
        if (!is_test_file(tr->visited[i].node.file_path)) {
            return false; /* a non-test node owns this name */
        }
    }
    return saw;
}

/* Serialize the edge list from a BFS traversal, optionally filtered by
 * min_confidence. Surfacing edges (with type/confidence) lets agents reason
 * about resolution quality, not just connectivity. */
/* The node (root or visited) with this id, or NULL. */
static const cbm_node_t *bfs_node_by_id(const cbm_traverse_result_t *tr, int64_t id) {
    if (tr->root.id == id) {
        return &tr->root;
    }
    for (int i = 0; i < tr->visited_count; i++) {
        if (tr->visited[i].node.id == id) {
            return &tr->visited[i].node;
        }
    }
    return NULL;
}

/* Test filter keyed on the edge's actual endpoint. The name-keyed variant kept
 * an edge from tests/foo.cpp whenever some non-test node shared the caller's
 * name (a common C++ situation), which is how a trace with include_tests=false
 * still listed test edges in caller_edges (Codex pilot, T2). */
static bool bfs_endpoint_is_test(const cbm_traverse_result_t *tr, int64_t id, const char *name) {
    const cbm_node_t *n = bfs_node_by_id(tr, id);
    if (n) {
        return is_test_file(n->file_path);
    }
    return bfs_name_is_filtered_test(tr, name);
}

/* Read the call-site line pass_calls splices into CALLS edge properties
 * (,"line":N). 0 when absent. */
static int edge_props_line(const char *props) {
    const char *p = props ? strstr(props, "\"line\":") : NULL;
    if (!p) {
        return 0;
    }
    long v = strtol(p + SLEN("\"line\":"), NULL, 10);
    return v > 0 && v < INT_MAX ? (int)v : 0;
}

/* Copy the resolver "strategy" string out of edge properties; false if none. */
static bool edge_props_strategy(const char *props, char *out, size_t outsz) {
    const char *p = props ? strstr(props, "\"strategy\":\"") : NULL;
    if (!p) {
        return false;
    }
    p += SLEN("\"strategy\":\"");
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < outsz) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static double edge_props_confidence(const char *props) {
    const char *p = props ? strstr(props, "\"confidence\":") : NULL;
    if (!p) {
        return 1.0;
    }
    double v = strtod(p + SLEN("\"confidence\":"), NULL);
    return v > 0.0 && v <= 1.0 ? v : 1.0;
}

/* "Ns::Class::method" -> "Ns.Class.method" (graph QNs are dot-separated), for
 * the dot-boundary suffix lookup. Returns a heap string; NULL when the input
 * has no scope separator. */
static char *scoped_name_to_qn_suffix(const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    size_t len = strlen(name);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == ':' && name[i + 1] == ':') {
            out[o++] = '.';
            i++;
        } else {
            out[o++] = name[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* Resolve a scoped name ("pcpp::Packet::addLayer", "Packet.addLayer") to graph
 * nodes. Graph QNs are "<project>.<path>.<file stem>.<Class>.<method>" and
 * carry no C++ namespace, so after the "::" -> "." mapping the lookup tries
 * the whole suffix, then drops leading scopes one at a time until only
 * "<Class>.<member>" is left. First non-empty tier wins. */
static int find_nodes_by_scoped_name(cbm_store_t *store, const char *project, const char *name,
                                     cbm_node_t **out, int *count) {
    *out = NULL;
    *count = 0;
    char *suffix = scoped_name_to_qn_suffix(name);
    if (!suffix) {
        return CBM_STORE_ERR;
    }
    const char *cur = suffix;
    for (;;) {
        cbm_store_find_nodes_by_qn_suffix(store, project, cur, out, count);
        if (*count > 0) {
            break;
        }
        const char *dot = strchr(cur, '.');
        if (!dot || !strchr(dot + 1, '.')) {
            break; /* two components left — nothing more to strip */
        }
        cur = dot + 1;
    }
    free(suffix);
    return *count > 0 ? CBM_STORE_OK : CBM_STORE_ERR;
}

/* Serialize the edge list from a BFS traversal, optionally filtered by
 * min_confidence. Each edge carries the resolver's confidence and strategy
 * plus the call-site line and endpoint files, so a caller list is verifiable
 * from the reply alone: open from_file at line and the call is there. */
static yyjson_mut_val *bfs_edges_to_json_array(yyjson_mut_doc *doc, const cbm_traverse_result_t *tr,
                                               double min_confidence, bool include_tests) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < tr->edge_count; i++) {
        const cbm_edge_info_t *e = &tr->edges[i];
        if (e->confidence < min_confidence) {
            continue;
        }
        if (!include_tests && (bfs_endpoint_is_test(tr, e->source_id, e->from_name) ||
                               bfs_endpoint_is_test(tr, e->target_id, e->to_name))) {
            continue;
        }
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "from", e->from_name ? e->from_name : "");
        yyjson_mut_obj_add_str(doc, item, "to", e->to_name ? e->to_name : "");
        yyjson_mut_obj_add_str(doc, item, "type", e->type ? e->type : "");
        yyjson_mut_obj_add_real(doc, item, "confidence", e->confidence);
        char strategy[CBM_SZ_64];
        if (edge_props_strategy(e->properties_json, strategy, sizeof(strategy))) {
            yyjson_mut_obj_add_strcpy(doc, item, "strategy", strategy);
        }
        const cbm_node_t *from = bfs_node_by_id(tr, e->source_id);
        if (from && from->file_path && from->file_path[0]) {
            yyjson_mut_obj_add_str(doc, item, "from_file", from->file_path);
        }
        int line = edge_props_line(e->properties_json);
        if (line > 0) {
            yyjson_mut_obj_add_int(doc, item, "line", line);
        }
        yyjson_mut_arr_add_val(arr, item);
    }
    return arr;
}

static char *snippet_suggestions(const char *input, cbm_node_t *nodes, int count);

/* Rank a candidate for name resolution. The label tier (callable > class-like >
 * module/file) is the primary key; WITHIN a tier the larger definition by line
 * span wins. In practice the .c-over-.h and C-main-over-shell-main preferences
 * come primarily from span (the real definition has the larger body), since the
 * competing matches usually share a tier — no file extension is hardcoded.
 * Consequence: two same-tier candidates with equal span tie and are reported
 * ambiguous (see pick_resolved_node) rather than guessed. */
enum {
    RES_RANK_CALLABLE = 2,     /* Function / Method */
    RES_RANK_OTHER = 1,        /* Class / Struct / etc. */
    RES_RANK_MODULE = 0,       /* Module / File */
    RES_LABEL_WEIGHT = 1000000 /* label tier dominates span */
};
static long node_resolution_score(const cbm_node_t *n) {
    long label_rank = RES_RANK_MODULE;
    if (n->label) {
        if (strcmp(n->label, "Function") == 0 || strcmp(n->label, "Method") == 0) {
            label_rank = RES_RANK_CALLABLE;
        } else if (strcmp(n->label, "Module") != 0 && strcmp(n->label, "File") != 0) {
            label_rank = RES_RANK_OTHER;
        }
    }
    /* Tie rule (CBM_MACRO_QN_SUFFIX): a name that is both a definition and a C
     * macro -- a typedef or enumerator next to its rename macro -- resolves to
     * the definition. The macro scores just under every other definition, and
     * still above Module/File; without this a one-line typedef and its macro
     * tie on span and the name reads as ambiguous. */
    size_t qn_len = n->qualified_name ? strlen(n->qualified_name) : 0;
    size_t fence_len = sizeof(CBM_MACRO_QN_SUFFIX) - SKIP_ONE;
    if (label_rank == RES_RANK_OTHER && qn_len > fence_len &&
        strcmp(n->qualified_name + qn_len - fence_len, CBM_MACRO_QN_SUFFIX) == 0) {
        return RES_RANK_OTHER * (long)RES_LABEL_WEIGHT - SKIP_ONE;
    }
    long span = (long)n->end_line - (long)n->start_line;
    if (span < 0) {
        span = 0;
    }
    return label_rank * (long)RES_LABEL_WEIGHT + span;
}

/* A "real" callable definition: a Function/Method node with a non-empty body
 * span (end_line > start_line). A body-less node (start_line == end_line) is an
 * ambient declaration / signature stub — e.g. a TypeScript `.d.ts` declaration
 * — which is a *fragment* of one logical symbol, not a distinct definition. The
 * distinction lets pick_resolved_node union a stub with its real implementation
 * (#546) while still treating two genuinely-different same-named functions as
 * ambiguous rather than conflating their caller sets. */
static bool node_is_real_callable_def(const cbm_node_t *n) {
    if (!n->label) {
        return false;
    }
    if (strcmp(n->label, "Function") != 0 && strcmp(n->label, "Method") != 0) {
        return false;
    }
    return (long)n->end_line - (long)n->start_line > 0;
}

/* Pick the best-resolving node among name matches. Sets *ambiguous when the
 * matches can't be reduced to one logical symbol, so resolution never silently
 * traces (or conflates) the wrong same-named node:
 *   1. the top score is shared by >1 candidate (a genuine rank/span tie), or
 *   2. two or more *real* callable definitions share the name — distinct
 *      implementations, not a definition plus its body-less stub(s).
 * Rule 2 completes rule 1: without it, two same-named functions whose bodies
 * differ in length score differently, dodge the tie, and get their caller sets
 * unioned by bfs_union_same_name (#546) into one confidently-conflated answer.
 * Body-less .d.ts stubs still union with their implementation (#650). */
static int pick_resolved_node(const cbm_node_t *nodes, int count, bool *ambiguous) {
    *ambiguous = false;
    if (count <= 1) {
        return 0;
    }
    int best = 0;
    long best_score = node_resolution_score(&nodes[0]);
    for (int i = 1; i < count; i++) {
        long s = node_resolution_score(&nodes[i]);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }
    int top_count = 0;
    int real_def_count = 0;
    for (int i = 0; i < count; i++) {
        if (node_resolution_score(&nodes[i]) == best_score) {
            top_count++;
        }
        if (node_is_real_callable_def(&nodes[i])) {
            real_def_count++;
        }
    }
    if (real_def_count > 1) {
        *ambiguous = true;
    }
    if (top_count > 1) {
        *ambiguous = true;
    }
    return best;
}

static bool trace_accept_production_file(const char *file_path, void *) {
    return !is_test_file(file_path);
}

/* BFS from EVERY node sharing the resolved name and merge the results, so the
 * caller/callee set is complete even when one logical symbol is represented by
 * more than one graph node — e.g. a real .ts implementation plus an ambient
 * .d.ts stub, whose inbound CALLS edges are otherwise split across the two
 * nodes and silently truncated by tracing only one (#546). visited hops are
 * deduped by node id; edges are concatenated. Ownership of all heap fields
 * transfers into *out, freed by cbm_store_traverse_free. */
static int bfs_union_same_name(cbm_store_t *store, const cbm_node_t *nodes, int node_count,
                               const char *direction, const char **edge_types, int edge_type_count,
                               int depth, bool include_tests, cbm_traverse_result_t *out) {
    memset(out, 0, sizeof(*out));
    int vcap = 0, ecap = 0;
    for (int k = 0; k < node_count; k++) {
        cbm_traverse_result_t tr = {0};
        int rc = cbm_store_bfs_filtered(store, nodes[k].id, direction, edge_types, edge_type_count,
                                        depth, MCP_BFS_LIMIT, MCP_BFS_SCAN_LIMIT,
                                        include_tests ? nullptr : trace_accept_production_file,
                                        nullptr, &tr);
        if (rc != CBM_STORE_OK) {
            cbm_store_traverse_free(&tr);
            return rc;
        }
        out->truncated = out->truncated || tr.truncated;
        for (int i = 0; i < tr.visited_count; i++) {
            bool dup = false;
            for (int j = 0; j < out->visited_count; j++) {
                if (out->visited[j].node.id == tr.visited[i].node.id) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (out->visited_count >= vcap) {
                vcap = vcap ? vcap * 2 : 8;
                out->visited =
                    (cbm_node_hop_t *)safe_realloc(out->visited, vcap * sizeof(cbm_node_hop_t));
            }
            out->visited[out->visited_count++] = tr.visited[i];
            memset(&tr.visited[i], 0, sizeof(tr.visited[i])); /* ownership moved */
        }
        for (int i = 0; i < tr.edge_count; i++) {
            if (out->edge_count >= ecap) {
                ecap = ecap ? ecap * 2 : 8;
                out->edges =
                    (cbm_edge_info_t *)safe_realloc(out->edges, ecap * sizeof(cbm_edge_info_t));
            }
            out->edges[out->edge_count++] = tr.edges[i];
            memset(&tr.edges[i], 0, sizeof(tr.edges[i])); /* ownership moved */
        }
        cbm_store_traverse_free(&tr); /* frees only the un-moved (root + dup) fields */
    }
    return CBM_STORE_OK;
}

/* A from_function query is a point-to-point problem. Walking inbound from the
 * destination first makes unrelated callers consume the shared result cap and
 * grows with repository fan-in. Search forward from the resolved source
 * instead: work is proportional to its reachable CALLS neighborhood and is
 * independently bounded by max_work, regardless of total graph size. */
typedef struct {
    cbm_node_t node; /* owned */
    int parent;
    int depth;
    std::string edge_properties;
    double edge_confidence;
} trace_path_step_t;

static void trace_path_steps_free(std::vector<trace_path_step_t> *steps) {
    for (auto &step : *steps) {
        free_node_contents(&step.node);
    }
    steps->clear();
}

bool cbm_path_within_root(const char *root_path, const char *abs_path); /* defined below */

struct trace_file_freshness_t {
    std::string file;
    const char *state;
    const char *reason;
};

struct trace_hash_budget_t {
    size_t bytes = 0;
    struct timespec started = {};
};

static int64_t trace_stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * (int64_t)CBM_NSEC_PER_SEC) +
           (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * (int64_t)CBM_NSEC_PER_SEC;
#else
    return ((int64_t)st->st_mtim.tv_sec * (int64_t)CBM_NSEC_PER_SEC) + (int64_t)st->st_mtim.tv_nsec;
#endif
}

static bool trace_sha256_valid(const char *sha) {
    if (!sha || strlen(sha) != CBM_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; ++i) {
        if (!isxdigit((unsigned char)sha[i])) {
            return false;
        }
    }
    return true;
}

static bool trace_relative_file_safe(const char *file) {
    if (!file || !file[0] || file[0] == '/' || file[0] == '\\' ||
        (isalpha((unsigned char)file[0]) && file[1] == ':')) {
        return false;
    }
    const char *part = file;
    for (const char *p = file;; ++p) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            if ((size_t)(p - part) == 2 && part[0] == '.' && part[1] == '.') {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            part = p + 1;
        }
    }
    return true;
}

static bool trace_hash_time_exhausted(const trace_hash_budget_t *budget) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed = (int64_t)(now.tv_sec - budget->started.tv_sec) * 1000LL +
                      (int64_t)(now.tv_nsec - budget->started.tv_nsec) / 1000000LL;
    return elapsed > MCP_PATH_HASH_TIMEOUT_MS;
}

struct trace_fd_stat_t {
    int64_t mtime_ns = 0;
    int64_t size = -1;
    bool regular = false;
#ifndef _WIN32
    dev_t dev = 0;
    ino_t ino = 0;
#endif
};

static bool trace_fstat(FILE *fp, trace_fd_stat_t *out) {
#ifdef _WIN32
    struct _stat64 st = {};
    if (_fstat64(cbm_fileno(fp), &st) != 0) {
        return false;
    }
    out->mtime_ns = (int64_t)st.st_mtime * (int64_t)CBM_NSEC_PER_SEC;
    out->size = st.st_size;
    out->regular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st = {};
    if (fstat(cbm_fileno(fp), &st) != 0) {
        return false;
    }
    out->mtime_ns = trace_stat_mtime_ns(&st);
    out->size = st.st_size;
    out->regular = S_ISREG(st.st_mode);
    out->dev = st.st_dev;
    out->ino = st.st_ino;
#endif
    return true;
}

static bool trace_fd_same_file(const trace_fd_stat_t &before, const trace_fd_stat_t &after) {
    if (before.mtime_ns != after.mtime_ns || before.size != after.size || !after.regular) {
        return false;
    }
#ifndef _WIN32
    return before.dev == after.dev && before.ino == after.ino;
#else
    return true;
#endif
}

static trace_file_freshness_t trace_check_path_file(cbm_store_t *store, const char *project,
                                                    const char *project_root, const char *file,
                                                    trace_hash_budget_t *budget) {
    trace_file_freshness_t out = {file ? file : "", "unknown", "no_file_path"};
    if (!trace_relative_file_safe(file)) {
        out.reason = file && file[0] ? "outside_root" : "no_file_path";
        return out;
    }
    if (!project_root || !project_root[0]) {
        out.reason = "project_root_unavailable";
        return out;
    }
    char stored_sha[CBM_SHA256_HEX_LEN + 1];
    int64_t stored_mtime = 0;
    int64_t stored_size = 0;
    int store_rc = cbm_store_get_file_hash(store, project, file, stored_sha, sizeof(stored_sha),
                                           &stored_mtime, &stored_size);
    if (store_rc == CBM_STORE_NOT_FOUND) {
        out.reason = "no_index_metadata";
        return out;
    }
    if (store_rc != CBM_STORE_OK) {
        out.reason = "store_error";
        return out;
    }
    if (!trace_sha256_valid(stored_sha) || stored_size < 0 || stored_mtime <= 0) {
        out.reason = "invalid_index_hash_or_metadata";
        return out;
    }

    std::string abs = std::string(project_root) + "/" + file;
    struct stat before = {};
    if (stat(abs.c_str(), &before) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            out.state = "stale";
            out.reason = "deleted";
        } else {
            out.reason = "stat_failed";
        }
        return out;
    }
    if (!cbm_path_within_root(project_root, abs.c_str())) {
        out.reason = "outside_root";
        return out;
    }
    if (!S_ISREG(before.st_mode)) {
        out.reason = "unsupported_file";
        return out;
    }
    if ((uint64_t)stored_size > trace_hash_file_max ||
        budget->bytes + (size_t)stored_size > trace_hash_request_max ||
        trace_hash_time_exhausted(budget)) {
        out.reason = "hash_budget_exhausted";
        return out;
    }

    FILE *fp = cbm_fopen(abs.c_str(), "rb");
    if (!fp) {
        out.reason = "open_failed";
        return out;
    }
    trace_fd_stat_t opened = {};
    if (!trace_fstat(fp, &opened)) {
        fclose(fp);
        out.reason = "fstat_failed";
        return out;
    }
    if (!opened.regular) {
        fclose(fp);
        out.reason = "unsupported_file";
        return out;
    }
#ifndef _WIN32
    if (opened.dev != before.st_dev || opened.ino != before.st_ino) {
        fclose(fp);
        out.reason = "file_replaced_before_hash";
        return out;
    }
#endif
    if (opened.mtime_ns != stored_mtime || opened.size != stored_size) {
        fclose(fp);
        out.state = "stale";
        out.reason = "metadata_mismatch";
        return out;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    unsigned char chunk[CBM_SZ_64K];
    size_t read_total = 0;
    bool timed_out = false;
    size_t n = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        read_total += n;
        budget->bytes += n;
        cbm_sha256_update(&hash, chunk, n);
        if (read_total > (size_t)stored_size || budget->bytes > trace_hash_request_max ||
            trace_hash_time_exhausted(budget)) {
            timed_out = true;
            break;
        }
    }
    bool read_failed = ferror(fp) != 0;
    trace_fd_stat_t finished = {};
    bool finish_stat_ok = trace_fstat(fp, &finished);
    fclose(fp);
    if (timed_out) {
        out.reason = "hash_budget_exhausted";
        return out;
    }
    if (read_failed) {
        out.reason = "read_failed";
        return out;
    }
    if (!finish_stat_ok || !trace_fd_same_file(opened, finished) ||
        read_total != (size_t)stored_size) {
        out.reason = "file_changed_during_hash";
        return out;
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    char current_sha[CBM_SHA256_HEX_LEN + 1];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        current_sha[i * 2] = hex[digest[i] >> 4];
        current_sha[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    current_sha[CBM_SHA256_HEX_LEN] = '\0';
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; ++i) {
        if (current_sha[i] != (char)tolower((unsigned char)stored_sha[i])) {
            out.state = "stale";
            out.reason = "content_hash_mismatch";
            return out;
        }
    }
    out.state = "match";
    out.reason = "sha256_and_metadata_match";
    return out;
}

static int trace_direct_path_to_json(cbm_store_t *store, const char *project, const char *entry,
                                     const cbm_node_t *targets, int target_count, int depth,
                                     int max_work, double min_confidence, bool include_tests,
                                     bool risk_labels, const char *freshness_mode,
                                     yyjson_mut_doc *doc, yyjson_mut_val *root) {
    bool index_only = freshness_mode && strcmp(freshness_mode, "index_only") == 0;
    yyjson_mut_obj_add_strcpy(doc, root, "from_function", entry);
    yyjson_mut_obj_add_strcpy(doc, root, "freshness_mode", index_only ? "index_only" : "strict");
    yyjson_mut_obj_add_bool(doc, root, "current_shortest_path_validated", false);
    yyjson_mut_obj_add_strcpy(doc, root, "traversal_strategy", "targeted_forward_bfs");
    yyjson_mut_obj_add_int(doc, root, "traversal_work_limit", max_work);
    yyjson_mut_obj_add_strcpy(
        doc, root, "path_note",
        "One shortest observed path in a bounded forward search of indexed CALLS edges. "
        "This is not a current-shortest-path claim: off-path edits are not checked. No path does "
        "not prove disconnection; depth, work, coverage and filters limit this result.");

    cbm_node_t *sources = nullptr;
    int source_count = 0;
    int resolve_rc = cbm_store_find_nodes_by_name(store, project, entry, &sources, &source_count);
    if (resolve_rc == CBM_STORE_ERR) {
        yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
        return CBM_STORE_ERR;
    }
    if (source_count == 0) {
        cbm_node_t exact = {};
        resolve_rc = cbm_store_find_node_by_qn(store, project, entry, &exact);
        if (resolve_rc == CBM_STORE_OK) {
            sources = (cbm_node_t *)malloc(sizeof(cbm_node_t));
            if (sources) {
                sources[0] = exact;
                source_count = 1;
            } else {
                free_node_contents(&exact);
            }
        } else if (resolve_rc == CBM_STORE_ERR) {
            yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
            return CBM_STORE_ERR;
        }
    }
    if (source_count == 0 && (strstr(entry, "::") || strchr(entry, '.'))) {
        find_nodes_by_scoped_name(store, project, entry, &sources, &source_count);
    }
    bool ambiguous = false;
    int selected = source_count ? pick_resolved_node(sources, source_count, &ambiguous) : 0;
    if (source_count == 0 || ambiguous) {
        yyjson_mut_obj_add_strcpy(doc, root, "error",
                                  source_count == 0
                                      ? "from_function not found"
                                      : "from_function is ambiguous; use a qualified name");
        cbm_store_free_nodes(sources, source_count);
        return CBM_STORE_OK;
    }

    auto is_target = [&](int64_t id) {
        for (int i = 0; i < target_count; ++i) {
            if (targets[i].id == id) {
                return true;
            }
        }
        return false;
    };

    std::vector<trace_path_step_t> steps;
    trace_path_step_t first = {};
    first.node = sources[selected];
    memset(&sources[selected], 0, sizeof(sources[selected])); /* ownership moved */
    first.parent = -1;
    first.depth = 0;
    first.edge_confidence = 1.0;
    steps.push_back(std::move(first));
    cbm_store_free_nodes(sources, source_count);

    std::unordered_map<int64_t, int> seen;
    seen[steps[0].node.id] = 0;
    int examined_edges = 0;
    int finish = -1;
    bool truncated = false;
    if (include_tests || !is_test_file(steps[0].node.file_path)) {
        if (is_target(steps[0].node.id)) {
            finish = 0;
        }
        for (size_t cursor = 0; cursor < steps.size() && finish < 0 && !truncated; ++cursor) {
            if (steps[cursor].depth >= depth) {
                continue;
            }
            cbm_edge_t *outgoing = nullptr;
            int outgoing_count = 0;
            int edge_rc = cbm_store_find_edges_by_source_type(store, steps[cursor].node.id, "CALLS",
                                                              &outgoing, &outgoing_count);
            if (edge_rc != CBM_STORE_OK) {
                yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
                trace_path_steps_free(&steps);
                return CBM_STORE_ERR;
            }
            if (outgoing_count > 1) {
                std::sort(outgoing, outgoing + outgoing_count,
                          [](const cbm_edge_t &a, const cbm_edge_t &b) {
                              if (a.target_id != b.target_id) {
                                  return a.target_id < b.target_id;
                              }
                              return a.id < b.id;
                          });
            }
            for (int i = 0; i < outgoing_count; ++i) {
                if (examined_edges >= max_work) {
                    truncated = true; /* this unexamined edge proves the bound was reached */
                    break;
                }
                examined_edges++;
                const cbm_edge_t &edge = outgoing[i];
                double confidence = edge_props_confidence(edge.properties_json);
                if (confidence < min_confidence || seen.find(edge.target_id) != seen.end()) {
                    continue;
                }
                cbm_node_t next = {};
                int node_rc = cbm_store_find_node_by_id(store, edge.target_id, &next);
                if (node_rc == CBM_STORE_ERR) {
                    yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
                    cbm_store_free_edges(outgoing, outgoing_count);
                    trace_path_steps_free(&steps);
                    return CBM_STORE_ERR;
                }
                if (node_rc != CBM_STORE_OK || (!include_tests && is_test_file(next.file_path))) {
                    free_node_contents(&next);
                    continue;
                }
                trace_path_step_t step = {};
                step.node = next;
                step.parent = (int)cursor;
                step.depth = steps[cursor].depth + 1;
                step.edge_properties = edge.properties_json ? edge.properties_json : "{}";
                step.edge_confidence = confidence;
                int step_index = (int)steps.size();
                seen[edge.target_id] = step_index;
                steps.push_back(std::move(step));
                if (is_target(edge.target_id)) {
                    finish = step_index;
                    break;
                }
            }
            cbm_store_free_edges(outgoing, outgoing_count);
        }
    }

    std::vector<int> selected_steps;
    if (finish >= 0) {
        for (int step = finish; step >= 0; step = steps[step].parent) {
            selected_steps.push_back(step);
        }
        std::reverse(selected_steps.begin(), selected_steps.end());
    }

    yyjson_mut_obj_add_int(doc, root, "traversal_examined_edges", examined_edges);
    yyjson_mut_obj_add_int(doc, root, "traversal_visited_nodes", (int)steps.size());
    yyjson_mut_obj_add_bool(doc, root, "traversal_truncated", truncated);

    if (finish >= 0 && !index_only) {
        std::string project_root;
        cbm_project_t proj = {};
        if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK && proj.root_path) {
            project_root = proj.root_path;
        }
        cbm_project_free_fields(&proj);

        std::vector<std::string> files;
        std::unordered_map<std::string, bool> seen_files;
        for (int selected_step : selected_steps) {
            const char *file = steps[selected_step].node.file_path;
            std::string key = file ? file : "";
            if (seen_files.emplace(key, true).second) {
                files.push_back(std::move(key));
            }
        }
        trace_hash_budget_t budget;
        cbm_clock_gettime(CLOCK_MONOTONIC, &budget.started);
        std::vector<trace_file_freshness_t> checks;
        bool all_match = true;
        for (const auto &file : files) {
            checks.push_back(trace_check_path_file(
                store, project, project_root.empty() ? nullptr : project_root.c_str(), file.c_str(),
                &budget));
            all_match = all_match && strcmp(checks.back().state, "match") == 0;
        }
        yyjson_mut_val *freshness = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, freshness, "mode", "strict");
        yyjson_mut_obj_add_strcpy(doc, freshness, "basis", "sha256_mtime_ns_size");
        yyjson_mut_obj_add_int(doc, freshness, "files_checked", (int)checks.size());
        yyjson_mut_obj_add_uint(doc, freshness, "bytes_hashed", budget.bytes);
        yyjson_mut_obj_add_bool(doc, freshness, "all_path_files_match", all_match);
        yyjson_mut_val *file_results = yyjson_mut_arr(doc);
        for (const auto &check : checks) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, item, "file", check.file.c_str());
            yyjson_mut_obj_add_strcpy(doc, item, "state", check.state);
            yyjson_mut_obj_add_strcpy(doc, item, "reason", check.reason);
            yyjson_mut_arr_add_val(file_results, item);
        }
        yyjson_mut_obj_add_val(doc, freshness, "files", file_results);
        yyjson_mut_obj_add_val(doc, root, "index_freshness", freshness);
        if (!all_match) {
            yyjson_mut_obj_add_bool(doc, root, "indexed_path_found", true);
            yyjson_mut_obj_add_bool(doc, root, "indexed_path_suppressed", true);
            yyjson_mut_obj_add_bool(doc, root, "path_returned", false);
            yyjson_mut_obj_add_bool(doc, root, "current_source_relationship_supported", false);
            yyjson_mut_obj_add_strcpy(
                doc, root, "error",
                "Indexed call path withheld because path-file freshness is stale or unknown. "
                "Re-index the project or explicitly request freshness_mode=index_only for "
                "historical graph evidence.");
            trace_path_steps_free(&steps);
            return CBM_STORE_OK;
        }
        yyjson_mut_obj_add_bool(doc, root, "path_files_match_index_snapshot", true);
        yyjson_mut_obj_add_bool(doc, root, "current_source_relationship_supported", true);
    } else if (index_only) {
        yyjson_mut_obj_add_bool(doc, root, "historical_index_only", true);
        yyjson_mut_obj_add_bool(doc, root, "current_source_relationship_supported", false);
    } else if (finish < 0) {
        yyjson_mut_obj_add_bool(doc, root, "current_source_relationship_supported", false);
    }

    yyjson_mut_obj_add_bool(doc, root, "path_found", finish >= 0);
    yyjson_mut_val *path = yyjson_mut_arr(doc);
    yyjson_mut_val *edges = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "path", path);
    yyjson_mut_obj_add_val(doc, root, "caller_edges", edges);

    if (finish >= 0) {
        for (size_t i = 0; i < selected_steps.size(); ++i) {
            const trace_path_step_t &step = steps[selected_steps[i]];
            const cbm_node_t &node = step.node;
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, item, "name", node.name ? node.name : "");
            yyjson_mut_obj_add_strcpy(doc, item, "qualified_name",
                                      node.qualified_name ? node.qualified_name : "");
            yyjson_mut_obj_add_strcpy(doc, item, "file", node.file_path ? node.file_path : "");
            yyjson_mut_obj_add_int(doc, item, "start_line", node.start_line);
            yyjson_mut_obj_add_int(doc, item, "end_line", node.end_line);
            if (risk_labels) {
                yyjson_mut_obj_add_strcpy(
                    doc, item, "risk",
                    cbm_risk_label(cbm_hop_to_risk((int)(selected_steps.size() - i - 1))));
            }
            yyjson_mut_arr_add_val(path, item);
            if (i == 0) {
                continue;
            }
            const trace_path_step_t &to = step;
            const trace_path_step_t &from = steps[selected_steps[i - 1]];
            yyjson_mut_val *edge = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, edge, "from", from.node.name ? from.node.name : "");
            yyjson_mut_obj_add_strcpy(doc, edge, "to", to.node.name ? to.node.name : "");
            yyjson_mut_obj_add_uint(doc, edge, "from_step", i - 1);
            yyjson_mut_obj_add_uint(doc, edge, "to_step", i);
            yyjson_mut_obj_add_strcpy(doc, edge, "type", "CALLS");
            yyjson_mut_obj_add_real(doc, edge, "confidence", to.edge_confidence);
            yyjson_mut_obj_add_strcpy(doc, edge, "from_file",
                                      from.node.file_path ? from.node.file_path : "");
            char strategy[CBM_SZ_64];
            if (edge_props_strategy(to.edge_properties.c_str(), strategy, sizeof(strategy))) {
                yyjson_mut_obj_add_strcpy(doc, edge, "strategy", strategy);
            }
            int line = edge_props_line(to.edge_properties.c_str());
            if (line > 0) {
                yyjson_mut_obj_add_int(doc, edge, "line", line);
            }
            yyjson_mut_arr_add_val(edges, edge);
        }
    }
    trace_path_steps_free(&steps);
    return CBM_STORE_OK;
}

/* Clamp a client-supplied traversal depth to the MCP ceiling (cbm_mcp_max_depth),
 * WARN-logging when it does so — never a silent truncation (#887). An unclamped
 * `depth` would drive the shared cbm_store_bfs to an arbitrary hop count. */
static int clamp_mcp_depth(int depth, const char *tool) {
    int cap = cbm_mcp_max_depth();
    if (depth > cap) {
        char req_buf[16];
        char cap_buf[16];
        snprintf(req_buf, sizeof(req_buf), "%d", depth);
        snprintf(cap_buf, sizeof(cap_buf), "%d", cap);
        cbm_log_warn("mcp.depth_capped", "tool", tool, "requested", req_buf, "cap", cap_buf);
        return cap;
    }
    return depth;
}

static void trace_add_source(cbm_mcp_server_t *srv, const char *project, yyjson_mut_doc *doc,
                             yyjson_mut_val *root, const char *args);

static char *handle_trace_call_path_impl(cbm_mcp_server_t *srv, const char *args) {
    char *func_name = cbm_mcp_get_string_arg(args, "function_name");
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    char *direction = cbm_mcp_get_string_arg(args, "direction");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *param_name = cbm_mcp_get_string_arg(args, "parameter_name");
    int depth = cbm_mcp_get_int_arg(args, "depth", MCP_DEFAULT_DEPTH);
    depth = clamp_mcp_depth(depth, "trace_call_path");
    bool risk_labels = cbm_mcp_get_bool_arg(args, "risk_labels");
    bool include_tests = cbm_mcp_get_bool_arg(args, "include_tests");
    /* Filter results to edges with CALLS-edge confidence >= threshold. 0
     * (default) preserves prior behavior; ~0.8 cuts most fuzzy-tail noise. */
    double min_confidence = cbm_mcp_get_double_arg(args, "min_confidence", 0.0);

    if (!func_name) {
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return cbm_mcp_text_result("function_name is required", true);
    }
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return _res;
    }

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        return not_indexed;
    }

    if (!direction) {
        direction = heap_strdup("both");
    }

    /* Find the node by name. If the bare-name lookup misses, fall back to
     * qualified_name so callers passing a fully-qualified identifier (which
     * the not-found hint actually recommends) hit the same path. The QN
     * lookup uses the same scan_node helper as the bare lookup, so the
     * shallow struct copy below transfers ownership of the strdup'd string
     * fields cleanly and cbm_store_free_nodes will free them. */
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_name(store, project, func_name, &nodes, &node_count);

    if (node_count == 0) {
        cbm_node_t qn_node = {0};
        if (cbm_store_find_node_by_qn(store, project, func_name, &qn_node) == CBM_STORE_OK) {
            /* A zero-row name lookup still returns its allocated (empty)
             * array; release it before the fallback replaces the pointer. */
            cbm_store_free_nodes(nodes, 0);
            nodes = (__typeof__(nodes))malloc(sizeof(cbm_node_t));
            if (nodes) {
                nodes[0] = qn_node;
                node_count = 1;
            } else {
                free_node_contents(&qn_node);
            }
        }
    }

    /* Third tier: a scoped name. C++ agents write Class::method (both studies
     * logged this exact failure); graph QNs are dot-separated, so map "::" to
     * "." and match on a dot-boundary suffix. A plain partial QN such as
     * "Layer.parseNextLayer" takes the same path. */
    if (node_count == 0 && (strstr(func_name, "::") || strchr(func_name, '.'))) {
        find_nodes_by_scoped_name(store, project, func_name, &nodes, &node_count);
    }

    if (node_count == 0) {
        enum { HINT_BUF_SZ = 512 };
        char hint[HINT_BUF_SZ];
        snprintf(hint, sizeof(hint),
                 "{\"error\":\"function not found\",\"function_name\":\"%s\","
                 "\"hint\":\"Use search_graph(name_pattern=\\\".*%s.*\\\") to find the exact "
                 "name, then pass it to trace_path.\"}",
                 func_name, func_name);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        cbm_store_free_nodes(nodes, 0);
        return cbm_mcp_text_result(hint, true);
    }

    /* Disambiguate same-named matches: prefer the real definition, and report
     * ambiguity (rather than silently tracing nodes[0]) on a genuine tie — e.g.
     * a C main() vs a same-named shell-script main(). */
    bool trace_ambiguous = false;
    int sel = pick_resolved_node(nodes, node_count, &trace_ambiguous);
    if (trace_ambiguous) {
        char *result = snippet_suggestions(func_name, nodes, node_count);
        free(func_name);
        free(project);
        free(direction);
        free(mode);
        free(param_name);
        cbm_store_free_nodes(nodes, node_count);
        return result;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "function", func_name);
    yyjson_mut_obj_add_str(doc, root, "direction", direction);
    if (mode) {
        yyjson_mut_obj_add_str(doc, root, "mode", mode);
    }

    /* Edge types: explicit > mode-based > default */
    const char *edge_types[MCP_COL_16];
    int edge_type_count = 0;
    yyjson_doc *et_doc_keep = resolve_trace_edge_types(args, mode, edge_types, &edge_type_count);
    char *entry = cbm_mcp_get_string_arg(args, "from_function");
    char *freshness_mode = cbm_mcp_get_string_arg(args, "freshness_mode");
    int max_work = cbm_mcp_get_int_arg(args, "max_work", MCP_PATH_WORK_DEFAULT);
    if (!entry) {
        yyjson_mut_obj_add_bool(doc, root, "historical_index_only", true);
        yyjson_mut_obj_add_bool(doc, root, "current_source_relationship_supported", false);
        yyjson_mut_obj_add_bool(doc, root, "current_shortest_path_validated", false);
        yyjson_mut_obj_add_strcpy(
            doc, root, "relationship_note",
            "Broad traversal returns indexed relationships only; it does not validate a current "
            "source path or a current shortest path.");
    }

    /* Run BFS for each requested direction.
     * IMPORTANT: yyjson_mut_obj_add_str borrows pointers — we must keep
     * traversal results alive until after yy_doc_to_str serialization. */
    bool do_outbound = strcmp(direction, "outbound") == 0 || strcmp(direction, "both") == 0;
    bool do_inbound = strcmp(direction, "inbound") == 0 || strcmp(direction, "both") == 0;

    cbm_traverse_result_t tr_out = {0};
    cbm_traverse_result_t tr_in = {0};

    bool data_flow = mode && strcmp(mode, "data_flow") == 0;
    bool traversal_error = false;

    (void)sel; /* union across all same-name nodes — see bfs_union_same_name (#546) */

    if (do_outbound) {
        if (bfs_union_same_name(store, nodes, node_count, "outbound", edge_types, edge_type_count,
                                depth, include_tests, &tr_out) != CBM_STORE_OK) {
            yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
            traversal_error = true;
        } else {
            yyjson_mut_obj_add_val(doc, root, "callees",
                                   bfs_to_json_array(doc, &tr_out, risk_labels, include_tests,
                                                     min_confidence, data_flow));
            yyjson_mut_obj_add_val(
                doc, root, "callee_edges",
                bfs_edges_to_json_array(doc, &tr_out, min_confidence, include_tests));
        }
    }

    if (do_inbound && !traversal_error) {
        if (entry) {
            (void)trace_direct_path_to_json(store, project, entry, nodes, node_count, depth,
                                            max_work, min_confidence, include_tests, risk_labels,
                                            freshness_mode, doc, root);
        } else {
            if (bfs_union_same_name(store, nodes, node_count, "inbound", edge_types,
                                    edge_type_count, depth, include_tests,
                                    &tr_in) != CBM_STORE_OK) {
                yyjson_mut_obj_add_strcpy(doc, root, "error", cbm_store_error(store));
            } else {
                yyjson_mut_obj_add_val(doc, root, "callers",
                                       bfs_to_json_array(doc, &tr_in, risk_labels, include_tests,
                                                         min_confidence, data_flow));
                yyjson_mut_obj_add_val(
                    doc, root, "caller_edges",
                    bfs_edges_to_json_array(doc, &tr_in, min_confidence, include_tests));
            }
        }
    }

    bool path_suppressed = yyjson_mut_obj_get(root, "indexed_path_suppressed") != nullptr;
    bool direct_index_only = entry && freshness_mode && strcmp(freshness_mode, "index_only") == 0;
    bool direct_strict = entry && !direct_index_only;
    if (!path_suppressed && (!entry || direct_index_only) &&
        cbm_mcp_get_int_arg(args, "source_lines", 0) > 0) {
        yyjson_mut_val *origins = yyjson_mut_arr(doc);
        for (int i = 0; i < node_count; i++) {
            yyjson_mut_val *origin = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, origin, "name", nodes[i].name ? nodes[i].name : "");
            yyjson_mut_obj_add_str(doc, origin, "file",
                                   nodes[i].file_path ? nodes[i].file_path : "");
            yyjson_mut_obj_add_int(doc, origin, "start_line", nodes[i].start_line);
            yyjson_mut_obj_add_int(doc, origin, "end_line", nodes[i].end_line);
            yyjson_mut_arr_add_val(origins, origin);
        }
        yyjson_mut_obj_add_val(doc, root, "definitions", origins);
    }
    if (!path_suppressed && direct_strict &&
        (cbm_mcp_get_int_arg(args, "source_lines", 0) > 0 ||
         cbm_mcp_get_int_arg(args, "source_context", 0) > 0)) {
        yyjson_mut_obj_add_bool(doc, root, "source_withheld", true);
        yyjson_mut_obj_add_strcpy(
            doc, root, "source_note",
            "Strict mode validates the indexed relationship snapshot but withholds live source "
            "snippets because they are not read from the validated hash stream. Use a separate "
            "focused source read or freshness_mode=index_only for historical indexed ranges.");
    } else if (!path_suppressed) {
        trace_add_source(srv, project, doc, root, args);
    }

    /* Serialize BEFORE freeing traversal results (yyjson borrows strings) */
    bool is_error = yyjson_mut_obj_get(root, "error") != nullptr;
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    /* Now safe to free traversal data */
    if (do_outbound) {
        cbm_store_traverse_free(&tr_out);
    }
    if (do_inbound) {
        cbm_store_traverse_free(&tr_in);
    }

    cbm_store_free_nodes(nodes, node_count);
    free(func_name);
    free(project);
    free(direction);
    free(mode);
    free(param_name);
    free(entry);
    free(freshness_mode);
    if (et_doc_keep) {
        yyjson_doc_free(et_doc_keep);
    }

    return mcp_text_result_take_owned(json, is_error);
}

static char *handle_trace_call_path(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *input = args ? yyjson_read(args, strlen(args), 0) : nullptr;
    yyjson_val *input_root = yyjson_doc_get_root(input);
    yyjson_val *entry = yyjson_obj_get(input_root, "from_function");
    yyjson_val *max_work = yyjson_obj_get(input_root, "max_work");
    yyjson_val *freshness = yyjson_obj_get(input_root, "freshness_mode");
    bool invalid_entry = false;
    if (entry) {
        const char *name = yyjson_get_str(entry);
        const char *direction = yyjson_get_str(yyjson_obj_get(input_root, "direction"));
        yyjson_val *mode = yyjson_obj_get(input_root, "mode");
        const char *mode_name = yyjson_get_str(mode);
        invalid_entry = !name || !name[0] || yyjson_get_len(entry) > 512 ||
                        (name && strlen(name) != yyjson_get_len(entry)) || !direction ||
                        strcmp(direction, "inbound") != 0 ||
                        (mode && (!mode_name || strcmp(mode_name, "calls") != 0));
        yyjson_val *types = yyjson_obj_get(input_root, "edge_types");
        if (types) {
            invalid_entry = invalid_entry || !yyjson_is_arr(types) || yyjson_arr_size(types) != 1;
            const char *type = yyjson_get_str(yyjson_arr_get(types, 0));
            invalid_entry = invalid_entry || !type || strcmp(type, "CALLS") != 0;
        }
    }
    if (max_work) {
        int64_t work = yyjson_get_sint(max_work);
        invalid_entry = invalid_entry || !entry || !yyjson_is_int(max_work) || work < 1 ||
                        work > MCP_PATH_WORK_MAX;
    }
    if (freshness) {
        const char *name = yyjson_get_str(freshness);
        invalid_entry = invalid_entry || !entry || !name ||
                        strlen(name) != yyjson_get_len(freshness) ||
                        (strcmp(name, "strict") != 0 && strcmp(name, "index_only") != 0);
    }
    yyjson_doc_free(input);
    if (invalid_entry) {
        return cbm_mcp_text_result(
            "{\"error\":\"from_function requires a nonempty string of at most 512 bytes, "
            "direction=inbound, mode=calls, CALLS edges, freshness_mode strict or index_only, and "
            "max_work from 1 to 100000\"}",
            true);
    }
    char *result = handle_trace_call_path_impl(srv, args);
    int budget = cbm_mcp_get_int_arg(args, "max_bytes", 24000);
    if (budget < 1500) {
        budget = 1500;
    }
    yyjson_doc *doc = result ? yyjson_read(result, strlen(result), 0) : nullptr;
    yyjson_val *content = yyjson_obj_get(yyjson_doc_get_root(doc), "content");
    const char *text = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(content, 0), "text"));
    bool oversized = text && strlen(text) > (size_t)budget;
    yyjson_doc_free(doc);
    if (oversized) {
        free(result);
        return cbm_mcp_text_result(
            "{\"error\":\"trace exceeds max_bytes\",\"truncated\":true,"
            "\"continuation\":\"Narrow direction, depth or function_name, or raise max_bytes.\"}",
            true);
    }
    return result;
}

/* ── Helper: free heap fields of a stack-allocated node ────────── */

static void free_node_contents(cbm_node_t *n) {
    safe_str_free(&n->project);
    safe_str_free(&n->label);
    safe_str_free(&n->name);
    safe_str_free(&n->qualified_name);
    safe_str_free(&n->file_path);
    safe_str_free(&n->properties_json);
    memset(n, 0, sizeof(*n));
}

/* ── Helper: read lines [start, end] from a file ─────────────── */

static char *read_file_lines(const char *path, int start, int end) {
    FILE *fp = cbm_fopen(path, "r");
    if (!fp) {
        return NULL;
    }

    size_t cap = CBM_SZ_4K;
    char *buf = (char *)malloc(cap);
    size_t len = 0;
    buf[0] = '\0';

    char line[CBM_SZ_2K];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (lineno < start) {
            continue;
        }
        if (lineno > end) {
            break;
        }
        size_t ll = strlen(line);
        while (len + ll + SKIP_ONE > cap) {
            cap *= PAIR_LEN;
            buf = (__typeof__(buf))safe_realloc(buf, cap);
        }
        memcpy(buf + len, line, ll);
        len += ll;
        buf[len] = '\0';
    }

    (void)fclose(fp);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ── Helper: get project root_path from store ─────────────────── */

static char *get_project_root(cbm_mcp_server_t *srv, const char *project) {
    if (!project) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) != CBM_STORE_OK) {
        return NULL;
    }
    char *root = heap_strdup(proj.root_path);
    safe_str_free(&proj.name);
    safe_str_free(&proj.indexed_at);
    safe_str_free(&proj.root_path);
    return root;
}

/* ── index_repository ─────────────────────────────────────────── */

/* Handle mode="cross-repo-intelligence" — extract to reduce complexity. */
static void cross_repo_note_used(const char *db_path) {
    cbm_cache_usage_touch(db_path, NULL);
}

static char *handle_cross_repo_mode(const char *repo_path, const char *args) {
    /* cbm_project_name_from_path already returns a heap copy we own; wrapping
     * it in heap_strdup leaked the original on every cross-repo call. */
    char *project = cbm_project_name_from_path(repo_path);
    if (!project) {
        return cbm_mcp_text_result("cannot derive project name", true);
    }

    yyjson_doc *jdoc = yyjson_read(args, strlen(args), 0);
    yyjson_val *jroot = jdoc ? yyjson_doc_get_root(jdoc) : NULL;
    yyjson_val *tp_arr = jroot ? yyjson_obj_get(jroot, "target_projects") : NULL;

    if (!tp_arr || !yyjson_is_arr(tp_arr) || yyjson_arr_size(tp_arr) == 0) {
        yyjson_doc_free(jdoc);
        free(project);
        return cbm_mcp_text_result(
            "{\"error\":\"target_projects is required for cross-repo-intelligence mode. "
            "Use [\\\"*\\\"] for all projects. Run list_projects to see available.\"}",
            true);
    }

    /* Every element must be a project name. A non-string element used to
     * reach cbm_cross_repo_match as a NULL name, which it strcmp()s — so
     * `target_projects: [1]` crashed the worker instead of failing the call. */
    size_t idx;
    size_t max;
    yyjson_val *val;
    yyjson_arr_foreach(tp_arr, idx, max, val) {
        if (!yyjson_is_str(val)) {
            yyjson_doc_free(jdoc);
            free(project);
            return cbm_mcp_text_result(
                "{\"error\":\"target_projects must be an array of project-name strings.\"}", true);
        }
    }

    int tp_count = (int)yyjson_arr_size(tp_arr);
    const char **targets = (const char **)malloc((size_t)tp_count * sizeof(char *));
    if (!targets) {
        yyjson_doc_free(jdoc);
        free(project);
        return cbm_mcp_text_result("{\"error\":\"out of memory\"}", true);
    }
    int ti = 0;
    yyjson_arr_foreach(tp_arr, idx, max, val) {
        targets[ti++] = yyjson_get_str(val);
    }

    /* Record the use of every database the pass opens: the sweep that may
     * follow must not judge them unused. */
    cbm_cross_repo_set_usage_hook(cross_repo_note_used);
    cbm_cross_repo_result_t result = cbm_cross_repo_match(project, targets, tp_count);
    free(targets);
    yyjson_doc_free(jdoc);

    if (result.failed) {
        free(project);
        return cbm_mcp_text_result(
            result.no_targets
                ? "cross-repo-intelligence resolved zero target projects: no indexed project "
                  "other than the source matched target_projects. Index the other service first "
                  "(run list_projects to see what is indexed); existing cross-repo edges were "
                  "left unchanged."
                : "cross-repo source or target project is missing, invalid, not indexed, or busy "
                  "(a cache sweep moving it); "
                  "existing cross-repo edges were left unchanged.",
            true);
    }

    int total = result.http_edges + result.async_edges + result.channel_edges + result.grpc_edges +
                result.graphql_edges + result.trpc_edges;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "success");
    yyjson_mut_obj_add_str(doc, root, "mode", "cross-repo-intelligence");
    yyjson_mut_obj_add_strcpy(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "projects_scanned", result.projects_scanned);
    yyjson_mut_obj_add_int(doc, root, "cross_http_calls", result.http_edges);
    yyjson_mut_obj_add_int(doc, root, "cross_async_calls", result.async_edges);
    yyjson_mut_obj_add_int(doc, root, "cross_channel", result.channel_edges);
    yyjson_mut_obj_add_int(doc, root, "cross_grpc_calls", result.grpc_edges);
    yyjson_mut_obj_add_int(doc, root, "cross_graphql_calls", result.graphql_edges);
    yyjson_mut_obj_add_int(doc, root, "cross_trpc_calls", result.trpc_edges);
    yyjson_mut_obj_add_int(doc, root, "total_cross_edges", total);
    yyjson_mut_obj_add_real(doc, root, "elapsed_ms", result.elapsed_ms);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(project);
    return mcp_text_result_take_owned(json, false);
}

/* Bootstrap from artifact if no local DB exists for this project. */
static void try_artifact_bootstrap(const char *project_name, const char *repo_path) {
    char db_buf[CBM_SZ_1K];
    project_db_path(project_name, db_buf, sizeof(db_buf));
    if (cbm_file_size(db_buf) < 0 && cbm_artifact_exists(repo_path)) {
        cbm_log_info("index.artifact_bootstrap", "project", project_name);
        cbm_artifact_import(repo_path, db_buf);
    }
}

/* Cap on excluded dir paths listed in the response — keep it compact on large
 * repos (node_modules / vendor / etc. can produce many skip points). The full
 * count is still reported via "count" + "truncated". */
enum { INDEX_EXCLUDED_DIR_CAP = 25 };

/* Attach a compact summary of directory subtrees skipped during discovery (#411).
 * Shape: "excluded": {"dirs": [up to 25 rel-paths], "count": <total>, "truncated": <bool>}.
 * No-op when nothing was excluded. excluded_dirs[] is borrowed (copied into doc). */
static void add_excluded_summary(yyjson_mut_doc *doc, yyjson_mut_val *root, char **excluded_dirs,
                                 int excluded_count) {
    if (!excluded_dirs || excluded_count <= 0) {
        return;
    }
    yyjson_mut_val *excluded = yyjson_mut_obj(doc);
    yyjson_mut_val *dirs = yyjson_mut_arr(doc);
    int shown = excluded_count < INDEX_EXCLUDED_DIR_CAP ? excluded_count : INDEX_EXCLUDED_DIR_CAP;
    for (int i = 0; i < shown; i++) {
        if (excluded_dirs[i]) {
            yyjson_mut_arr_add_strcpy(doc, dirs, excluded_dirs[i]);
        }
    }
    yyjson_mut_obj_add_val(doc, excluded, "dirs", dirs);
    yyjson_mut_obj_add_int(doc, excluded, "count", excluded_count);
    yyjson_mut_obj_add_bool(doc, excluded, "truncated", excluded_count > INDEX_EXCLUDED_DIR_CAP);
    yyjson_mut_obj_add_val(doc, root, "excluded", excluded);
}

/* Cap on per-file skips embedded in the JSON response — keep it compact on
 * large repos. The FULL, uncapped list always goes to the per-run logfile;
 * the JSON carries "count" + "truncated" so nothing is silently hidden. */
enum { INDEX_SKIPPED_FILE_CAP = 50 };

/* Attach the by-design ignored-FILES summary (#963 "purposely not indexed").
 * Individual files dropped by ignore rules — deliberate, not failures; whole
 * excluded subtrees are reported separately via "excluded". Always emits
 * "not_indexed_files_count" (the uncapped total); the list itself is capped
 * like skipped[] and marked truncated when discovery hit its storage cap. */
static void add_not_indexed_files_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                          cbm_pipeline_t *p) {
    cbm_ignored_file_t *ignored = NULL;
    int stored = 0;
    int total = 0;
    cbm_pipeline_get_ignored(p, &ignored, &stored, &total);
    yyjson_mut_obj_add_int(doc, root, "not_indexed_files_count", total);
    if (!ignored || stored <= 0) {
        return;
    }
    yyjson_mut_val *ni = yyjson_mut_obj(doc);
    yyjson_mut_val *files = yyjson_mut_arr(doc);
    int shown = stored < INDEX_SKIPPED_FILE_CAP ? stored : INDEX_SKIPPED_FILE_CAP;
    for (int i = 0; i < shown; i++) {
        yyjson_mut_val *fe = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, fe, "path", ignored[i].rel_path ? ignored[i].rel_path : "");
        yyjson_mut_obj_add_strcpy(doc, fe, "reason", ignored[i].reason ? ignored[i].reason : "");
        yyjson_mut_arr_add_val(files, fe);
    }
    yyjson_mut_obj_add_val(doc, ni, "files", files);
    yyjson_mut_obj_add_int(doc, ni, "count", total);
    yyjson_mut_obj_add_bool(doc, ni, "truncated", total > shown);
    yyjson_mut_obj_add_str(doc, ni, "note",
                           "Purposely not indexed — excluded BY DESIGN via "
                           "gitignore/.cbmignore/skip-lists (see each file's reason). Not an "
                           "error: change the ignore rules and re-index to include them. Whole "
                           "excluded subtrees are listed separately under \"excluded\".");
    yyjson_mut_obj_add_val(doc, root, "not_indexed_files", ni);
}

/* True when a recorded per-file entry is the parse-partial coverage signal
 * (#963) rather than a genuine skip. Kept out of skipped[]/skipped_count so
 * the "skipped" contract (file NOT indexed) stays exact. */
static bool is_parse_partial(const cbm_file_error_t *e) {
    return e->phase && strcmp(e->phase, "parse_partial") == 0;
}

/* The whole-file variant: one range covers 80%+ of the file, so listing the
 * lines is useless advice. Also indexed, also not a skip. */
static bool is_parse_unusable(const cbm_file_error_t *e) {
    return e->phase && strcmp(e->phase, "parse_unusable") == 0;
}

static bool is_walk_truncated(const cbm_file_error_t *e) {
    return e->phase && strcmp(e->phase, "walk_truncated") == 0;
}

static bool is_lsp_skipped(const cbm_file_error_t *e) {
    return e->phase && strcmp(e->phase, "lsp_skipped") == 0;
}

/* Any coverage phase. All of them mean the file WAS indexed, so all stay out of
 * skipped[] — a reader who sees a file there believes it is absent. */
static bool is_parse_coverage(const cbm_file_error_t *e) {
    return is_parse_partial(e) || is_parse_unusable(e) || is_walk_truncated(e) || is_lsp_skipped(e);
}

/* Attach a summary of per-file skips (Stage 2 / Track B). Always emits a
 * top-level "skipped_count" (0 on clean runs) so consumers can rely on it.
 * When there are skips, also emits:
 *   "skipped": {"files":[{path,reason,phase}..(<=50)], "count":N, "truncated":bool}
 * and, if a per-run logfile was written, "logfile": "<path>".
 * The run status stays "indexed" — a skipped file is the expected handled
 * outcome, not a failure. errs[] is borrowed (copied into doc) and may contain
 * parse_partial entries, which are filtered out here (reported separately by
 * add_parse_partial_summary). */
static void add_skipped_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                const cbm_file_error_t *errs, int count, const char *logfile) {
    int skips = 0;
    for (int i = 0; i < count; i++) {
        if (!is_parse_coverage(&errs[i])) {
            skips++;
        }
    }
    yyjson_mut_obj_add_int(doc, root, "skipped_count", skips);
    if (logfile && logfile[0]) {
        yyjson_mut_obj_add_strcpy(doc, root, "logfile", logfile);
    }
    if (!errs || skips <= 0) {
        return;
    }
    yyjson_mut_val *skipped = yyjson_mut_obj(doc);
    yyjson_mut_val *files = yyjson_mut_arr(doc);
    int shown = 0;
    for (int i = 0; i < count && shown < INDEX_SKIPPED_FILE_CAP; i++) {
        if (is_parse_coverage(&errs[i])) {
            continue;
        }
        yyjson_mut_val *fe = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, fe, "path", errs[i].path ? errs[i].path : "");
        yyjson_mut_obj_add_strcpy(doc, fe, "reason", errs[i].reason ? errs[i].reason : "");
        yyjson_mut_obj_add_strcpy(doc, fe, "phase", errs[i].phase ? errs[i].phase : "");
        yyjson_mut_arr_add_val(files, fe);
        shown++;
    }
    yyjson_mut_obj_add_val(doc, skipped, "files", files);
    yyjson_mut_obj_add_int(doc, skipped, "count", skips);
    yyjson_mut_obj_add_bool(doc, skipped, "truncated", skips > INDEX_SKIPPED_FILE_CAP);
    yyjson_mut_obj_add_val(doc, root, "skipped", skipped);
}

/* Attach the best-effort parse-coverage summary (#963). Always emits a
 * top-level "parse_partial_count" (0 on clean runs). When files were flagged:
 *   "parse_partial": {"files":[{path,error_ranges}..(<=50)], "count":N,
 *                     "truncated":bool, "note":"..."}
 * These files WERE indexed — constructs inside the listed 1-based line ranges
 * are missing from the graph because tree-sitter could not parse them. The
 * note spells out the best-effort framing: absence from this list is NOT a
 * completeness guarantee. */
/* One half of the coverage summary: which entries belong to it, the JSON keys
 * it writes, how one file entry looks, and the note attached to the section. */
typedef struct {
    bool (*matches)(const cbm_file_error_t *e);
    const char *count_key;
    const char *section_key;
    void (*add_file)(yyjson_mut_doc *doc, yyjson_mut_val *files, const cbm_file_error_t *e);
    const char *note;
} parse_coverage_section_t;

static void add_parse_coverage_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                       const cbm_file_error_t *errs, int count,
                                       const parse_coverage_section_t *section) {
    int flagged = 0;
    for (int i = 0; i < count; i++) {
        if (section->matches(&errs[i])) {
            flagged++;
        }
    }
    yyjson_mut_obj_add_int(doc, root, section->count_key, flagged);
    if (!errs || flagged <= 0) {
        return;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_val *files = yyjson_mut_arr(doc);
    int shown = 0;
    for (int i = 0; i < count && shown < INDEX_SKIPPED_FILE_CAP; i++) {
        if (!section->matches(&errs[i])) {
            continue;
        }
        section->add_file(doc, files, &errs[i]);
        shown++;
    }
    yyjson_mut_obj_add_val(doc, obj, "files", files);
    yyjson_mut_obj_add_int(doc, obj, "count", flagged);
    yyjson_mut_obj_add_bool(doc, obj, "truncated", flagged > INDEX_SKIPPED_FILE_CAP);
    yyjson_mut_obj_add_str(doc, obj, "note", section->note);
    yyjson_mut_obj_add_val(doc, root, section->section_key, obj);
}

static void add_parse_partial_file(yyjson_mut_doc *doc, yyjson_mut_val *files,
                                   const cbm_file_error_t *e) {
    yyjson_mut_val *fe = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, fe, "path", e->path ? e->path : "");
    yyjson_mut_obj_add_strcpy(doc, fe, "error_ranges", e->reason ? e->reason : "");
    yyjson_mut_arr_add_val(files, fe);
}

static void add_parse_unusable_error_file(yyjson_mut_doc *doc, yyjson_mut_val *files,
                                          const cbm_file_error_t *e) {
    add_parse_unusable_file(doc, files, e->path ? e->path : "", e->reason);
}

static void add_parse_partial_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                      const cbm_file_error_t *errs, int count) {
    static const parse_coverage_section_t section = {
        is_parse_partial, "parse_partial_count", "parse_partial", add_parse_partial_file,
        "Best-effort signal, not a completeness guarantee: these files WERE "
        "indexed, but constructs inside the listed line ranges (1-based) could "
        "not be parsed and MAY be missing from the graph (tree-sitter error "
        "recovery still salvages some). Prefer text search (grep) for those "
        "regions. Files absent from this list are NOT guaranteed to be fully "
        "indexed. Query the persisted signal via index_status."};
    add_parse_coverage_summary(doc, root, errs, count, &section);
}

/* The whole-file half of the coverage summary. Always emits a top-level
 * "parse_unusable_count" (0 on clean runs). When files were flagged:
 *   "parse_unusable": {"files":[{path,range_end,whole_file}..(<=50)], "count":N,
 *                      "truncated":bool, "note":"..."}
 * These files WERE indexed, like parse_partial ones, but their one range covers
 * 80%+ of the file, so the range is not worth printing. "range_end" is the last
 * line the range names — it can exceed the file, so it is not the length. */
static void add_parse_unusable_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                       const cbm_file_error_t *errs, int count) {
    static const parse_coverage_section_t section = {
        is_parse_unusable, "parse_unusable_count", "parse_unusable", add_parse_unusable_error_file,
        "Indexed, but the parse failed across nearly the whole file, so line "
        "ranges are not useful here — read the source directly."};
    add_parse_coverage_summary(doc, root, errs, count, &section);
}

static void add_walk_coverage_error_file(yyjson_mut_doc *doc, yyjson_mut_val *files,
                                         const cbm_file_error_t *e) {
    add_walk_coverage_file(doc, files, e->path, e->reason);
}

/* The walk half of the coverage summary (upstream 8c1a9d61). Always emits
 * top-level "walk_truncated_count" and "lsp_skipped_count" (0 on clean runs);
 * sections only when files were flagged. Both kinds WERE indexed. */
static void add_walk_coverage_summary(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                      const cbm_file_error_t *errs, int count) {
    static const parse_coverage_section_t truncated = {
        is_walk_truncated, "walk_truncated_count", "walk_truncated", add_walk_coverage_error_file,
        "Indexed, but extraction stopped at its per-file budget partway through each file "
        "(detail = nodes walked / nodes in the tree), so calls and usages past that point are "
        "missing. Read or grep the source for the rest."};
    static const parse_coverage_section_t lsp = {
        is_lsp_skipped, "lsp_skipped_count", "lsp_skipped", add_walk_coverage_error_file,
        "Indexed, but too large or too deeply nested for the type-aware (LSP) call resolution, "
        "so calls from these files are resolved by name only (detail = the rule that skipped "
        "it, tree size)."};
    add_parse_coverage_summary(doc, root, errs, count, &truncated);
    add_parse_coverage_summary(doc, root, errs, count, &lsp);
}

/* Write the FULL (uncapped) skip list to a per-run logfile — ONLY when >=1 file
 * was skipped (no logfile on a clean run). Location:
 *   $CBM_INDEX_LOG (override) else <cache_dir>/logs/<project>-<epoch>.log
 * Returns true and fills out_path on success. */
static bool write_skip_logfile(const char *project, const cbm_file_error_t *errs, int count,
                               char *out_path, size_t out_sz) {
    if (!errs || count <= 0) {
        return false;
    }
    char path[CBM_SZ_1K];
    const char *override = getenv("CBM_INDEX_LOG");
    /* A path that does not fit is cut off into another file's name, and "wb"
     * would create or truncate that file: no logfile then. */
    auto fits = [](int written, size_t cap) { return written > 0 && (size_t)written < cap; };
    if (override && override[0]) {
        if (!fits(snprintf(path, sizeof(path), "%s", override), sizeof(path))) {
            cbm_log_warn("index.logfile_path_too_long", "source", "CBM_INDEX_LOG");
            return false;
        }
    } else {
        const char *cdir = cbm_resolve_cache_dir();
        if (!cdir) {
            return false;
        }
        char logdir[CBM_SZ_1K];
        if (!fits(snprintf(logdir, sizeof(logdir), "%s/logs", cdir), sizeof(logdir)) ||
            !fits(snprintf(path, sizeof(path), "%s/%s-%lld.log", logdir,
                           project ? project : "index", (long long)time(NULL)),
                  sizeof(path))) {
            cbm_log_warn("index.logfile_path_too_long", "source", "cache_dir");
            return false;
        }
        cbm_mkdir_p(logdir, 0755);
    }
    FILE *f = cbm_fopen(path, "wb");
    if (!f) {
        cbm_log_warn("index.logfile_open_fail", "path", path);
        return false;
    }
    int partials = 0;
    int unusable = 0;
    int walk = 0;
    for (int i = 0; i < count; i++) {
        if (is_parse_partial(&errs[i])) {
            partials++;
        } else if (is_parse_unusable(&errs[i])) {
            unusable++;
        } else if (is_walk_truncated(&errs[i]) || is_lsp_skipped(&errs[i])) {
            walk++;
        }
    }
    (void)fprintf(f, "# code-cortex-mcp index coverage report\n");
    (void)fprintf(f, "# project=%s skipped=%d parse_partial=%d parse_unusable=%d walk=%d\n",
                  project ? project : "", count - partials - unusable - walk, partials, unusable,
                  walk);
    (void)fprintf(f, "# columns: phase\treason\tpath\n");
    for (int i = 0; i < count; i++) {
        (void)fprintf(f, "%s\t%s\t%s\n", errs[i].phase ? errs[i].phase : "",
                      errs[i].reason ? errs[i].reason : "", errs[i].path ? errs[i].path : "");
    }
    (void)fclose(f);
    if (out_path && out_sz) {
        snprintf(out_path, out_sz, "%s", path);
    }
    return true;
}

/* Build the success portion of the index_repository response.
 * Returns true when status should be "degraded" (#334 plausibility gate). */
/* The pipeline persists the complete CURRENT coverage set before this response
 * is built. Prefer that set over the per-run errors, so an incremental run that
 * does not revisit a flagged file — or an artifact bootstrap, which revisits
 * nothing — does not make existing gaps appear to have VANISHED. By-design
 * exclusions have their own response surface and are not failures. */
static bool add_persisted_failure_summaries(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                            cbm_store_t *store, const char *project,
                                            const char *logfile) {
    cbm_coverage_row_t *rows = NULL;
    int row_count = 0;
    if (cbm_store_coverage_get(store, project, &rows, &row_count) != CBM_STORE_OK) {
        return false;
    }

    int failure_count = 0;
    for (int i = 0; i < row_count; i++) {
        const char *kind = rows[i].kind ? rows[i].kind : "";
        if (strcmp(kind, "not_indexed_dir") != 0 && strcmp(kind, "not_indexed_file") != 0) {
            failure_count++;
        }
    }

    cbm_file_error_t *failures =
        failure_count > 0 ? (cbm_file_error_t *)calloc((size_t)failure_count, sizeof(*failures))
                          : NULL;
    if (failure_count > 0 && !failures) {
        cbm_store_free_coverage(rows, row_count);
        return false;
    }

    int n = 0;
    for (int i = 0; i < row_count; i++) {
        const char *kind = rows[i].kind ? rows[i].kind : "";
        if (strcmp(kind, "not_indexed_dir") == 0 || strcmp(kind, "not_indexed_file") == 0) {
            continue;
        }
        /* Borrowed from `rows`, which outlives both emitters below. */
        failures[n].path = (char *)rows[i].rel_path;
        failures[n].reason = (char *)rows[i].detail;
        failures[n].phase = (char *)rows[i].kind;
        n++;
    }

    add_skipped_summary(doc, root, failures, failure_count, logfile);
    add_parse_partial_summary(doc, root, failures, failure_count);
    add_parse_unusable_summary(doc, root, failures, failure_count);
    add_walk_coverage_summary(doc, root, failures, failure_count);
    free(failures);
    cbm_store_free_coverage(rows, row_count);
    return true;
}

static bool build_index_success_response(cbm_mcp_server_t *srv, yyjson_mut_doc *doc,
                                         yyjson_mut_val *root, const char *project_name,
                                         const char *repo_path, bool persistence, cbm_pipeline_t *p,
                                         char **excluded_dirs, int excluded_count,
                                         const cbm_file_error_t *file_errors, int file_error_count,
                                         const char *logfile) {
    add_excluded_summary(doc, root, excluded_dirs, excluded_count);
    add_not_indexed_files_summary(doc, root, p);

    int exp_nodes = -1;
    int exp_edges = -1;
    cbm_pipeline_get_committed_counts(p, &exp_nodes, &exp_edges);

    const double ratio = cbm_dump_verify_min_ratio();
    const int min_floor = CBM_DUMP_VERIFY_MIN_FLOOR;

    cbm_store_t *store = resolve_store(srv, project_name);

    /* Persisted coverage is authoritative; the per-run errors are the fallback
     * for when it cannot be read at all. */
    if (!store || !add_persisted_failure_summaries(doc, root, store, project_name, logfile)) {
        add_skipped_summary(doc, root, file_errors, file_error_count, logfile);
        add_parse_partial_summary(doc, root, file_errors, file_error_count);
        add_parse_unusable_summary(doc, root, file_errors, file_error_count);
        add_walk_coverage_summary(doc, root, file_errors, file_error_count);
    }

    int nodes = 0;
    int edges = 0;
    bool degraded = false;

    if (!store) {
        degraded = true;
    } else {
        nodes = cbm_store_count_nodes(store, project_name);
        edges = cbm_store_count_edges(store, project_name);
        if (nodes < 0) {
            degraded = true;
            nodes = 0;
            edges = edges >= 0 ? edges : 0;
        } else if (cbm_dump_verify_is_degraded(exp_nodes, nodes, ratio, min_floor)) {
            (void)cbm_store_checkpoint(store);
            int nodes2 = cbm_store_count_nodes(store, project_name);
            int edges2 = cbm_store_count_edges(store, project_name);
            if (nodes2 >= 0) {
                nodes = nodes2;
            }
            if (edges2 >= 0) {
                edges = edges2;
            }
            degraded = cbm_dump_verify_is_degraded(exp_nodes, nodes, ratio, min_floor);
        }
    }

    yyjson_mut_obj_add_int(doc, root, "nodes", nodes);
    yyjson_mut_obj_add_int(doc, root, "edges", edges);
    if (exp_nodes >= 0) {
        yyjson_mut_obj_add_int(doc, root, "expected_nodes", exp_nodes);
        yyjson_mut_obj_add_int(doc, root, "expected_edges", exp_edges);
    }

    if (degraded) {
        if (!store) {
            yyjson_mut_obj_add_str(doc, root, "hint",
                                   "Index database failed integrity check and was removed. "
                                   "Re-run index_repository(repo_path=...) to rebuild.");
            cbm_log_warn("dump.verify", "reason", "store_missing", "expected_nodes",
                         exp_nodes >= 0 ? "set" : "unknown");
        } else {
            char exp_buf[MCP_FIELD_SIZE];
            char got_buf[MCP_FIELD_SIZE];
            snprintf(exp_buf, sizeof(exp_buf), "%d", exp_nodes);
            snprintf(got_buf, sizeof(got_buf), "%d", nodes);
            yyjson_mut_obj_add_str(
                doc, root, "hint",
                "Persisted far fewer nodes than indexed — likely durability loss from a "
                "hard-killed sibling process. Re-run index_repository(repo_path=...) to rebuild.");
            cbm_log_warn("dump.verify", "expected_nodes", exp_buf, "persisted_nodes", got_buf);
        }
    }

    bool adr_exists = project_has_adr(store, project_name, repo_path);
    yyjson_mut_obj_add_bool(doc, root, "adr_present", adr_exists);
    if (!adr_exists && !degraded) {
        yyjson_mut_obj_add_str(
            doc, root, "adr_hint",
            "Project indexed. Consider creating an Architecture Decision Record: "
            "explore the codebase with get_architecture(aspects=['all']), then use "
            "manage_adr(mode='update') to persist architectural insights across sessions.");
    }

    bool has_artifact = cbm_artifact_exists(repo_path);
    yyjson_mut_obj_add_bool(doc, root, "artifact_present", has_artifact);
    if (persistence && has_artifact) {
        yyjson_mut_obj_add_str(doc, root, "artifact_hint",
                               "Persistent artifact written to .code-cortex/graph.db.zst. "
                               "Commit this file to share the index with teammates.");
    }

    return degraded;
}

/* Build the response for a worker that crashed/hung/failed without producing a
 * result. The crash is already contained (this process survived); we report it
 * rather than dying. Precise skip-and-continue (quarantine the culprit, index the
 * rest) is layered on in the probe stage. */
static char *build_worker_failure_response(const char *args, cbm_proc_outcome_t outcome) {
    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "error");
    yyjson_mut_obj_add_str(doc, root, "outcome",
                           outcome == CBM_PROC_CLEAN ? "missing_response"
                                                     : cbm_proc_outcome_str(outcome));
    const char *hint = "Indexing worker crashed on a file. The crash was contained (the server "
                       "survived). Re-run to retry; a future release isolates the culprit file.";
    if (outcome == CBM_PROC_CLEAN) {
        hint = "The indexing worker exited cleanly but did not publish a response. The server "
               "survived; inspect the retained worker log and response path.";
    } else if (outcome == CBM_PROC_HANG) {
        hint = "Indexing worker exceeded its deadline. The worker was terminated and the server "
               "survived. Re-run to retry.";
    } else if (outcome == CBM_PROC_SPAWN_FAILED) {
        hint = "The indexing worker could not be started. No in-process fallback was attempted, "
               "so the server remains responsive.";
    } else if (outcome == CBM_PROC_EXIT_NONZERO) {
        hint = "The indexing worker exited without a usable response. The server survived; inspect "
               "the retained worker log for the reported error.";
    }
    yyjson_mut_obj_add_str(doc, root, "hint", hint);
    if (repo_path) {
        yyjson_mut_obj_add_strcpy(doc, root, "repo_path", repo_path);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(repo_path);
    return mcp_text_result_take_owned(json, true);
}

/* Upstream #1300: the explicit error for a worker that exited cleanly without
 * writing a response (worker_result->response_missing). Names the retained
 * worker log and the last phase it reached. */
char *cbm_mcp_index_worker_no_response_failure(const char *args,
                                               const cbm_index_worker_result_t *worker_result) {
    const char *phase =
        worker_result && worker_result->last_phase[0] ? worker_result->last_phase : "unknown";
    const char *log =
        worker_result && worker_result->worker_log[0] ? worker_result->worker_log : "unavailable";
    char *repo_path = args ? cbm_mcp_get_string_arg(args, "repo_path") : NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "error");
    yyjson_mut_obj_add_str(doc, root, "outcome", "missing_response");
    yyjson_mut_obj_add_str(doc, root, "reason", "no_response");
    yyjson_mut_obj_add_strcpy(doc, root, "last_phase", phase);
    yyjson_mut_obj_add_strcpy(doc, root, "worker_log", log);
    char hint[CBM_SZ_2K];
    (void)snprintf(hint, sizeof(hint),
                   "Indexing worker exited cleanly (exit 0) without writing a response; the "
                   "index was not published. Last phase reached: %s. Inspect the worker log: %s",
                   phase, log);
    yyjson_mut_obj_add_strcpy(doc, root, "hint", hint);
    if (repo_path) {
        yyjson_mut_obj_add_strcpy(doc, root, "repo_path", repo_path);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    free(repo_path);
    return mcp_text_result_take_owned(json, true);
}

/* Drop the cached store so the next query reopens whatever the worker wrote (each
 * worker is a fresh process that deletes + recreates the .db). NULL-safe: the
 * background watcher path (main.c) has no MCP server / cached store — the child
 * writes the DB and the parent only needs the return code, so there is nothing
 * to invalidate. */
static void supervisor_invalidate_store(cbm_mcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;
}

/* Resolve a per-supervisor-run temp path <cache_dir>/logs/.supervisor-<pid><suffix>
 * (falls back to the CWD if the cache dir is unresolvable). Used for the crash-
 * attribution marker and the quarantine list during the recovery re-run. */
static void supervisor_tmp_path(char *out, size_t out_sz, const char *suffix) {
    const char *cdir = cbm_resolve_cache_dir();
    if (cdir && cdir[0]) {
        char logdir[CBM_SZ_1K];
        snprintf(logdir, sizeof(logdir), "%s/logs", cdir);
        cbm_mkdir_p(logdir, 0755);
        snprintf(out, out_sz, "%s/.supervisor-%d%s", logdir, (int)getpid(), suffix);
    } else {
        snprintf(out, out_sz, ".supervisor-%d%s", (int)getpid(), suffix);
    }
}

/* Parse the worker's marker JOURNAL ("S <rel>" / "D <rel>" lines, one event
 * per line — see cbm_index_mark_start/done) into the crash/hang SUSPECT set:
 * files whose last event is an S with no closing D, i.e. the in-flight set
 * at kill time. Recovery runs are PARALLEL, so there are up to worker_count
 * suspects; a torn final line (no trailing newline) is discarded by design.
 * Returns a malloc'd array of malloc'd rel paths, OLDEST OPEN S FIRST (for a
 * hang, the oldest still-open file IS the stuck one). Caller frees via
 * supervisor_free_suspects. */
static char **supervisor_read_suspects(const char *path, int *out_n) {
    *out_n = 0;
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char **open_paths = NULL; /* open (S-without-D) files in first-S order */
    int open_n = 0;
    int open_cap = 0;
    char line[CBM_SZ_1K];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {
            break; /* torn final line — discard and stop */
        }
        line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
        if (len < 3 || (line[0] != 'S' && line[0] != 'D') || line[1] != ' ') {
            continue;
        }
        const char *rel = line + 2;
        if (line[0] == 'S') {
            bool already = false;
            for (int i = 0; i < open_n && !already; i++) {
                already = strcmp(open_paths[i], rel) == 0;
            }
            if (already) {
                continue;
            }
            if (open_n == open_cap) {
                int ncap = open_cap ? open_cap * 2 : 16;
                char **np = (char **)realloc(open_paths, (size_t)ncap * sizeof(char *));
                if (!np) {
                    break;
                }
                open_paths = np;
                open_cap = ncap;
            }
            open_paths[open_n++] = cbm_strdup(rel);
        } else {
            for (int i = 0; i < open_n; i++) {
                if (strcmp(open_paths[i], rel) == 0) {
                    free(open_paths[i]);
                    memmove(&open_paths[i], &open_paths[i + 1],
                            (size_t)(open_n - i - 1) * sizeof(char *));
                    open_n--;
                    break;
                }
            }
        }
    }
    (void)fclose(f);
    if (open_n == 0) {
        free(open_paths);
        return NULL;
    }
    *out_n = open_n;
    return open_paths;
}

static void supervisor_free_suspects(char **s, int n) {
    if (!s) {
        return;
    }
    for (int i = 0; i < n; i++) {
        free(s[i]);
    }
    free(s);
}

static bool supervisor_suspect_contains(char **s, int n, const char *rel) {
    for (int i = 0; i < n; i++) {
        if (s[i] && strcmp(s[i], rel) == 0) {
            return true;
        }
    }
    return false;
}

/* Append one quarantine entry "rel\tphase\n" (phase = "crash"|"hang"|"error") to
 * the
 * quarantine list. The worker's loader parses this back and reports the skip's
 * phase in skipped[]; a bare "rel" line is still tolerated there (defaults crash). */
static bool supervisor_append_quarantine(const char *path, const char *rel, const char *phase) {
    FILE *f = cbm_fopen(path, "ab");
    if (!f) {
        return false;
    }
    (void)fprintf(f, "%s\t%s\n", rel, phase);
    (void)fclose(f);
    return true;
}

static int supervisor_remaining_ms(uint64_t deadline_ms) {
    uint64_t now_ms = cbm_now_ms();
    if (now_ms >= deadline_ms) {
        return 0;
    }
    uint64_t remaining = deadline_ms - now_ms;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

/* Run index_repository in a supervised worker subprocess with skip-and-continue
 * (Stage 3c). Returns the response string (caller frees):
 *   - the worker's own response on a clean first run (the common path);
 *   - after a crash/hang, the response from a clean single-threaded RECOVERY run
 *     that quarantines the culprit file(s) — status="indexed" with them listed in
 *     skipped[] as phase="crash"/"hang"/"error", and the good files indexed;
 *   - a best-effort PARTIAL index (one final quarantine-only run) if the recovery
 *     loop cannot converge but at least one file was quarantined;
 *   - a contained-failure response if even that cannot produce a clean run.
 * Every attempt shares one hard deadline; failure never falls back to an
 * unbounded in-process index in the production host. */
/* How many times a failed index worker may be re-run before the server gives
 * up, as CBM_INDEX_MAX_RESTARTS sets it. Default 100; 0 means no restarts. */
static int index_restart_cap(void) {
    enum { INDEX_RESTART_CAP_DEFAULT = 100 };
    long v = 0;
    /* Unset is the ordinary case and says nothing. A value that is set but
     * unreadable is a person's intent being dropped, so name it. */
    if (!cbm_env_long_warn("CBM_INDEX_MAX_RESTARTS", LONG_MIN, LONG_MAX, &v,
                           "index.restart_cap.ignored")) {
        return INDEX_RESTART_CAP_DEFAULT;
    }
    /* Zero is a real answer. The old reader kept the default unless the number
     * was above zero, so the one value meaning "leave the worker alone" did the
     * opposite. */
    if (v < 0 || v > INT_MAX) {
        cbm_log_warn("index.restart_cap.out_of_range", "action", "using_default");
        return INDEX_RESTART_CAP_DEFAULT;
    }
    return (int)v;
}

int cbm_index_restart_cap_for_testing(void) {
    return index_restart_cap();
}

static char *index_run_supervised(cbm_mcp_server_t *srv, const char *args) {
    supervisor_invalidate_store(srv);
    uint64_t deadline_ms = cbm_now_ms() + (uint64_t)cbm_index_timeout_ms();

    /* First attempt: normal parallel run. */
    cbm_index_worker_result_t wr;
    int rc =
        cbm_index_spawn_worker(args, false, NULL, NULL, supervisor_remaining_ms(deadline_ms), &wr);

    if (rc != 0 || wr.outcome == CBM_PROC_SPAWN_FAILED) {
        cbm_index_worker_result_free(&wr);
        supervisor_invalidate_store(srv);
        return build_worker_failure_response(args, CBM_PROC_SPAWN_FAILED);
    }
    if (wr.outcome == CBM_PROC_CLEAN) {
        /* Clean exit → transfer the worker's response (the common path). A clean
         * worker that wrote no response is still a contained worker failure; do
         * not fall back to an unbounded in-process run. */
        char *resp = wr.response; /* transfer ownership to caller (may be NULL) */
        wr.response = NULL;
        if (!resp) {
            /* #1300: a clean exit without a response is a named failure. */
            resp = cbm_mcp_index_worker_no_response_failure(args, &wr);
        }
        cbm_index_worker_result_free(&wr);
        supervisor_invalidate_store(srv);
        return resp;
    }

    /* Crash / hang / nonzero exit → skip-and-continue recovery. Re-run the
     * worker PARALLEL (there are no sequential production runs) with the
     * per-file marker JOURNAL armed; after each failed run the journal's
     * open-S set is the in-flight SUSPECT set. A file is quarantined only
     * when it appears in the suspect sets of TWO CONSECUTIVE failed runs
     * (intersection — a stale or merely unlucky in-flight file rotates out),
     * and only ONE file per round: the OLDEST open S in the intersection
     * (for a hang the oldest still-open file IS the stuck one; for a crash
     * it is the longest-running suspect — the best single deterministic
     * pick). A clean run then indexes the good files and reports the
     * quarantined ones as phase="crash"/"hang"/"error" skips via the ordinary
     * Stage-2 skip plumbing. The old design re-ran SINGLE-THREADED to keep
     * one exact marker; at scale that fell into the sequential crawl, went
     * quiet, was killed as a hang mid-pass, and the stale marker got FOUR
     * innocent ms-typescript fixtures quarantined one 15-minute retry at a
     * time. */
    cbm_proc_outcome_t last_outcome = wr.outcome;
    cbm_index_worker_result_free(&wr);

    char marker_path[CBM_SZ_1K];
    char quarantine_path[CBM_SZ_1K];
    supervisor_tmp_path(marker_path, sizeof(marker_path), ".marker");
    supervisor_tmp_path(quarantine_path, sizeof(quarantine_path), ".quarantine");
    (void)remove(marker_path);
    /* Start the quarantine list empty (truncate any stale file). */
    FILE *qinit = cbm_fopen(quarantine_path, "wb");
    if (qinit) {
        (void)fclose(qinit);
    }

    int cap = index_restart_cap();

    char *resp = NULL;
    int quarantined = 0;         /* files pinned + added to the quarantine list so far */
    char **prev_suspects = NULL; /* previous failed round's in-flight set */
    int prev_n = 0;
    for (int i = 0; i < cap; i++) {
        int remaining_ms = supervisor_remaining_ms(deadline_ms);
        if (remaining_ms <= 0) {
            last_outcome = CBM_PROC_HANG;
            break;
        }
        cbm_index_worker_result_t wr2;
        int rc2 = cbm_index_spawn_worker(args, /*single_thread=*/false, marker_path,
                                         quarantine_path, remaining_ms, &wr2);
        if (rc2 != 0) {
            last_outcome = wr2.outcome;
            cbm_index_worker_result_free(&wr2);
            break; /* spawn failed mid-recovery — give up */
        }
        if (wr2.outcome == CBM_PROC_CLEAN && wr2.response) {
            resp = wr2.response; /* transfer ownership to caller */
            wr2.response = NULL;
            cbm_index_worker_result_free(&wr2);
            break; /* good files indexed; quarantined files reported as crash/hang */
        }
        if (wr2.outcome == CBM_PROC_CLEAN && wr2.response_missing) {
            /* #1300: a recovery run that exits cleanly without a response is
             * reported as such, not as a generic failed recovery. */
            resp = cbm_mcp_index_worker_no_response_failure(args, &wr2);
            cbm_index_worker_result_free(&wr2);
            break;
        }
        if (wr2.outcome == CBM_PROC_CRASH || wr2.outcome == CBM_PROC_HANG ||
            wr2.outcome == CBM_PROC_EXIT_NONZERO) {
            last_outcome = wr2.outcome;
            cbm_index_worker_result_free(&wr2);
            /* crash vs hang vs nonzero-exit: the phase this file is quarantined
             * under and reported as in skipped[]. A fault signal → "crash"; a
             * no-progress kill → "hang"; a graceful nonzero exit (e.g. an
             * internal parse-limit/abort on a pathological file) → "error".
             * EXIT_NONZERO is attributed via the SAME marker-journal suspect
             * mechanism as a crash: the two-consecutive-strikes intersection
             * below still guards against quarantining an innocent file, and a
             * SYSTEMIC nonzero exit (e.g. a bad arg) produces no recurring
             * suspect → the intersection is empty → give_up (correct). This
             * makes a single pathological file skip-and-continue instead of
             * aborting the whole chunk.
             *
             * Note: A deterministic first-file failure (e.g. worker crashing or
             * exiting on the very first file every run) will progressively
             * quarantine in-flight files until it reaches the culprit. That is
             * an existing property of the crash/hang path, accepted here as a
             * considered tradeoff. */
            const char *phase;
            if (last_outcome == CBM_PROC_HANG) {
                phase = "hang";
            } else if (last_outcome == CBM_PROC_EXIT_NONZERO) {
                phase = "error";
            } else {
                phase = "crash";
            }
            int sus_n = 0;
            char **suspects = supervisor_read_suspects(marker_path, &sus_n);
            (void)remove(marker_path); /* fresh journal for the next re-run */
            if (!suspects || sus_n == 0) {
                supervisor_free_suspects(suspects, sus_n);
                cbm_log_warn("index.supervisor.unattributable", "action", "give_up");
                break;
            }
            if (prev_suspects) {
                /* Two-consecutive-strikes: quarantine the OLDEST open S that
                 * was also in flight in the previous failed round. */
                const char *pick = NULL;
                for (int k = 0; k < sus_n && !pick; k++) {
                    if (supervisor_suspect_contains(prev_suspects, prev_n, suspects[k])) {
                        pick = suspects[k];
                    }
                }
                if (!pick) {
                    /* Disjoint consecutive in-flight sets: the failure is not
                     * attributable to a recurring file (systemic) — stop
                     * rather than quarantine an innocent. */
                    supervisor_free_suspects(suspects, sus_n);
                    cbm_log_warn("index.supervisor.unattributable", "action", "give_up");
                    break;
                }
                if (!supervisor_append_quarantine(quarantine_path, pick, phase)) {
                    cbm_log_warn("index.supervisor.quarantine_write_fail", "path", pick);
                    supervisor_free_suspects(suspects, sus_n);
                    break;
                }
                quarantined++;
                char attempt_buf[MCP_FIELD_SIZE];
                snprintf(attempt_buf, sizeof(attempt_buf), "%d", i + 1);
                cbm_log_warn("index.file_quarantined", "path", pick, "outcome", phase, "attempt",
                             attempt_buf);
            }
            supervisor_free_suspects(prev_suspects, prev_n);
            prev_suspects = suspects;
            prev_n = sus_n;
            continue;
        }
        /* SPAWN_FAILED / nonzero exit / non-fault kill → not a crash we can
         * attribute; stop and report a contained failure. */
        last_outcome = wr2.outcome;
        cbm_index_worker_result_free(&wr2);
        break;
    }
    supervisor_free_suspects(prev_suspects, prev_n);

    (void)remove(marker_path); /* marker no longer needed */

    /* Terminal best-effort-partial: the loop exited WITHOUT a clean run (cap
     * exhausted, or an unattributable failure) but at least one file was already
     * quarantined. Try ONE final PARALLEL spawn with the accumulated quarantine
     * and NO marker — every known-bad file short-circuits, so a clean run yields
     * a PARTIAL index (all good files indexed, all known crashers/hangs reported
     * as skips) rather than a hard failure. Bounded by the same quiet-timeout,
     * so it cannot itself hang. Rare given monotonic progress. */
    if (!resp && quarantined > 0) {
        int remaining_ms = supervisor_remaining_ms(deadline_ms);
        if (remaining_ms > 0) {
            cbm_index_worker_result_t wrp;
            int rcp = cbm_index_spawn_worker(args, /*single_thread=*/false, NULL, quarantine_path,
                                             remaining_ms, &wrp);
            if (rcp == 0 && wrp.outcome == CBM_PROC_CLEAN && wrp.response) {
                resp = wrp.response; /* transfer ownership to caller */
                wrp.response = NULL;
                char qn[MCP_FIELD_SIZE];
                snprintf(qn, sizeof(qn), "%d", quarantined);
                cbm_log_error("index.supervisor.partial", "quarantined", qn, "outcome",
                              cbm_proc_outcome_str(last_outcome));
            }
            cbm_index_worker_result_free(&wrp);
        }
    }

    (void)remove(quarantine_path);
    supervisor_invalidate_store(srv);

    if (resp) {
        return resp;
    }
    return build_worker_failure_response(args, last_outcome);
}

/* Build a minimal {"repo_path": "<root>"} args object (path safely escaped) and
 * run it through index_run_supervised. Shared by the session auto-index (srv
 * present → its cached store is invalidated) and the watcher re-index (srv NULL).
 * Returns the worker's response string (caller frees), or NULL only when the
 * input path could not be encoded. */
static char *index_run_supervised_path(cbm_mcp_server_t *srv, const char *root_path) {
    if (!root_path || !root_path[0]) {
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "repo_path", root_path);
    char *args = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    if (!args) {
        return NULL;
    }
    char *resp = index_run_supervised(srv, args);
    free(args);
    return resp;
}

/* Public entry (see mcp.h): the watcher re-index in main.c has no MCP server, so
 * it reaches the supervised runner through this srv-less wrapper. */
char *cbm_mcp_index_run_supervised_path(const char *root_path) {
    return index_run_supervised_path(NULL, root_path);
}

/* #1211: index_repository requires repo_path, but list_projects only ever
 * advertises the project NAME, and every read tool accepts that name back via
 * get_project_arg's "project"/"project_name"/"project_id"/"projectName"
 * aliases. Re-indexing an already-indexed project by that same name had no
 * resolution path and fell straight to "repo_path is required". Look up the
 * project's own stored root_path (list_projects proves it's on file) before
 * giving up. Query-only open, always closed here: this never creates a store
 * or touches srv->store/srv->current_project, so it cannot disturb whatever
 * project the server has cached. */
static char *resolved_repo_path_from_project_arg(const char *args) {
    /* A missing project means "this repository"; an unknown name stays an
     * error here — re-indexing is not something to guess about. */
    char *project = get_project_arg_or_cwd(NULL, args, false);
    if (!project) {
        return NULL;
    }
    char db_path[CBM_SZ_1K];
    project_db_path(project, db_path, sizeof(db_path));
    cbm_store_t *store = db_path[0] ? cbm_store_open_path_query(db_path) : NULL;
    char *root_path = NULL;
    if (store) {
        cbm_project_t proj = {0};
        if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK) {
            root_path = proj.root_path ? heap_strdup(proj.root_path) : NULL;
            cbm_project_free_fields(&proj);
        }
        cbm_store_close(store);
    }
    free(project);
    return root_path;
}

/* Parked-branch limits for an index run (pipeline/branch_slots.h): config
 * branch_index (default true), branch_index_keep (default 8) and
 * branch_index_budget (bytes, "2G" style; default 2 GiB). The environment
 * wins: CBM_BRANCH_INDEX=0 turns parking off and CBM_BRANCH_INDEX_BUDGET sets
 * the size budget. The supervised worker runs without srv->config, so the
 * store is opened just for this read. A field left at -1 keeps the
 * pipeline's own default. */
static branch_limits_t branch_index_limits(const cbm_mcp_server_t *srv) {
    branch_limits_t lim = {-1, -1};
    char env[CBM_SZ_32] = "";
    cbm_safe_getenv("CBM_BRANCH_INDEX", env, sizeof(env), NULL);
    if (strcmp(env, "0") == 0) {
        lim.keep = 0;
        return lim;
    }
    cbm_config_t *cfg = srv ? srv->config : NULL;
    cbm_config_t *owned = NULL;
    if (!cfg) {
        const char *dir = cbm_resolve_cache_dir();
        owned = dir ? cbm_config_open(dir) : NULL;
        cfg = owned;
    }
    if (cfg) {
        lim.keep = cbm_config_get_bool(cfg, CBM_CONFIG_BRANCH_INDEX, true)
                       ? cbm_config_get_int(cfg, CBM_CONFIG_BRANCH_INDEX_KEEP,
                                            CBM_BRANCH_SLOTS_DEFAULT_KEEP)
                       : 0;
        const char *b = cbm_config_get(cfg, CBM_CONFIG_BRANCH_INDEX_BUDGET, "");
        int64_t v = 0;
        if (b && b[0] && cbm_branch_budget_parse(b, &v)) {
            lim.budget = v;
        } else if (b && b[0]) {
            cbm_log_warn("branch.budget_invalid", "value", b);
        }
    }
    cbm_config_close(owned);
    char benv[CBM_SZ_32] = "";
    int64_t v = 0;
    if (cbm_safe_getenv("CBM_BRANCH_INDEX_BUDGET", benv, sizeof(benv), NULL) &&
        cbm_branch_budget_parse(benv, &v)) {
        lim.budget = v;
    }
    return lim;
}

static void apply_branch_index_config(const cbm_mcp_server_t *srv, cbm_pipeline_t *p) {
    branch_limits_t lim = branch_index_limits(srv);
    if (lim.keep >= 0) {
        cbm_pipeline_set_branch_keep(p, lim.keep);
    }
    if (lim.budget >= 0) {
        cbm_pipeline_set_branch_budget(p, lim.budget);
    }
}

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*index_test_cb)(const char *, void *) = nullptr;
static void *index_test_data = nullptr;

void cbm_mcp_test_index_hook(void (*callback)(const char *project, void *data), void *data) {
    index_test_cb = callback;
    index_test_data = data;
}
#define INDEX_TEST_HOOK(p)                                                \
    do {                                                                  \
        if (index_test_cb) {                                              \
            index_test_cb(cbm_pipeline_project_name(p), index_test_data); \
        }                                                                 \
    } while (0)
#else
#define INDEX_TEST_HOOK(p) ((void)0)
#endif

/* An index run is a use, and it knows the checkout root exactly. The
 * pipeline calls this before it releases the index lease: a sweep in another
 * process that judged the database stale before the run takes the lease
 * only after this write, re-reads the row, and keeps the database. */
static void touch_indexed(const cbm_pipeline_t *p, void *ud) {
    (void)ud;
    const char *name = cbm_pipeline_project_name(p);
    char path[CBM_SZ_1K];
    project_db_path(name, path, sizeof(path));
    if (path[0] && cbm_file_exists(path)) {
        cbm_cache_usage_touch(path, cbm_pipeline_repo_path(p));
    }
}

/* A full in-process index of root_path that records its use and root
 * (touch_indexed). srv supplies the branch-index config (NULL: defaults);
 * serialize holds the process-wide pipeline lock around the run. */
static int index_in_process(const cbm_mcp_server_t *srv, const char *root_path, bool serialize) {
    cbm_pipeline_t *p = cbm_pipeline_new(root_path, NULL, CBM_MODE_FULL);
    if (!p) {
        return CBM_NOT_FOUND;
    }
    apply_branch_index_config(srv, p);
    cbm_pipeline_set_on_indexed(p, touch_indexed, NULL);
    if (serialize) {
        cbm_pipeline_lock();
    }
    int rc = cbm_pipeline_run(p);
    if (serialize) {
        cbm_pipeline_unlock();
    }
    INDEX_TEST_HOOK(p);
    cbm_pipeline_free(p);
    return rc;
}

int cbm_mcp_index_in_process(const char *root_path) {
    return index_in_process(NULL, root_path, false);
}

/* index_repository's "branch" block: which branch this run indexed, which one
 * the database had been built for, and what the per-branch switch did. */
static void add_branch_switch_json(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                   const cbm_pipeline_t *p) {
    const char *action = NULL;
    const char *from = NULL;
    const char *to = NULL;
    int reparsed = -1;
    cbm_pipeline_get_branch_switch(p, &action, &from, &to, &reparsed);
    if (!to) {
        return;
    }
    yyjson_mut_val *b = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, b, "indexed", to);
    if (from) {
        yyjson_mut_obj_add_strcpy(doc, b, "previous", from);
    }
    yyjson_mut_obj_add_strcpy(doc, b, "switch", action);
    if (reparsed >= 0) {
        yyjson_mut_obj_add_int(doc, b, "files_reparsed", reparsed);
    }
    yyjson_mut_obj_add_val(doc, root, "branch", b);
}

static bool wait_for_pipeline_lock(int timeout_ms) {
    uint64_t deadline_ms = cbm_now_ms() + (uint64_t)timeout_ms;
    do {
        if (cbm_pipeline_try_lock()) {
            return true;
        }
        struct timespec delay = {0, MCP_INDEX_LOCK_POLL_MS * 1000000L};
        cbm_nanosleep(&delay, NULL);
    } while (cbm_now_ms() < deadline_ms);
    return false;
}

static char *index_repository_in_process(cbm_mcp_server_t *srv, const char *args) {
    char *repo_path = cbm_mcp_get_string_arg(args, "repo_path");
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    char *name_override = cbm_mcp_get_string_arg(args, "name");
    cbm_normalize_path_sep(repo_path);

    if (!repo_path) {
        repo_path = resolved_repo_path_from_project_arg(args);
        cbm_normalize_path_sep(repo_path);
    }

    if (!repo_path) {
        free(mode_str);
        free(name_override);
        return cbm_mcp_text_result("repo_path is required", true);
    }

    repo_path = canonicalize_repo_path_if_exists(repo_path);

    /* Optional workspace boundary: when CBM_ALLOWED_ROOT is set (agentic /
     * multi-tenant deployments where repo_path may be influenced by an
     * untrusted caller), refuse to index a path that resolves outside it.
     * Unset by default, so the standard "index the path I gave you" behaviour
     * is unchanged. */
    const char *allowed_root = getenv("CBM_ALLOWED_ROOT");
    if (allowed_root && allowed_root[0] && repo_path &&
        !cbm_path_within_root(allowed_root, repo_path)) {
        free(mode_str);
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result("repo_path is outside the allowed root", true);
    }

    if (mode_str && strcmp(mode_str, "cross-repo-intelligence") == 0) {
        free(mode_str);
        free(name_override);
        char *result = handle_cross_repo_mode(repo_path, args);
        free(repo_path);
        return result;
    }

    cbm_index_mode_t mode = CBM_MODE_FULL;
    if (mode_str && strcmp(mode_str, "fast") == 0) {
        mode = CBM_MODE_FAST;
    } else if (mode_str && strcmp(mode_str, "moderate") == 0) {
        mode = CBM_MODE_MODERATE;
    }
    free(mode_str);

    bool persistence = cbm_mcp_get_bool_arg(args, "persistence");

    cbm_pipeline_t *p = cbm_pipeline_new(repo_path, NULL, mode);
    if (!p) {
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result("failed to create pipeline", true);
    }
    if (name_override && name_override[0] && !cbm_pipeline_set_project_name(p, name_override)) {
        cbm_pipeline_free(p);
        free(name_override);
        free(repo_path);
        return cbm_mcp_text_result("invalid project name", true);
    }
    free(name_override);
    cbm_pipeline_set_persistence(p, persistence);
    apply_branch_index_config(srv, p);

    char *project_name = heap_strdup(cbm_pipeline_project_name(p));

    /* Own the DB generation before artifact installation, not just before
     * pipeline_run's eventual SQLite open. Worker owns the OS lease. */
    int claim_rc = cbm_pipeline_claim_index(p);
    if (claim_rc != 0) {
        cbm_pipeline_free(p);
        free(project_name);
        free(repo_path);
        return claim_rc == CBM_INDEX_BUSY
                   ? index_busy_result()
                   : cbm_mcp_text_result("failed to acquire database indexing lease", true);
    }

    /* Bootstrap from artifact if no local DB exists */
    try_artifact_bootstrap(project_name, repo_path);

    /* Close cached store — pipeline will delete + recreate the .db file */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;

    /* Serialize pipeline runs to prevent concurrent writes.
     * Track active pipeline so signal handler and notifications/cancelled
     * can cancel it mid-run. */
    cbm_pipeline_lock();
    srv->active_pipeline = p;
    cbm_pipeline_set_on_indexed(p, touch_indexed, NULL);
    int rc = cbm_pipeline_run(p);
    srv->active_pipeline = NULL;
    cbm_pipeline_unlock();
    INDEX_TEST_HOOK(p);

    /* Capture the excluded-subtree list (#411) while the pipeline (which owns
     * the strings) is still alive — the response builder copies them into the
     * JSON doc, so they need only outlive that call, not cbm_pipeline_free. */
    char **excluded_dirs = NULL;
    int excluded_count = 0;
    cbm_pipeline_get_excluded(p, &excluded_dirs, &excluded_count);

    /* Capture the per-file skip list (Stage 2 / Track B) while the pipeline
     * still owns the strings; the response builder copies them into the doc. */
    cbm_file_error_t *file_errors = NULL;
    int file_error_count = 0;
    cbm_pipeline_get_file_errors(p, &file_errors, &file_error_count);

    cbm_mem_collect(); /* return mimalloc pages to OS after large indexing */

    /* Invalidate cached store so next query reopens the fresh database */
    if (srv->owns_store && srv->store) {
        cbm_store_close(srv->store);
        srv->store = NULL;
    }
    free(srv->current_project);
    srv->current_project = NULL;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "project", project_name);

    if (rc == 0) {
        /* Write the per-run logfile ONLY when there were skips (no logfile on a
         * clean run). The FULL list goes to the file; the JSON caps at 50. */
        char logfile_path[CBM_SZ_1K];
        logfile_path[0] = '\0';
        bool has_logfile = write_skip_logfile(project_name, file_errors, file_error_count,
                                              logfile_path, sizeof(logfile_path));
        bool degraded = build_index_success_response(
            srv, doc, root, project_name, repo_path, persistence, p, excluded_dirs, excluded_count,
            file_errors, file_error_count, has_logfile ? logfile_path : NULL);
        yyjson_mut_obj_add_str(doc, root, "status", degraded ? "degraded" : "indexed");
        if (cbm_pipeline_had_format_migration(p)) {
            yyjson_mut_obj_add_bool(doc, root, "format_migration", true);
        }
        add_branch_switch_json(doc, root, p);
    } else {
        yyjson_mut_obj_add_str(doc, root, "status", "error");
        /* #1665: a post-publish artifact export failure (read-only repo, etc.)
         * must not be blamed on the repository path. The pipeline snapshots the
         * export error of THIS run, so its presence names the phase exactly —
         * the graph database was already published when the export ran. */
        const char *export_error = cbm_pipeline_export_error(p);
        if (cbm_pipeline_branch_stamp_failed(p)) {
            yyjson_mut_obj_add_str(doc, root, "hint",
                                   "Index database was built, but recording its git branch "
                                   "failed (database busy or cache not writable). Re-run "
                                   "index_repository; the database is not parked under another "
                                   "branch meanwhile.");
        } else if (export_error && export_error[0]) {
            char hint[CBM_SZ_2K];
            (void)snprintf(
                hint, sizeof(hint),
                "Index database was published, but the persistence artifact export failed for "
                "repo_path/.code-cortex (%s). Use a writable checkout, or re-run with "
                "persistence false if the shared artifact is not needed.",
                export_error);
            yyjson_mut_obj_add_strcpy(doc, root, "hint", hint);
        } else {
            yyjson_mut_obj_add_str(doc, root, "hint",
                                   "Pipeline failed. Check repo_path exists and contains source "
                                   "files. Try mode='fast' for a quicker diagnostic run.");
        }
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    /* Free the pipeline only after the response doc copied the excluded list.
     * Supervised worker: skip the deep free — the process exits right after
     * handing over the response (main.c fast-exits), and piecemeal-freeing a
     * multi-GB graph before process death costs minutes on kernel-scale repos;
     * the OS reclaims it wholesale at exit. In-process paths (tests, kill
     * switch, degrade) still free normally. */
    if (cbm_index_worker_active()) {
        cbm_log_info("index.worker.fast_exit", "skip", "pipeline_free");
    } else {
        cbm_pipeline_free(p);
    }
    free(project_name);
    free(repo_path);

    return mcp_text_result_take_owned(json, rc != 0);
}

static char *handle_index_repository(cbm_mcp_server_t *srv, const char *args) {
    /* Cross-repo matching extracts nothing: it reads and writes existing
     * project DBs. Run it in-process (under the pipeline lock), ahead of the
     * supervisor, so its own error result (zero targets, a missing target)
     * reaches the caller instead of a generic "worker exited nonzero". */
    char *mode_peek = cbm_mcp_get_string_arg(args, "mode");
    bool cross_repo = mode_peek && strcmp(mode_peek, "cross-repo-intelligence") == 0;
    free(mode_peek);

    /* Supervisor gate: run the index in a crash/hang-isolating worker subprocess
     * unless this process IS the worker or the kill switch (CBM_INDEX_SUPERVISOR=0)
     * is set. A production supervisor failure returns an error; it never falls
     * through to an unbounded in-process run. */
    if (cbm_index_supervisor_should_wrap()) {
        if (!wait_for_pipeline_lock(MCP_INDEX_LOCK_WAIT_MS)) {
            return cbm_mcp_text_result(
                "{\"status\":\"error\",\"error\":\"indexing is already active after waiting "
                "5 seconds\",\"hint\":\"Call index_status for the target project, then retry "
                "index_repository.\"}",
                true);
        }
        char *result =
            cross_repo ? index_repository_in_process(srv, args) : index_run_supervised(srv, args);
        cbm_pipeline_unlock();
        return result;
    }
    return index_repository_in_process(srv, args);
}

/* ── get_code_snippet ─────────────────────────────────────────── */

/* Copy a node from an array into a heap-allocated standalone node. */
static void copy_node(const cbm_node_t *src, cbm_node_t *dst) {
    dst->id = src->id;
    dst->project = heap_strdup(src->project);
    dst->label = heap_strdup(src->label);
    dst->name = heap_strdup(src->name);
    dst->qualified_name = heap_strdup(src->qualified_name);
    dst->file_path = heap_strdup(src->file_path);
    dst->start_line = src->start_line;
    dst->end_line = src->end_line;
    dst->properties_json = src->properties_json ? heap_strdup(src->properties_json) : NULL;
}

/* Build a JSON suggestions response for ambiguous or fuzzy results. */
static char *snippet_suggestions(const char *input, cbm_node_t *nodes, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "status", "ambiguous");

    char msg[CBM_SZ_512];
    snprintf(msg, sizeof(msg),
             "%d matches for \"%s\". Pick a qualified_name from suggestions below, "
             "or use search_graph(name_pattern=\"...\") to narrow results.",
             count, input);
    yyjson_mut_obj_add_str(doc, root, "message", msg);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *s = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, s, "qualified_name",
                               nodes[i].qualified_name ? nodes[i].qualified_name : "");
        yyjson_mut_obj_add_str(doc, s, "name", nodes[i].name ? nodes[i].name : "");
        yyjson_mut_obj_add_str(doc, s, "label", nodes[i].label ? nodes[i].label : "");
        yyjson_mut_obj_add_str(doc, s, "file_path", nodes[i].file_path ? nodes[i].file_path : "");
        yyjson_mut_arr_append(arr, s);
    }
    yyjson_mut_obj_add_val(doc, root, "suggestions", arr);

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    return mcp_text_result_take_owned(json, false);
}

/* Enrich a mutable JSON object with key-value pairs from a node's properties_json.
 * Returns the parsed yyjson_doc (caller frees AFTER serialization — zero-copy). */
/* Per-function complexity metrics: useful for a quality audit, noise for a
 * lookup. search_graph omits them unless include_metrics=true — they were
 * 14 of the ~20 keys behind its 3.4-7 KB replies. */
static bool node_property_is_metric(const char *k) {
    static const char *metrics[] = {"complexity",
                                    "cognitive",
                                    "loop_count",
                                    "loop_depth",
                                    "transitive_loop_depth",
                                    "self_recursive",
                                    "recursive",
                                    "param_count",
                                    "max_access_depth",
                                    "linear_scan_in_loop",
                                    "alloc_in_loop",
                                    "recursion_in_loop",
                                    "unguarded_recursion",
                                    "lines"};
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); i++) {
        if (strcmp(k, metrics[i]) == 0) {
            return true;
        }
    }
    return false;
}

static yyjson_doc *enrich_node_properties_ex(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                             const char *properties_json, bool skip_metrics,
                                             size_t docstring_cap);

static yyjson_doc *enrich_node_properties(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                          const char *properties_json) {
    return enrich_node_properties_ex(doc, obj, properties_json, false, 0);
}

/* Length of s cut to at most cap bytes, backed off to a UTF-8 lead byte so the
 * cut never splits a character. */
static size_t utf8_prefix_len(const char *s, size_t len, size_t cap) {
    if (len <= cap) {
        return len;
    }
    size_t n = cap;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

/* docstring_cap > 0 cuts a longer "docstring" to that many bytes and adds
 * "docstring_truncated": true; 0 keeps it whole. */
static yyjson_doc *enrich_node_properties_ex(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                             const char *properties_json, bool skip_metrics,
                                             size_t docstring_cap) {
    if (!properties_json || properties_json[0] == '\0') {
        return NULL;
    }
    yyjson_doc *props_doc = yyjson_read(properties_json, strlen(properties_json), 0);
    if (!props_doc) {
        return NULL;
    }
    yyjson_val *props_root = yyjson_doc_get_root(props_doc);
    if (!props_root || !yyjson_is_obj(props_root)) {
        yyjson_doc_free(props_doc);
        return NULL;
    }
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(props_root, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);
        const char *k = yyjson_get_str(key);
        if (!k) {
            continue;
        }
        if (skip_metrics && node_property_is_metric(k)) {
            continue;
        }
        if (yyjson_is_str(val) && docstring_cap > 0 && strcmp(k, "docstring") == 0 &&
            yyjson_get_len(val) > docstring_cap) {
            const char *ds = yyjson_get_str(val);
            yyjson_mut_obj_add_strn(doc, obj, k, ds,
                                    utf8_prefix_len(ds, yyjson_get_len(val), docstring_cap));
            yyjson_mut_obj_add_bool(doc, obj, "docstring_truncated", true);
        } else if (yyjson_is_str(val)) {
            yyjson_mut_obj_add_str(doc, obj, k, yyjson_get_str(val));
        } else if (yyjson_is_bool(val)) {
            yyjson_mut_obj_add_bool(doc, obj, k, yyjson_get_bool(val));
        } else if (yyjson_is_int(val)) {
            yyjson_mut_obj_add_int(doc, obj, k, yyjson_get_int(val));
        } else if (yyjson_is_real(val)) {
            yyjson_mut_obj_add_real(doc, obj, k, yyjson_get_real(val));
        }
    }
    return props_doc; /* caller frees after serialization */
}

/* Resolve an absolute path from root_path + file_path, verify containment,
 * and read source lines. Sets *out_abs_path (caller frees). Returns source
 * string (caller frees) or NULL if path is invalid/unreadable. */
/* True only when abs_path, after realpath/_fullpath resolution (which collapses
 * `..` and resolves symlinks/junctions), stays within root_path. This is the
 * single containment guard every MCP file-read sink must pass before reading a
 * file into a tool response: both the snippet path (resolve_snippet_source) and
 * the search path (attach_result_source) route through it, so a result whose
 * indexed path escapes the project root — via a `..` segment, or a symlink /
 * Windows junction picked up during discovery — is never read back out. */
/* Canonicalize `path` (resolve symlinks/junctions and `..`) into `out`
 * (>= CBM_SZ_4K bytes); returns true on success. Isolating the per-OS resolver
 * keeps cbm_path_within_root's control flow unconditional: the previous `#ifdef`
 * opened the `if (...) {` brace in one branch and a different one in the other,
 * sharing a single close brace — legal C, but it splits the function's braces
 * across preprocessor branches, which defeats source-level tooling that parses
 * without the preprocessor (and left this function unindexed in the graph). */
static bool resolve_canonical_path(const char *path, char *out, size_t out_sz) {
#ifdef _WIN32
    if (!_fullpath(out, path, out_sz)) {
        return false;
    }
    cbm_normalize_path_sep(out);
    return true;
#else
    (void)out_sz;
    return realpath(path, out) != NULL;
#endif
}

bool cbm_path_within_root(const char *root_path, const char *abs_path) {
    if (!root_path || !abs_path) {
        return false;
    }
    char real_root[CBM_SZ_4K];
    char real_file[CBM_SZ_4K];
    if (resolve_canonical_path(root_path, real_root, sizeof(real_root)) &&
        resolve_canonical_path(abs_path, real_file, sizeof(real_file))) {
        size_t root_len = strlen(real_root);
        if (strncmp(real_file, real_root, root_len) == 0 &&
            (real_file[root_len] == '/' || real_file[root_len] == '\0')) {
            return true;
        }
    }
    return false;
}

/* Upstream #2585 (1c1d5422, snippet half): a snippet's start/end are indexed
 * line numbers, but its bytes came from the file as it is now, so after an
 * edit above the symbol the same range held another symbol's code under the
 * requested name. file_hashes records the SHA-256 of the indexed bytes; the
 * file is read once, that buffer is hashed, and the lines are sliced from the
 * same buffer, so a concurrent write cannot produce a verified slice of bytes
 * that were never indexed.
 *   content_verified: the hash check ran (recorded hash + readable file).
 *   drift: it ran and the bytes differ; the source is withheld.
 *   freshness: "metadata_match" / "content_match" (bytes match; metadata
 *     agrees / is stale) or "content_changed" / "metadata_changed" (bytes
 *     differ; metadata agrees / differs). NULL when unverified.
 * Unverifiable files (no recorded hash, unreadable, larger than
 * SNIPPET_VERIFY_MAX_BYTES) keep the previous unverified read. */
enum { SNIPPET_VERIFY_MAX_BYTES = 16 * 1024 * 1024 };

struct snippet_source_check_t {
    bool content_verified = false;
    bool drift = false;
    const char *freshness = nullptr;
};

/* Whole file into a malloc'd buffer (NUL-terminated; *len excludes the NUL).
 * NULL on open/read failure, allocation failure, or past max_bytes. */
static char *snippet_read_whole_file(FILE *fp, size_t max_bytes, size_t *len) {
    *len = 0;
    size_t cap = CBM_SZ_4K;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return NULL;
    }
    for (;;) {
        if (*len + CBM_SZ_4K + SKIP_ONE > cap) {
            size_t want = cap * PAIR_LEN;
            char *grown = want > cap ? (char *)realloc(buf, want) : NULL;
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap = want;
        }
        size_t n = fread(buf + *len, 1, CBM_SZ_4K, fp);
        *len += n;
        if (*len > max_bytes) {
            free(buf);
            return NULL;
        }
        if (n < CBM_SZ_4K) {
            break;
        }
    }
    if (ferror(fp)) {
        free(buf);
        return NULL;
    }
    buf[*len] = '\0';
    return buf;
}

/* Lines [start, end] of an in-memory file, with read_file_lines' result shape
 * (text-mode line endings, NULL when the range is empty). */
static char *snippet_slice_lines(const char *bytes, size_t len, int start, int end) {
    size_t begin = len;
    size_t stop = len;
    int lineno = 1;
    for (size_t i = 0; i < len; i++) {
        if (lineno >= start && begin == len) {
            begin = i;
        }
        if (bytes[i] == '\n') {
            if (lineno >= end) {
                stop = i + 1;
                break;
            }
            lineno++;
        }
    }
    if (begin >= stop) {
        return NULL;
    }
    char *slice = (char *)malloc(stop - begin + 1);
    if (!slice) {
        return NULL;
    }
    size_t out = 0;
    for (size_t i = begin; i < stop; i++) {
#ifdef _WIN32
        /* read_file_lines opens in text mode. */
        if (bytes[i] == '\r' && i + 1 < stop && bytes[i + 1] == '\n') {
            continue;
        }
#endif
        slice[out++] = bytes[i];
    }
    slice[out] = '\0';
    if (out == 0) {
        free(slice);
        return NULL;
    }
    return slice;
}

/* The verified read: NULL source with check->drift set when the bytes are not
 * the indexed bytes; check->content_verified false when it could not run. */
static char *snippet_read_verified(cbm_store_t *store, const char *project, const char *file_path,
                                   const char *abs_path, int start, int end,
                                   snippet_source_check_t *check) {
    char stored_sha[CBM_SHA256_HEX_LEN + 1] = "";
    int64_t stored_mtime = 0;
    int64_t stored_size = -1;
    if (!store || !project ||
        cbm_store_get_file_hash(store, project, file_path, stored_sha, sizeof(stored_sha),
                                &stored_mtime, &stored_size) != CBM_STORE_OK ||
        !trace_sha256_valid(stored_sha)) {
        return read_file_lines(abs_path, start, end);
    }
    FILE *fp = cbm_fopen(abs_path, "rb");
    if (!fp) {
        return NULL; /* no file: nothing to serve, verified or not */
    }
    trace_fd_stat_t opened = {};
    bool stat_ok = trace_fstat(fp, &opened);
    size_t len = 0;
    char *bytes = stat_ok && opened.regular && opened.size >= 0 &&
                          (uint64_t)opened.size <= (uint64_t)SNIPPET_VERIFY_MAX_BYTES
                      ? snippet_read_whole_file(fp, SNIPPET_VERIFY_MAX_BYTES, &len)
                      : NULL;
    (void)fclose(fp);
    if (!bytes) {
        return read_file_lines(abs_path, start, end);
    }
    char current[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(bytes, len, current);
    bool same_bytes = true;
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; ++i) {
        if (current[i] != (char)tolower((unsigned char)stored_sha[i])) {
            same_bytes = false;
            break;
        }
    }
    bool same_meta = opened.mtime_ns == stored_mtime && opened.size == stored_size;
    check->content_verified = true;
    check->drift = !same_bytes;
    if (same_bytes) {
        check->freshness = same_meta ? "metadata_match" : "content_match";
    } else {
        check->freshness = same_meta ? "content_changed" : "metadata_changed";
    }
    char *source = same_bytes ? snippet_slice_lines(bytes, len, start, end) : NULL;
    free(bytes);
    return source;
}

/* store/project may be NULL: the read is then unverified, as before. */
static char *resolve_snippet_source(cbm_store_t *store, const char *project, const char *root_path,
                                    const char *file_path, int start, int end, char **out_abs_path,
                                    snippet_source_check_t *check) {
    *out_abs_path = NULL;
    *check = snippet_source_check_t{};
    if (!root_path || !file_path) {
        return NULL;
    }
    size_t apsz = strlen(root_path) + strlen(file_path) + MCP_SEPARATOR;
    char *abs_path = (char *)malloc(apsz);
    if (!abs_path) {
        return NULL;
    }
    snprintf(abs_path, apsz, "%s/%s", root_path, file_path);

    *out_abs_path = abs_path;
    if (cbm_path_within_root(root_path, abs_path)) {
        return snippet_read_verified(store, project, file_path, abs_path, start, end, check);
    }
    return NULL;
}

/* content_verified always; on drift the reason the source is withheld. */
static void add_snippet_source_check(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                     const snippet_source_check_t &check) {
    yyjson_mut_obj_add_bool(doc, obj, "content_verified", check.content_verified);
    if (check.drift) {
        yyjson_mut_obj_add_bool(doc, obj, "source_drift", true);
        yyjson_mut_obj_add_str(doc, obj, "freshness", check.freshness ? check.freshness : "");
        yyjson_mut_obj_add_str(doc, obj, "recommended_action", "reindex_then_retry");
    }
}

static bool utf8_is_cont(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

static char *sanitize_utf8_lossy(const char *s) {
    enum {
        UTF8_REPLACEMENT_LEN = 3,
        UTF8_THREE_BYTE_LEN = 3,
        UTF8_FOUR_BYTE_LEN = 4,
        UTF8_FOURTH_BYTE = 3,
    };
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    if (len > (((size_t)-1) - SKIP_ONE) / UTF8_REPLACEMENT_LEN) {
        return NULL;
    }
    char *out = (char *)malloc(len * UTF8_REPLACEMENT_LEN + SKIP_ONE);
    if (!out) {
        return NULL;
    }

    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;
    unsigned char *dst = (unsigned char *)out;
    while (p < end) {
        unsigned char c = *p;
        size_t n = 0;
        if (c < 0x80) {
            n = 1;
        } else if (c >= 0xC2 && c <= 0xDF && p + 1 < end && utf8_is_cont(p[1])) {
            n = 2;
        } else if (c == 0xE0 && p + 2 < end && p[1] >= 0xA0 && p[1] <= 0xBF && utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c >= 0xE1 && c <= 0xEC && p + 2 < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c == 0xED && p + 2 < end && p[1] >= 0x80 && p[1] <= 0x9F && utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c >= 0xEE && c <= 0xEF && p + 2 < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2])) {
            n = UTF8_THREE_BYTE_LEN;
        } else if (c == 0xF0 && p + UTF8_FOURTH_BYTE < end && p[1] >= 0x90 && p[1] <= 0xBF &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        } else if (c >= 0xF1 && c <= 0xF3 && p + UTF8_FOURTH_BYTE < end && utf8_is_cont(p[1]) &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        } else if (c == 0xF4 && p + UTF8_FOURTH_BYTE < end && p[1] >= 0x80 && p[1] <= 0x8F &&
                   utf8_is_cont(p[2]) && utf8_is_cont(p[UTF8_FOURTH_BYTE])) {
            n = UTF8_FOUR_BYTE_LEN;
        }

        if (n > 0) {
            memcpy(dst, p, n);
            dst += n;
            p += n;
        } else {
            *dst++ = 0xEF;
            *dst++ = 0xBF;
            *dst++ = 0xBD;
            p++;
        }
    }
    *dst = '\0';
    return out;
}

/* Optional current-source evidence for a trace. Keep indexed coordinates explicit:
 * reading current bytes is not proof that an old graph location is still correct. */
static char *trace_read_source(const char *root, const char *file, int start, int end,
                               size_t budget, int *actual_end, bool *clipped) {
    if (!root || !file) {
        return nullptr;
    }
    size_t path_size = strlen(root) + strlen(file) + 2;
    char *path = (char *)malloc(path_size);
    if (!path) {
        return nullptr;
    }
    snprintf(path, path_size, "%s/%s", root, file);
    FILE *fp = cbm_path_within_root(root, path) ? cbm_fopen(path, "r") : nullptr;
    free(path);
    if (!fp) {
        return nullptr;
    }
    char *source = (char *)malloc(budget + 1);
    if (!source) {
        fclose(fp);
        return nullptr;
    }
    size_t used = 0;
    int64_t line = 1;
    int ch;
    *actual_end = start;
    while (line <= end && (ch = fgetc(fp)) != EOF) {
        if (line >= start) {
            if (used == budget) {
                *clipped = true;
                break;
            }
            source[used++] = (char)ch;
            *actual_end = (int)line;
        }
        if (ch == '\n') {
            line++;
        }
    }
    if (ferror(fp) || *actual_end < end) {
        *clipped = true;
    }
    source[used] = '\0';
    fclose(fp);
    return source;
}

static void trace_attach_source(const char *project_root, const char *file, int start, int end,
                                bool clipped, int budget, yyjson_mut_doc *doc,
                                yyjson_mut_val *item) {
    yyjson_mut_obj_add_strcpy(doc, item, "source_range_basis", "indexed_range");
    yyjson_mut_obj_add_bool(doc, item, "current_definition_extent_validated", false);
    if (!file || start <= 0 || end < start) {
        yyjson_mut_obj_add_bool(doc, item, "source_unavailable", true);
        return;
    }
    int actual_end = end;
    char *source =
        trace_read_source(project_root, file, start, end, (size_t)budget, &actual_end, &clipped);
    char *safe = source ? sanitize_utf8_lossy(source) : nullptr;
    if (safe && safe[0]) {
        yyjson_mut_obj_add_strcpy(doc, item, "source", safe);
        yyjson_mut_obj_add_int(doc, item, "source_start_line", start);
        yyjson_mut_obj_add_int(doc, item, "source_end_line", actual_end);
        yyjson_mut_obj_add_bool(doc, item, "source_clipped", clipped);
        char *candidate = yy_doc_to_str(doc);
        if (!candidate || strlen(candidate) + 128 > (size_t)budget) {
            yyjson_mut_obj_remove_key(item, "source");
            yyjson_mut_obj_remove_key(item, "source_start_line");
            yyjson_mut_obj_remove_key(item, "source_end_line");
            yyjson_mut_obj_remove_key(item, "source_clipped");
            yyjson_mut_obj_add_bool(doc, item, "source_omitted", true);
        }
        free(candidate);
    } else {
        yyjson_mut_obj_add_bool(doc, item, "source_unavailable", true);
    }
    free(safe);
    free(source);
}

static void trace_add_source(cbm_mcp_server_t *srv, const char *project, yyjson_mut_doc *doc,
                             yyjson_mut_val *root, const char *args) {
    int lines = cbm_mcp_get_int_arg(args, "source_lines", 0);
    int context = cbm_mcp_get_int_arg(args, "source_context", 0);
    if (context > 10) {
        context = 10;
    }
    if (lines <= 0 && context <= 0) {
        return;
    }
    const char *mode = yyjson_mut_get_str(yyjson_mut_obj_get(root, "mode"));
    if (mode && strcmp(mode, "cross_service") == 0) {
        yyjson_mut_obj_add_str(
            doc, root, "source_note",
            "Source is unavailable in cross_service mode. Read each symbol using its own project.");
        yyjson_mut_obj_add_bool(doc, root, "source_unavailable", true);
        return;
    }
    if (lines > MCP_SNIPPET_MAX_LINES) {
        lines = MCP_SNIPPET_MAX_LINES;
    }
    int budget = cbm_mcp_get_int_arg(args, "max_bytes", 24000);
    if (budget < 1500) {
        budget = 1500;
    }
    char *project_root = get_project_root(srv, project);
    yyjson_mut_obj_add_str(
        doc, root, "source_note",
        "Current source at indexed locations; locations and relationships may be stale. "
        "source_range_basis=indexed_range; source_clipped=false only means that indexed range was "
        "read completely, not that the current definition extent was validated. source_clipped, "
        "source_unavailable or source_omitted require a focused source read.");
    if (context > 0) {
        for (const char *key : {"caller_edges", "callee_edges"}) {
            yyjson_mut_val *edges = yyjson_mut_obj_get(root, key);
            for (size_t i = 0; i < yyjson_mut_arr_size(edges); i++) {
                yyjson_mut_val *edge = yyjson_mut_arr_get(edges, i);
                const char *file = yyjson_mut_get_str(yyjson_mut_obj_get(edge, "from_file"));
                int64_t line = yyjson_mut_get_int(yyjson_mut_obj_get(edge, "line"));
                if (line <= 0 || line > INT32_MAX - context) {
                    yyjson_mut_obj_add_bool(doc, edge, "source_unavailable", true);
                    continue;
                }
                int start = line > context ? (int)line - context : 1;
                trace_attach_source(project_root, file, start, (int)line + context, false, budget,
                                    doc, edge);
            }
        }
    }
    if (lines <= 0) {
        free(project_root);
        return;
    }
    for (const char *key : {"definitions", "callers", "callees", "path"}) {
        yyjson_mut_val *arr = yyjson_mut_obj_get(root, key);
        size_t count = yyjson_mut_arr_size(arr);
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *node = yyjson_mut_arr_get(arr, i);
            const char *file = yyjson_mut_get_str(yyjson_mut_obj_get(node, "file"));
            int start = (int)yyjson_mut_get_int(yyjson_mut_obj_get(node, "start_line"));
            int end = (int)yyjson_mut_get_int(yyjson_mut_obj_get(node, "end_line"));
            if (!file || start <= 0 || end < start) {
                yyjson_mut_obj_add_bool(doc, node, "source_unavailable", true);
                continue;
            }
            bool clipped = (int64_t)end - start + 1 > lines;
            if (clipped) {
                end = start + lines - 1;
            }
            trace_attach_source(project_root, file, start, end, clipped, budget, doc, node);
        }
    }
    free(project_root);
}

/* Build an enriched snippet response for a resolved node. */
/* Add a string array to a JSON object (no-op if count == 0). */
static void add_string_array(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                             char **strings, int count) {
    if (count <= 0) {
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_str(doc, arr, strings[i]);
    }
    yyjson_mut_obj_add_val(doc, obj, key, arr);
}

/* get_code_snippet coverage note (#963): if the resolved node's file is
 * flagged parse_partial, warn that the graph may under-report this file.
 * Correlated by construction — the result names its file. (An entirely-
 * skipped file cannot appear here: it has no nodes to resolve a snippet
 * from.) */
static void add_snippet_coverage_note(yyjson_mut_doc *doc, yyjson_mut_val *root_obj,
                                      cbm_store_t *store, const cbm_node_t *node) {
    if (!node->file_path || !node->file_path[0] || !node->project) {
        return;
    }
    cbm_coverage_row_t *rows = NULL;
    int count = 0;
    if (cbm_store_coverage_get(store, node->project, &rows, &count) != CBM_STORE_OK) {
        return;
    }
    char skip_note[CBM_SZ_1K] = "";
    for (int i = 0; i < count; i++) {
        if (!rows[i].rel_path || strcmp(rows[i].rel_path, node->file_path) != 0 || !rows[i].kind) {
            continue;
        }
        char note[CBM_SZ_1K];
        if (strcmp(rows[i].kind, "parse_unusable") == 0) {
            snprintf(note, sizeof(note),
                     "The parse of this file failed across nearly the whole of it, so most "
                     "constructs are missing from the graph and naming line ranges would not "
                     "help. Read the source directly — the source above is ground truth. "
                     "(best-effort signal)");
        } else if (strcmp(rows[i].kind, "parse_partial") == 0) {
            snprintf(note, sizeof(note),
                     "This file was only PARTIALLY indexed — line range(s) %s could not be "
                     "parsed, so constructs there may be missing from the graph (callers/callees "
                     "and search results can under-report this file). The source above is ground "
                     "truth. (best-effort signal)",
                     rows[i].detail && rows[i].detail[0] ? rows[i].detail : "?");
        } else if (strcmp(rows[i].kind, "walk_truncated") == 0) {
            snprintf(note, sizeof(note),
                     "Extraction stopped partway through this file (%s), so calls and usages past "
                     "that point are missing from the graph. The source above is ground truth. "
                     "(best-effort signal)",
                     rows[i].detail && rows[i].detail[0] ? rows[i].detail : "?");
        } else if (strcmp(rows[i].kind, "lsp_skipped") == 0) {
            /* Weaker than the rows above: keep looking, so a partial parse or
             * a truncated walk of the same file still wins (as in the hook's
             * coverage note). */
            if (!skip_note[0]) {
                snprintf(skip_note, sizeof(skip_note),
                         "Type-aware call resolution was skipped for this file (%s), so its "
                         "callers/callees were matched by name only and may be missing or "
                         "ambiguous. The source above is ground truth. (best-effort signal)",
                         rows[i].detail && rows[i].detail[0] ? rows[i].detail : "?");
            }
            continue;
        } else {
            continue;
        }
        yyjson_mut_obj_add_strcpy(doc, root_obj, "coverage_note", note);
        skip_note[0] = '\0';
        break;
    }
    if (skip_note[0]) {
        yyjson_mut_obj_add_strcpy(doc, root_obj, "coverage_note", skip_note);
    }
    cbm_store_free_coverage(rows, count);
}

static char *build_snippet_response(cbm_mcp_server_t *srv, cbm_node_t *node,
                                    const char *match_method, bool include_neighbors,
                                    cbm_node_t *alternatives, int alt_count) {
    char *root_path = get_project_root(srv, node->project);

    int start = node->start_line > 0 ? node->start_line : SKIP_ONE;
    int end = node->end_line > start ? node->end_line : start + SNIPPET_DEFAULT_LINES;
    /* Context-bomb guard: a structural node (Module/File) spans its whole file,
     * so an unclipped read returned the ENTIRE source — a field-eval agent that
     * fell back to a Module snippet pulled 400KB in one call. Cap the line span
     * (far above any real function) and flag it; the exact range is still in
     * start_line/end_line for a targeted re-read. */
    bool snippet_clipped = false;
    if (end - start + 1 > MCP_SNIPPET_MAX_LINES) {
        end = start + MCP_SNIPPET_MAX_LINES - 1;
        snippet_clipped = true;
    }
    char *abs_path = NULL;
    snippet_source_check_t source_check;
    char *source = resolve_snippet_source(srv->store, node->project, root_path, node->file_path,
                                          start, end, &abs_path, &source_check);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    yyjson_mut_obj_add_str(doc, root_obj, "name", node->name ? node->name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "qualified_name",
                           node->qualified_name ? node->qualified_name : "");
    yyjson_mut_obj_add_str(doc, root_obj, "label", node->label ? node->label : "");

    const char *display_path = "";
    if (abs_path) {
        display_path = abs_path;
    } else if (node->file_path) {
        display_path = node->file_path;
    }
    yyjson_mut_obj_add_str(doc, root_obj, "file_path", display_path);
    yyjson_mut_obj_add_int(doc, root_obj, "start_line", start);
    yyjson_mut_obj_add_int(doc, root_obj, "end_line", end);
    add_snippet_source_check(doc, root_obj, source_check);
    if (snippet_clipped) {
        yyjson_mut_obj_add_bool(doc, root_obj, "source_clipped", true);
        yyjson_mut_obj_add_int(doc, root_obj, "clipped_at_lines", MCP_SNIPPET_MAX_LINES);
    }

    if (source) {
        char *safe_source = sanitize_utf8_lossy(source);
        if (safe_source) {
            yyjson_mut_obj_add_strcpy(doc, root_obj, "source", safe_source);
            free(safe_source);
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
        }
    } else if (source_check.drift) {
        yyjson_mut_obj_add_str(doc, root_obj, "source",
                               "(source withheld: the file changed since it was indexed, so the "
                               "indexed line range no longer holds this symbol; re-index, then "
                               "retry)");
    } else {
        yyjson_mut_obj_add_str(doc, root_obj, "source", "(source not available)");
    }

    /* match_method — omitted for exact matches */
    if (match_method) {
        yyjson_mut_obj_add_str(doc, root_obj, "match_method", match_method);
    }

    /* Enrich with node properties (freed AFTER serialization — zero-copy). */
    yyjson_doc *props_doc = enrich_node_properties(doc, root_obj, node->properties_json);

    /* Caller/callee counts — store already resolved by calling handler */
    cbm_store_t *store = srv->store;
    int in_deg = 0;
    int out_deg = 0;
    cbm_store_node_degree(store, node->id, &in_deg, &out_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callers", in_deg);
    yyjson_mut_obj_add_int(doc, root_obj, "callees", out_deg);

    add_snippet_coverage_note(doc, root_obj, store, node);

    char **nb_callers = NULL;
    int nb_caller_count = 0;
    char **nb_callees = NULL;
    int nb_callee_count = 0;
    if (include_neighbors) {
        cbm_store_node_neighbor_names(store, node->id, MCP_DEFAULT_LIMIT, &nb_callers,
                                      &nb_caller_count, &nb_callees, &nb_callee_count);
        add_string_array(doc, root_obj, "caller_names", nb_callers, nb_caller_count);
        add_string_array(doc, root_obj, "callee_names", nb_callees, nb_callee_count);
    }

    /* Alternatives (when auto-resolved from ambiguous) */
    if (alternatives && alt_count > 0) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (int i = 0; i < alt_count; i++) {
            yyjson_mut_val *a = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, a, "qualified_name",
                                   alternatives[i].qualified_name ? alternatives[i].qualified_name
                                                                  : "");
            yyjson_mut_obj_add_str(doc, a, "file_path",
                                   alternatives[i].file_path ? alternatives[i].file_path : "");
            yyjson_mut_arr_append(arr, a);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "alternatives", arr);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(props_doc); /* safe if NULL */
    for (int i = 0; i < nb_caller_count; i++) {
        free(nb_callers[i]);
    }
    for (int i = 0; i < nb_callee_count; i++) {
        free(nb_callees[i]);
    }
    free(nb_callers);
    free(nb_callees);
    free(root_path);
    free(abs_path);
    free(source);

    return mcp_text_result_take_owned(json, false);
}

static char *handle_get_code_snippet(cbm_mcp_server_t *srv, const char *args) {
    char *qn = cbm_mcp_get_string_arg(args, "qualified_name");
    char *project = get_project_arg_or_cwd(srv, args, true);
    bool include_neighbors = cbm_mcp_get_bool_arg(args, "include_neighbors");

    if (!qn) {
        free(project);
        return cbm_mcp_text_result("qualified_name is required", true);
    }

    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(qn);
        free(project);
        return _res;
    }

    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(qn);
        free(project);
        return not_indexed;
    }

    /* Default to current project (same as all other tools) */
    const char *effective_project = project ? project : srv->current_project;

    /* Tier 1: Exact QN match */
    cbm_node_t node = {0};
    int rc = cbm_store_find_node_by_qn(store, effective_project, qn, &node);
    if (rc == CBM_STORE_OK) {
        char *result = build_snippet_response(srv, &node, NULL, include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    /* Tier 2: Suffix match — handles partial QNs ("main.HandleRequest")
     * and short names ("ProcessOrder") via LIKE '%.X'. */
    cbm_node_t *suffix_nodes = NULL;
    int suffix_count = 0;
    cbm_store_find_nodes_by_qn_suffix(store, effective_project, qn, &suffix_nodes, &suffix_count);
    if (suffix_count == 0 && (strstr(qn, "::") || strchr(qn, '.'))) {
        /* "Class::method" / a partial QN with more scope than the graph keeps. */
        find_nodes_by_scoped_name(store, effective_project, qn, &suffix_nodes, &suffix_count);
    }

    /* Tier 3: the C-macro namespace. A macro's QN is `<module>.<NAME>#macro`
     * (CBM_MACRO_QN_SUFFIX), out of reach of '%.X'. Tried only when no
     * definition answered to the name above (tie rule), for a short name, a
     * partial QN, or the macro's plain QN. */
    char macro_qn[CBM_SZ_512];
    int macro_len = snprintf(macro_qn, sizeof(macro_qn), "%s" CBM_MACRO_QN_SUFFIX, qn);
    if (suffix_count == 0 && macro_len > 0 && (size_t)macro_len < sizeof(macro_qn)) {
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        suffix_nodes = NULL;
        cbm_store_find_nodes_by_qn_suffix(store, effective_project, macro_qn, &suffix_nodes,
                                          &suffix_count);
    }

    if (suffix_count == SKIP_ONE) {
        copy_node(&suffix_nodes[0], &node);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        char *result = build_snippet_response(srv, &node, "suffix", include_neighbors, NULL, 0);
        free_node_contents(&node);
        free(qn);
        free(project);
        return result;
    }

    if (suffix_count > SKIP_ONE) {
        /* Prefer the real definition (a .c body over a .h declaration, a Function
         * over a Module) so an unambiguous-by-preference match resolves directly
         * instead of forcing a disambiguation round trip; only a genuine tie still
         * returns suggestions. */
        bool snip_ambiguous = false;
        int ssel = pick_resolved_node(suffix_nodes, suffix_count, &snip_ambiguous);
        if (!snip_ambiguous) {
            copy_node(&suffix_nodes[ssel], &node);
            cbm_store_free_nodes(suffix_nodes, suffix_count);
            char *result = build_snippet_response(srv, &node, "suffix", include_neighbors, NULL, 0);
            free_node_contents(&node);
            free(qn);
            free(project);
            return result;
        }
        char *result = snippet_suggestions(qn, suffix_nodes, suffix_count);
        cbm_store_free_nodes(suffix_nodes, suffix_count);
        free(qn);
        free(project);
        return result;
    }

    cbm_store_free_nodes(suffix_nodes, suffix_count);
    free(qn);
    free(project);

    /* Nothing found — guide the caller toward search_graph */
    return cbm_mcp_text_result(
        "symbol not found. Use search_graph(name_pattern=\"...\") first to discover "
        "the exact qualified_name, then pass it to get_code_snippet.",
        true);
}

/* ── search_code v2: graph-augmented code search ─────────────── */

/* Intermediate grep match */
typedef struct {
    char file[CBM_SZ_512];
    int line;
    char content[CBM_SZ_1K];
} grep_match_t;

/* Deduped result: one per containing graph node */
typedef struct {
    int64_t node_id; /* 0 = raw match (no containing node) */
    char node_name[CBM_SZ_256];
    char qualified_name[CBM_SZ_512];
    char label[CBM_SZ_64];
    char file[CBM_SZ_512];
    int start_line;
    int end_line;
    int in_degree;
    int out_degree;
    int score;
    int match_lines[CBM_SZ_64];
    int match_count;
} search_result_t;

/* Score a result for ranking: project source first, vendored last, tests lowest */
enum { SCORE_FUNC = 10, SCORE_ROUTE = 15, SCORE_VENDORED = -50, SCORE_TEST = -5 };
enum { MAX_LINE_SPAN = 999999 };

static int compute_search_score(const search_result_t *r) {
    int score = r->in_degree;
    if (strcmp(r->label, "Function") == 0 || strcmp(r->label, "Method") == 0) {
        score += SCORE_FUNC;
    }
    if (strcmp(r->label, "Route") == 0) {
        score += SCORE_ROUTE;
    }
    if (strstr(r->file, "vendored/") || strstr(r->file, "vendor/") ||
        strstr(r->file, "node_modules/")) {
        score += SCORE_VENDORED;
    }
    /* Penalize test files */
    if (strstr(r->file, "test") || strstr(r->file, "spec") || strstr(r->file, "_test.")) {
        score += SCORE_TEST;
    }
    return score;
}

static bool search_result_score_greater(const search_result_t &a, const search_result_t &b) {
    return a.score > b.score; /* descending by score */
}

/* Moving an arbitrary file_pattern ahead of Select-String is NOT generally
 * results-preserving: the Windows path applies PowerShell -like to the full
 * MatchInfo.Path, while POSIX delegates glob semantics to grep --include.
 * Restrict the optimization to plain suffix globs, whose meaning cannot depend
 * on path-separator normalization or directory components. The original
 * post-scan filter stays in place as a second guard. */
bool cbm_search_code_file_pattern_can_prefilter(const char *file_pattern) {
    if (!file_pattern || file_pattern[0] != '*' || file_pattern[1] != '.' ||
        file_pattern[2] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)file_pattern + 2; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

/* Windows PowerShell 5.1 encodes stdout for a native-process pipe in the console
 * OEM codepage, so any character the inherited CP cannot carry (Cyrillic under
 * CP437/850, and so on) degrades to '?' before it ever reaches
 * collect_grep_matches — and WHETHER it degrades depends on the console the
 * server happened to inherit, which makes the bug look intermittent. Pin the
 * pipe to UTF-8 inside every generated command so raw search content is
 * codepage-independent. The read side is already safe: Select-String decodes
 * BOM-less UTF-8 via .NET StreamReader defaults. */
#define CBM_PS_UTF8_PRELUDE "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "

/* Build the grep/search command string based on scoped vs recursive mode.
 * On Windows, uses PowerShell Select-String with tab-delimited output.
 * On POSIX, uses grep with colon-delimited output. */
void cbm_search_code_build_grep_cmd(char *cmd, size_t cmd_sz, bool use_regex, bool scoped,
                                    const char *file_pattern, const char *tmpfile,
                                    const char *filelist, const char *root_path) {
#ifdef _WIN32
    const char *sm = use_regex ? "" : " -SimpleMatch";
    if (scoped) {
        if (file_pattern) {
            if (cbm_search_code_file_pattern_can_prefilter(file_pattern)) {
                /* Drop non-matching paths BEFORE Select-String opens them. */
                snprintf(
                    cmd, cmd_sz,
                    "powershell -Command \"" CBM_PS_UTF8_PRELUDE
                    "$pat = Get-Content -Encoding UTF8 -LiteralPath '%s'; "
                    "Get-Content -Encoding UTF8 -LiteralPath '%s'"
                    " | Where-Object { $_ -like '%s' }"
                    " | ForEach-Object { Select-String -LiteralPath $_ -Pattern $pat%s "
                    "-ErrorAction SilentlyContinue }"
                    " | Where-Object { $_.Path -like '*%s' }"
                    " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }\"",
                    tmpfile, filelist, file_pattern, sm, file_pattern);
            } else {
                snprintf(
                    cmd, cmd_sz,
                    "powershell -Command \"" CBM_PS_UTF8_PRELUDE
                    "$pat = Get-Content -Encoding UTF8 -LiteralPath '%s'; "
                    "Get-Content -Encoding UTF8 -LiteralPath '%s' | ForEach-Object { Select-String "
                    "-LiteralPath $_ -Pattern $pat%s "
                    "-ErrorAction SilentlyContinue }"
                    " | Where-Object { $_.Path -like '*%s' }"
                    " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }\"",
                    tmpfile, filelist, sm, file_pattern);
            }
        } else {
            snprintf(
                cmd, cmd_sz,
                "powershell -Command \"" CBM_PS_UTF8_PRELUDE
                "$pat = Get-Content -Encoding UTF8 -LiteralPath '%s'; "
                "Get-Content -Encoding UTF8 -LiteralPath '%s' | ForEach-Object { Select-String "
                "-LiteralPath $_ -Pattern $pat%s "
                "-ErrorAction SilentlyContinue }"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }\"",
                tmpfile, filelist, sm);
        }
    } else {
        if (file_pattern) {
            snprintf(
                cmd, cmd_sz,
                "powershell -Command \"" CBM_PS_UTF8_PRELUDE
                "Get-ChildItem -Recurse -Path '%s\\*' -Include '%s' -File "
                "-ErrorAction SilentlyContinue"
                " | Select-String -Pattern (Get-Content -Encoding UTF8 -LiteralPath '%s')%s "
                "-ErrorAction SilentlyContinue"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }\"",
                root_path, file_pattern, tmpfile, sm);
        } else {
            snprintf(
                cmd, cmd_sz,
                "powershell -Command \"" CBM_PS_UTF8_PRELUDE
                "Get-ChildItem -Recurse -Path '%s\\*' -File -ErrorAction "
                "SilentlyContinue"
                " | Select-String -Pattern (Get-Content -Encoding UTF8 -LiteralPath '%s')%s "
                "-ErrorAction SilentlyContinue"
                " | ForEach-Object { $_.Path + [char]9 + $_.LineNumber + [char]9 + $_.Line }\"",
                root_path, tmpfile, sm);
        }
    }
#else
    const char *flag = use_regex ? "-E" : "-F";
    if (scoped) {
        if (file_pattern) {
            /* -0: read NUL-separated paths from the filelist so paths containing
             * spaces stay one argument (issue #687). Pairs with the NUL separator
             * written by write_scoped_filelist. */
            /* -d skip: a stale directory operand (an incremental store can keep
             * structural paths) is skipped instead of being read as a file. */
            snprintf(cmd, cmd_sz,
                     "xargs -0 grep -Hn -d skip %s --include='%s' -f '%s' < '%s' 2>/dev/null", flag,
                     file_pattern, tmpfile, filelist);
        } else {
            snprintf(cmd, cmd_sz, "xargs -0 grep -Hn -d skip %s -f '%s' < '%s' 2>/dev/null", flag,
                     tmpfile, filelist);
        }
    } else {
        if (file_pattern) {
            snprintf(cmd, cmd_sz, "grep -rn %s --include='%s' -f '%s' '%s' 2>/dev/null", flag,
                     file_pattern, tmpfile, root_path);
        } else {
            snprintf(cmd, cmd_sz, "grep -rn %s -f '%s' '%s' 2>/dev/null", flag, tmpfile, root_path);
        }
    }
#endif
}

/* Build deduplicated file list from search results + raw matches. */
static yyjson_mut_val *build_dedup_files_array(yyjson_mut_doc *doc, search_result_t *sr,
                                               int output_count, grep_match_t *raw, int raw_count) {
    yyjson_mut_val *files_arr = yyjson_mut_arr(doc);
    char *seen_files[CBM_SZ_512];
    int seen_count = 0;
    for (int fi = 0; fi < output_count; fi++) {
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_files[j], sr[fi].file) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup && seen_count < CBM_SZ_512) {
            seen_files[seen_count++] = sr[fi].file;
            yyjson_mut_arr_add_str(doc, files_arr, sr[fi].file);
        }
    }
    for (int fi = 0; fi < raw_count && seen_count < CBM_SZ_512; fi++) {
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_files[j], raw[fi].file) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            seen_files[seen_count++] = raw[fi].file;
            yyjson_mut_arr_add_str(doc, files_arr, raw[fi].file);
        }
    }
    return files_arr;
}

/* Attach source or context lines to a search result JSON item. */
static void attach_result_source(yyjson_mut_doc *doc, yyjson_mut_val *item, search_result_t *r,
                                 int mode, int context_lines, const char *root_path) {
    enum { MODE_FULL = 1 };
    if (r->start_line <= 0 || r->end_line <= 0) {
        return;
    }
    char abs_path[CBM_SZ_1K];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", root_path, r->file);

    /* Containment: a search result whose indexed path resolves outside the
     * project root (a `..` segment, or a symlink/junction that discovery
     * followed) must not be read back into the response. Same guard the
     * snippet path already uses. */
    if (!cbm_path_within_root(root_path, abs_path)) {
        return;
    }

    if (mode == MODE_FULL) {
        /* Full mode used to return the entire node body — a ten-result reply
         * measured 55 KB. Return the part of the body that surrounds the
         * matches instead: from SEARCH_FULL_MARGIN lines before the first hit
         * to SEARCH_FULL_MARGIN after the last, clipped to the node and to
         * SEARCH_FULL_MAX_LINES. The exact node range stays in
         * start_line/end_line for get_code_snippet. */
        enum { SEARCH_FULL_MARGIN = 20, SEARCH_FULL_MAX_LINES = 80 };
        int s = r->start_line;
        int e = r->end_line;
        if (r->match_count > 0) {
            int ws = r->match_lines[0] - SEARCH_FULL_MARGIN;
            int we = r->match_lines[r->match_count - SKIP_ONE] + SEARCH_FULL_MARGIN;
            if (ws > s) {
                s = ws;
            }
            if (we < e) {
                e = we;
            }
        }
        bool clipped = s != r->start_line || e != r->end_line;
        if (e - s + 1 > SEARCH_FULL_MAX_LINES) {
            e = s + SEARCH_FULL_MAX_LINES - 1;
            clipped = true;
        }
        char *source = read_file_lines(abs_path, s, e);
        if (source) {
            /* Lossy UTF-8 repair, not ASCII stripping: sanitize_ascii replaced
             * every byte > 127 with '?', so any non-English identifier, string
             * or comment came back mangled even though it was perfectly valid
             * UTF-8 and perfectly valid JSON. */
            char *safe_source = sanitize_utf8_lossy(source);
            if (safe_source) {
                yyjson_mut_obj_add_strcpy(doc, item, "source", safe_source);
                yyjson_mut_obj_add_int(doc, item, "source_start_line", s);
                yyjson_mut_obj_add_int(doc, item, "source_end_line", e);
                if (clipped) {
                    yyjson_mut_obj_add_bool(doc, item, "source_clipped", true);
                }
                free(safe_source);
            }
            free(source);
        }
    } else if (context_lines > 0 && r->match_count > 0) {
        /* Bounded here as well as at the handler: the window must stay
         * match +/- MCP_SEARCH_CONTEXT_MAX_LINES for any caller, and the
         * arithmetic must not overflow for any value. */
        int ctx_lines = context_lines > MCP_SEARCH_CONTEXT_MAX_LINES ? MCP_SEARCH_CONTEXT_MAX_LINES
                                                                     : context_lines;
        int first_match = r->match_lines[0];
        int last_match = r->match_lines[r->match_count - SKIP_ONE];
        int ctx_start = first_match > ctx_lines ? first_match - ctx_lines : SKIP_ONE;
        int ctx_end = last_match > INT_MAX - ctx_lines ? INT_MAX : last_match + ctx_lines;
        char *ctx = read_file_lines(abs_path, ctx_start, ctx_end);
        if (ctx) {
            char *safe_context = sanitize_utf8_lossy(ctx);
            if (safe_context) {
                yyjson_mut_obj_add_strcpy(doc, item, "context", safe_context);
                yyjson_mut_obj_add_int(doc, item, "context_start", ctx_start);
                free(safe_context);
            }
            free(ctx);
        }
    }
}

/* Build directory distribution object from search results (top-level dir → count). */
static yyjson_mut_val *build_dir_distribution(yyjson_mut_doc *doc, search_result_t *sr,
                                              int sr_count) {
    yyjson_mut_val *dirs = yyjson_mut_obj(doc);
    char dir_names[CBM_SZ_64][CBM_SZ_128];
    int dir_counts[CBM_SZ_64];
    int dir_n = 0;
    for (int di = 0; di < sr_count; di++) {
        char top[CBM_SZ_128] = "";
        const char *slash = strchr(sr[di].file, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - sr[di].file + SKIP_ONE);
            if (dlen >= sizeof(top)) {
                dlen = sizeof(top) - SKIP_ONE;
            }
            memcpy(top, sr[di].file, dlen);
            top[dlen] = '\0';
        } else {
            snprintf(top, sizeof(top), "%s", sr[di].file);
        }
        int found = CBM_NOT_FOUND;
        for (int d = 0; d < dir_n; d++) {
            if (strcmp(dir_names[d], top) == 0) {
                found = d;
                break;
            }
        }
        if (found >= 0) {
            dir_counts[found]++;
        } else if (dir_n < CBM_SZ_64) {
            snprintf(dir_names[dir_n], sizeof(dir_names[0]), "%s", top);
            dir_counts[dir_n] = SKIP_ONE;
            dir_n++;
        }
    }
    for (int d = 0; d < dir_n; d++) {
        yyjson_mut_val *key = yyjson_mut_strcpy(doc, dir_names[d]);
        yyjson_mut_val *val = yyjson_mut_int(doc, dir_counts[d]);
        yyjson_mut_obj_add(dirs, key, val);
    }
    return dirs;
}

/* Hint on a search whose '/' file_pattern matched no file (upstream #2836). */
#define SEARCH_CODE_PATH_HINT                                                            \
    "no results; a file_pattern with '/' must match the whole repo-relative path (e.g. " \
    "src/**/*.c); to scope by folder you can also use path_filter (regex on the "        \
    "repo-relative path, e.g. ^src/cli/)"

/* Phase 4: assemble JSON output from search results */
static char *assemble_search_output_once(search_result_t *sr, int sr_count, grep_match_t *raw,
                                         int raw_count, int gm_count, int output_count, int mode,
                                         int context_lines, const char *root_path,
                                         bool warn_literal_pipe, bool hint_path_filter,
                                         uint64_t elapsed_ms, int requested, int max_bytes,
                                         bool truncated, int raw_limit) {
    enum { MODE_COMPACT = 0, MODE_FULL = 1, MODE_FILES = 2, SEARCH_SLOW_MS = 5000 };

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    if (truncated) {
        yyjson_mut_obj_add_bool(doc, root_obj, "truncated", true);
        yyjson_mut_obj_add_int(doc, root_obj, "shown", output_count);
        char hint[CBM_SZ_512];
        snprintf(hint, sizeof(hint),
                 "Reply cut to fit max_bytes=%d: %d of %d requested results shown (%d matched). "
                 "Narrow with path_filter/file_pattern, use mode=\"compact\" or mode=\"files\", "
                 "or raise max_bytes.",
                 max_bytes, output_count, requested < sr_count ? requested : sr_count, sr_count);
        yyjson_mut_obj_add_strcpy(doc, root_obj, "continuation", hint);
    }

    if (mode == MODE_FILES) {
        yyjson_mut_obj_add_val(doc, root_obj, "files",
                               build_dedup_files_array(doc, sr, output_count, raw, raw_count));
    } else {
        yyjson_mut_val *results_arr = yyjson_mut_arr(doc);
        for (int ri = 0; ri < output_count; ri++) {
            search_result_t *r = &sr[ri];
            yyjson_mut_val *item = yyjson_mut_obj(doc);

            yyjson_mut_obj_add_str(doc, item, "node", r->node_name);
            yyjson_mut_obj_add_str(doc, item, "qualified_name", r->qualified_name);
            yyjson_mut_obj_add_str(doc, item, "label", r->label);
            yyjson_mut_obj_add_str(doc, item, "file", r->file);
            yyjson_mut_obj_add_int(doc, item, "start_line", r->start_line);
            yyjson_mut_obj_add_int(doc, item, "end_line", r->end_line);
            yyjson_mut_obj_add_int(doc, item, "in_degree", r->in_degree);
            yyjson_mut_obj_add_int(doc, item, "out_degree", r->out_degree);

            yyjson_mut_val *ml = yyjson_mut_arr(doc);
            for (int j = 0; j < r->match_count; j++) {
                yyjson_mut_arr_add_int(doc, ml, r->match_lines[j]);
            }
            yyjson_mut_obj_add_val(doc, item, "match_lines", ml);
            attach_result_source(doc, item, r, mode, context_lines, root_path);
            yyjson_mut_arr_add_val(results_arr, item);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "results", results_arr);

        yyjson_mut_val *raw_arr = yyjson_mut_arr(doc);
        int raw_output = std::min(raw_count, raw_limit);
        for (int ri = 0; ri < raw_output; ri++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, item, "file", raw[ri].file);
            yyjson_mut_obj_add_int(doc, item, "line", raw[ri].line);
            yyjson_mut_obj_add_str(doc, item, "content", raw[ri].content);
            yyjson_mut_arr_add_val(raw_arr, item);
        }
        yyjson_mut_obj_add_val(doc, root_obj, "raw_matches", raw_arr);
    }

    yyjson_mut_obj_add_val(doc, root_obj, "directories", build_dir_distribution(doc, sr, sr_count));

    /* Summary stats */
    yyjson_mut_obj_add_int(doc, root_obj, "total_grep_matches", gm_count);
    yyjson_mut_obj_add_int(doc, root_obj, "total_results", sr_count);
    yyjson_mut_obj_add_int(doc, root_obj, "raw_match_count", raw_count);
    yyjson_mut_obj_add_int(doc, root_obj, "elapsed_ms", (int)elapsed_ms);
    if (sr_count > 0 && gm_count > 0) {
        char ratio[CBM_SZ_32];
        snprintf(ratio, sizeof(ratio), "%.1fx", (double)gm_count / (double)(sr_count + raw_count));
        yyjson_mut_obj_add_strcpy(doc, root_obj, "dedup_ratio", ratio);
    }

    /* Warnings: surface common foot-guns instead of leaving them silent. */
    yyjson_mut_val *warnings = yyjson_mut_arr(doc);
    if (warn_literal_pipe) {
        yyjson_mut_arr_add_strcpy(
            doc, warnings,
            "pattern contains '|' but regex=false, so it is matched literally (not as "
            "alternation). Pass regex=true for 'foo|bar' to mean 'foo OR bar'.");
    }
    if (elapsed_ms >= SEARCH_SLOW_MS) {
        char slow[CBM_SZ_128];
        snprintf(slow, sizeof(slow),
                 "search took %dms (>%ds); narrow file_pattern/path_filter or use a more "
                 "specific pattern",
                 (int)elapsed_ms, SEARCH_SLOW_MS / 1000);
        yyjson_mut_arr_add_strcpy(doc, warnings, slow);
        char ems[CBM_SZ_32];
        snprintf(ems, sizeof(ems), "%d", (int)elapsed_ms);
        cbm_log_warn("search.slow", "elapsed_ms", ems); /* visibility in logs */
    }
    if (yyjson_mut_arr_size(warnings) > 0) {
        yyjson_mut_obj_add_val(doc, root_obj, "warnings", warnings);
    }
    if (hint_path_filter) {
        yyjson_mut_obj_add_str(doc, root_obj, "hint", SEARCH_CODE_PATH_HINT);
    }

    char *json = yy_doc_to_str(doc);
    if (json) {
        char *safe_json = sanitize_utf8_lossy(json);
        if (safe_json) {
            free(json);
            json = safe_json;
        }
    }
    yyjson_mut_doc_free(doc);
    return json;
}

/* Byte-budgeted assembly: result COUNT alone never bounded the reply (ten
 * full-mode results reached 55 KB), so the payload is rebuilt with fewer
 * results until it fits max_bytes, and says so with a continuation hint. */
static char *assemble_search_output(search_result_t *sr, int sr_count, grep_match_t *raw,
                                    int raw_count, int gm_count, int limit, int mode,
                                    int context_lines, const char *root_path,
                                    bool warn_literal_pipe, bool hint_path_filter,
                                    uint64_t elapsed_ms, int max_bytes) {
    enum { SEARCH_SHRINK_ROUNDS = 12, SEARCH_MAX_RAW = 20 };
    int output_count = std::min(sr_count, limit);
    int raw_limit = SEARCH_MAX_RAW;
    bool truncated = false;
    char *json = NULL;
    for (int round = 0; round <= SEARCH_SHRINK_ROUNDS; round++) {
        free(json);
        json = assemble_search_output_once(sr, sr_count, raw, raw_count, gm_count, output_count,
                                           mode, context_lines, root_path, warn_literal_pipe,
                                           hint_path_filter, elapsed_ms, limit, max_bytes,
                                           truncated, raw_limit);
        if (!json || max_bytes <= 0 || (int)strlen(json) <= max_bytes) {
            break;
        }
        /* Shrink the enriched results first, then the raw (unmatched) hits —
         * a search that maps to no graph node has only raw hits to cut. */
        if (output_count > 0) {
            output_count = output_count > 1 ? output_count / 2 : 0;
        } else if (raw_limit > 0) {
            raw_limit = raw_limit > 1 ? raw_limit / 2 : 0;
        } else {
            break;
        }
        truncated = true;
    }
    char *result = cbm_mcp_text_result(json ? json : "{}", false);
    free(json);
    return result;
}

/* Read grep output from fp, parse file:line:content format, apply path filter,
 * and return a dynamically-allocated grep_match_t array. */
/* Strip root path prefix from a file path. */
static const char *strip_root_prefix(const char *path, const char *root, size_t root_len) {
    if (strncmp(path, root, root_len) != 0) {
        return path;
    }
    const char *p = path + root_len;
    if (*p == '/') {
        p++;
    }
    return p;
}

/* True when "<root_path>/<file>" resolves inside root_path. The scan stream
 * carries whatever its walker opened: the scoped list is the index's paths,
 * and an indexed path that passes through a directory link is a regular file
 * to lstat, so grep reads the link's target. This is the containment check
 * attach_result_source already applies before it reads a result's source,
 * applied to the match itself (upstream f7b541ee). */
static bool search_hit_file_within_root(const char *root_path, const char *file) {
    char abs_path[CBM_SZ_4K];
    int length = snprintf(abs_path, sizeof(abs_path), "%s/%s", root_path, file);
    if (length < 0 || (size_t)length >= sizeof(abs_path)) {
        return false;
    }
    return cbm_path_within_root(root_path, abs_path);
}

/* `check_containment`: drop the hits of a file that resolves outside the
 * root, checked once per distinct file (grep groups a file's hits); dropped
 * hits are not counted, like a path_filter miss. The in-process scan has
 * already applied it and passes false. */
static grep_match_t *collect_grep_matches(FILE *fp, const char *root_path, size_t root_len,
                                          bool has_path_filter, cbm_regex_t *path_regex,
                                          int grep_limit, bool check_containment, int *out_count) {
    int gm_cap = CBM_SZ_64;
    int gm_count = 0;
    grep_match_t *gm = (grep_match_t *)malloc(gm_cap * sizeof(grep_match_t));
    char line[CBM_SZ_2K];
    std::string current_file;
    bool current_inside = false;
    bool have_current = false;

    while (fgets(line, sizeof(line), fp) && gm_count < grep_limit) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* PowerShell output uses tab as delimiter (paths may contain colons
         * on Windows, e.g. C:\dir\file). Unix grep uses colon. */
#ifdef _WIN32
        char sep = '\t';
#else
        char sep = ':';
#endif
        char *sep1 = strchr(line, (unsigned char)sep);
        if (!sep1) {
            continue;
        }
        char *sep2 = strchr(sep1 + SKIP_ONE, (unsigned char)sep);
        if (!sep2) {
            continue;
        }
        *sep1 = '\0';
        *sep2 = '\0';

#ifdef _WIN32
        cbm_normalize_path_sep(line);
#endif
        const char *path = line;
        const char *file = strip_root_prefix(path, root_path, root_len);

        if (has_path_filter && cbm_regexec(path_regex, file, 0, NULL, 0) != CBM_REG_OK) {
            continue;
        }
        if (check_containment) {
            if (!have_current || current_file != file) {
                current_file = file;
                have_current = true;
                current_inside = search_hit_file_within_root(root_path, file);
            }
            if (!current_inside) {
                continue;
            }
        }

        safe_grow(gm, gm_count, gm_cap, PAIR_LEN);
        snprintf(gm[gm_count].file, sizeof(gm[0].file), "%s", file);
        gm[gm_count].line = (int)strtol(sep1 + SKIP_ONE, NULL, CBM_DECIMAL_BASE);
        char *safe_content = sanitize_utf8_lossy(sep2 + SKIP_ONE);
        snprintf(gm[gm_count].content, sizeof(gm[0].content), "%s",
                 safe_content ? safe_content : sep2 + SKIP_ONE);
        free(safe_content);
        gm_count++;
    }

    *out_count = gm_count;
    return gm;
}

/* Find the tightest node containing a line in a file. Returns index or -1. */
static int find_tightest_node(cbm_node_t *nodes, int count, int line) {
    int best = CBM_NOT_FOUND;
    int best_span = MAX_LINE_SPAN;
    for (int j = 0; j < count; j++) {
        if (nodes[j].start_line <= line && nodes[j].end_line >= line) {
            int span = nodes[j].end_line - nodes[j].start_line;
            if (span < best_span) {
                best = j;
                best_span = span;
            }
        }
    }
    return best;
}

/* Add a grep hit to the search result set (merge into existing or create new). */
static void add_to_search_results(search_result_t **sr, int *sr_count, int *sr_cap, cbm_node_t *n,
                                  int line) {
    for (int j = 0; j < *sr_count; j++) {
        if ((*sr)[j].node_id == n->id) {
            if ((*sr)[j].match_count < CBM_SZ_64) {
                (*sr)[j].match_lines[(*sr)[j].match_count++] = line;
            }
            return;
        }
    }
    if (*sr_count >= *sr_cap) {
        *sr_cap *= PAIR_LEN;
        *sr = (__typeof__(*sr))safe_realloc(*sr, *sr_cap * sizeof(search_result_t));
        memset(&(*sr)[*sr_count], 0, (*sr_cap - *sr_count) * sizeof(search_result_t));
    }
    search_result_t *r = &(*sr)[*sr_count];
    r->node_id = n->id;
    snprintf(r->node_name, sizeof(r->node_name), "%s", n->name ? n->name : "");
    snprintf(r->qualified_name, sizeof(r->qualified_name), "%s",
             n->qualified_name ? n->qualified_name : "");
    snprintf(r->label, sizeof(r->label), "%s", n->label ? n->label : "");
    snprintf(r->file, sizeof(r->file), "%s", n->file_path ? n->file_path : "");
    r->start_line = n->start_line;
    r->end_line = n->end_line;
    r->match_lines[0] = line;
    r->match_count = SKIP_ONE;
    (*sr_count)++;
}

/* Match a single grep hit to the tightest containing node, then add to sr or raw. */
static void classify_grep_hit(grep_match_t *hit, cbm_node_t *file_nodes, int file_node_count,
                              search_result_t **sr, int *sr_count, int *sr_cap, grep_match_t **raw,
                              int *raw_count, int *raw_cap) {
    int best = find_tightest_node(file_nodes, file_node_count, hit->line);
    if (best >= 0) {
        add_to_search_results(sr, sr_count, sr_cap, &file_nodes[best], hit->line);
    } else {
        if (*raw_count >= *raw_cap) {
            *raw_cap = (*raw_cap == 0) ? CBM_SZ_32 : *raw_cap * PAIR_LEN;
            *raw = (__typeof__(*raw))safe_realloc(*raw, *raw_cap * sizeof(grep_match_t));
        }
        if (*raw) {
            (*raw)[(*raw_count)++] = *hit;
        }
    }
}

/* Free a file_nodes array returned from cbm_store_find_nodes_by_file. */
static void free_file_nodes(cbm_node_t *nodes, int count) {
    for (int j = 0; j < count; j++) {
        safe_str_free(&nodes[j].project);
        safe_str_free(&nodes[j].label);
        safe_str_free(&nodes[j].name);
        safe_str_free(&nodes[j].qualified_name);
        safe_str_free(&nodes[j].file_path);
        safe_str_free(&nodes[j].properties_json);
    }
    free(nodes);
}

/* Classify all grep matches file-by-file into search results and raw hits. */
static void classify_all_grep_hits(grep_match_t *gm, int gm_count, cbm_store_t *store,
                                   const char *project, search_result_t **sr, int *sr_count,
                                   int *sr_cap, grep_match_t **raw, int *raw_count, int *raw_cap) {
    std::sort(gm, gm + gm_count, [](const grep_match_t &a, const grep_match_t &b) {
        return strcmp(a.file, b.file) < 0;
    });
    int i = 0;
    while (i < gm_count) {
        const char *cur_file = gm[i].file;
        int file_start = i;
        while (i < gm_count && strcmp(gm[i].file, cur_file) == 0) {
            i++;
        }
        cbm_node_t *file_nodes = NULL;
        int file_node_count = 0;
        if (store) {
            cbm_store_find_nodes_by_file(store, project, cur_file, &file_nodes, &file_node_count);
        }
        for (int mi = file_start; mi < i; mi++) {
            classify_grep_hit(&gm[mi], file_nodes, file_node_count, sr, sr_count, sr_cap, raw,
                              raw_count, raw_cap);
        }
        free_file_nodes(file_nodes, file_node_count);
    }
}

/* Write indexed file list for scoped grep. Returns true if scoped.
 * When a path_filter is provided, apply it here — before grep — so large
 * indexed projects do not scan files only for collect_grep_matches to discard
 * them later. The predicate is IDENTICAL to the post-grep filter: the same
 * compiled regex run against the same root-relative path (separators
 * normalized on Windows first), so prefiltering can only skip files whose
 * hits would be dropped anyway — results-preserving by construction.
 * *out_written receives the number of records written (0 = the filter
 * excluded every indexed file). */
/* A file_pattern containing '/' is matched against the whole repo-relative path, like
 * search_graph's file_pattern (upstream #2836): '*' and '**' match any run of characters
 * including '/', a '**' followed by '/' also matches zero directories, '?' matches one
 * character, and a pattern without wildcards is a substring match. Portable (no fnmatch
 * on Windows) and linear: every '*' is equivalent, so one backtrack point suffices. */
static bool search_code_path_glob_match(const char *pat, const char *path) {
    if (!strpbrk(pat, "*?")) {
        return strstr(path, pat) != NULL;
    }
    const char *star_pat = NULL;
    const char *star_path = NULL;
    for (;;) {
        if (*pat == '*') {
            const char *run = pat;
            while (*pat == '*') {
                pat++;
            }
            if (pat - run > SKIP_ONE && *pat == '/') {
                pat++;
            }
            star_pat = pat;
            star_path = path;
        } else if (*path && (*pat == '?' || *pat == *path)) {
            pat++;
            path++;
        } else if (!*pat && !*path) {
            return true;
        } else if (star_pat && *star_path) {
            pat = star_pat;
            path = ++star_path;
        } else {
            return false;
        }
    }
}

/* True when search_code matches this file_pattern against the repo-relative path
 * (it names a directory) instead of the file's base name. */
static bool search_code_file_pattern_is_path_glob(const char *file_pattern) {
    return file_pattern && strchr(file_pattern, '/') != NULL;
}

/* path_glob: a file_pattern with '/' (search_code_file_pattern_is_path_glob), matched
 * here against each repo-relative path on every platform; NULL when there is none, and
 * a base-name file_pattern is left to the scanner / grep --include / PowerShell -like. */
static bool write_scoped_filelist(cbm_mcp_server_t *srv, const char *project, const char *root_path,
                                  const char *filelist, bool has_path_filter,
                                  cbm_regex_t *path_regex, const char *path_glob,
                                  int *out_written) {
    *out_written = 0;
    cbm_store_t *pre_store = resolve_store(srv, project);
    if (!pre_store) {
        return false;
    }
    char **indexed_files = NULL;
    int indexed_count = 0;
    if (cbm_store_list_files(pre_store, project, &indexed_files, &indexed_count) != CBM_STORE_OK ||
        indexed_count == 0) {
        return false;
    }
    FILE *fl = fopen(filelist, "wb");
    bool ok = false;
    int written = 0;
    if (fl) {
        for (int fi = 0; fi < indexed_count; fi++) {
            /* A source path never legitimately contains a newline or carriage
             * return. Those bytes are exactly the record separator on the
             * Windows filelist (and would split naive line readers elsewhere),
             * so a crafted indexed path with an embedded newline could inject
             * an extra entry into the scan set. Skip such paths entirely. */
            if (strpbrk(indexed_files[fi], "\r\n") != NULL) {
                continue;
            }
            if (has_path_filter && path_regex) {
#ifdef _WIN32
                cbm_normalize_path_sep(indexed_files[fi]);
#endif
                if (cbm_regexec(path_regex, indexed_files[fi], 0, NULL, 0) != CBM_REG_OK) {
                    continue;
                }
            }
            if (path_glob) {
#ifdef _WIN32
                cbm_normalize_path_sep(indexed_files[fi]);
#endif
                if (!search_code_path_glob_match(path_glob, indexed_files[fi])) {
                    continue;
                }
            }
            /* Write "<root>/<file>" piece-by-piece (no fixed-size buffer, so an
             * arbitrarily long absolute path cannot overflow). Forward slash join
             * so xargs doesn't treat Windows backslashes as escapes; binary mode
             * (wb) prevents CRLF translation. Record separator differs by platform:
             *   - Unix: NUL, consumed by `xargs -0` — handles spaces in paths (a
             *     newline separator would split plain xargs on the space).
             *   - Windows: newline, consumed by PowerShell `Get-Content |
             *     Select-String -LiteralPath` (NUL bytes break Get-Content). */
            (void)fwrite(root_path, 1, strlen(root_path), fl);
            (void)fputc('/', fl);
            (void)fwrite(indexed_files[fi], 1, strlen(indexed_files[fi]), fl);
#ifdef _WIN32
            (void)fputc('\n', fl);
#else
            (void)fputc('\0', fl);
#endif
            written++;
        }
        (void)fclose(fl);
        ok = true;
    }
    for (int fi = 0; fi < indexed_count; fi++) {
        free(indexed_files[fi]);
    }
    free(indexed_files);
    *out_written = written;
    return ok;
}

/* Parse search mode string (0=compact, 1=full, 2=files). */
static int parse_search_mode(const char *mode_str) {
    if (!mode_str) {
        return 0;
    }
    if (strcmp(mode_str, "full") == 0) {
        return SKIP_ONE;
    }
    if (strcmp(mode_str, "files") == 0) {
        return MCP_RETURN_2;
    }
    return 0;
}

/* Validate shell-safe arguments for search. */
/* Search/grep paths and globs are ALWAYS single-quoted (POSIX sh) or
 * double-/single-quoted (Windows cmd/PowerShell) on the command line, which
 * neutralises '&' — a very common character in real paths (R&D, "Foo & Bar",
 * OneDrive). Accept '&' here while still rejecting every metacharacter that
 * could break out of the quoting (#272). */
static bool validate_search_path_arg(const char *s) {
    if (!s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\'':
        case '"':
        case ';':
        case '|':
        case '$':
        case '`':
        case '<':
        case '>':
        case '\n':
        case '\r':
#ifndef _WIN32
        case '\\':
#endif
            return false;
        default:
            break;
        }
    }
    return true;
}

static bool validate_search_args(const char *root_path, const char *file_pattern) {
    if (!validate_search_path_arg(root_path)) {
        return false;
    }
    if (file_pattern && !validate_search_path_arg(file_pattern)) {
        return false;
    }
    return true;
}

/* Write pattern to a temp file for grep -f. Returns true on success. */
static bool write_pattern_file(char *tmpfile, int tmpfile_sz, const char *pattern) {
    snprintf(tmpfile, tmpfile_sz, "%s/cbm_search_%d.pat", cbm_tmpdir(), (int)getpid());
    FILE *tf = fopen(tmpfile, "w");
    if (!tf) {
        return false;
    }
    (void)fprintf(tf, "%s\n", pattern);
    (void)fclose(tf);
    return true;
}

/* Compile a path filter regex. Returns true when *re holds the compiled filter.
 * An absent or empty filter compiles nothing and reports CBM_REG_OK in *rc; a
 * filter the regex wrapper refuses or cannot compile reports its error there. */
static bool compile_path_filter(const char *filter, cbm_regex_t *re, int *rc) {
    *rc = CBM_REG_OK;
    if (!filter || !filter[0]) {
        return false;
    }
    *rc = cbm_regcomp(re, filter, CBM_REG_EXTENDED | CBM_REG_NOSUB);
    return *rc == CBM_REG_OK;
}

#ifndef _WIN32
/* ── In-process content scan (POSIX) ────────────────────────────────
 * search_code used to hand the scoped file list to `xargs grep -Hn` through a
 * shell: a process spawn per call plus BSD grep's ~130 MB/s on macOS. This
 * scanner reads the same list in the same order and produces the records
 * `grep -Hn` printed, which collect_grep_matches then parses exactly as it
 * parsed grep's pipe, so everything after the scan is unchanged:
 *   - patterns: the pattern split on '\n', as `grep -f` reads its file; an
 *     empty pattern matches every line;
 *   - literal mode is a substring match (grep -F); regex mode is the
 *     platform's POSIX ERE in grep's dialect (CBM_REG_GREP);
 *   - --include: the glob against the listed path or, on Apple, its base name
 *     (BSD grep), elsewhere any suffix that starts after a '/' (GNU grep);
 *   - a NUL byte in the first SEARCH_SCAN_BINARY_PROBE bytes marks the file
 *     binary and yields no record. A later NUL: BSD grep keeps printing lines,
 *     GNU grep stops line output, so lines stop at the last complete line
 *     before it there;
 *   - the locale is the environment's (LANG / LC_*), as grep inherited it:
 *     it decides what \w or [[:alpha:]] match, and under a UTF-8 locale a
 *     line that is not valid UTF-8 yields no record, as both greps print no
 *     such line;
 *   - an unreadable listed file or a directory is skipped, as grep -d skip did.
 * The scan stops after `max_records` records: collect_grep_matches stops after
 * the same number, and grep was cut off by the closed pipe at that point too. */
#ifdef __APPLE__
enum { SEARCH_SCAN_BINARY_PROBE = CBM_SZ_32K }; /* BSD grep's first buffer */
#else
enum { SEARCH_SCAN_BINARY_PROBE = 3 * CBM_SZ_32K }; /* GNU grep's 96 KiB first buffer */
#endif

struct search_scan_patterns_t {
    std::vector<std::string> items;
    std::vector<cbm_regex_t> line_regexes; /* regex mode: one per non-empty pattern */
    std::vector<cbm_regex_t> span_regexes; /* the same with CBM_REG_NEWLINE, for spans */
    bool any_empty = false;                /* an empty pattern: every line matches */
    bool compiled = false;
    bool utf8_lines = false; /* UTF-8 locale: a line must be valid UTF-8 to match */
    /* Regex mode, one pattern: a literal every match must contain (the longest
     * one found by search_scan_required_literal), or empty when there is none.
     * Candidate lines are then found with memmem instead of the regex engine. */
    std::string required;
};

enum { SEARCH_SCAN_MIN_REQUIRED = 3 }; /* shorter literals filter too little */

/* Bytes in the UTF-8 sequence starting at s: 1 for ASCII or a malformed
 * lead, else 2-4 when that many continuation bytes follow. */
static size_t search_scan_utf8_seq_len(const char *s) {
    unsigned char lead = (unsigned char)s[0];
    size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    for (size_t i = 1; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return len;
}

/* The longest run of plain characters that every match of the ERE `re` must
 * contain, or "" when none is certain. Conservative by construction: any
 * construct it does not fully understand ends the current run, and a top-level
 * '|' (outside groups and brackets) means no single literal is required. A
 * character followed by a quantifier is optional ('?', '*', '{') or still
 * required once ('+') but cannot continue a run past itself. */
static std::string search_scan_required_literal(const char *re) {
    std::string best;
    std::string run;
    int depth = 0;
    auto flush = [&]() {
        if (run.size() > best.size()) {
            best = run;
        }
        run.clear();
    };
    for (const char *p = re; *p;) {
        char c = *p;
        if (c == '[') {
            /* Bracket expression: skip to its closing ']' (a leading ']' or
             * '^]' is a member, as are [:class:] / [=x=] / [.x.] forms). */
            flush();
            const char *q = p + SKIP_ONE;
            if (*q == '^') {
                q++;
            }
            if (*q == ']') {
                q++;
            }
            while (*q && *q != ']') {
                if (*q == '[' && (q[SKIP_ONE] == ':' || q[SKIP_ONE] == '=' || q[SKIP_ONE] == '.')) {
                    char kind = q[SKIP_ONE];
                    const char *close = q + PAIR_LEN;
                    while (*close && !(close[0] == kind && close[SKIP_ONE] == ']')) {
                        close++;
                    }
                    if (!*close) {
                        return std::string();
                    }
                    q = close + PAIR_LEN;
                    continue;
                }
                q++;
            }
            if (!*q) {
                return std::string();
            }
            p = q + SKIP_ONE;
            continue;
        }
        if (c == '{') {
            /* An interval's bounds are not text: skip to its '}'. */
            flush();
            const char *close = strchr(p, '}');
            if (!close) {
                return std::string();
            }
            p = close + SKIP_ONE;
            continue;
        }
        if (c == '(') {
            flush();
            depth++;
            p++;
            continue;
        }
        if (c == ')') {
            flush();
            depth = depth > 0 ? depth - SKIP_ONE : 0;
            p++;
            continue;
        }
        if (c == '|') {
            if (depth == 0) {
                return std::string();
            }
            flush();
            p++;
            continue;
        }
        /* A literal is one character: under a UTF-8 locale a quantifier binds
         * a whole multi-byte sequence, so `fooé*bar` must not require é's
         * lead byte. Taking the sequence as a unit is also safe under a byte
         * locale, where it only makes the required run shorter. */
        bool literal = false;
        size_t lit_at = 0;
        size_t lit_len = SKIP_ONE;
        size_t width = SKIP_ONE;
        if (c == '\\') {
            unsigned char e = (unsigned char)p[SKIP_ONE];
            if (e && !isalnum(e) && e != '<' && e != '>' && e != '`' && e != '\'') {
                literal = true;
                lit_at = SKIP_ONE;
                lit_len = search_scan_utf8_seq_len(p + SKIP_ONE);
                width = SKIP_ONE + lit_len;
            } else if (!e) {
                return std::string();
            } else {
                width = PAIR_LEN; /* \w \s \b \d \1 ...: not a plain character */
            }
        } else if (c != '.' && c != '^' && c != '$' && c != '*' && c != '+' && c != '?' &&
                   c != '{' && c != '}') {
            literal = true;
            lit_len = search_scan_utf8_seq_len(p);
            width = lit_len;
        }
        char next = p[width];
        bool quantified = next == '*' || next == '+' || next == '?' || next == '{';
        if (depth > 0 || !literal) {
            flush();
        } else if (quantified) {
            if (next == '+') {
                run.append(p + lit_at, lit_len);
            }
            flush();
        } else {
            run.append(p + lit_at, lit_len);
        }
        p += width;
    }
    flush();
    return best.size() >= (size_t)SEARCH_SCAN_MIN_REQUIRED ? best : std::string();
}

static void search_scan_patterns_free(search_scan_patterns_t *patterns) {
    for (cbm_regex_t &re : patterns->line_regexes) {
        cbm_regfree(&re);
    }
    for (cbm_regex_t &re : patterns->span_regexes) {
        cbm_regfree(&re);
    }
    patterns->line_regexes.clear();
    patterns->span_regexes.clear();
}

/* False when a regex does not compile: grep then exited with an error and
 * printed no record, so the scan yields none either. */
static bool search_scan_patterns_parse(search_scan_patterns_t *patterns, const char *pattern,
                                       bool use_regex) {
    const char *start = pattern;
    for (;;) {
        const char *end = strchr(start, '\n');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        patterns->items.emplace_back(start, length);
        patterns->any_empty = patterns->any_empty || length == 0;
        if (!end) {
            break;
        }
        start = end + SKIP_ONE;
    }
    /* grep -f reads lines: the trailing newline write_pattern_file adds ends
     * the last line instead of adding an empty pattern. The caller passes the
     * pattern without that newline, so nothing to drop here. */
    if (!use_regex) {
        return true;
    }
    for (const std::string &item : patterns->items) {
        if (item.empty()) {
            continue;
        }
        cbm_regex_t line_re;
        if (cbm_regcomp(&line_re, item.c_str(), CBM_REG_EXTENDED | CBM_REG_NOSUB | CBM_REG_GREP) !=
            CBM_REG_OK) {
            search_scan_patterns_free(patterns);
            return false;
        }
        patterns->line_regexes.push_back(line_re);
        cbm_regex_t span_re;
        if (cbm_regcomp(&span_re, item.c_str(),
                        CBM_REG_EXTENDED | CBM_REG_GREP | CBM_REG_NEWLINE) != CBM_REG_OK) {
            search_scan_patterns_free(patterns);
            return false;
        }
        patterns->span_regexes.push_back(span_re);
    }
    patterns->compiled = true;
    if (patterns->items.size() == SKIP_ONE && !patterns->any_empty) {
        patterns->required = search_scan_required_literal(patterns->items[0].c_str());
    }
    return true;
}

/* grep --include for a command-line operand (see the block comment above). */
static bool search_scan_include_matches(const char *glob, const char *path) {
    if (fnmatch(glob, path, 0) == 0) {
        return true;
    }
#ifdef __APPLE__
    const char *base = strrchr(path, '/');
    return base && fnmatch(glob, base + SKIP_ONE, 0) == 0;
#else
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[SKIP_ONE] != '/' && fnmatch(glob, p + SKIP_ONE, 0) == 0) {
            return true;
        }
    }
    return false;
#endif
}

/* Whether one line (NUL-terminated at its end) matches any pattern. */
static bool search_scan_line_matches(const search_scan_patterns_t *patterns, bool use_regex,
                                     const char *line, size_t length) {
    if (patterns->any_empty) {
        return true;
    }
    if (use_regex) {
        /* A line can hold a NUL only on Apple (see search_scan_buffer), where
         * BSD grep matched past it: try each NUL-separated piece. */
        for (const char *piece = line; piece <= line + length; piece += strlen(piece) + SKIP_ONE) {
            int eflags = piece == line ? 0 : REG_NOTBOL; /* ^ is the line start only */
            if (std::any_of(patterns->line_regexes.begin(), patterns->line_regexes.end(),
                            [piece, eflags](const cbm_regex_t &re) {
                                return cbm_regexec(&re, piece, 0, NULL, eflags) == CBM_REG_OK;
                            })) {
                return true;
            }
        }
        return false;
    }
    return std::any_of(patterns->items.begin(), patterns->items.end(),
                       [line, length](const std::string &item) {
                           return memmem(line, length, item.data(), item.size()) != NULL;
                       });
}

/* Start of the first line at or after `pos` that may match: a literal or
 * regex hit found over the whole remaining span, backed up to its line start.
 * Never skips a line that matches on its own: the span engines find the
 * leftmost hit, and a per-line hit is a hit in the span too (REG_NEWLINE
 * gives ^ $ . [^...] their per-line meaning). The caller re-checks the line,
 * since a span hit can cross a newline through a class such as \s. Returns
 * `length` when nothing further can match. */
static size_t search_scan_next_candidate(const search_scan_patterns_t *patterns, bool use_regex,
                                         const char *buffer, size_t pos, size_t length) {
    if (patterns->any_empty) {
        return pos;
    }
    size_t best = length;
    if (use_regex && !patterns->required.empty()) {
        const void *hit = memmem(buffer + pos, length - pos, patterns->required.data(),
                                 patterns->required.size());
        best = hit ? (size_t)((const char *)hit - buffer) : length;
    } else if (use_regex) {
        for (const cbm_regex_t &re : patterns->span_regexes) {
            cbm_regmatch_t m[1];
            if (cbm_regexec(&re, buffer + pos, 1, m, 0) == CBM_REG_OK && m[0].rm_so >= 0 &&
                pos + (size_t)m[0].rm_so < best) {
                best = pos + (size_t)m[0].rm_so;
            }
        }
    } else {
        for (const std::string &item : patterns->items) {
            const void *hit = memmem(buffer + pos, length - pos, item.data(), item.size());
            if (hit && (size_t)((const char *)hit - buffer) < best) {
                best = (size_t)((const char *)hit - buffer);
            }
        }
    }
    if (best >= length) {
        return length;
    }
    while (best > pos && buffer[best - SKIP_ONE] != '\n') {
        best--;
    }
    return best;
}

/* RFC 3629 UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF. */
static bool search_scan_valid_utf8(const unsigned char *s, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t need = 0;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            lo = c == 0xE0 ? 0xA0 : 0x80;
            hi = c == 0xED ? 0x9F : 0xBF;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            lo = c == 0xF0 ? 0x90 : 0x80;
            hi = c == 0xF4 ? 0x8F : 0xBF;
        } else {
            return false;
        }
        if (i + need >= length) {
            return false;
        }
        if (s[i + 1] < lo || s[i + 1] > hi) {
            return false;
        }
        for (size_t k = 2; k <= need; k++) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) {
                return false;
            }
        }
        i += need + 1;
    }
    return true;
}

/* The environment's locale for one scan thread, as the grep child inherited
 * it (process-wide setlocale is never called, so this is per thread). */
struct search_scan_locale_t {
    locale_t locale = (locale_t)0;
    locale_t previous = (locale_t)0;
    bool utf8 = false;
};

static void search_scan_locale_enter(search_scan_locale_t *l) {
    l->locale = newlocale(LC_ALL_MASK, "", (locale_t)0);
    if (!l->locale) {
        return; /* an unknown locale name: grep fell back to "C" as well */
    }
    l->previous = uselocale(l->locale);
    const char *codeset = nl_langinfo(CODESET);
    l->utf8 = codeset && (strcmp(codeset, "UTF-8") == 0 || strcmp(codeset, "utf8") == 0);
}

static void search_scan_locale_leave(search_scan_locale_t *l) {
    if (l->locale) {
        uselocale(l->previous);
        freelocale(l->locale);
        l->locale = (locale_t)0;
    }
}

/* Append one `grep -Hn` record. */
/* collect_grep_matches reads records through a CBM_SZ_2K fgets buffer, so it
 * never sees past a record's first CBM_SZ_2K - 1 bytes (the rest of a longer
 * line came back as stray partial records). A record is cut to what that
 * buffer holds, which keeps a scan of minified one-line files from holding
 * whole lines -- up to the file cap each -- until the join. */
enum { SEARCH_SCAN_RECORD_MAX = CBM_SZ_2K - PAIR_LEN }; /* room for '\n' and NUL */

static void search_scan_emit(std::string &out, const char *path, size_t line_number,
                             const char *line, size_t line_length) {
    char number[CBM_SZ_32];
    int n = snprintf(number, sizeof(number), ":%zu:", line_number);
    size_t header = strlen(path) + (n > 0 ? (size_t)n : 0);
    size_t room = header < (size_t)SEARCH_SCAN_RECORD_MAX ? SEARCH_SCAN_RECORD_MAX - header : 0;
    out.append(path);
    out.append(number, n > 0 ? (size_t)n : 0);
    out.append(line, line_length < room ? line_length : room);
    out.push_back('\n');
}

/* Scan one file's bytes (buffer[length] is writable). Returns the records
 * added; stops at *budget records. */
static void search_scan_buffer(std::string &out, const search_scan_patterns_t *patterns,
                               bool use_regex, const char *path, char *buffer, size_t length,
                               int *budget) {
    size_t probe =
        length < (size_t)SEARCH_SCAN_BINARY_PROBE ? length : (size_t)SEARCH_SCAN_BINARY_PROBE;
    if (memchr(buffer, '\0', probe)) {
        return; /* binary: grep prints no line of it */
    }
    const char *nul = (const char *)memchr(buffer, '\0', length);
    bool line_mode = false; /* NUL inside the scanned span: no span search */
    if (nul) {
#ifdef __APPLE__
        line_mode = true;
#else
        size_t keep = 0;
        for (size_t i = (size_t)(nul - buffer); i > 0; i--) {
            if (buffer[i - SKIP_ONE] == '\n') {
                keep = i;
                break;
            }
        }
        length = keep;
#endif
    }
    buffer[length] = '\0';
    size_t line_number = 1;
    size_t pos = 0;
    while (pos < length && *budget > 0) {
        size_t line_start = pos;
        // cppcheck-suppress knownConditionTrueFalse ; line_mode is set on __APPLE__ only
        if (!line_mode) {
            line_start = search_scan_next_candidate(patterns, use_regex, buffer, pos, length);
            if (line_start >= length) {
                break;
            }
            for (const char *p = buffer + pos; p < buffer + line_start;) {
                const char *newline =
                    (const char *)memchr(p, '\n', (size_t)(buffer + line_start - p));
                if (!newline) {
                    break;
                }
                line_number++;
                p = newline + SKIP_ONE;
            }
        }
        char *newline = (char *)memchr(buffer + line_start, '\n', length - line_start);
        size_t line_end = newline ? (size_t)(newline - buffer) : length;
        buffer[line_end] = '\0';
        bool matched = search_scan_line_matches(patterns, use_regex, buffer + line_start,
                                                line_end - line_start) &&
                       (!patterns->utf8_lines ||
                        search_scan_valid_utf8((const unsigned char *)buffer + line_start,
                                               line_end - line_start));
        if (newline) {
            *newline = '\n';
        }
        if (matched) {
            search_scan_emit(out, path, line_number, buffer + line_start, line_end - line_start);
            (*budget)--;
        }
        pos = line_end + SKIP_ONE;
        line_number++;
    }
}

/* A file larger than this is not scanned. The indexer keeps files up to
 * CBM_DEFAULT_MAX_FILE_SIZE (2 MiB) unless told otherwise, so this only drops
 * outliers -- a path grown or retargeted to an endless pseudo-file after
 * indexing -- and it bounds a request's buffers to SEARCH_SCAN_THREADS times
 * this cap. */
static constexpr size_t SEARCH_SCAN_FILE_MAX = (size_t)64 << 20;

/* The cap, which test builds can lower (CBM_TEST_SEARCH_SCAN_FILE_MAX=<bytes>)
 * to exercise it on small fixtures. */
static size_t search_scan_file_max(void) {
#ifdef CBM_ENABLE_TEST_SEAMS
    const char *v = getenv("CBM_TEST_SEARCH_SCAN_FILE_MAX");
    if (v && v[0]) {
        return (size_t)strtoull(v, NULL, 10);
    }
#endif
    return SEARCH_SCAN_FILE_MAX;
}

/* Read a whole file into `buffer` (one spare byte past the data). False when
 * it cannot be opened or read (a directory, a vanished file), when it passes
 * SEARCH_SCAN_FILE_MAX, or when its buffer cannot be allocated -- a failed
 * resize must not escape as std::bad_alloc and terminate the server. */
static bool search_scan_read_file(const char *path, std::vector<char> &buffer, size_t *length) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }
    size_t used = 0;
    bool ok = true;
    const size_t file_max = search_scan_file_max();
    for (;;) {
        if (buffer.size() - used < (size_t)CBM_SZ_64K + SKIP_ONE) {
            if (used >= file_max) {
                ok = false;
                break;
            }
            /* Never hold more than the cap plus one read's slack. */
            size_t grow = buffer.empty() ? (size_t)CBM_SZ_64K * PAIR_LEN : buffer.size() * PAIR_LEN;
            grow = std::min(grow, file_max + (size_t)CBM_SZ_64K + SKIP_ONE);
            try {
                buffer.resize(grow);
            } catch (const std::bad_alloc &) {
                ok = false;
                break;
            }
        }
        size_t want = buffer.size() - used - SKIP_ONE;
        size_t got = fread(buffer.data() + used, 1, want, fp);
        used += got;
        if (got < want) {
            ok = !ferror(fp) && used <= file_max; /* EOF inside the read slack */
            break;
        }
    }
    (void)fclose(fp);
    *length = used;
    return ok;
}

/* Scan the NUL-separated file list into `grep -Hn` records. False when the
 * list cannot be read (the caller then runs grep). */
/* Files are scanned by up to SEARCH_SCAN_THREADS threads, each with its own
 * compiled patterns (glibc serializes regexec on a shared regex_t) and read
 * buffer. Files are handed out in list order and their records are joined in
 * list order, so the output is the sequential scan's. The scan stops once the
 * files finished in list order hold max_records records; a thread runs at most
 * SEARCH_SCAN_WINDOW files ahead of that prefix, which bounds the records held
 * for files that may not be needed. */
enum {
    SEARCH_SCAN_THREADS = 4,
    SEARCH_SCAN_PARALLEL_MIN_FILES = 32, /* fewer files: one thread */
    SEARCH_SCAN_WINDOW = 64,
};

struct search_scan_shared_t {
    const char *root_path; /* NULL: no containment check (tests) */
    const std::vector<const char *> *paths;
    const char *pattern;
    bool use_regex;
    int max_records;
    std::vector<std::string> outputs; /* per file, in list order */
    std::vector<int> counts;          /* records per file; -1 = not finished */
    std::mutex lock;
    std::condition_variable advanced;
    size_t next = 0;      /* next file to hand out */
    size_t committed = 0; /* files [0, committed) are finished */
    int committed_records = 0;
    bool stop = false;
};

static void search_scan_worker(search_scan_shared_t *sh) {
    search_scan_locale_t locale;
    search_scan_locale_enter(&locale);
    search_scan_patterns_t patterns;
    if (!search_scan_patterns_parse(&patterns, sh->pattern, sh->use_regex)) {
        search_scan_locale_leave(&locale);
        return; /* checked by the caller first: cannot happen */
    }
    patterns.utf8_lines = locale.utf8;
    std::vector<char> buffer;
    const std::vector<const char *> &paths = *sh->paths;
    for (;;) {
        size_t i = 0;
        int budget = 0;
        {
            std::unique_lock<std::mutex> guard(sh->lock);
            sh->advanced.wait(guard, [sh] {
                return sh->stop || sh->next >= sh->outputs.size() ||
                       sh->next < sh->committed + (size_t)SEARCH_SCAN_WINDOW;
            });
            if (sh->stop || sh->next >= sh->outputs.size()) {
                break;
            }
            i = sh->next++;
            budget = sh->max_records - sh->committed_records;
        }
        int left = budget;
        size_t length = 0;
        if (budget > 0 && search_scan_read_file(paths[i], buffer, &length)) {
            search_scan_buffer(sh->outputs[i], &patterns, sh->use_regex, paths[i], buffer.data(),
                               length, &left);
            /* A file that resolves outside the root (a directory link on its
             * indexed path) yields no record, as collect_grep_matches drops
             * grep's hits for it. Checked only for files with hits. */
            if (left < budget && sh->root_path && !cbm_path_within_root(sh->root_path, paths[i])) {
                sh->outputs[i].clear();
                left = budget;
            }
        }
        std::lock_guard<std::mutex> guard(sh->lock);
        sh->counts[i] = budget - left;
        while (sh->committed < sh->counts.size() && sh->counts[sh->committed] >= 0) {
            sh->committed_records += sh->counts[sh->committed];
            sh->committed++;
        }
        if (sh->committed_records >= sh->max_records) {
            sh->stop = true;
        }
        sh->advanced.notify_all();
    }
    search_scan_patterns_free(&patterns);
    search_scan_locale_leave(&locale);
}

/* Join the per-file records in list order, up to max_records records. */
static void search_scan_join(search_scan_shared_t *sh, std::string &out) {
    int budget = sh->max_records;
    for (size_t i = 0; i < sh->committed && budget > 0; i++) {
        const std::string &records = sh->outputs[i];
        if (sh->counts[i] <= budget) {
            out.append(records);
            budget -= sh->counts[i];
            continue;
        }
        /* Every record ends in the one '\n' a line cannot otherwise hold. */
        size_t cut = 0;
        for (int r = 0; r < budget; r++) {
            cut = records.find('\n', cut) + SKIP_ONE;
        }
        out.append(records, 0, cut);
        budget = 0;
    }
}

static bool search_scan_in_process(const char *filelist_path, const char *root_path,
                                   const char *pattern, bool use_regex, const char *file_pattern,
                                   int max_records, std::string &out) {
    std::vector<char> list;
    size_t list_length = 0;
    if (!search_scan_read_file(filelist_path, list, &list_length)) {
        return false;
    }
    std::vector<const char *> paths;
    for (size_t pos = 0; pos < list_length;) {
        const char *end = (const char *)memchr(list.data() + pos, '\0', list_length - pos);
        size_t record_end = end ? (size_t)(end - list.data()) : list_length;
        list[record_end] = '\0';
        const char *path = list.data() + pos;
        if (record_end > pos &&
            (!file_pattern || search_scan_include_matches(file_pattern, path))) {
            paths.push_back(path);
        }
        pos = record_end + SKIP_ONE;
    }
    search_scan_locale_t locale;
    search_scan_locale_enter(&locale);
    search_scan_patterns_t patterns;
    bool compiles = search_scan_patterns_parse(&patterns, pattern, use_regex);
    search_scan_locale_leave(&locale);
    if (!compiles) {
        return true; /* grep -E rejected the pattern: no records */
    }
    if (max_records <= 0 || paths.empty()) {
        search_scan_patterns_free(&patterns);
        return true;
    }
    search_scan_shared_t sh;
    sh.root_path = root_path;
    sh.paths = &paths;
    sh.pattern = pattern;
    sh.use_regex = use_regex;
    sh.max_records = max_records;
    sh.outputs.resize(paths.size());
    sh.counts.assign(paths.size(), CBM_NOT_FOUND);
    int threads = paths.size() >= (size_t)SEARCH_SCAN_PARALLEL_MIN_FILES ? SEARCH_SCAN_THREADS : 1;
    int nprocs = cbm_nprocs();
    if (nprocs > 0 && threads > nprocs) {
        threads = nprocs;
    }
    search_scan_patterns_free(&patterns); /* each worker compiles its own */
    std::vector<std::thread> helpers;
    for (int t = SKIP_ONE; t < threads; t++) {
        try {
            helpers.emplace_back(search_scan_worker, &sh);
        } catch (...) {
            break; /* no thread: the calling thread scans the rest */
        }
    }
    search_scan_worker(&sh);
    for (std::thread &helper : helpers) {
        helper.join();
    }
    search_scan_join(&sh, out);
    return true;
}

/* Records from search_scan_in_process, parsed by the grep-pipe parser. */
static grep_match_t *search_scan_collect(const std::string &records, const char *root_path,
                                         bool has_path_filter, cbm_regex_t *path_regex,
                                         int grep_limit, int *out_count) {
    if (records.empty()) {
        *out_count = 0;
        return (grep_match_t *)malloc(sizeof(grep_match_t)); /* empty set; caller frees */
    }
    FILE *fp = fmemopen((void *)records.data(), records.size(), "r");
    if (!fp) {
        return NULL;
    }
    grep_match_t *gm = collect_grep_matches(fp, root_path, strlen(root_path), has_path_filter,
                                            path_regex, grep_limit, false, out_count);
    (void)fclose(fp);
    return gm;
}
#endif /* !_WIN32 */

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_mcp_search_scan_for_test(const char *filelist_path, const char *pattern, bool use_regex,
                                   const char *file_pattern, int max_records) {
#ifdef _WIN32
    (void)filelist_path;
    (void)pattern;
    (void)use_regex;
    (void)file_pattern;
    (void)max_records;
    return NULL;
#else
    std::string records;
    if (!search_scan_in_process(filelist_path, NULL, pattern, use_regex, file_pattern, max_records,
                                records)) {
        return NULL;
    }
    return heap_strdup(records.c_str());
#endif
}
#endif

static char *handle_search_code(cbm_mcp_server_t *srv, const char *args) {
    char *pattern = cbm_mcp_get_string_arg(args, "pattern");
    char *project = get_project_arg_or_cwd(srv, args, true);
    char *file_pattern = cbm_mcp_get_string_arg(args, "file_pattern");
    char *path_filter = cbm_mcp_get_string_arg(args, "path_filter");
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    int limit = cbm_mcp_get_int_arg(args, "limit", MCP_DEFAULT_LIMIT);
    int context_lines = cbm_mcp_get_int_arg(args, "context", 0);
    if (context_lines < 0) {
        context_lines = 0;
    } else if (context_lines > MCP_SEARCH_CONTEXT_MAX_LINES) {
        context_lines = MCP_SEARCH_CONTEXT_MAX_LINES;
    }
    /* Reply byte budget (0 disables). Default sized so a compact reply never
     * hits it and a full-mode reply stays under ~6k tokens. */
    enum { SEARCH_DEFAULT_MAX_BYTES = 24000 };
    int max_bytes = cbm_mcp_get_int_arg(args, "max_bytes", SEARCH_DEFAULT_MAX_BYTES);
    bool use_regex = cbm_mcp_get_bool_arg(args, "regex");
    uint64_t search_t0 = cbm_now_ms();
    /* In literal (non-regex) mode a '|' is matched as a byte, not alternation —
     * a common silent 0-match trap; flagged in the result warnings (#282). */
    bool pat_has_pipe = pattern && strchr(pattern, '|') != NULL;

    int mode = parse_search_mode(mode_str);
    free(mode_str);

    cbm_regex_t path_regex;
    int path_filter_rc = CBM_REG_OK; /* reported after validate_search_args below */
    bool has_path_filter = compile_path_filter(path_filter, &path_regex, &path_filter_rc);
    free(path_filter);
    path_filter = NULL;

    if (!pattern) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("pattern is required", true);
    }

    /* Project is required */
    if (!project) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(pattern);
        free(file_pattern);
        char *_err = build_project_list_error("project is required");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        return _res;
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(pattern);
        free(project);
        free(file_pattern);
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        return _res;
    }

    if (!validate_search_args(root_path, file_pattern)) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result("path or file_pattern contains invalid characters", true);
    }

    /* NOTE (fork deviation from upstream #283): upstream validates the user's
     * regex up front with cbm_regcomp and returns an explicit "invalid regex"
     * error to distinguish a broken pattern from a legitimate no-match. We
     * intentionally do NOT port that check: upstream's cbm_regcomp is TRE-backed
     * and accepts the same dialect as the underlying `grep` (GNU extensions like
     * \s \w \d), whereas this fork backs cbm_regcomp with std::regex::extended
     * (strict POSIX ERE), which rejects those escapes. The real search shells
     * out to system grep, so validating with std::regex here would falsely
     * reject patterns like "def\\s+\\w+_handler" that grep handles fine. See
     * UPSTREAM_SYNC.md. */

    /* A path_filter the regex wrapper refused or could not compile is reported
     * instead of silently searching unfiltered. A regex=true pattern is not
     * compiled here (see above), but it gets the same compile-size bound:
     * grep expands bounded repetition the same way. */
    const char *regex_error = nullptr;
    if (path_filter_rc != CBM_REG_OK) {
        regex_error = path_filter_rc == CBM_REG_ETOOBIG
                          ? "path_filter: " CBM_REG_ETOOBIG_REASON
                          : "invalid path_filter regex: check for unbalanced (), [], or {}";
    } else if (use_regex && cbm_regex_too_big(pattern, CBM_REG_EXTENDED)) {
        regex_error = CBM_REG_ETOOBIG_REASON " (regex=true)";
    }
    if (regex_error) {
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result(regex_error, true);
    }

    /* ── Phase 0.5: Multi-word → regex conversion ───────────── */
    /* If pattern contains whitespace and is not already a regex, convert to a
     * regex that matches all words in order: "foo bar baz" → "foo.*bar.*baz".
     * This avoids requiring the exact phrase as a contiguous substring. */
    if (!use_regex && strchr(pattern, ' ')) {
        size_t plen = strlen(pattern);
        /* Worst case: every char is a space → ".*" between each char */
        char *regex_pat = (char *)malloc(plen * 3 + 1);
        if (regex_pat) {
            char *dst = regex_pat;
            const char *src = pattern;
            bool in_space = false;
            while (*src) {
                if (*src == ' ' || *src == '\t') {
                    if (!in_space) {
                        *dst++ = '.';
                        *dst++ = '*';
                        in_space = true;
                    }
                } else {
                    /* Escape regex metacharacters from user input */
                    if (strchr("\\^$.|?*+()[]{}", *src)) {
                        *dst++ = '\\';
                    }
                    *dst++ = *src;
                    in_space = false;
                }
                src++;
            }
            *dst = '\0';
            free(pattern);
            pattern = regex_pat;
            use_regex = true;
        }
    }

    /* ── Phase 1: Grep scan ──────────────────────────────────── */
    char tmpfile[CBM_SZ_256];
    if (!write_pattern_file(tmpfile, sizeof(tmpfile), pattern)) {
        char errmsg[CBM_SZ_256];
        snprintf(errmsg, sizeof(errmsg), "search failed: cannot create temp file (%s)",
                 strerror(errno));
        if (has_path_filter) {
            cbm_regfree(&path_regex);
        }
        free(root_path);
        free(pattern);
        free(project);
        free(file_pattern);
        return cbm_mcp_text_result(errmsg, true);
    }

    /* No grep-level match limit — let grep find all matches, then dedup and
     * cap in our code. The -m flag caused results from large vendored files
     * to exhaust the quota before reaching project source files. */
    enum { GREP_MAX_MATCHES = 500 };
    int grep_limit = GREP_MAX_MATCHES;

    /* Scope grep to indexed files only — avoids scanning vendored/generated code.
     * Query the graph for distinct file paths, write them to a temp file,
     * then use xargs to pass them to grep. Falls back to recursive grep if
     * no indexed files found (project not fully indexed). */
    char filelist[CBM_SZ_256];
    snprintf(filelist, sizeof(filelist), "%s.files", tmpfile);
    bool scoped = false;
    int scoped_written = 0;

    const bool path_glob = search_code_file_pattern_is_path_glob(file_pattern);
    scoped = write_scoped_filelist(srv, project, root_path, filelist, has_path_filter,
                                   has_path_filter ? &path_regex : NULL,
                                   path_glob ? file_pattern : NULL, &scoped_written);
    /* A path glob already chose the scoped list's files; the scanner, grep --include and
     * the PowerShell -like post-filter all match base names (or, on Windows, the
     * backslashed full path), so they must not see it again. The unscoped recursive
     * fallback (project with no indexed files) keeps the raw pattern. */
    const char *scan_file_pattern = (scoped && path_glob) ? NULL : file_pattern;

    /* Collect grep matches into array */
    int gm_count = 0;
    grep_match_t *gm = NULL;
    if (scoped && scoped_written == 0) {
        /* The path_filter excluded every indexed file — nothing to scan.
         * Skip the grep subprocess: xargs on an empty filelist is
         * platform-dependent (GNU execs grep once with no operands, BSD
         * skips), and the post-grep filter would drop every hit anyway. */
        gm = (grep_match_t *)malloc(sizeof(grep_match_t)); /* empty set; freed below */
        cbm_unlink(tmpfile);
        cbm_unlink(filelist);
    }
#ifndef _WIN32
    /* An indexed project is scanned in process (see search_scan_in_process);
     * the recursive fallback for an unindexed root keeps grep. */
    if (!gm && scoped) {
        std::string records;
        if (search_scan_in_process(filelist, root_path, pattern, use_regex, scan_file_pattern,
                                   grep_limit, records)) {
            gm = search_scan_collect(records, root_path, has_path_filter, &path_regex, grep_limit,
                                     &gm_count);
        }
        if (gm) {
            cbm_unlink(tmpfile);
            cbm_unlink(filelist);
        }
    }
#endif
    if (!gm) {
        char cmd[CBM_SZ_4K];
        cbm_search_code_build_grep_cmd(cmd, sizeof(cmd), use_regex, scoped, scan_file_pattern,
                                       tmpfile, filelist, root_path);

        FILE *fp = cbm_popen(cmd, "r");
        if (!fp) {
            cbm_unlink(tmpfile);
            if (scoped) {
                cbm_unlink(filelist);
            }
            /* Every other exit from this function releases the compiled path
             * filter; the launch-failure path did not. */
            if (has_path_filter) {
                cbm_regfree(&path_regex);
            }
            free(root_path);
            free(pattern);
            free(project);
            free(file_pattern);
            return cbm_mcp_text_result("search failed", true);
        }

        gm = collect_grep_matches(fp, root_path, strlen(root_path), has_path_filter, &path_regex,
                                  grep_limit, true, &gm_count);
        cbm_pclose(fp);
        cbm_unlink(tmpfile);
        if (scoped) {
            cbm_unlink(filelist);
        }
    }

    /* ── Phase 2+3: Block expansion + graph ranking ──────────── */
    /* Sort grep matches by file for contiguous processing.
     * Then: one SQL query per unique file for nodes, one batch query for all degrees. */

    cbm_store_t *store = resolve_store(srv, project);

    int sr_cap = CBM_SZ_32;
    int sr_count = 0;
    search_result_t *sr = (search_result_t *)calloc(sr_cap, sizeof(search_result_t));

    int raw_cap = CBM_SZ_32;
    int raw_count = 0;
    grep_match_t *raw = (grep_match_t *)malloc(raw_cap * sizeof(grep_match_t));

    /* Sort matches by file path for contiguous per-file processing */
    std::sort(gm, gm + gm_count, [](const grep_match_t &a, const grep_match_t &b) {
        return strcmp(a.file, b.file) < 0;
    });

    classify_all_grep_hits(gm, gm_count, store, project, &sr, &sr_count, &sr_cap, &raw, &raw_count,
                           &raw_cap);

    /* Phase 3: batch degree query — ONE query for all results instead of 2×N */
    if (store && sr_count > 0) {
        int64_t *ids = (int64_t *)malloc(sr_count * sizeof(int64_t));
        int *in_degs = (int *)malloc(sr_count * sizeof(int));
        int *out_degs = (int *)malloc(sr_count * sizeof(int));
        for (int j = 0; j < sr_count; j++) {
            ids[j] = sr[j].node_id;
        }
        if (cbm_store_batch_count_degrees(store, ids, sr_count, "CALLS", in_degs, out_degs) ==
            CBM_STORE_OK) {
            for (int j = 0; j < sr_count; j++) {
                sr[j].in_degree = in_degs[j];
                sr[j].out_degree = out_degs[j];
            }
        }
        free(ids);
        free(in_degs);
        free(out_degs);
    }

    /* Compute scores and sort */
    for (int j = 0; j < sr_count; j++) {
        sr[j].score = compute_search_score(&sr[j]);
    }
    if (sr_count > SKIP_ONE) {
        std::sort(sr, sr + sr_count, search_result_score_greater);
    }

    /* ── Phase 4: Context assembly (extracted helper) ─────────── */

    /* A '/' file_pattern that matched nothing: say it is a whole-path match. */
    const bool hint_path_filter = path_glob && gm_count == 0;
    char *result = assemble_search_output(sr, sr_count, raw, raw_count, gm_count, limit, mode,
                                          context_lines, root_path, pat_has_pipe && !use_regex,
                                          hint_path_filter, cbm_now_ms() - search_t0, max_bytes);
    free(gm);
    free(sr);
    free(raw);
    free(root_path);
    free(pattern);
    free(project);
    free(file_pattern);
    if (has_path_filter) {
        cbm_regfree(&path_regex);
    }
    return result;
}

/* ── detect_changes ───────────────────────────────────────────── */

/* Does `node`'s line range overlap any recorded hunk for `file`? Used to scope
 * seed detection to the actually-changed lines rather than the whole file.
 * Non-static (declared in mcp_internal.h) so tests can exercise the overlap
 * logic directly, matching this file's existing white-box test hooks. */
bool cbm_detect_node_in_hunks(const cbm_node_t *node, const cbm_changed_hunk_t *hunks,
                              int hunk_count, const char *file) {
    for (int h = 0; h < hunk_count; h++) {
        if (strcmp(hunks[h].path, file) == 0 && node->start_line <= hunks[h].end_line &&
            node->end_line >= hunks[h].start_line) {
            return true;
        }
    }
    return false;
}

/* Structural container labels carry no CALLS edges and span the whole file, so
 * they are never seeds. Shared by the seeding loop and the overlap probe. */
static bool detect_is_seedable_label(const char *lb) {
    return lb && strcmp(lb, "File") != 0 && strcmp(lb, "Folder") != 0 &&
           strcmp(lb, "Project") != 0 && strcmp(lb, "Module") != 0 && strcmp(lb, "Package") != 0 &&
           strcmp(lb, "Section") != 0;
}

/* Collect BFS seed ids for one changed file (everything but the structural
 * container labels — those have no CALLS edges). These anchor the
 * multi-source impact traversal.
 *
 * When `hunks` has at least one entry for `file`, only definitions whose line
 * range overlaps a hunk become seeds — a one-line edit inside a single
 * function no longer seeds every other definition in the file. When no hunk
 * is recorded for `file` (a brand-new/untracked file has no comparable
 * "before" state, or the hunk fetch failed/was skipped), every non-container
 * definition in the file is a seed — the previous, whole-file behavior — so
 * this is a precision improvement, not a new failure mode. */
static void detect_collect_seeds(cbm_store_t *store, const char *project, const char *file,
                                 const cbm_changed_hunk_t *hunks, int hunk_count, int64_t **seeds,
                                 int *n, int *cap) {
    cbm_node_t *nodes = NULL;
    int ncount = 0;
    cbm_store_find_nodes_by_file(store, project, file, &nodes, &ncount);
    bool scope_to_hunks = false;
    for (int h = 0; h < hunk_count; h++) {
        if (strcmp(hunks[h].path, file) == 0) {
            scope_to_hunks = true;
            break;
        }
    }
    /* A file can have hunks yet no SEEDABLE definition overlapping any of them:
     * an import-only edit, a module-level constant, or a change above the first
     * definition all land outside every definition's line range. Scoping would
     * then drop the file from the seed set entirely — strictly worse recall
     * than the whole-file behavior this replaces. Probe for an overlap first
     * and keep whole-file seeding for that file when there is none.
     *
     * The probe must apply the same label filter as the seeding loop below:
     * container nodes span the whole file (a Module node is lines 1..EOF), so
     * counting them would report an overlap for every hunk and defeat the
     * fallback entirely. */
    if (scope_to_hunks) {
        bool any_overlap = false;
        for (int i = 0; i < ncount && !any_overlap; i++) {
            any_overlap = detect_is_seedable_label(nodes[i].label) &&
                          cbm_detect_node_in_hunks(&nodes[i], hunks, hunk_count, file);
        }
        scope_to_hunks = any_overlap;
    }
    for (int i = 0; i < ncount; i++) {
        if (detect_is_seedable_label(nodes[i].label)) {
            if (scope_to_hunks && !cbm_detect_node_in_hunks(&nodes[i], hunks, hunk_count, file)) {
                continue;
            }
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *seeds = (int64_t *)safe_realloc(*seeds, (size_t)*cap * sizeof(int64_t));
            }
            (*seeds)[(*n)++] = nodes[i].id;
        }
    }
    cbm_store_free_nodes(nodes, ncount);
}

/* Module key for the impacted rollup = the first TWO path segments
 * ("src/mcp/mcp.c" -> "src/mcp"), a quotient of the blast radius coarse enough
 * to fit yet specific enough to localize (one segment collapses a whole tree
 * to "src"). Falls back to one segment, then the whole path. */
static void detect_module_of(const char *file, char *out, size_t outsz) {
    if (!file || !file[0]) {
        snprintf(out, outsz, "(root)");
        return;
    }
    const char *s1 = strchr(file, '/');
    if (!s1) {
        snprintf(out, outsz, "%s", file);
        return;
    }
    const char *s2 = strchr(s1 + 1, '/');
    size_t len = s2 ? (size_t)(s2 - file) : strlen(file);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, file, len);
    out[len] = '\0';
}

#ifdef _WIN32
#define DETECT_GIT_Q "\""
#define DETECT_GIT_NULL "2>NUL"
#else
#define DETECT_GIT_Q "'"
#define DETECT_GIT_NULL "2>/dev/null"
#endif

/* Run one git query (scrubbed git environment) and return its first output
 * line without the line ending. *complete reports whether a whole,
 * newline-terminated line was read. False when the command cannot run or
 * exits non-zero. */
static bool detect_git_line(const char *command, char *out, size_t out_size, bool *complete) {
    out[0] = '\0';
    *complete = false;
    FILE *fp = cbm_popen_git(command);
    if (!fp) {
        return false;
    }
    bool read = fgets(out, (int)out_size, fp) != NULL;
    /* Drain so the child never blocks on a full pipe before cbm_pclose. */
    char drain[CBM_SZ_256];
    while (fgets(drain, sizeof(drain), fp)) {}
    int status = cbm_pclose(fp);
    size_t length = read ? strlen(out) : 0;
    *complete = length > 0 && out[length - 1] == '\n';
    while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r')) {
        out[--length] = '\0';
    }
    if (status != 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* First output line of a git query, only when it is a non-empty value that
 * can be spliced back into the later git commands safely. */
static bool detect_git_first_line(const char *command, char *out, size_t out_size) {
    bool complete = false;
    bool ok = detect_git_line(command, out, out_size, &complete) && complete && out[0] &&
              out[0] != '-' && cbm_validate_shell_arg(out);
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

/* Upstream #1357: without base_branch, diff against the repository's default
 * branch rather than a literal "main" that trunk/master/develop repositories
 * do not have. Order: origin/HEAD (what the clone calls the default), a local
 * main, a local master, then the current branch's upstream. main and master
 * come before the upstream on purpose: a pushed feature branch tracks its own
 * remote twin, and diffing against that would silently shrink the impact
 * analysis to unpushed commits. Nothing resolves -> false, out = "main", and
 * the caller reports that ref as the one it tried. */
static bool detect_default_base(const char *root_path, char *out, size_t out_size) {
    char command[CBM_SZ_2K];
    char line[CBM_SZ_1K];
    snprintf(command, sizeof(command),
             "git -C " DETECT_GIT_Q "%s" DETECT_GIT_Q
             " symbolic-ref -q --short refs/remotes/origin/HEAD " DETECT_GIT_NULL,
             root_path);
    if (detect_git_first_line(command, line, sizeof(line))) {
        snprintf(out, out_size, "%s", line);
        return true;
    }
    static const char *const local_defaults[] = {"main", "master"};
    for (const char *candidate : local_defaults) {
        snprintf(command, sizeof(command),
                 "git -C " DETECT_GIT_Q "%s" DETECT_GIT_Q " rev-parse -q --verify " DETECT_GIT_Q
                 "refs/heads/%s^{commit}" DETECT_GIT_Q " " DETECT_GIT_NULL,
                 root_path, candidate);
        if (detect_git_first_line(command, line, sizeof(line))) {
            snprintf(out, out_size, "%s", candidate);
            return true;
        }
    }
    snprintf(command, sizeof(command),
             "git -C " DETECT_GIT_Q "%s" DETECT_GIT_Q
             " rev-parse -q --abbrev-ref --symbolic-full-name " DETECT_GIT_Q
             "@{upstream}" DETECT_GIT_Q " " DETECT_GIT_NULL,
             root_path);
    if (detect_git_first_line(command, line, sizeof(line))) {
        snprintf(out, out_size, "%s", line);
        return true;
    }
    snprintf(out, out_size, "%s", "main");
    return false;
}

/* Upstream #1951: the project root may be a SUBDIRECTORY of the Git worktree.
 * `git diff --name-only` and `git status --porcelain` report paths relative to
 * the Git ROOT, graph file_paths are relative to the project root. Fills
 * `prefix` with the project's position inside the worktree as
 * `rev-parse --show-prefix` prints it ("" at the root, else "dir/"). Any
 * failure (not a repository, malformed answer) leaves it "", which is the
 * pre-fix behaviour. */
static void detect_git_prefix(const char *root_path, char *prefix, size_t prefix_size) {
    prefix[0] = '\0';
    char command[CBM_SZ_2K];
    snprintf(command, sizeof(command),
             "git -C " DETECT_GIT_Q "%s" DETECT_GIT_Q " rev-parse --show-prefix " DETECT_GIT_NULL,
             root_path);
    bool complete = false;
    if (!detect_git_line(command, prefix, prefix_size, &complete) || !complete) {
        prefix[0] = '\0';
        return;
    }
    size_t length = strlen(prefix);
    if (length > 0 && prefix[length - 1] != '/') {
        prefix[0] = '\0';
    }
}
#undef DETECT_GIT_Q
#undef DETECT_GIT_NULL

/* Git-root path -> project-relative tail, or NULL for a path outside the
 * project (it has no graph nodes). */
static const char *detect_project_relative_path(const char *git_path, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    if (strncmp(git_path, prefix, prefix_length) != 0) {
        return NULL;
    }
    return git_path + prefix_length;
}

static char *handle_detect_changes(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, true);
    char *base_branch = cbm_mcp_get_string_arg(args, "base_branch");
    char *since = cbm_mcp_get_string_arg(args, "since");
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    int depth = cbm_mcp_get_int_arg(args, "depth", MCP_DEFAULT_BFS_DEPTH);
    depth = clamp_mcp_depth(depth, "detect_changes");

    /* scope: "files" = just changed files, "symbols" = files + symbols (default) */
    bool want_symbols = !scope || strcmp(scope, "symbols") == 0 || strcmp(scope, "impact") == 0;

    /* `since` (e.g. "HEAD~10", "v0.5.0") is the documented diff base but was
     * previously parsed and never used: it takes precedence over base_branch.
     * Route it through base_branch so the shared shell-arg validation and the
     * existing `<base>...HEAD` (three-dot) diff apply unchanged — `since` thus
     * adopts the same merge-base semantics base_branch already uses. */
    if (since && since[0]) {
        free(base_branch);
        base_branch = since; /* transfer ownership */
        since = NULL;
    }
    free(since); /* no-op after the swap (since is NULL); frees it otherwise */

    /* No base given: resolved from the repository once the project root is
     * known (upstream #1357). An explicit base_branch/since is used as given. */
    bool base_defaulted = base_branch == NULL;

    /* Reject shell metacharacters, and a leading '-', in the user-supplied
     * branch name. base_branch is spliced into `git diff --name-only
     * "<base>"...HEAD`; a value starting with '-' would be read by git as an
     * option rather than a ref (e.g. `--output=<path>` writes the diff to an
     * arbitrary file). A real git ref never begins with '-'. */
    if (base_branch && (!cbm_validate_shell_arg(base_branch) || base_branch[0] == '-')) {
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result("base_branch contains invalid characters", true);
    }

    char *root_path = get_project_root(srv, project);
    if (!root_path) {
        char *err = build_no_store_error(srv, project);
        char *res = cbm_mcp_text_result(err, true);
        free(err);
        free(project);
        free(base_branch);
        free(scope);
        return res;
    }

    if (!validate_search_path_arg(root_path)) {
        free(root_path);
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result("project path contains invalid characters", true);
    }

    /* The default base is looked up only once root_path is known to be safe
     * to splice into the git command line. */
    bool default_base_found = true;
    if (base_defaulted) {
        char default_base[CBM_SZ_1K];
        default_base_found = detect_default_base(root_path, default_base, sizeof(default_base));
        base_branch = heap_strdup(default_base);
        if (!base_branch) {
            free(root_path);
            free(project);
            free(scope);
            return cbm_mcp_text_result("out of memory", true);
        }
    }
    char git_prefix[CBM_SZ_4K];
    detect_git_prefix(root_path, git_prefix, sizeof(git_prefix));

    /* Get changed files via git (-C avoids cd + quoting issues on Windows).
     * Three sources are merged:
     *   1. committed changes vs base   (diff <base>...HEAD)
     *   2. unstaged tracked changes    (diff)
     *   3. untracked + staged-new files (status --porcelain) — these are
     *      invisible to `git diff` and were silently missed before, so a
     *      brand-new file never appeared until a manual re-index (#520).
     * status --porcelain prefixes each path with a 2-char code + space
     * ("?? path", "A  path"); the prefix is stripped when parsing below. */
    char cmd[CBM_SZ_2K];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "git -C \"%s\" diff --name-only \"%s\"...HEAD 2>NUL & "
             "git -C \"%s\" diff --name-only 2>NUL & "
             "git --no-optional-locks -C \"%s\" status --porcelain "
             "--untracked-files=normal 2>NUL",
             root_path, base_branch, root_path, root_path);
#else
    snprintf(cmd, sizeof(cmd),
             "{ git -C '%s' diff --name-only '%s'...HEAD 2>/dev/null; "
             "git -C '%s' diff --name-only 2>/dev/null; "
             "git --no-optional-locks -C '%s' status --porcelain "
             "--untracked-files=normal 2>/dev/null; } | sort -u",
             root_path, base_branch, root_path, root_path);
#endif

    FILE *fp = cbm_popen_git(cmd);
    if (!fp) {
        char errmsg[CBM_SZ_256];
        snprintf(errmsg, sizeof(errmsg),
                 "git diff failed: cannot execute command (%s). Check that git is installed.",
                 strerror(errno));
        free(root_path);
        free(project);
        free(base_branch);
        free(scope);
        return cbm_mcp_text_result(errmsg, true);
    }

    /* resolve_store already called via get_project_root above */
    cbm_store_t *store = srv->store;

    /* Direction of impact. Default inbound = the BLAST RADIUS: the transitive
     * CALLERS of the changed symbols, which may need review. outbound = what
     * the changed code depends on; both = union. */
    char *direction = cbm_mcp_get_string_arg(args, "direction");
    if (!direction) {
        direction = heap_strdup("inbound");
    }
    if (strcmp(direction, "inbound") != 0 && strcmp(direction, "outbound") != 0 &&
        strcmp(direction, "both") != 0) {
        free(direction);
        direction = heap_strdup("inbound");
    }

    /* Per-symbol impacted-row display cap (the module rollup stays complete).
     * impacted_total always reports the true count, so this never hides scale. */
    int imp_limit = cbm_mcp_get_int_arg(args, "limit", MCP_DEFAULT_IMPACT_LIMIT);
    if (imp_limit < 1) {
        imp_limit = 1;
    }
    if (imp_limit > MCP_BFS_LIMIT_MAX) {
        imp_limit = MCP_BFS_LIMIT_MAX;
    }

    /* Collect changed file paths into a C array (drives seeds, the rollup, and
     * the output). */
    char **files = NULL;
    int file_cap = 0;
    int64_t *seeds = NULL;
    int seed_count = 0;
    int seed_cap = 0;

    /* Hunk line ranges (unified=0 diff), used to scope seed detection to the
     * actually-changed lines instead of every definition in a changed file
     * (see detect_collect_seeds). Best-effort: any failure here just leaves
     * `hunks` empty and every file falls back to its previous whole-file
     * seeding — this is a precision improvement, not a correctness
     * dependency, so it is never treated as a request-level failure.
     *
     * Coordinate systems: `base...HEAD` hunks carry HEAD-side line numbers,
     * the worktree diff carries worktree-side ones, and node line ranges come
     * from the indexed snapshot. These agree while the index is fresh — the
     * watcher reindexes on HEAD movement and on a dirty tree — but a stale
     * index combined with insertions earlier in the file shifts the node lines
     * relative to the hunks and can mis-scope. The failure is bounded by
     * detect_collect_seeds' zero-overlap fallback: a file whose definitions all
     * miss reverts to whole-file seeding rather than dropping out.
     * --relative keeps hunk paths in the same project-relative coordinates as
     * `files` when the project root is a repository subdirectory (#1951). */
    cbm_changed_hunk_t *hunks = NULL;
    int hunk_count = 0;
    if (want_symbols) {
        char hunk_cmd[CBM_SZ_2K];
#ifdef _WIN32
        snprintf(hunk_cmd, sizeof(hunk_cmd),
                 "git -C \"%s\" diff --relative --unified=0 \"%s\"...HEAD 2>NUL & "
                 "git -C \"%s\" diff --relative --unified=0 2>NUL",
                 root_path, base_branch, root_path);
#else
        snprintf(hunk_cmd, sizeof(hunk_cmd),
                 "{ git -C '%s' diff --relative --unified=0 '%s'...HEAD 2>/dev/null; "
                 "git -C '%s' diff --relative --unified=0 2>/dev/null; }",
                 root_path, base_branch, root_path);
#endif
        FILE *hfp = cbm_popen_git(hunk_cmd);
        if (hfp) {
            /* Slurp the diff into one growable buffer — cbm_parse_hunks takes
             * the whole text. Capped so a pathological diff cannot balloon the
             * request; a truncated read just means fewer hunks, which the
             * cap check below then turns into whole-file seeding. */
            enum { HUNK_READ_MAX = 8u << 20, HUNK_READ_CHUNK = 64u << 10 };
            size_t hlen = 0;
            size_t hcap = HUNK_READ_CHUNK;
            char *hbuf = (char *)malloc(hcap);
            while (hbuf && hlen + SKIP_ONE < HUNK_READ_MAX) {
                if (hlen + HUNK_READ_CHUNK + SKIP_ONE > hcap) {
                    size_t ncap = hcap * 2;
                    char *grown = (char *)realloc(hbuf, ncap);
                    if (!grown) {
                        free(hbuf);
                        hbuf = NULL;
                        break;
                    }
                    hbuf = grown;
                    hcap = ncap;
                }
                size_t got = fread(hbuf + hlen, SKIP_ONE, HUNK_READ_CHUNK, hfp);
                hlen += got;
                if (got < HUNK_READ_CHUNK) {
                    break;
                }
            }
            (void)cbm_pclose(hfp);
            if (hbuf) {
                hbuf[hlen] = '\0';
                enum { HUNK_CAP = 4096 };
                hunks = (cbm_changed_hunk_t *)calloc((size_t)HUNK_CAP, sizeof(cbm_changed_hunk_t));
                if (hunks) {
                    hunk_count = cbm_parse_hunks(hbuf, hunks, HUNK_CAP);
                    /* A filled buffer means the diff was truncated: the hunks
                     * past the cap are gone, so files captured only partially
                     * would still look scoped and silently under-seed. Drop
                     * scoping for the whole request rather than under-report a
                     * large refactor — whole-file seeding is the safe side. */
                    if (hunk_count >= HUNK_CAP) {
                        cbm_log_info("detect_changes.hunks", "action", "scoping_disabled", "reason",
                                     "hunk_cap_reached");
                        free(hunks);
                        hunks = NULL;
                        hunk_count = 0;
                    }
                }
                free(hbuf);
            }
        }
    }

    char line[CBM_SZ_1K];
    int file_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* `git status --porcelain` prefixes each path with a two-character
         * status code and a space ("?? path", "A  path", " M path"). The two
         * `git diff --name-only` sources emit bare paths. Strip the porcelain
         * prefix when present so all three sources yield clean paths; for a
         * rename ("R  old -> new") keep the post-arrow destination path. */
        char *path_line = line;
        if (len > PAIR_LEN && line[PAIR_LEN] == ' ' && strchr(" MADRCU?!", line[0]) &&
            strchr(" MADRCU?!", line[1])) {
            path_line = line + PAIR_LEN + SKIP_ONE;
            char *arrow = strstr(path_line, " -> ");
            if (arrow) {
                enum { ARROW_LEN = 4 }; /* length of " -> " */
                path_line = arrow + ARROW_LEN;
            }
        }
        /* Git-root coordinates -> project-relative ones (#1951); a change
         * outside the project has no graph nodes and is left out. */
        path_line = (char *)detect_project_relative_path(path_line, git_prefix);
        if (!path_line || path_line[0] == '\0') {
            continue;
        }

        /* Dedup: the three git sources are sorted+unioned on POSIX but not on
         * Windows (separate commands), and a path can repeat. */
        bool dup = false;
        for (int i = 0; i < file_count; i++) {
            if (strcmp(files[i], path_line) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (file_count >= file_cap) {
            file_cap = file_cap ? file_cap * 2 : 16;
            files = (char **)safe_realloc(files, (size_t)file_cap * sizeof(char *));
        }
        files[file_count++] = heap_strdup(path_line);

        if (want_symbols) {
            detect_collect_seeds(store, project, path_line, hunks, hunk_count, &seeds, &seed_count,
                                 &seed_cap);
        }
    }
    int git_status = cbm_pclose(fp);

    /* merge-base SHA: the exact commit the diff is measured against, so the
     * result is reproducible even as base_branch advances. Best-effort. */
    char merge_base[64] = "";
    {
        char mbcmd[CBM_SZ_2K];
#ifdef _WIN32
        snprintf(mbcmd, sizeof(mbcmd), "git -C \"%s\" merge-base \"%s\" HEAD 2>NUL", root_path,
                 base_branch);
#else
        snprintf(mbcmd, sizeof(mbcmd), "git -C '%s' merge-base '%s' HEAD 2>/dev/null", root_path,
                 base_branch);
#endif
        FILE *mbfp = cbm_popen_git(mbcmd);
        if (mbfp) {
            if (fgets(merge_base, sizeof(merge_base), mbfp)) {
                size_t l = strlen(merge_base);
                while (l > 0 && (merge_base[l - 1] == '\n' || merge_base[l - 1] == '\r')) {
                    merge_base[--l] = '\0';
                }
            }
            (void)cbm_pclose(mbfp);
        }
    }

    /* The impact traversal: ONE multi-source BFS over all seeds. */
    cbm_traverse_result_t impact = {0};
    bool truncated = false;
    if (want_symbols && seed_count > 0) {
        (void)cbm_store_bfs_multi(store, seeds, seed_count, direction, NULL, 0, depth,
                                  MCP_BFS_LIMIT_MAX, &impact, &truncated);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    bool is_error = false;
    if (git_status != 0 && file_count == 0) {
        /* Name the ref that was tried: with the implicit default, callers
         * otherwise had to guess which base was meant (#1357). */
        char hint_buf[CBM_SZ_2K];
        if (!base_defaulted) {
            snprintf(hint_buf, sizeof(hint_buf),
                     "git diff exited with status %d. Check that branch '%s' exists.", git_status,
                     base_branch);
        } else if (!default_base_found) {
            snprintf(hint_buf, sizeof(hint_buf),
                     "git diff exited with status %d. No default base found (tried origin/HEAD, "
                     "main, master and the upstream; fell back to '%s'); pass base_branch.",
                     git_status, base_branch);
        } else {
            snprintf(hint_buf, sizeof(hint_buf),
                     "git diff exited with status %d against the default base '%s'; pass "
                     "base_branch.",
                     git_status, base_branch);
        }
        yyjson_mut_obj_add_strcpy(doc, root_obj, "hint", hint_buf);
        is_error = true;
    }

    if (!is_error && base_defaulted && !default_base_found) {
        yyjson_mut_obj_add_str(doc, root_obj, "hint",
                               "No default base found (tried origin/HEAD, main, master and the "
                               "upstream); fell back to 'main'. Pass base_branch.");
    }
    yyjson_mut_obj_add_strcpy(doc, root_obj, "base", base_branch);
    if (merge_base[0]) {
        yyjson_mut_obj_add_strcpy(doc, root_obj, "merge_base", merge_base);
    }
    yyjson_mut_val *changed = yyjson_mut_arr(doc);
    for (int i = 0; i < file_count; i++) {
        yyjson_mut_arr_add_strcpy(doc, changed, files[i]);
    }
    yyjson_mut_obj_add_val(doc, root_obj, "changed_files", changed);
    yyjson_mut_obj_add_int(doc, root_obj, "changed_count", file_count);
    yyjson_mut_obj_add_int(doc, root_obj, "depth", depth);
    yyjson_mut_obj_add_strcpy(doc, root_obj, "direction", direction);
    yyjson_mut_obj_add_int(doc, root_obj, "seed_count", seed_count);

    /* Per-symbol rows: capped at imp_limit, nearest hops first (the BFS result
     * is already (hop,id)-ordered). impacted_total stays exact. */
    int shown = impact.visited_count;
    if (shown > imp_limit) {
        shown = imp_limit;
    }
    yyjson_mut_val *impacted = yyjson_mut_arr(doc);
    for (int i = 0; i < shown; i++) {
        const cbm_node_t *nd = &impact.visited[i].node;
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "name", nd->name ? nd->name : "");
        yyjson_mut_obj_add_strcpy(doc, item, "qualified_name",
                                  nd->qualified_name ? nd->qualified_name : "");
        yyjson_mut_obj_add_strcpy(doc, item, "label", nd->label ? nd->label : "");
        yyjson_mut_obj_add_strcpy(doc, item, "file", nd->file_path ? nd->file_path : "");
        yyjson_mut_obj_add_int(doc, item, "hop", impact.visited[i].hop);
        yyjson_mut_arr_add_val(impacted, item);
    }
    yyjson_mut_obj_add_val(doc, root_obj, "impacted_symbols", impacted);
    yyjson_mut_obj_add_int(doc, root_obj, "impacted_total", impact.visited_count);
    yyjson_mut_obj_add_int(doc, root_obj, "impacted_shown", shown);
    if (shown < impact.visited_count) {
        yyjson_mut_obj_add_int(doc, root_obj, "impacted_omitted", impact.visited_count - shown);
    }
    if (truncated) {
        yyjson_mut_obj_add_bool(doc, root_obj, "truncated", true);
    }

    /* impacted_modules: a 2-segment rollup over the WHOLE impact set — always
     * complete, so the per-row cap above never hides where the radius lands. */
    {
        char **mnames = NULL;
        int *mcounts = NULL;
        int mn = 0;
        int mcap = 0;
        for (int i = 0; i < impact.visited_count; i++) {
            char key[CBM_SZ_512];
            detect_module_of(impact.visited[i].node.file_path, key, sizeof(key));
            int found = -1;
            for (int m = 0; m < mn; m++) {
                if (strcmp(mnames[m], key) == 0) {
                    found = m;
                    break;
                }
            }
            if (found >= 0) {
                mcounts[found]++;
                continue;
            }
            if (mn >= mcap) {
                mcap = mcap ? mcap * 2 : 16;
                mnames = (char **)safe_realloc(mnames, (size_t)mcap * sizeof(char *));
                mcounts = (int *)safe_realloc(mcounts, (size_t)mcap * sizeof(int));
            }
            mnames[mn] = heap_strdup(key);
            mcounts[mn] = 1;
            mn++;
        }
        yyjson_mut_val *mods = yyjson_mut_arr(doc);
        for (int m = 0; m < mn; m++) {
            yyjson_mut_val *o = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, o, "module", mnames[m]);
            yyjson_mut_obj_add_int(doc, o, "count", mcounts[m]);
            yyjson_mut_arr_add_val(mods, o);
            free(mnames[m]);
        }
        free(mnames);
        free(mcounts);
        yyjson_mut_obj_add_val(doc, root_obj, "impacted_modules", mods);
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    cbm_store_traverse_free(&impact);
    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }
    free(files);
    free(seeds);
    free(hunks);
    free(direction);
    free(root_path);
    free(project);
    free(base_branch);
    free(scope);

    return mcp_text_result_take_owned(json, is_error);
}

/* ── manage_adr ───────────────────────────────────────────────── */

/* ADR "sections" mode: list markdown headers ('#'-prefixed lines) from the
 * ADR content string. */
static void adr_list_sections_from_content(yyjson_mut_doc *doc, yyjson_mut_val *root_obj,
                                           const char *content) {
    yyjson_mut_val *sections = yyjson_mut_arr(doc);
    const char *p = content;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        while (linelen > 0 && p[linelen - SKIP_ONE] == '\r') {
            linelen--;
        }
        if (linelen > 0 && p[0] == '#') {
            char hdr[CBM_SZ_1K];
            if (linelen >= sizeof(hdr)) {
                linelen = sizeof(hdr) - SKIP_ONE;
            }
            memcpy(hdr, p, linelen);
            hdr[linelen] = '\0';
            yyjson_mut_arr_add_strcpy(doc, sections, hdr);
        }
        if (!eol) {
            break;
        }
        p = eol + SKIP_ONE;
    }
    yyjson_mut_obj_add_val(doc, root_obj, "sections", sections);
}

/* Read the legacy file-based ADR (<root>/.code-cortex/adr.md), used by
 * older versions. Returns a heap buffer (caller frees) or NULL if missing/
 * empty. Kept only to migrate old ADRs into the store (#256). */
static char *adr_read_legacy_file(const char *root_path) {
    if (!root_path) {
        return NULL;
    }
    char adr_path[CBM_SZ_4K];
    snprintf(adr_path, sizeof(adr_path), "%s/.code-cortex/adr.md", root_path);
    FILE *fp = cbm_fopen(adr_path, "r");
    if (!fp) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) {
        (void)fclose(fp);
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + SKIP_ONE);
    if (!buf) {
        (void)fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, SKIP_ONE, (size_t)sz, fp);
    buf[n] = '\0';
    (void)fclose(fp);
    if (buf[0] == '\0') {
        free(buf);
        return NULL;
    }
    return buf;
}

#define ADR_EMPTY_HINT                                                             \
    "No ADR yet. Create one with manage_adr(mode='update', "                       \
    "content='## PURPOSE\\n...\\n\\n## STACK\\n...\\n\\n## ARCHITECTURE\\n..."     \
    "\\n\\n## PATTERNS\\n...\\n\\n## TRADEOFFS\\n...\\n\\n## PHILOSOPHY\\n...'). " \
    "For guided creation: explore the codebase with get_architecture, "            \
    "then draft and store. Sections: PURPOSE, STACK, ARCHITECTURE, "               \
    "PATTERNS, TRADEOFFS, PHILOSOPHY."

/* ── inspect_symbol ───────────────────────────────────────────────
 *
 * One bounded call that answers "what is X, who calls it (with file:line),
 * what does it call, which tests cover it, and can I trust that" — the
 * sequence an agent otherwise spends 3-5 search/read/trace exchanges on. It
 * is assembled from the same store readers as get_code_snippet and trace_path
 * and adds nothing the graph does not already know; what it changes is that
 * every relationship is delivered with the evidence needed to verify it
 * without reopening whole files: call-site lines, resolver confidence and
 * strategy, per-file parse coverage, and index freshness. */

enum {
    INSPECT_DEFAULT_CALLERS = 25,
    INSPECT_DEFAULT_CALLEES = 15,
    INSPECT_DEFAULT_SOURCE_LINES = 60,
    INSPECT_DEFAULT_MAX_BYTES = 12000,
    INSPECT_MIN_MAX_BYTES = 1500,
    INSPECT_MAX_TESTS = 10,
    INSPECT_MAX_SUBCLASSES = 20,
    INSPECT_MAX_SITE_LINES = 8,
    INSPECT_MAX_ALSO = 5,
    INSPECT_MAX_CROSS_LANG = 5,
    INSPECT_SHRINK_ROUNDS = 24,
};

/* An edge is flagged "heuristic" when the resolver had nothing but a pattern
 * to go on. Registry confidences are not calibrated across strategies (a
 * correct C++ unique_name edge can carry 0.38), so the strategy is the primary
 * signal and the score only catches the speculative band. */
static const double INSPECT_SPECULATIVE_CONFIDENCE = 0.25;

static bool edge_is_heuristic(const char *strategy, double confidence) {
    if (confidence < INSPECT_SPECULATIVE_CONFIDENCE) {
        return true;
    }
    if (!strategy || !strategy[0]) {
        return false;
    }
    return strcmp(strategy, "service_pattern") == 0 || strcmp(strategy, "unknown") == 0 ||
           strcmp(strategy, "fuzzy") == 0 || strcmp(strategy, "name_only") == 0;
}

typedef struct {
    cbm_node_t node; /* owned */
    int lines[INSPECT_MAX_SITE_LINES];
    int line_count; /* stored */
    int line_total; /* seen */
    double confidence;
    char strategy[CBM_SZ_64];
} inspect_neighbor_t;

typedef struct {
    inspect_neighbor_t *items;
    int count;
    int cap;
} inspect_neighbors_t;

static void inspect_neighbors_free(inspect_neighbors_t *nb) {
    for (int i = 0; i < nb->count; i++) {
        free_node_contents(&nb->items[i].node);
    }
    free(nb->items);
    memset(nb, 0, sizeof(*nb));
}

/* "2026-09-06T16:19:13Z" -> time_t (UTC); 0 when absent or malformed. */
static time_t parse_iso8601_utc(const char *s) {
    if (!s || !s[0]) {
        return 0;
    }
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 || y < 1970 || mo < 1 ||
        mo > 12 || d < 1 || d > 31) {
        return 0;
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = sec;
#ifdef _WIN32
    return _mkgmtime(&tmv);
#else
    return timegm(&tmv);
#endif
}

static const char *lang_family(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) {
        return "other";
    }
    const char *ext = dot + 1;
    static const struct {
        const char *ext;
        const char *family;
    } map[] = {
        {"c", "C/C++"},        {"cc", "C/C++"},       {"cpp", "C/C++"},      {"cxx", "C/C++"},
        {"h", "C/C++"},        {"hh", "C/C++"},       {"hpp", "C/C++"},      {"hxx", "C/C++"},
        {"ipp", "C/C++"},      {"py", "Python"},      {"pyi", "Python"},     {"go", "Go"},
        {"js", "JavaScript"},  {"jsx", "JavaScript"}, {"mjs", "JavaScript"}, {"ts", "TypeScript"},
        {"tsx", "TypeScript"}, {"java", "Java"},      {"kt", "Kotlin"},      {"rs", "Rust"},
        {"rb", "Ruby"},        {"cs", "C#"},          {"php", "PHP"},        {"swift", "Swift"},
        {"sh", "Shell"},       {"bash", "Shell"},     {"lua", "Lua"}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(ext, map[i].ext) == 0) {
            return map[i].family;
        }
    }
    return "other";
}

static int inspect_neighbor_cmp(const void *a, const void *b) {
    const inspect_neighbor_t *x = (const inspect_neighbor_t *)a;
    const inspect_neighbor_t *y = (const inspect_neighbor_t *)b;
    int c = strcmp(x->node.file_path ? x->node.file_path : "",
                   y->node.file_path ? y->node.file_path : "");
    if (c != 0) {
        return c;
    }
    int lx = x->line_count ? x->lines[0] : x->node.start_line;
    int ly = y->line_count ? y->lines[0] : y->node.start_line;
    if (lx != ly) {
        return lx < ly ? -1 : 1;
    }
    return (x->node.id < y->node.id) ? -1 : (x->node.id > y->node.id);
}

static bool inspect_id_in_seeds(const cbm_node_t *seeds, int seed_count, int64_t id) {
    for (int i = 0; i < seed_count; i++) {
        if (seeds[i].id == id) {
            return true;
        }
    }
    return false;
}

/* Collect the nodes on the far side of `edge_type` edges from every seed,
 * deduplicated by node id, each with the call-site lines recorded on its
 * edges and the weakest resolver confidence among them. */
static void inspect_collect_neighbors(cbm_store_t *store, const cbm_node_t *seeds, int seed_count,
                                      bool inbound, const char *edge_type,
                                      inspect_neighbors_t *out) {
    std::unordered_map<int64_t, std::vector<int>> lines_by_neighbor;
    for (int s = 0; s < seed_count; s++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        if (inbound) {
            cbm_store_find_edges_by_target_type(store, seeds[s].id, edge_type, &edges, &edge_count);
        } else {
            cbm_store_find_edges_by_source_type(store, seeds[s].id, edge_type, &edges, &edge_count);
        }
        for (int e = 0; e < edge_count; e++) {
            int64_t other = inbound ? edges[e].source_id : edges[e].target_id;
            if (inspect_id_in_seeds(seeds, seed_count, other)) {
                continue; /* self-recursion / decl<->def edges */
            }
            inspect_neighbor_t *nb = NULL;
            for (int i = 0; i < out->count; i++) {
                if (out->items[i].node.id == other) {
                    nb = &out->items[i];
                    break;
                }
            }
            if (!nb) {
                cbm_node_t n = {0};
                if (cbm_store_find_node_by_id(store, other, &n) != CBM_STORE_OK) {
                    continue;
                }
                if (out->count == out->cap) {
                    int ncap = out->cap ? out->cap * 2 : 16;
                    inspect_neighbor_t *grown = (inspect_neighbor_t *)realloc(
                        out->items, (size_t)ncap * sizeof(inspect_neighbor_t));
                    if (!grown) {
                        free_node_contents(&n);
                        continue;
                    }
                    out->items = grown;
                    out->cap = ncap;
                }
                nb = &out->items[out->count++];
                memset(nb, 0, sizeof(*nb));
                nb->node = n;
                nb->confidence = 1.0;
            }
            /* New graphs retain all source lines on the relationship. Older
             * graphs only have the representative scalar line. Sort/dedup at
             * the boundary so malformed or repeated entries cannot inflate
             * the count or consume the bounded response slots. */
            auto &site_lines = lines_by_neighbor[other];
            const char *props = edges[e].properties_json;
            yyjson_doc *site_doc = props ? yyjson_read(props, strlen(props), 0) : nullptr;
            yyjson_val *site_root = site_doc ? yyjson_doc_get_root(site_doc) : nullptr;
            auto add_site = [&](yyjson_val *value) {
                if (yyjson_is_int(value)) {
                    int64_t line = yyjson_get_sint(value);
                    if (line > 0 && line < INT_MAX) {
                        site_lines.push_back((int)line);
                    }
                }
            };
            add_site(yyjson_obj_get(site_root, "line"));
            yyjson_val *array = yyjson_obj_get(site_root, "call_lines");
            size_t site_index, site_count;
            yyjson_val *site_value;
            yyjson_arr_foreach(array, site_index, site_count, site_value) {
                add_site(site_value);
            }
            yyjson_doc_free(site_doc);
            double conf = edge_props_confidence(edges[e].properties_json);
            if (conf < nb->confidence) {
                nb->confidence = conf;
                if (!edge_props_strategy(edges[e].properties_json, nb->strategy,
                                         sizeof(nb->strategy))) {
                    nb->strategy[0] = '\0';
                }
            } else if (!nb->strategy[0]) {
                edge_props_strategy(edges[e].properties_json, nb->strategy, sizeof(nb->strategy));
            }
        }
        cbm_store_free_edges(edges, edge_count);
    }
    // One caller can reach several declarations or overloads. Union across
    // their edges before applying the cap, so repeated lines cannot hide sites.
    for (int i = 0; i < out->count; i++) {
        auto &nb = out->items[i];
        auto found = lines_by_neighbor.find(nb.node.id);
        if (found == lines_by_neighbor.end()) {
            continue;
        }
        auto &lines = found->second;
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        nb.line_total = (int)std::min<size_t>(lines.size(), INT_MAX);
        nb.line_count = std::min(nb.line_total, (int)INSPECT_MAX_SITE_LINES);
        std::copy_n(lines.begin(), nb.line_count, nb.lines);
    }
    if (out->count > 1) {
        qsort(out->items, (size_t)out->count, sizeof(inspect_neighbor_t), inspect_neighbor_cmp);
    }
}

static void inspect_add_neighbor(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                                 const inspect_neighbor_t *nb, bool with_resolution) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, item, "name", nb->node.name ? nb->node.name : "");
    yyjson_mut_obj_add_strcpy(doc, item, "qualified_name",
                              nb->node.qualified_name ? nb->node.qualified_name : "");
    yyjson_mut_obj_add_strcpy(doc, item, "file", nb->node.file_path ? nb->node.file_path : "");
    if (nb->node.start_line > 0) {
        yyjson_mut_obj_add_int(doc, item, "start_line", nb->node.start_line);
    }
    if (nb->line_count > 0) {
        yyjson_mut_val *lines = yyjson_mut_arr(doc);
        for (int i = 0; i < nb->line_count; i++) {
            yyjson_mut_arr_add_int(doc, lines, nb->lines[i]);
        }
        yyjson_mut_obj_add_val(doc, item, "call_lines", lines);
        if (nb->line_total > nb->line_count) {
            yyjson_mut_obj_add_int(doc, item, "call_sites_total", nb->line_total);
        }
    }
    if (with_resolution) {
        yyjson_mut_obj_add_real(doc, item, "confidence", nb->confidence);
        if (nb->strategy[0]) {
            yyjson_mut_obj_add_strcpy(doc, item, "strategy", nb->strategy);
        }
        if (edge_is_heuristic(nb->strategy, nb->confidence)) {
            yyjson_mut_obj_add_bool(doc, item, "heuristic", true);
        }
    }
    yyjson_mut_arr_add_val(arr, item);
}

typedef struct {
    const char *file; // borrowed from declaration_nodes
    int line;
} inspect_declaration_t;

typedef struct {
    const char *file; // borrowed from callers/tests
    int call_sites;
    bool test;
} inspect_caller_file_t;

typedef struct {
    cbm_mcp_server_t *srv;
    cbm_store_t *store;
    const char *project;
    const char *symbol;
    cbm_node_t *nodes; /* all candidates; def is nodes[sel] */
    int node_count;
    int sel;
    char *root_path;
    inspect_neighbors_t callers; /* non-test */
    inspect_neighbors_t tests;   /* callers in test files */
    inspect_neighbors_t callees;
    inspect_neighbors_t subclasses;
    bool file_modified_after_index;
    char indexed_at[CBM_SZ_64];
    int callers_offset;
    inspect_declaration_t *decls;
    int decl_count;
    cbm_node_t *declaration_nodes;
    int declaration_node_count;
    int declarations_offset;
    inspect_caller_file_t *caller_files;
    int caller_file_count;
    int caller_files_offset;
    bool overload_family;
} inspect_ctx_t;

static bool node_is_declaration(const cbm_node_t *node) {
    return node->label && strcmp(node->label, "Declaration") == 0;
}

static bool inspect_same_declaration_key(const cbm_node_t *node, const char *key) {
    char *candidate = cbm_mcp_get_string_arg(node->properties_json, "declaration_key");
    bool same = candidate && key && strcmp(candidate, key) == 0;
    free(candidate);
    return same;
}

static int inspect_overload_family(const cbm_node_t *nodes, int count) {
    int first = -1, definitions = 0;
    size_t length = 0;
    for (int i = 0; i < count; i++) {
        if (node_is_declaration(&nodes[i]))
            continue;
        if (nodes[i].label && strcmp(nodes[i].label, "OverloadSet") == 0)
            continue;
        if (!nodes[i].qualified_name || !nodes[i].label ||
            (strcmp(nodes[i].label, "Function") != 0 && strcmp(nodes[i].label, "Method") != 0))
            return -1;
        const char *suffix = strstr(nodes[i].qualified_name, "@overload_");
        if (!suffix)
            return -1;
        size_t current = (size_t)(suffix - nodes[i].qualified_name);
        if (first < 0) {
            first = i;
            length = current;
        } else if (current != length ||
                   strncmp(nodes[first].qualified_name, nodes[i].qualified_name, length) != 0)
            return -1;
        definitions++;
    }
    if (definitions < 2)
        return -1;
    for (int i = 0; i < count; i++) {
        if (nodes[i].label && strcmp(nodes[i].label, "OverloadSet") == 0 &&
            (!nodes[i].qualified_name || strlen(nodes[i].qualified_name) != length ||
             strncmp(nodes[i].qualified_name, nodes[first].qualified_name, length) != 0))
            return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!node_is_declaration(&nodes[i]))
            continue;
        char *key = cbm_mcp_get_string_arg(nodes[i].properties_json, "declaration_key");
        bool matched = false;
        for (int j = 0; j < count && !matched; j++) {
            if (!node_is_declaration(&nodes[j]))
                matched = inspect_same_declaration_key(&nodes[j], key);
        }
        free(key);
        if (!matched)
            return -1;
    }
    return first;
}

static bool inspect_in_overload_family(const cbm_node_t *node, const char *base) {
    if (!node->qualified_name)
        return false;
    size_t n = strlen(base);
    return strcmp(node->qualified_name, base) == 0 ||
           (strncmp(node->qualified_name, base, n) == 0 &&
            strncmp(node->qualified_name + n, "@overload_", 10) == 0);
}

static void inspect_expand_overload_candidates(cbm_store_t *store, const char *project,
                                               cbm_node_t **nodes, int *count) {
    if (*count != 1 || !(*nodes)[0].label || strcmp((*nodes)[0].label, "OverloadSet") != 0)
        return;
    const char *base = (*nodes)[0].qualified_name;
    cbm_node_t *all = nullptr;
    int total = 0;
    cbm_store_find_nodes_by_name(store, project, (*nodes)[0].name, &all, &total);
    // Decide membership before moving records so declaration joins see every definition.
    std::vector<bool> keep((size_t)total, false);
    for (int i = 0; i < total; i++) {
        keep[i] = inspect_in_overload_family(&all[i], base);
        if (!keep[i] && node_is_declaration(&all[i])) {
            char *key = cbm_mcp_get_string_arg(all[i].properties_json, "declaration_key");
            for (int j = 0; j < total && !keep[i]; j++)
                if (inspect_in_overload_family(&all[j], base))
                    keep[i] = inspect_same_declaration_key(&all[j], key);
            free(key);
        }
    }
    int kept = 0;
    for (int i = 0; i < total; i++) {
        if (keep[i]) {
            if (i != kept) {
                all[kept] = all[i];
                memset(&all[i], 0, sizeof(all[i]));
            }
            kept++;
        } else
            free_node_contents(&all[i]);
    }
    if (kept) {
        cbm_store_free_nodes(*nodes, *count);
        *nodes = all;
        *count = kept;
    } else
        free(all);
}

/* A scoped name or exact declaration QN may initially find only prototypes.
 * Join their extraction identity to the definition before collecting callers. */
static void inspect_expand_declaration_candidates(cbm_store_t *store, const char *project,
                                                  cbm_node_t **nodes, int *count) {
    if (*count == 0 || !node_is_declaration(&(*nodes)[0])) {
        return;
    }
    char *key = cbm_mcp_get_string_arg((*nodes)[0].properties_json, "declaration_key");
    if (!key) {
        return;
    }
    for (int i = 0; i < *count; i++) {
        if (!node_is_declaration(&(*nodes)[i]) ||
            !inspect_same_declaration_key(&(*nodes)[i], key)) {
            free(key);
            return;
        }
    }
    cbm_node_t *candidates = nullptr;
    int candidate_count = 0;
    cbm_store_find_nodes_by_name(store, project, (*nodes)[0].name, &candidates, &candidate_count);
    int kept = 0;
    for (int i = 0; i < candidate_count; i++) {
        const char *label = candidates[i].label;
        bool callable = label && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0);
        if ((callable || node_is_declaration(&candidates[i])) &&
            inspect_same_declaration_key(&candidates[i], key)) {
            if (kept != i) {
                candidates[kept] = candidates[i];
                memset(&candidates[i], 0, sizeof(candidates[i]));
            }
            kept++;
        } else {
            free_node_contents(&candidates[i]);
        }
    }
    free(key);
    if (kept > 0) {
        cbm_store_free_nodes(*nodes, *count);
        *nodes = candidates;
        *count = kept;
    } else {
        free(candidates);
    }
}

/* All locations come from extraction. No header naming convention or source
 * reads are involved, and same-named symbols in other scopes do not match. */
static void inspect_find_header_decls(inspect_ctx_t *c, const cbm_node_t *def) {
    char *key = cbm_mcp_get_string_arg(def->properties_json, "declaration_key");
    if (!key || !def->name) {
        free(key);
        return;
    }
    cbm_store_find_nodes_by_name(c->store, c->project, def->name, &c->declaration_nodes,
                                 &c->declaration_node_count);
    if (c->declaration_node_count == 0) {
        free(key);
        return;
    }
    c->decls = (inspect_declaration_t *)calloc((size_t)c->declaration_node_count,
                                               sizeof(inspect_declaration_t));
    if (!c->decls) {
        free(key);
        return;
    }
    for (int i = 0; i < c->declaration_node_count; i++) {
        const cbm_node_t *node = &c->declaration_nodes[i];
        bool matches = inspect_same_declaration_key(node, key);
        if (!matches && c->overload_family) {
            for (int j = 0; j < c->node_count && !matches; j++) {
                if (node_is_declaration(&c->nodes[j]))
                    continue;
                char *other_key =
                    cbm_mcp_get_string_arg(c->nodes[j].properties_json, "declaration_key");
                matches = inspect_same_declaration_key(node, other_key);
                free(other_key);
            }
        }
        if (node_is_declaration(node) && node->file_path && matches) {
            c->decls[c->decl_count++] = {node->file_path, node->start_line};
        }
    }
    free(key);
    if (c->decl_count > 1) {
        qsort(c->decls, (size_t)c->decl_count, sizeof(*c->decls),
              [](const void *left, const void *right) {
                  const auto *a = (const inspect_declaration_t *)left;
                  const auto *b = (const inspect_declaration_t *)right;
                  int order = strcmp(a->file, b->file);
                  return order ? order : (a->line > b->line) - (a->line < b->line);
              });
        int unique = 1;
        for (int i = 1; i < c->decl_count; i++) {
            if (strcmp(c->decls[i].file, c->decls[unique - 1].file) != 0 ||
                c->decls[i].line != c->decls[unique - 1].line) {
                c->decls[unique++] = c->decls[i];
            }
        }
        c->decl_count = unique;
    }
}

/* Sort before paging, then combine all call sites for each file. A byte-limited
 * first page must neither change counts nor make later files unreachable. */
static void inspect_collect_caller_files(inspect_ctx_t *c) {
    size_t capacity = (size_t)c->callers.count + (size_t)c->tests.count;
    if (capacity == 0) {
        return;
    }
    c->caller_files = (inspect_caller_file_t *)calloc(capacity, sizeof(*c->caller_files));
    if (!c->caller_files) {
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        const inspect_neighbors_t *src = pass == 0 ? &c->callers : &c->tests;
        for (int i = 0; i < src->count; i++) {
            const inspect_neighbor_t *neighbor = &src->items[i];
            c->caller_files[c->caller_file_count++] = {
                neighbor->node.file_path ? neighbor->node.file_path : "",
                neighbor->line_total > 0 ? neighbor->line_total : 1, pass == 1};
        }
    }
    if (c->caller_file_count < 2) {
        return;
    }
    qsort(c->caller_files, (size_t)c->caller_file_count, sizeof(*c->caller_files),
          [](const void *left, const void *right) {
              return strcmp(((const inspect_caller_file_t *)left)->file,
                            ((const inspect_caller_file_t *)right)->file);
          });
    int unique = 1;
    for (int i = 1; i < c->caller_file_count; i++) {
        inspect_caller_file_t *last = &c->caller_files[unique - 1];
        if (strcmp(last->file, c->caller_files[i].file) == 0) {
            last->call_sites += c->caller_files[i].call_sites;
            last->test = last->test || c->caller_files[i].test;
        } else {
            c->caller_files[unique++] = c->caller_files[i];
        }
    }
    c->caller_file_count = unique;
}

/* Serialize one attempt at the given budgets. list_limit caps the secondary
 * lists (related_tests, subclasses, cross-language callers) and rollup_limit
 * the caller_files rollup, so a symbol called from hundreds of files can
 * still be brought under max_bytes. */
static char *inspect_build_json(inspect_ctx_t *c, int callers_limit, int callees_limit,
                                int source_lines, bool truncated, int list_limit, int rollup_limit,
                                int declaration_limit, bool compact_metadata = false) {
    const cbm_node_t *def = &c->nodes[c->sel];
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (!compact_metadata) {
        yyjson_mut_obj_add_strcpy(doc, root, "project", c->project);
    }

    /* Definition. */
    yyjson_mut_val *sym = yyjson_mut_obj(doc);
    if (!compact_metadata) {
        yyjson_mut_obj_add_strcpy(doc, sym, "name", def->name ? def->name : "");
        yyjson_mut_obj_add_strcpy(doc, sym, "qualified_name",
                                  def->qualified_name ? def->qualified_name : "");
    }
    yyjson_mut_obj_add_strcpy(doc, sym, "label", def->label ? def->label : "");
    if (!compact_metadata) {
        yyjson_mut_obj_add_strcpy(doc, sym, "file", def->file_path ? def->file_path : "");
    }
    yyjson_mut_obj_add_int(doc, sym, "start_line", def->start_line);
    yyjson_mut_obj_add_int(doc, sym, "end_line", def->end_line);
    yyjson_doc *props = def->properties_json
                            ? yyjson_read(def->properties_json, strlen(def->properties_json), 0)
                            : NULL;
    if (props && !compact_metadata) {
        yyjson_val *pr = yyjson_doc_get_root(props);
        static const char *keep[] = {"signature",      "return_type", "language", "is_exported",
                                     "is_entry_point", "complexity",  "lines"};
        for (size_t k = 0; k < sizeof(keep) / sizeof(keep[0]); k++) {
            yyjson_val *v = yyjson_obj_get(pr, keep[k]);
            if (!v) {
                continue;
            }
            if (yyjson_is_str(v)) {
                yyjson_mut_obj_add_strcpy(doc, sym, keep[k], yyjson_get_str(v));
            } else if (yyjson_is_bool(v)) {
                yyjson_mut_obj_add_bool(doc, sym, keep[k], yyjson_get_bool(v));
            } else if (yyjson_is_int(v)) {
                yyjson_mut_obj_add_int(doc, sym, keep[k], yyjson_get_int(v));
            }
        }
    }
    yyjson_doc_free(props);
    yyjson_mut_obj_add_val(doc, root, "symbol", sym);
    if (c->overload_family) {
        yyjson_mut_obj_add_str(doc, root, "relationship_scope", "overload_family");
        yyjson_mut_obj_add_str(
            doc, root, "symbol_note",
            "symbol and source describe one representative overload; relationships and "
            "declarations cover the family. "
            "Use also_defined_as qualified names to inspect individual overloads.");
    }

    /* Other nodes carrying the same name: the header declaration next to a
     * .cpp definition, an overload, a same-named script function. */
    yyjson_mut_val *also = yyjson_mut_arr(doc);
    int also_n = 0;
    int also_total = 0;
    for (int i = 0; i < c->node_count; i++) {
        if (i != c->sel && !node_is_declaration(&c->nodes[i]) &&
            strcmp(c->nodes[i].label, "OverloadSet") != 0) {
            also_total++;
        }
    }
    for (int i = 0; !compact_metadata && i < c->node_count && also_n < INSPECT_MAX_ALSO; i++) {
        if (i == c->sel || node_is_declaration(&c->nodes[i]) ||
            strcmp(c->nodes[i].label, "OverloadSet") == 0) {
            continue;
        }
        yyjson_mut_val *a = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, a, "qualified_name",
                                  c->nodes[i].qualified_name ? c->nodes[i].qualified_name : "");
        yyjson_mut_obj_add_strcpy(doc, a, "label", c->nodes[i].label ? c->nodes[i].label : "");
        yyjson_mut_obj_add_strcpy(doc, a, "file",
                                  c->nodes[i].file_path ? c->nodes[i].file_path : "");
        yyjson_mut_obj_add_int(doc, a, "start_line", c->nodes[i].start_line);
        char *signature = cbm_mcp_get_string_arg(c->nodes[i].properties_json, "signature");
        if (signature)
            yyjson_mut_obj_add_strcpy(doc, a, "signature", signature);
        free(signature);
        yyjson_mut_arr_add_val(also, a);
        also_n++;
    }
    if (also_n > 0) {
        yyjson_mut_obj_add_val(doc, root, "also_defined_as", also);
        if (also_total > also_n) {
            yyjson_mut_obj_add_int(doc, root, "also_defined_as_total", also_total);
        }
    }

    /* Source excerpt: the head of the definition, clipped to source_lines. */
    if (source_lines > 0 && def->file_path && def->start_line > 0) {
        int start = def->start_line;
        int end = def->end_line >= start ? def->end_line : start;
        bool clipped = false;
        if (end - start + 1 > source_lines) {
            end = start + source_lines - 1;
            clipped = true;
        }
        char *abs_path = NULL;
        snippet_source_check_t source_check;
        char *src = resolve_snippet_source(c->store, def->project, c->root_path, def->file_path,
                                           start, end, &abs_path, &source_check);
        if (source_check.drift) {
            /* Same withholding as get_code_snippet: the indexed range now holds
             * other bytes. */
            add_snippet_source_check(doc, root, source_check);
        }
        if (src) {
            char *safe = sanitize_utf8_lossy(src);
            yyjson_mut_obj_add_strcpy(doc, root, "source", safe ? safe : "");
            free(safe);
            yyjson_mut_obj_add_int(doc, root, "source_end_line", end);
            if (clipped) {
                yyjson_mut_obj_add_bool(doc, root, "source_clipped", true);
            }
        }
        free(abs_path);
        free(src);
    }

    /* Extraction-backed declaration locations, independently paged. */
    yyjson_mut_val *decls = yyjson_mut_arr(doc);
    int declarations_shown = 0;
    for (int i = c->declarations_offset;
         i < c->decl_count && declarations_shown < declaration_limit; i++, declarations_shown++) {
        yyjson_mut_val *d = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, d, "file", c->decls[i].file);
        yyjson_mut_obj_add_int(doc, d, "line", c->decls[i].line);
        yyjson_mut_arr_add_val(decls, d);
    }
    yyjson_mut_obj_add_val(doc, root, "declared_in", decls);
    yyjson_mut_obj_add_int(doc, root, "declared_in_total", c->decl_count);
    yyjson_mut_obj_add_int(doc, root, "declared_in_shown", declarations_shown);
    yyjson_mut_obj_add_int(doc, root, "declarations_offset", c->declarations_offset);
    yyjson_mut_obj_add_bool(doc, root, "declared_in_has_more",
                            c->declarations_offset < c->decl_count - declarations_shown);

    /* Trust signals. */
    yyjson_mut_val *idx = yyjson_mut_obj(doc);
    if (c->indexed_at[0]) {
        yyjson_mut_obj_add_strcpy(doc, idx, "indexed_at", c->indexed_at);
    }
    yyjson_mut_obj_add_bool(doc, idx, "file_modified_after_index", c->file_modified_after_index);
    yyjson_mut_obj_add_val(doc, root, "index", idx);
    if (c->file_modified_after_index) {
        yyjson_mut_obj_add_str(doc, root, "freshness_note",
                               "The defining file changed after it was indexed; line numbers and "
                               "relationships below may be stale. Re-run index_repository to "
                               "refresh.");
    }
    if (!compact_metadata) {
        add_snippet_coverage_note(doc, root, c->store, def);
    }

    /* Callers, paged. */
    yyjson_mut_val *callers = yyjson_mut_arr(doc);
    int heuristic = 0;
    int shown = 0;
    for (int i = 0; i < c->callers.count; i++) {
        if (edge_is_heuristic(c->callers.items[i].strategy, c->callers.items[i].confidence)) {
            heuristic++;
        }
        if (i < c->callers_offset || shown >= callers_limit) {
            continue;
        }
        inspect_add_neighbor(doc, callers, &c->callers.items[i], true);
        shown++;
    }
    yyjson_mut_obj_add_val(doc, root, "callers", callers);
    yyjson_mut_obj_add_int(doc, root, "callers_total", c->callers.count);
    yyjson_mut_obj_add_int(doc, root, "callers_offset", c->callers_offset);
    yyjson_mut_obj_add_bool(doc, root, "callers_has_more",
                            c->callers_offset + shown < c->callers.count);
    if (heuristic > 0) {
        yyjson_mut_obj_add_int(doc, root, "callers_heuristic", heuristic);
    }

    /* File rollup includes all production and test callers before paging. */
    yyjson_mut_val *rollup = yyjson_mut_arr(doc);
    int files_shown = 0;
    for (int i = c->caller_files_offset; i < c->caller_file_count && files_shown < rollup_limit;
         i++, files_shown++) {
        const inspect_caller_file_t *file = &c->caller_files[i];
        yyjson_mut_val *f = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, f, "file", file->file);
        yyjson_mut_obj_add_int(doc, f, "call_sites", file->call_sites);
        if (file->test) {
            yyjson_mut_obj_add_bool(doc, f, "test", true);
        }
        yyjson_mut_arr_add_val(rollup, f);
    }
    bool files_more = c->caller_files_offset < c->caller_file_count - files_shown;
    yyjson_mut_obj_add_val(doc, root, "caller_files", rollup);
    yyjson_mut_obj_add_int(doc, root, "caller_files_total", c->caller_file_count);
    yyjson_mut_obj_add_int(doc, root, "caller_files_shown", files_shown);
    yyjson_mut_obj_add_int(doc, root, "caller_files_offset", c->caller_files_offset);
    yyjson_mut_obj_add_bool(doc, root, "caller_files_has_more", files_more);
    if (files_more) {
        yyjson_mut_obj_add_bool(doc, root, "caller_files_truncated", true);
    }

    /* Tests that exercise the symbol (direct callers in test files). */
    yyjson_mut_val *tests = yyjson_mut_arr(doc);
    int tests_shown = 0;
    for (int i = 0;
         i < c->tests.count && tests_shown < INSPECT_MAX_TESTS && tests_shown < list_limit; i++) {
        inspect_add_neighbor(doc, tests, &c->tests.items[i], false);
        tests_shown++;
    }
    yyjson_mut_obj_add_val(doc, root, "related_tests", tests);
    yyjson_mut_obj_add_int(doc, root, "related_tests_total", c->tests.count);

    /* Cross-language callers: what no text search over one language shows. */
    const char *def_lang = lang_family(def->file_path);
    yyjson_mut_val *cross = yyjson_mut_arr(doc);
    int cross_n = 0;
    int cross_total = 0;
    for (int pass = 0; pass < 2; pass++) {
        const inspect_neighbors_t *src = pass == 0 ? &c->callers : &c->tests;
        for (int i = 0; i < src->count; i++) {
            const char *lang = lang_family(src->items[i].node.file_path);
            if (strcmp(lang, def_lang) == 0 || strcmp(lang, "other") == 0) {
                continue;
            }
            cross_total++;
            if (cross_n < INSPECT_MAX_CROSS_LANG && cross_n < list_limit) {
                yyjson_mut_val *x = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, x, "file", src->items[i].node.file_path);
                yyjson_mut_obj_add_strcpy(doc, x, "language", lang);
                yyjson_mut_obj_add_strcpy(doc, x, "name",
                                          src->items[i].node.name ? src->items[i].node.name : "");
                yyjson_mut_arr_add_val(cross, x);
                cross_n++;
            }
        }
    }
    if (cross_total > 0) {
        yyjson_mut_obj_add_val(doc, root, "cross_language_callers", cross);
        yyjson_mut_obj_add_int(doc, root, "cross_language_callers_total", cross_total);
    }

    /* Callees. */
    yyjson_mut_val *callees = yyjson_mut_arr(doc);
    for (int i = 0; i < c->callees.count && i < callees_limit; i++) {
        inspect_add_neighbor(doc, callees, &c->callees.items[i], false);
    }
    yyjson_mut_obj_add_val(doc, root, "callees", callees);
    yyjson_mut_obj_add_int(doc, root, "callees_total", c->callees.count);

    /* Subtypes, for class-like symbols. */
    if (c->subclasses.count > 0) {
        yyjson_mut_val *subs = yyjson_mut_arr(doc);
        for (int i = 0; i < c->subclasses.count && i < INSPECT_MAX_SUBCLASSES && i < list_limit;
             i++) {
            inspect_add_neighbor(doc, subs, &c->subclasses.items[i], false);
        }
        yyjson_mut_obj_add_val(doc, root, "subclasses", subs);
        yyjson_mut_obj_add_int(doc, root, "subclasses_total", c->subclasses.count);
    }

    if (compact_metadata) {
        yyjson_mut_obj_add_bool(doc, root, "metadata_omitted", true);
        yyjson_mut_obj_add_int(doc, sym, "id", def->id);
    }
    if (truncated || files_more || c->declarations_offset < c->decl_count - declarations_shown) {
        if (truncated) {
            yyjson_mut_obj_add_bool(doc, root, "truncated", true);
        }
        yyjson_mut_obj_add_str(
            doc, root, "continuation",
            "Advance callers_offset, caller_files_offset or declarations_offset by the shown "
            "array length. If a page is empty or metadata_omitted, raise max_bytes.");
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return json;
}

static bool node_is_class_like(const cbm_node_t *n) {
    return n->label && (strcmp(n->label, "Class") == 0 || strcmp(n->label, "Struct") == 0 ||
                        strcmp(n->label, "Interface") == 0 || strcmp(n->label, "Trait") == 0);
}

static char *handle_inspect_symbol_impl(cbm_mcp_server_t *srv, const char *args) {
    char *symbol = cbm_mcp_get_string_arg(args, "symbol");
    if (!symbol) {
        symbol = cbm_mcp_get_string_arg(args, "name");
    }
    if (!symbol) {
        symbol = cbm_mcp_get_string_arg(args, "qualified_name");
    }
    if (!symbol) {
        symbol = cbm_mcp_get_string_arg(args, "function_name");
    }
    if (!symbol || !symbol[0]) {
        free(symbol);
        return cbm_mcp_text_result(
            "{\"error\":\"symbol is required\",\"hint\":\"Pass the function, method, class or "
            "variable name (Class::method and qualified names are accepted).\"}",
            true);
    }
    char *project = get_project_arg_or_cwd(srv, args, true);
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        free(symbol);
        REQUIRE_STORE(store, project);
    }
    char *not_indexed = verify_project_indexed(store, project);
    if (not_indexed) {
        free(symbol);
        free(project);
        return not_indexed;
    }

    int callers_limit = cbm_mcp_get_int_arg(args, "callers_limit", INSPECT_DEFAULT_CALLERS);
    int callees_limit = cbm_mcp_get_int_arg(args, "callees_limit", INSPECT_DEFAULT_CALLEES);
    int source_lines = cbm_mcp_get_int_arg(args, "source_lines", INSPECT_DEFAULT_SOURCE_LINES);
    int max_bytes = cbm_mcp_get_int_arg(args, "max_bytes", INSPECT_DEFAULT_MAX_BYTES);
    int callers_offset = cbm_mcp_get_int_arg(args, "callers_offset", 0);
    int files_offset = cbm_mcp_get_int_arg(args, "caller_files_offset", 0);
    int declarations_offset = cbm_mcp_get_int_arg(args, "declarations_offset", 0);
    if (callers_limit < 0) {
        callers_limit = 0;
    }
    if (callees_limit < 0) {
        callees_limit = 0;
    }
    if (source_lines < 0) {
        source_lines = 0;
    }
    if (source_lines > MCP_SNIPPET_MAX_LINES) {
        source_lines = MCP_SNIPPET_MAX_LINES;
    }
    if (max_bytes < INSPECT_MIN_MAX_BYTES) {
        max_bytes = INSPECT_MIN_MAX_BYTES;
    }
    if (callers_offset < 0) {
        callers_offset = 0;
    }

    /* Resolution: exact name, exact QN, then scoped/partial QN. */
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_name(store, project, symbol, &nodes, &node_count);
    if (node_count == 0) {
        cbm_node_t qn_node = {0};
        if (cbm_store_find_node_by_qn(store, project, symbol, &qn_node) == CBM_STORE_OK) {
            nodes = (cbm_node_t *)malloc(sizeof(cbm_node_t));
            if (nodes) {
                nodes[0] = qn_node;
                node_count = 1;
            } else {
                free_node_contents(&qn_node);
            }
        }
    }
    if (node_count == 0 && (strstr(symbol, "::") || strchr(symbol, '.'))) {
        find_nodes_by_scoped_name(store, project, symbol, &nodes, &node_count);
    }
    if (node_count == 0) {
        cbm_store_find_nodes_by_qn_suffix(store, project, symbol, &nodes, &node_count);
    }
    if (node_count == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "error", "symbol not found");
        yyjson_mut_obj_add_strcpy(doc, root, "symbol", symbol);
        yyjson_mut_obj_add_strcpy(doc, root, "project", project);
        yyjson_mut_obj_add_str(
            doc, root, "hint",
            "No graph node has this name. Try search_graph(query=\"<words>\") "
            "for a ranked lookup, or search source for unsupported macros and syntax.");
        char *json = yy_doc_to_str(doc);
        yyjson_mut_doc_free(doc);
        char *res = cbm_mcp_text_result(json, true);
        free(json);
        free(symbol);
        free(project);
        return res;
    }

    inspect_expand_declaration_candidates(store, project, &nodes, &node_count);
    inspect_expand_overload_candidates(store, project, &nodes, &node_count);

    /* A class name also matches its constructors (Methods whose QN extends
     * the class QN). "Inspect Layer" means the class; keep the constructors
     * as also_defined_as rather than reporting an ambiguity. */
    bool ambiguous = false;
    int sel = -1;
    int class_idx = -1;
    int class_count = 0;
    for (int i = 0; i < node_count; i++) {
        if (node_is_class_like(&nodes[i])) {
            class_idx = i;
            class_count++;
        }
    }
    if (class_count == 1) {
        /* Same-named Methods are constructors (declared in the header, defined
         * in the .cpp, so their QNs differ by path); anything else is a real
         * ambiguity. */
        bool others_are_ctors = true;
        for (int i = 0; i < node_count && others_are_ctors; i++) {
            if (i != class_idx && !node_is_declaration(&nodes[i]) &&
                (!nodes[i].label || strcmp(nodes[i].label, "Method") != 0)) {
                others_are_ctors = false;
            }
        }
        if (others_are_ctors) {
            sel = class_idx;
        }
    }
    if (sel < 0) {
        sel = pick_resolved_node(nodes, node_count, &ambiguous);
        if (ambiguous && node_is_declaration(&nodes[0])) {
            char *key = cbm_mcp_get_string_arg(nodes[0].properties_json, "declaration_key");
            bool equivalent = key != nullptr;
            for (int i = 0; i < node_count && equivalent; i++) {
                equivalent =
                    node_is_declaration(&nodes[i]) && inspect_same_declaration_key(&nodes[i], key);
            }
            free(key);
            if (equivalent) {
                sel = 0;
                ambiguous = false;
            }
        }
    }
    int overload_sel = ambiguous ? inspect_overload_family(nodes, node_count) : -1;
    if (overload_sel >= 0) {
        sel = overload_sel;
        ambiguous = false;
    }
    if (ambiguous) {
        char *res = snippet_suggestions(symbol, nodes, node_count);
        cbm_store_free_nodes(nodes, node_count);
        free(symbol);
        free(project);
        return res;
    }

    inspect_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.srv = srv;
    c.store = store;
    c.project = project;
    c.symbol = symbol;
    c.nodes = nodes;
    c.node_count = node_count;
    c.sel = sel;
    c.overload_family = overload_sel >= 0;
    c.callers_offset = callers_offset;
    c.caller_files_offset = files_offset < 0 ? 0 : files_offset;
    c.declarations_offset = declarations_offset < 0 ? 0 : declarations_offset;
    c.root_path = get_project_root(srv, project);
    inspect_find_header_decls(&c, &nodes[sel]);

    /* Index freshness + indexed_at. */
    cbm_project_t proj = {0};
    if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK) {
        if (proj.indexed_at) {
            snprintf(c.indexed_at, sizeof(c.indexed_at), "%s", proj.indexed_at);
        }
        cbm_project_free_fields(&proj);
    }
    if (c.root_path && nodes[sel].file_path) {
        /* Compare the file against the project's recorded indexed_at, not the
         * db file's mtime (a restored or vacuumed db is newer than the source
         * it no longer reflects). Falls back to the db mtime when indexed_at
         * is absent or unparseable. */
        char abs_path[CBM_SZ_1K];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", c.root_path, nodes[sel].file_path);
        struct stat sf;
        if (stat(abs_path, &sf) == 0) {
            time_t indexed = parse_iso8601_utc(c.indexed_at);
            if (indexed <= 0) {
                char db_path[CBM_SZ_1K];
                project_db_path(project, db_path, sizeof(db_path));
                struct stat sd;
                if (db_path[0] && stat(db_path, &sd) == 0) {
                    indexed = sd.st_mtime;
                }
            }
            if (indexed > 0 && sf.st_mtime > indexed) {
                c.file_modified_after_index = true;
            }
        }
    }

    /* Callers of every candidate (a header declaration and its .cpp
     * definition are separate nodes and calls may attach to either), split
     * into production callers and tests. */
    inspect_neighbors_t all_in;
    memset(&all_in, 0, sizeof(all_in));
    inspect_collect_neighbors(store, nodes, node_count, true, "CALLS", &all_in);
    for (int i = 0; i < all_in.count; i++) {
        inspect_neighbors_t *dst =
            is_test_file(all_in.items[i].node.file_path) ? &c.tests : &c.callers;
        if (dst->count == dst->cap) {
            int ncap = dst->cap ? dst->cap * 2 : 16;
            inspect_neighbor_t *grown = (inspect_neighbor_t *)realloc(
                dst->items, (size_t)ncap * sizeof(inspect_neighbor_t));
            if (!grown) {
                free_node_contents(&all_in.items[i].node);
                continue;
            }
            dst->items = grown;
            dst->cap = ncap;
        }
        dst->items[dst->count++] = all_in.items[i]; /* ownership moves */
    }
    free(all_in.items);
    inspect_collect_caller_files(&c);

    inspect_collect_neighbors(store, c.overload_family ? nodes : &nodes[sel],
                              c.overload_family ? node_count : 1, false, "CALLS", &c.callees);
    if (node_is_class_like(&nodes[sel])) {
        inspect_collect_neighbors(store, nodes, node_count, true, "INHERITS", &c.subclasses);
        inspect_collect_neighbors(store, nodes, node_count, true, "IMPLEMENTS", &c.subclasses);
    }

    /* Fit the byte budget: shrink source first, then the lists. */
    char *json = NULL;
    bool truncated = false;
    int cl = callers_limit;
    int ce = callees_limit;
    int sl = source_lines;
    int ll = INSPECT_MAX_SUBCLASSES; /* secondary lists */
    int rl = 200;                    /* caller_files rollup */
    int dl = 100;                    /* declaration locations */
    for (int round = 0; round <= INSPECT_SHRINK_ROUNDS; round++) {
        free(json);
        json = inspect_build_json(&c, cl, ce, sl, truncated, ll, rl, dl);
        if (!json || (int)strlen(json) <= max_bytes) {
            break;
        }
        if (round == INSPECT_SHRINK_ROUNDS) {
            break;
        }
        truncated = true;
        /* Order: source, then the paged lists, then the secondary lists and
         * the rollup; totals stay exact whatever is cut. */
        if (sl > 12) {
            sl = sl / 2 < 12 ? 12 : sl / 2;
        } else if (cl > 3 || ce > 3) {
            cl = cl > 3 ? cl / 2 : cl;
            ce = ce > 3 ? ce / 2 : ce;
        } else if (sl > 0) {
            sl = 0;
        } else if (ll > 3 || rl > 10 || dl > 3) {
            ll = ll > 3 ? ll / 2 : ll;
            rl = rl > 10 ? rl / 2 : rl;
            dl = dl > 3 ? dl / 2 : dl;
        } else {
            cl = 0;
            ce = 0;
            ll = 0;
            rl = 0;
            dl = 0;
        }
    }
    if (json && strlen(json) > (size_t)max_bytes) {
        free(json);
        json = inspect_build_json(&c, 0, 0, 0, true, 0, 0, 0, true);
    }

    char *result = cbm_mcp_text_result(json ? json : "{}", false);
    free(json);
    inspect_neighbors_free(&c.callers);
    inspect_neighbors_free(&c.tests);
    inspect_neighbors_free(&c.callees);
    inspect_neighbors_free(&c.subclasses);
    free(c.root_path);
    free(c.decls);
    cbm_store_free_nodes(c.declaration_nodes, c.declaration_node_count);
    free(c.caller_files);
    cbm_store_free_nodes(nodes, node_count);
    free(symbol);
    free(project);
    return result;
}

/* Error/suggestion replies also obey the payload byte contract. They can contain
 * caller-supplied names or a large project/candidate catalog. */
static char *handle_inspect_symbol(cbm_mcp_server_t *srv, const char *args) {
    char *result = handle_inspect_symbol_impl(srv, args);
    int budget = cbm_mcp_get_int_arg(args, "max_bytes", INSPECT_DEFAULT_MAX_BYTES);
    if (budget < INSPECT_MIN_MAX_BYTES) {
        budget = INSPECT_MIN_MAX_BYTES;
    }
    yyjson_doc *doc = result ? yyjson_read(result, strlen(result), 0) : nullptr;
    yyjson_val *content = yyjson_obj_get(yyjson_doc_get_root(doc), "content");
    const char *text = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(content, 0), "text"));
    bool oversized = text && strlen(text) > (size_t)budget;
    yyjson_doc_free(doc);
    if (oversized) {
        free(result);
        return cbm_mcp_text_result(
            "{\"error\":\"response metadata exceeds max_bytes\",\"truncated\":true,"
            "\"metadata_omitted\":true,\"continuation\":\"Raise max_bytes or use an exact "
            "qualified_name and explicit project to narrow the request.\"}",
            true);
    }
    return result;
}

static char *handle_manage_adr(cbm_mcp_server_t *srv, const char *args) {
    char *project = get_project_arg_or_cwd(srv, args, false);
    char *mode_str = cbm_mcp_get_string_arg(args, "mode");
    char *content = cbm_mcp_get_string_arg(args, "content");

    if (!mode_str) {
        mode_str = heap_strdup("get");
    }

    /* `sections` used to be advertised as an input argument, but it was never
     * consumed: mode=update still replaced the whole document. Reject the old
     * shape explicitly before opening a store so a stale client cannot mistake
     * a whole-document replacement for a section-scoped update. */
    bool has_sections_arg = false;
    yyjson_doc *args_doc = yyjson_read(args, strlen(args), 0);
    if (args_doc) {
        yyjson_val *args_root = yyjson_doc_get_root(args_doc);
        has_sections_arg =
            args_root && yyjson_is_obj(args_root) && yyjson_obj_get(args_root, "sections") != NULL;
        yyjson_doc_free(args_doc);
    }
    if (has_sections_arg) {
        free(project);
        free(mode_str);
        free(content);
        return cbm_mcp_text_result(
            "{\"status\":\"invalid_arguments\",\"error\":\"The sections argument is not an "
            "update primitive and has been removed. No ADR write was performed.\"}",
            true);
    }

    /* ADRs are stored in the SQLite store (project_summaries), the SAME
     * backend the UI /api/adr endpoints use — so writes via the MCP tool and
     * the UI are visible to each other (#256). */
    cbm_store_t *resolved = resolve_store(srv, project);
    if (!resolved) {
        char *err = build_no_store_error(srv, project);
        char *res = cbm_mcp_text_result(err, true);
        free(err);
        free(project);
        free(mode_str);
        free(content);
        return res;
    }

    /* resolve_store opens file-backed projects READ-ONLY (query stores must
     * not mutate the DB). manage_adr is the only resolve_store caller that
     * WRITES, so it needs a writable handle. For a file-backed project open a
     * dedicated read-write handle to the same DB file (the project is verified
     * to exist via resolve_store, so cbm_store_open_path won't create a ghost
     * DB). For an in-memory / embedded store (db_path == NULL) the resolved
     * store is already writable — use it directly. */
    cbm_store_t *store = resolved;
    cbm_store_t *owned_rw = NULL;
    const char *resolved_db_path = cbm_store_db_path(resolved);
    if (resolved_db_path) {
        owned_rw = cbm_store_open_path(resolved_db_path);
        if (!owned_rw) {
            char *err = build_no_store_error(srv, project);
            char *res = cbm_mcp_text_result(err, true);
            free(err);
            free(project);
            free(mode_str);
            free(content);
            return res;
        }
        store = owned_rw;
    }

    /* One-time migration: older versions wrote ADRs to a file at
     * <root>/.code-cortex/adr.md. If the store has no ADR yet but that
     * legacy file exists, import it so nothing is lost on upgrade. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    bool have_adr = (cbm_store_adr_get(store, project, &adr) == CBM_STORE_OK);
    if (!have_adr) {
        char *root_path = get_project_root(srv, project);
        char *legacy = adr_read_legacy_file(root_path);
        free(root_path);
        if (legacy) {
            if (cbm_store_adr_store(store, project, legacy) == CBM_STORE_OK) {
                have_adr = (cbm_store_adr_get(store, project, &adr) == CBM_STORE_OK);
            }
            free(legacy);
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_obj);

    bool is_error = false;
    if ((strcmp(mode_str, "update") == 0 || strcmp(mode_str, "store") == 0) && content) {
        if (cbm_store_adr_store(store, project, content) == CBM_STORE_OK) {
            yyjson_mut_obj_add_str(doc, root_obj, "status", "updated");
            yyjson_mut_obj_add_str(doc, root_obj, "semantics", "whole_document_replaced");
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "status", "write_error");
            is_error = true;
        }
    } else if (strcmp(mode_str, "sections") == 0) {
        adr_list_sections_from_content(doc, root_obj, have_adr ? adr.content : NULL);
    } else { /* get */
        if (have_adr && adr.content) {
            yyjson_mut_obj_add_strcpy(doc, root_obj, "content", adr.content);
        } else {
            yyjson_mut_obj_add_str(doc, root_obj, "content", "");
            yyjson_mut_obj_add_str(doc, root_obj, "status", "no_adr");
            yyjson_mut_obj_add_str(doc, root_obj, "adr_hint", ADR_EMPTY_HINT);
        }
    }

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    if (have_adr) {
        cbm_store_adr_free(&adr);
    }
    if (owned_rw) {
        cbm_store_close(owned_rw);
    }
    free(project);
    free(mode_str);
    free(content);

    return mcp_text_result_take_owned(json, is_error);
}

/* ── ingest_traces ────────────────────────────────────────────── */

static char *handle_ingest_traces(cbm_mcp_server_t *srv, const char *args) {
    (void)srv;
    /* Parse traces array from JSON args */
    yyjson_doc *adoc = yyjson_read(args, strlen(args), 0);
    int trace_count = 0;

    if (adoc) {
        yyjson_val *aroot = yyjson_doc_get_root(adoc);
        yyjson_val *traces = yyjson_obj_get(aroot, "traces");
        if (traces && yyjson_is_arr(traces)) {
            trace_count = (int)yyjson_arr_size(traces);
        }
        yyjson_doc_free(adoc);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "status", "accepted");
    yyjson_mut_obj_add_int(doc, root, "traces_received", trace_count);
    yyjson_mut_obj_add_str(doc, root, "note",
                           "Runtime edge creation from traces not yet implemented");

    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);

    return mcp_text_result_take_owned(json, false);
}

/* ── Tool dispatch ────────────────────────────────────────────── */

/* Read tools whose work is store reads from start to end run them in one store
 * read batch (cbm_store_read_batch_begin). Left out: tools that write, or hold
 * the store across a long scan or a git subprocess (search_code,
 * index_status, detect_changes, inspect_symbol). */
static bool mcp_tool_reads_in_batch(const char *tool_name) {
    static const char *const batched[] = {
        "query_graph",      "trace_path",       "trace_call_path",  "search_graph",
        "get_architecture", "get_graph_schema", "get_code_snippet",
    };
    for (size_t i = 0; tool_name && i < sizeof(batched) / sizeof(batched[0]); i++) {
        if (strcmp(tool_name, batched[i]) == 0) {
            return true;
        }
    }
    return false;
}

static char *dispatch_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json);
static char *handle_tool_impl(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json);

char *cbm_mcp_handle_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {
    /* A project argument given as a path scopes lookups to that directory for
     * this call only, so the expectation is restored on the way out and never
     * leaks into a later call or a direct cbm_mcp_* lookup. One the caller set
     * (a hook walk-up level, cbm_mcp_expect_project_root) stays in force for
     * the call: clearing it here answered a walk-up level from another
     * checkout's database under the same derived name. */
    char saved[sizeof(g_expected_root)];
    memcpy(saved, g_expected_root, sizeof(saved));
    char *result = handle_tool_impl(srv, tool_name, args_json);
    memcpy(g_expected_root, saved, sizeof(saved));
    /* Cached statements are reset on their next use, so the call's last
     * queries still hold a read snapshot. Release it now, not when the next
     * request starts: an idle connection would otherwise pin the snapshot
     * for as long as the client is quiet, and block a WAL reset (a cache
     * restore's checkpoint) all that time. */
    if (srv && srv->store) {
        cbm_store_end_reads(srv->store);
    }
    return result;
}

static char *handle_tool_impl(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {
    g_project_note[0] = '\0';
    /* Every tool call starts here -- tool-server worker, in-process stdio
     * server, CLI, hooks -- and none is nested inside another's statement
     * loop: a re-index or delete may have replaced the file behind the
     * cached store, or written into it in place, since the last call, so
     * never answer from a stale generation or snapshot. */
    cbm_mcp_server_revalidate_store(srv);
    if (!tool_name) {
        return cbm_mcp_text_result("missing tool name", true);
    }
    if (!srv) {
        return dispatch_tool(srv, tool_name, args_json);
    }
    srv->read_batch_tool = mcp_tool_reads_in_batch(tool_name);
    char *result = dispatch_tool(srv, tool_name, args_json);
    /* The store outlives the request (it is cached between calls): end its
     * batch here so no read transaction stays open between tool calls. */
    srv->read_batch_tool = false;
    cbm_store_read_batch_end(srv->store);
    return result;
}

static char *dispatch_tool(cbm_mcp_server_t *srv, const char *tool_name, const char *args_json) {

    if (strcmp(tool_name, "list_projects") == 0) {
        return handle_list_projects(srv, args_json);
    }
    if (strcmp(tool_name, "get_graph_schema") == 0) {
        return handle_get_graph_schema(srv, args_json);
    }
    if (strcmp(tool_name, "search_graph") == 0) {
        return handle_search_graph(srv, args_json);
    }
    if (strcmp(tool_name, "query_graph") == 0) {
        return handle_query_graph(srv, args_json);
    }
    if (strcmp(tool_name, "index_status") == 0) {
        return handle_index_status(srv, args_json);
    }
    if (strcmp(tool_name, "delete_project") == 0) {
        return handle_delete_project(srv, args_json);
    }
    if (strcmp(tool_name, "trace_path") == 0 || strcmp(tool_name, "trace_call_path") == 0) {
        return handle_trace_call_path(srv, args_json);
    }
    if (strcmp(tool_name, "get_architecture") == 0) {
        return handle_get_architecture(srv, args_json);
    }

    /* Pipeline-dependent tools */
    if (strcmp(tool_name, "index_repository") == 0) {
        return handle_index_repository(srv, args_json);
    }
    if (strcmp(tool_name, "get_code_snippet") == 0) {
        return handle_get_code_snippet(srv, args_json);
    }
    if (strcmp(tool_name, "inspect_symbol") == 0) {
        return handle_inspect_symbol(srv, args_json);
    }
    if (strcmp(tool_name, "search_code") == 0) {
        return handle_search_code(srv, args_json);
    }
    if (strcmp(tool_name, "detect_changes") == 0) {
        return handle_detect_changes(srv, args_json);
    }
    if (strcmp(tool_name, "manage_adr") == 0) {
        return handle_manage_adr(srv, args_json);
    }
    if (strcmp(tool_name, "ingest_traces") == 0) {
        return handle_ingest_traces(srv, args_json);
    }
    char msg[CBM_SZ_256];
    snprintf(msg, sizeof(msg), "unknown tool: %s", tool_name);
    return cbm_mcp_text_result(msg, true);
}

/* ── Session detection + auto-index ────────────────────────────── */

/* Detect session root from CWD (fallback: single indexed project from DB). */
static void detect_session(cbm_mcp_server_t *srv) {
    if (srv->session_detected) {
        return;
    }
    srv->session_detected = true;

    /* 1. Try CWD */
    char cwd[CBM_SZ_1K];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        const char *home = cbm_get_home_dir();
        /* Skip useless roots: / and $HOME */
        if (strcmp(cwd, "/") != 0 && (home == NULL || strcmp(cwd, home) != 0)) {
            snprintf(srv->session_root, sizeof(srv->session_root), "%s", cwd);
            cbm_log_info("session.root.cwd", "path", cwd);
        }
    }

    /* Derive project name from path — must match cbm_project_name_from_path
     * used by the pipeline, otherwise session queries look for a .db file
     * that doesn't match the indexed project name. */
    if (srv->session_root[0]) {
        char *pname = cbm_project_name_from_path(srv->session_root);
        if (pname) {
            snprintf(srv->session_project, sizeof(srv->session_project), "%s", pname);
            free(pname);
        }
        /* A session started in a subdirectory of an indexed checkout belongs
         * to that project, as the tool calls' cwd resolution and the sweep's
         * exemption already say. The name derived from cwd itself names no
         * database: the watcher and the startup branch check would look for
         * it and find nothing, and with auto_index on the subdirectory would
         * be indexed as a second project. Take the project the walk-up finds
         * and the root it records; with nothing indexed, the cwd stays the
         * session root. Only a project named after its root: the watcher
         * re-indexes by root path, which derives the name, so watching a
         * custom-named index would build a second database under that name. */
        const char *indexed = cwd_project_name();
        if (indexed && indexed[0]) {
            char db_path[CBM_SZ_1K];
            project_db_path(indexed, db_path, sizeof(db_path));
            cbm_store_t *store = db_path[0] ? cbm_store_open_path_query(db_path) : NULL;
            cbm_project_t proj = {0};
            char *derived = NULL;
            if (store && cbm_store_get_project(store, indexed, &proj) == CBM_STORE_OK &&
                proj.root_path && proj.root_path[0] &&
                strlen(proj.root_path) < sizeof(srv->session_root) &&
                (derived = cbm_project_name_from_path(proj.root_path)) != NULL &&
                strcmp(derived, indexed) == 0) {
                snprintf(srv->session_project, sizeof(srv->session_project), "%s", indexed);
                snprintf(srv->session_root, sizeof(srv->session_root), "%s", proj.root_path);
                cbm_log_info("session.root.indexed", "project", indexed, "path", proj.root_path);
            }
            free(derived);
            cbm_project_free_fields(&proj);
            cbm_store_close(store);
        }
    }
}

/* auto_watch config: gates background watcher registration (default on).
 * Multi-project users can contain a session to its own project with
 * `config set auto_watch false`. */
static bool auto_watch_enabled(cbm_mcp_server_t *srv) {
    if (!srv->config) {
        return true; /* default on */
    }
    return cbm_config_get_bool(srv->config, CBM_CONFIG_AUTO_WATCH, true);
}

/* Register the session project with the background watcher for ongoing
 * change detection — unless auto_watch is disabled. */
static void register_watcher_if_enabled(cbm_mcp_server_t *srv, bool index_pending = false) {
    if (!srv->watcher || srv->session_project[0] == '\0' || srv->session_root[0] == '\0') {
        return;
    }
    if (!auto_watch_enabled(srv)) {
        cbm_log_info("watcher.register.skipped", "reason", "auto_watch_off", "project",
                     srv->session_project);
        return;
    }
    cbm_watcher_watch(srv->watcher, srv->session_project, srv->session_root);
    if (index_pending) {
        cbm_watcher_mark_index_pending(srv->watcher, srv->session_project);
    }
}

/* Admission guard for auto_index (#713): true when the root holds at most
 * file_limit indexable files under the full discovery policy. *file_count_out
 * gets the exact count, file_limit + 1 when the limit was passed, or -1 when
 * the root could not be counted (missing, or the count timed out). */
bool cbm_mcp_auto_index_within_file_limit(const char *root_path, int file_limit,
                                          int *file_count_out) {
    if (file_count_out) {
        *file_count_out = -1;
    }
    if (!root_path || !root_path[0] || file_limit < 0) {
        return false;
    }
    enum { AUTO_INDEX_COUNT_TIMEOUT_MS = 5000 };
    cbm_discover_opts_t options = {};
    options.mode = CBM_MODE_FULL;
    int count = -1;
    cbm_discover_status_t status = cbm_discover_count_bounded(
        root_path, &options, file_limit, cbm_now_ms() + AUTO_INDEX_COUNT_TIMEOUT_MS, &count);
    if (file_count_out) {
        *file_count_out = status == CBM_DISCOVER_LIMIT_EXCEEDED
                              ? (file_limit < INT_MAX ? file_limit + 1 : INT_MAX)
                              : count;
    }
    return status == CBM_DISCOVER_OK;
}

/* Background auto-index thread function */
static void *autoindex_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    /* Bounded file count to avoid OOM on massive trees (#713). The old guard
     * counted `git ls-files`, which is 0 outside a checkout, so a plain
     * directory of 60k files was admitted and walked in full (tens of GB RSS).
     * The discovery layer's bounded count applies the SAME filter policy as the
     * index itself, to every root, and stops one file past the limit. It runs
     * here, not in maybe_auto_index, because counting up to the limit can take
     * seconds and `initialize` must not wait for it. */
    int file_limit = srv->autoindex_file_limit;
    int file_count = -1;
    if (!cbm_mcp_auto_index_within_file_limit(srv->session_root, file_limit, &file_count)) {
        char files[CBM_SZ_32];
        char limit[CBM_SZ_32];
        (void)snprintf(files, sizeof(files), "%d", file_count);
        (void)snprintf(limit, sizeof(limit), "%d", file_limit);
        char root_disp[CBM_SZ_1K];
        (void)snprintf(root_disp, sizeof(root_disp), "%s", srv->session_root);
        cbm_normalize_path_sep(root_disp); /* forward-slash paths in diagnostics */
        cbm_log_warn("autoindex.skip", "reason",
                     file_count >= 0 ? "too_many_files" : "unsafe_or_unavailable_path", "files",
                     files, "limit", limit, "root", root_disp);
        return NULL;
    }

    cbm_log_info("autoindex.start", "project", srv->session_project, "path", srv->session_root);

    /* #832: prefer the supervised worker subprocess. Indexing the whole session in
     * this long-lived server thread ratchets RSS (mimalloc v3 does not reclaim the
     * pages worker threads abandon at exit); running it in a child that exits hands
     * 100% of that memory back to the OS every cycle. Use the in-process pipeline
     * below only when the supervisor is explicitly disabled. */
    if (cbm_index_supervisor_should_wrap()) {
        cbm_pipeline_lock();
        char *resp = index_run_supervised_path(srv, srv->session_root);
        cbm_pipeline_unlock();
        bool is_error = true;
        bool valid = cbm_mcp_tool_result_valid(resp, &is_error);
        bool busy = valid && cbm_mcp_result_is_index_busy(resp);
        free(resp);
        if (busy) {
            cbm_log_info("autoindex.deferred", "project", srv->session_project, "reason",
                         "index_busy");
            register_watcher_if_enabled(srv, true);
            return NULL;
        }
        if (valid && !is_error) {
            cbm_log_info("autoindex.done", "project", srv->session_project, "mode", "supervised");
            /* Register with watcher for ongoing change detection — gated on
             * auto_watch (#849), same as the in-process branch below. A bare
             * `if (srv->watcher)` would register even when the user set
             * `config set auto_watch false`, since srv->watcher is always set. */
            register_watcher_if_enabled(srv);
            return NULL;
        }
        cbm_log_warn("autoindex.err", "msg",
                     valid ? "supervised_index_failed" : "supervisor_returned_invalid_response");
        register_watcher_if_enabled(srv);
        return NULL;
    }

    /* Block until any concurrent pipeline finishes. The run records its use
     * and root for cache expiry, as every other index path does. */
    int rc = index_in_process(srv, srv->session_root, true);
    /* Extraction builds a THREAD-LOCAL node-type bitset cache (cbm_kind_in_set).
     * Parallel workers free theirs in pass_parallel.cpp; the in-process auto-index
     * runs extraction on THIS short-lived thread, so it must free its own cache
     * before exiting or LeakSanitizer reports the orphaned bitsets. */
    cbm_kind_in_set_free_cache();
    cbm_mem_collect(); /* return mimalloc pages to OS after indexing (in-process only) */

    if (rc == 0) {
        cbm_log_info("autoindex.done", "project", srv->session_project);
        register_watcher_if_enabled(srv);
    } else if (rc == CBM_INDEX_BUSY) {
        cbm_log_info("autoindex.deferred", "project", srv->session_project, "reason", "index_busy");
        register_watcher_if_enabled(srv, true);
    } else {
        cbm_log_warn("autoindex.err", "msg", "pipeline_run_failed");
    }
    return NULL;
}

/* Start auto-indexing if configured and project not yet indexed. */
/* True when the session's index was built for another checkout than the one
 * there now -- another branch, or the same branch (or a detached HEAD) at
 * another commit: a checkout, pull or commit made while no server was
 * watching. The watcher baselines on the CURRENT HEAD, so it would never see
 * that move; marking the project pending makes its first poll run the index,
 * which swaps in this branch's database (pipeline/branch_slots.h) and passes
 * over the changed files. One SQLite read and a few small file reads (HEAD,
 * the branch ref or packed-refs): initialize stays free of git subprocesses.
 * When HEAD's commit cannot be read from files, only the branch is compared. */
static bool checkout_differs_from_index(const char *root, const char *project, const char *db_path,
                                        std::string *note);

bool cbm_mcp_checkout_differs_from_index(const char *root, const char *project,
                                         const char *db_path) {
    return checkout_differs_from_index(root, project, db_path, nullptr);
}

char *cbm_mcp_checkout_mismatch_note(const char *root, const char *project, const char *db_path) {
    std::string note;
    return checkout_differs_from_index(root, project, db_path, &note) ? strdup(note.c_str()) : NULL;
}

/* "branch `x` at abc1234" / "a detached HEAD at abc1234" for the note. */
static std::string checkout_label(const cbm_branch_id_t *id) {
    std::string s = id->is_detached || !(id->branch || id->slug)
                        ? std::string("a detached HEAD")
                        : "branch `" + std::string(id->branch ? id->branch : id->slug) + "`";
    if (id->has_sha && id->head_sha[0]) {
        s += " at " + std::string(id->head_sha, strnlen(id->head_sha, 7));
    }
    return s;
}

/* note: when set, the comparison is the hook's (no log line) and a
 * difference fills it with one sentence naming both sides. */
static bool checkout_differs_from_index(const char *root, const char *project, const char *db_path,
                                        std::string *note) {
    cbm_branch_id_t current;
    if (!cbm_branch_id_from_head(root, &current)) {
        cbm_branch_id_free(&current);
        return false;
    }
    cbm_branch_id_t indexed = {};
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    bool known = store && cbm_branch_id_indexed(store, project, &indexed);
    cbm_store_close(store);
    /* With HEAD's commit read from files, a recorded identity without one
     * (a legacy node) is not trusted: pending once, and that run's stamp
     * records the commit. Without it (a ref store files cannot read), the
     * lenient comparison, so such a checkout is never marked pending on
     * every session. An index with no usable identity at all (no Branch
     * node; one built while git refused the checkout) is left alone: no run
     * would record one, so marking it would repeat every session. */
    bool other = known && !(current.has_sha ? cbm_branch_id_same_strict(&indexed, &current)
                                            : cbm_branch_id_same(&indexed, &current));
    if (other && note) {
        *note = "Note: this index was built for " + checkout_label(&indexed) +
                ", but the checkout is now " + checkout_label(&current) +
                ". Until the next index run (the MCP server's watcher starts one on its first "
                "poll; or call index_repository), graph answers describe the indexed code, not "
                "the files on disk.";
    } else if (other) {
        cbm_log_info("autoindex.checkout_changed", "project", project, "indexed", indexed.slug,
                     "indexed_head", indexed.head_sha, "checkout", current.slug, "checkout_head",
                     current.head_sha);
    }
    cbm_branch_id_free(&indexed);
    cbm_branch_id_free(&current);
    return other;
}

static bool session_index_on_other_branch(const cbm_mcp_server_t *srv, const char *db_path) {
    return cbm_mcp_checkout_differs_from_index(srv->session_root, srv->session_project, db_path);
}

static void maybe_auto_index(cbm_mcp_server_t *srv) {
    if (srv->session_root[0] == '\0') {
        return; /* no session root detected */
    }

    /* Check if project already has a DB */
    const char *home = cbm_get_home_dir();
    if (home) {
        char db_check[CBM_SZ_1K];
        snprintf(db_check, sizeof(db_check), "%s/%s.db", cbm_resolve_cache_dir(),
                 srv->session_project);
        if (cbm_file_size(db_check) >= 0 &&
            !project_db_owned_by(srv->session_project, srv->session_root, NULL)) {
            /* The derived name belongs to a database indexed from another
             * directory (cbm_root_matches). Watching or auto-indexing this
             * root under that name would overwrite the other checkout's
             * index, so do neither; an explicit index_repository with a
             * name override is the way out. */
            cbm_log_warn("autoindex.skip", "reason", "project_name_collision", "project",
                         srv->session_project, "path", srv->session_root);
            return;
        }
        if (cbm_file_size(db_check) >= 0) {
            /* Already indexed → register watcher for change detection */
            cbm_log_info("autoindex.skip", "reason", "already_indexed", "project",
                         srv->session_project);
            register_watcher_if_enabled(srv, session_index_on_other_branch(srv, db_check));
            return;
        }
    }

/* Default file limit for auto-indexing new projects */
#define DEFAULT_AUTO_INDEX_LIMIT 50000

    /* Check auto_index config */
    bool auto_index = false;
    int file_limit = DEFAULT_AUTO_INDEX_LIMIT;
    if (srv->config) {
        auto_index = cbm_config_get_bool(srv->config, CBM_CONFIG_AUTO_INDEX, false);
        file_limit =
            cbm_config_get_int(srv->config, CBM_CONFIG_AUTO_INDEX_LIMIT, DEFAULT_AUTO_INDEX_LIMIT);
    }

    if (!auto_index) {
        cbm_log_info("autoindex.skip", "reason", "disabled", "hint",
                     "run: code-cortex-mcp config set auto_index true");
        return;
    }

    /* Launch auto-index in background; the #713 file-limit admission check runs
     * first thing on that thread, so `initialize` answers without waiting. */
    srv->autoindex_file_limit = file_limit;
    if (cbm_thread_create(&srv->autoindex_tid, 0, autoindex_thread, srv) == 0) {
        srv->autoindex_active = true;
    }
}

/* ── Background update check ──────────────────────────────────── */

#define UPDATE_CHECK_URL "https://api.github.com/repos/tigercosmos/code-cortex-mcp/releases/latest"

static void *update_check_thread(void *arg) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)arg;

    /* Use curl with 5s timeout to fetch latest release tag */
    FILE *fp = cbm_popen("curl -sf --max-time 5 -H 'Accept: application/vnd.github+json' "
                         "'" UPDATE_CHECK_URL "' 2>/dev/null",
                         "r");
    if (!fp) {
        srv->update_checked = true;
        return NULL;
    }

    char buf[CBM_SZ_4K];
    size_t total = 0;
    while (total < sizeof(buf) - SKIP_ONE) {
        size_t n = fread(buf + total, SKIP_ONE, sizeof(buf) - SKIP_ONE - total, fp);
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';
    cbm_pclose(fp);

    /* Parse tag_name from JSON response */
    yyjson_doc *doc = yyjson_read(buf, total, 0);
    if (!doc) {
        srv->update_checked = true;
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tag = yyjson_obj_get(root, "tag_name");
    const char *tag_str = yyjson_get_str(tag);

    if (tag_str) {
        const char *current = cbm_cli_get_version();
        if (cbm_compare_versions(tag_str, current) > 0) {
            snprintf(srv->update_notice, sizeof(srv->update_notice),
                     "Update available: %s -> %s -- run: code-cortex-mcp update  |  "
                     "Enjoying code-cortex-mcp? Please leave a star: "
                     "https://github.com/tigercosmos/code-cortex-mcp",
                     current, tag_str);
            cbm_log_info("update.available", "current", current, "latest", tag_str);
        }
    }

    yyjson_doc_free(doc);
    srv->update_checked = true;
    return NULL;
}

bool cbm_mcp_update_check_enabled(void) {
    const char *value = getenv("CBM_UPDATE_CHECK");
    return !value || strcmp(value, "0") != 0;
}

/* ── Cache expiry ───────────────────────────────────────────────── */

#ifdef CBM_ENABLE_TEST_SEAMS
static void (*sweep_test_cb)(int, void *) = nullptr;
static void *sweep_test_data = nullptr;

void cbm_mcp_test_sweep_hook(void (*callback)(int stage, void *data), void *data) {
    sweep_test_cb = callback;
    sweep_test_data = data;
}
#define SWEEP_TEST_HOOK(stage)                       \
    do {                                             \
        if (sweep_test_cb) {                         \
            sweep_test_cb((stage), sweep_test_data); \
        }                                            \
    } while (0)
#else
#define SWEEP_TEST_HOOK(stage) ((void)0)
#endif

static bool sweep_keep_watched(const char *project, void *ud) {
    cbm_mcp_server_t *srv = (cbm_mcp_server_t *)ud;
    return srv->watcher && cbm_watcher_is_watched(srv->watcher, project);
}

/* The sweep thread's view of the session: the project name is copied on the
 * main thread before the sweep starts, because initialize may rewrite
 * srv->session_project while a sweep is running. */
struct sweep_job {
    cbm_mcp_server_t *srv;
    char keep_project[CBM_SZ_256];
};

static void *sweep_thread(void *arg) {
    sweep_job *job = (sweep_job *)arg;
    cbm_mcp_server_t *srv = job->srv;
    /* An exception leaving a thread function terminates the server: the
     * sweep is housekeeping, so a failure in it (bad_alloc in the scan) is
     * logged and dropped. Either way the flag is cleared and the job freed,
     * so a later sweep can start. */
    try {
        SWEEP_TEST_HOOK(0);
        cbm_cache_expiry_cfg_t cfg;
        cbm_cache_expiry_cfg_load(&cfg, NULL);
        cbm_cache_sweep_opts_t opts = {};
        opts.keep_project = job->keep_project[0] ? job->keep_project : NULL;
        opts.keep = sweep_keep_watched;
        opts.keep_ud = srv;
        cbm_cache_sweep_result_t res;
        (void)cbm_cache_expiry_sweep(&cfg, &opts, &res);
        SWEEP_TEST_HOOK(1);
    } catch (const std::exception &e) {
        cbm_log_warn("cache.expire.error", "msg", e.what());
    } catch (...) {
        cbm_log_warn("cache.expire.error", "msg", "unknown exception");
    }
    srv->sweep_running.store(false);
    free(job);
    return NULL;
}

/* Start a background sweep when one is due (at most once a day per cache,
 * recorded in _config.db). Called at initialize and after an index run; the
 * due check is one read of _config.db. */
static void maybe_sweep_cache(cbm_mcp_server_t *srv) {
    cbm_cache_expiry_cfg_t cfg;
    cbm_cache_expiry_cfg_load(&cfg, srv->config);
    if (!cbm_cache_expiry_due(&cfg, (int64_t)time(NULL))) {
        return;
    }
    if (srv->sweep_running.load()) {
        return; /* the earlier sweep records last_sweep when it ends */
    }
    join_sweep(srv); /* finished: reap it */
    sweep_job *job = (sweep_job *)calloc(1, sizeof(*job));
    if (!job) {
        return;
    }
    job->srv = srv;
    /* The project to exempt is the one the session's tool calls resolve to:
     * the indexed ancestor the cwd walk-up finds (a server started in a
     * subdirectory, or on a custom-named index), not the name derived from
     * cwd itself, which names no database and would exempt nothing while the
     * live index went to trash. */
    const char *keep = cwd_project_name();
    snprintf(job->keep_project, sizeof(job->keep_project), "%s",
             keep && keep[0] ? keep : srv->session_project);
    srv->sweep_running.store(true);
    if (cbm_thread_create(&srv->sweep_tid, 0, sweep_thread, job) == 0) {
        srv->sweep_active = true;
    } else {
        srv->sweep_running.store(false);
        free(job);
    }
}

static void start_update_check(cbm_mcp_server_t *srv) {
    if (srv->update_checked || !cbm_mcp_update_check_enabled()) {
        return;
    }
    srv->update_checked = true; /* prevent double-launch */
    if (cbm_thread_create(&srv->update_tid, 0, update_check_thread, srv) == 0) {
        srv->update_thread_active = true;
    }
}

/* Prepend update notice to a tool result, then clear it (one-shot). */
static char *inject_update_notice(cbm_mcp_server_t *srv, char *result_json) {
    if (srv->update_notice[0] == '\0') {
        return result_json;
    }

    /* Parse existing result, prepend notice text, rebuild */
    yyjson_doc *doc = yyjson_read(result_json, strlen(result_json), 0);
    if (!doc) {
        return result_json;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return result_json;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Find the "content" array */
    yyjson_mut_val *content = yyjson_mut_obj_get(root, "content");
    if (content && yyjson_mut_is_arr(content)) {
        /* Prepend a text content item with the update notice */
        yyjson_mut_val *notice_item = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_str(mdoc, notice_item, "type", "text");
        yyjson_mut_obj_add_str(mdoc, notice_item, "text", srv->update_notice);
        yyjson_mut_arr_prepend(content, notice_item);
    }

    size_t len;
    char *new_json = yyjson_mut_write(mdoc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    yyjson_mut_doc_free(mdoc);

    if (new_json) {
        free(result_json);
        srv->update_notice[0] = '\0'; /* clear — one-shot */
        return new_json;
    }
    return result_json;
}

/* ── Server request handler ───────────────────────────────────── */

char *cbm_mcp_server_handle(cbm_mcp_server_t *srv, const char *line) {
    cbm_jsonrpc_request_t req = {0};
    if (cbm_jsonrpc_parse(line, &req) < 0) {
        return cbm_jsonrpc_format_error(0, JSONRPC_PARSE_ERROR, "Parse error");
    }

    /* Notifications (no id) → handle cancellation, then no response */
    if (!req.has_id) {
        if (req.method && strcmp(req.method, "notifications/cancelled") == 0) {
            if (srv->active_pipeline &&
                cbm_mcp_cancel_request_matches(req.params_raw, srv->active_request_id,
                                               srv->active_request_id_str)) {
                cbm_pipeline_cancel(srv->active_pipeline);
                cbm_log_info("mcp.cancelled", "match", "true");
            }
        }
        cbm_jsonrpc_request_free(&req);
        return NULL;
    }

    struct timespec req_t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &req_t0);
    char *result_json = NULL;
    bool request_logged = false;

    if (strcmp(req.method, "initialize") == 0) {
        result_json = cbm_mcp_initialize_response(req.params_raw);
        start_update_check(srv);
        detect_session(srv);
        maybe_auto_index(srv);
        maybe_sweep_cache(srv);
    } else if (strcmp(req.method, "ping") == 0) {
        result_json = heap_strdup("{}");
    } else if (strcmp(req.method, "tools/list") == 0) {
        result_json = cbm_mcp_tools_list_page(req.params_raw);
    } else if (strcmp(req.method, "tools/call") == 0) {
        char *tool_name = req.params_raw ? cbm_mcp_get_tool_name(req.params_raw) : NULL;
        char *tool_args =
            req.params_raw ? cbm_mcp_get_arguments(req.params_raw) : heap_strdup("{}");
        srv->active_request_id = req.id;
        free(srv->active_request_id_str);
        srv->active_request_id_str = req.id_str ? heap_strdup(req.id_str) : NULL;

        struct timespec t0;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t0);
        bool wrap_tool = mcp_tool_known(tool_name) && strcmp(tool_name, "index_repository") != 0 &&
                         cbm_tool_supervisor_should_wrap();
        /* get_project_arg records a path argument's directory as the
         * expected root (normalize_project_arg); this lookup is not a tool
         * call, so put the expectation back, or it would scope every later
         * call in this process to that directory. */
        char *delete_project = NULL;
        if (wrap_tool && strcmp(tool_name, "delete_project") == 0) {
            char saved_root[sizeof(g_expected_root)];
            memcpy(saved_root, g_expected_root, sizeof(saved_root));
            delete_project = get_project_arg(tool_args);
            memcpy(g_expected_root, saved_root, sizeof(saved_root));
        }
        bool delete_locked = false;
        if (delete_project) {
            delete_locked = cbm_pipeline_try_lock();
            if (!delete_locked) {
                result_json = cbm_mcp_text_result(
                    "delete_project cannot run while indexing is active; retry after indexing "
                    "finishes",
                    true);
            } else if (srv->current_project && strcmp(srv->current_project, delete_project) == 0) {
                if (srv->owns_store && srv->store) {
                    cbm_store_close(srv->store);
                }
                srv->store = NULL;
                free(srv->current_project);
                srv->current_project = NULL;
                srv->store_last_used = 0;
            }
        }
        if (wrap_tool) {
            cbm_index_worker_result_t worker = {};
            /* Persistent worker when available (tool_server.h); the one-shot
             * worker per call remains the fallback (Windows, CBM_TOOL_SERVER=0). */
            int spawn_rc = result_json ? -1
                           : cbm_tool_server_enabled()
                               ? cbm_tool_server_call(tool_name, tool_args, &worker)
                               : cbm_tool_spawn_worker(tool_name, tool_args, &worker);
            bool response_error = true;
            bool usable_outcome =
                worker.outcome == CBM_PROC_CLEAN || worker.outcome == CBM_PROC_EXIT_NONZERO;
            bool usable_response = spawn_rc == 0 && usable_outcome &&
                                   cbm_mcp_tool_result_valid(worker.response, &response_error);
            if (usable_response) {
                result_json = worker.response;
                worker.response = NULL;
            } else if (spawn_rc == 0 && worker.outcome == CBM_PROC_HANG) {
                char msg[CBM_SZ_256];
                snprintf(msg, sizeof(msg),
                         "{\"error\":\"tool call exceeded the server deadline\","
                         "\"timeout_ms\":%d}",
                         cbm_tool_timeout_ms(tool_name));
                result_json = cbm_mcp_text_result(msg, true);
            } else if (!result_json) {
                char msg[CBM_SZ_256];
                snprintf(msg, sizeof(msg),
                         "{\"error\":\"supervised tool worker failed\","
                         "\"outcome\":\"%s\","
                         "\"hint\":\"Check the cache directory and process limits. "
                         "CBM_TOOL_SUPERVISOR=0 disables isolation for emergency diagnosis, "
                         "but removes the hard tool deadline.\"}",
                         spawn_rc == 0 ? cbm_proc_outcome_str(worker.outcome) : "spawn_failed");
                result_json = cbm_mcp_text_result(msg, true);
            }
            if (delete_locked) {
                char delete_path[CBM_SZ_1K];
                project_db_path(delete_project, delete_path, sizeof(delete_path));
                bool delete_gone = delete_path[0] && !cbm_file_exists(delete_path);
                /* A worker that kept the database because another checkout
                 * owns it (a project-name collision: "kept_database_of")
                 * leaves that checkout's watch in place, as the in-process
                 * handler does; unwatching by name would stop its
                 * re-indexing. */
                bool kept_foreign = result_json && strstr(result_json, "kept_database_of");
                if (((usable_response && !response_error && !kept_foreign) || delete_gone) &&
                    srv->watcher) {
                    cbm_watcher_unwatch(srv->watcher, delete_project);
                }
                cbm_pipeline_unlock();
                cbm_mem_collect();
            }
            cbm_index_worker_result_free(&worker);
        } else {
            result_json = cbm_mcp_handle_tool(srv, tool_name, tool_args);
        }
        free(delete_project);
        srv->active_request_id = CBM_NOT_FOUND;
        free(srv->active_request_id_str);
        srv->active_request_id_str = NULL;
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - t0.tv_nsec) / MCP_MS_TO_US);
        bool is_err = (result_json != NULL) && (strstr(result_json, "\"isError\":true") != NULL);
        cbm_diag_record_query(dur_us, is_err);
        long long request_dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                                   ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, tool_name, is_err, request_dur_us);
        request_logged = true;

        result_json = inject_update_notice(srv, result_json);
        if (tool_name && strcmp(tool_name, "index_repository") == 0) {
            maybe_sweep_cache(srv);
        }
        free(tool_name);
        free(tool_args);
    } else {
        /* Echo the original id (string or numeric, issue #253) on the error. */
        char err_obj[160];
        snprintf(err_obj, sizeof(err_obj), "{\"code\":%d,\"message\":\"Method not found\"}",
                 JSONRPC_METHOD_NOT_FOUND);
        cbm_jsonrpc_response_t err_resp = {
            .id = req.id,
            .id_str = req.id_str,
            .error_json = err_obj,
        };
        char *err = cbm_jsonrpc_format_response(&err_resp);
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, NULL, true, dur_us);
        cbm_jsonrpc_request_free(&req);
        return err;
    }

    if (!request_logged) {
        struct timespec t1;
        cbm_clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dur_us = ((long long)(t1.tv_sec - req_t0.tv_sec) * MCP_S_TO_US) +
                           ((long long)(t1.tv_nsec - req_t0.tv_nsec) / MCP_MS_TO_US);
        cbm_log_mcp_request(req.method, NULL, false, dur_us);
    }

    cbm_jsonrpc_response_t resp = {
        .id = req.id,
        .id_str = req.id_str,
        .result_json = result_json,
    };
    char *out = cbm_jsonrpc_format_response(&resp);
    free(result_json);
    cbm_jsonrpc_request_free(&req);
    return out;
}

/* Handle a Content-Length-framed message (LSP-style transport).
 * Reads headers, body, processes request, writes framed response. */
static void handle_content_length_frame(cbm_mcp_server_t *srv, FILE *in, FILE *out, char **line,
                                        size_t *cap, int content_len) {
    /* Skip blank line(s) between header and body */
    while (cbm_getline(line, cap, in) > 0) {
        size_t hlen = strlen(*line);
        while (hlen > 0 && ((*line)[hlen - SKIP_ONE] == '\n' || (*line)[hlen - SKIP_ONE] == '\r')) {
            (*line)[--hlen] = '\0';
        }
        if (hlen == 0) {
            break;
        }
    }

    char *body = (char *)malloc((size_t)content_len + SKIP_ONE);
    if (!body) {
        return;
    }
    size_t nread = fread(body, SKIP_ONE, (size_t)content_len, in);
    body[nread] = '\0';

    char *resp = cbm_mcp_server_handle(srv, body);
    free(body);

    if (resp) {
        size_t rlen = strlen(resp);
        (void)fprintf(out, "Content-Length: %zu\r\n\r\n%s", rlen, resp);
        (void)fflush(out);
        free(resp);
    }
}

#ifndef _WIN32
/* Unix 3-phase poll: non-blocking fd check, FILE* buffer peek, blocking poll.
 * Returns: 1 = data ready, 0 = timeout (evicted idle stores), -1 = error/EOF. */
static int poll_for_input_unix(cbm_mcp_server_t *srv, int fd, FILE *in) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, SKIP_ONE, 0); /* Phase 1: non-blocking */

    if (pr < 0) {
        return CBM_NOT_FOUND;
    }
    if (pr > 0) {
        return SKIP_ONE;
    }

    /* Phase 2: peek FILE* buffer */
    int saved_flags = fcntl(fd, F_GETFL);
    if (saved_flags < 0) {
        /* fcntl failed — fall through to a short blocking poll (see the Phase-3
         * note below on why the interval is bounded, not the full idle timeout) */
        pr = poll(&pfd, SKIP_ONE, MCP_TIMEOUT_MS);
        if (pr < 0) {
            return CBM_NOT_FOUND;
        }
        if (pr == 0) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            return 0;
        }
        return SKIP_ONE;
    }

    (void)fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK);
    int c = fgetc(in);
    (void)fcntl(fd, F_SETFL, saved_flags);

    if (c == EOF) {
        if (feof(in)) {
            return CBM_NOT_FOUND; /* true EOF */
        }
        clearerr(in);
        /* Phase 3: blocking poll, bounded to a SHORT interval (not the full idle
         * timeout). macOS poll()/select() do NOT report POLLIN/POLLHUP when a
         * FIFO's last writer closes — only read() returns 0 there (verified). A
         * 60s poll would therefore leave the server blocked up to a full idle
         * timeout after stdin EOF (a client that closes the pipe would appear to
         * hang). Waking every MCP_TIMEOUT_MS lets the Phase-2 read() above detect
         * the EOF within ~1s. Idle-store eviction (threshold STORE_IDLE_TIMEOUT_S)
         * is idempotent, so checking it on each short tick is harmless. */
        pr = poll(&pfd, SKIP_ONE, MCP_TIMEOUT_MS);
        if (pr < 0) {
            return CBM_NOT_FOUND;
        }
        if (pr == 0) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            return 0;
        }
        return SKIP_ONE;
    }

    (void)ungetc(c, in);
    return SKIP_ONE;
}
#endif

/* ── Event loop ───────────────────────────────────────────────── */

int cbm_mcp_server_run(cbm_mcp_server_t *srv, FILE *in, FILE *out) {
    char *line = NULL;
    size_t cap = 0;
    int fd = cbm_fileno(in);

#ifdef _WIN32
    /* Ensure stdio is in binary mode to prevent CRLF translation from corrupting
     * Content-Length byte counts and causing fread() to hang. (<io.h> and
     * <fcntl.h> are already included unconditionally on the _WIN32 branch of
     * this file, so _setmode/_O_BINARY do not depend on transitive inclusion
     * through <windows.h> the way upstream's version does.) */
    _setmode(cbm_fileno(in), _O_BINARY);
    _setmode(cbm_fileno(out), _O_BINARY);
#endif

    for (;;) {
        /* Poll with idle timeout so we can evict unused stores between requests.
         *
         * IMPORTANT: poll() operates on the raw fd, but getline() reads from a
         * buffered FILE*. When a client sends multiple messages in rapid
         * succession, the first getline() call may drain ALL kernel data into
         * libc's internal FILE* buffer. Subsequent poll() calls then see an
         * empty kernel fd and block for STORE_IDLE_TIMEOUT_S seconds even
         * though the next messages are already in the FILE* buffer.
         *
         * Fix (Unix): use a three-phase approach —
         *   Phase 1: non-blocking poll (timeout=0) to check the kernel fd.
         *   Phase 2: if Phase 1 returns 0, peek the FILE* buffer via fgetc/
         *            ungetc to detect data buffered by a prior getline() call.
         *            The fd is temporarily set O_NONBLOCK so fgetc() returns
         *            immediately (EAGAIN → EOF + ferror) instead of blocking
         *            when the FILE* buffer is empty, which would otherwise
         *            bypass the Phase 3 idle eviction timeout.
         *   Phase 3: only if both phases confirm no data, do blocking poll. */
#ifdef _WIN32
        /* Windows: WaitForSingleObject on stdin handle */
        HANDLE hStdin = (HANDLE)_get_osfhandle(fd);
        DWORD wr = WaitForSingleObject(hStdin, STORE_IDLE_TIMEOUT_S * MCP_TIMEOUT_MS);
        if (wr == WAIT_FAILED) {
            break;
        }
        if (wr == WAIT_TIMEOUT) {
            cbm_mcp_server_evict_idle(srv, STORE_IDLE_TIMEOUT_S);
            continue;
        }
#else
        int pr = poll_for_input_unix(srv, fd, in);
        if (pr < 0) {
            break;
        }
        if (pr == 0) {
            continue; /* timeout — idle stores evicted */
        }
#endif

        if (cbm_getline(&line, &cap, in) <= 0) {
            break;
        }

        /* Trim trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len - SKIP_ONE] == '\n' || line[len - SKIP_ONE] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        /* Content-Length framing (LSP-style transport) */
        if (strncmp(line, "Content-Length:", SLEN("Content-Length:")) == 0) {
            int content_len = (int)strtol(line + MCP_CONTENT_PREFIX, NULL, CBM_DECIMAL_BASE);
            if (content_len > 0 && content_len <= MCP_DEFAULT_LIMIT * CBM_SZ_1K * CBM_SZ_1K) {
                handle_content_length_frame(srv, in, out, &line, &cap, content_len);
            }
            continue;
        }

        char *resp = cbm_mcp_server_handle(srv, line);
        if (resp) {
            (void)fprintf(out, "%s\n", resp);
            (void)fflush(out);
            free(resp);
        }
    }

    free(line);
    return 0;
}

/* ── cbm_parse_file_uri ──────────────────────────────────────── */

bool cbm_parse_file_uri(const char *uri, char *out_path, int out_size) {
    if (!uri || !out_path || out_size <= 0) {
        if (out_path && out_size > 0) {
            out_path[0] = '\0';
        }
        return false;
    }

    /* Must start with file:// */
    if (strncmp(uri, "file://", SLEN("file://")) != 0) {
        out_path[0] = '\0';
        return false;
    }

    const char *path = uri + MCP_URI_PREFIX;

    /* On Windows, file:///C:/path → /C:/path. Strip leading / before drive letter. */
    if (path[0] == '/' && path[SKIP_ONE] &&
        ((path[SKIP_ONE] >= 'A' && path[SKIP_ONE] <= 'Z') ||
         (path[SKIP_ONE] >= 'a' && path[SKIP_ONE] <= 'z')) &&
        path[PAIR_LEN] == ':') {
        path++; /* skip the leading / */
    }

    snprintf(out_path, out_size, "%s", path);
    return true;
}

/* ── Hook helpers: edit impact note ──────────────────────────────── */

/* PostToolUse(Edit|Write) context: what the graph knows about the file the
 * agent just changed. Callers of every symbol the file defines, grouped by
 * file, with tests counted separately — the answer grep cannot give without
 * one search per symbol, delivered at the moment the agent needs it and at
 * zero adoption cost. *resolved reports whether the project resolved at all
 * (false → the caller may climb to a parent directory). Returns malloc'd
 * text, or NULL when the file defines nothing the graph tracks. */
char *cbm_mcp_edit_impact_note(cbm_mcp_server_t *srv, const char *project, const char *rel_path,
                               bool *resolved) {
    enum {
        IMPACT_MAX_SYMBOLS = 40,
        IMPACT_MAX_CALLERS = 400,
        IMPACT_MAX_FILES = 64,
        IMPACT_LIST_FILES = 6,
        IMPACT_LIST_SYMBOLS = 8,
        IMPACT_TEXT_SZ = 2048,
    };
    if (resolved) {
        *resolved = false;
    }
    if (!srv || !project || !rel_path) {
        return NULL;
    }
    cbm_store_t *store = resolve_store(srv, project);
    if (!store) {
        return NULL;
    }
    if (resolved) {
        *resolved = true;
    }
    cbm_node_t *nodes = NULL;
    int node_count = 0;
    cbm_store_find_nodes_by_file(store, project, rel_path, &nodes, &node_count);
    if (node_count == 0) {
        cbm_store_free_nodes(nodes, 0);
        return NULL;
    }

    struct {
        const char *name;
        int callers;
    } syms[IMPACT_MAX_SYMBOLS];
    int sym_count = 0;
    int64_t seen[IMPACT_MAX_CALLERS];
    int seen_count = 0;
    const char *files[IMPACT_MAX_FILES];
    int file_counts[IMPACT_MAX_FILES];
    int file_count = 0;
    int callers_total = 0;
    int test_callers = 0;
    bool overflow = false;
    /* File names are borrowed from looked-up caller nodes; keep them alive. */
    cbm_node_t keep[IMPACT_MAX_FILES];
    int keep_count = 0;

    for (int i = 0; i < node_count && sym_count < IMPACT_MAX_SYMBOLS; i++) {
        const char *label = nodes[i].label ? nodes[i].label : "";
        if (strcmp(label, "Function") != 0 && strcmp(label, "Method") != 0 &&
            strcmp(label, "Class") != 0 && strcmp(label, "Struct") != 0) {
            continue;
        }
        syms[sym_count].name = nodes[i].name ? nodes[i].name : "?";
        syms[sym_count].callers = 0;
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        cbm_store_find_edges_by_target_type(store, nodes[i].id, "CALLS", &edges, &edge_count);
        /* Per-symbol count: distinct external callers of THIS symbol. The
         * global `seen` set below only deduplicates the file rollup, so a
         * caller of two symbols in the file counts once for each. */
        int64_t sym_seen[IMPACT_MAX_CALLERS];
        int sym_seen_count = 0;
        for (int e = 0; e < edge_count; e++) {
            int64_t src_id = edges[e].source_id;
            /* A caller inside the edited file itself is not impact. */
            bool internal = false;
            for (int k = 0; k < node_count; k++) {
                if (nodes[k].id == src_id) {
                    internal = true;
                    break;
                }
            }
            if (internal) {
                continue;
            }
            bool sym_dup = false;
            for (int k = 0; k < sym_seen_count; k++) {
                if (sym_seen[k] == src_id) {
                    sym_dup = true;
                    break;
                }
            }
            if (!sym_dup && sym_seen_count < IMPACT_MAX_CALLERS) {
                sym_seen[sym_seen_count++] = src_id;
                syms[sym_count].callers++;
            }
            bool dup = false;
            for (int k = 0; k < seen_count; k++) {
                if (seen[k] == src_id) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (seen_count == IMPACT_MAX_CALLERS) {
                overflow = true;
                break;
            }
            seen[seen_count++] = src_id;
            cbm_node_t caller = {0};
            if (cbm_store_find_node_by_id(store, src_id, &caller) != CBM_STORE_OK) {
                continue;
            }
            callers_total++;
            const char *fp = caller.file_path ? caller.file_path : "";
            bool test = is_test_file(fp);
            if (test) {
                test_callers++;
            }
            int f = 0;
            for (; f < file_count; f++) {
                if (strcmp(files[f], fp) == 0) {
                    break;
                }
            }
            if (f == file_count && file_count < IMPACT_MAX_FILES && keep_count < IMPACT_MAX_FILES) {
                keep[keep_count] = caller; /* ownership moves */
                files[file_count] = keep[keep_count].file_path ? keep[keep_count].file_path : "";
                file_counts[file_count] = 0;
                file_count++;
                keep_count++;
                file_counts[f]++;
                continue;
            }
            if (f < file_count) {
                file_counts[f]++;
            }
            free_node_contents(&caller);
        }
        cbm_store_free_edges(edges, edge_count);
        sym_count++;
    }

    char *text = NULL;
    if (sym_count > 0) {
        text = (char *)malloc(IMPACT_TEXT_SZ);
    }
    if (text) {
        int off = snprintf(text, IMPACT_TEXT_SZ,
                           "[code-cortex] %s defines %d graph symbol(s). Direct callers outside "
                           "this file: %d%s in %d file(s)",
                           rel_path, sym_count, callers_total, overflow ? "+" : "", file_count);
        if (test_callers > 0 && off > 0 && off < IMPACT_TEXT_SZ) {
            off +=
                snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), ", %d in tests", test_callers);
        }
        if (file_count > 0 && off > 0 && off < IMPACT_TEXT_SZ) {
            off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), ": ");
            for (int f = 0;
                 f < file_count && f < IMPACT_LIST_FILES && off > 0 && off < IMPACT_TEXT_SZ; f++) {
                off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), "%s%s (%d)",
                                f ? ", " : "", files[f], file_counts[f]);
            }
            if (file_count > IMPACT_LIST_FILES && off > 0 && off < IMPACT_TEXT_SZ) {
                off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), ", +%d more",
                                file_count - IMPACT_LIST_FILES);
            }
        }
        if (off > 0 && off < IMPACT_TEXT_SZ) {
            off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), ". Per symbol: ");
            int listed = 0;
            for (int s = 0;
                 s < sym_count && listed < IMPACT_LIST_SYMBOLS && off > 0 && off < IMPACT_TEXT_SZ;
                 s++) {
                off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), "%s%s <- %d",
                                listed ? ", " : "", syms[s].name, syms[s].callers);
                listed++;
            }
            if (sym_count > IMPACT_LIST_SYMBOLS && off > 0 && off < IMPACT_TEXT_SZ) {
                off += snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off), ", +%d more",
                                sym_count - IMPACT_LIST_SYMBOLS);
            }
        }
        if (off > 0 && off < IMPACT_TEXT_SZ) {
            snprintf(text + off, (size_t)(IMPACT_TEXT_SZ - off),
                     ". (Per the last index; inspect_symbol(<name>) gives call-site lines, "
                     "detect_changes the transitive set.)");
        }
    }
    for (int k = 0; k < keep_count; k++) {
        free_node_contents(&keep[k]);
    }
    cbm_store_free_nodes(nodes, node_count);
    return text;
}
