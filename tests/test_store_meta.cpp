/* test_store_meta.cpp — on-disk store metadata memo (src/mcp/store_meta.h).
 *
 * The memo exists so that fresh processes (tool workers, hook-augment) do not
 * re-run PRAGMA quick_check or reopen every cache database. These tests pin
 * the contract that makes that safe: a row answers only for the exact
 * (size, mtime) generation it was recorded at, TRANSIENT verdicts are never
 * stored, deletion forgets, and the kill switch turns every lookup into a
 * miss. */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h" /* cbm_file_generation */
#include "test_framework.h"
#include "test_helpers.h"
#include <mcp/mcp.h>
#include <mcp/store_meta.h>
#include <store/store.h>
#include <stdlib.h>
#include <string.h>

/* A synthetic generation, for rows that describe no real file. */
static cbm_file_gen_t sm_gen(int64_t size, int64_t mtime, int64_t mtime_ns, uint64_t ino) {
    cbm_file_gen_t g;
    g.size = size;
    g.mtime = mtime;
    g.mtime_ns = mtime_ns;
    g.ino = ino;
    return g;
}

/* Point CBM_CACHE_DIR at a fresh temp dir for one test; restores on close. */
typedef struct {
    char *dir;
    char *saved;
} sm_env_t;

static bool sm_env_open(sm_env_t *e, const char *prefix) {
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

static void sm_env_close(sm_env_t *e) {
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

TEST(store_meta_key_is_final_path_component) {
    ASSERT_STR_EQ(cbm_store_meta_key("/a/b/proj.db"), "proj.db");
    ASSERT_STR_EQ(cbm_store_meta_key("C:\\cache\\proj.db"), "proj.db");
    ASSERT_STR_EQ(cbm_store_meta_key("proj.db"), "proj.db");
    ASSERT_STR_EQ(cbm_store_meta_key(NULL), "");
    PASS();
}

TEST(store_meta_put_then_get_hits_same_generation_only) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);

    cbm_store_meta_row_t row;
    cbm_file_gen_t g = sm_gen(1234, 5678, 4242, 99);
    cbm_store_meta_row_init(&row, "/anywhere/alpha.db", &g);
    ASSERT_EQ(row.nodes, CBM_STORE_META_UNKNOWN);
    ASSERT_EQ(row.verdict, CBM_STORE_META_UNKNOWN);
    snprintf(row.project, sizeof(row.project), "%s", "alpha-internal");
    snprintf(row.root_path, sizeof(row.root_path), "%s", "/src/alpha");
    row.nodes = 10;
    row.edges = 20;
    row.verdict = (int)CBM_INTEGRITY_OK;
    ASSERT_TRUE(cbm_store_meta_put(db, &row));

    cbm_store_meta_row_t got;
    memset(&got, 0, sizeof(got));
    /* Same generation: hit, with every field intact. */
    ASSERT_TRUE(cbm_store_meta_get(db, "alpha.db", &g, &got));
    ASSERT_STR_EQ(got.db_name, "alpha.db");
    ASSERT_STR_EQ(got.project, "alpha-internal");
    ASSERT_STR_EQ(got.root_path, "/src/alpha");
    ASSERT_EQ(got.nodes, 10);
    ASSERT_EQ(got.edges, 20);
    ASSERT_EQ(got.verdict, (int)CBM_INTEGRITY_OK);
    /* A rewritten file must miss — that is the whole invalidation story;
     * nothing else ever expires a row. EVERY component of the generation
     * participates: a same-second rebuild at the same byte size differs only in
     * the sub-second stamp and the inode, and that case is exactly why those
     * two fields exist. */
    cbm_file_gen_t other_size = sm_gen(1235, 5678, 4242, 99);
    cbm_file_gen_t other_sec = sm_gen(1234, 5679, 4242, 99);
    cbm_file_gen_t other_ns = sm_gen(1234, 5678, 4243, 99);
    cbm_file_gen_t other_ino = sm_gen(1234, 5678, 4242, 100);
    ASSERT_FALSE(cbm_store_meta_get(db, "alpha.db", &other_size, &got));
    ASSERT_FALSE(cbm_store_meta_get(db, "alpha.db", &other_sec, &got));
    ASSERT_FALSE(cbm_store_meta_get(db, "alpha.db", &other_ns, &got));
    ASSERT_FALSE(cbm_store_meta_get(db, "alpha.db", &other_ino, &got));
    ASSERT_FALSE(cbm_store_meta_get(db, "other.db", &g, &got));
    /* An unstattable file has no generation and can never hit. */
    cbm_file_gen_t missing = sm_gen(-1, 0, 0, 0);
    ASSERT_FALSE(cbm_store_meta_get(db, "alpha.db", &missing, &got));

    /* Upsert replaces in place: one row per file. */
    row.nodes = 11;
    ASSERT_TRUE(cbm_store_meta_put(db, &row));
    ASSERT_TRUE(cbm_store_meta_get(db, "/x/alpha.db", &g, &got));
    ASSERT_EQ(got.nodes, 11);

    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

TEST(store_meta_find_project_returns_the_file_name) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t row;
    cbm_file_gen_t g = sm_gen(1, 2, 3, 4);
    cbm_store_meta_row_init(&row, "drifted-file-name.db", &g);
    snprintf(row.project, sizeof(row.project), "%s", "real-internal-name");
    ASSERT_TRUE(cbm_store_meta_put(db, &row));

    cbm_store_meta_row_t got;
    ASSERT_TRUE(cbm_store_meta_find_project(db, "real-internal-name", &got));
    ASSERT_STR_EQ(got.db_name, "drifted-file-name.db");
    ASSERT_TRUE(cbm_file_gen_equal(&got.gen, &g));
    ASSERT_FALSE(cbm_store_meta_find_project(db, "nobody", &got));
    ASSERT_FALSE(cbm_store_meta_find_project(db, "", &got));

    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

TEST(store_meta_transient_verdict_is_never_stored) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t row;
    cbm_file_gen_t g = sm_gen(7, 7, 7, 7);
    cbm_store_meta_row_init(&row, "busy.db", &g);
    row.verdict = (int)CBM_INTEGRITY_TRANSIENT;
    ASSERT_TRUE(cbm_store_meta_put(db, &row));
    cbm_store_meta_row_t got;
    ASSERT_TRUE(cbm_store_meta_get(db, "busy.db", &g, &got));
    /* "Ask again" must not become sticky across processes. */
    ASSERT_EQ(got.verdict, CBM_STORE_META_UNKNOWN);
    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

TEST(store_meta_forget_removes_the_row) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t row;
    cbm_file_gen_t g = sm_gen(3, 4, 5, 6);
    cbm_store_meta_row_init(&row, "gone.db", &g);
    snprintf(row.project, sizeof(row.project), "%s", "gone");
    ASSERT_TRUE(cbm_store_meta_put(db, &row));
    cbm_store_meta_forget(db, "/cache/gone.db");
    cbm_store_meta_row_t got;
    ASSERT_FALSE(cbm_store_meta_get(db, "gone.db", &g, &got));
    ASSERT_FALSE(cbm_store_meta_find_project(db, "gone", &got));
    cbm_store_meta_forget(db, "never-existed.db"); /* no-op, no error */
    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

TEST(store_meta_null_handle_is_a_miss_everywhere) {
    cbm_store_meta_row_t row;
    cbm_file_gen_t g = sm_gen(1, 1, 1, 1);
    cbm_store_meta_row_init(&row, "x.db", &g);
    ASSERT_FALSE(cbm_store_meta_get(NULL, "x.db", &g, &row));
    ASSERT_FALSE(cbm_store_meta_find_project(NULL, "x", &row));
    ASSERT_FALSE(cbm_store_meta_put(NULL, &row));
    cbm_store_meta_forget(NULL, "x.db");
    cbm_store_meta_close(NULL);
    PASS();
}

TEST(store_meta_kill_switch_disables_the_memo) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    cbm_setenv("CBM_STORE_META", "0", 1);
    cbm_store_meta_db_t *db = cbm_store_meta_open();
    cbm_unsetenv("CBM_STORE_META");
    ASSERT_NULL(db);
    sm_env_close(&env);
    PASS();
}

