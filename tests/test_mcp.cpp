/*
 * test_mcp.c — Tests for the MCP server module.
 *
 * Covers: JSON-RPC parsing, MCP protocol, tool dispatch, tool handlers.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h" /* cbm_unlink / cbm_rmdir */
#include "../src/foundation/log.h"     /* cbm_log_set_sink — routing capture */
#include "test_framework.h"
#include "test_helpers.h" /* th_write_file / th_rmtree / th_mktempdir */
#include <mcp/mcp.h>
#include <mcp/mcp_internal.h> /* cbm_detect_node_in_hunks (#1363) */
#include <mcp/index_supervisor.h>
#include <mcp/store_meta.h> /* custom-name cwd resolution test */
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <yyjson/yyjson.h>
#include <initializer_list>
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

TEST(mcp_tool_result_validation_rejects_partial_response) {
    bool is_error = true;
    ASSERT_TRUE(cbm_mcp_tool_result_valid("{\"content\":[],\"isError\":false}", &is_error));
    ASSERT_FALSE(is_error);
    ASSERT_FALSE(cbm_mcp_tool_result_valid("{\"content\":[", &is_error));
    ASSERT_FALSE(cbm_mcp_tool_result_valid("{\"content\":[],\"isError\":\"false\"}", &is_error));
    PASS();
}

TEST(mcp_tool_deadlines_are_bounded_and_tool_specific) {
    const char *saved = getenv("CBM_TOOL_TIMEOUT_S");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_unsetenv("CBM_TOOL_TIMEOUT_S");
    ASSERT_EQ(cbm_tool_timeout_ms("get_graph_schema"), 30000);
    ASSERT_EQ(cbm_tool_timeout_ms("list_projects"), 90000);
    ASSERT_EQ(cbm_tool_timeout_ms("index_status"), 90000);
    ASSERT_EQ(cbm_tool_timeout_ms("search_graph"), 90000);
    ASSERT_EQ(cbm_tool_timeout_ms("query_graph"), 90000);
    ASSERT_EQ(cbm_tool_timeout_ms("get_architecture"), 90000);
    cbm_setenv("CBM_TOOL_TIMEOUT_S", "7", 1);
    ASSERT_EQ(cbm_tool_timeout_ms("search_graph"), 7000);
    ASSERT_EQ(cbm_tool_timeout_ms("query_graph"), 7000);
    if (saved_copy) {
        cbm_setenv("CBM_TOOL_TIMEOUT_S", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_TOOL_TIMEOUT_S");
    }
    PASS();
}

TEST(mcp_tool_worker_name_rejects_option_injection) {
    ASSERT_TRUE(cbm_tool_worker_name_safe("search_graph"));
    ASSERT_FALSE(cbm_tool_worker_name_safe("--response-out"));
    ASSERT_FALSE(cbm_tool_worker_name_safe("search-graph"));
    ASSERT_FALSE(cbm_tool_worker_name_safe(""));
    ASSERT_FALSE(cbm_tool_worker_name_safe(NULL));
    PASS();
}

TEST(mcp_update_check_can_be_disabled) {
    const char *saved = getenv("CBM_UPDATE_CHECK");
    char *saved_copy = saved ? strdup(saved) : NULL;

    cbm_unsetenv("CBM_UPDATE_CHECK");
    ASSERT_TRUE(cbm_mcp_update_check_enabled());
    cbm_setenv("CBM_UPDATE_CHECK", "0", 1);
    ASSERT_FALSE(cbm_mcp_update_check_enabled());
    cbm_setenv("CBM_UPDATE_CHECK", "1", 1);
    ASSERT_TRUE(cbm_mcp_update_check_enabled());

    if (saved_copy) {
        cbm_setenv("CBM_UPDATE_CHECK", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_UPDATE_CHECK");
    }
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_request) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"capabilities\":{}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_EQ(req.id, 1);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_notification) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "notifications/initialized");
    ASSERT_FALSE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_invalid) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("not json", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_tools_call) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"search_graph\","
                       "\"arguments\":{\"label\":\"Function\",\"limit\":5}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "tools/call");
    ASSERT_EQ(req.id, 42);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* issue #253: JSON-RPC 2.0 §4 permits string ids (Claude Desktop sends them
 * for "initialize"). Previously strtol-coerced to 0; must be preserved. */
TEST(jsonrpc_parse_string_id_issue253) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"init-abc\",\"method\":\"initialize\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "init-abc");
    cbm_jsonrpc_request_free(&req);

    /* A purely non-numeric string would have become 0 under strtol. */
    const char *line2 = "{\"jsonrpc\":\"2.0\",\"id\":\"xyz\",\"method\":\"ping\"}";
    cbm_jsonrpc_request_t req2 = {0};
    ASSERT_EQ(cbm_jsonrpc_parse(line2, &req2), 0);
    ASSERT_NOT_NULL(req2.id_str);
    ASSERT_STR_EQ(req2.id_str, "xyz");
    cbm_jsonrpc_request_free(&req2);
    PASS();
}

/* issue #253: the response must echo the string id verbatim, not as a number. */
TEST(jsonrpc_format_response_string_id_issue253) {
    cbm_jsonrpc_response_t resp = {
        .id_str = "init-abc",
        .result_json = "{\"ok\":true}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":\"init-abc\""));
    /* Must NOT have coerced to a numeric id. */
    ASSERT_NULL(strstr(json, "\"id\":0"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_format_response) {
    cbm_jsonrpc_response_t resp = {
        .id = 1,
        .result_json = "{\"name\":\"code-cortex-mcp\"}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    /* Should contain jsonrpc, id, and result */
    ASSERT_NOT_NULL(strstr(json, "\"jsonrpc\":\"2.0\""));
    ASSERT_NOT_NULL(strstr(json, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(json, "\"result\""));
    free(json);
    PASS();
}

TEST(jsonrpc_format_error) {
    char *json = cbm_jsonrpc_format_error(5, -32600, "Invalid Request");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":5"));
    ASSERT_NOT_NULL(strstr(json, "\"error\""));
    ASSERT_NOT_NULL(strstr(json, "-32600"));
    ASSERT_NOT_NULL(strstr(json, "Invalid Request"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_initialize_response) {
    /* Default (no params): returns latest supported version */
    char *json = cbm_mcp_initialize_response(NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "code-cortex-mcp"));
    ASSERT_NOT_NULL(strstr(json, "capabilities"));
    ASSERT_NOT_NULL(strstr(json, "tools"));
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);

    /* Client requests a supported version: server echoes it */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2024-11-05"));
    free(json);

    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2025-06-18\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-06-18"));
    free(json);

    /* Client requests unknown version: server returns its latest */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"9999-01-01\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);
    PASS();
}

TEST(mcp_tools_list) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    /* Should contain all 14 tools */
    ASSERT_NOT_NULL(strstr(json, "index_repository"));
    ASSERT_NOT_NULL(strstr(json, "search_graph"));
    ASSERT_NOT_NULL(strstr(json, "query_graph"));
    ASSERT_NOT_NULL(strstr(json, "trace_path"));
    ASSERT_NOT_NULL(strstr(json, "get_code_snippet"));
    ASSERT_NOT_NULL(strstr(json, "get_graph_schema"));
    ASSERT_NOT_NULL(strstr(json, "get_architecture"));
    ASSERT_NOT_NULL(strstr(json, "search_code"));
    ASSERT_NOT_NULL(strstr(json, "list_projects"));
    ASSERT_NOT_NULL(strstr(json, "delete_project"));
    ASSERT_NOT_NULL(strstr(json, "index_status"));
    ASSERT_NOT_NULL(strstr(json, "detect_changes"));
    ASSERT_NOT_NULL(strstr(json, "manage_adr"));
    ASSERT_NOT_NULL(strstr(json, "ingest_traces"));
    free(json);
    PASS();
}

TEST(mcp_tools_array_schemas_have_items) {
    /* VS Code 1.112+ rejects array schemas without "items" (see
     * https://github.com/microsoft/vscode/issues/248810).
     * Walk every tool's inputSchema and verify that every "type":"array"
     * property also contains "items". */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    /* Scan for all occurrences of "type":"array" — each must be followed
     * by "items" before the next closing brace of that property. */
    const char *p = json;
    while ((p = strstr(p, "\"type\":\"array\"")) != NULL) {
        /* Find the enclosing '}' for this property object */
        const char *end = strchr(p, '}');
        ASSERT_NOT_NULL(end);
        /* "items" must appear between p and end */
        size_t span = (size_t)(end - p);
        char *segment = (char *)malloc(span + 1);
        memcpy(segment, p, span);
        segment[span] = '\0';
        ASSERT_NOT_NULL(strstr(segment, "\"items\"")); /* array missing items */
        free(segment);
        p = end;
    }

    free(json);
    PASS();
}

TEST(mcp_text_result) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"type\":\"text\""));
    /* The text value is JSON-escaped inside the "text" field */
    ASSERT_NOT_NULL(strstr(json, "total"));
    ASSERT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_text_result_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(json, "something failed"));
    free(json);
    PASS();
}

/* #1522: the three structuredContent branches, bound one test each.
 *
 * A declared outputSchema makes spec-honoring clients (Claude Code among them)
 * read structuredContent as THE result, so the key must be present exactly
 * when it carries structure — and no tool may advertise a schema its
 * payload does not always satisfy. */

TEST(mcp_text_result_object_payload_carries_structured_content) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"structuredContent\""));
    /* The parsed object itself, not a re-wrapped string. */
    ASSERT_NULL(strstr(json, "\"structuredContent\":{\"text\""));
    free(json);
    PASS();
}

TEST(mcp_text_result_text_payload_omits_structured_content) {
    /* A non-object payload has no structure to carry. Emitting {} here is what
     * rendered whole replies as literally "{}" in schema-honoring clients. */
    char *json = cbm_mcp_text_result("plain text answer", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(strstr(json, "structuredContent"));
    ASSERT_NOT_NULL(strstr(json, "plain text answer"));
    free(json);
    PASS();
}

TEST(mcp_text_result_error_carries_structured_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"structuredContent\":{\"error\":\"something failed\"}"));
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_tools_list_declares_no_output_schema) {
    /* Output is payload-shaped, not schema-shaped: an error envelope and a
     * text answer do not satisfy one static schema, so no tool declares one. */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(strstr(json, "outputSchema"));
    ASSERT_NOT_NULL(strstr(json, "inputSchema"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_tool_name) {
    const char *params = "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\"}}";
    char *name = cbm_mcp_get_tool_name(params);
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "search_graph");
    free(name);
    PASS();
}

TEST(mcp_get_arguments) {
    const char *params =
        "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\",\"limit\":5}}";
    char *args = cbm_mcp_get_arguments(params);
    ASSERT_NOT_NULL(args);
    ASSERT_NOT_NULL(strstr(args, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(args, "\"limit\":5"));
    free(args);
    PASS();
}

TEST(mcp_get_string_arg) {
    const char *args = "{\"label\":\"Function\",\"name_pattern\":\".*Order.*\"}";
    char *val = cbm_mcp_get_string_arg(args, "label");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "Function");
    free(val);

    val = cbm_mcp_get_string_arg(args, "name_pattern");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, ".*Order.*");
    free(val);

    val = cbm_mcp_get_string_arg(args, "nonexistent");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg) {
    const char *args = "{\"limit\":10,\"offset\":5}";
    int val = cbm_mcp_get_int_arg(args, "limit", 0);
    ASSERT_EQ(val, 10);
    val = cbm_mcp_get_int_arg(args, "offset", 0);
    ASSERT_EQ(val, 5);
    val = cbm_mcp_get_int_arg(args, "missing", 42);
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(mcp_get_bool_arg) {
    const char *args = "{\"include_connected\":true,\"regex\":false}";
    bool val = cbm_mcp_get_bool_arg(args, "include_connected");
    ASSERT_TRUE(val);
    val = cbm_mcp_get_bool_arg(args, "regex");
    ASSERT_FALSE(val);
    val = cbm_mcp_get_bool_arg(args, "missing");
    ASSERT_FALSE(val);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — PROTOCOL FLOW
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_initialize) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                   "\"params\":{\"capabilities\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(resp, "code-cortex-mcp"));
    ASSERT_NOT_NULL(strstr(resp, "capabilities"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_initialized_notification) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Notification has no id → no response */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    ASSERT_NOT_NULL(strstr(resp, "query_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_unknown_method) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"unknown/method\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32601")); /* Method not found */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS (via server_handle)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: create a server with an in-memory store populated with test data */
static cbm_mcp_server_t *setup_mcp_with_data(void) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL); /* NULL = in-memory */
    return srv;
}

TEST(tool_list_projects_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":10"));
    /* Should return a result (possibly empty list) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    ASSERT_NOT_NULL(strstr(resp, "incomplete"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_graph_schema_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_graph_schema\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_unknown_tool) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"nonexistent_tool\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return result with isError */
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    /* search_graph with no project → should work on empty store */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"label\":\"Function\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Forward declarations for helpers defined later in this file */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz);
static void cleanup_snippet_dir(const char *tmp_dir);
static char *extract_text_content(const char *mcp_result);

TEST(tool_search_graph_includes_node_properties) {
    /* search_graph results must surface each node's properties_json
     * payload so callers don't have to round-trip through get_code_snippet
     * just to read them. The setup_snippet_server inserts HandleRequest
     * with a signature/return_type/is_exported property blob; this test
     * pins that those keys reach the MCP response. */
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project\":\"test-project\",\"label\":\"Function\","
             "\"name_pattern\":\"HandleRequest\",\"limit\":5}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* Properties from HandleRequest's properties_json must appear. */
    ASSERT_NOT_NULL(strstr(inner, "signature"));
    ASSERT_NOT_NULL(strstr(inner, "func HandleRequest"));
    ASSERT_NOT_NULL(strstr(inner, "is_exported"));
    free(inner);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_query_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\","
             "\"arguments\":{\"query\":\"MATCH (f:Function) RETURN f.name\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_no_project) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or empty status */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_includes_git_metadata) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\","
                                   "\"arguments\":{\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"root_path\""));
    ASSERT_NOT_NULL(strstr(inner, "\"git\""));
    ASSERT_NOT_NULL(strstr(inner, "\"is_git\":false"));
    ASSERT_NOT_NULL(strstr(inner, "\"root_exists\":true"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS WITH DATA
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_trace_call_path_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{\"function_name\":\"NonExistent\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about project not found */
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_trace_missing_function_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_delete_project_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"delete_project\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not_found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_architecture_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No store for nonexistent project — should return project error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #281: handle_get_architecture must actually call
 * cbm_store_get_architecture and surface its sections. Before the fix
 * only label/edge histograms were emitted regardless of which aspects
 * were requested. The store-side arch_entry_points query reads
 * properties.is_entry_point on Function nodes, so we tag one node and
 * assert the resulting JSON surfaces an "entry_points" array containing
 * the tagged function — which is impossible without the wiring. */
TEST(tool_get_architecture_emits_populated_sections) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-test";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-test");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch-test.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-test\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The handler always emits node/edge counts and schema histograms;
     * those existed before #281. The "entry_points" array only appears
     * when cbm_store_get_architecture is actually called and its result
     * is serialized — which is exactly what #281 wires up. */
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "main"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed") ||
                strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_code_multi_word) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Multi-word query "HandleRequest error" — should find the line
     * "func HandleRequest() error {" via regex conversion. */
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest error\","
             "\"project\":\"test-project\"}}}");

    char *resp = cbm_mcp_server_handle(srv, req);
    ASSERT_NOT_NULL(resp);
    /* Should find at least one result (not zero) */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL);
    /* Should NOT contain an error about "not found" */
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* NOTE: upstream's search_code_invalid_regex_errors_issue283 test is omitted in
 * this fork — the corresponding up-front cbm_regcomp validation is not ported
 * (std::regex::extended is stricter than the grep dialect actually used). See
 * UPSTREAM_SYNC.md. */

/* issue #282: a literal '|' under regex=false is a silent 0-match trap. It must
 * now be surfaced as a warning (and the result carries elapsed_ms). */
TEST(search_code_literal_pipe_warns_issue282) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest|Nope\","
                                   "\"regex\":false,\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "warnings"));   /* surfaced, not silent */
    ASSERT_NOT_NULL(strstr(resp, "regex=true")); /* the hint names the fix */
    ASSERT_NOT_NULL(strstr(resp, "elapsed_ms")); /* timing is reported */
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #272: '&' in a path / file_pattern is neutralised by the command's
 * quoting and must no longer be rejected as "invalid characters". */
TEST(search_code_ampersand_accepted_issue272) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest\","
                                   "\"file_pattern\":\"*R&D*.go\",\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid characters") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression test for issue #1363: detect_changes seeded every definition in
 * a changed file instead of just the ones whose line range overlaps the diff
 * hunk. cbm_detect_node_in_hunks is the overlap primitive; this exercises it
 * directly, independent of the git/subprocess/index plumbing around it. */
