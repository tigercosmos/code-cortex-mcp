/* tool_server — persistent supervised worker for MCP tool calls. See tool_server.h. */
#include "tool_server.h"

#include "foundation/compat.h" /* cbm_nanosleep */
#include "foundation/log.h"
#include "foundation/mem.h"      /* cbm_mem_collect */
#include "foundation/platform.h" /* cbm_now_ms, cbm_resolve_self_exe_path */
#include "foundation/subprocess.h"
#include "mcp/index_supervisor.h"
#include "mcp/mcp.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

enum {
    TS_HEADER_MAX = 64,             /* "<tool_len> <args_len>\n" / "<len>\n" */
    TS_TOOL_NAME_MAX = 256,         /* cbm_tool_worker_name_safe already bounds the charset */
    TS_ARGS_MAX = 64 * 1024 * 1024, /* refuse absurd request bodies */
    TS_DEFAULT_MAX_REQUESTS = 2000, /* recycle the worker after this many calls */
    TS_COLLECT_EVERY = 64,          /* mi_collect cadence inside the worker */
    TS_IDLE_EVICT_S = 60,           /* mirrors STORE_IDLE_TIMEOUT_S in mcp.cpp */
    TS_SPAWN_BUDGET_MS = 5000,      /* spawn-retry ladder budget */
    TS_EXIT_GRACE_MS = 50,          /* wait for a worker that just closed the pipe */
    TS_SHUTDOWN_GRACE_MS = 200,     /* EOF → exit grace before SIGKILL on shutdown */
    TS_READ_CHUNK = 64 * 1024,
    TS_MAX_ATTEMPTS = 2, /* one transparent respawn+retry for a cleanly recycled worker */
};

static int g_ts_spawn_count = 0;

int cbm_tool_server_spawn_count(void) {
    return g_ts_spawn_count;
}

bool cbm_tool_server_enabled(void) {
#ifdef _WIN32
    return false; /* no piped spawn on Windows yet — one-shot workers */
#else
    const char *e = getenv("CBM_TOOL_SERVER");
    return !e || strcmp(e, "0") != 0;
#endif
}

/* ── Worker side ─────────────────────────────────────────────────── */

static bool ts_read_exact(FILE *in, char *buf, size_t n) {
    return n == 0 || fread(buf, 1, n, in) == n;
}

/* Parse "<a> <b>\n". Returns false at EOF or on a malformed header. */
static bool ts_read_header2(FILE *in, size_t *a, size_t *b) {
    char line[TS_HEADER_MAX];
    if (!fgets(line, sizeof(line), in)) {
        return false;
    }
    char *end = NULL;
    unsigned long long va = strtoull(line, &end, 10);
    if (end == line || *end != ' ') {
        return false;
    }
    char *end2 = NULL;
    unsigned long long vb = strtoull(end + 1, &end2, 10);
    if (end2 == end + 1 || *end2 != '\n') {
        return false;
    }
    *a = (size_t)va;
    *b = (size_t)vb;
    return true;
}

static long ts_max_requests(void) {
    const char *e = getenv("CBM_TOOL_SERVER_MAX_REQUESTS");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 0) {
            return v; /* 0 => never recycle */
        }
    }
    return TS_DEFAULT_MAX_REQUESTS;
}

/* Block until a request arrives, evicting the idle store every TS_IDLE_EVICT_S
 * seconds meanwhile so a worker that sits between agent turns does not pin a
 * database (and its page cache) forever. Returns false when stdin is gone. */
static bool ts_wait_for_request(FILE *in, cbm_mcp_server_t *srv) {
#ifndef _WIN32
    int fd = fileno(in);
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, TS_IDLE_EVICT_S * 1000);
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        if (pr < 0) {
            return false;
        }
        if (pr == 0) {
            cbm_mcp_server_evict_idle(srv, TS_IDLE_EVICT_S);
            continue;
        }
        return true;
    }
#else
    (void)in;
    (void)srv;
    return true;
#endif
}

