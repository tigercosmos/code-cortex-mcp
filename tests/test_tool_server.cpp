/* test_tool_server.cpp — supervision contract of the persistent tool worker.
 *
 * The point of the persistent worker (src/mcp/tool_server.h) is to keep the
 * isolation the per-call worker bought — a crashing or wedged tool costs one
 * process, never the stdio server — while paying the process cost once instead
 * of on every call. These tests pin both halves: one worker serves many calls,
 * and a worker that aborts or stalls is classified, killed, and replaced
 * without the next call noticing anything beyond a fresh process.
 *
 * The worker is stood up through cbm_tool_server_set_argv_for_testing rather
 * than the real `<self> cli --tool-server`, because the test binary is not the
 * server binary. The stand-in speaks the same framing and reports its own pid,
 * which is what makes "same worker" and "new worker" directly observable.
 *
 * POSIX only, for two independent reasons: Windows has no piped spawn, so
 * cbm_tool_server_call refuses outright and the per-call worker is used there
 * instead (see tool_server.h); and the stand-in worker is a /bin/sh script.
 * Both constraints lift together if the worker is ever ported. */
#include "../src/foundation/compat.h"
#include "../src/foundation/platform.h" /* cbm_now_ms */
#include "test_framework.h"
#include "test_helpers.h" /* th_mktempdir / th_write_file / th_rmtree */
#include <mcp/tool_server.h>
#include <stdlib.h>
#include <string.h>

#if defined(CBM_ENABLE_TEST_SEAMS) && !defined(_WIN32)

/* A POSIX-sh worker: read "<tool_len> <args_len>\n", the tool name and the
 * args, then reply "<len>\n<body>" with a body naming the tool and the pid.
 * FAKE_CRASH_TOOL / FAKE_HANG_TOOL make it misbehave on one chosen tool. */
static const char *TS_FAKE_WORKER =
    "#!/bin/sh\n"
    "while IFS=' ' read -r tlen alen; do\n"
    "  case \"$tlen\" in ''|*[!0-9]*) exit 0 ;; esac\n"
    "  tool=$(dd bs=1 count=\"$tlen\" 2>/dev/null)\n"
    "  [ \"$alen\" -gt 0 ] && dd bs=1 count=\"$alen\" >/dev/null 2>&1\n"
    "  if [ -n \"$FAKE_CRASH_TOOL\" ] && [ \"$tool\" = \"$FAKE_CRASH_TOOL\" ]; then\n"
    "    kill -ABRT $$\n"
    "  fi\n"
    "  if [ -n \"$FAKE_HANG_TOOL\" ] && [ \"$tool\" = \"$FAKE_HANG_TOOL\" ]; then\n"
    "    sleep 60\n"
    "  fi\n"
    "  body=\"{\\\"tool\\\":\\\"$tool\\\",\\\"pid\\\":$$}\"\n"
    "  printf '%s\\n%s' \"${#body}\" \"$body\"\n"
    "done\n";

typedef struct {
    char *dir;
    char script[1024];
    const char *argv[2];
} ts_fake_t;

static bool ts_fake_open_script(ts_fake_t *f, const char *body) {
    const char *made = th_mktempdir("cbm-tool-server");
    f->dir = made ? strdup(made) : NULL;
    if (!f->dir) {
        return false;
    }
    snprintf(f->script, sizeof(f->script), "%s/worker.sh", f->dir);
    if (th_write_file(f->script, body) != 0) {
        return false;
    }
    if (chmod(f->script, 0755) != 0) {
        return false;
    }
    f->argv[0] = f->script;
    f->argv[1] = NULL;
    cbm_tool_server_set_argv_for_testing(f->argv);
    return true;
}

static bool ts_fake_open(ts_fake_t *f) {
    return ts_fake_open_script(f, TS_FAKE_WORKER);
}

