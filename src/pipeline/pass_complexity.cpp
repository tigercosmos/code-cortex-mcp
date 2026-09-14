/*
 * pass_complexity.c — Interprocedural complexity propagation (Tier B).
 *
 * Tier A (in the extraction walk) stamps each Function/Method node with local
 * structural metrics: complexity (cyclomatic), cognitive, loop_count, loop_depth.
 * This pass propagates loop_depth along CALLS edges to estimate a worst-case
 * *transitive* nested-loop degree: a function with a depth-1 loop that calls an
 * O(n) helper is effectively O(n^2). The estimate assumes calls may occur inside
 * loops (an upper bound) — it is a queryable bottleneck *candidate* signal, not a
 * proof (true big-O is undecidable; cf. SPEED / Loopus). Cycles in the call graph
 * are broken and flagged via a `recursive` property.
 *
 * Writes two extra node properties: transitive_loop_depth, recursive.
 */
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/hash_table.h"
#include "cbm.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

enum { CBM_TLD_MAX_DEPTH = 256 }; /* recursion-depth cap (cycle/stack guard) */

/* Int → string for structured logging (thread-safe ring buffer). */
static const char *itoa_cx(int val) {
    enum { RING = 2, MASK = 1 };
    static CBM_TLS char bufs[RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + 1) & MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* ── Flat JSON property helpers (shared by both passes) ─────────────── */

/* Pointer just past the JSON value starting at p, or NULL if unterminated. */
static const char *json_value_end(const char *p) {
    if (*p == '"') {
        for (p++; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') {
                return p + 1;
            }
        }
        return nullptr;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        bool in_str = false;
        for (; *p; p++) {
            if (in_str) {
                if (*p == '\\' && p[1]) {
                    p++;
                } else if (*p == '"') {
                    in_str = false;
                }
            } else if (*p == '"') {
                in_str = true;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if ((*p == '}' || *p == ']') && --depth == 0) {
                return p + 1;
            }
        }
        return nullptr;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/* Start of the value stored under `"key"` (past the ':' and any blanks), or
 * NULL when the key is absent. The key must be genuinely in KEY position —
 * preceded by '{' or ',' and followed by ':' (modulo whitespace); a bare
 * strstr would also match text inside a string VALUE. */
static const char *json_find_value(const char *json, const char *key) {
    if (!json) {
        return nullptr;
    }
    char pat[CBM_SZ_64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return nullptr;
    }
    for (const char *p = json; (p = strstr(p, pat)) != nullptr; p += n) {
        const char *before = p;
        while (before > json && isspace((unsigned char)before[-1])) {
            before--;
        }
        char prev = (before > json) ? before[-1] : '\0';
        const char *after = p + n;
        while (isspace((unsigned char)*after)) {
            after++;
        }
        if ((prev == '{' || prev == ',') && *after == ':') {
            after++;
            while (isspace((unsigned char)*after)) {
                after++;
            }
            return after;
        }
    }
    return nullptr;
}

/* Parse an integer "key":N from a flat JSON object. Returns dflt if absent. */
static int json_get_int(const char *json, const char *key, int dflt) {
    const char *v = json_find_value(json, key);
    return v ? (int)strtol(v, NULL, CBM_DECIMAL_BASE) : dflt;
}

/* Parse a boolean "key":true/false from a flat JSON object. */
static bool json_get_bool(const char *json, const char *key) {
    const char *v = json_find_value(json, key);
    return v && *v == 't';
}

/* Store `value` (a ready JSON token) under `key` in the node's properties
 * object: overwrite the existing value in place, or append the key when it is
 * absent. A blob that is not a JSON object, or an existing value that cannot
 * be delimited, leaves the node untouched; so does an unchanged value. */
static void props_upsert(cbm_gbuf_node_t *node, const char *key, const char *value) {
    const char *old = node->properties_json ? node->properties_json : "{}";
    size_t olen = strlen(old);
    if (olen < 2 || old[0] != '{' || old[olen - 1] != '}') {
        return;
    }
    size_t vlen = strlen(value);

    const char *v = json_find_value(old, key);
    if (v) {
        const char *vend = json_value_end(v);
        if (!vend) {
            return;
        }
        if ((size_t)(vend - v) == vlen && memcmp(v, value, vlen) == 0) {
            return; /* already holds this value */
        }
        size_t head = (size_t)(v - old);
        size_t tail = strlen(vend);
        char *neu = (char *)malloc(head + vlen + tail + 1);
        if (!neu) {
            return;
        }
        memcpy(neu, old, head);
        memcpy(neu + head, value, vlen);
        memcpy(neu + head + vlen, vend, tail + 1);
        free(node->properties_json);
        node->properties_json = neu;
        return;
    }

    /* An object holding only whitespace ("{ }") takes no leading comma. */
    bool empty = true;
    for (size_t i = 1; i + 1 < olen; i++) {
        if (!isspace((unsigned char)old[i])) {
            empty = false;
            break;
        }
    }
    size_t keep = empty ? 1 : olen - 1; /* drop the trailing '}' (and any inner blanks) */
    size_t klen = strlen(key);
    /* keep + ',' + '"' key '"' ':' + value + '}' + NUL */
    char *neu = (char *)malloc(keep + 1 + klen + 3 + vlen + 2);
    if (!neu) {
        return;
    }
    char *w = neu;
    memcpy(w, old, keep);
    w += keep;
    if (!empty) {
        *w++ = ',';
    }
    *w++ = '"';
    memcpy(w, key, klen);
    w += klen;
    *w++ = '"';
    *w++ = ':';
    memcpy(w, value, vlen);
    w += vlen;
    *w++ = '}';
    *w = '\0';
    free(node->properties_json);
    node->properties_json = neu;
}

/* Record transitive_loop_depth + recursive in a node's properties JSON. */
static void append_complexity_props(cbm_gbuf_node_t *node, int tld, bool recursive) {
    char val[CBM_SZ_32];
    snprintf(val, sizeof(val), "%d", tld);
    props_upsert(node, "transitive_loop_depth", val);
    props_upsert(node, "recursive", recursive ? "true" : "false");
}

/* Content-only node order: qualified_name, then file path and start line.
 * Never the temp id, and never the graph buffer's label-list order: extract
 * workers draw ids from one shared counter, so both follow worker scheduling
 * and differ between a sequential and a parallel index (and run to run). The
 * memoised DFS truncates a branch at the first back edge, so which member of a
 * call cycle it ENTERS first decides the transitive_loop_depth every other
 * member reads; both the seed order and the callee order must therefore be a
 * function of the inputs alone. A dangling target (no node for the id) has no
 * edges of its own, so where it sorts cannot move a result; it keys as empty
 * strings.
 *
 * The string comparison runs once per node: every node the traversal can order
 * (the seeds and every CALLS target) is sorted into a dense integer rank —
 * equal keys share a rank — and the DFS orders callees by that integer. */
enum { TLD_CMP_LESS = -1, TLD_CMP_GREATER = 1 };

static const char *str_or_empty(const char *s) {
    return s ? s : "";
}

static int cmp_node_canonical(const cbm_gbuf_node_t *a, const cbm_gbuf_node_t *b) {
    int r = strcmp(str_or_empty(a ? a->qualified_name : NULL),
                   str_or_empty(b ? b->qualified_name : NULL));
    if (r != 0) {
        return r;
    }
    r = strcmp(str_or_empty(a ? a->file_path : NULL), str_or_empty(b ? b->file_path : NULL));
    if (r != 0) {
        return r;
    }
    int la = a ? a->start_line : 0;
    int lb = b ? b->start_line : 0;
    if (la != lb) {
        return la < lb ? TLD_CMP_LESS : TLD_CMP_GREATER;
    }
    return 0;
}

static int cmp_node_ptr_canonical(const void *pa, const void *pb) {
    return cmp_node_canonical(*(const cbm_gbuf_node_t *const *)pa,
                              *(const cbm_gbuf_node_t *const *)pb);
}

enum { TLD_UNRANKED = -1 };

/* Traversal state. `callees` is one growable stack shared by every DFS frame:
 * a frame pushes its callee ids, sorts them by rank, recurses, then pops them.
 * `path` is the DFS path stack used to attribute a back edge to every member
 * of the cycle it closes. `rank` holds the canonical position of every seed
 * and CALLS target (TLD_UNRANKED otherwise); an id with no node sorts at
 * `dangling_rank`. */
typedef struct {
    const cbm_gbuf_t *gb;
    int64_t maxid;
    int *loop_depth;
    int *tld;
    char *state;
    bool *recursive;
    int64_t *path;
    int *rank;
    int dangling_rank;
    int64_t *callees;
    size_t callee_top;
    size_t callee_cap;
} tld_ctx_t;

static int tld_rank(const tld_ctx_t *cx, int64_t id) {
    if (id < 1 || id > cx->maxid || cx->rank[id] == TLD_UNRANKED) {
        return cx->dangling_rank;
    }
    return cx->rank[id];
}

static bool tld_callees_reserve(tld_ctx_t *cx, size_t extra) {
    size_t need = cx->callee_top + extra;
    if (need <= cx->callee_cap) {
        return true;
    }
    size_t cap = cx->callee_cap ? cx->callee_cap : CBM_SZ_64;
    while (cap < need) {
        cap *= 2;
    }
    auto *grown = (int64_t *)realloc(cx->callees, cap * sizeof(int64_t));
    if (!grown) {
        return false;
    }
    cx->callees = grown;
    cx->callee_cap = cap;
    return true;
}

/* Memoized DFS: tld(id) = loop_depth(id) + max over CALLS-callees of tld(callee).
 * state: 0=unvisited, 1=in-progress (back-edge → cycle), 2=done. */
static int tld_dfs(tld_ctx_t *cx, int64_t id, int depth) {
    if (id < 1 || id > cx->maxid) {
        return 0;
    }
    if (cx->state[id] == 2) {
        return cx->tld[id];
    }
    if (cx->state[id] == 1) {
        /* Back edge: id is an ancestor on the current DFS path, so every node
         * from the current caller back up to id forms one call cycle (mutual
         * recursion A→B→A). Mark the whole cycle, not just id — otherwise the
         * caller is memoized done with recursive=false. */
        for (int k = depth - 1; k >= 0; k--) {
            cx->recursive[cx->path[k]] = true;
            if (cx->path[k] == id) {
                break;
            }
        }
        cx->recursive[id] = true; /* back edge → call-graph cycle */
        return 0;
    }
    if (depth > CBM_TLD_MAX_DEPTH) {
        return cx->loop_depth[id];
    }
    const cbm_gbuf_edge_t **edges = NULL;
    int ne = 0;
    cbm_gbuf_find_edges_by_source_type(cx->gb, id, "CALLS", &edges, &ne);
    if (!tld_callees_reserve(cx, (size_t)ne)) {
        return cx->loop_depth[id]; /* out of memory: same as the depth cap */
    }
    cx->state[id] = 1;
    cx->path[depth] = id; /* push onto the DFS path stack for cycle attribution */
    size_t base = cx->callee_top;
    for (int i = 0; i < ne; i++) {
        int64_t c = edges[i]->target_id;
        if (c == id) {
            cx->recursive[id] = true; /* direct self-recursion */
            continue;
        }
        cx->callees[cx->callee_top++] = c;
    }
    std::sort(cx->callees + base, cx->callees + cx->callee_top,
              [cx](int64_t a, int64_t b) { return tld_rank(cx, a) < tld_rank(cx, b); });
    int best = 0;
    /* Index, not pointer: a deeper frame may grow (move) the stack. */
    for (size_t i = base; i < cx->callee_top; i++) {
        int ct = tld_dfs(cx, cx->callees[i], depth + 1);
        if (ct > best) {
            best = ct;
        }
    }
    cx->callee_top = base;
    cx->tld[id] = cx->loop_depth[id] + best;
    cx->state[id] = 2;
    return cx->tld[id];
}

/* Seed each Function/Method node's loop_depth and self_recursive flag, and
 * collect the node as a traversal seed (also the write-back target). The
 * self_recursive seed (set at extraction) feeds the final recursive flag;
 * tld_dfs additionally ORs in mutual recursion discovered as a call-graph
 * cycle. A node carries one label and is listed once under it, so no node is
 * seeded twice and the seeds fit an array sized to the id ceiling. */
static void seed_loop_depths(tld_ctx_t *cx, const char *label, cbm_gbuf_node_t **seeds,
                             int *seed_count) {
    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_label(cx->gb, label, &nodes, &count) != 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *n = nodes[i];
        if (n->id >= 1 && n->id <= cx->maxid) {
            cx->loop_depth[n->id] = json_get_int(n->properties_json, "loop_depth", 0);
            cx->recursive[n->id] = json_get_bool(n->properties_json, "self_recursive");
            seeds[(*seed_count)++] = (cbm_gbuf_node_t *)n;
        }
    }
}

/* Rank the seeds and every CALLS target in canonical order. `order` is scratch
 * sized for the id ceiling plus the NULL entry that keys a dangling id. */
static void tld_build_ranks(tld_ctx_t *cx, cbm_gbuf_node_t **seeds, int seed_count,
                            const cbm_gbuf_node_t **order) {
    size_t n = 0;
    for (int i = 0; i < seed_count; i++) {
        cx->rank[seeds[i]->id] = 0; /* mark collected */
        order[n++] = seeds[i];
    }
    const cbm_gbuf_edge_t **edges = NULL;
    int ne = 0;
    if (cbm_gbuf_find_edges_by_type(cx->gb, "CALLS", &edges, &ne) == 0) {
        for (int i = 0; i < ne; i++) {
            int64_t t = edges[i]->target_id;
            if (t < 1 || t > cx->maxid || cx->rank[t] != TLD_UNRANKED) {
                continue;
            }
            const cbm_gbuf_node_t *node = cbm_gbuf_find_by_id(cx->gb, t);
            if (node) {
                cx->rank[t] = 0;
                order[n++] = node;
            }
        }
    }
    order[n++] = NULL; /* the dangling-id key */
    qsort(order, n, sizeof(*order), cmp_node_ptr_canonical);
    int r = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && cmp_node_canonical(order[i - 1], order[i]) != 0) {
            r++;
        }
        if (order[i]) {
            cx->rank[order[i]->id] = r;
        } else {
            cx->dangling_rank = r;
        }
    }
}