/* End to end through the MCP layer: a store whose FILE name differs from its
 * internal project name (#704) is found by the cache-dir scan on the first
 * query, and the scan leaves behind a memo row naming the file, so a later
 * process can go straight to it. The verdict is memoized as OK on the same
 * generation, which is what lets a fresh worker skip quick_check. */
TEST(store_meta_resolve_records_drifted_file_and_verdict) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/file-name-drifted.db", env.dir);
    cbm_store_t *st = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(st);
    ASSERT_EQ(cbm_store_upsert_project(st, "internal-name", "/src/internal"), CBM_STORE_OK);
    cbm_store_close(st);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"internal-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\\\"project\\\":\\\"internal-name\\\""));
    free(resp);
    cbm_mcp_server_free(srv);

    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t got;
    ASSERT_TRUE(cbm_store_meta_find_project(db, "internal-name", &got));
    ASSERT_STR_EQ(got.db_name, "file-name-drifted.db");
    /* The VERDICT must be recorded too, not just the name mapping. The
     * scanned-store branch used to run cbm_store_check_integrity_verdict
     * directly, so a drifted-name database was never memoized and every fresh
     * worker and hook paid the full quick_check again — the one cost the memo
     * exists to remove. */
    cbm_file_gen_t gen;
    ASSERT_TRUE(cbm_file_generation(db_path, &gen));
    ASSERT_TRUE(cbm_store_meta_get(db, db_path, &gen, &got));
    ASSERT_EQ(got.verdict, (int)CBM_INTEGRITY_OK);
    cbm_store_meta_close(db);

    /* Second, fresh server: resolves through the memo hint (no other db to
     * scan here, so the observable is simply that it still resolves and the
     * verdict has been recorded for this generation). */
    srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"internal-name\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);
    sm_env_close(&env);
    PASS();
}