static void ts_fake_close(ts_fake_t *f) {
    cbm_tool_server_set_argv_for_testing(NULL); /* also stops the worker */
    cbm_unsetenv("FAKE_CRASH_TOOL");
    cbm_unsetenv("FAKE_HANG_TOOL");
    cbm_unsetenv("CBM_TOOL_TIMEOUT_S");
    if (f->dir) {
        th_rmtree(f->dir);
        free(f->dir);
    }
}

/* Pull the "pid":N field out of a reply body. -1 when absent. */
static long ts_reply_pid(const char *body) {
    const char *p = body ? strstr(body, "\"pid\":") : NULL;
    return p ? strtol(p + 6, NULL, 10) : -1;
}

TEST(tool_server_one_worker_serves_many_calls) {
    ts_fake_t f;
    if (!ts_fake_open(&f)) {
        ts_fake_close(&f);
        PASS();
    }
    int spawns_before = cbm_tool_server_spawn_count();

    cbm_index_worker_result_t a = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{\"project\":\"p\"}", &a), 0);
    ASSERT_EQ(a.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(a.response);
    long pid_a = ts_reply_pid(a.response);
    ASSERT_TRUE(pid_a > 0);
    ASSERT_NOT_NULL(strstr(a.response, "\"tool\":\"search_graph\""));
    free(a.response);

    cbm_index_worker_result_t b = {};
    ASSERT_EQ(cbm_tool_server_call("trace_path", "{\"project\":\"p\"}", &b), 0);
    ASSERT_EQ(b.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(b.response);
    ASSERT_NOT_NULL(strstr(b.response, "\"tool\":\"trace_path\""));
    /* The whole point: the second call did not pay for a process. */
    ASSERT_EQ(ts_reply_pid(b.response), pid_a);
    free(b.response);

    cbm_index_worker_result_t c = {};
    ASSERT_EQ(cbm_tool_server_call("get_code_snippet", "{}", &c), 0);
    ASSERT_EQ(c.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(c.response);
    ASSERT_EQ(ts_reply_pid(c.response), pid_a);
    free(c.response);

    ASSERT_EQ(cbm_tool_server_spawn_count() - spawns_before, 1);
    ts_fake_close(&f);
    PASS();
}

TEST(tool_server_crash_is_classified_and_the_next_call_respawns) {
    ts_fake_t f;
    if (!ts_fake_open(&f)) {
        ts_fake_close(&f);
        PASS();
    }
    /* Establish a worker and learn its pid, so "respawned" is provable. */
    cbm_index_worker_result_t warm = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &warm), 0);
    ASSERT_EQ(warm.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(warm.response);
    long pid_before = ts_reply_pid(warm.response);
    free(warm.response);
    int spawns_before = cbm_tool_server_spawn_count();

    cbm_setenv("FAKE_CRASH_TOOL", "query_graph", 1);
    /* The env var is read by the worker at exec, and this one is already
     * running — restart it so the setting takes effect. */
    cbm_tool_server_set_argv_for_testing(f.argv);

    cbm_index_worker_result_t dead = {};
    ASSERT_EQ(cbm_tool_server_call("query_graph", "{}", &dead), 0);
    /* A fault is reported as a fault, and no body is invented for it. */
    ASSERT_EQ(dead.outcome, CBM_PROC_CRASH);
    ASSERT_NULL(dead.response);

    /* The server is still serving: the next call gets a brand-new worker. */
    cbm_unsetenv("FAKE_CRASH_TOOL");
    cbm_tool_server_set_argv_for_testing(f.argv);
    cbm_index_worker_result_t after = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &after), 0);
    ASSERT_EQ(after.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(after.response);
    long pid_after = ts_reply_pid(after.response);
    ASSERT_TRUE(pid_after > 0);
    ASSERT_TRUE(pid_after != pid_before);
    free(after.response);
    ASSERT_TRUE(cbm_tool_server_spawn_count() > spawns_before);

    ts_fake_close(&f);
    PASS();
}