static void tld_ctx_free(tld_ctx_t *cx) {
    free(cx->loop_depth);
    free(cx->tld);
    free(cx->state);
    free(cx->recursive);
    free(cx->path);
    free(cx->rank);
    free(cx->callees);
}

void cbm_pipeline_pass_complexity(cbm_pipeline_ctx_t *ctx) {
    cbm_gbuf_t *gb = ctx->gbuf;
    /* Node and edge IDs are drawn from one shared counter, so node IDs are NOT
     * contiguous 1..node_count — they interleave with edge IDs. Size the lookup
     * arrays by the id ceiling (next_id) so every node id is addressable. */
    int64_t maxid = cbm_gbuf_next_id(gb) - 1;
    if (maxid < 1) {
        return;
    }
    size_t sz = (size_t)maxid + 1;
    tld_ctx_t cx = {};
    cx.gb = gb;
    cx.maxid = maxid;
    cx.loop_depth = (int *)calloc(sz, sizeof(int));
    cx.tld = (int *)calloc(sz, sizeof(int));
    cx.state = (char *)calloc(sz, sizeof(char));
    cx.recursive = (bool *)calloc(sz, sizeof(bool));
    cx.path = (int64_t *)calloc(CBM_TLD_MAX_DEPTH + 1, sizeof(int64_t));
    cx.rank = (int *)malloc(sz * sizeof(int));
    /* Seeds are distinct nodes, so the id ceiling bounds them. */
    auto **seeds = (cbm_gbuf_node_t **)calloc(sz, sizeof(cbm_gbuf_node_t *));
    auto **order = (const cbm_gbuf_node_t **)malloc((sz + 1) * sizeof(cbm_gbuf_node_t *));
    if (cx.loop_depth && cx.tld && cx.state && cx.recursive && cx.path && cx.rank && seeds &&
        order) {
        std::fill(cx.rank, cx.rank + sz, (int)TLD_UNRANKED);
        int seed_count = 0;
        seed_loop_depths(&cx, "Function", seeds, &seed_count);
        seed_loop_depths(&cx, "Method", seeds, &seed_count);
        tld_build_ranks(&cx, seeds, seed_count, order);
        std::sort(seeds, seeds + seed_count, [&cx](cbm_gbuf_node_t *a, cbm_gbuf_node_t *b) {
            return cx.rank[a->id] < cx.rank[b->id];
        });

        for (int i = 0; i < seed_count; i++) {
            cbm_gbuf_node_t *n = seeds[i];
            if (cx.state[n->id] != 2) {
                tld_dfs(&cx, n->id, 0);
            }
            append_complexity_props(n, cx.tld[n->id], cx.recursive[n->id]);
        }
        cbm_log_info("pass.complexity", "functions", itoa_cx(seed_count));
    }
    tld_ctx_free(&cx);
    free(seeds);
    free(order);
}