TEST(detect_changes_node_in_hunks_overlap_issue1363) {
    cbm_changed_hunk_t hunks[2] = {
        {"pkg/mod.py", 10, 12},
        {"pkg/other.py", 1, 1},
    };

    cbm_node_t inside = {};
    inside.start_line = 8;
    inside.end_line = 15;
    ASSERT(cbm_detect_node_in_hunks(&inside, hunks, 2, "pkg/mod.py"));

    cbm_node_t exact = {};
    exact.start_line = 10;
    exact.end_line = 12;
    ASSERT(cbm_detect_node_in_hunks(&exact, hunks, 2, "pkg/mod.py"));

    cbm_node_t touches_edge = {};
    touches_edge.start_line = 12;
    touches_edge.end_line = 20;
    ASSERT(cbm_detect_node_in_hunks(&touches_edge, hunks, 2, "pkg/mod.py"));

    cbm_node_t before = {};
    before.start_line = 1;
    before.end_line = 9;
    ASSERT(!cbm_detect_node_in_hunks(&before, hunks, 2, "pkg/mod.py"));

    cbm_node_t after = {};
    after.start_line = 13;
    after.end_line = 20;
    ASSERT(!cbm_detect_node_in_hunks(&after, hunks, 2, "pkg/mod.py"));

    /* Same line range, different file — must not match. */
    cbm_node_t wrong_file = {};
    wrong_file.start_line = 10;
    wrong_file.end_line = 12;
    ASSERT(!cbm_detect_node_in_hunks(&wrong_file, hunks, 2, "pkg/unrelated.py"));

    PASS();
}

/* `git -C` with double quotes, not `cd '<dir>' &&`: single quotes are not
 * quoting characters for cmd.exe, and identity/branch/signing come from -c so
 * the fixture does not depend on the machine's global git config. */
#define DC1363_GITCFG \
    "-c user.name=t -c user.email=t@t.io -c init.defaultBranch=main -c commit.gpgsign=false"

static int dc1363_git_init(const char *repo) {
    char cmd[1200];
    const char *steps[] = {"init -q", "add -A", "commit -q -m init"};
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" " DC1363_GITCFG " %s", repo, steps[s]);
        if (system(cmd) != 0) {
            return -1;
        }
    }
    return 0;
}

/* End-to-end regression test for issue #1363: a same-line-count edit inside
 * one function must seed only that function, not every definition in the
 * file. A flat file with two independent top-level functions (no enclosing
 * class) makes this unambiguous — before the fix, editing foo() also seeded
 * bar() because seeding was scoped to the whole changed file.
 *
 * Asserted against this fork's JSON detect_changes response ("seed_count"),
 * not upstream's tree-format "seed_symbols: N" text, which this fork does not
 * carry. */
TEST(detect_changes_seeds_only_touched_symbol_issue1363) {
    char *tmp = th_mktempdir("cbm-detect-seed-scope");
    ASSERT_NOT_NULL(tmp);
    char repo[512];
    snprintf(repo, sizeof(repo), "%s", tmp);

    char src[600];
    snprintf(src, sizeof(src), "%s/mod.py", repo);
    ASSERT_EQ(th_write_file(src, "def foo():\n"
                                 "    x = 1\n"
                                 "    return x\n"
                                 "\n"
                                 "\n"
                                 "def bar():\n"
                                 "    y = 2\n"
                                 "    return y\n"),
              0);

    if (dc1363_git_init(repo) != 0) {
        th_rmtree(repo);
        FAIL("git fixture setup failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char idx_args[700];
    snprintf(idx_args, sizeof(idx_args), "{\"repo_path\":\"%s\",\"mode\":\"full\"}", repo);
    char *idx_resp = cbm_mcp_handle_tool(srv, "index_repository", idx_args);
    ASSERT_NOT_NULL(idx_resp);
    free(idx_resp);

    /* Same-line-count in-place edit inside foo() only; bar() is untouched. */
    ASSERT_EQ(th_write_file(src, "def foo():\n"
                                 "    x = 11\n"
                                 "    return x\n"
                                 "\n"
                                 "\n"
                                 "def bar():\n"
                                 "    y = 2\n"
                                 "    return y\n"),
              0);

    char *project = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(project);
    char dc_args[700];
    snprintf(dc_args, sizeof(dc_args), "{\"project\":\"%s\",\"depth\":1}", project);
    char *dc_resp = cbm_mcp_handle_tool(srv, "detect_changes", dc_args);
    ASSERT_NOT_NULL(dc_resp);
    /* Before the fix this was 2 — editing foo() also seeded bar(). */
    ASSERT_NOT_NULL(strstr(dc_resp, "\"seed_count\":1"));

    free(dc_resp);
    free(project);
    cbm_mcp_server_free(srv);
    th_rmtree(repo);
    PASS();
}

/* Recall guard for the zero-overlap case (#1363 review): an import-only edit
 * changes lines that lie outside every definition's range. Scoping alone would
 * drop the file from the seed set — worse recall than the whole-file behavior
 * being replaced — so detect_collect_seeds falls back to whole-file seeding
 * when a changed file has hunks but no definition overlapping any of them. */
TEST(detect_changes_zero_overlap_falls_back_issue1363) {
    char *tmp = th_mktempdir("cbm-detect-zero-overlap");
    ASSERT_NOT_NULL(tmp);
    char repo[512];
    snprintf(repo, sizeof(repo), "%s", tmp);

    char src[600];
    snprintf(src, sizeof(src), "%s/mod.py", repo);
    /* Import on line 1 sits above both definitions. */
    ASSERT_EQ(th_write_file(src, "import os\n"
                                 "\n"
                                 "\n"
                                 "def foo():\n"
                                 "    return 1\n"
                                 "\n"
                                 "\n"
                                 "def bar():\n"
                                 "    return 2\n"),
              0);

    if (dc1363_git_init(repo) != 0) {
        th_rmtree(repo);
        FAIL("git fixture setup failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char idx_args[700];
    snprintf(idx_args, sizeof(idx_args), "{\"repo_path\":\"%s\",\"mode\":\"full\"}", repo);
    char *idx_resp = cbm_mcp_handle_tool(srv, "index_repository", idx_args);
    ASSERT_NOT_NULL(idx_resp);
    free(idx_resp);

    /* Edit ONLY the import line — outside every definition's line range. */
    ASSERT_EQ(th_write_file(src, "import os, sys\n"
                                 "\n"
                                 "\n"
                                 "def foo():\n"
                                 "    return 1\n"
                                 "\n"
                                 "\n"
                                 "def bar():\n"
                                 "    return 2\n"),
              0);

    char *project = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(project);
    char dc_args[700];
    snprintf(dc_args, sizeof(dc_args), "{\"project\":\"%s\",\"depth\":1}", project);
    char *dc_resp = cbm_mcp_handle_tool(srv, "detect_changes", dc_args);
    ASSERT_NOT_NULL(dc_resp);
    /* Both definitions must survive: zero overlaps means no scoping for this
     * file, not an empty seed set. */
    ASSERT_NOT_NULL(strstr(dc_resp, "\"seed_count\":2"));

    free(dc_resp);
    free(project);
    cbm_mcp_server_free(srv);
    th_rmtree(repo);
    PASS();
}

TEST(tool_manage_adr_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"manage_adr\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression test for use-after-free in handle_manage_adr (get path).
 * MUST FAIL before fix: free(buf) is called before yy_doc_to_str serializes doc,
 * so result field is missing or contains garbage. MUST PASS after fix. */
TEST(tool_manage_adr_get_with_existing_adr) {
    /* Create a temp directory with .code-cortex/adr.md */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS(); /* skip if mkdtemp fails */
    }

    char adr_dir[512];
    snprintf(adr_dir, sizeof(adr_dir), "%s/.code-cortex", tmp_dir);
    cbm_mkdir(adr_dir);

    char adr_path[512];
    snprintf(adr_path, sizeof(adr_path), "%s/adr.md", adr_dir);
    FILE *fp = fopen(adr_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("## PURPOSE\nTest ADR content for regression test.\n\n"
          "## STACK\nC, SQLite.\n\n"
          "## ARCHITECTURE\nMCP server.\n",
          fp);
    fclose(fp);

    /* Create server and register the project */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "test-adr-uaf", tmp_dir);
    cbm_mcp_server_set_project(srv, "test-adr-uaf");

    /* Call manage_adr via full JSON-RPC path to exercise cbm_jsonrpc_format_response.
     * The bug: free(buf) before yy_doc_to_str causes garbage JSON; format_response
     * then fails to parse the result and omits the "result" field entirely. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\","
             "\"arguments\":{\"project\":\"test-adr-uaf\",\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    /* JSON-RPC response must include a "result" field (absent when use-after-free) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    /* ADR content must appear in response */
    ASSERT_NOT_NULL(strstr(resp, "PURPOSE"));
    /* Must not be an error */
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* Clean up */
    cbm_mcp_server_free(srv);
    remove(adr_path);
    rmdir(adr_dir);
    rmdir(tmp_dir);
    PASS();
}

/* issue #256: manage_adr (MCP) and the UI /api/adr endpoints must share ONE
 * backend. A manage_adr(update) write must be readable via cbm_store_adr_get
 * (the exact API the UI's /api/adr GET uses). */
TEST(tool_manage_adr_unified_backend_issue256) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-unify", "/tmp/adr-unify");
    cbm_mcp_server_set_project(srv, "adr-unify");

    /* Write via the MCP tool. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":120,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"update\",\"content\":\"## PURPOSE\\nUnified ADR backend.\\n\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    /* Read DIRECTLY via the store API the UI /api/adr uses — must see it. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-unify", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_NOT_NULL(strstr(adr.content, "Unified ADR backend."));
    cbm_store_adr_free(&adr);

    /* And manage_adr(get) round-trips the same content. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":121,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Unified ADR backend."));
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"ingest_traces\","
             "\"arguments\":{\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    ASSERT_NOT_NULL(strstr(resp, "traces_received"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":38,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"ingest_traces\","
                                   "\"arguments\":{\"traces\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Write sample source file */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "package main\n"
                "\n"
                "func HandleRequest() error {\n"
                "\treturn nil\n"
                "}\n"
                "\n"
                "func ProcessOrder(id int) {\n"
                "\t// process\n"
                "}\n"
                "\n"
                "func Run() {\n"
                "\t// server\n"
                "}\n");
    fclose(fp);

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    return srv;
}

/* SCC condensation (get_architecture aspect "cycles"): a 3-function CALLS
 * cycle A->B->C->A must be reported as one circular dependency of size 3 with
 * all three members; a separate acyclic chain (D->E) must NOT appear. The
 * aspect is opt-in — a default get_architecture call must NOT compute it.
 * (Fork: assertions target the JSON response model — this tree has no TOON /
 * tree-format output.) */
TEST(tool_get_architecture_cycles_detects_scc) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "cycproj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/cyc");

    const char *names[5] = {"A", "B", "C", "D", "E"};
    int64_t id[5];
    for (int i = 0; i < 5; i++) {
        char qn[32];
        snprintf(qn, sizeof(qn), "cycproj.m.%s", names[i]);
        cbm_node_t n = {.project = proj,
                        .label = "Function",
                        .name = names[i],
                        .qualified_name = qn,
                        .file_path = "m.c",
                        .start_line = i + 1,
                        .end_line = i + 2};
        id[i] = cbm_store_upsert_node(st, &n);
        ASSERT_GT(id[i], 0);
    }
    /* cycle A->B->C->A, plus acyclic D->E */
    struct {
        int f;
        int t;
    } e[] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}};
    for (size_t i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
        cbm_edge_t ed = {
            .project = proj, .source_id = id[e[i].f], .target_id = id[e[i].t], .type = "CALLS"};
        ASSERT_GT(cbm_store_insert_edge(st, &ed), 0);
    }

    /* opt-in cycles aspect */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":71,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"get_architecture\",\"arguments\":{\"project\":\"cycproj\","
             "\"aspects\":[\"cycles\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"cycles\""));
    ASSERT_NOT_NULL(strstr(inner, "\"size\":3")); /* exactly one SCC, of size 3 */
    ASSERT_NOT_NULL(strstr(inner, "cycproj.m.A"));
    ASSERT_NOT_NULL(strstr(inner, "cycproj.m.B"));
    ASSERT_NOT_NULL(strstr(inner, "cycproj.m.C"));
    ASSERT_NULL(strstr(inner, "cycproj.m.D")); /* acyclic node not in any cycle */
    free(inner);
    free(resp);

    /* default call (no aspects) must NOT run the scan. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":72,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"get_architecture\",\"arguments\":{\"project\":\"cycproj\"}}}");
    ASSERT_NOT_NULL(resp);
    inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "\"cycles\""));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Context-bomb guard: get_code_snippet on a whole-file node (a Module/File
 * span) used to read the ENTIRE file into one response — a field-eval agent
 * that fell back to a Module snippet pulled ~400KB in a single call. The read
 * must clip at MCP_SNIPPET_MAX_LINES and flag source_clipped, while the exact
 * start/end range stays in the response for a targeted re-read. */
