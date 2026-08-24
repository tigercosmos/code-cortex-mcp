/* store_meta — on-disk memo of per-database facts that are expensive to
 * recompute: the integrity verdict (PRAGMA quick_check walks the whole
 * btree: ~45ms on a 54MB store, ~400ms on a 100MB one), the internal project
 * name and root path (which otherwise require opening the file), and the
 * node/edge counts list_projects reports.
 *
 * Every row is keyed on the database FILE NAME inside the cache directory and
 * stamped with the file's GENERATION (cbm_file_gen_t: size, mtime to the
 * nanosecond, and inode). A lookup is a hit only while all of them still
 * match, so any rewrite of the file — a re-index, a copy from a teammate,
 * damage — invalidates it without anyone having to remember to. Size and
 * whole-second mtime alone would not: SQLite file sizes are page-granular, so
 * a database rebuilt within the same second at the same page count matches
 * exactly, and the stale row survives the rewrite that should have killed it.
 * TRANSIENT verdicts are never stored ("ask again" must not become sticky).
 *
 * The memo is shared by every process that opens stores: the MCP server, its
 * tool workers, and the hook-augment process spawned per Grep/Glob/Read. That
 * is the point — the in-memory memo in cbm_mcp_server_t dies with each
 * worker, and before this table existed every one of them re-ran quick_check.
 *
 * Storage: table `store_meta` in <cache_dir>/_config.db (the same file the
 * `config` key/value table lives in). Failure to open, read, or write is a
 * cache miss, never an error: callers fall back to computing the fact.
 * CBM_STORE_META=0 disables the memo entirely (every lookup misses). */
#ifndef CBM_STORE_META_H
#define CBM_STORE_META_H

#include <stdbool.h>
#include <stdint.h>

#include "foundation/platform.h" /* cbm_file_gen_t */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CBM_STORE_META_NAME_MAX = 1024,
    CBM_STORE_META_PATH_MAX = 1024,
    CBM_STORE_META_UNKNOWN = -1, /* nodes/edges/verdict not recorded */
};

typedef struct {
    char db_name[CBM_STORE_META_NAME_MAX];   /* file name in the cache dir, e.g. "foo.db" */
    cbm_file_gen_t gen;                      /* file generation the facts were computed at */
    char project[CBM_STORE_META_NAME_MAX];   /* internal project name; "" if unknown */
    char root_path[CBM_STORE_META_PATH_MAX]; /* project root; "" if unknown */
    int nodes;                               /* CBM_STORE_META_UNKNOWN if not recorded */
    int edges;                               /* CBM_STORE_META_UNKNOWN if not recorded */
    int verdict;                             /* cbm_integrity_verdict_t, or UNKNOWN */
} cbm_store_meta_row_t;

typedef struct cbm_store_meta_db cbm_store_meta_db_t;

/* Open the memo. Returns NULL when disabled (CBM_STORE_META=0), when the cache
 * directory cannot be resolved, or when _config.db cannot be opened/created.
 * Every other function below accepts NULL and behaves as a miss / no-op, so
 * callers never need to branch on the handle. Cheap (one sqlite open), but
 * callers that touch many rows (list_projects, the fallback scan) should keep
 * one handle for the whole operation. */
cbm_store_meta_db_t *cbm_store_meta_open(void);
void cbm_store_meta_close(cbm_store_meta_db_t *db);

/* Reset `row` to "nothing known" for the file `db_path` at generation `gen`.
 * db_path may be a full path; only its final component is kept as the key. */
void cbm_store_meta_row_init(cbm_store_meta_row_t *row, const char *db_path,
                             const cbm_file_gen_t *gen);

/* Load the row for the file named by `db_path` (full path or bare name) if it
 * exists AND its recorded generation equals `gen`. Returns false — leaving
 * `row` untouched — on a miss or stale row. */
bool cbm_store_meta_get(cbm_store_meta_db_t *db, const char *db_path, const cbm_file_gen_t *gen,
                        cbm_store_meta_row_t *row);

/* Find the row whose recorded internal project name is `project`. The caller
 * must still verify the file's current generation against row->gen before
 * trusting anything in it. Returns false when no row names that project. */
bool cbm_store_meta_find_project(cbm_store_meta_db_t *db, const char *project,
                                 cbm_store_meta_row_t *row);

/* Insert or replace the row. Best-effort: returns false when the write did not
 * happen (busy, read-only cache dir, disabled). A TRANSIENT verdict is stored
 * as UNKNOWN. */
bool cbm_store_meta_put(cbm_store_meta_db_t *db, const cbm_store_meta_row_t *row);

/* Drop the row for `db_path` (full path or bare name). Used when the file is
 * deleted or quarantined. No-op when the row does not exist. */
void cbm_store_meta_forget(cbm_store_meta_db_t *db, const char *db_path);

/* The final path component (after the last '/' or '\\'), i.e. the memo key. */
const char *cbm_store_meta_key(const char *db_path);

#ifdef __cplusplus
}
#endif

#endif /* CBM_STORE_META_H */