/* ══════════════════════════════════════════════════════════════════════
 * Importance — index-time per-symbol score (weighted degree).
 *
 * Computes an importance score for every Function/Method/Class node and stores
 * it as a numeric "importance" key inside the node's EXISTING properties_json —
 * no new column, no schema change, no CBM_INDEX_FORMAT_VERSION bump.
 *
 *   importance = sqrt(num_refs) * priv * generic * distinct * test_penalty
 *
 *     num_refs      incoming CALLS + USAGE edges. 0 -> sqrt(0) = 0.
 *     priv     0.1  name is private (leading underscore)
 *     generic  0.1  the name is defined in >= 5 DISTINCT files
 *     distinct 10   name is snake_case or camelCase AND len >= 8
 *     test     0.1  target of an incoming TESTS edge, or lives in a test file
 *                   per cbm_is_test_path() (the classifier pass_tests uses)
 *
 * Cost: the generic multiplier needs |{files a name is defined in}|. Computed
 * per NODE by walking the whole same-name group and comparing paths pairwise,
 * that is O(k^3) per group of size k (upstream measured 85 s -> 481 s on a
 * 666 MB Java corpus). Here the count is memoized once per DISTINCT name and
 * paths are deduplicated through a hash set, so the total is linear.
 *
 * Ordering is load-bearing: see cbm_pipeline_pass_importance in
 * pipeline_internal.h. (Ported from DeusData/codebase-memory-mcp@e5f3aab0.)
 * ══════════════════════════════════════════════════════════════════════ */