TEST(tool_get_code_snippet_clips_whole_file_node) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_snipcap_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmp));
    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp);
    cbm_mkdir(proj_dir);
    char src_path[600];
    snprintf(src_path, sizeof(src_path), "%s/big.py", proj_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    enum { BIG_LINES = 2000 };
    for (int i = 0; i < BIG_LINES; i++) {
        fprintf(fp, "line_%04d = %d  # padding to blow up an unclipped read\n", i, i);
    }
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "test-project";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    cbm_node_t mod = {0};
    mod.project = proj;
    mod.label = "Module";
    mod.name = "big";
    mod.qualified_name = "test-project.big";
    mod.file_path = "big.py";
    mod.start_line = 1;
    mod.end_line = BIG_LINES;
    ASSERT_GT(cbm_store_upsert_node(st, &mod), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":70,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"get_code_snippet\",\"arguments\":{\"project\":\"test-project\","
             "\"qualified_name\":\"test-project.big\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"source_clipped\":true"));
    /* The whole 2000-line file (~100KB) must NOT be in the response. */
    ASSERT_TRUE(strlen(inner) < 60000);
    /* The last line must be absent (clipped), the first present. */
    ASSERT_NOT_NULL(strstr(inner, "line_0000"));
    ASSERT_NULL(strstr(inner, "line_1999"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    unlink(src_path);
    rmdir(proj_dir);
    rmdir(tmp);
    PASS();
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content) {
        /* Handle JSON-RPC wrapper: {"jsonrpc":...,"result":{"content":[...]}} */
        yyjson_val *rpc_result = yyjson_obj_get(root, "result");
        if (rpc_result) {
            content = yyjson_obj_get(rpc_result, "content");
        }
    }
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* Enriched properties */
    ASSERT_NOT_NULL(strstr(resp, "\"signature\":\"func HandleRequest() error\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\":\"error\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NOT_NULL(strstr(resp, "\"is_exported\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_empty_string) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_jsonrpc_field) {
    /* jsonrpc field absent — parser defaults to "2.0" if method present */
    const char *line = "{\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_TRUE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_method) {
    /* method is required — should fail */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_string_id) {
    /* JSON-RPC §4: string and numeric ids are distinct. A string id is
     * preserved verbatim (issue #253), never coerced to a number. */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "99");
    ASSERT_STR_EQ(req.method, "tools/list");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_no_params) {
    /* Request with no params field — params_raw should be NULL */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(req.params_raw);
    ASSERT_EQ(req.id, 5);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_extra_whitespace) {
    /* Leading/trailing whitespace and internal spacing in JSON */
    const char *line = "  { \"jsonrpc\" : \"2.0\" , \"id\" : 7 , \"method\" : \"ping\" }  ";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(req.id, 7);
    ASSERT_STR_EQ(req.method, "ping");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_array_not_object) {
    /* JSON array at root — not a valid JSON-RPC request */
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("[1,2,3]", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_string_arg_empty_json) {
    /* Empty JSON string — yyjson_read fails → NULL */
    char *val = cbm_mcp_get_string_arg("", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_empty_object) {
    /* Valid JSON with no keys → NULL for any key */
    char *val = cbm_mcp_get_string_arg("{}", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_nested_value) {
    /* Value is an object, not a string → should return NULL */
    const char *args = "{\"config\":{\"nested\":true},\"name\":\"hello\"}";
    char *val = cbm_mcp_get_string_arg(args, "config");
    ASSERT_NULL(val); /* not a string type */
    val = cbm_mcp_get_string_arg(args, "name");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "hello");
    free(val);
    PASS();
}

TEST(mcp_get_string_arg_int_value) {
    /* Value is an integer, not a string → NULL */
    char *val = cbm_mcp_get_string_arg("{\"count\":42}", "count");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg_empty_json) {
    int val = cbm_mcp_get_int_arg("", "key", 99);
    ASSERT_EQ(val, 99);
    PASS();
}

TEST(mcp_get_int_arg_string_value) {
    /* Value is a string, not int → should return default */
    int val = cbm_mcp_get_int_arg("{\"limit\":\"ten\"}", "limit", 5);
    ASSERT_EQ(val, 5);
    PASS();
}

TEST(mcp_get_int_arg_bool_value) {
    /* Value is a bool, not int → default */
    int val = cbm_mcp_get_int_arg("{\"flag\":true}", "flag", -1);
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(mcp_get_bool_arg_empty_json) {
    bool val = cbm_mcp_get_bool_arg("", "key");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_bool_arg_int_value) {
    /* Value is int 1, not bool → should return false */
    bool val = cbm_mcp_get_bool_arg("{\"flag\":1}", "flag");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_tool_name_empty_json) {
    char *name = cbm_mcp_get_tool_name("");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_tool_name_missing_name) {
    char *name = cbm_mcp_get_tool_name("{\"arguments\":{}}");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_arguments_empty_json) {
    char *args = cbm_mcp_get_arguments("");
    ASSERT_NULL(args);
    PASS();
}

TEST(mcp_get_arguments_no_arguments_key) {
    /* No "arguments" key → returns "{}" */
    char *args = cbm_mcp_get_arguments("{\"name\":\"tool\"}");
    ASSERT_NOT_NULL(args);
    ASSERT_STR_EQ(args, "{}");
    free(args);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  FILE URI PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_http_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("http://example.com/path", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_ftp_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("ftp://server/file.txt", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_buffer_too_small) {
    char path[5]; /* only 5 bytes — path gets truncated */
    ASSERT_TRUE(cbm_parse_file_uri("file:///usr/local/bin", path, sizeof(path)));
    /* snprintf truncates to 4 chars + NUL */
    ASSERT_EQ(strlen(path), 4);
    ASSERT_STR_EQ(path, "/usr");
    PASS();
}

TEST(parse_file_uri_spaces_in_path) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/my%20project", path, sizeof(path)));
    /* Raw percent-encoding is preserved (not decoded) */
    ASSERT_STR_EQ(path, "/home/user/my%20project");
    PASS();
}

TEST(parse_file_uri_null_out_path) {
    /* NULL out_path — should not crash */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", NULL, 256));
    PASS();
}

TEST(parse_file_uri_zero_size) {
    char path[256] = "garbage";
    /* out_size=0 → should fail safely */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", path, 0));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_invalid_json) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(srv, "this is not json at all");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32700")); /* Parse error */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_empty_object) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Valid JSON but no method field → parse error */
    char *resp = cbm_mcp_server_handle(srv, "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_call_missing_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* tools/call with no tool name in params */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
                                   "\"params\":{\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about unknown/missing tool */
    ASSERT_NOT_NULL(strstr(resp, "\"id\":50"));
    ASSERT_TRUE(strstr(resp, "error") || strstr(resp, "isError") || strstr(resp, "unknown"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  POLL/GETLINE FILE* BUFFERING FIX
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>

/* Signal handler used by alarm() to abort the test if it hangs */
static void alarm_handler(int sig) {
    (void)sig;
    /* Writing to stderr is async-signal-safe */
    const char msg[] = "FAIL: mcp_server_run_rapid_messages timed out (>5s)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

TEST(mcp_server_run_rapid_messages) {
    /* Simulate a client sending initialize + notifications/initialized +
     * tools/list all at once (no delays), which exercises the FILE*
     * buffering fix: the first getline() over-reads kernel data into the
     * libc buffer; without the fix, subsequent poll() calls block for 60s.
     *
     * We use alarm(5) to abort the test process if the server hangs. */
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Write all 3 messages to the write end in one shot */
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
    ssize_t written = write(fds[1], msgs, strlen(msgs));
    ASSERT_TRUE(written > 0);
    close(fds[1]); /* EOF signals end of input to the server */

    FILE *in_fp = fdopen(fds[0], "r");
    ASSERT_NOT_NULL(in_fp);

    FILE *out_fp = tmpfile();
    ASSERT_NOT_NULL(out_fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Install alarm to fail the test if cbm_mcp_server_run blocks */
    signal(SIGALRM, alarm_handler);
    alarm(5);

    int rc = cbm_mcp_server_run(srv, in_fp, out_fp);

    alarm(0); /* cancel alarm */
    signal(SIGALRM, SIG_DFL);

    ASSERT_EQ(rc, 0);

    /* Verify both responses are present:
     *   id:1 — initialize response
     *   id:2 — tools/list response (notifications/initialized produces none)
     * and that the tools list payload is included. */
    rewind(out_fp);
    char buf[4096] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, out_fp);
    ASSERT_TRUE(nread > 0);
    ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(buf, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(buf, "tools"));

    cbm_mcp_server_free(srv);
    fclose(out_fp);
    /* in_fp already EOF; fclose cleans up */
    fclose(in_fp);
    PASS();
}
#endif /* !_WIN32 */

/* Issue #235: passing an unrecognised project name to a tool crashed the
 * binary with a buffer overflow while building the "available_projects"
 * error list — collect_db_project_names overflowed projects[CBM_SZ_4K] via
 * an unsigned underflow on (out_sz - offset) once the listed names exceeded
 * the buffer. Fill a temp cache dir with enough long-named .db files to
 * exceed 4 KB, then hit the bad-project path. Under ASan a regression aborts
 * here; the fixed bounds-check keeps it clean and returns a normal error. */
#define ISSUE235_DBNAME(buf, dir, i)                                                         \
    snprintf((buf), sizeof(buf),                                                             \
             "%s/proj_%02d_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.db",                      \
             (dir), (i))
TEST(tool_bad_project_name_no_overflow_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 40 * ~130-char names overflows the 4 KB available-projects buffer. */
    enum { ISSUE235_N = 40 };
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        FILE *fp = fopen(name, "w");
        if (fp) {
            fputc('x', fp);
            fclose(fp);
        }
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"search_graph\",\"arguments\":{\"label\":\"Function\","
             "\"project\":\"definitely-not-a-real-project-xyz\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        cbm_unlink(name);
    }
    cbm_rmdir(cache);
    PASS();
}
#undef ISSUE235_DBNAME

#ifndef _WIN32
TEST(tool_unknown_project_skips_nonregular_cache_db) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-fifo-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char fifo_path[512];
    snprintf(fifo_path, sizeof(fifo_path), "%s/blocked.db", cache);
    ASSERT_EQ(mkfifo(fifo_path, 0600), 0);

    struct timespec start;
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph", "{\"project\":\"definitely-not-indexed\",\"name_pattern\":\"x\"}");
    clock_gettime(CLOCK_MONOTONIC, &finish);

    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    long elapsed_ms =
        (finish.tv_sec - start.tv_sec) * 1000L + (finish.tv_nsec - start.tv_nsec) / 1000000L;
    ASSERT_TRUE(elapsed_ms < 10000);
    free(resp);
    cbm_mcp_server_free(srv);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_unlink(fifo_path);
    cbm_rmdir(cache);
    PASS();
}
#endif

/* #1211: list_projects only ever advertises the project NAME, never the
 * repo_path, but re-indexing by that same name (the natural next call) used
 * to fall straight to "repo_path is required" because nothing resolved the
 * name back to its stored root_path. Index once by repo_path, then re-index
 * by project name alone and confirm it actually indexes instead of erroring. */
TEST(tool_index_repository_resolves_root_path_from_project_name_issue1211) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-byname-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-byname-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char index_args[1024];
    snprintf(index_args, sizeof(index_args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", index_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"status\\\":\\\"indexed\\\""));
    free(resp);

    char by_name_args[512];
    snprintf(by_name_args, sizeof(by_name_args), "{\"project\":\"%s\",\"mode\":\"fast\"}", project);
    resp = cbm_mcp_handle_tool(srv, "index_repository", by_name_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "repo_path is required"));
    ASSERT_NOT_NULL(strstr(resp, "\\\"status\\\":\\\"indexed\\\""));
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(project);
    remove(src_path);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

/* Same gap, opposite outcome: a project name that was never indexed has no
 * stored root_path to resolve, so it must still fail with the same clear
 * "repo_path is required" error rather than a resolver crash or silent
 * no-op. Guards the fallback path the fix above added. */
TEST(tool_index_repository_unknown_project_name_still_requires_repo_path) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-byname-unknown-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_handle_tool(srv, "index_repository", "{\"project\":\"never-indexed-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "repo_path is required"));
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_rmdir(cache);
    PASS();
}

/* A partially-parsed file makes indexing write an internal "<name>::missed"
 * miss-graph row into the SAME database as the primary project. Project
 * resolution required exactly ONE row over ALL rows returned by
 * cbm_store_list_projects, which does not filter those — so any project that
 * had ever recorded a parse miss became unresolvable and vanished from
 * list_projects entirely, for a project that plainly was indexed. There is no
 * user-level workaround: a partial parse is not something the operator
 * controls, and re-indexing reproduces the shadow row.
 *
 * The single-primary requirement itself is kept — it is what proves the db
 * belongs to one project rather than being shared or mislabelled — so the
 * control below (a clean project in the same cache) must keep resolving too. */
TEST(tool_list_projects_includes_a_project_with_a_miss_graph) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-missgraph-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-missgraph-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* A file the Rust grammar cannot parse — this is what mints the shadow row. */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/broken.rs", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("fn broken( { let x = ;;; unterminated\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char index_args[1024];
    snprintf(index_args, sizeof(index_args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", index_args);
    ASSERT_NOT_NULL(resp);
    /* Precondition: the fixture really did record a parse miss. Without this the
     * test could pass vacuously on a grammar that happens to accept the file.
     * Captured rather than asserted here, for the teardown reason below. */
    bool recorded_a_miss = resp && strstr(resp, "\\\"parse_partial_count\\\":0") == NULL;
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "list_projects", "{}");
    bool listed = resp && strstr(resp, project) != NULL;
    free(resp);

    /* Tear down BEFORE asserting. ASSERT returns from the test function, so a
     * failure here would otherwise skip the CBM_CACHE_DIR restore below and
     * leave every later test pointed at this temp cache — turning one honest
     * red into a cascade of unrelated ones and burying the actual cause. */
    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(project);
    remove(src_path);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);

    ASSERT_TRUE(recorded_a_miss);
    ASSERT_TRUE(listed);
    PASS();
}

/* The test-side caller lives directly under a PROJECT-ROOT tests/ directory
 * with no test_/_test naming (tests/repro/helper.c). is_test_file() in mcp.cpp
 * matched a NESTED ".../tests/..." path but not a project-root-relative one, so
 * this row leaked into results with the default include_tests=false (#1294,
 * secondary bug). The include_tests=true half is the control: it proves the row
 * exists at all, so the default-filtered count cannot pass vacuously. */
TEST(tool_trace_totals_respect_test_filter_tests_root_subtree_issue1294) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "totproj2";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/tot2");

    cbm_node_t tgt = {.project = proj,
                      .label = "Function",
                      .name = "tgt2",
                      .qualified_name = "totproj2.a.tgt2",
                      .file_path = "a.c",
                      .start_line = 1,
                      .end_line = 5};
    int64_t tid = cbm_store_upsert_node(st, &tgt);
    ASSERT_GT(tid, 0);
    cbm_node_t prod = {.project = proj,
                       .label = "Function",
                       .name = "prod_caller2",
                       .qualified_name = "totproj2.a.prod_caller2",
                       .file_path = "a.c",
                       .start_line = 10,
                       .end_line = 15};
    int64_t pid = cbm_store_upsert_node(st, &prod);
    ASSERT_GT(pid, 0);
    cbm_node_t tst = {.project = proj,
                      .label = "Function",
                      .name = "repro_caller",
                      .qualified_name = "totproj2.t.repro_caller",
                      .file_path = "tests/repro/helper.c",
                      .start_line = 1,
                      .end_line = 5};
    int64_t xid = cbm_store_upsert_node(st, &tst);
    ASSERT_GT(xid, 0);
    cbm_edge_t e1 = {.project = proj, .source_id = pid, .target_id = tid, .type = "CALLS"};
    ASSERT_GT(cbm_store_insert_edge(st, &e1), 0);
    cbm_edge_t e2 = {.project = proj, .source_id = xid, .target_id = tid, .type = "CALLS"};
    ASSERT_GT(cbm_store_insert_edge(st, &e2), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_call_path\",\"arguments\":{\"project\":\"totproj2\","
             "\"function_name\":\"tgt2\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    /* Assert on the caller NAMES rather than upstream's callers_total field,
     * which this tree's trace output does not emit. Stronger anyway: it names
     * exactly which row is present and which is filtered. */
    bool default_has_prod = strstr(inner, "prod_caller2") != NULL;
    bool default_has_test = strstr(inner, "repro_caller") != NULL;
    free(inner);
    ASSERT_TRUE(default_has_prod);
    ASSERT_FALSE(default_has_test); /* tests/repro/ row filtered by default */

    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_call_path\",\"arguments\":{\"project\":\"totproj2\","
             "\"function_name\":\"tgt2\",\"direction\":\"inbound\",\"include_tests\":true}}}");
    ASSERT_NOT_NULL(resp);
    inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    bool incl_has_prod = strstr(inner, "prod_caller2") != NULL;
    bool incl_has_test = strstr(inner, "repro_caller") != NULL;
    free(inner);
    ASSERT_TRUE(incl_has_prod);
    ASSERT_TRUE(incl_has_test); /* control: the row exists, so the filter above is real */
    cbm_mcp_server_free(srv);
    PASS();
}

/* search_code full results must preserve valid UTF-8. The old sanitizer replaced
 * every byte > 127 with '?', so any non-English identifier, string or comment
 * came back mangled even though it was perfectly valid UTF-8 — and valid JSON.
 * Measured on a Python fixture before the fix: 39 substituted bytes, zero
 * readable Chinese; after: 0 and 0.
 *
 * Asserts on the BYTES appearing in the response rather than on a fixed JSON
 * column index, so it does not bind to this tree's row layout. */
TEST(search_code_full_preserves_utf8_source) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_srch_utf8_XXXXXX");
    ASSERT_TRUE(cbm_mkdtemp(tmp) != NULL);

    char design_dir[768];
    snprintf(design_dir, sizeof(design_dir), "%s/design", tmp);
    ASSERT_EQ(cbm_mkdir(design_dir), 0);

    char source_path[900];
    snprintf(source_path, sizeof(source_path), "%s/design.md", design_dir);
    FILE *fp = cbm_fopen(source_path, "wb");
    ASSERT_NOT_NULL(fp);
    static const char kCyrillic[] = "Русский текст: бухгалтерский учет.";
    char body[256];
    snprintf(body, sizeof(body), "# accounting-design\n%s\n", kCyrillic);
    ASSERT_EQ(fwrite(body, 1, strlen(body), fp), strlen(body));
    ASSERT_EQ(fclose(fp), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *project = "utf8-search";
    cbm_mcp_server_set_project(srv, project);
    cbm_store_upsert_project(st, project, tmp);

    cbm_node_t section = {.project = project,
                          .label = "Section",
                          .name = "accounting-design",
                          .qualified_name = "utf8-search.design.accounting-design",
                          .file_path = "design/design.md",
                          .start_line = 1,
                          .end_line = 2};
    ASSERT_GT(cbm_store_upsert_node(st, &section), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":97,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\",\"arguments\":{"
             "\"project\":\"utf8-search\",\"pattern\":\"accounting-design\","
             "\"file_pattern\":\"*.md\",\"mode\":\"full\",\"limit\":5}}}");
    bool kept_utf8 = resp && strstr(resp, "Русский") != NULL;
    /* The control: if the search returned nothing at all, kept_utf8 would be
     * false for the wrong reason. Require the row itself to be present. */
    bool found_row = resp && strstr(resp, "accounting-design") != NULL;
    free(resp);

    cbm_mcp_server_free(srv);
    cbm_unlink(source_path);
    cbm_rmdir(design_dir);
    cbm_rmdir(tmp);

    ASSERT_TRUE(found_row);
    ASSERT_TRUE(kept_utf8);
    PASS();
}

/* A semantic-only search_graph must NOT also return the unfiltered graph. With
 * no structural filter, cbm_store_search matches everything, so appending
 * semantic hits to it produced a response byte-identical to "no filters at all"
 * — measured on a 31-node fixture: 31 of 31 returned for a two-keyword semantic
 * query, the semantic query contributing nothing but noise.
 *
 * The two controls matter as much as the claim: a STRUCTURAL query must be
 * unaffected, and a NO-FILTER query must still return everything. Without them
 * this would pass on a build that had simply broken search_graph. */
TEST(search_graph_semantic_only_skips_structural_scan) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "semonly";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/semonly");

    for (int i = 0; i < 5; i++) {
        char name[32];
        char qn[64];
        snprintf(name, sizeof(name), "fn_%d", i);
        snprintf(qn, sizeof(qn), "semonly.a.fn_%d", i);
        cbm_node_t n = {.project = proj,
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "a.py",
                        .start_line = 1,
                        .end_line = 2};
        ASSERT_GT(cbm_store_upsert_node(st, &n), 0);
    }

    /* Semantic-only: no structural rows. (Vector search may legitimately return
     * nothing without embeddings; the claim under test is the ABSENCE of the
     * unfiltered structural dump.) */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":80,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"project\":\"semonly\","
             "\"semantic_query\":[\"alpha\",\"beta\"]}}}");
    ASSERT_NOT_NULL(resp);
    bool semantic_leaked_structural = strstr(resp, "fn_0") != NULL;
    free(resp);

    /* Control 1: a structural query still returns its rows. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":81,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"project\":\"semonly\","
             "\"label\":\"Function\"}}}");
    ASSERT_NOT_NULL(resp);
    bool structural_works = strstr(resp, "fn_0") != NULL;
    free(resp);

    /* Control 2: no filters at all still returns everything. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":82,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"project\":\"semonly\"}}}");
    ASSERT_NOT_NULL(resp);
    bool unfiltered_works = strstr(resp, "fn_0") != NULL;
    free(resp);

    cbm_mcp_server_free(srv);
    ASSERT_FALSE(semantic_leaked_structural);
    ASSERT_TRUE(structural_works);
    ASSERT_TRUE(unfiltered_works);
    PASS();
}

/* Coverage gaps must survive an incremental run that does not revisit them.
 * The response used to be built from the PER-RUN parser errors, so re-indexing
 * after touching only a clean neighbour reported parse_partial_count = 0 and
 * the known-broken files appeared to have been fixed. Measured before the fix:
 * 8 -> 0 across two runs of the same tree; after: 8 -> 8.
 *
 * The pipeline persists the complete current coverage set before this response
 * is built, so that set is authoritative and the per-run errors are only the
 * fallback for when it cannot be read. */
TEST(index_response_reports_persisted_coverage_on_reindex) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-persistcov-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-persistcov-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char broken_path[512];
    char clean_path[512];
    snprintf(broken_path, sizeof(broken_path), "%s/broken.rs", tmp_dir);
    snprintf(clean_path, sizeof(clean_path), "%s/clean.rs", tmp_dir);
    FILE *fp = cbm_fopen(broken_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("fn broken( { let x = ;;; unterminated\n", fp);
    ASSERT_EQ(fclose(fp), 0);
    fp = cbm_fopen(clean_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("fn clean_one() {}\n", fp);
    ASSERT_EQ(fclose(fp), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[700];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    bool first_saw_gap = strstr(resp, "\\\"parse_partial_count\\\":0") == NULL;
    free(resp);

    /* Re-index after changing ONLY the clean neighbour, so the broken file is
     * not revisited and produces no fresh parser error. */
    fp = cbm_fopen(clean_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("fn clean_one() {}\nfn clean_two() {}\n", fp);
    ASSERT_EQ(fclose(fp), 0);

    resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    bool reindex_kept_gap = strstr(resp, "\\\"parse_partial_count\\\":0") == NULL;
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    remove(broken_path);
    remove(clean_path);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);

    ASSERT_TRUE(first_saw_gap);    /* control: the fixture really is partial */
    ASSERT_TRUE(reindex_kept_gap); /* the claim */
    PASS();
}

/* list_projects pages DETERMINISTICALLY. Without the sort, paging over readdir
 * order is meaningless — the order is filesystem-dependent, so page 2 could
 * repeat or skip entries from page 1, and even the unpaginated list differed
 * across platforms.
 *
 * Details are now OPT-IN: opening every project's database to count nodes and
 * edges is what made this slow on a large install, and it now happens for at
 * most `limit` of them. The NAME is always present, because it is the
 * identifier every other tool takes. */
TEST(tool_list_projects_pages_deterministically) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-lp-page-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Created in NON-alphabetical order so a passing sort cannot be readdir
     * order by luck. */
    static const char *kNames[] = {"zeta", "alpha", "mike", "bravo"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        char dbpath[512];
        snprintf(dbpath, sizeof(dbpath), "%s/%s.db", cache, kNames[i]);
        cbm_store_t *st = cbm_store_open_path(dbpath);
        if (st) {
            cbm_store_upsert_project(st, kNames[i], "/tmp/x");
            cbm_node_t n = {.project = kNames[i],
                            .label = "Function",
                            .name = "f",
                            .qualified_name = "x.f",
                            .file_path = "a.py",
                            .start_line = 1,
                            .end_line = 1};
            cbm_store_upsert_node(st, &n);
            cbm_store_close(st);
        }
    }

    char *resp = cbm_mcp_handle_tool(srv, "list_projects", "{\"offset\":1,\"limit\":2}");
    ASSERT_NOT_NULL(resp);
    const char *bravo = strstr(resp, "bravo");
    const char *mike = strstr(resp, "mike");
    bool sliced = bravo && mike && bravo < mike;      /* the middle two, in order */
    bool excluded_ends = !strstr(resp, "alpha") && !strstr(resp, "zeta");
    bool no_details_by_default = strstr(resp, "size_bytes") == NULL;
    free(resp);

    /* Control: details come back when asked for. */
    resp = cbm_mcp_handle_tool(srv, "list_projects", "{\"include_details\":true}");
    ASSERT_NOT_NULL(resp);
    bool details_on_request = strstr(resp, "size_bytes") != NULL;
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(cache);

    ASSERT_TRUE(sliced);
    ASSERT_TRUE(excluded_ends);
    ASSERT_TRUE(no_details_by_default);
    ASSERT_TRUE(details_on_request);
    PASS();
}

/* The prefilter is only safe for plain suffix globs. Moving an arbitrary
 * file_pattern ahead of Select-String is NOT results-preserving: the Windows
 * path applies PowerShell -like to the FULL MatchInfo.Path, while POSIX
 * delegates glob semantics to grep --include. These are the boundary cases that
 * separate "cannot change meaning" from "might".
 *
 * Runs on every platform — the predicate is pure. */
TEST(search_code_file_pattern_prefilter_boundaries) {
    ASSERT_TRUE(cbm_search_code_file_pattern_can_prefilter("*.pas"));
    ASSERT_TRUE(cbm_search_code_file_pattern_can_prefilter("*.PAS"));
    ASSERT_TRUE(cbm_search_code_file_pattern_can_prefilter("*.d.ts"));
    ASSERT_TRUE(cbm_search_code_file_pattern_can_prefilter("*.foo-bar_1"));

    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter(NULL));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter(""));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter(".pas"));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter("*.*"));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter("src/*.pas"));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter("src\\*.pas"));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter("*.c++"));
    ASSERT_FALSE(cbm_search_code_file_pattern_can_prefilter("*R&D*.go"));
    PASS();
}