TEST(tool_server_deadline_kills_the_worker_and_the_next_call_succeeds) {
    ts_fake_t f;
    if (!ts_fake_open(&f)) {
        ts_fake_close(&f);
        PASS();
    }
    /* The stand-in sleeps 60s on this tool; a 1s budget must not wait for it. */
    cbm_setenv("CBM_TOOL_TIMEOUT_S", "1", 1);
    cbm_setenv("FAKE_HANG_TOOL", "query_graph", 1);
    cbm_tool_server_set_argv_for_testing(f.argv);

    uint64_t t0 = cbm_now_ms();
    cbm_index_worker_result_t hung = {};
    ASSERT_EQ(cbm_tool_server_call("query_graph", "{}", &hung), 0);
    uint64_t elapsed = cbm_now_ms() - t0;
    ASSERT_EQ(hung.outcome, CBM_PROC_HANG);
    ASSERT_NULL(hung.response);
    ASSERT_TRUE(elapsed >= 1000);
    ASSERT_TRUE(elapsed < 15000); /* generous for a loaded CI box, far under 60s */

    /* The wedged process was killed, not merely abandoned, and the server
     * still answers. */
    cbm_unsetenv("FAKE_HANG_TOOL");
    cbm_tool_server_set_argv_for_testing(f.argv);
    cbm_index_worker_result_t after = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &after), 0);
    ASSERT_EQ(after.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(after.response);
    ASSERT_NOT_NULL(strstr(after.response, "\"tool\":\"search_graph\""));
    free(after.response);

    ts_fake_close(&f);
    PASS();
}

TEST(tool_server_shutdown_is_idempotent_and_a_later_call_respawns) {
    ts_fake_t f;
    if (!ts_fake_open(&f)) {
        ts_fake_close(&f);
        PASS();
    }
    cbm_index_worker_result_t first = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &first), 0);
    ASSERT_NOT_NULL(first.response);
    long pid_first = ts_reply_pid(first.response);
    free(first.response);

    cbm_tool_server_shutdown();
    cbm_tool_server_shutdown(); /* second one must be a no-op, not a double-reap */

    cbm_index_worker_result_t second = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &second), 0);
    ASSERT_EQ(second.outcome, CBM_PROC_CLEAN);
    ASSERT_NOT_NULL(second.response);
    ASSERT_TRUE(ts_reply_pid(second.response) != pid_first);
    free(second.response);

    ts_fake_close(&f);
    PASS();
}

/* A tool name outside [a-z0-9_] never reaches a worker: it would be
 * interpolated into a worker command line elsewhere in this subsystem. */
TEST(tool_server_rejects_unsafe_tool_names) {
    ts_fake_t f;
    if (!ts_fake_open(&f)) {
        ts_fake_close(&f);
        PASS();
    }
    const char *bad[] = {"search graph", "search;rm", "Search_Graph", "../x", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cbm_index_worker_result_t r = {};
        ASSERT_EQ(cbm_tool_server_call(bad[i], "{}", &r), -1);
        ASSERT_NULL(r.response);
    }
    ts_fake_close(&f);
    PASS();
}

#endif /* CBM_ENABLE_TEST_SEAMS && !_WIN32 */

/* A worker wedged BEFORE it ever reads: it never drains stdin. The parent's
 * request then fills the pipe buffer (64KB on most systems) and the rest of
 * the write cannot complete until the worker reads, which it never will.
 *
 * With a blocking write end and the deadline armed only after the request was
 * sent, this parked the stdio MCP server in write() forever — the exact
 * unbounded hang the supervised design exists to prevent, reachable by any
 * request larger than the pipe buffer (TS_ARGS_MAX allows 64MB). The call must
 * instead give up at its own deadline and kill the worker. */
