/* test_hook_fastpath.cpp — the two MCP entry points hook-augment relies on.
 *
 * hook-augment runs before every Grep/Glob/Read under a 300ms hard deadline,
 * in a fresh process, and probes up to HA_MAX_WALKUP speculative project names
 * per invocation. Two costs used to make that budget unreachable: the
 * cache-dir scan (one sqlite open per database, hundreds of them, per missed
 * guess) and index_status (a git subprocess plus full-graph counts) just to
 * read one file's coverage row. These tests pin the replacements. */
#include "../src/cli/cli.h"
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "test_framework.h"
#include "test_helpers.h" /* th_mktempdir / th_rmtree */
#include <mcp/mcp.h>
#include <store/store.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#ifdef CBM_ENABLE_TEST_SEAMS
extern bool cbm_prompt_freshness_for_testing(const char *inspect_json);
extern char *cbm_prompt_label_for_testing(bool scanned);
#endif

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

/* SessionStart brief inputs: a stored brief is read back as-is (one row, no
 * architecture queries); with none stored, a small graph is computed live and
 * a large one (node-id ceiling >= CBM_MCP_BRIEF_LIVE_MAX_NODES) gets only an
 * approximate size, so a missing brief never costs the multi-second queries. */
static bool hf_seed_brief_db(const hf_env_t *e, const char *project, long long extra_id) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.db", e->dir, project);
    cbm_store_t *st = cbm_store_open_path(path);
    if (!st) {
        return false;
    }
    bool ok = cbm_store_upsert_project(st, project, "/src/brief") == CBM_STORE_OK;
    cbm_store_close(st);
    sqlite3 *db = nullptr;
    if (!ok || sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO nodes(project,label,name,qualified_name,file_path) VALUES"
             "('%s','Function','alpha_fn','%s.a.alpha_fn','a.cpp'),"
             "('%s','Function','beta_fn','%s.b.beta_fn','b.py');",
             project, project, project, project);
    ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok && extra_id > 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO nodes(id,project,label,name,qualified_name) VALUES"
                 "(%lld,'%s','Function','far_fn','%s.far_fn');",
                 extra_id, project, project);
        ok = sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    sqlite3_close(db);
    return ok;
}

TEST(hook_session_brief_reads_stored_inputs_and_bounds_misses) {
    hf_env_t env;
    if (!hf_env_open(&env, "cbm-hook-brief")) {
        PASS();
    }
    ASSERT_TRUE(hf_seed_brief_db(&env, "smallproj", 0));
    ASSERT_TRUE(hf_seed_brief_db(&env, "bigproj", CBM_MCP_BRIEF_LIVE_MAX_NODES + 5));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);

    /* Small, nothing stored: computed live with exact counts. */
    bool resolved = false;
    char *json = cbm_mcp_session_brief_json(srv, "smallproj", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"nodes\":2"));
    ASSERT_NOT_NULL(strstr(json, "\"languages\""));
    ASSERT_NULL(strstr(json, "approximate"));
    free(json);

    /* Large, nothing stored: approximate size only. */
    resolved = false;
    json = cbm_mcp_session_brief_json(srv, "bigproj", &resolved);
    ASSERT_TRUE(resolved);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"approximate\":true"));
    free(json);
    cbm_mcp_server_free(srv);

    /* Stored: returned verbatim for any size, no live computation. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/bigproj.db", env.dir);
    cbm_store_t *st = cbm_store_open_path(path);
    ASSERT_NOT_NULL(st);
    char *missing = nullptr;
    ASSERT_EQ(cbm_store_session_brief_get(st, "bigproj", &missing), CBM_STORE_NOT_FOUND);
    ASSERT_NULL(missing);
    ASSERT_TRUE(cbm_store_node_id_ceiling(st) >= CBM_MCP_BRIEF_LIVE_MAX_NODES);
    const char *stored = "{\"nodes\":2200000,\"edges\":4100000,\"central\":[{\"name\":\"stored_marker\","
                         "\"in_degree\":9,\"file_path\":\"x.cpp\"}]}";
    ASSERT_EQ(cbm_store_session_brief_put(st, "bigproj", stored), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_session_brief_put(st, "bigproj", stored), CBM_STORE_OK); /* upsert */
    cbm_store_close(st);

    srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_mcp_server_set_scan_fallback(srv, false);
    json = cbm_mcp_session_brief_json(srv, "bigproj", &resolved);
    ASSERT_NOT_NULL(json);
    ASSERT_STR_EQ(json, stored);
    free(json);

    /* Unindexed project: not resolved, so the hook climbs. */
    resolved = true;
    ASSERT_NULL(cbm_mcp_session_brief_json(srv, "no-such-project", &resolved));
    ASSERT_FALSE(resolved);
    ASSERT_NULL(cbm_mcp_session_brief_json(NULL, "bigproj", NULL));
    cbm_mcp_server_free(srv);
    hf_env_close(&env);
    PASS();
}