/* Pins the PowerShell pipeline ORDERING without starting a shell: the cheap
 * path prefilter must run BEFORE the content scan, and the original post-scan
 * filter must survive as a second guard. Windows-only, because the command is
 * only built there. */
TEST(search_code_windows_prefilter_precedes_content_scan) {
#ifdef _WIN32
    char command[CBM_SZ_4K];
    cbm_search_code_build_grep_cmd(command, sizeof(command), false, true, "*.go", "C:/tmp/pattern",
                                   "C:/tmp/filelist", "C:/tmp/root");

    const char *prefilter = strstr(command, "Where-Object { $_ -like '*.go' }");
    const char *content_scan = strstr(command, "ForEach-Object { Select-String");
    const char *postfilter = strstr(command, "Where-Object { $_.Path -like '**.go' }");
    ASSERT_NOT_NULL(prefilter);
    ASSERT_NOT_NULL(content_scan);
    ASSERT_NOT_NULL(postfilter);
    ASSERT_TRUE(prefilter < content_scan);
    ASSERT_TRUE(content_scan < postfilter);

    /* A pattern with an interior wildcard must NOT be prefiltered. */
    cbm_search_code_build_grep_cmd(command, sizeof(command), false, true, "*handler*.go",
                                   "C:/tmp/pattern", "C:/tmp/filelist", "C:/tmp/root");
    ASSERT_NULL(strstr(command, "Where-Object { $_ -like '*handler*.go' }"));
    ASSERT_NOT_NULL(strstr(command, "Where-Object { $_.Path -like '**handler*.go' }"));
    PASS();
#else
    SKIP_PLATFORM("PowerShell prefilter runs on Windows");
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
 *  INDEX-FORMAT BOUNDARY  (#769)
 *
 *  File-node QNs keep the file extension, so an index written before that
 *  holds COLLIDED File identities (badge.component.{ts,html,scss} all stripped
 *  to one stem, only one node surviving). Refreshing such an index
 *  incrementally would mint new-format QNs for the changed files only and
 *  leave the old collided node behind — a mixed graph with duplicate nodes
 *  and stale edges.
 *
 *  CBM_INDEX_FORMAT_VERSION is stamped into PRAGMA user_version on every
 *  rebuild. A DB carrying a different value is routed through the full
 *  reindex exactly once (ADR preserved, per #516), and the rebuilt index must
 *  not force a second rebuild on the next unchanged run.
 * ══════════════════════════════════════════════════════════════════ */

enum { IFMT_LOG_BUF = 8192 };
static char g_ifmt_log[IFMT_LOG_BUF];
static size_t g_ifmt_log_len;

static void ifmt_capture_sink(const char *line) {
    size_t n = strlen(line);
    if (g_ifmt_log_len + n + 2 < sizeof(g_ifmt_log)) {
        memcpy(g_ifmt_log + g_ifmt_log_len, line, n);
        g_ifmt_log_len += n;
        g_ifmt_log[g_ifmt_log_len++] = '\n';
        g_ifmt_log[g_ifmt_log_len] = '\0';
    }
}

/* index_repository through the production MCP flow, capturing the routing log
 * (pipeline.route is the only place the decision is visible). */
static char *ifmt_index_capture(cbm_mcp_server_t *srv, const char *repo) {
    char args[512];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", repo);
    g_ifmt_log_len = 0;
    g_ifmt_log[0] = '\0';
    cbm_log_set_sink(ifmt_capture_sink);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    cbm_log_set_sink(NULL);
    return resp;
}

TEST(index_format_stale_db_rebuilds_once_issue769) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-format-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        FAIL("mkdtemp failed");
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-format-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        FAIL("mkdtemp cache failed");
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* Component siblings: the exact shape the extension-stripping QN collided. */
    char p_ts[512];
    char p_html[512];
    char p_scss[512];
    snprintf(p_ts, sizeof(p_ts), "%s/badge.component.ts", tmp_dir);
    snprintf(p_html, sizeof(p_html), "%s/badge.component.html", tmp_dir);
    snprintf(p_scss, sizeof(p_scss), "%s/badge.component.scss", tmp_dir);
    th_write_file(p_ts, "export class BadgeComponent {\n  hi() { return 1; }\n}\n");
    th_write_file(p_html, "<div class=\"badge\">hi</div>\n");
    th_write_file(p_scss, ".badge { color: red; }\n");

    char p_header[512];
    char p_source[512];
    snprintf(p_header, sizeof(p_header), "%s/upgrade.h", tmp_dir);
    snprintf(p_source, sizeof(p_source), "%s/upgrade.c", tmp_dir);
    th_write_file(p_header, "int upgrade_target(int value);\n");
    th_write_file(p_source, "int upgrade_target(int value) { return value; }\n");

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/%s.db", cache, project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Run 1: a fresh index stamps the current format. */
    char *resp = ifmt_index_capture(srv, tmp_dir);
    ASSERT_NOT_NULL(resp);
    free(resp);

    cbm_store_t *w = cbm_store_open_path(dbpath);
    ASSERT_NOT_NULL(w);
    int fmt = -1;
    ASSERT_EQ(cbm_store_get_format_version(w, &fmt), CBM_STORE_OK);
    ASSERT_EQ(fmt, CBM_INDEX_FORMAT_VERSION);

    /* Version 1 had no Declaration nodes. Leave file hashes unchanged so
     * only format invalidation can recover those nodes on the next run. */
    ASSERT_EQ(cbm_store_adr_store(w, project, "index-format-adr"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_delete_nodes_by_label(w, project, "Declaration"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_set_format_version(w, 1), CBM_STORE_OK);
    cbm_store_close(w);

    /* Run 2: stale format forces the full rebuild and surfaces the migration. */
    resp = ifmt_index_capture(srv, tmp_dir);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(g_ifmt_log, "format_change_reindex"));
    ASSERT_NOT_NULL(strstr(resp, "format_migration"));
    free(resp);

    cbm_store_t *r1 = cbm_store_open_path(dbpath);
    ASSERT_NOT_NULL(r1);
    fmt = -1;
    ASSERT_EQ(cbm_store_get_format_version(r1, &fmt), CBM_STORE_OK);
    ASSERT_EQ(fmt, CBM_INDEX_FORMAT_VERSION);
    cbm_node_t *upgraded = NULL;
    int upgraded_count = 0;
    ASSERT_EQ(
        cbm_store_find_nodes_by_name(r1, project, "upgrade_target", &upgraded, &upgraded_count),
        CBM_STORE_OK);
    int declarations = 0;
    int definitions = 0;
    for (int i = 0; i < upgraded_count; i++) {
        ASSERT_NOT_NULL(upgraded[i].properties_json);
        ASSERT_NOT_NULL(strstr(upgraded[i].properties_json, "declaration_key"));
        if (strcmp(upgraded[i].label, "Declaration") == 0) {
            declarations++;
        }
        if (strcmp(upgraded[i].label, "Function") == 0) {
            definitions++;
        }
    }
    ASSERT_EQ(declarations, 1);
    ASSERT_EQ(definitions, 1);
    cbm_store_free_nodes(upgraded, upgraded_count);
    /* #516: the forced rebuild deletes the DB, so the ADR must be carried. */
    cbm_adr_t adr = {0};
    ASSERT_EQ(cbm_store_adr_get(r1, project, &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_NOT_NULL(strstr(adr.content, "index-format-adr"));
    cbm_store_adr_free(&adr);
    cbm_store_close(r1);

    /* Run 3: unchanged and current — no second rebuild, no migration flag. */
    resp = ifmt_index_capture(srv, tmp_dir);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(g_ifmt_log, "format_change_reindex"));
    ASSERT_NULL(strstr(resp, "format_migration"));
    free(resp);

    cbm_mcp_server_free(srv);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    free(project);
    remove(p_ts);
    remove(p_html);
    remove(p_scss);
    remove(p_header);
    remove(p_source);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  EXIT_NONZERO QUARANTINE  (upstream #1438)
 *
 *  A worker that exits nonzero (an internal parse-limit abort on a
 *  pathological file) used to abort the WHOLE chunk, discarding every file
 *  indexed for that directory. It is now attributed through the same
 *  marker-journal suspect mechanism as a crash: the offender is quarantined
 *  under phase "error" and the rest of the chunk still indexes.
 *
 *  The two-consecutive-strikes intersection is what keeps this honest — a
 *  SYSTEMIC nonzero exit produces no recurring suspect, so the intersection
 *  is empty and the supervisor gives up instead of quarantining innocents.
 *  Both halves are pinned below.
 * ══════════════════════════════════════════════════════════════════ */

enum {
    IDXPAR_OK = 0,
    IDXPAR_ST_SPAWN = 61,       /* single-threaded recovery spawn happened (RED) */
    IDXPAR_NULL_RESP = 62,      /* supervised entry degraded to NULL */
    IDXPAR_NOT_INDEXED = 63,    /* response lacks status indexed */
    IDXPAR_NO_QUARANTINE = 64,  /* offender missing from skipped[] */
    IDXPAR_INNOCENT_HIT = 65,   /* a good file was quarantined/skipped */
    IDXPAR_GOOD_MISSING = 66,   /* good file's Function absent from the store */
    IDXPAR_NOT_ERROR = 67,      /* systemic failure did not report status error */
    IDXPAR_OUTCOME_WRONG = 68,  /* systemic failure outcome is not exit_nonzero */
};

#ifndef _WIN32
/* The supervised entry point returns the worker's MCP payload, in which the
 * JSON is carried as an ESCAPED string. Accept either form so the assertion
 * does not depend on which layer answered. */
static bool idxpar_response_has(const char *resp, const char *key, const char *value) {
    char plain[128];
    char escaped[256];
    snprintf(plain, sizeof(plain), "\"%s\":\"%s\"", key, value);
    snprintf(escaped, sizeof(escaped), "\\\"%s\\\":\\\"%s\\\"", key, value);
    return strstr(resp, plain) != NULL || strstr(resp, escaped) != NULL;
}

static int idxpar_exit_nonzero_recovery_check(const char *repo_dir) {
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    /* Rounds needed: fail+record, fail+quarantine, clean. Generous cap. */
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "5", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);
    cbm_setenv("CBM_TEST_EXIT_ON", "idxpar_exit_nonzero", 1);

    int st_before = cbm_index_supervisor_spawn_st_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int st_after = cbm_index_supervisor_spawn_st_count();
    cbm_unsetenv("CBM_TEST_EXIT_ON");

    if (st_after != st_before) {
        free(resp);
        return IDXPAR_ST_SPAWN;
    }
    if (!resp) {
        return IDXPAR_NULL_RESP;
    }
    bool indexed = idxpar_response_has(resp, "status", "indexed");
    bool offender_skipped = strstr(resp, "idxpar_exit_nonzero.py") != NULL;
    bool innocent_hit =
        strstr(resp, "idxpar_good_a.py") != NULL || strstr(resp, "idxpar_good_b.py") != NULL;
    bool phase_error =
        idxpar_response_has(resp, "phase", "error") || strstr(resp, "quarantined after error");
    free(resp);
    if (!indexed) {
        return IDXPAR_NOT_INDEXED;
    }
    if (!offender_skipped || !phase_error) {
        return IDXPAR_NO_QUARANTINE;
    }
    if (innocent_hit) {
        return IDXPAR_INNOCENT_HIT;
    }

    /* Store proof: an innocent's Function node exists. */
    char *project = cbm_project_name_from_path(repo_dir);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    int code = IDXPAR_OK;
    if (srv && project) {
        char q[512];
        snprintf(q, sizeof(q),
                 "{\"project\":\"%s\",\"name_pattern\":\"idxpar_good_fn\",\"label\":\"Function\"}",
                 project);
        char *sr = cbm_mcp_handle_tool(srv, "search_graph", q);
        if (!sr || !strstr(sr, "idxpar_good_fn")) {
            code = IDXPAR_GOOD_MISSING;
        }
        free(sr);
    }
    if (srv) {
        cbm_mcp_server_free(srv);
    }
    free(project);
    return code;
}

static int idxpar_systemic_exit_nonzero_give_up_check(const char *repo_dir) {
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "5", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);
    cbm_setenv("CBM_TEST_EXIT_ON", "idxpar_", 1); /* EVERY file exits nonzero */

    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    cbm_unsetenv("CBM_TEST_EXIT_ON");

    if (!resp) {
        return IDXPAR_NULL_RESP;
    }
    bool is_error = idxpar_response_has(resp, "status", "error");
    bool is_exit_nonzero = idxpar_response_has(resp, "outcome", "exit_nonzero");
    bool innocent_hit =
        strstr(resp, "idxpar_good_a.py") != NULL || strstr(resp, "idxpar_good_b.py") != NULL;
    free(resp);

    if (!is_error) {
        return IDXPAR_NOT_ERROR;
    }
    if (!is_exit_nonzero) {
        return IDXPAR_OUTCOME_WRONG;
    }
    if (innocent_hit) {
        return IDXPAR_INNOCENT_HIT;
    }
    return IDXPAR_OK;
}

/* Run one check in a forked child: it mutates process-global supervisor state
 * and env, and a fault-injected exit must not take the test runner with it. */
static int idxpar_run_child(int (*check)(const char *), const char *repo_dir, bool *signalled,
                            int *sig) {
    *signalled = false;
    *sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(120);
        _exit(check(repo_dir));
    }
    if (pid < 0) {
        return -1;
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        *signalled = true;
        *sig = WTERMSIG(status);
    }
    return -1;
}
#endif /* !_WIN32 */

TEST(index_recovery_quarantines_exit_nonzero) {
#ifdef _WIN32
    SKIP_PLATFORM("supervised-recovery guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idxpar-exit-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        FAIL("mkdtemp failed");
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idxpar-exit-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        FAIL("mkdtemp cache failed");
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char p1[512];
    char p2[512];
    char pc[512];
    snprintf(p1, sizeof(p1), "%s/idxpar_good_a.py", tmp_dir);
    snprintf(p2, sizeof(p2), "%s/idxpar_good_b.py", tmp_dir);
    snprintf(pc, sizeof(pc), "%s/idxpar_exit_nonzero.py", tmp_dir);
    th_write_file(p1, "def idxpar_good_fn():\n    return 'ok'\n");
    th_write_file(p2, "def idxpar_good_fn_b():\n    return 'ok'\n");
    th_write_file(pc, "def idxpar_bad_fn():\n    return 'exit'\n");

    bool signalled = false;
    int sig = 0;
    int code = idxpar_run_child(idxpar_exit_nonzero_recovery_check, tmp_dir, &signalled, &sig);

    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    remove(p1);
    remove(p2);
    remove(pc);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d\n", sig);
    } else if (code != IDXPAR_OK) {
        printf("    child exit code %d\n", code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDXPAR_OK);
    PASS();
#endif
}

TEST(index_recovery_systemic_exit_nonzero_gives_up) {
#ifdef _WIN32
    SKIP_PLATFORM("supervised-recovery guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idxpar-sys-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        FAIL("mkdtemp failed");
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idxpar-sys-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        FAIL("mkdtemp cache failed");
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char p1[512];
    char p2[512];
    snprintf(p1, sizeof(p1), "%s/idxpar_good_a.py", tmp_dir);
    snprintf(p2, sizeof(p2), "%s/idxpar_good_b.py", tmp_dir);
    th_write_file(p1, "def idxpar_good_fn():\n    return 'ok'\n");
    th_write_file(p2, "def idxpar_good_fn_b():\n    return 'ok'\n");

    bool signalled = false;
    int sig = 0;
    int code =
        idxpar_run_child(idxpar_systemic_exit_nonzero_give_up_check, tmp_dir, &signalled, &sig);

    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    remove(p1);
    remove(p2);
    th_rmtree(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d\n", sig);
    } else if (code != IDXPAR_OK) {
        printf("    child exit code %d\n", code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDXPAR_OK);
    PASS();
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  Agent-helpfulness changes: single-page catalog, evidence-carrying
 *  trace/inspect replies, byte budgets, cwd project resolution, hooks.
 * ══════════════════════════════════════════════════════════════════ */

/* Codex CLI never follows tools/list nextCursor; a page of 8 hid six tools. */
TEST(tools_list_is_one_page) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "nextCursor"));
    ASSERT_NOT_NULL(strstr(resp, "\"list_projects\""));
    ASSERT_NOT_NULL(strstr(resp, "\"detect_changes\""));
    ASSERT_NOT_NULL(strstr(resp, "\"inspect_symbol\""));
    ASSERT_NOT_NULL(strstr(resp, "\"ingest_traces\""));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Add a caller of ProcessOrder with the properties pass_calls writes. */
static void add_scored_call_edge(cbm_store_t *st, const char *caller_name, const char *caller_file,
                                 int caller_line, const char *props) {
    cbm_node_t *po = NULL;
    int po_n = 0;
    cbm_store_find_nodes_by_name(st, "test-project", "ProcessOrder", &po, &po_n);
    cbm_node_t n = {0};
    n.project = "test-project";
    n.label = "Function";
    n.name = caller_name;
    char qn[256];
    snprintf(qn, sizeof(qn), "test-project.x.%s", caller_name);
    n.qualified_name = qn;
    n.file_path = caller_file;
    n.start_line = caller_line;
    n.end_line = caller_line + 3;
    int64_t caller_id = cbm_store_upsert_node(st, &n);
    if (po_n > 0) {
        cbm_edge_t e = {.project = "test-project",
                        .source_id = caller_id,
                        .target_id = po[0].id,
                        .type = "CALLS",
                        .properties_json = props};
        cbm_store_insert_edge(st, &e);
    }
    cbm_store_free_nodes(po, po_n);
}

TEST(tool_trace_between_preserves_identity_and_filters) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    auto add_node = [&](const char *name, const char *qn, const char *file) {
        cbm_node_t n = {};
        n.project = "test-project";
        n.label = "Function";
        n.name = name;
        n.qualified_name = qn;
        n.file_path = file;
        n.start_line = 3;
        n.end_line = 5;
        return cbm_store_upsert_node(store, &n);
    };
    auto add_edge = [&](int64_t from, int64_t to, const char *props) {
        cbm_edge_t edge = {.project = "test-project", .source_id = from,
                           .target_id = to, .type = "CALLS", .properties_json = props};
        cbm_store_insert_edge(store, &edge);
    };
    int64_t entry = add_node("Entry", "test-project.Entry", "main.go");
    int64_t left = add_node("Bridge", "test-project.left.Bridge", "main.go");
    int64_t right = add_node("Bridge", "test-project.right.Bridge", "main.go");
    int64_t target = add_node("Destination", "test-project.Destination", "main.go");
    int64_t via = add_node("Via", "test-project.Via", "main.go");
    int64_t test = add_node("TestBridge", "test-project.TestBridge", "tests/test_graph.go");
    const char *high = "{\"confidence\":0.95,\"line\":3,\"strategy\":\"test\"}";
    add_edge(entry, left, high);
    add_edge(left, target, "{\"confidence\":0.2,\"line\":3}");
    add_edge(right, target, high);
    add_edge(entry, via, high);
    add_edge(via, test, high);
    add_edge(test, via, high); /* cycle must terminate */
    add_edge(test, target, high);
    auto query = [&](double confidence, bool tests, int depth, int budget) {
        char request[1024];
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
            "\"function_name\":\"Destination\",\"from_function\":\"Entry\","
            "\"direction\":\"inbound\",\"depth\":%d,\"min_confidence\":%.2f,"
            "\"include_tests\":%s,\"source_context\":1,\"max_bytes\":%d}}}",
            depth, confidence, tests ? "true" : "false", budget);
        char *response = cbm_mcp_server_handle(srv, request);
        char *text = extract_text_content(response);
        free(response);
        return text;
    };
    char *text = query(0, false, 6, 12000);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"path_found\":true"));
    ASSERT_NOT_NULL(strstr(text, "test-project.left.Bridge"));
    ASSERT_NULL(strstr(text, "test-project.right.Bridge"));
    ASSERT_NULL(strstr(text, "test-project.Via"));
    ASSERT_NOT_NULL(strstr(text, "\"from_step\":0,\"to_step\":1"));
    ASSERT_NOT_NULL(strstr(text, "\"source_start_line\":2"));
    free(text);
    text = query(0.8, false, 6, 12000);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"path_found\":false"));
    free(text);
    text = query(0.8, true, 6, 12000);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"path_found\":true"));
    ASSERT_NOT_NULL(strstr(text, "test-project.TestBridge"));
    ASSERT_NULL(strstr(text, "test-project.left.Bridge"));
    ASSERT_NULL(strstr(text, "test-project.right.Bridge"));
    free(text);
    text = query(0.8, true, 2, 12000);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"path_found\":false"));
    free(text);
    text = query(0, false, 6, 1500);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strlen(text) <= 1500);
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_trace_between_rejects_ambiguous_and_invalid_entries) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    const char *cases[] = {
        "\"from_function\":\"Run\",\"direction\":\"inbound\"",
        "\"from_function\":\"Missing\",\"direction\":\"inbound\"",
        "\"from_function\":\"\",\"direction\":\"inbound\"",
        "\"from_function\":null,\"direction\":\"inbound\"",
        "\"from_function\":\"HandleRequest\\u0000other\",\"direction\":\"inbound\"",
        "\"from_function\":\"HandleRequest\",\"direction\":\"both\"",
        "\"from_function\":\"HandleRequest\",\"direction\":\"inbound\",\"mode\":\"data_flow\"",
        "\"from_function\":\"HandleRequest\",\"direction\":\"inbound\",\"edge_types\":[\"HTTP_CALLS\"]",
        "\"from_function\":\"HandleRequest\",\"direction\":\"inbound\",\"max_work\":0",
        "\"from_function\":\"HandleRequest\",\"direction\":\"inbound\",\"max_work\":100001",
        "\"max_work\":100"
    };
    for (const char *args : cases) {
        char request[1024];
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
            "\"function_name\":\"ProcessOrder\",%s}}}", args);
        char *response = cbm_mcp_server_handle(srv, request);
        ASSERT_NOT_NULL(response);
        ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
        free(response);
    }
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_trace_path_current_source_and_budget) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    const char *request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
        "\"function_name\":\"HandleRequest\",\"direction\":\"outbound\","
        "\"source_lines\":2,\"max_bytes\":12000}}}";
    char *response = cbm_mcp_server_handle(srv, request);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "source_note"));
    ASSERT_NOT_NULL(strstr(response, "source_clipped"));
    ASSERT_NOT_NULL(strstr(response, "func ProcessOrder"));
    free(response);

    /* A stale graph must not serve cached source bytes. */
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp);
    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    /* Physical line numbers must survive lines longer than an I/O buffer. */
    for (int i = 0; i < 6000; i++) {
        fputc(' ', fp);
    }
    fputs("package main\n\nfunc HandleRequest() error {\nreturn nil\n}\n\n"
          "func Replacement() {\n// changed on disk\n}\n", fp);
    fclose(fp);
    response = cbm_mcp_server_handle(srv, request);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "func Replacement"));
    ASSERT_NULL(strstr(response, "func ProcessOrder"));
    free(response);

    add_scored_call_edge(cbm_mcp_server_store(srv), "ContextCaller", "main.go", 3,
                        "{\"callee\":\"ProcessOrder\",\"confidence\":1,\"line\":7}");
    response = cbm_mcp_server_handle(srv,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
        "\"function_name\":\"ProcessOrder\",\"direction\":\"inbound\",\"source_context\":1}}}");
    ASSERT_NOT_NULL(response);
    char *context_text = extract_text_content(response);
    ASSERT_NOT_NULL(context_text);
    ASSERT_NOT_NULL(strstr(context_text, "\"source_start_line\":6"));
    ASSERT_NOT_NULL(strstr(context_text, "func Replacement"));
    ASSERT_NOT_NULL(strstr(context_text, "changed on disk"));
    free(context_text);
    free(response);

    response = cbm_mcp_server_handle(srv,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
        "\"function_name\":\"HandleRequest\",\"source_lines\":500,\"max_bytes\":1500}}}");
    ASSERT_NOT_NULL(response);
    yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    const char *payload = yyjson_get_str(yyjson_obj_get(
        yyjson_arr_get(yyjson_obj_get(result, "content"), 0), "text"));
    ASSERT_NOT_NULL(payload);
    ASSERT(strlen(payload) <= 1500);
    yyjson_doc_free(doc);
    free(response);
    remove(path);
    response = cbm_mcp_server_handle(srv, request);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "source_unavailable"));
    ASSERT_NULL(strstr(response, "func Replacement"));
    free(response);
    for (int i = 0; i < 20; i++) {
        char name[64];
        snprintf(name, sizeof(name), "TraceBudgetCaller%d", i);
        add_scored_call_edge(cbm_mcp_server_store(srv), name, "main.go", 3,
                            "{\"callee\":\"ProcessOrder\",\"confidence\":1}");
    }
    response = cbm_mcp_server_handle(srv,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
        "\"function_name\":\"ProcessOrder\",\"direction\":\"inbound\",\"max_bytes\":1500}}}");
    ASSERT_NOT_NULL(response);
    char *bounded = extract_text_content(response);
    ASSERT_NOT_NULL(bounded);
    ASSERT(strlen(bounded) <= 1500);
    ASSERT_NOT_NULL(strstr(bounded, "trace exceeds max_bytes"));
    ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
    free(bounded);
    free(response);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_trace_path_carries_location_and_call_site) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    add_scored_call_edge(cbm_mcp_server_store(srv), "Dispatch", "svc/dispatch.go", 40,
                         "{\"callee\":\"ProcessOrder\",\"confidence\":0.42,"
                         "\"strategy\":\"unique_name\",\"line\":42}");

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
             "\"function_name\":\"ProcessOrder\",\"direction\":\"inbound\",\"depth\":1}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    /* Caller nodes carry their definition location... */
    ASSERT_NOT_NULL(strstr(inner, "\"file\":\"svc/dispatch.go\""));
    ASSERT_NOT_NULL(strstr(inner, "\"start_line\":40"));
    /* ...and edges carry the call site plus the resolver's real confidence
     * (the store used to hardcode 1.0). */
    ASSERT_NOT_NULL(strstr(inner, "\"from_file\":\"svc/dispatch.go\""));
    ASSERT_NOT_NULL(strstr(inner, "\"line\":42"));
    ASSERT_NOT_NULL(strstr(inner, "\"strategy\":\"unique_name\""));
    ASSERT_NOT_NULL(strstr(inner, "\"confidence\":0.42"));
    free(inner);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_trace_test_nodes_do_not_spend_result_budget) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    auto add = [&](const char *name, const char *file) {
        char qn[256];
        snprintf(qn, sizeof(qn), "test-project.budget.%s", name);
        cbm_node_t node = {.project = "test-project", .label = "Function", .name = name,
                          .qualified_name = qn, .file_path = file, .start_line = 1, .end_line = 3};
        return cbm_store_upsert_node(store, &node);
    };
    auto edge = [&](int64_t from, int64_t to) {
        cbm_edge_t e = {.project = "test-project", .source_id = from, .target_id = to,
                        .type = "CALLS", .properties_json = "{\"confidence\":0.38,\"line\":2}"};
        cbm_store_insert_edge(store, &e);
    };
    int64_t target = add("BudgetTarget", "src/target.c");
    for (int i = 0; i < 101; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "BudgetTest%d", i);
        edge(add(name, "Tests/callers.c"), target);
    }
    int64_t near_node = add("BudgetNear", "src/chain.c");
    int64_t middle = add("BudgetMiddle", "src/chain.c");
    int64_t start = add("BudgetStart", "src/chain.c");
    edge(near_node, target);
    edge(middle, near_node);
    edge(start, middle);
    for (bool include_tests : {false, true}) {
        char request[1024];
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
            "\"function_name\":\"BudgetTarget\",\"from_function\":\"BudgetStart\","
            "\"direction\":\"inbound\",\"depth\":3,\"include_tests\":%s}}}",
            include_tests ? "true" : "false");
        char *response = cbm_mcp_server_handle(srv, request);
        ASSERT_NOT_NULL(response);
        char *text = extract_text_content(response);
        ASSERT_NOT_NULL(text);
        ASSERT_NOT_NULL(strstr(text, "\"traversal_strategy\":\"targeted_forward_bfs\""));
        ASSERT_NOT_NULL(strstr(text, "\"path_found\":true"));
        ASSERT_NOT_NULL(strstr(text, "\"traversal_examined_edges\":3"));
        ASSERT_NOT_NULL(strstr(text, "\"traversal_visited_nodes\":4"));
        ASSERT_NOT_NULL(strstr(text, "\"traversal_truncated\":false"));
        free(text);
        free(response);
    }
    for (int max_work : {2, 3}) {
        char request[1024];
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
            "\"function_name\":\"BudgetTarget\",\"from_function\":\"BudgetStart\","
            "\"direction\":\"inbound\",\"depth\":3,\"max_work\":%d}}}", max_work);
        char *response = cbm_mcp_server_handle(srv, request);
        ASSERT_NOT_NULL(response);
        char *text = extract_text_content(response);
        ASSERT_NOT_NULL(text);
        ASSERT_NOT_NULL(strstr(text, max_work == 2 ? "\"path_found\":false"
                                                   : "\"path_found\":true"));
        ASSERT_NOT_NULL(strstr(text, max_work == 2 ? "\"traversal_truncated\":true"
                                                   : "\"traversal_truncated\":false"));
        char examined[64];
        snprintf(examined, sizeof(examined), "\"traversal_examined_edges\":%d", max_work);
        ASSERT_NOT_NULL(strstr(text, examined));
        free(text);
        free(response);
    }
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(store_bfs_result_and_examined_caps_are_truthful) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    cbm_node_t node = {.project = "test-project", .label = "Function", .name = "CapTarget",
                      .qualified_name = "test-project.CapTarget", .file_path = "src/target.c",
                      .start_line = 1, .end_line = 3};
    int64_t target = cbm_store_upsert_node(store, &node);
    for (int i = 0; i < 101; ++i) {
        char name[64], qn[128];
        snprintf(name, sizeof(name), "CapCaller%d", i);
        snprintf(qn, sizeof(qn), "test-project.%s", name);
        node.name = name;
        node.qualified_name = qn;
        node.file_path = i < 100 ? "tests/callers.c" : "src/caller.c";
        cbm_edge_t edge = {.project = "test-project", .source_id = cbm_store_upsert_node(store, &node),
                           .target_id = target, .type = "CALLS", .properties_json = "{}"};
        cbm_store_insert_edge(store, &edge);
    }
    const char *types[] = {"CALLS"};
    auto accept = [](const char *path, void *) { return strncmp(path, "tests/", 6) != 0; };
    cbm_traverse_result_t tr = {};
    ASSERT_EQ(cbm_store_bfs_filtered(store, target, "inbound", types, 1, 1, 100, 100,
                                     accept, nullptr, &tr), CBM_STORE_OK);
    ASSERT_EQ(tr.visited_count, 0);
    ASSERT_TRUE(tr.truncated); // scan budget exhausted by excluded nodes
    cbm_store_traverse_free(&tr);
    ASSERT_EQ(cbm_store_bfs_filtered(store, target, "inbound", types, 1, 1, 100, 101,
                                     accept, nullptr, &tr), CBM_STORE_OK);
    ASSERT_EQ(tr.visited_count, 1);
    ASSERT_EQ(tr.edge_count, 1);
    ASSERT_FALSE(tr.truncated); // exactly the examined budget, no extra row
    cbm_store_traverse_free(&tr);
    for (int cap : {100, 101}) {
        ASSERT_EQ(cbm_store_bfs(store, target, "inbound", types, 1, 1, cap, &tr), CBM_STORE_OK);
        ASSERT_EQ(tr.visited_count, cap);
        ASSERT_EQ(tr.edge_count, cap);
        ASSERT_EQ(tr.truncated, cap == 100);
        cbm_store_traverse_free(&tr);
        ASSERT_EQ(cbm_store_bfs_trail(store, target, "inbound", types, 1, 1, 1, cap, &tr), CBM_STORE_OK);
        ASSERT_EQ(tr.visited_count, cap);
        ASSERT_EQ(tr.truncated, cap == 100);
        cbm_store_traverse_free(&tr);
    }
    ASSERT_EQ(cbm_store_bfs(store, target, "inbound", types, 1, 1, 0, &tr), CBM_STORE_OK);
    ASSERT_EQ(tr.visited_count, 0);
    ASSERT_TRUE(tr.truncated);
    cbm_store_traverse_free(&tr);
    ASSERT_EQ(cbm_store_bfs_filtered(store, target, "inbound", types, 1, 1, 100, 0,
                                     accept, nullptr, &tr), CBM_STORE_ERR);
    cbm_store_traverse_free(&tr);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(store_bfs_null_filter_still_enforces_examined_cap) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    cbm_node_t node = {.project = "test-project", .label = "Function", .name = "NullFilterTarget",
                      .qualified_name = "test-project.NullFilterTarget", .file_path = "src/target.c",
                      .start_line = 1, .end_line = 3};
    int64_t target = cbm_store_upsert_node(store, &node);
    int64_t first_caller = 0;
    const char *types[] = {"CALLS"};
    for (int i = 0; i < 3; ++i) {
        char name[64], qn[128];
        snprintf(name, sizeof(name), "NullFilterCaller%d", i);
        snprintf(qn, sizeof(qn), "test-project.%s", name);
        node.name = name;
        node.qualified_name = qn;
        int64_t caller = cbm_store_upsert_node(store, &node);
        if (i == 0) {
            first_caller = caller;
        }
        cbm_edge_t edge = {.project = "test-project", .source_id = caller, .target_id = target,
                           .type = "CALLS", .properties_json = "{}"};
        cbm_store_insert_edge(store, &edge);
        if (i == 0 || i == 2) {
            cbm_traverse_result_t tr = {};
            ASSERT_EQ(cbm_store_bfs_filtered(store, target, "inbound", types, 1, 1, 3, 1,
                                             nullptr, nullptr, &tr), CBM_STORE_OK);
            ASSERT_EQ(tr.visited_count, 1);
            ASSERT_EQ(tr.edge_count, 1);
            ASSERT_EQ(tr.visited[0].node.id, first_caller);
            ASSERT_EQ(tr.edges[0].source_id, first_caller);
            ASSERT_EQ(tr.truncated, i == 2);
            cbm_store_traverse_free(&tr);
        }
    }
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_trace_path_test_filter_is_case_insensitive) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    /* "Tests/" (capital T, as in PcapPlusPlus) must count as a test path. */
    add_scored_call_edge(cbm_mcp_server_store(srv), "SuiteCaller", "Tests/Orders/Suite.go", 10,
                         "{\"confidence\":0.9,\"strategy\":\"lsp_direct\",\"line\":11}");

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
             "\"function_name\":\"ProcessOrder\",\"direction\":\"inbound\",\"depth\":1}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "SuiteCaller")); /* filtered from callers AND caller_edges */
    free(resp);

    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
             "\"function_name\":\"ProcessOrder\",\"direction\":\"inbound\",\"depth\":1,"
             "\"include_tests\":true}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "SuiteCaller"));
    ASSERT_NOT_NULL(strstr(resp, "\"is_test\":true"));
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