int cbm_tool_server_serve(FILE *in, FILE *out) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        return 1;
    }
    long max_requests = ts_max_requests();
    long served = 0;
    int exit_code = 0;
    cbm_log_info("tool.server.start", "max_requests", max_requests > 0 ? "bounded" : "unbounded");
    for (;;) {
        /* cppcheck reads only the _WIN32 arm, where the wait is a stub that
         * cannot fail; the POSIX arm returns false on a broken poll. */
        // cppcheck-suppress knownConditionTrueFalse
        if (!ts_wait_for_request(in, srv)) {
            break;
        }
        size_t tool_len = 0;
        size_t args_len = 0;
        if (!ts_read_header2(in, &tool_len, &args_len)) {
            break; /* EOF: the parent closed the pipe */
        }
        if (tool_len == 0 || tool_len >= TS_TOOL_NAME_MAX || args_len > TS_ARGS_MAX) {
            cbm_log_warn("tool.server.bad_frame");
            exit_code = 2;
            break;
        }
        char tool[TS_TOOL_NAME_MAX];
        char *args = (char *)malloc(args_len + 1);
        if (!args || !ts_read_exact(in, tool, tool_len) || !ts_read_exact(in, args, args_len)) {
            free(args);
            exit_code = 2;
            break;
        }
        tool[tool_len] = '\0';
        args[args_len] = '\0';
#ifdef CBM_ENABLE_TEST_SEAMS
        /* Deterministic misbehaviour for the supervision tests: die from a
         * fault, or stall past the caller's deadline, on a chosen tool name. */
        const char *crash_on = getenv("CBM_TOOL_SERVER_TEST_CRASH_TOOL");
        if (crash_on && strcmp(crash_on, tool) == 0) {
            abort();
        }
        const char *hang_on = getenv("CBM_TOOL_SERVER_TEST_HANG_TOOL");
        if (hang_on && strcmp(hang_on, tool) == 0) {
            struct timespec ts = {60, 0};
            cbm_nanosleep(&ts, NULL);
        }
#endif
        /* A re-index or delete may have replaced the file behind the cached
         * store since the last call; never answer from a stale generation. */
        cbm_mcp_server_revalidate_store(srv);
        char *result = cbm_mcp_handle_tool(srv, tool, args);
        free(args);
        const char *body = result ? result : "";
        size_t body_len = strlen(body);
        bool written = fprintf(out, "%zu\n", body_len) > 0 &&
                       fwrite(body, 1, body_len, out) == body_len && fflush(out) == 0;
        free(result);
        if (!written) {
            break; /* parent went away */
        }
        served++;
        if (served % TS_COLLECT_EVERY == 0) {
            cbm_mem_collect();
        }
        if (max_requests > 0 && served >= max_requests) {
            cbm_log_info("tool.server.recycle", "served", "limit_reached");
            break;
        }
    }
    cbm_mcp_server_free(srv);
    return exit_code;
}

/* ── Parent side ─────────────────────────────────────────────────── */

static cbm_proc_pipe_t g_ts = {-1, -1, -1};
static char g_ts_self[1024];
static const char *g_ts_argv[4];
#ifdef CBM_ENABLE_TEST_SEAMS
static const char *const *g_ts_test_argv = NULL;

void cbm_tool_server_set_argv_for_testing(const char *const *argv) {
    g_ts_test_argv = argv;
    cbm_tool_server_shutdown(); /* a new command line means a new worker */
}
#endif

static const char *const *ts_argv(void) {
#ifdef CBM_ENABLE_TEST_SEAMS
    if (g_ts_test_argv) {
        return g_ts_test_argv;
    }
#endif
    if (!g_ts_self[0] &&
        (!cbm_resolve_self_exe_path(NULL, g_ts_self, sizeof(g_ts_self)) || !g_ts_self[0])) {
        return NULL;
    }
    g_ts_argv[0] = g_ts_self;
    g_ts_argv[1] = "cli";
    g_ts_argv[2] = "--tool-server";
    g_ts_argv[3] = NULL;
    return g_ts_argv;
}

static bool ts_spawn(int budget_ms) {
#ifndef _WIN32
    /* A worker that died leaves a pipe with no reader; writing the next request
     * must return EPIPE, not deliver SIGPIPE to the stdio server. */
    (void)signal(SIGPIPE, SIG_IGN);
#endif
    const char *const *argv = ts_argv();
    if (!argv) {
        cbm_log_warn("tool.server.no_self_path");
        return false;
    }
    cbm_proc_opts_t opts = {0};
    opts.bin = argv[0];
    opts.argv = argv;
    /* The spawn-retry ladder may not outlive the CALL's deadline: it used to
     * carry its own independent 5s budget, so a tool with a 1s timeout could
     * spend six seconds before the reply clock even started. */
    opts.total_timeout_ms = budget_ms < TS_SPAWN_BUDGET_MS ? budget_ms : TS_SPAWN_BUDGET_MS;
    if (cbm_subprocess_spawn_piped(&opts, &g_ts) != 0) {
        cbm_log_warn("tool.server.spawn_failed");
        return false;
    }
    g_ts_spawn_count++;
    cbm_log_info("tool.server.spawn", "spawn_count", g_ts_spawn_count == 1 ? "first" : "respawn");
    return true;
}