static std::atomic<uint64_t> g_importance_name_visits{0};

uint64_t cbm_pipeline_importance_name_visits(void) {
    return g_importance_name_visits.load(std::memory_order_relaxed);
}

enum {
    CBM_IMPORTANCE_GENERIC_MIN_FILES = 5, /* name defined in >= N files -> generic */
    CBM_IMPORTANCE_DISTINCT_MIN_LEN = 8,  /* distinctive-identifier length floor */
    CBM_IMPORTANCE_NAME_MEMO_CAP = 4096,  /* initial buckets: one entry per distinct name */
    CBM_IMPORTANCE_PATH_SET_CAP = 256,
};
static const double CBM_IMPORTANCE_PRIV_MUL = 0.1;
static const double CBM_IMPORTANCE_GENERIC_MUL = 0.1;
static const double CBM_IMPORTANCE_DISTINCT_MUL = 10.0;
static const double CBM_IMPORTANCE_TEST_MUL = 0.1;

static const char *const CBM_IMPORTANCE_LABELS[] = {"Function", "Method", "Class"};

static bool imp_name_is_private(const char *name) {
    return name != nullptr && name[0] == '_';
}

/* snake_case is an embedded '_'; camelCase is a lower->upper hump. A plain
 * long lowercase word is NOT distinctive. */