static int64_t inspect_test_node(cbm_store_t *store, const char *label, const char *name,
                                 const char *qn, const char *file, int line, const char *props) {
    cbm_node_t node = {};
    node.project = "test-project";
    node.label = label;
    node.name = name;
    node.qualified_name = qn;
    node.file_path = file;
    node.start_line = line;
    node.end_line = line;
    node.properties_json = props;
    return cbm_store_upsert_node(store, &node);
}

static char *inspect_test_call(cbm_mcp_server_t *srv, const char *args) {
    char request[16384];
    snprintf(request, sizeof(request),
             "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"inspect_symbol\",\"arguments\":%s}}",
             args);
    char *response = cbm_mcp_server_handle(srv, request);
    char *text = response ? extract_text_content(response) : nullptr;
    free(response);
    return text;
}

TEST(tool_inspect_symbol_uses_graph_declarations) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    const char *key = "{\"declaration_key\":\"scope-int\"}";
    inspect_test_node(store, "Function", "target", "test-project.engine.target", "engine.c", 20,
                      key);
    // None of these files exist: locations must come from graph data alone.
    inspect_test_node(store, "Declaration", "target", "test-project.api.__decl_1.target",
                      "api/Public.h", 3, key);
    inspect_test_node(store, "Declaration", "target", "test-project.api.__decl_2.target",
                      "api/Public.h", 3, key);
    inspect_test_node(store, "Declaration", "target", "test-project.api.__decl_3.target",
                      "api/Wrong.h", 8, "{\"declaration_key\":\"other-scope-int\"}");
    inspect_test_node(store, "Declaration", "target", "test-project.api.__decl_4.target",
                      "api/Overload.h", 9, "{\"declaration_key\":\"scope-double\"}");
    char *text = inspect_test_call(
        srv, "{\"project\":\"test-project\",\"symbol\":\"target\",\"source_lines\":0}");
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":1"));
    ASSERT_NOT_NULL(strstr(text, "\"file\":\"api/Public.h\",\"line\":3"));
    ASSERT_NULL(strstr(text, "api/Wrong.h"));
    ASSERT_NULL(strstr(text, "api/Overload.h"));
    ASSERT_NULL(strstr(text, "also_defined_as"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_overload_family_labels_scope_and_declarations) {
    char tmp[256];
    auto *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    auto *store = cbm_mcp_server_store(srv);
    auto family = inspect_test_node(store, "OverloadSet", "target", "test-project.api.target", "api.cpp", 0, "{}");
    auto caller = inspect_test_node(store, "Function", "caller", "test-project.use.caller", "use.cpp", 1, "{}");
    cbm_edge_t edge = {.project = "test-project", .source_id = caller, .target_id = family, .type = "CALLS"};
    ASSERT_GT(cbm_store_insert_edge(store, &edge), 0);
    inspect_test_node(store, "Function", "target", "test-project.api.target@overload_10",
                      "api.cpp", 3, "{\"declaration_key\":\"int\",\"signature\":\"(int x)\"}");
    inspect_test_node(store, "Function", "target", "test-project.api.target@overload_50",
                      "api.cpp", 4, "{\"declaration_key\":\"double\",\"signature\":\"(double x)\"}");
    inspect_test_node(store, "Declaration", "target", "test-project.header.__decl_1.target",
                      "api.h", 1, "{\"declaration_key\":\"int\"}");
    inspect_test_node(store, "Declaration", "target", "test-project.header.__decl_2.target",
                      "api.h", 2, "{\"declaration_key\":\"double\"}");
    char *text = inspect_test_call(srv,
        "{\"project\":\"test-project\",\"symbol\":\"target\",\"source_lines\":0,\"max_bytes\":2000}");
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 2000);
    ASSERT_NOT_NULL(strstr(text, "\"relationship_scope\":\"overload_family\""));
    ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":2"));
    ASSERT_NOT_NULL(strstr(text, "also_defined_as"));
    ASSERT_NOT_NULL(strstr(text, "use.cpp"));
    free(text);
    text = inspect_test_call(srv,
        "{\"project\":\"test-project\",\"symbol\":\"test-project.api.target\",\"source_lines\":0}");
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "\"relationship_scope\":\"overload_family\""));
    ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":2"));
    free(text);
    text = inspect_test_call(srv,
        "{\"project\":\"test-project\",\"symbol\":\"test-project.api.target@overload_10\",\"source_lines\":0}");
    ASSERT_NOT_NULL(text);
    ASSERT_NULL(strstr(text, "relationship_scope"));
    ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":1"));
    free(text);
    inspect_test_node(store, "Function", "target", "test-project.other.target@overload_1",
                      "other.cpp", 1, "{}");
    text = inspect_test_call(srv, "{\"project\":\"test-project\",\"symbol\":\"target\"}");
    ASSERT_NOT_NULL(text);
    ASSERT_NULL(strstr(text, "relationship_scope"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_declarations_page_with_budget) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    const char *key = "{\"declaration_key\":\"repeated-int\"}";
    inspect_test_node(store, "Function", "repeated", "test-project.engine.repeated", "engine.c", 1,
                      key);
    for (int i = 23; i >= 0; i--) {
        char qn[128], file[128];
        snprintf(qn, sizeof(qn), "test-project.api.__decl_%02d.repeated", i);
        snprintf(file, sizeof(file), "api/copy%02d.h", i);
        inspect_test_node(store, "Declaration", "repeated", qn, file, 1, key);
    }
    int offset = 0;
    while (offset < 24) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"test-project\",\"symbol\":\"repeated\","
                 "\"source_lines\":0,\"max_bytes\":1500,\"declarations_offset\":%d}",
                 offset);
        char *text = inspect_test_call(srv, args);
        ASSERT_NOT_NULL(text);
        ASSERT(strlen(text) <= 1500);
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        ASSERT_NOT_NULL(doc);
        yyjson_val *root = yyjson_doc_get_root(doc);
        ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "declared_in_total")), 24);
        int shown = (int)yyjson_get_int(yyjson_obj_get(root, "declared_in_shown"));
        ASSERT(shown > 0);
        yyjson_val *decls = yyjson_obj_get(root, "declared_in");
        ASSERT_EQ(yyjson_arr_size(decls), shown);
        for (int i = 0; i < shown; i++) {
            char expected[128];
            snprintf(expected, sizeof(expected), "api/copy%02d.h", offset + i);
            ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(decls, (size_t)i), "file")),
                          expected);
        }
        offset += shown;
        ASSERT_EQ(yyjson_get_bool(yyjson_obj_get(root, "declared_in_has_more")), offset < 24);
        yyjson_doc_free(doc);
        free(text);
    }
    ASSERT_EQ(offset, 24);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_header_only_equivalent_declarations) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    const char *key = "{\"declaration_key\":\"scope-int\"}";
    inspect_test_node(store, "Declaration", "api_only", "test-project.a.__decl_1.ns.api_only",
                      "a.h", 1, key);
    inspect_test_node(store, "Declaration", "api_only", "test-project.b.__decl_1.ns.api_only",
                      "b.h", 1, key);
    for (const char *symbol : {"api_only", "ns::api_only", "test-project.a.__decl_1.ns.api_only"}) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"test-project\",\"symbol\":\"%s\",\"source_lines\":0}", symbol);
        char *text = inspect_test_call(srv, args);
        ASSERT_NOT_NULL(text);
        ASSERT_NULL(strstr(text, "\"error\""));
        ASSERT_NOT_NULL(strstr(text, "\"label\":\"Declaration\""));
        ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":2"));
        free(text);
    }
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_scoped_declaration_finds_callers) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    const char *key = "{\"declaration_key\":\"ns-target-int\"}";
    inspect_test_node(store, "Declaration", "target", "test-project.api.__decl_1.ns.target",
                      "api.h", 1, key);
    int64_t target = inspect_test_node(store, "Function", "target", "test-project.engine.target",
                                       "engine.c", 5, key);
    int64_t caller = inspect_test_node(store, "Function", "caller", "test-project.user.caller",
                                       "user.c", 9, "{}");
    cbm_edge_t edge = {.project = "test-project",
                       .source_id = caller,
                       .target_id = target,
                       .type = "CALLS",
                       .properties_json = "{\"line\":10,\"confidence\":1}"};
    cbm_store_insert_edge(store, &edge);
    // A different lexical scope must never enter this join.
    inspect_test_node(store, "Function", "target", "test-project.other.target", "other.c", 5,
                      "{\"declaration_key\":\"other-target-int\"}");
    for (const char *symbol : {"ns::target", "test-project.api.__decl_1.ns.target"}) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"test-project\",\"symbol\":\"%s\",\"source_lines\":0}", symbol);
        char *text = inspect_test_call(srv, args);
        ASSERT_NOT_NULL(text);
        ASSERT_NULL(strstr(text, "\"error\""));
        ASSERT_NOT_NULL(strstr(text, "\"label\":\"Function\""));
        ASSERT_NOT_NULL(strstr(text, "\"file\":\"engine.c\""));
        ASSERT_NOT_NULL(strstr(text, "\"callers_total\":1"));
        ASSERT_NOT_NULL(strstr(text, "\"file\":\"user.c\""));
        ASSERT_NULL(strstr(text, "other.c"));
        free(text);
    }
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_unions_calls_to_declaration_and_definition) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    const char *key = "{\"declaration_key\":\"ns-target-int\"}";
    int64_t declaration = inspect_test_node(store, "Declaration", "target",
        "test-project.api.__decl_1.ns.target", "api.h", 1, key);
    int64_t definition = inspect_test_node(store, "Function", "target",
        "test-project.engine.target", "engine.c", 5, key);
    int64_t caller = inspect_test_node(store, "Function", "caller", "test-project.user.caller",
                                       "user.c", 9, "{}");
    cbm_edge_t edge = {.project = "test-project", .source_id = caller,
        .target_id = declaration, .type = "CALLS",
        .properties_json = "{\"line\":30,\"call_lines\":[20,30],\"confidence\":1}"};
    cbm_store_insert_edge(store, &edge);
    edge.target_id = definition;
    edge.properties_json = "{\"line\":40,\"call_lines\":[20,40],\"confidence\":1}";
    cbm_store_insert_edge(store, &edge);
    char *text = inspect_test_call(srv,
        "{\"project\":\"test-project\",\"symbol\":\"ns::target\",\"max_bytes\":8000}");
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 8000);
    ASSERT_NULL(strstr(text, "\"error\""));
    ASSERT_NOT_NULL(strstr(text, "\"call_lines\":[20,30,40]"));
    ASSERT_NOT_NULL(strstr(text, "\"file\":\"user.c\",\"call_sites\":3"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_caller_file_pages_are_complete) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    for (int i = 300; i >= 0; i--) {
        char name[64], file[128];
        snprintf(name, sizeof(name), "Caller%03d", i);
        snprintf(file, sizeof(file), "users/call_%03d.c", i);
        add_scored_call_edge(store, name, file, 1, "{\"confidence\":1,\"line\":2}");
    }
    add_scored_call_edge(store, "ExtraCaller", "users/call_000.c", 5,
                         "{\"confidence\":1,\"line\":6}");
    int retrieved = 0;
    for (int offset : {0, 200}) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"test-project\",\"symbol\":\"ProcessOrder\","
                 "\"source_lines\":0,\"callers_limit\":0,\"callees_limit\":0,"
                 "\"max_bytes\":100000,\"caller_files_offset\":%d}",
                 offset);
        char *text = inspect_test_call(srv, args);
        ASSERT_NOT_NULL(text);
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        ASSERT_NOT_NULL(doc);
        yyjson_val *root = yyjson_doc_get_root(doc);
        ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "caller_files_total")), 302);
        yyjson_val *files = yyjson_obj_get(root, "caller_files");
        int shown = (int)yyjson_arr_size(files);
        ASSERT_EQ(shown, offset == 0 ? 200 : 102);
        ASSERT_EQ(yyjson_get_int(yyjson_obj_get(root, "caller_files_shown")), shown);
        ASSERT_EQ(yyjson_get_bool(yyjson_obj_get(root, "caller_files_has_more")), offset == 0);
        for (int i = 0; i < shown; i++) {
            char expected[128];
            if (offset + i == 0) {
                snprintf(expected, sizeof(expected), "main.go");
            } else {
                snprintf(expected, sizeof(expected), "users/call_%03d.c", offset + i - 1);
            }
            yyjson_val *entry = yyjson_arr_get(files, (size_t)i);
            ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(entry, "file")), expected);
            if (offset + i == 1) {
                ASSERT_EQ(yyjson_get_int(yyjson_obj_get(entry, "call_sites")), 2);
            }
        }
        retrieved += shown;
        yyjson_doc_free(doc);
        free(text);
    }
    ASSERT_EQ(retrieved, 302);
    char *text =
        inspect_test_call(srv, "{\"project\":\"test-project\",\"symbol\":\"ProcessOrder\","
                               "\"source_lines\":0,\"callers_limit\":0,\"max_bytes\":1500}");
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 1500);
    ASSERT_NOT_NULL(strstr(text, "\"caller_files_total\":302"));
    ASSERT_NOT_NULL(strstr(text, "\"caller_files_has_more\":true"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_metadata_and_errors_obey_byte_budget) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    char name[3000];
    memset(name, 'x', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    inspect_test_node(cbm_mcp_server_store(srv), "Function", name, "test-project.long", "long.c", 1,
                      "{}");
    char args[4000];
    snprintf(args, sizeof(args),
             "{\"project\":\"test-project\",\"symbol\":\"%s\",\"max_bytes\":1500}", name);
    char *text = inspect_test_call(srv, args);
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 1500);
    ASSERT_NOT_NULL(strstr(text, "\"metadata_omitted\":true"));
    ASSERT_NOT_NULL(strstr(text, "\"caller_files_total\":0"));
    ASSERT_NOT_NULL(strstr(text, "\"declared_in_total\":0"));
    free(text);
    name[0] = 'y'; // Missing symbol errors must not echo an unbounded name.
    snprintf(args, sizeof(args),
             "{\"project\":\"test-project\",\"symbol\":\"%s\",\"max_bytes\":1500}", name);
    text = inspect_test_call(srv, args);
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 1500);
    ASSERT_NOT_NULL(strstr(text, "\"error\""));
    ASSERT_NOT_NULL(strstr(text, "\"truncated\":true"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_one_call) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    add_scored_call_edge(cbm_mcp_server_store(srv), "Dispatch", "svc/dispatch.go", 40,
                         "{\"confidence\":0.42,\"strategy\":\"unique_name\",\"line\":42}");
    add_scored_call_edge(cbm_mcp_server_store(srv), "SuiteCaller", "tests/suite_test.go", 10,
                         "{\"confidence\":0.9,\"strategy\":\"lsp_direct\",\"line\":11}");

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"inspect_symbol\",\"arguments\":{\"project\":\"test-project\","
             "\"symbol\":\"ProcessOrder\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    char *inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    /* Definition + source head from the fixture file. */
    ASSERT_NOT_NULL(strstr(inner, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(inner, "func ProcessOrder(id int)"));
    /* Production callers (HandleRequest, Dispatch) vs the test caller. */
    ASSERT_NOT_NULL(strstr(inner, "\"callers_total\":2"));
    ASSERT_NOT_NULL(strstr(inner, "\"related_tests_total\":1"));
    ASSERT_NOT_NULL(strstr(inner, "\"call_lines\":[42]"));
    ASSERT_NOT_NULL(strstr(inner, "\"strategy\":\"unique_name\""));
    /* The complete file rollup names every file, tests flagged. */
    ASSERT_NOT_NULL(strstr(inner, "\"caller_files\""));
    ASSERT_NOT_NULL(strstr(inner, "\"file\":\"tests/suite_test.go\",\"call_sites\":1,\"test\":true"));
    ASSERT_NOT_NULL(strstr(inner, "\"callees_total\":0"));
    ASSERT_NOT_NULL(strstr(inner, "\"file_modified_after_index\""));
    free(inner);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_reports_bounded_distinct_call_lines) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    add_scored_call_edge(cbm_mcp_server_store(srv), "Dispatch", "svc/dispatch.go", 40,
                         "{\"confidence\":0.95,\"strategy\":\"lsp_direct\",\"line\":42,"
                         "\"call_lines\":[50,42,41,43,44,45,46,47,48,49,42,0,-1,1.5,\"3\"]}");
    char *text = inspect_test_call(srv,
        "{\"project\":\"test-project\",\"symbol\":\"ProcessOrder\",\"max_bytes\":8000}");
    ASSERT_NOT_NULL(text);
    ASSERT(strlen(text) <= 8000);
    ASSERT_NOT_NULL(strstr(text, "\"call_lines\":[41,42,43,44,45,46,47,48]"));
    ASSERT_NOT_NULL(strstr(text, "\"call_sites_total\":10"));
    ASSERT_NOT_NULL(strstr(text, "\"file\":\"svc/dispatch.go\",\"call_sites\":10"));
    free(text);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_accepts_scoped_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    cbm_node_t m = {0};
    m.project = "test-project";
    m.label = "Method";
    m.name = "Save";
    m.qualified_name = "test-project.pkg.order.Order.Save";
    m.file_path = "main.go";
    m.start_line = 11;
    m.end_line = 13;
    cbm_store_upsert_node(st, &m);

    /* C++-style scoping with more namespace than the graph keeps. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"inspect_symbol\",\"arguments\":{\"project\":\"test-project\","
             "\"symbol\":\"pkg::order::Order::Save\",\"source_lines\":0}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "test-project.pkg.order.Order.Save"));
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* trace_path takes the same path. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"trace_path\",\"arguments\":{\"project\":\"test-project\","
             "\"function_name\":\"Order::Save\",\"direction\":\"both\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "function not found"));
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_inspect_symbol_ambiguous_returns_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    /* Two same-tier "Run" definitions with equal span: a genuine tie. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"inspect_symbol\",\"arguments\":{\"project\":\"test-project\","
             "\"symbol\":\"Run\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "ambiguous"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_search_code_max_bytes_truncates_with_continuation) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_code\",\"arguments\":{\"project\":\"test-project\","
             "\"pattern\":\"func\",\"mode\":\"full\",\"limit\":10,\"max_bytes\":300}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    free(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"truncated\":true"));
    ASSERT_NOT_NULL(strstr(inner, "\"continuation\""));
    /* Totals stay exact even when the list is cut. */
    ASSERT_NOT_NULL(strstr(inner, "\"total_results\":"));
    free(inner);

    /* Budget off: nothing is cut. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_code\",\"arguments\":{\"project\":\"test-project\","
             "\"pattern\":\"func\",\"mode\":\"full\",\"max_bytes\":0}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "\"truncated\":true"));
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* A missing project resolves from cwd; an unknown one is answered for the
 * cwd project with a project_note (10-12% of graph calls failed on guessed
 * names in two agent studies). */
TEST(tool_project_resolves_from_cwd) {
#ifdef _WIN32
    /* chdir/realpath-driven; the resolution code is exercised on POSIX CI. */
    PASS();
#else
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm_cwd_cache_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));
    char repo[256];
    snprintf(repo, sizeof(repo), "/tmp/cbm_cwd_repo_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(repo));
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char *pname = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(pname);
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, pname);
    cbm_store_t *st = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, pname, repo);
    cbm_node_t n = {0};
    n.project = pname;
    n.label = "Function";
    n.name = "CwdProbe";
    n.qualified_name = "x.CwdProbe";
    n.file_path = "a.go";
    n.start_line = 1;
    n.end_line = 2;
    cbm_store_upsert_node(st, &n);
    cbm_store_close(st);

    char old_cwd[1024];
    ASSERT_NOT_NULL(getcwd(old_cwd, sizeof(old_cwd)));
    ASSERT_EQ(chdir(repo), 0);
    cbm_mcp_reset_cwd_project_cache();
    ASSERT_NOT_NULL(cbm_mcp_cwd_project());
    ASSERT_STR_EQ(cbm_mcp_cwd_project(), pname);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    /* 1. No project argument at all. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"name_pattern\":\"^CwdProbe$\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"CwdProbe\""));
    ASSERT_NULL(strstr(resp, "project_note"));
    free(resp);
    /* 2. A guessed name that exists nowhere: answered, and it says so. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"project\":\"totally-wrong\","
             "\"name_pattern\":\"^CwdProbe$\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"CwdProbe\""));
    ASSERT_NOT_NULL(strstr(resp, "project_note"));
    ASSERT_NOT_NULL(strstr(resp, "totally-wrong"));
    free(resp);
    /* 3. Destructive tools never substitute. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"delete_project\",\"arguments\":{\"project\":\"totally-wrong\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "project_note"));
    free(resp);
    /* 4. An explicit path is never second-guessed either. */
    char path_req[700];
    snprintf(path_req, sizeof(path_req),
             "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"search_graph\",\"arguments\":{\"project\":\"%s/nowhere\","
             "\"name_pattern\":\"^CwdProbe$\"}}}",
             repo);
    resp = cbm_mcp_server_handle(srv, path_req);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "project_note"));
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);

    /* 5. A project indexed under a CUSTOM name resolves through the memo's
     * root_path (the path-derived name matches no file). */
    char repo2[256];
    snprintf(repo2, sizeof(repo2), "/tmp/cbm_cwd_custom_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(repo2));
    char db2[512];
    snprintf(db2, sizeof(db2), "%s/custom-proj.db", cache);
    cbm_store_t *st2 = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(st2);
    cbm_store_upsert_project(st2, "custom-proj", repo2);
    cbm_store_close(st2);
    cbm_store_meta_db_t *meta = cbm_store_meta_open();
    ASSERT_NOT_NULL(meta);
    cbm_file_gen_t gen2;
    ASSERT_TRUE(cbm_file_generation(db2, &gen2));
    cbm_store_meta_row_t mrow;
    cbm_store_meta_row_init(&mrow, db2, &gen2);
    snprintf(mrow.project, sizeof(mrow.project), "custom-proj");
    /* index_repository records the CANONICAL root (realpath); getcwd returns
     * the same form (/tmp is a symlink to /private/tmp on macOS). */
    char *repo2_real = realpath(repo2, NULL);
    ASSERT_NOT_NULL(repo2_real);
    snprintf(mrow.root_path, sizeof(mrow.root_path), "%s", repo2_real);
    free(repo2_real);
    ASSERT_TRUE(cbm_store_meta_put(meta, &mrow));
    cbm_store_meta_close(meta);
    ASSERT_EQ(chdir(repo2), 0);
    cbm_mcp_reset_cwd_project_cache();
    ASSERT_NOT_NULL(cbm_mcp_cwd_project());
    ASSERT_STR_EQ(cbm_mcp_cwd_project(), "custom-proj");
    cbm_unlink(db2);
    cbm_rmdir(repo2);

    ASSERT_EQ(chdir(old_cwd), 0);
    cbm_mcp_reset_cwd_project_cache();
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    cbm_unlink(db_path);
    char config_db[512];
    snprintf(config_db, sizeof(config_db), "%s/_config.db", cache);
    cbm_unlink(config_db);
    cbm_rmdir(cache);
    cbm_rmdir(repo);
    free(pname);
    PASS();
#endif
}