void cbm_tool_server_shutdown(void) {
    if (g_ts.pid <= 0 && g_ts.to_child < 0 && g_ts.from_child < 0) {
        return;
    }
    cbm_proc_result_t r;
    (void)cbm_subprocess_reap(&g_ts, false, TS_SHUTDOWN_GRACE_MS, &r);
}

#ifndef _WIN32
typedef enum { TS_WRITE_OK = 0, TS_WRITE_TIMEOUT, TS_WRITE_GONE } ts_write_t;

/* Write the whole buffer, or give up at deadline_ms.
 *
 * h->to_child is non-blocking (see cbm_subprocess_spawn_piped), because a
 * request larger than the pipe buffer against a worker that has stopped
 * reading would otherwise park the stdio server in write() forever — an
 * unbounded hang inside the very component whose job is to bound them. A
 * timeout here leaves a partial frame in the pipe, so the caller must kill the
 * worker rather than try to resynchronise. */
static ts_write_t ts_write_all(int fd, const char *buf, size_t n, uint64_t deadline_ms) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w > 0) {
            buf += (size_t)w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = cbm_now_ms();
            if (now >= deadline_ms) {
                return TS_WRITE_TIMEOUT;
            }
            int wr = cbm_subprocess_wait_writable(&g_ts, (int)(deadline_ms - now));
            if (wr == 0) {
                return TS_WRITE_TIMEOUT;
            }
            if (wr < 0) {
                return TS_WRITE_GONE;
            }
            continue;
        }
        return TS_WRITE_GONE; /* EPIPE and friends: the worker is gone */
    }
    return TS_WRITE_OK;
}

typedef enum { TS_REPLY_OK = 0, TS_REPLY_TIMEOUT, TS_REPLY_GONE } ts_reply_t;

/* Read one reply frame before deadline_ms. On OK, *body is heap (caller frees).
 * *got_any reports whether any byte arrived, which separates "worker exited
 * before starting the reply" (recyclable) from "died mid-reply" (a crash). */
static ts_reply_t ts_read_reply(uint64_t deadline_ms, char **body, bool *got_any) {
    *body = NULL;
    *got_any = false;
    char header[TS_HEADER_MAX];
    size_t hlen = 0;
    for (;;) {
        uint64_t now = cbm_now_ms();
        if (now >= deadline_ms) {
            return TS_REPLY_TIMEOUT;
        }
        int wr = cbm_subprocess_wait_readable(&g_ts, (int)(deadline_ms - now));
        if (wr == 0) {
            return TS_REPLY_TIMEOUT;
        }
        if (wr < 0) {
            return TS_REPLY_GONE;
        }
        char c;
        ssize_t r = read(g_ts.from_child, &c, 1);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return TS_REPLY_GONE;
        }
        *got_any = true;
        if (c == '\n') {
            break;
        }
        if (hlen + 1 >= sizeof(header)) {
            return TS_REPLY_GONE; /* protocol violation */
        }
        header[hlen++] = c;
    }
    header[hlen] = '\0';
    char *end = NULL;
    unsigned long long len = strtoull(header, &end, 10);
    if (end == header || *end != '\0' || len > (unsigned long long)INT_MAX) {
        return TS_REPLY_GONE;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        return TS_REPLY_GONE;
    }
    size_t have = 0;
    while (have < (size_t)len) {
        uint64_t now = cbm_now_ms();
        if (now >= deadline_ms) {
            free(buf);
            return TS_REPLY_TIMEOUT;
        }
        int wr = cbm_subprocess_wait_readable(&g_ts, (int)(deadline_ms - now));
        if (wr == 0) {
            free(buf);
            return TS_REPLY_TIMEOUT;
        }
        if (wr < 0) {
            free(buf);
            return TS_REPLY_GONE;
        }
        size_t want = (size_t)len - have;
        if (want > TS_READ_CHUNK) {
            want = TS_READ_CHUNK;
        }
        ssize_t r = read(g_ts.from_child, buf + have, want);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return TS_REPLY_GONE;
        }
        have += (size_t)r;
    }
    buf[len] = '\0';
    *body = buf;
    return TS_REPLY_OK;
}
#endif /* !_WIN32 */

