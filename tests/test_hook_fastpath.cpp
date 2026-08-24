/* test_hook_fastpath.cpp — the two MCP entry points hook-augment relies on.
 *
 * hook-augment runs before every Grep/Glob/Read under a 300ms hard deadline,
 * in a fresh process, and probes up to HA_MAX_WALKUP speculative project names
 * per invocation. Two costs used to make that budget unreachable: the
 * cache-dir scan (one sqlite open per database, hundreds of them, per missed
 * guess) and index_status (a git subprocess plus full-graph counts) just to
 * read one file's coverage row. These tests pin the replacements. */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "test_framework.h"
#include "test_helpers.h" /* th_mktempdir / th_rmtree */
#include <mcp/mcp.h>
#include <store/store.h>
#include <stdlib.h>
#include <string.h>

/* Point CBM_CACHE_DIR at a fresh temp dir for one test; restores on close. */
typedef struct {
    char *dir;
    char *saved;
} hf_env_t;

static bool hf_env_open(hf_env_t *e, const char *prefix) {
    const char *made = th_mktempdir(prefix); /* static buffer — copy it */
    e->dir = made ? strdup(made) : NULL;
    if (!e->dir) {
        return false;
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    e->saved = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", e->dir, 1);
    return true;
}

static void hf_env_close(hf_env_t *e) {
    if (e->saved) {
        cbm_setenv("CBM_CACHE_DIR", e->saved, 1);
        free(e->saved);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (e->dir) {
        th_rmtree(e->dir);
        free(e->dir);
    }
}

/* Create <cache>/<file_name> holding a single project called `internal`. */
static bool hf_make_db(const hf_env_t *e, const char *file_name, const char *internal,
                       const char *root) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", e->dir, file_name);
    cbm_store_t *st = cbm_store_open_path(path);
    if (!st) {
        return false;
    }
    bool ok = cbm_store_upsert_project(st, internal, root) == CBM_STORE_OK;
    cbm_store_close(st);
    return ok;
}

/* A drifted filename (#704) is only resolvable by the cache-dir scan on the
 * FIRST lookup. With the scan off — the hook's policy — that first lookup must
 * miss rather than walk the directory; once any scanning caller has recorded
 * the file in the memo, the same policy resolves it from that row alone. */
TEST(hook_scan_fallback_off_resolves_from_the_memo_only) {
    hf_env_t env;
    if (!hf_env_open(&env, "cbm-hook-fastpath")) {
        PASS();
    }
    ASSERT_TRUE(hf_make_db(&env, "drifted-file.db", "internal-name", "/src/internal"));
    /* Decoys: what the scan would have to open to find the one above. */
    ASSERT_TRUE(hf_make_db(&env, "decoy-a.db", "decoy-a", "/src/a"));
    ASSERT_TRUE(hf_make_db(&env, "decoy-b.db", "decoy-b", "/src/b"));

    /* Scan off, memo empty → not found, and nothing in the cache dir opened. */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"internal-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "/src/internal"));
    free(resp);
    cbm_mcp_server_free(srv);

    /* A normal (scanning) caller resolves it and leaves the memo row behind. */
    srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"internal-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "/src/internal"));
    free(resp);
    cbm_mcp_server_free(srv);

    /* Scan still off — now the memo answers, so the hook pays one open. */
    srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);
    resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"internal-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "/src/internal"));
    free(resp);
    cbm_mcp_server_free(srv);

    hf_env_close(&env);
    PASS();
}

/* Turning the scan back on is not sticky in either direction. */
TEST(hook_scan_fallback_defaults_on_and_toggles_back) {
    hf_env_t env;
    if (!hf_env_open(&env, "cbm-hook-fastpath")) {
        PASS();
    }
    ASSERT_TRUE(hf_make_db(&env, "drifted-again.db", "toggle-name", "/src/toggle"));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"toggle-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "/src/toggle"));
    free(resp);

    cbm_mcp_server_set_scan_fallback(srv, true);
    resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"toggle-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "/src/toggle"));
    free(resp);
    cbm_mcp_server_free(srv);

    cbm_mcp_server_set_scan_fallback(NULL, false); /* no crash on a null server */
    hf_env_close(&env);
    PASS();
}

/* Seed a project with one partially-parsed file and one skipped file. Both
 * need a file_hashes row or cbm_store_coverage_replace prunes them as
 * belonging to files that no longer exist. */