TEST(hook_session_brief_describes_bounded_evidence) {
#ifdef CBM_ENABLE_TEST_SEAMS
    const char *inputs[] = {
        R"({"nodes":120,"edges":340,"languages":[{"language":"C++","file_count":9}]})",
        R"({"nodes":2200000,"approximate":true})",
    };
    for (const char *input : inputs) {
        char *text = cbm_session_brief_format_for_testing("proj", input);
        ASSERT_NOT_NULL(text);
        ASSERT_NOT_NULL(strstr(text, "bounded evidence, not a completeness guarantee"));
        ASSERT_NOT_NULL(strstr(text, "independent verification could change the answer"));
        ASSERT_NULL(strstr(text, "already contains the whole-word matches"));
        ASSERT_NULL(strstr(text, "same name again returns the same lines"));
        free(text);
    }
#endif
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

/* Structural prompt evidence fails closed when the index reports a changed
 * source family or when an older index has no freshness metadata. */
TEST(hook_prompt_freshness_requires_explicit_current_index) {
#ifdef CBM_ENABLE_TEST_SEAMS
    ASSERT_TRUE(
        cbm_prompt_freshness_for_testing(R"({"index":{"file_modified_after_index":false}})"));
    ASSERT_FALSE(
        cbm_prompt_freshness_for_testing(R"({"index":{"file_modified_after_index":true}})"));
    ASSERT_FALSE(cbm_prompt_freshness_for_testing(R"({"index":{}})"));
    ASSERT_FALSE(cbm_prompt_freshness_for_testing(R"({})"));
    ASSERT_FALSE(cbm_prompt_freshness_for_testing(nullptr));
    ASSERT_NULL(cbm_hook_symbol_brief_for_testing(
        R"({"symbol":{"label":"Function","file":"old.cpp","start_line":4,"end_line":8},"index":{"file_modified_after_index":true},"callers_total":9})",
        "old_fn"));
    ASSERT_NULL(cbm_hook_symbol_brief_for_testing(
        R"({"symbol":{"label":"Function","file":"unknown.cpp","start_line":4,"end_line":8},"callers_total":9})",
        "unknown_fn"));
    char *brief = cbm_hook_symbol_brief_for_testing(
        R"({"symbol":{"label":"Function","file":"fresh.cpp","start_line":4,"end_line":8},"index":{"file_modified_after_index":false},"callers_total":0,"caller_files":[]})",
        "fresh_fn");
    ASSERT_NOT_NULL(brief);
    free(brief);
#endif
    PASS();
}

/* A trace edge's callee name must still appear outside recognized comments
 * and strings. These lexical checks are corroboration, not proof of a call. */
TEST(hook_prompt_chain_suppresses_stale_source_edges) {
#ifdef CBM_ENABLE_TEST_SEAMS
    const char *made = th_mktempdir("cbm-hook-chain");
    if (!made) {
        PASS();
    }
    char *root = strdup(made);
    ASSERT_NOT_NULL(root);
    char path[1024];
    snprintf(path, sizeof(path), "%s/chain.cpp", root);
    const char *trace =
        R"({"path_found":true,"path":[)"
        R"({"name":"start_fn","qualified_name":"p.chain.start_fn","file":"chain.cpp","start_line":1,"end_line":3},)"
        R"({"name":"target_fn","qualified_name":"p.chain.target_fn","file":"chain.cpp","start_line":4,"end_line":4}],)"
        R"("caller_edges":[{"to_step":1,"line":2}]})";

    FILE *fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  target_fn();\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    char *block = cbm_prompt_chain_for_testing(root, trace);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "not a current shortest-path claim"));
    ASSERT_NULL(strstr(block, "shortest: the chain"));
    free(block);

    fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  // target_fn();\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, trace));

    fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  log(\"target_fn()\");\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, trace));

    fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  log('target_fn()');\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, trace));

    fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  int x = 0; /* target_fn(); */\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, trace));

    fp = fopen(path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("void start_fn() {\n  /* stale edge\n  target_fn(); */\n}\nvoid target_fn() {}\n", fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, trace));

    ASSERT_EQ(cbm_unlink(path), 0);

    char py_path[1024];
    snprintf(py_path, sizeof(py_path), "%s/chain.py", root);
    const char *py_trace =
        R"({"path_found":true,"path":[)"
        R"({"name":"start_fn","qualified_name":"p.chain.start_fn","file":"chain.py","start_line":1,"end_line":4},)"
        R"({"name":"target_fn","qualified_name":"p.chain.target_fn","file":"chain.py","start_line":5,"end_line":6}],)"
        R"("caller_edges":[{"to_step":1,"line":3}]})";
    fp = fopen(py_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs(
        "def start_fn():\n  value = 1  # target_fn()\n  return value\n\ndef target_fn():\n  pass\n",
        fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, py_trace));

    fp = fopen(py_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs(
        "def start_fn():\n  \"\"\"stale edge\n  target_fn()\n  \"\"\"\ndef target_fn():\n  pass\n",
        fp);
    fclose(fp);
    ASSERT_NULL(cbm_prompt_chain_for_testing(root, py_trace));
    ASSERT_EQ(cbm_unlink(py_path), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    free(root);
#endif
    PASS();
}

TEST(hook_prompt_chain_uses_bare_path_and_preserves_qualified_identities) {
#ifdef CBM_ENABLE_TEST_SEAMS
    const char *made = th_mktempdir("cbm-hook-chain-names");
    if (!made) {
        PASS();
    }
    char *root = strdup(made);
    ASSERT_NOT_NULL(root);
    char django_path[1024];
    char core_path[1024];
    char utils_path[1024];
    char signing_path[1024];
    char crypto_path[1024];
    char duplicate_path[1024];
    snprintf(django_path, sizeof(django_path), "%s/django", root);
    snprintf(core_path, sizeof(core_path), "%s/django/core", root);
    snprintf(utils_path, sizeof(utils_path), "%s/django/utils", root);
    snprintf(signing_path, sizeof(signing_path), "%s/django/core/signing.py", root);
    snprintf(crypto_path, sizeof(crypto_path), "%s/django/utils/crypto.py", root);
    snprintf(duplicate_path, sizeof(duplicate_path), "%s/duplicate.py", root);
    ASSERT_TRUE(cbm_mkdir_p(core_path, 0755));
    ASSERT_TRUE(cbm_mkdir_p(utils_path, 0755));

    FILE *fp = fopen(signing_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("from django.utils.crypto import salted_hmac\n"
          "def base64_hmac():\n"
          "    return salted_hmac()\n"
          "\n"
          "class Signer:\n"
          "    def signature(self):\n"
          "        return base64_hmac()\n"
          "    def sign(self):\n"
          "        return self.signature()\n",
          fp);
    fclose(fp);
    fp = fopen(crypto_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("def salted_hmac():\n    return 1\n", fp);
    fclose(fp);

    const char *django_trace =
        R"({"path_found":true,"path":[)"
        R"({"name":"sign","qualified_name":"p.django.core.signing.Signer.sign","file":"django/core/signing.py","start_line":8,"end_line":9},)"
        R"({"name":"signature","qualified_name":"p.django.core.signing.Signer.signature","file":"django/core/signing.py","start_line":6,"end_line":7},)"
        R"({"name":"base64_hmac","qualified_name":"p.django.core.signing.base64_hmac","file":"django/core/signing.py","start_line":2,"end_line":3},)"
        R"({"name":"salted_hmac","qualified_name":"p.django.utils.crypto.salted_hmac","file":"django/utils/crypto.py","start_line":1,"end_line":2}],)"
        R"("caller_edges":[{"to_step":1,"line":9},{"to_step":2,"line":7},{"to_step":3,"line":3}]})";
    char *block = cbm_prompt_chain_for_testing(root, django_trace);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(
        block, "- call chain sign -> salted_hmac (3 hops): sign (django/core/signing.py:8) -> "
               "signature (django/core/signing.py:6) -> base64_hmac "
               "(django/core/signing.py:2) -> salted_hmac (django/utils/crypto.py:1)"));
    ASSERT_NOT_NULL(strstr(block,
                           "  qualified identities: Signer.sign (django/core/signing.py:8) -> "
                           "Signer.signature (django/core/signing.py:6) -> base64_hmac "
                           "(django/core/signing.py:2) -> salted_hmac (django/utils/crypto.py:1)"));
    ASSERT_NOT_NULL(strstr(block, "\n    sign -> signature: django/core/signing.py:9:"));
    ASSERT_NOT_NULL(strstr(block, "\n    signature -> base64_hmac: django/core/signing.py:7:"));
    ASSERT_NULL(strstr(block, "\n    Signer.sign -> Signer.signature:"));

    /* The payload keeps newline-delimited prefixes. Derive the exact rendered
     * size of the primary line, then prove that exact fit survives while the
     * next line is dropped; one byte less cannot retain even the primary. */
    const char *first_newline = strchr(block, '\n');
    ASSERT_NOT_NULL(first_newline);
    size_t primary_len = (size_t)(first_newline - block);
    char *primary = (char *)malloc(primary_len + 1);
    ASSERT_NOT_NULL(primary);
    memcpy(primary, block, primary_len);
    primary[primary_len] = '\0';
    const char *label = "[code-cortex] bounded evidence:";
    const char *primary_blocks[] = {primary};
    char *primary_json = cbm_prompt_payload_for_testing(primary_blocks, 1, 9000, label);
    ASSERT_NOT_NULL(primary_json);
    size_t primary_cap = strlen(primary_json);
    free(primary_json);
    const char *full_blocks[] = {block};
    char *capped = cbm_prompt_payload_for_testing(full_blocks, 1, primary_cap, label);
    ASSERT_NOT_NULL(capped);
    ASSERT_TRUE(strlen(capped) <= primary_cap);
    ASSERT_NOT_NULL(strstr(capped, "call chain sign -> salted_hmac"));
    ASSERT_NOT_NULL(strstr(capped, "django/core/signing.py:8"));
    ASSERT_NOT_NULL(strstr(capped, "django/utils/crypto.py:1"));
    ASSERT_NULL(strstr(capped, "qualified identities"));
    free(capped);
    ASSERT_NULL(cbm_prompt_payload_for_testing(full_blocks, 1, primary_cap - 1, label));
    free(primary);
    free(block);

    /* Repeated bare endpoint names stay positional and unambiguous through
     * the complete cleaned identity line. */
    fp = fopen(duplicate_path, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("class A:\n"
          "    def run(self):\n"
          "        return bridge()\n"
          "\n"
          "def bridge():\n"
          "    return B().run()\n"
          "\n"
          "class B:\n"
          "    def run(self):\n"
          "        return 0\n",
          fp);
    fclose(fp);
    const char *duplicate_trace =
        R"({"path_found":true,"path":[)"
        R"({"name":"run","qualified_name":"p.duplicate.A.run","file":"duplicate.py","start_line":2,"end_line":3},)"
        R"({"name":"bridge","qualified_name":"p.duplicate.bridge","file":"duplicate.py","start_line":5,"end_line":6},)"
        R"({"name":"run","qualified_name":"p.duplicate.B.run","file":"duplicate.py","start_line":9,"end_line":10}],)"
        R"("caller_edges":[{"to_step":1,"line":3},{"to_step":2,"line":6}]})";
    block = cbm_prompt_chain_for_testing(root, duplicate_trace);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "- call chain run -> run (2 hops): run (duplicate.py:2) -> "
                                  "bridge (duplicate.py:5) -> run (duplicate.py:9)"));
    ASSERT_NOT_NULL(strstr(block, "  qualified identities: A.run (duplicate.py:2) -> bridge "
                                  "(duplicate.py:5) -> B.run (duplicate.py:9)"));
    free(block);

    ASSERT_EQ(cbm_unlink(signing_path), 0);
    ASSERT_EQ(cbm_unlink(crypto_path), 0);
    ASSERT_EQ(cbm_unlink(duplicate_path), 0);
    ASSERT_EQ(cbm_rmdir(core_path), 0);
    ASSERT_EQ(cbm_rmdir(utils_path), 0);
    ASSERT_EQ(cbm_rmdir(django_path), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    free(root);
#endif
    PASS();
}

TEST(hook_prompt_context_preserves_explicit_verification_requests) {
#ifdef CBM_ENABLE_TEST_SEAMS
    for (int mode = 0; mode < 2; ++mode) {
        char *label = cbm_prompt_label_for_testing(mode != 0);
        ASSERT_NOT_NULL(label);
        ASSERT_NOT_NULL(strstr(label, "Follow explicit user requests"));
        ASSERT_NOT_NULL(strstr(label, "independent verification or specific tools"));
        ASSERT_NOT_NULL(strstr(label, "requested output format"));
        ASSERT_NOT_NULL(strstr(label, "requests for no commentary"));
        ASSERT_NOT_NULL(strstr(label, "do not prove the current target binding"));
        ASSERT_NOT_NULL(strstr(label, "may have changed since submission"));
        ASSERT_NULL(strstr(label, "search only for what is not shown"));
        ASSERT_NULL(strstr(label, "returns these same lines"));
        free(label);
    }
#endif
    PASS();
}

TEST(hook_prompt_caller_evidence_keeps_lexical_and_binding_uncertainty) {
#ifdef CBM_ENABLE_TEST_SEAMS
    const char *made = th_mktempdir("cbm-hook-caller-evidence");
    if (!made) {
        PASS();
    }
    char *root = strdup(made);
    ASSERT_NOT_NULL(root);
    char lexical[1024], formatted[1024], import_use[1024], multiline[1024], shapes[1024];
    char namespaces[1024], capped[1024];
    snprintf(lexical, sizeof(lexical), "%s/lexical.h", root);
    snprintf(formatted, sizeof(formatted), "%s/formatted.py", root);
    snprintf(import_use, sizeof(import_use), "%s/import_use.py", root);
    snprintf(multiline, sizeof(multiline), "%s/multiline.js", root);
    snprintf(shapes, sizeof(shapes), "%s/shapes.cpp", root);
    snprintf(namespaces, sizeof(namespaces), "%s/namespaces.cpp", root);
    snprintf(capped, sizeof(capped), "%s/capped.cpp", root);

    FILE *fp = fopen(lexical, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("/*\n"
          " * probe_fn();\n"
          " */\n"
          "const char *s = \"probe_fn()\";\n"
          "void use() { const char *t = \"probe_fn()\"; probe_fn(); }\n"
          "int probe_fn(int);\n",
          fp);
    fclose(fp);
    fp = fopen(formatted, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("note = f\"\"\"\nprobe_fn()\n\"\"\"\n"
          "escaped = \"\134probe_fn\"\n"
          "probe_fn()\n",
          fp);
    fclose(fp);
    fp = fopen(import_use, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("from module import probe_fn; probe_fn()\n", fp);
    fclose(fp);
    fp = fopen(multiline, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("const note = `first line\nprobe_fn()\n`;\nprobe_fn();\n", fp);
    fclose(fp);
    const char *lex_files[] = {"lexical.h", "formatted.py", "import_use.py", "multiline.js"};
    const char *lex_facts =
        R"({"display":"probe_fn","bare":"probe_fn","label":"Function",)"
        R"("decls":[["lexical.h",6]],"calls":[["lexical.h",4]]})";
    char *block = cbm_prompt_evidence_for_testing(root, lex_facts, lex_files, 4, 0);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "lexical.h:2: * probe_fn();  [comment occurrence]"));
    ASSERT_NOT_NULL(strstr(block,
                           "lexical.h:4: const char *s = \"probe_fn()\";  [string occurrence]"));
    ASSERT_NOT_NULL(strstr(block, "lexical.h:5: void use() { const char *t = \"probe_fn()\"; "
                                  "probe_fn(); }  [unresolved call shape; string occurrence]"));
    ASSERT_NOT_NULL(strstr(block, "formatted.py:2: probe_fn()  [string occurrence]"));
    ASSERT_NOT_NULL(strstr(block,
                           "formatted.py:4: escaped = \"\\probe_fn\"  [string occurrence]"));
    ASSERT_NOT_NULL(strstr(block, "formatted.py:5: probe_fn()  [unresolved call shape]"));
    ASSERT_NOT_NULL(strstr(block, "import_use.py:1: from module import probe_fn; probe_fn()  "
                                  "[unresolved call shape; import]"));
    ASSERT_NOT_NULL(strstr(block, "multiline.js:2: probe_fn()  [string occurrence]"));
    ASSERT_NOT_NULL(strstr(block, "multiline.js:4: probe_fn();  [unresolved call shape]"));
    ASSERT_NULL(strstr(block, "[indexed edge candidate; binding not re-resolved]"));
    ASSERT_NULL(strstr(block, " calls="));
    free(block);

    fp = fopen(shapes, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("std::vector<double> shape_fn(4);\n"
          "auto value = shape_fn<T>(mesh);\n",
          fp);
    fclose(fp);
    const char *shape_files[] = {"shapes.cpp"};
    block = cbm_prompt_evidence_for_testing(
        root, R"({"display":"shape_fn","bare":"shape_fn","label":"Function"})", shape_files,
        1, 0);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "shapes.cpp:1: std::vector<double> shape_fn(4);  [unresolved "
                                  "call shape]"));
    ASSERT_NOT_NULL(strstr(block, "shapes.cpp:2: auto value = shape_fn<T>(mesh);  [unresolved "
                                  "call shape]"));
    ASSERT_NOT_NULL(strstr(block, "unresolved_call_shapes=2"));
    free(block);

    fp = fopen(namespaces, "wb");
    ASSERT_NOT_NULL(fp);
    fputs("namespace cv { int borderInterpolate(int); }\n"
          "namespace cvtest {\n"
          "static int borderInterpolate(int x) { return x; }\n"
          "int local() { return borderInterpolate(1); }\n"
          "int target() { return cv::borderInterpolate(1); }\n"
          "}\n",
          fp);
    fclose(fp);
    const char *namespace_files[] = {"namespaces.cpp"};
    block = cbm_prompt_evidence_for_testing(
        root,
        R"({"display":"cv::borderInterpolate","bare":"borderInterpolate","label":"Function","decls":[["namespaces.cpp",1]],"calls":[["namespaces.cpp",4]]})",
        namespace_files, 1, 2);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "namespaces.cpp:4: int local() { return borderInterpolate(1); }  "
                                  "[indexed edge candidate; binding not re-resolved]"));
    ASSERT_NOT_NULL(strstr(block, "namespaces.cpp:5: int target() { return "
                                  "cv::borderInterpolate(1); }  [unresolved call shape]"));
    ASSERT_NOT_NULL(strstr(block, "indexed_edge_candidates=1 unresolved_call_shapes=2"));
    ASSERT_NOT_NULL(strstr(block, "do not prove the current target binding"));
    free(block);

    fp = fopen(capped, "wb");
    ASSERT_NOT_NULL(fp);
    for (int i = 0; i < 201; ++i) {
        fprintf(fp, "void cap_%d() { cap_probe(); }\n", i);
    }
    fclose(fp);
    const char *cap_files[] = {"capped.cpp"};
    block = cbm_prompt_evidence_for_testing(
        root, R"({"display":"cap_probe","bare":"cap_probe","label":"Function"})", cap_files,
        1, 2);
    ASSERT_NOT_NULL(block);
    ASSERT_NOT_NULL(strstr(block, "per-file match cap reached in 1 file(s)"));
    ASSERT_NOT_NULL(strstr(block, "occurrence counts and line lists are lower bounds"));
    free(block);

    ASSERT_EQ(cbm_unlink(lexical), 0);
    ASSERT_EQ(cbm_unlink(formatted), 0);
    ASSERT_EQ(cbm_unlink(import_use), 0);
    ASSERT_EQ(cbm_unlink(multiline), 0);
    ASSERT_EQ(cbm_unlink(shapes), 0);
    ASSERT_EQ(cbm_unlink(namespaces), 0);
    ASSERT_EQ(cbm_unlink(capped), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    free(root);
#endif
    PASS();
}

void suite_hook_fastpath(void) {
    RUN_TEST(hook_scan_fallback_off_resolves_from_the_memo_only);
    RUN_TEST(hook_scan_fallback_defaults_on_and_toggles_back);
    RUN_TEST(hook_coverage_note_reports_partial_skipped_and_clean);
    RUN_TEST(hook_coverage_note_on_unindexed_tree_is_a_scan_free_miss);
    RUN_TEST(hook_session_brief_reads_stored_inputs_and_bounds_misses);
    RUN_TEST(hook_session_brief_describes_bounded_evidence);
    RUN_TEST(hook_prompt_freshness_requires_explicit_current_index);
    RUN_TEST(hook_prompt_chain_suppresses_stale_source_edges);
    RUN_TEST(hook_prompt_chain_uses_bare_path_and_preserves_qualified_identities);
    RUN_TEST(hook_prompt_context_preserves_explicit_verification_requests);
    RUN_TEST(hook_prompt_caller_evidence_keeps_lexical_and_binding_uncertainty);
}