int cbm_tool_server_call(const char *tool_name, const char *args_json,
                         cbm_index_worker_result_t *result) {
    result->outcome = CBM_PROC_SPAWN_FAILED;
    result->exit_code = -1;
    result->term_signal = 0;
    result->response = NULL;
#ifdef _WIN32
    (void)tool_name;
    (void)args_json;
    return -1;
#else
    if (!cbm_tool_worker_name_safe(tool_name)) {
        cbm_log_warn("tool.server.invalid_name");
        return -1;
    }
    const char *args = args_json ? args_json : "{}";
    size_t tool_len = strlen(tool_name);
    size_t args_len = strlen(args);
    char header[TS_HEADER_MAX];
    int hn = snprintf(header, sizeof(header), "%zu %zu\n", tool_len, args_len);
    if (hn <= 0 || (size_t)hn >= sizeof(header)) {
        return -1;
    }

    /* ONE deadline for the whole call — spawn, write and reply alike, and
     * shared by the retry attempt. Starting the clock after the spawn and the
     * write (as this once did) meant the advertised timeout bounded only the
     * reply: the spawn ladder carried its own budget, the writes had none at
     * all, and each attempt re-armed the timer. */
    uint64_t deadline_ms = cbm_now_ms() + (uint64_t)cbm_tool_timeout_ms(tool_name);

    for (int attempt = 0; attempt < TS_MAX_ATTEMPTS; attempt++) {
        cbm_proc_result_t pr;
        /* A worker that recycled itself (or died) while idle is reaped here and
         * replaced before the request is sent, so the retry budget is spent on
         * failures that happen DURING a call, not on housekeeping. */
        if (g_ts.pid > 0 && cbm_subprocess_poll_exit(&g_ts, &pr) == 1) {
            cbm_log_info("tool.server.exited_idle", "outcome", cbm_proc_outcome_str(pr.outcome));
        }
        uint64_t now = cbm_now_ms();
        if (now >= deadline_ms) {
            result->outcome = CBM_PROC_HANG;
            result->exit_code = -1;
            cbm_log_warn("tool.server.deadline", "tool", tool_name, "action", "before_spawn");
            return 0;
        }
        if (g_ts.pid <= 0 && !ts_spawn((int)(deadline_ms - now))) {
            return -1;
        }
        ts_write_t wrote = ts_write_all(g_ts.to_child, header, (size_t)hn, deadline_ms);
        if (wrote == TS_WRITE_OK) {
            wrote = ts_write_all(g_ts.to_child, tool_name, tool_len, deadline_ms);
        }
        if (wrote == TS_WRITE_OK) {
            wrote = ts_write_all(g_ts.to_child, args, args_len, deadline_ms);
        }
        if (wrote == TS_WRITE_TIMEOUT) {
            /* A half-written frame is unrecoverable: the worker is wedged and
             * its input stream is now out of sync. Kill it so the next call
             * starts clean. */
            (void)cbm_subprocess_reap(&g_ts, true, 0, &pr);
            result->outcome = CBM_PROC_HANG;
            result->exit_code = -1;
            result->term_signal = 0;
            cbm_log_warn("tool.server.deadline", "tool", tool_name, "action",
                         "killed_during_write");
            return 0;
        }
        if (wrote != TS_WRITE_OK) {
            (void)cbm_subprocess_reap(&g_ts, false, TS_EXIT_GRACE_MS, &pr);
            if (pr.outcome == CBM_PROC_CLEAN) {
                continue; /* recycled between calls: respawn and resend */
            }
            result->outcome = pr.outcome;
            result->exit_code = pr.exit_code;
            result->term_signal = pr.term_signal;
            cbm_log_warn("tool.server.write_failed", "tool", tool_name, "outcome",
                         cbm_proc_outcome_str(pr.outcome));
            return 0;
        }

        char *body = NULL;
        bool got_any = false;
        ts_reply_t rr = ts_read_reply(deadline_ms, &body, &got_any);
        if (rr == TS_REPLY_OK) {
            result->outcome = CBM_PROC_CLEAN;
            result->exit_code = 0;
            result->response = body;
            return 0;
        }
        if (rr == TS_REPLY_TIMEOUT) {
            (void)cbm_subprocess_reap(&g_ts, true, 0, &pr);
            result->outcome = CBM_PROC_HANG;
            result->exit_code = -1;
            result->term_signal = 0;
            cbm_log_warn("tool.server.deadline", "tool", tool_name, "action", "killed");
            return 0;
        }
        /* The pipe closed: the worker exited. Classify how. */
        (void)cbm_subprocess_reap(&g_ts, false, TS_EXIT_GRACE_MS, &pr);
        if (!got_any && pr.outcome == CBM_PROC_CLEAN) {
            cbm_log_info("tool.server.recycled_midcall", "tool", tool_name);
            continue; /* it exited cleanly before replying — try one fresh worker */
        }
        result->outcome = pr.outcome == CBM_PROC_CLEAN ? CBM_PROC_KILLED : pr.outcome;
        result->exit_code = pr.exit_code;
        result->term_signal = pr.term_signal;
        cbm_log_warn("tool.server.worker_died", "tool", tool_name, "outcome",
                     cbm_proc_outcome_str(result->outcome));
        return 0;
    }
    return -1;
#endif
}
