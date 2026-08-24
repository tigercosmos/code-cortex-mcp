/* store_meta — on-disk memo of per-database facts. See store_meta.h. */
#include "store_meta.h"

#include "foundation/platform.h" /* cbm_resolve_cache_dir */
#include "store/store.h"         /* cbm_integrity_verdict_t */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SM_BUSY_TIMEOUT_MS = 50, /* a locked _config.db is a miss, not a stall */
    SM_PATH_MAX = 2048,
    SM_COL_DB_NAME = 1,
    SM_COL_SIZE = 2,
    SM_COL_MTIME = 3,
    SM_COL_MTIME_NS = 4,
    SM_COL_INO = 5,
    SM_COL_PROJECT = 6,
    SM_COL_ROOT_PATH = 7,
    SM_COL_NODES = 8,
    SM_COL_EDGES = 9,
    SM_COL_VERDICT = 10,
};

struct cbm_store_meta_db {
    sqlite3 *db;
};

static bool sm_disabled(void) {
    const char *e = getenv("CBM_STORE_META");
    return e && strcmp(e, "0") == 0;
}

const char *cbm_store_meta_key(const char *db_path) {
    if (!db_path) {
        return "";
    }
    const char *key = db_path;
    for (const char *p = db_path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            key = p + 1;
        }
    }
    return key;
}

cbm_store_meta_db_t *cbm_store_meta_open(void) {
    if (sm_disabled()) {
        return NULL;
    }
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir || !cdir[0]) {
        return NULL;
    }
    char path[SM_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/_config.db", cdir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return NULL;
    }
    sqlite3 *db = NULL;
    /* READWRITE|CREATE mirrors cbm_config_open: the file is shared with the
     * config table and may not exist yet on a fresh machine. No directory
     * creation here — a missing cache dir means nothing has been indexed, and
     * creating it from a lookup would be a side effect the caller never asked
     * for. */
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, SM_BUSY_TIMEOUT_MS);
    /* v1 keyed rows on (size, whole-second mtime) only, which a same-second
     * rebuild at the same page count matches — so its rows cannot be trusted
     * and cannot be upgraded in place (they carry no file identity to check).
     * This is a pure cache: drop it and let it refill at the real key. The
     * DROP is a no-op on a fresh machine. */
    const char *ddl = "DROP TABLE IF EXISTS store_meta;"
                      "CREATE TABLE IF NOT EXISTS store_meta_v2 ("
                      "db_name TEXT PRIMARY KEY,"
                      "size INTEGER NOT NULL,"
                      "mtime INTEGER NOT NULL,"
                      "mtime_ns INTEGER NOT NULL DEFAULT 0,"
                      "ino INTEGER NOT NULL DEFAULT 0,"
                      "project TEXT NOT NULL DEFAULT '',"
                      "root_path TEXT NOT NULL DEFAULT '',"
                      "nodes INTEGER NOT NULL DEFAULT -1,"
                      "edges INTEGER NOT NULL DEFAULT -1,"
                      "verdict INTEGER NOT NULL DEFAULT -1,"
                      "updated_at INTEGER NOT NULL DEFAULT 0);"
                      "CREATE INDEX IF NOT EXISTS idx_store_meta_project "
                      "ON store_meta_v2(project);";
    if (sqlite3_exec(db, ddl, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    cbm_store_meta_db_t *h = (cbm_store_meta_db_t *)calloc(1, sizeof(*h));
    if (!h) {
        sqlite3_close(db);
        return NULL;
    }
    h->db = db;
    return h;
}

void cbm_store_meta_close(cbm_store_meta_db_t *h) {
    if (!h) {
        return;
    }
    if (h->db) {
        sqlite3_close(h->db);
    }
    free(h);
}

void cbm_store_meta_row_init(cbm_store_meta_row_t *row, const char *db_path,
                             const cbm_file_gen_t *gen) {
    memset(row, 0, sizeof(*row));
    snprintf(row->db_name, sizeof(row->db_name), "%s", cbm_store_meta_key(db_path));
    if (gen) {
        row->gen = *gen;
    } else {
        row->gen.size = -1;
    }
    row->nodes = CBM_STORE_META_UNKNOWN;
    row->edges = CBM_STORE_META_UNKNOWN;
    row->verdict = CBM_STORE_META_UNKNOWN;
}

/* Copy one result row of the canonical SELECT (see sm_select_by_*) into `row`. */
static void sm_load_row(sqlite3_stmt *stmt, cbm_store_meta_row_t *row) {
    memset(row, 0, sizeof(*row));
    const char *s = (const char *)sqlite3_column_text(stmt, SM_COL_DB_NAME - 1);
    snprintf(row->db_name, sizeof(row->db_name), "%s", s ? s : "");
    row->gen.size = sqlite3_column_int64(stmt, SM_COL_SIZE - 1);
    row->gen.mtime = sqlite3_column_int64(stmt, SM_COL_MTIME - 1);
    row->gen.mtime_ns = sqlite3_column_int64(stmt, SM_COL_MTIME_NS - 1);
    row->gen.ino = (uint64_t)sqlite3_column_int64(stmt, SM_COL_INO - 1);
    s = (const char *)sqlite3_column_text(stmt, SM_COL_PROJECT - 1);
    snprintf(row->project, sizeof(row->project), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(stmt, SM_COL_ROOT_PATH - 1);
    snprintf(row->root_path, sizeof(row->root_path), "%s", s ? s : "");
    row->nodes = sqlite3_column_int(stmt, SM_COL_NODES - 1);
    row->edges = sqlite3_column_int(stmt, SM_COL_EDGES - 1);
    row->verdict = sqlite3_column_int(stmt, SM_COL_VERDICT - 1);
}

#define SM_SELECT_COLS \
    "db_name, size, mtime, mtime_ns, ino, project, root_path, nodes, edges, verdict"

bool cbm_store_meta_get(cbm_store_meta_db_t *h, const char *db_path, const cbm_file_gen_t *gen,
                        cbm_store_meta_row_t *row) {
    if (!h || !h->db || !db_path || !row || !gen || gen->size < 0) {
        return false; /* an unstattable file has no generation to match */
    }
    const char *key = cbm_store_meta_key(db_path);
    if (!key[0]) {
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT " SM_SELECT_COLS " FROM store_meta_v2 "
                      "WHERE db_name = ?1 AND size = ?2 AND mtime = ?3 "
                      "AND mtime_ns = ?4 AND ino = ?5;";
    if (sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, gen->size);
    sqlite3_bind_int64(stmt, 3, gen->mtime);
    sqlite3_bind_int64(stmt, 4, gen->mtime_ns);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)gen->ino);
    bool hit = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sm_load_row(stmt, row);
        hit = true;
    }
    sqlite3_finalize(stmt);
    return hit;
}