/* A direct-name store (file == internal name) records its verdict; a
 * delete_project call forgets the row. */
TEST(store_meta_delete_project_forgets_row) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/plain.db", env.dir);
    cbm_store_t *st = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(st);
    ASSERT_EQ(cbm_store_upsert_project(st, "plain", "/src/plain"), CBM_STORE_OK);
    cbm_store_close(st);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_handle_tool(srv, "index_status", "{\"project\":\"plain\"}");
    ASSERT_NOT_NULL(resp);
    free(resp);

    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t got;
    cbm_file_gen_t gen;
    ASSERT_TRUE(cbm_file_generation(db_path, &gen));
    ASSERT_TRUE(cbm_store_meta_get(db, db_path, &gen, &got));
    ASSERT_EQ(got.verdict, (int)CBM_INTEGRITY_OK);
    ASSERT_STR_EQ(got.project, "plain");
    ASSERT_STR_EQ(got.root_path, "/src/plain");
    cbm_store_meta_close(db);

    resp = cbm_mcp_handle_tool(srv, "delete_project", "{\"project\":\"plain\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "deleted"));
    free(resp);
    cbm_mcp_server_free(srv);

    db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    ASSERT_FALSE(cbm_store_meta_get(db, db_path, &gen, &got));
    ASSERT_FALSE(cbm_store_meta_find_project(db, "plain", &got));
    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

/* The collision the (size, whole-second mtime) key could not see: a file
 * replaced within the same second at exactly the same byte size. The old key
 * matched it and served the previous generation's facts; the generation key
 * must treat it as a miss. Uses real files so the platform's own stat data
 * decides, not a synthetic stamp. */
TEST(store_meta_same_second_same_size_rewrite_is_a_miss) {
    sm_env_t env;
    if (!sm_env_open(&env, "cbm-store-meta")) {
        PASS();
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/twin.db", env.dir);
    ASSERT_EQ(th_write_file(path, "AAAAAAAA"), 0);
    cbm_file_gen_t first;
    ASSERT_TRUE(cbm_file_generation(path, &first));

    cbm_store_meta_db_t *db = cbm_store_meta_open();
    ASSERT_NOT_NULL(db);
    cbm_store_meta_row_t row;
    cbm_store_meta_row_init(&row, path, &first);
    snprintf(row.project, sizeof(row.project), "%s", "twin");
    row.verdict = (int)CBM_INTEGRITY_OK;
    row.nodes = 7;
    ASSERT_TRUE(cbm_store_meta_put(db, &row));

    /* Replace it: unlink + recreate, same length, immediately (same second). */
    cbm_unlink(path);
    ASSERT_EQ(th_write_file(path, "BBBBBBBB"), 0);
    cbm_file_gen_t second;
    ASSERT_TRUE(cbm_file_generation(path, &second));
    ASSERT_EQ(first.size, second.size); /* the trap: identical size ... */

    /* ... and the old key would now hit, because within one second the
     * whole-second mtime is identical too. The generation key must not. */
    cbm_store_meta_row_t got;
    if (first.mtime == second.mtime) {
        ASSERT_FALSE(cbm_store_meta_get(db, path, &second, &got));
    }
    ASSERT_FALSE(cbm_file_gen_equal(&first, &second));
    /* The row is still addressable at its own generation — invalidation is by
     * generation, not by wiping the table. */
    ASSERT_TRUE(cbm_store_meta_get(db, path, &first, &got));
    ASSERT_EQ(got.nodes, 7);

    cbm_store_meta_close(db);
    sm_env_close(&env);
    PASS();
}

void suite_store_meta(void) {
    RUN_TEST(store_meta_key_is_final_path_component);
    RUN_TEST(store_meta_put_then_get_hits_same_generation_only);
    RUN_TEST(store_meta_find_project_returns_the_file_name);
    RUN_TEST(store_meta_transient_verdict_is_never_stored);
    RUN_TEST(store_meta_forget_removes_the_row);
    RUN_TEST(store_meta_same_second_same_size_rewrite_is_a_miss);
    RUN_TEST(store_meta_null_handle_is_a_miss_everywhere);
    RUN_TEST(store_meta_kill_switch_disables_the_memo);
    RUN_TEST(store_meta_resolve_records_drifted_file_and_verdict);
    RUN_TEST(store_meta_delete_project_forgets_row);
}
