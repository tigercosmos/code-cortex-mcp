/*
 * pass_cross_repo.c — Cross-repo intelligence: match Routes, Channels, and
 * async topics across indexed projects to create CROSS_* edges.
 *
 * For each HTTP_CALLS/ASYNC_CALLS edge in the source project, looks up the
 * target Route QN in other project DBs. For each Channel node with EMITS
 * edges, looks for matching LISTENS_ON in other projects (and vice versa).
 *
 * Edges are written bidirectionally: both source and target project DBs
 * get a CROSS_* edge so the link is visible from either side.
 */
#include "pipeline/pass_cross_repo.h"
#include "pipeline/pipeline_internal.h" // cbm_route_canon_path
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h" // cbm_json_escape, CBM_JSON_EMPTY_OBJECT
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "service_patterns.h" // cbm_service_pattern_http_method (#2768)

#include <sqlite3/sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    CR_PATH_BUF = 1024,
    CR_QN_BUF = 512,
    CR_PROPS_BUF = 2048,
    CR_DB_EXT_LEN = 3, /* strlen(".db") */
    CR_INIT_CAP = 32,
    CR_COL_3 = 3,
    CR_COL_4 = 4,
    CR_SCHEME_SKIP = 3,          /* strlen("://") */
    CR_IPV4_LOOPBACK_PREFIX = 4, /* strlen("127.") */
    CR_IPV6_MAX_GROUPS = 8,
    CR_ROUTE_PREFIX_LEN = 9, /* strlen("__route__") */
    CR_ANY_LEN = 3,          /* strlen("ANY") */
};

#define CR_MS_PER_SEC 1000.0
#define CR_NS_PER_MS 1000000.0