static bool imp_name_is_distinctive(const char *name) {
    if (!name) {
        return false;
    }
    size_t len = strlen(name);
    if (len < (size_t)CBM_IMPORTANCE_DISTINCT_MIN_LEN) {
        return false;
    }
    if (strchr(name, '_') != nullptr) {
        return true;
    }
    for (size_t i = 1; i < len; i++) {
        if (islower((unsigned char)name[i - 1]) && isupper((unsigned char)name[i])) {
            return true;
        }
    }
    return false;
}

/* IDEMPOTENT BY CONTRACT: the incremental path rehydrates nodes that already
 * carry "importance"; a pure append would write {"importance":1,...,
 * "importance":2}. props_upsert overwrites in place, appends only when absent,
 * and skips the rewrite when the formatted score is unchanged. A blob that is
 * not a JSON object — or a non-finite score, which "%f" renders as the invalid
 * JSON token `nan`/`inf` — leaves the node untouched: properties feed generated
 * columns, and one malformed blob gets the whole database quarantined. */
void cbm_pipeline_importance_append_prop(cbm_gbuf_node_t *node, double score) {
    if (!node || !std::isfinite(score)) {
        return;
    }
    char val[CBM_SZ_32];
    int vn = snprintf(val, sizeof(val), "%.6f", score);
    if (vn < 0 || (size_t)vn >= sizeof(val)) {
        return;
    }
    props_upsert(node, "importance", val);
}