TEST(hook_edit_impact_note) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);
    add_scored_call_edge(cbm_mcp_server_store(srv), "Dispatch", "svc/dispatch.go", 40,
                         "{\"confidence\":0.42,\"strategy\":\"unique_name\",\"line\":42}");
    add_scored_call_edge(cbm_mcp_server_store(srv), "SuiteCaller", "tests/suite_test.go", 10,
                         "{\"line\":11}");

    bool resolved = false;
    char *note = cbm_mcp_edit_impact_note(srv, "test-project", "main.go", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NOT_NULL(note);
    /* Callers inside the edited file (HandleRequest -> ProcessOrder) are not
     * impact; the two external callers are, one of them a test. */
    ASSERT_NOT_NULL(strstr(note, "main.go defines"));
    ASSERT_NOT_NULL(strstr(note, "Direct callers outside this file: 2 in 2 file(s), 1 in tests"));
    ASSERT_NOT_NULL(strstr(note, "ProcessOrder <- 2"));
    free(note);

    /* Unknown project: not resolved, no note. */
    resolved = true;
    note = cbm_mcp_edit_impact_note(srv, "no-such-project", "main.go", &resolved);
    ASSERT_NULL(note);
    ASSERT_FALSE(resolved);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

#include <watcher/watcher.h>
#include <sqlite3.h>
#include <string>

struct deletion_cache_env {
    char *old = getenv("CBM_CACHE_DIR") ? strdup(getenv("CBM_CACHE_DIR")) : NULL;
    explicit deletion_cache_env(const char *path) { cbm_setenv("CBM_CACHE_DIR", path, 1); }
    ~deletion_cache_env() {
        if (old) { cbm_setenv("CBM_CACHE_DIR", old, 1); free(old); }
        else { cbm_unsetenv("CBM_CACHE_DIR"); }
    }
};
static std::string deletion_file_bytes(const std::string& path) {
    FILE *file = cbm_fopen(path.c_str(), "rb");
    if (!file) { return "<missing>"; }
    std::string result;
    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) != 0) { result.append(chunk, n); }
    fclose(file);
    return result;
}