static bool hf_seed_coverage(const hf_env_t *e) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/covproj.db", e->dir);
    cbm_store_t *st = cbm_store_open_path(path);
    if (!st) {
        return false;
    }
    bool ok =
        cbm_store_upsert_project(st, "covproj", "/src/covproj") == CBM_STORE_OK &&
        cbm_store_upsert_file_hash(st, "covproj", "src/partial.c", "aa", 1, 10) == CBM_STORE_OK &&
        cbm_store_upsert_file_hash(st, "covproj", "src/skipped.c", "bb", 1, 10) == CBM_STORE_OK &&
        cbm_store_upsert_file_hash(st, "covproj", "src/clean.c", "cc", 1, 10) == CBM_STORE_OK;
    if (ok) {
        cbm_coverage_row_t rows[] = {
            {"src/partial.c", "parse_partial", "12-40,88-90"},
            {"src/skipped.c", "extract", "unsupported syntax"},
        };
        ok = cbm_store_coverage_replace(st, "covproj", rows, 2) == CBM_STORE_OK;
    }
    cbm_store_close(st);
    return ok;
}

TEST(hook_coverage_note_reports_partial_skipped_and_clean) {
    hf_env_t env;
    if (!hf_env_open(&env, "cbm-hook-fastpath")) {
        PASS();
    }
    ASSERT_TRUE(hf_seed_coverage(&env));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Partially parsed: the note names the unparsed line ranges. */
    bool resolved = false;
    char *note = cbm_mcp_coverage_note(srv, "covproj", "src/partial.c", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NOT_NULL(note);
    ASSERT_NOT_NULL(strstr(note, "PARTIALLY indexed"));
    ASSERT_NOT_NULL(strstr(note, "12-40,88-90"));
    free(note);

    /* Skipped entirely: the note names the phase and the reason. */
    resolved = false;
    note = cbm_mcp_coverage_note(srv, "covproj", "src/skipped.c", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NOT_NULL(note);
    ASSERT_NOT_NULL(strstr(note, "NOT indexed"));
    ASSERT_NOT_NULL(strstr(note, "extract"));
    ASSERT_NOT_NULL(strstr(note, "unsupported syntax"));
    free(note);

    /* Fully covered file: nothing to say, but the project DID resolve — the
     * signal that stops the hook climbing to a parent directory. */
    resolved = false;
    note = cbm_mcp_coverage_note(srv, "covproj", "src/clean.c", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NULL(note);

    /* Unindexed project: no note AND not resolved — the hook climbs. */
    resolved = true;
    note = cbm_mcp_coverage_note(srv, "no-such-project", "src/clean.c", &resolved);
    ASSERT_FALSE(resolved);
    ASSERT_NULL(note);

    /* Degenerate arguments are misses, never crashes. */
    ASSERT_NULL(cbm_mcp_coverage_note(srv, "covproj", "", &resolved));
    ASSERT_NULL(cbm_mcp_coverage_note(srv, NULL, "src/partial.c", &resolved));
    ASSERT_NULL(cbm_mcp_coverage_note(NULL, "covproj", "src/partial.c", NULL));
    /* The out-param is optional — a caller that only wants the note may omit it. */
    note = cbm_mcp_coverage_note(srv, "covproj", "src/partial.c", NULL);
    ASSERT_NOT_NULL(note);
    free(note);

    cbm_mcp_server_free(srv);
    hf_env_close(&env);
    PASS();
}

/* The hook's Read path uses the note under a scan-free policy: an unindexed
 * tree must come back not-resolved (so the walk-up ends) without the cache-dir
 * walk that used to eat the whole 300ms deadline. */
TEST(hook_coverage_note_on_unindexed_tree_is_a_scan_free_miss) {
    hf_env_t env;
    if (!hf_env_open(&env, "cbm-hook-fastpath")) {
        PASS();
    }
    ASSERT_TRUE(hf_make_db(&env, "decoy-a.db", "decoy-a", "/src/a"));
    ASSERT_TRUE(hf_make_db(&env, "decoy-b.db", "decoy-b", "/src/b"));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);
    /* The names a walk-up over /home/someone/repo/src/deep would try. */
    const char *guesses[] = {"deep", "repo-src-deep", "repo-src", "repo", "someone", "home"};
    for (size_t i = 0; i < sizeof(guesses) / sizeof(guesses[0]); i++) {
        bool resolved = true;
        char *note = cbm_mcp_coverage_note(srv, guesses[i], "src/deep/x.c", &resolved);
        ASSERT_FALSE(resolved);
        ASSERT_NULL(note);
    }
    cbm_mcp_server_free(srv);
    hf_env_close(&env);
    PASS();
}

void suite_hook_fastpath(void) {
    RUN_TEST(hook_scan_fallback_off_resolves_from_the_memo_only);
    RUN_TEST(hook_scan_fallback_defaults_on_and_toggles_back);
    RUN_TEST(hook_coverage_note_reports_partial_skipped_and_clean);
    RUN_TEST(hook_coverage_note_on_unindexed_tree_is_a_scan_free_miss);
}