typedef struct {
    CBMHashTable *counts; /* name -> (void *)(intptr_t)(distinct_files + 1) */
    CBMHashTable *paths;  /* scratch path set, cleared between names */
} imp_name_index_t;

/* Distinct files a name is DEFINED in — once per distinct name, then a hash
 * hit. Keys are borrowed from gbuf nodes, which outlive the pass. */
static int imp_distinct_file_count(const cbm_gbuf_t *gb, const char *name, imp_name_index_t *ix) {
    if (!name || !*name || !ix->counts) {
        return 0;
    }
    void *cached = cbm_ht_get(ix->counts, name);
    if (cached) {
        return (int)((intptr_t)cached - 1);
    }
    const cbm_gbuf_node_t **nodes = nullptr;
    int count = 0;
    int distinct = 0;
    if (cbm_gbuf_find_by_name(gb, name, &nodes, &count) == 0 && count > 0) {
        g_importance_name_visits.fetch_add((uint64_t)count, std::memory_order_relaxed);
        if (ix->paths) {
            cbm_ht_clear(ix->paths);
            for (int i = 0; i < count; i++) {
                const char *fp = nodes[i]->file_path;
                if (!fp || cbm_ht_has(ix->paths, fp)) {
                    continue;
                }
                cbm_ht_set(ix->paths, fp, (void *)(intptr_t)1);
                distinct++;
            }
        }
        /* No scratch set (allocation failure): degrade to "never generic"
         * rather than a pairwise scan — a silent quadratic is worse than a
         * missing 0.1 multiplier. */
    }
    cbm_ht_set(ix->counts, name, (void *)(intptr_t)(distinct + 1));
    return distinct;
}

static int imp_incoming_edge_count(const cbm_gbuf_t *gb, int64_t id, const char *type) {
    const cbm_gbuf_edge_t **edges = nullptr;
    int ne = 0;
    if (cbm_gbuf_find_edges_by_target_type(gb, id, type, &edges, &ne) != 0) {
        return 0;
    }
    return ne;
}

void cbm_pipeline_pass_importance(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf) {
        return;
    }
    cbm_gbuf_t *gb = ctx->gbuf;
    /* Modest reservations, not sized from node_count: the memo holds one entry
     * per DISTINCT name, and sizing for every node would cost hundreds of MB of
     * transient peak on a multi-million-node graph. */
    imp_name_index_t ix = {cbm_ht_create(CBM_IMPORTANCE_NAME_MEMO_CAP),
                           cbm_ht_create(CBM_IMPORTANCE_PATH_SET_CAP)};
    if (!ix.counts) {
        cbm_ht_free(ix.paths);
        cbm_log_error("pass.importance", "msg", "name_index_alloc_failed");
        return;
    }

    int updated = 0;
    for (const char *label : CBM_IMPORTANCE_LABELS) {
        const cbm_gbuf_node_t **nodes = nullptr;
        int count = 0;
        if (cbm_gbuf_find_by_label(gb, label, &nodes, &count) != 0) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            auto *n = (cbm_gbuf_node_t *)nodes[i];
            int num_refs = imp_incoming_edge_count(gb, n->id, "CALLS") +
                           imp_incoming_edge_count(gb, n->id, "USAGE");
            double score = std::sqrt((double)num_refs);
            if (imp_name_is_private(n->name)) {
                score *= CBM_IMPORTANCE_PRIV_MUL;
            }
            if (imp_distinct_file_count(gb, n->name, &ix) >= CBM_IMPORTANCE_GENERIC_MIN_FILES) {
                score *= CBM_IMPORTANCE_GENERIC_MUL;
            }
            if (imp_name_is_distinctive(n->name)) {
                score *= CBM_IMPORTANCE_DISTINCT_MUL;
            }
            if (cbm_is_test_path(n->file_path) || imp_incoming_edge_count(gb, n->id, "TESTS") > 0) {
                score *= CBM_IMPORTANCE_TEST_MUL;
            }
            cbm_pipeline_importance_append_prop(n, score);
            updated++;
        }
    }

    cbm_ht_free(ix.paths);
    cbm_ht_free(ix.counts);
    cbm_log_info("pass.importance", "symbols", itoa_cx(updated));
}