TEST(mcp_delete_busy_preserves_explicit_target) {
    char root[256] = "/tmp/cbm_delete_lease_XXXXXX";
    ASSERT(cbm_mkdtemp(root));
    deletion_cache_env env(root);
    std::string db = std::string(root) + "/explicit-target.db";
    const char *suffixes[] = {"", "-wal", "-shm"};
    const char *values[] = {"keep db", "keep wal", "keep shm"};
    for (int i = 0; i < 3; ++i) { ASSERT_EQ(th_write_file((db + suffixes[i]).c_str(), values[i]), 0); }
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(db.c_str(), &lease), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_watcher_t *watcher = cbm_watcher_new(NULL, NULL, NULL);
    cbm_watcher_watch(watcher, "explicit-target", root);
    cbm_mcp_server_set_watcher(srv, watcher);
    char *result = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"explicit-target\"}");
    ASSERT(cbm_mcp_result_is_index_busy(result));
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    ASSERT(doc);
    yyjson_val *payload = yyjson_obj_get(yyjson_doc_get_root(doc), "structuredContent");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(payload, "project")), "explicit-target");
    yyjson_doc_free(doc);
    free(result);
    ASSERT_EQ(cbm_watcher_watch_count(watcher), 1);
    for (int i = 0; i < 3; ++i) { ASSERT(deletion_file_bytes(db + suffixes[i]) == values[i]); }
    // Wrong explicit project must never substitute the watched/current target.
    result = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"wrong-target\"}");
    ASSERT(result && strstr(result, "not_found"));
    free(result);
    ASSERT(deletion_file_bytes(db) == values[0]);
    cbm_db_lease_release(lease);
    result = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"explicit-target\"}");
    ASSERT(result && strstr(result, "deleted"));
    free(result);
    for (const char *suffix : suffixes) { ASSERT(!cbm_is_regular_file((db + suffix).c_str())); }
    ASSERT(cbm_is_regular_file((db + ".index.lock").c_str()));
    ASSERT_EQ(cbm_watcher_watch_count(watcher), 0);
    cbm_mcp_server_set_watcher(srv, NULL);
    cbm_mcp_server_free(srv);
    cbm_watcher_free(watcher);
    th_rmtree(root);
    PASS();
}