/* TLS buffer for integer-to-string in log calls. */
static CBM_TLS char cr_ibuf[CBM_SZ_32];
static const char *cr_itoa(int v) {
    snprintf(cr_ibuf, sizeof(cr_ibuf), "%d", v);
    return cr_ibuf;
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static const char *cr_cache_dir(void) {
    const char *dir = cbm_resolve_cache_dir();
    return dir ? dir : cbm_tmpdir();
}

static void cr_db_path(const char *project, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%s/%s.db", cr_cache_dir(), project);
}

/* Extract a JSON string property from properties_json.
 * Writes into buf, returns buf on success, NULL on miss. */
static const char *json_str_prop(const char *json, const char *key, char *buf, size_t bufsz) {
    if (!json || !key) {
        return NULL;
    }
    char pat[CBM_SZ_128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *start = strstr(json, pat);
    if (!start) {
        return NULL;
    }
    start += strlen(pat);
    const char *end = strchr(start, '"');
    if (!end) {
        return NULL;
    }
    size_t len = (size_t)(end - start);
    if (len >= bufsz) {
        len = bufsz - SKIP_ONE;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* Build CROSS_* edge properties JSON. */
static void build_cross_props(char *buf, size_t bufsz, const char *target_project,
                              const char *target_function, const char *target_file,
                              const char *url_or_channel, const char *extra_key,
                              const char *extra_val) {
    /* Every value here is read back out of a stored properties blob or a nodes
     * row — url_path/channel_name originate in source string literals, and
     * json_str_prop() stops at the first '"', so an escaped value comes back
     * with a trailing backslash. Re-escape before re-serializing, or the
     * CROSS_HTTP_CALLS url_path (a generated column) is malformed. */
    char esc_proj[CBM_SZ_256];
    char esc_fn[CBM_SZ_256];
    char esc_file[CBM_SZ_512];
    char esc_uc[CBM_SZ_512];
    char esc_ev[CBM_SZ_256];
    cbm_json_escape(esc_proj, sizeof(esc_proj), target_project ? target_project : "");
    cbm_json_escape(esc_fn, sizeof(esc_fn), target_function ? target_function : "");
    cbm_json_escape(esc_file, sizeof(esc_file), target_file ? target_file : "");
    cbm_json_escape(esc_uc, sizeof(esc_uc), url_or_channel ? url_or_channel : "");
    cbm_json_escape(esc_ev, sizeof(esc_ev), extra_val ? extra_val : "");
    int n = snprintf(buf, bufsz,
                     "{\"target_project\":\"%s\",\"target_function\":\"%s\","
                     "\"target_file\":\"%s\"",
                     esc_proj, esc_fn, esc_file);
    if (esc_uc[0] && n > 0 && (size_t)n < bufsz) {
        n += snprintf(buf + n, bufsz - (size_t)n, ",\"%s\":\"%s\"",
                      extra_key ? extra_key : "url_path", esc_uc);
    }
    if (esc_ev[0] && n > 0 && (size_t)n < bufsz) {
        n += snprintf(buf + n, bufsz - (size_t)n, ",\"%s\":\"%s\"",
                      extra_key ? "transport" : "method", esc_ev);
    }
    if (n <= 0 || (size_t)n >= bufsz - SKIP_ONE) {
        /* Truncated: emit the empty object rather than an unterminated blob. */
        snprintf(buf, bufsz, "%s", CBM_JSON_EMPTY_OBJECT);
        return;
    }
    buf[n] = '}';
    buf[n + SKIP_ONE] = '\0';
}

/* Delete all CROSS_* edges for a project from a store. */
/* The project a source-side CROSS_* edge points at: every edge the pass
 * writes into a project's store records the other project as
 * target_project. NULL for a blob that is not JSON or lacks the key (a
 * truncated edge is stored as "{}"). */
#define CR_EDGE_TARGET_SQL \
    "CASE WHEN json_valid(properties) THEN json_extract(properties, '$.target_project') END"
#define CR_CROSS_TYPES_SQL                                                                \
    "type IN ('CROSS_HTTP_CALLS','CROSS_ASYNC_CALLS','CROSS_CHANNEL','CROSS_GRPC_CALLS'," \
    "'CROSS_GRAPHQL_CALLS','CROSS_TRPC_CALLS')"

/* Delete project's CROSS_* edges that point at target. */
static void delete_cross_edges_to(cbm_store_t *store, const char *project, const char *target) {
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *st = NULL;
    if (!db || sqlite3_prepare_v2(db,
                                  "DELETE FROM edges WHERE project = ?1 AND " CR_CROSS_TYPES_SQL
                                  " AND " CR_EDGE_TARGET_SQL " = ?2",
                                  CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        /* Never fall back to the full clean: it would take the edges just
         * rebuilt for earlier targets. Stale edges stay until the next run. */
        cbm_log_warn("cross_repo.clean_failed", "project", project, "target", target);
        return;
    }
    sqlite3_bind_text(st, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, target, CBM_NOT_FOUND, SQLITE_TRANSIENT);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Delete project's CROSS_* edges pointing anywhere but the projects in keep
 * (and those whose target is unknown). */
static void delete_cross_edges_except(cbm_store_t *store, const char *project,
                                      const std::vector<std::string> &keep) {
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *st = NULL;
    if (!db || sqlite3_prepare_v2(db,
                                  "SELECT DISTINCT " CR_EDGE_TARGET_SQL
                                  " FROM edges WHERE project = ?1 AND " CR_CROSS_TYPES_SQL,
                                  CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return;
    }
    sqlite3_bind_text(st, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_TRANSIENT);
    std::vector<std::string> gone;
    bool unknown = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (!t) {
            unknown = true;
        } else if (std::find(keep.begin(), keep.end(), t) == keep.end()) {
            gone.emplace_back(t);
        }
    }
    sqlite3_finalize(st);
    for (const std::string &t : gone) {
        delete_cross_edges_to(store, project, t.c_str());
    }
    if (unknown && sqlite3_prepare_v2(db,
                                      "DELETE FROM edges WHERE project = ?1 AND " CR_CROSS_TYPES_SQL
                                      " AND " CR_EDGE_TARGET_SQL " IS NULL",
                                      CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

/* Insert a CROSS_* edge into a store. Idempotent by construction: the edges
 * table is UNIQUE(source_id, target_id, type) and cbm_store_insert_edge
 * upserts on conflict, so a pair reached from both match directions or on a
 * repeat run never duplicates a row. (#523) */
static void insert_cross_edge(cbm_store_t *store, const char *project, int64_t from_id,
                              int64_t to_id, const char *edge_type, const char *props) {
    cbm_edge_t edge = {
        .project = project,
        .source_id = from_id,
        .target_id = to_id,
        .type = edge_type,
        .properties_json = props,
    };
    cbm_store_insert_edge(store, &edge);
}

/* Strip "scheme://host[:port]" from a stored HTTP_CALLS url, returning the
 * path. url_path property values are stored raw from the call's first string
 * argument, so they can be full URLs ("scheme://host:port/v2/x") — and
 * cbm_route_canon_path only canonicalizes placeholder syntax, never strips
 * authorities. Returns "/" for a URL with no path after the host (a request
 * against the bare base URL targets the root route). (#523) */
static const char *cr_url_path(const char *url) {
    if (!url) {
        return url;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return url; /* already a bare path */
    }
    const char *path_start = strchr(scheme_end + CR_SCHEME_SKIP, '/');
    return path_start ? path_start : "/";
}

/* True when the IPv6 literal s[0..len) (brackets and any %zone stripped) is
 * ::1 in any spelling: zero groups, at most one "::", last group 1. */
static bool cr_ipv6_is_loopback(const char *s, size_t len) {
    const char *zone = (const char *)memchr(s, '%', len);
    if (zone) {
        len = (size_t)(zone - s);
    }
    const char *last = s + len;
    while (last > s && last[-1] != ':') {
        last--;
    }
    if (last == s) {
        return false; /* no colon: not an IPv6 literal */
    }
    size_t last_len = (size_t)(s + len - last);
    size_t zeros = strspn(last, "0");
    if (last_len == 0 || zeros != last_len - SKIP_ONE || last[last_len - SKIP_ONE] != '1') {
        return false;
    }
    int groups = 1;
    int double_colons = 0;
    for (const char *p = s; p < last; p++) {
        if (*p == ':') {
            groups++;
            if (p + SKIP_ONE < s + len && p[SKIP_ONE] == ':') {
                double_colons++;
            }
        } else if (*p != '0') {
            return false;
        }
    }
    return double_colons <= SKIP_ONE && groups <= CR_IPV6_MAX_GROUPS + SKIP_ONE &&
           (double_colons == SKIP_ONE || groups == CR_IPV6_MAX_GROUPS);
}

/* True when url names a host other than this machine: "scheme://host/..."
 * with a host that is not localhost or a loopback address. Such a call goes
 * to another service even when the caller serves the same path itself, so
 * the #1459 caller-local floor must not swallow it. */
static bool cr_url_names_remote_host(const char *url) {
    const char *scheme_end = url ? strstr(url, "://") : NULL;
    if (!scheme_end) {
        return false; /* a bare path: same origin */
    }
    const char *host = scheme_end + CR_SCHEME_SKIP;
    const char *at = strpbrk(host, "@/");
    if (at && *at == '@') {
        host = at + SKIP_ONE; /* drop userinfo */
    }
    /* A bracketed IPv6 literal holds colons: the host runs to the bracket. */
    const char *close = host[0] == '[' ? strchr(host, ']') : NULL;
    size_t len = close ? (size_t)(close - host) + SKIP_ONE : strcspn(host, ":/?#");
    if (len == 0) {
        return false;
    }
    if (close && cr_ipv6_is_loopback(host + SKIP_ONE, len - PAIR_LEN)) {
        return false;
    }
    static const char *const loopback[] = {"localhost", "0.0.0.0"};
    for (const char *name : loopback) {
        if (strlen(name) == len && strncasecmp(host, name, len) == 0) {
            return false;
        }
    }
    /* 127.0.0.0/8 is loopback as a whole, not only 127.0.0.1. */
    if (len > CR_IPV4_LOOPBACK_PREFIX && strncmp(host, "127.", CR_IPV4_LOOPBACK_PREFIX) == 0 &&
        strspn(host, "0123456789.") >= len) {
        return false;
    }
    return true;
}

/* Look up a node's name and file_path by id. */
static void lookup_node_info(struct sqlite3 *db, int64_t node_id, char *name_out, size_t name_sz,
                             char *file_out, size_t file_sz) {
    name_out[0] = '\0';
    file_out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name, file_path FROM nodes WHERE id = ?1", CBM_NOT_FOUND,
                           &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(st, SKIP_ONE, node_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(st, 0);
        const char *fp = (const char *)sqlite3_column_text(st, SKIP_ONE);
        if (nm) {
            snprintf(name_out, name_sz, "%s", nm);
        }
        if (fp) {
            snprintf(file_out, file_sz, "%s", fp);
        }
    }
    sqlite3_finalize(st);
}

/* ── Phase A: HTTP Route matching ────────────────────────────────── */

/* Find a Route node in target_store by QN and return the handler function's
 * node id, name, and file_path via HANDLES edges. Returns 0 if not found. */
static int64_t find_route_handler(cbm_store_t *target_store, const char *route_qn,
                                  char *handler_name, size_t name_sz, char *handler_file,
                                  size_t file_sz) {
    handler_name[0] = '\0';
    handler_file[0] = '\0';
    struct sqlite3 *db = cbm_store_get_db(target_store);
    if (!db) {
        return 0;
    }

    /* Find Route node by QN */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT id FROM nodes WHERE qualified_name = ?1 AND label = 'Route' LIMIT 1",
            CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, route_qn, CBM_NOT_FOUND, SQLITE_STATIC);
    int64_t route_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        route_id = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    if (route_id == 0) {
        return 0;
    }

    /* Follow HANDLES edge to find the handler function */
    if (sqlite3_prepare_v2(db,
                           "SELECT n.id, n.name, n.file_path FROM edges e "
                           "JOIN nodes n ON n.id = e.source_id "
                           "WHERE e.target_id = ?1 AND e.type = 'HANDLES' LIMIT 1",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(s, SKIP_ONE, route_id);
    int64_t handler_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        handler_id = sqlite3_column_int64(s, 0);
        const char *n = (const char *)sqlite3_column_text(s, SKIP_ONE);
        const char *f = (const char *)sqlite3_column_text(s, PAIR_LEN);
        if (n) {
            snprintf(handler_name, name_sz, "%s", n);
        }
        if (f) {
            snprintf(handler_file, file_sz, "%s", f);
        }
    }
    sqlite3_finalize(s);
    return handler_id;
}

/* Segment-wise match of a concrete path against a route template path, where a
 * "{...}" segment in the template matches any single non-empty concrete
 * segment. Both inputs are bare paths (no method prefix, no authority).
 * Leading/trailing slashes are insignificant. Returns true on a full match. */
static bool cr_path_matches_template(const char *concrete, const char *templ) {
    const char *c = concrete;
    const char *t = templ;
    while (*c && *t) {
        if (*c == '/') {
            c++;
        }
        if (*t == '/') {
            t++;
        }
        const char *cseg = c;
        while (*c && *c != '/') {
            c++;
        }
        const char *tseg = t;
        while (*t && *t != '/') {
            t++;
        }
        size_t clen = (size_t)(c - cseg);
        size_t tlen = (size_t)(t - tseg);
        bool t_is_param = (tlen >= PAIR_LEN && tseg[0] == '{' && tseg[tlen - 1] == '}');
        if (!t_is_param) {
            if (clen != tlen || strncmp(cseg, tseg, clen) != 0) {
                return false;
            }
        } else if (clen == 0) {
            return false; /* a parameter never matches an empty segment */
        }
    }
    while (*c == '/') {
        c++;
    }
    while (*t == '/') {
        t++;
    }
    return *c == '\0' && *t == '\0';
}

/* Split a "__route__<METHOD>__<path>" QN into method and path. Returns false
 * for any other QN shape. */
static bool cr_split_route_qn(const char *qn, const char **method, size_t *mlen,
                              const char **path) {
    if (!qn || strncmp(qn, "__route__", CR_ROUTE_PREFIX_LEN) != 0) {
        return false;
    }
    const char *rest = qn + CR_ROUTE_PREFIX_LEN;
    const char *sep = strstr(rest, "__");
    if (!sep) {
        return false;
    }
    *method = rest;
    *mlen = (size_t)(sep - rest);
    *path = sep + PAIR_LEN;
    return true;
}

/* Fallback for when the exact route-QN lookup misses: a concrete client path
 * ("/v2/orders/123") never exact-matches a templated route QN
 * ("__route__GET__/v2/orders/{}"). Enumerate the target store's Route nodes
 * and segment-match the concrete path against each template. On a match, copy
 * the route QN into out_qn and return the handler id; returns 0 on no match.
 *
 * COST: this scans every Route node of the target project once per unmatched
 * HTTP_CALLS edge — O(calls × routes) per project pair. It only runs for edges
 * the exact lookup missed; no row count stops a scan (a cap used to decide
 * which calls were matched at all); revisit with a prepared template index if
 * cross-repo matching ever shows up in profiles. (#523) */
static int64_t find_route_handler_fuzzy(cbm_store_t *target_store, const char *concrete_path,
                                        const char *method, char *out_qn, size_t out_qn_sz,
                                        char *handler_name, size_t name_sz, char *handler_file,
                                        size_t file_sz) {
    struct sqlite3 *db = cbm_store_get_db(target_store);
    if (!db || !concrete_path || !concrete_path[0]) {
        return 0;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT qualified_name FROM nodes WHERE label = 'Route'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    int64_t found = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(s, 0);
        const char *rest = NULL;
        const char *rpath = NULL;
        size_t mlen = 0;
        if (!cr_split_route_qn(qn, &rest, &mlen, &rpath)) {
            continue;
        }
        /* Method gate: the route's method must equal the caller's, or be ANY.
         * A missing caller method matches any route method. */
        if (method && method[0]) {
            bool same_method = (strncmp(rest, method, mlen) == 0 && method[mlen] == '\0');
            bool route_any = (mlen == CR_ANY_LEN && strncmp(rest, "ANY", CR_ANY_LEN) == 0);
            if (!same_method && !route_any) {
                continue;
            }
        }
        if (!cr_path_matches_template(concrete_path, rpath)) {
            continue;
        }
        /* A concrete path can match more than one stored template (e.g. a raw
         * "{id}" variant and its canonical "{}" form). Only accept a Route that
         * actually has a HANDLES edge — the handler is attached to the
         * canonical node. Keep scanning otherwise. */
        int64_t hid =
            find_route_handler(target_store, qn, handler_name, name_sz, handler_file, file_sz);
        if (hid != 0) {
            snprintf(out_qn, out_qn_sz, "%s", qn);
            found = hid;
            break;
        }
    }
    sqlite3_finalize(s);
    return found;
}

/* Emit CROSS_* edge for a route match: forward into source, reverse into target. */
static void emit_cross_route_bidirectional(cbm_store_t *src_store, const char *src_project,
                                           struct sqlite3 *src_db, int64_t caller_id,
                                           int64_t local_route_id, cbm_store_t *tgt_store,
                                           const char *tgt_project, int64_t handler_id,
                                           const char *route_qn, const char *handler_name,
                                           const char *handler_file, const char *url_path,
                                           const char *method, const char *edge_type) {
    /* Forward: caller → local Route in source DB */
    char fwd[CR_PROPS_BUF];
    build_cross_props(fwd, sizeof(fwd), tgt_project, handler_name, handler_file, url_path,
                      "url_path", method);
    insert_cross_edge(src_store, src_project, caller_id, local_route_id, edge_type, fwd);

    /* Reverse: handler → Route in target DB */
    struct sqlite3 *tgt_db = cbm_store_get_db(tgt_store);
    if (!tgt_db) {
        return;
    }
    sqlite3_stmt *rq = NULL;
    if (sqlite3_prepare_v2(tgt_db, "SELECT id FROM nodes WHERE qualified_name = ?1 LIMIT 1",
                           CBM_NOT_FOUND, &rq, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(rq, SKIP_ONE, route_qn, CBM_NOT_FOUND, SQLITE_STATIC);
    int64_t tgt_route_id = 0;
    if (sqlite3_step(rq) == SQLITE_ROW) {
        tgt_route_id = sqlite3_column_int64(rq, 0);
    }
    sqlite3_finalize(rq);
    if (tgt_route_id == 0) {
        return;
    }

    char caller_name[CBM_SZ_256] = {0};
    char caller_file[CBM_SZ_512] = {0};
    lookup_node_info(src_db, caller_id, caller_name, sizeof(caller_name), caller_file,
                     sizeof(caller_file));

    char rev[CR_PROPS_BUF];
    build_cross_props(rev, sizeof(rev), src_project, caller_name, caller_file, url_path, "url_path",
                      method);
    insert_cross_edge(tgt_store, tgt_project, handler_id, tgt_route_id, edge_type, rev);
}

/* The caller project's own handled routes: every Route node with a HANDLES
 * edge, loaded once per match_http_routes call so the caller-local floor
 * below costs one query per project pair instead of one Route scan per call.
 * find_route_handler only ever answers for a Route with a HANDLES edge, so
 * this set holds exactly the routes the target-side resolver could return. */
struct cr_local_routes {
    std::vector<int64_t> ids;
    std::vector<std::string> qns;
};

static void cr_load_local_routes(struct sqlite3 *db, const char *project, cr_local_routes *out) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT DISTINCT r.id, r.qualified_name FROM nodes r "
                           "JOIN edges e ON e.target_id = r.id AND e.type = 'HANDLES' "
                           "WHERE r.project = ?1 AND r.label = 'Route'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(s, SKIP_ONE, project, CBM_NOT_FOUND, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(s, SKIP_ONE);
        out->ids.push_back(sqlite3_column_int64(s, 0));
        out->qns.emplace_back(qn ? qn : "");
    }
    sqlite3_finalize(s);
}

/* Caller-local floor (upstream #1459): a call the caller's own project serves
 * is an in-project call; another project exposing the same path is no
 * evidence of a cross-service call. "Served" is a HANDLES edge on the Route
 * node the HTTP_CALLS points at, or the same exact-method / ANY / template
 * resolution the target lookup uses, applied to the caller's own routes: a
 * method-less fetch() points at the ANY node while the local handler sits on
 * the GET node. */
static bool call_served_locally(const cr_local_routes *local, int64_t route_id, const char *method,
                                const char *curl) {
    if (std::any_of(local->ids.begin(), local->ids.end(),
                    [route_id](int64_t id) { return id == route_id; })) {
        return true;
    }
    for (const std::string &qn : local->qns) {
        const char *rmethod = NULL;
        const char *rpath = NULL;
        size_t mlen = 0;
        if (!cr_split_route_qn(qn.c_str(), &rmethod, &mlen, &rpath)) {
            continue;
        }
        if (method && method[0]) {
            bool same_method = (strncmp(rmethod, method, mlen) == 0 && method[mlen] == '\0');
            bool route_any = (mlen == CR_ANY_LEN && strncmp(rmethod, "ANY", CR_ANY_LEN) == 0);
            if (!same_method && !route_any) {
                continue;
            }
        }
        if (strcmp(rpath, curl) == 0 || cr_path_matches_template(curl, rpath)) {
            return true;
        }
    }
    return false;
}

static int match_http_routes(cbm_store_t *src_store, const char *src_project,
                             cbm_store_t *tgt_store, const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }
    cr_local_routes local;
    cr_load_local_routes(src_db, src_project, &local);

    /* Find all HTTP_CALLS edges in source project */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT e.source_id, e.target_id, e.properties FROM edges e "
                           "WHERE e.project = ?1 AND e.type = 'HTTP_CALLS'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char url_path[CBM_SZ_256] = {0};
        char method[CBM_SZ_32] = {0};
        json_str_prop(props, "url_path", url_path, sizeof(url_path));
        json_str_prop(props, "method", method, sizeof(method));
        /* A client edge stored without a method (an older index, or an emitter
         * that did not type it) still names its verb in the callee
         * (`this.http.get`, `api.patch`): infer it, so it binds the server's
         * verb-specific Route rather than only an ANY one (#2768, upstream
         * c548d8e4). */
        if (!method[0]) {
            char callee[CBM_SZ_256] = {0};
            json_str_prop(props, "callee", callee, sizeof(callee));
            const char *m = callee[0] ? cbm_service_pattern_http_method(callee) : NULL;
            if (m) {
                snprintf(method, sizeof(method), "%s", m);
            }
        }
        if (!url_path[0]) {
            continue;
        }

        /* Build the expected Route QN in the target project (authority-stripped
         * and param-canonicalized so client url_path matches the server handler
         * regardless of base URL and framework placeholder syntax). */
        char route_qn[CR_QN_BUF];
        char cpath[CBM_SZ_256];
        const char *curl = cbm_route_canon_path(cr_url_path(url_path), cpath, sizeof(cpath));
        if (!cr_url_names_remote_host(url_path) &&
            call_served_locally(&local, route_id, method, curl)) {
            continue;
        }
        snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", method[0] ? method : "ANY", curl);

        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id =
            find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                               handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            /* Try without method (ANY) */
            snprintf(route_qn, sizeof(route_qn), "__route__ANY__%s", curl);
            handler_id = find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                                            handler_file, sizeof(handler_file));
        }
        if (handler_id == 0) {
            /* Exact QN lookup missed. A concrete client path ("/v2/orders/123")
             * never exact-matches a templated route ("/v2/orders/{}"), so fall
             * back to segment-wise template matching. (#523) */
            handler_id = find_route_handler_fuzzy(
                tgt_store, curl, method[0] ? method : NULL, route_qn, sizeof(route_qn),
                handler_name, sizeof(handler_name), handler_file, sizeof(handler_file));
        }
        if (handler_id == 0) {
            continue;
        }

        emit_cross_route_bidirectional(src_store, src_project, src_db, caller_id, route_id,
                                       tgt_store, tgt_project, handler_id, route_qn, handler_name,
                                       handler_file, url_path, method, "CROSS_HTTP_CALLS");

        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase B: Async matching ─────────────────────────────────────── */

static int match_async_routes(cbm_store_t *src_store, const char *src_project,
                              cbm_store_t *tgt_store, const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT e.source_id, e.target_id, e.properties FROM edges e "
                           "WHERE e.project = ?1 AND e.type = 'ASYNC_CALLS'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char url_path[CBM_SZ_256] = {0};
        char broker[CBM_SZ_128] = {0};
        json_str_prop(props, "url_path", url_path, sizeof(url_path));
        json_str_prop(props, "broker", broker, sizeof(broker));
        if (!url_path[0]) {
            continue;
        }

        char route_qn[CR_QN_BUF];
        snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", broker[0] ? broker : "async",
                 url_path);

        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id =
            find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                               handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            continue;
        }

        char edge_props[CR_PROPS_BUF];
        build_cross_props(edge_props, sizeof(edge_props), tgt_project, handler_name, handler_file,
                          url_path, "url_path", broker);
        insert_cross_edge(src_store, src_project, caller_id, route_id, "CROSS_ASYNC_CALLS",
                          edge_props);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase C: Channel matching ───────────────────────────────────── */

/* Try to find a matching listener in target DB for a channel name. */
static bool try_match_channel_listener(cbm_store_t *src_store, const char *src_project,
                                       cbm_store_t *tgt_store, const char *tgt_project,
                                       const char *channel_name, const char *transport,
                                       int64_t emitter_id, int64_t channel_id) {
    struct sqlite3 *tgt_db = cbm_store_get_db(tgt_store);
    if (!tgt_db) {
        return false;
    }
    sqlite3_stmt *tq = NULL;
    if (sqlite3_prepare_v2(tgt_db,
                           "SELECT n.id, e.source_id, fn.name, fn.file_path FROM nodes n "
                           "JOIN edges e ON e.target_id = n.id AND e.type = 'LISTENS_ON' "
                           "JOIN nodes fn ON fn.id = e.source_id "
                           "WHERE n.project = ?1 AND n.name = ?2 AND n.label = 'Channel' LIMIT 1",
                           CBM_NOT_FOUND, &tq, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(tq, SKIP_ONE, tgt_project, CBM_NOT_FOUND, SQLITE_STATIC);
    sqlite3_bind_text(tq, PAIR_LEN, channel_name, CBM_NOT_FOUND, SQLITE_STATIC);

    bool matched = false;
    if (sqlite3_step(tq) == SQLITE_ROW) {
        int64_t tgt_channel_id = sqlite3_column_int64(tq, 0);
        int64_t listener_id = sqlite3_column_int64(tq, SKIP_ONE);
        const char *listener_name = (const char *)sqlite3_column_text(tq, PAIR_LEN);
        const char *listener_file = (const char *)sqlite3_column_text(tq, CR_COL_3);

        /* Forward edge: emitter → local Channel */
        char fwd[CR_PROPS_BUF];
        build_cross_props(fwd, sizeof(fwd), tgt_project, listener_name ? listener_name : "",
                          listener_file ? listener_file : "", channel_name, "channel_name",
                          transport);
        insert_cross_edge(src_store, src_project, emitter_id, channel_id, "CROSS_CHANNEL", fwd);

        /* Reverse edge: listener → target Channel */
        char caller_name[CBM_SZ_256] = {0};
        char caller_file[CBM_SZ_512] = {0};
        lookup_node_info(cbm_store_get_db(src_store), emitter_id, caller_name, sizeof(caller_name),
                         caller_file, sizeof(caller_file));

        char rev[CR_PROPS_BUF];
        build_cross_props(rev, sizeof(rev), src_project, caller_name, caller_file, channel_name,
                          "channel_name", transport);
        insert_cross_edge(tgt_store, tgt_project, listener_id, tgt_channel_id, "CROSS_CHANNEL",
                          rev);
        matched = true;
    }
    sqlite3_finalize(tq);
    return matched;
}

static int match_channels(cbm_store_t *src_store, const char *src_project, cbm_store_t *tgt_store,
                          const char *tgt_project) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db,
                           "SELECT DISTINCT n.id, n.name, n.qualified_name, n.properties, "
                           "e.source_id FROM nodes n "
                           "JOIN edges e ON e.target_id = n.id AND e.type = 'EMITS' "
                           "WHERE n.project = ?1 AND n.label = 'Channel'",
                           CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *channel_name = (const char *)sqlite3_column_text(s, SKIP_ONE);
        const char *channel_qn = (const char *)sqlite3_column_text(s, PAIR_LEN);
        if (!channel_name || !channel_qn) {
            continue;
        }
        int64_t channel_id = sqlite3_column_int64(s, 0);
        const char *channel_props = (const char *)sqlite3_column_text(s, CR_COL_3);
        int64_t emitter_id = sqlite3_column_int64(s, CR_COL_4);

        char transport[CBM_SZ_64] = {0};
        json_str_prop(channel_props, "transport", transport, sizeof(transport));

        if (try_match_channel_listener(src_store, src_project, tgt_store, tgt_project, channel_name,
                                       transport, emitter_id, channel_id)) {
            count++;
        }
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Phase D: Generic route-type matcher (gRPC, GraphQL, tRPC) ──── */

/* Look up a node's qualified_name by id. Returns true if found. */
static bool lookup_node_qn(struct sqlite3 *db, int64_t node_id, char *out, size_t out_sz) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT qualified_name FROM nodes WHERE id = ?1", CBM_NOT_FOUND, &st,
                           NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, SKIP_ONE, node_id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        if (qn) {
            snprintf(out, out_sz, "%s", qn);
            found = true;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* Match edges of a given type against Route nodes with a given QN prefix.
 * Reuses the same infrastructure as HTTP/async matching. */
static int match_typed_routes(cbm_store_t *src_store, const char *src_project,
                              cbm_store_t *tgt_store, const char *tgt_project,
                              const char *edge_type, const char *svc_key, const char *method_key,
                              const char *cross_edge_type) {
    struct sqlite3 *src_db = cbm_store_get_db(src_store);
    if (!src_db) {
        return 0;
    }

    char sql[CBM_SZ_256];
    snprintf(sql, sizeof(sql),
             "SELECT e.source_id, e.target_id, e.properties FROM edges e "
             "WHERE e.project = ?1 AND e.type = '%s'",
             edge_type);

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(src_db, sql, CBM_NOT_FOUND, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, SKIP_ONE, src_project, CBM_NOT_FOUND, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t caller_id = sqlite3_column_int64(s, 0);
        int64_t route_id = sqlite3_column_int64(s, SKIP_ONE);
        const char *props = (const char *)sqlite3_column_text(s, PAIR_LEN);

        char svc_val[CBM_SZ_256] = {0};
        char meth_val[CBM_SZ_256] = {0};
        json_str_prop(props, svc_key, svc_val, sizeof(svc_val));
        json_str_prop(props, method_key, meth_val, sizeof(meth_val));
        if (!svc_val[0] && !meth_val[0]) {
            continue;
        }

        /* Look up the Route QN from the target node (already points to the Route). */
        char route_qn[CR_QN_BUF] = {0};
        if (!lookup_node_qn(src_db, route_id, route_qn, sizeof(route_qn))) {
            continue;
        }

        char handler_name[CBM_SZ_256] = {0};
        char handler_file[CBM_SZ_512] = {0};
        int64_t handler_id =
            find_route_handler(tgt_store, route_qn, handler_name, sizeof(handler_name),
                               handler_file, sizeof(handler_file));
        if (handler_id == 0) {
            continue;
        }

        emit_cross_route_bidirectional(src_store, src_project, src_db, caller_id, route_id,
                                       tgt_store, tgt_project, handler_id, route_qn, handler_name,
                                       handler_file, svc_val, svc_key, cross_edge_type);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Collect target projects ─────────────────────────────────────── */

/* When target_projects = ["*"], scan the cache directory for all .db files. */
static int collect_all_projects(char ***out) {
    const char *dir = cr_cache_dir();
    cbm_dir_t *d = cbm_opendir(dir);
    if (!d) {
        *out = NULL;
        return 0;
    }

    int cap = CR_INIT_CAP;
    int count = 0;
    char **projects = (char **)malloc((size_t)cap * sizeof(char *));

    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        /* Internal stores are exact filenames (cbm_is_internal_cache_db).
         * Substring filtering would hide legitimate projects such as
         * orders_config_service or api-wal. */
        if (!cbm_is_project_index_db(ent->name)) {
            continue;
        }
        size_t len = strlen(ent->name);
        if (count >= cap) {
            cap *= PAIR_LEN;
            char **tmp = (char **)realloc(projects, (size_t)cap * sizeof(char *));
            if (!tmp) {
                break;
            }
            projects = tmp;
        }
        /* Strip .db extension */
        projects[count] = (__typeof__(projects[count]))malloc(len - PAIR_LEN);
        memcpy(projects[count], ent->name, len - CR_DB_EXT_LEN);
        projects[count][len - CR_DB_EXT_LEN] = '\0';
        count++;
    }
    cbm_closedir(d);

    *out = projects;
    return count;
}

/* True when `project` is a usable cross-repo input: its store exists, holds
 * that exact project, and carries the schema the read-write open requires.
 * The probe is read-only, so it never creates a ghost .db for a missing
 * project and leaves a pre-#768 store untouched for its reindex. */
static bool cr_project_usable(const char *project) {
    char path[CR_PATH_BUF];
    cr_db_path(project, path, sizeof(path));
    cbm_store_t *store = cbm_store_open_path_query(path);
    if (!store) {
        return false;
    }
    cbm_project_t proj = {};
    bool usable = cbm_store_get_project(store, project, &proj) == CBM_STORE_OK;
    if (usable) {
        cbm_project_free_fields(&proj);
    }
    if (usable && !cbm_store_edges_schema_current(store)) {
        cbm_log_warn("cross_repo.project_unusable", "project", project, "reason",
                     "pre_768_schema_reindex_required");
        usable = false;
    }
    cbm_store_close(store);
    return usable;
}

static void free_project_list(char **projects, int count) {
    for (int i = 0; i < count; i++) {
        free(projects[i]);
    }
    free(projects);
}

/* ── Entry point ─────────────────────────────────────────────────── */

static void (*g_cr_usage_hook)(const char *db_path) = nullptr;

void cbm_cross_repo_set_usage_hook(void (*hook)(const char *db_path)) {
    g_cr_usage_hook = hook;
}

/* A database this pass read and wrote is in use: tell the usage recorder,
 * or a sweep right after could expire it as unused. */
static void cr_note_used(const char *db_path) {
    if (g_cr_usage_hook) {
        g_cr_usage_hook(db_path);
    }
}

cbm_cross_repo_result_t cbm_cross_repo_match(const char *project, const char **target_projects,
                                             int target_count) {
    cbm_cross_repo_result_t result = {0};
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (!project || (target_count > 0 && !target_projects)) {
        result.failed = true;
        return result;
    }
    if (!cr_project_usable(project)) {
        result.failed = true;
        return result;
    }

    /* Resolve target projects */
    char **resolved = NULL;
    int resolved_count = 0;
    bool own_list = false;
    bool wildcard =
        target_count == SKIP_ONE && target_projects[0] && strcmp(target_projects[0], "*") == 0;

    if (wildcard) {
        resolved_count = collect_all_projects(&resolved);
        own_list = true;
    } else {
        resolved = (char **)target_projects;
        resolved_count = target_count;
    }

    /* Validate every target before any write. A named target that is missing,
     * holds another project, or carries a pre-#768 schema fails the run here,
     * before the source's existing CROSS_* edges are deleted (upstream #2133).
     * A wildcard skips such a store instead: it is not a usable target, and
     * one legacy index in the cache must not decide the whole run. */
    std::vector<const char *> targets;
    for (int i = 0; i < resolved_count; i++) {
        const char *tgt = resolved[i];
        if (!tgt) {
            result.failed = true;
            break;
        }
        if (strcmp(tgt, project) == 0) {
            continue; /* skip self */
        }
        if (!cr_project_usable(tgt)) {
            if (wildcard) {
                continue;
            }
            result.failed = true;
            break;
        }
        targets.push_back(tgt);
    }
    /* Nothing but the source itself to match against (a self-only list, or
     * ["*"] in a cache holding only the source). Reporting that as success
     * with projects_scanned:0 is indistinguishable from "these services share
     * no routes", and the source's CROSS_* edges would be wiped below with
     * nothing to rebuild them. Fail before any write (upstream #1133). */
    if (!result.failed && targets.empty()) {
        result.failed = true;
        result.no_targets = true;
    }

    /* Open source project store (read-write), under its reader gate for the
     * whole pass, as the targets are: the cache sweep moves a database only
     * with the gate held exclusive, so it cannot trash the source mid-pass
     * and leave the writes going to the renamed file. A move in progress
     * fails the pass like a missing source. A gate that cannot be had at all
     * leaves the open ungated. */
    char src_path[CR_PATH_BUF];
    cr_db_path(project, src_path, sizeof(src_path));
    cbm_db_lease_t *src_gate = NULL;
    cbm_store_t *src_store = NULL;
    if (!result.failed) {
        if (cbm_db_read_gate_acquire(src_path, false, &src_gate) == CBM_INDEX_BUSY) {
            cbm_log_info("cross_repo.skip", "project", project, "reason", "source_gate_busy");
        } else {
            src_store = cbm_store_open_path_existing(src_path);
            if (!src_store) {
                cbm_db_lease_release(src_gate);
                src_gate = NULL;
            }
        }
    }
    if (!src_store) {
        result.failed = true;
        if (own_list) {
            free_project_list(resolved, resolved_count);
        }
        return result;
    }

    cr_note_used(src_path);

    /* Match against each validated target. The source's existing CROSS_*
     * edges toward a target are cleaned only once that target has opened, so
     * a target that is busy or gone (a cache sweep moving it) keeps its
     * previous edges instead of losing them unrebuilt. After the loop, edges
     * toward projects this run did not target go, as a full clean would
     * have taken them. When no target opens, nothing is written and the
     * pass fails. */
    std::vector<std::string> kept_targets;
    bool cleaned = false;
    for (const char *tgt : targets) {
        kept_targets.emplace_back(tgt);
        char tgt_path[CR_PATH_BUF];
        cr_db_path(tgt, tgt_path, sizeof(tgt_path));

        /* Open target store read-write (for bidirectional edge writes), but
         * only a database that exists, and under its reader gate for as long
         * as the pass uses it. Opened with CREATE and no gate, a target the
         * cache expiry sweep moved to trash mid-pass took edges into the
         * trashed file, and one moved before the open came back as an empty
         * new database, which then made `cache restore` refuse. A gate held
         * exclusive (a move in progress) or a missing target: skip it. A gate
         * that cannot be had at all (read-only cache, no lock file) leaves the
         * open ungated, as for a reader: the sweep cannot take it either. */
        cbm_db_lease_t *gate = NULL;
        int grc = cbm_db_read_gate_acquire(tgt_path, false, &gate);
        if (grc == CBM_INDEX_BUSY) {
            cbm_log_info("cross_repo.skip", "project", tgt, "reason", "target_gate_busy");
            continue;
        }
        cbm_store_t *tgt_store = cbm_store_open_path_existing(tgt_path);
        if (!tgt_store) {
            cbm_db_lease_release(gate);
            continue;
        }
        cr_note_used(tgt_path);
        delete_cross_edges_to(src_store, project, tgt);
        cleaned = true;

        result.http_edges += match_http_routes(src_store, project, tgt_store, tgt);
        /* Reverse direction: when this pass runs from the provider side, the
         * consumer's HTTP_CALLS live in tgt, not src — the forward pass above
         * finds nothing because the provider has no outbound calls. This also
         * re-creates the provider-side reverse edges that delete_cross_edges
         * just wiped, which a provider-side run previously destroyed for good.
         * A caller edge lives in exactly one DB, so the two directions scan
         * disjoint edge sets and never double-count a pair; the store's
         * (source, target, type) upsert keeps re-recorded rows unique. (#523) */
        result.http_edges += match_http_routes(tgt_store, tgt, src_store, project);
        result.async_edges += match_async_routes(src_store, project, tgt_store, tgt);
        result.channel_edges += match_channels(src_store, project, tgt_store, tgt);
        result.grpc_edges += match_typed_routes(src_store, project, tgt_store, tgt, "GRPC_CALLS",
                                                "service", "method", "CROSS_GRPC_CALLS");
        result.graphql_edges +=
            match_typed_routes(src_store, project, tgt_store, tgt, "GRAPHQL_CALLS", "operation",
                               "operation", "CROSS_GRAPHQL_CALLS");
        result.trpc_edges += match_typed_routes(src_store, project, tgt_store, tgt, "TRPC_CALLS",
                                                "procedure", "procedure", "CROSS_TRPC_CALLS");
        result.projects_scanned++;

        cbm_store_close(tgt_store);
        cbm_db_lease_release(gate);
    }

    if (cleaned) {
        delete_cross_edges_except(src_store, project, kept_targets);
    } else {
        result.failed = true; /* no target could be opened: nothing written */
    }
    cbm_store_close(src_store);
    cbm_db_lease_release(src_gate);

    if (own_list) {
        free_project_list(resolved, resolved_count);
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    result.elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * CR_MS_PER_SEC) +
                        ((double)(t1.tv_nsec - t0.tv_nsec) / CR_NS_PER_MS);

    int total = result.http_edges + result.async_edges + result.channel_edges + result.grpc_edges +
                result.graphql_edges + result.trpc_edges;
    cbm_log_info("cross_repo.done", "project", project, "total", cr_itoa(total));

    return result;
}