TEST(tool_server_deadline_bounds_a_worker_that_never_reads) {
    ts_fake_t f;
    if (!ts_fake_open_script(&f, "#!/bin/sh\nsleep 120\n")) {
        ts_fake_close(&f);
        PASS();
    }
    cbm_setenv("CBM_TOOL_TIMEOUT_S", "1", 1);

    /* Comfortably past any pipe buffer, and far below TS_ARGS_MAX. */
    size_t big = 1024u * 1024u;
    char *args = (char *)malloc(big + 1);
    ASSERT_NOT_NULL(args);
    memset(args, 'x', big);
    args[big] = '\0';
    args[0] = '{';
    args[1] = '"';
    args[big - 1] = '}';

    uint64_t t0 = cbm_now_ms();
    cbm_index_worker_result_t r = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", args, &r), 0);
    uint64_t elapsed = cbm_now_ms() - t0;
    free(args);
    ASSERT_EQ(r.outcome, CBM_PROC_HANG);
    ASSERT_NULL(r.response);
    ASSERT_TRUE(elapsed < 15000);

    /* The wedged worker was killed, not leaked, so the next call is served by
     * a healthy one. */
    cbm_unsetenv("CBM_TOOL_TIMEOUT_S");
    cbm_tool_server_set_argv_for_testing(NULL);
    ts_fake_t good;
    if (ts_fake_open(&good)) {
        cbm_index_worker_result_t after = {};
        ASSERT_EQ(cbm_tool_server_call("search_graph", "{}", &after), 0);
        ASSERT_EQ(after.outcome, CBM_PROC_CLEAN);
        ASSERT_NOT_NULL(after.response);
        free(after.response);
        ts_fake_close(&good);
    }

    ts_fake_close(&f);
    PASS();
}

/* The advertised timeout is the budget for the WHOLE call — spawn, write and
 * reply together — not one budget per stage and not one per retry attempt.
 * Uses the blocked-write path (a worker that never reads, a request past the
 * pipe buffer) so the spawn and the write are both inside the measured window
 * rather than free. */
TEST(tool_server_timeout_covers_the_whole_call_once) {
    ts_fake_t f;
    if (!ts_fake_open_script(&f, "#!/bin/sh\nsleep 120\n")) {
        ts_fake_close(&f);
        PASS();
    }
    cbm_setenv("CBM_TOOL_TIMEOUT_S", "2", 1);
    size_t big = 1024u * 1024u;
    char *args = (char *)malloc(big + 1);
    ASSERT_NOT_NULL(args);
    memset(args, 'x', big);
    args[big] = '\0';

    uint64_t t0 = cbm_now_ms();
    cbm_index_worker_result_t r = {};
    ASSERT_EQ(cbm_tool_server_call("search_graph", args, &r), 0);
    uint64_t elapsed = cbm_now_ms() - t0;
    free(args);
    ASSERT_EQ(r.outcome, CBM_PROC_HANG);
    /* One budget (2s) — not TS_MAX_ATTEMPTS of them, and not 2s plus the 5s
     * spawn ladder. The ceiling still separates 2s from 4s and from 7s. */
    ASSERT_TRUE(elapsed >= 2000);
    ASSERT_TRUE(elapsed < 3500);
    ts_fake_close(&f);
    PASS();
}

void suite_tool_server(void) {
#if defined(CBM_ENABLE_TEST_SEAMS) && !defined(_WIN32)
    RUN_TEST(tool_server_one_worker_serves_many_calls);
    RUN_TEST(tool_server_crash_is_classified_and_the_next_call_respawns);
    RUN_TEST(tool_server_deadline_kills_the_worker_and_the_next_call_succeeds);
    RUN_TEST(tool_server_shutdown_is_idempotent_and_a_later_call_respawns);
    RUN_TEST(tool_server_deadline_bounds_a_worker_that_never_reads);
    RUN_TEST(tool_server_timeout_covers_the_whole_call_once);
    RUN_TEST(tool_server_rejects_unsafe_tool_names);
#endif
}