TEST(mcp_corrupt_quarantine_busy_preserves_generation) {
    char root[256] = "/tmp/cbm_repair_lease_XXXXXX";
    ASSERT(cbm_mkdtemp(root));
    deletion_cache_env env(root);
    std::string db = std::string(root) + "/damaged.db";
    sqlite3 *raw = NULL;
    ASSERT_EQ(sqlite3_open(db.c_str(), &raw), SQLITE_OK);
    // Valid SQLite bytes with a confirmed invalid project root (not a transient schema error).
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE projects(root_path TEXT); INSERT INTO projects VALUES('1invalid');", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw);
    ASSERT_EQ(th_write_file((db + "-wal").c_str(), "keep wal"), 0);
    ASSERT_EQ(th_write_file((db + "-shm").c_str(), "keep shm"), 0);
    ASSERT_EQ(th_write_file((db + ".corrupt").c_str(), "old backup"), 0);
    std::string before = deletion_file_bytes(db);
    cbm_db_lease_t *lease = NULL;
    ASSERT_EQ(cbm_db_lease_try_acquire(db.c_str(), &lease), 0);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_scan_fallback(srv, false);
    char *result = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"damaged\"}");
    ASSERT(result && strstr(result, "lookup incomplete"));
    free(result);
    ASSERT(deletion_file_bytes(db) == before);
    ASSERT(deletion_file_bytes(db + "-wal") == "keep wal");
    // A normal READONLY SQLite connection rebuilds this deliberately invalid
    // shared-memory index. Repair must not unlink it; its bytes are not durable.
    ASSERT(cbm_is_regular_file((db + "-shm").c_str()));
    ASSERT(deletion_file_bytes(db + ".corrupt") == "old backup");
    cbm_db_lease_release(lease);
    // Once admitted, the normal confirmed-corruption recovery still runs.
    result = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"damaged\"}");
    ASSERT(result && strstr(result, "integrity check"));
    free(result);
    ASSERT(!cbm_is_regular_file(db.c_str()));
    ASSERT(deletion_file_bytes(db + ".corrupt") == before);
    ASSERT(cbm_is_regular_file((db + ".index.lock").c_str()));
    cbm_mcp_server_free(srv);
    th_rmtree(root);
    PASS();
}

#ifndef _WIN32
#include <cli/cli.h>
struct symlink_prune_grace {
    char *old = getenv("CBM_WATCHER_PRUNE_GRACE_S") ? strdup(getenv("CBM_WATCHER_PRUNE_GRACE_S")) : NULL;
    symlink_prune_grace() { cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", "0", 1); }
    ~symlink_prune_grace() {
        if (old) { cbm_setenv("CBM_WATCHER_PRUNE_GRACE_S", old, 1); free(old); }
        else { cbm_unsetenv("CBM_WATCHER_PRUNE_GRACE_S"); }
    }
};
TEST(mcp_destructive_symlink_paths_preserve_backing_database) {
    char root[256] = "/tmp/cbm_delete_alias_XXXXXX";
    ASSERT(cbm_mkdtemp(root));
    std::string cache = std::string(root) + "/cache";
    ASSERT(cbm_mkdir_p(cache.c_str(), 0755));
    deletion_cache_env env(cache.c_str());
    symlink_prune_grace grace;
    std::string backing = std::string(root) + "/backing.db";
    std::string alias = cache + "/alias.db";
    sqlite3 *raw = NULL;
    ASSERT_EQ(sqlite3_open(backing.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE projects(root_path TEXT); INSERT INTO projects VALUES('1invalid');", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw);
    std::string original = deletion_file_bytes(backing);
    ASSERT_EQ(th_write_file((backing + "-wal").c_str(), "backing wal"), 0);
    ASSERT_EQ(th_write_file((backing + "-shm").c_str(), "backing shm"), 0);
    for (int operation = 0; operation < 4; ++operation) {
        ASSERT_EQ(symlink(backing.c_str(), alias.c_str()), 0);
        ASSERT_EQ(th_write_file((alias + "-wal").c_str(), "alias wal"), 0);
        ASSERT_EQ(th_write_file((alias + "-shm").c_str(), "alias shm"), 0);
        if (operation == 0 || operation == 1) {
            cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
            cbm_mcp_server_set_scan_fallback(srv, false);
            if (operation == 0) {
                cbm_db_lease_t *lease = NULL;
                ASSERT_EQ(cbm_db_lease_try_acquire(backing.c_str(), &lease), 0);
                char *busy = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"alias\"}");
                ASSERT(cbm_mcp_result_is_index_busy(busy));
                free(busy);
                ASSERT(deletion_file_bytes(alias) == original);
                ASSERT(deletion_file_bytes(alias + "-wal") == "alias wal");
                cbm_db_lease_release(lease);
            }
            char *result = cbm_mcp_handle_tool(srv, operation == 0 ? "delete_project" : "index_status", "{\"project\":\"alias\"}");
            ASSERT(result && strstr(result, operation == 0 ? "deleted" : "integrity check"));
            free(result);
            cbm_mcp_server_free(srv);
        } else if (operation == 2) {
            // Earlier MCP operations also created _config.db in this cache.
            // Existing CLI cleanup counts it alongside the alias database.
            ASSERT_EQ(cbm_remove_indexes(root), 2);
        } else {
            cbm_watcher_t *w = cbm_watcher_new(NULL, NULL, NULL);
            std::string missing = std::string(root) + "/missing-root";
            cbm_watcher_watch(w, "alias", missing.c_str());
            for (int i = 0; i < 3; ++i) { cbm_watcher_poll_once(w); }
            ASSERT_EQ(cbm_watcher_watch_count(w), 0);
            cbm_watcher_free(w);
        }
        struct stat st;
        ASSERT(lstat(alias.c_str(), &st) != 0); // Remove requested entry, not referent.
        ASSERT(deletion_file_bytes(backing) == original);
        ASSERT(deletion_file_bytes(backing + "-wal") == "backing wal");
        // The read-only corruption check may rebuild SQLite's shared-memory
        // index through the alias. It must not delete the backing sidecar.
        ASSERT(cbm_is_regular_file((backing + "-shm").c_str()));
        ASSERT(!cbm_is_regular_file((alias + "-wal").c_str()));
        ASSERT(!cbm_is_regular_file((alias + "-shm").c_str()));
        ASSERT(cbm_is_regular_file((backing + ".index.lock").c_str()));
    }
    struct stat backup;
    ASSERT_EQ(lstat((alias + ".corrupt").c_str(), &backup), 0);
    ASSERT(S_ISLNK(backup.st_mode)); // Quarantine renamed the alias itself.
    th_rmtree(root);
    PASS();
}
#endif

SUITE(mcp) {
#ifndef _WIN32
    RUN_TEST(mcp_destructive_symlink_paths_preserve_backing_database);
#endif
    RUN_TEST(mcp_delete_busy_preserves_explicit_target);
    RUN_TEST(mcp_corrupt_quarantine_busy_preserves_generation);

    /* JSON-RPC parsing */
    RUN_TEST(jsonrpc_parse_request);
    RUN_TEST(jsonrpc_parse_notification);
    RUN_TEST(jsonrpc_parse_invalid);
    RUN_TEST(jsonrpc_parse_tools_call);
    RUN_TEST(jsonrpc_parse_string_id_issue253);
    RUN_TEST(jsonrpc_format_response_string_id_issue253);

    /* JSON-RPC parsing — edge cases */
    RUN_TEST(jsonrpc_parse_empty_string);
    RUN_TEST(jsonrpc_parse_missing_jsonrpc_field);
    RUN_TEST(jsonrpc_parse_missing_method);
    RUN_TEST(jsonrpc_parse_string_id);
    RUN_TEST(jsonrpc_parse_no_params);
    RUN_TEST(jsonrpc_parse_extra_whitespace);
    RUN_TEST(jsonrpc_parse_array_not_object);

    /* JSON-RPC formatting */
    RUN_TEST(jsonrpc_format_response);
    RUN_TEST(jsonrpc_format_error);

    /* MCP protocol helpers */
    RUN_TEST(mcp_initialize_response);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_tools_array_schemas_have_items);
    RUN_TEST(mcp_text_result);
    RUN_TEST(mcp_text_result_error);
    RUN_TEST(mcp_text_result_object_payload_carries_structured_content);
    RUN_TEST(mcp_text_result_text_payload_omits_structured_content);
    RUN_TEST(mcp_text_result_error_carries_structured_error);
    RUN_TEST(mcp_tools_list_declares_no_output_schema);
    RUN_TEST(mcp_tool_result_validation_rejects_partial_response);
    RUN_TEST(mcp_tool_deadlines_are_bounded_and_tool_specific);
    RUN_TEST(mcp_tool_worker_name_rejects_option_injection);
    RUN_TEST(mcp_update_check_can_be_disabled);

    /* Argument extraction */
    RUN_TEST(mcp_get_tool_name);
    RUN_TEST(mcp_get_arguments);
    RUN_TEST(mcp_get_string_arg);
    RUN_TEST(mcp_get_int_arg);
    RUN_TEST(mcp_get_bool_arg);

    /* Argument extraction — edge cases */
    RUN_TEST(mcp_get_string_arg_empty_json);
    RUN_TEST(mcp_get_string_arg_empty_object);
    RUN_TEST(mcp_get_string_arg_nested_value);
    RUN_TEST(mcp_get_string_arg_int_value);
    RUN_TEST(mcp_get_int_arg_empty_json);
    RUN_TEST(mcp_get_int_arg_string_value);
    RUN_TEST(mcp_get_int_arg_bool_value);
    RUN_TEST(mcp_get_bool_arg_empty_json);
    RUN_TEST(mcp_get_bool_arg_int_value);
    RUN_TEST(mcp_get_tool_name_empty_json);
    RUN_TEST(mcp_get_tool_name_missing_name);
    RUN_TEST(mcp_get_arguments_empty_json);
    RUN_TEST(mcp_get_arguments_no_arguments_key);

    /* Server protocol handling */
    RUN_TEST(server_handle_initialize);
    RUN_TEST(server_handle_initialized_notification);
    RUN_TEST(server_handle_tools_list);
    RUN_TEST(server_handle_unknown_method);

    /* Server handle — edge cases */
    RUN_TEST(server_handle_invalid_json);
    RUN_TEST(server_handle_empty_object);
    RUN_TEST(server_handle_tools_call_missing_name);

    /* Tool handlers */
    RUN_TEST(tool_list_projects_empty);
    RUN_TEST(tool_list_projects_includes_a_project_with_a_miss_graph);
    RUN_TEST(tool_trace_totals_respect_test_filter_tests_root_subtree_issue1294);
    RUN_TEST(search_code_full_preserves_utf8_source);
    RUN_TEST(search_graph_semantic_only_skips_structural_scan);
    RUN_TEST(index_response_reports_persisted_coverage_on_reindex);
    RUN_TEST(tool_list_projects_pages_deterministically);
    RUN_TEST(search_code_file_pattern_prefilter_boundaries);
    RUN_TEST(search_code_windows_prefilter_precedes_content_scan);
    RUN_TEST(tool_get_graph_schema_empty);
    RUN_TEST(tool_unknown_tool);
    RUN_TEST(tool_search_graph_basic);
    RUN_TEST(tool_get_architecture_cycles_detects_scc);
    RUN_TEST(tool_get_code_snippet_clips_whole_file_node);
    RUN_TEST(tool_search_graph_includes_node_properties);
    RUN_TEST(tool_query_graph_basic);
    RUN_TEST(tool_index_status_no_project);
    RUN_TEST(tool_index_status_includes_git_metadata);

    /* Tool handlers with validation */
    RUN_TEST(tool_trace_call_path_not_found);
    RUN_TEST(tool_trace_missing_function_name);
    RUN_TEST(tool_delete_project_not_found);
    RUN_TEST(tool_get_architecture_empty);
    RUN_TEST(tool_get_architecture_emits_populated_sections);
    RUN_TEST(tool_query_graph_missing_query);

    /* Pipeline-dependent tool handlers */
    RUN_TEST(tool_index_repository_missing_path);
    RUN_TEST(tool_get_code_snippet_missing_qn);
    RUN_TEST(tool_get_code_snippet_not_found);
    RUN_TEST(tool_search_code_missing_pattern);
    RUN_TEST(tool_search_code_no_project);
    RUN_TEST(search_code_multi_word);
    RUN_TEST(search_code_literal_pipe_warns_issue282);
    RUN_TEST(search_code_ampersand_accepted_issue272);
    RUN_TEST(tool_detect_changes_no_project);
    RUN_TEST(detect_changes_node_in_hunks_overlap_issue1363);
    RUN_TEST(detect_changes_seeds_only_touched_symbol_issue1363);
    RUN_TEST(detect_changes_zero_overlap_falls_back_issue1363);
    RUN_TEST(tool_manage_adr_no_project);
    RUN_TEST(tool_manage_adr_get_with_existing_adr);
    RUN_TEST(tool_manage_adr_unified_backend_issue256);
    RUN_TEST(tool_ingest_traces_basic);
    RUN_TEST(tool_ingest_traces_empty);

    /* Idle store eviction */
    RUN_TEST(store_idle_eviction);
    RUN_TEST(store_idle_no_eviction_within_timeout);
    RUN_TEST(store_idle_evict_protects_initial_store);
    RUN_TEST(store_idle_evict_access_resets_timer);

    /* URI helpers */
    RUN_TEST(parse_file_uri_unix);
    RUN_TEST(parse_file_uri_windows);
    RUN_TEST(parse_file_uri_invalid);

    /* URI helpers — edge cases */
    RUN_TEST(parse_file_uri_http_scheme);
    RUN_TEST(parse_file_uri_ftp_scheme);
    RUN_TEST(parse_file_uri_buffer_too_small);
    RUN_TEST(parse_file_uri_spaces_in_path);
    RUN_TEST(parse_file_uri_null_out_path);
    RUN_TEST(parse_file_uri_zero_size);

    /* Poll/getline FILE* buffering fix */
#ifndef _WIN32
    RUN_TEST(mcp_server_run_rapid_messages);
#endif

    /* Snippet resolution (port of snippet_test.go) */
    RUN_TEST(snippet_exact_qn);
    RUN_TEST(snippet_qn_suffix);
    RUN_TEST(snippet_unique_short_name);
    RUN_TEST(snippet_name_tier);
    RUN_TEST(snippet_ambiguous_short_name);
    RUN_TEST(snippet_not_found);
    RUN_TEST(snippet_fuzzy_suggestions);
    RUN_TEST(snippet_enriched_properties);
    RUN_TEST(snippet_fuzzy_last_segment);
    RUN_TEST(snippet_auto_resolve_default);
    RUN_TEST(snippet_auto_resolve_enabled);
    RUN_TEST(snippet_include_neighbors_default);
    RUN_TEST(snippet_include_neighbors_enabled);
    RUN_TEST(tool_bad_project_name_no_overflow_issue235);
#ifndef _WIN32
    RUN_TEST(tool_unknown_project_skips_nonregular_cache_db);
#endif
    RUN_TEST(tool_index_repository_resolves_root_path_from_project_name_issue1211);
    RUN_TEST(index_format_stale_db_rebuilds_once_issue769);
    RUN_TEST(index_recovery_quarantines_exit_nonzero);
    RUN_TEST(index_recovery_systemic_exit_nonzero_gives_up);
    RUN_TEST(tool_index_repository_unknown_project_name_still_requires_repo_path);
    RUN_TEST(tools_list_is_one_page);
    RUN_TEST(tool_trace_path_current_source_and_budget);
    RUN_TEST(tool_trace_between_preserves_identity_and_filters);
    RUN_TEST(tool_trace_between_rejects_ambiguous_and_invalid_entries);
    RUN_TEST(tool_trace_path_carries_location_and_call_site);
    RUN_TEST(tool_trace_path_test_filter_is_case_insensitive);
    RUN_TEST(tool_trace_test_nodes_do_not_spend_result_budget);
    RUN_TEST(store_bfs_result_and_examined_caps_are_truthful);
    RUN_TEST(store_bfs_null_filter_still_enforces_examined_cap);
    RUN_TEST(tool_inspect_symbol_uses_graph_declarations);
    RUN_TEST(tool_inspect_overload_family_labels_scope_and_declarations);
    RUN_TEST(tool_inspect_symbol_declarations_page_with_budget);
    RUN_TEST(tool_inspect_symbol_header_only_equivalent_declarations);
    RUN_TEST(tool_inspect_symbol_scoped_declaration_finds_callers);
    RUN_TEST(tool_inspect_symbol_unions_calls_to_declaration_and_definition);
    RUN_TEST(tool_inspect_symbol_caller_file_pages_are_complete);
    RUN_TEST(tool_inspect_symbol_metadata_and_errors_obey_byte_budget);
    RUN_TEST(tool_inspect_symbol_one_call);
    RUN_TEST(tool_inspect_symbol_reports_bounded_distinct_call_lines);
    RUN_TEST(tool_inspect_symbol_accepts_scoped_name);
    RUN_TEST(tool_inspect_symbol_ambiguous_returns_suggestions);
    RUN_TEST(tool_search_code_max_bytes_truncates_with_continuation);
    RUN_TEST(tool_project_resolves_from_cwd);
    RUN_TEST(hook_edit_impact_note);
}