bool cbm_store_meta_find_project(cbm_store_meta_db_t *h, const char *project,
                                 cbm_store_meta_row_t *row) {
    if (!h || !h->db || !project || !project[0] || !row) {
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    /* Newest generation first: a project re-indexed under a drifted file name
     * may briefly have two rows (the old file is stale, which the caller's
     * (size, mtime) check rejects), so prefer the most recently recorded one. */
    const char *sql = "SELECT " SM_SELECT_COLS " FROM store_meta_v2 "
                      "WHERE project = ?1 ORDER BY updated_at DESC LIMIT 1;";
    if (sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    bool hit = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sm_load_row(stmt, row);
        hit = true;
    }
    sqlite3_finalize(stmt);
    return hit;
}

bool cbm_store_meta_put(cbm_store_meta_db_t *h, const cbm_store_meta_row_t *row) {
    if (!h || !h->db || !row || !row->db_name[0]) {
        return false;
    }
    int verdict = row->verdict;
    if (verdict == (int)CBM_INTEGRITY_TRANSIENT) {
        verdict = CBM_STORE_META_UNKNOWN; /* "ask again" must never become sticky */
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO store_meta_v2 (db_name, size, mtime, mtime_ns, ino, project, root_path, "
        "nodes, edges, verdict, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, strftime('%s','now')) "
        "ON CONFLICT(db_name) DO UPDATE SET size = excluded.size, mtime = excluded.mtime, "
        "mtime_ns = excluded.mtime_ns, ino = excluded.ino, "
        "project = excluded.project, root_path = excluded.root_path, nodes = excluded.nodes, "
        "edges = excluded.edges, verdict = excluded.verdict, updated_at = excluded.updated_at;";
    if (sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, row->db_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, row->gen.size);
    sqlite3_bind_int64(stmt, 3, row->gen.mtime);
    sqlite3_bind_int64(stmt, 4, row->gen.mtime_ns);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)row->gen.ino);
    sqlite3_bind_text(stmt, 6, row->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, row->root_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, row->nodes);
    sqlite3_bind_int(stmt, 9, row->edges);
    sqlite3_bind_int(stmt, 10, verdict);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void cbm_store_meta_forget(cbm_store_meta_db_t *h, const char *db_path) {
    if (!h || !h->db || !db_path) {
        return;
    }
    const char *key = cbm_store_meta_key(db_path);
    if (!key[0]) {
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h->db, "DELETE FROM store_meta_v2 WHERE db_name = ?1;", -1, &stmt,
                           NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
