/*
 * discover.c — Recursive directory walk with filtering.
 *
 * Walks a repository directory tree, applying:
 *   1. Hardcoded directory skip patterns (60+ dirs like .git, node_modules)
 *   2. Hardcoded suffix filters (.pyc, .png, .wasm, etc.)
 *   3. Fast-mode additional filters (docs, examples, lock files, etc.)
 *   4. Gitignore-style pattern matching
 *   5. Language detection for accepted files
 */
#include "discover/discover.h"
#include "cbm.h" // CBMLanguage, CBM_LANG_COUNT, CBM_LANG_JSON

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif
#include <ctype.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strdup
#include <sys/stat.h>

int cbm_gitignore_match_result(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir);

/* ── Hardcoded always-skip directories ───────────────────────────── */

static const char *ALWAYS_SKIP_DIRS[] = {
    /* VCS */
    ".git", ".hg", ".svn", ".worktrees",
    /* The indexer's own artifact (pipeline/artifact.h CBM_ARTIFACT_DIR): an
     * export rewrites it, so indexing it made every run after an export
     * re-extract and re-export it. */
    ".code-cortex",
    /* IDE */
    ".idea", ".vs", ".vscode", ".eclipse", ".claude", ".claude-worktrees", "Antigravity",
    /* Python */
    ".cache", ".eggs", ".env", ".mypy_cache", ".nox", ".pytest_cache", ".ruff_cache", ".tox",
    ".venv", "__pycache__", "env", "htmlcov", "site-packages", "venv",
    /* JS/TS */
    ".npm", ".nyc_output", ".pnpm-store", ".yarn", "bower_components", "coverage", "node_modules",
    ".next", ".nuxt", ".svelte-kit", ".angular", ".turbo", ".parcel-cache", ".docusaurus", ".expo",
    /* Build artifacts */
    "dist", "obj", "Pods", "target", "temp", "tmp", ".terraform", ".serverless", "bazel-bin",
    "bazel-out", "bazel-testlogs",
    /* Language caches */
    ".cargo", ".stack-work", ".dart_tool", "zig-cache", "zig-out", ".metals", ".bloop", ".bsp",
    ".ccls-cache", ".clangd", "elm-stuff", "_opam", ".cpcache", ".shadow-cljs",
    /* Deploy */
    ".vercel", ".netlify", "deploy", "deployed",
    /* Misc */
    ".qdrant_code_embeddings", ".tmp", "vendor", "vendored", NULL};

static const char *FAST_SKIP_DIRS[] = {
    "generated", "gen",           "auto-generated", "fixtures",     "testdata",    "test_data",
    "__tests__", "__mocks__",     "__snapshots__",  "__fixtures__", "__test__",    "docs",
    "doc",       "documentation", "examples",       "example",      "samples",     "sample",
    "assets",    "static",        "public",         "media",        "third_party", "thirdparty",
    "3rdparty",  "external",      "migrations",     "seeds",        "e2e",         "integration",
    "locale",    "locales",       "i18n",           "l10n",         "scripts",     "tools",
    "hack",      "bin",           "build",          "out",          NULL};

/* ── Ignored suffixes ────────────────────────────────────────────── */

static const char *ALWAYS_IGNORED_SUFFIXES[] = {
    ".tmp",    "~",        ".pyc",  ".pyo",   ".o",   ".a",   ".so",  ".dll",
    ".class",  ".png",     ".jpg",  ".jpeg",  ".gif", ".ico", ".bmp", ".tiff",
    ".webp",   ".svg",     ".wasm", ".node",  ".exe", ".bin", ".dat", ".db",
    ".sqlite", ".sqlite3", ".woff", ".woff2", ".ttf", ".eot", ".otf", NULL};

static const char *FAST_IGNORED_SUFFIXES[] = {
    ".zip", ".tar",  ".gz",       ".bz2",  ".xz",  ".rar",    ".7z",      ".jar",
    ".war", ".ear",  ".mp3",      ".mp4",  ".avi", ".mov",    ".wav",     ".flac",
    ".ogg", ".mkv",  ".webm",     ".pdf",  ".doc", ".docx",   ".xls",     ".xlsx",
    ".ppt", ".pptx", ".odt",      ".ods",  ".map", ".min.js", ".min.css", ".pem",
    ".crt", ".key",  ".cer",      ".p12",  ".pb",  ".avro",   ".parquet", ".beam",
    ".elc", ".rlib", ".coverage", ".prof", ".out", ".patch",  ".diff",    NULL};

/* ── Fast-mode skip filenames ────────────────────────────────────── */

static const char *FAST_SKIP_FILENAMES[] = {
    "LICENSE",        "LICENSE.txt",     "LICENSE.md",   "LICENSE-MIT",   "LICENSE-APACHE",
    "LICENCE",        "LICENCE.txt",     "LICENCE.md",   "CHANGELOG",     "CHANGELOG.md",
    "CHANGES.md",     "HISTORY",         "HISTORY.md",   "AUTHORS",       "AUTHORS.md",
    "CONTRIBUTORS",   "CONTRIBUTORS.md", "CODEOWNERS",   "go.sum",        "yarn.lock",
    "pnpm-lock.yaml", "Pipfile.lock",    "poetry.lock",  "Gemfile.lock",  "Cargo.lock",
    "mix.lock",       "flake.lock",      "pubspec.lock", "composer.lock", "package-lock.json",
    "configure",      "Makefile.in",     "config.guess", "config.sub",    NULL};

/* ── Fast-mode substring patterns ────────────────────────────────── */

static const char *FAST_PATTERNS[] = {".d.ts",      ".bundle.", ".chunk.", ".generated.",
                                      ".pb.go",     "_pb2.py",  ".pb2.py", "_grpc.pb.go",
                                      "_string.go", "mock_",    "_mock.",  "_test_helpers.",
                                      ".stories.",  ".spec.",   ".test.",  NULL};

/* ── Ignored JSON filenames ──────────────────────────────────────── */

static const char *IGNORED_JSON_FILES[] = {
    "package.json",       "package-lock.json", "tsconfig.json",
    "jsconfig.json",      "composer.json",     "composer.lock",
    "yarn.lock",          "openapi.json",      "swagger.json",
    "jest.config.json",   ".eslintrc.json",    ".prettierrc.json",
    ".babelrc.json",      "tslint.json",       "angular.json",
    "firebase.json",      "renovate.json",     "lerna.json",
    "turbo.json",         ".stylelintrc.json", "pnpm-lock.json",
    "deno.json",          "biome.json",        "devcontainer.json",
    ".devcontainer.json", "launch.json",       "settings.json",
    "extensions.json",    "tasks.json",        NULL};

/* ── Helper: check if string is in NULL-terminated array ─────────── */

static bool str_in_list(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Helper: check if string ends with suffix ────────────────────── */

static bool ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) {
        return false;
    }
    return strcmp(s + slen - sufflen, suffix) == 0;
}

/* ── Helper: check if string contains substring ──────────────────── */

static bool str_contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

/* ── Git global excludes resolution ───────────────────────────── */

enum { GIT_TILDE_PREFIX_LEN = 2 }; /* "~/". */

static bool ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void strip_inline_comment(char *s) {
    bool in_quote = false;
    char quote = '\0';
    for (char *p = s; *p; p++) {
        if ((*p == '"' || *p == '\'') && (p == s || p[-1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (*p == quote) {
                in_quote = false;
            }
            continue;
        }
        if (!in_quote && (*p == '#' || *p == ';') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static char *strip_matching_quotes(char *s) {
    size_t len = strlen(s);
    if (len >= CBM_QUOTE_PAIR && ((s[0] == '"' && s[len - SKIP_ONE] == '"') ||
                                  (s[0] == '\'' && s[len - SKIP_ONE] == '\''))) {
        s[len - SKIP_ONE] = '\0';
        return s + SKIP_ONE;
    }
    return s;
}

static bool has_trailing_sep(const char *path) {
    size_t len = strlen(path);
    return len > 0 && (path[len - SKIP_ONE] == '/' || path[len - SKIP_ONE] == '\\');
}

static void path_join(char *out, size_t out_sz, const char *base, const char *rel) {
    if (!out || out_sz == 0) {
        return;
    }
    if (!base || base[0] == '\0') {
        snprintf(out, out_sz, "%s", rel ? rel : "");
    } else if (!rel || rel[0] == '\0') {
        snprintf(out, out_sz, "%s", base);
    } else if (has_trailing_sep(base)) {
        snprintf(out, out_sz, "%s%s", base, rel);
    } else {
        snprintf(out, out_sz, "%s/%s", base, rel);
    }
    cbm_normalize_path_sep(out);
}

static bool expand_git_path(const char *path, char *out, size_t out_sz) {
    if (!path || !path[0] || !out || out_sz == 0) {
        return false;
    }
    char normalized[CBM_SZ_4K];
    snprintf(normalized, sizeof(normalized), "%s", path);
    cbm_normalize_path_sep(normalized);

    if (normalized[0] != '~') {
        snprintf(out, out_sz, "%s", normalized);
        cbm_normalize_path_sep(out);
        return out[0] != '\0';
    }

    if (normalized[1] != '\0' && normalized[1] != '/') {
        return false; /* ~user expansion is intentionally not supported. */
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    if (normalized[1] == '\0') {
        snprintf(out, out_sz, "%s", home);
        cbm_normalize_path_sep(out);
    } else {
        path_join(out, out_sz, home, normalized + GIT_TILDE_PREFIX_LEN);
    }
    return out[0] != '\0';
}

static bool read_core_excludes_file(const char *config_path, char *out, size_t out_sz) {
    FILE *f = cbm_fopen(config_path, "r");
    if (!f) {
        return false;
    }

    bool in_core = false;
    bool found = false;
    char line[CBM_SZ_4K];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim_ws(line);
        if (s[0] == '\0' || s[0] == '#' || s[0] == ';') {
            continue;
        }

        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                in_core = false;
                continue;
            }
            *end = '\0';
            in_core = ascii_ieq(trim_ws(s + SKIP_ONE), "core");
            continue;
        }

        if (!in_core) {
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_ws(s);
        char *value = trim_ws(eq + SKIP_ONE);
        strip_inline_comment(value);
        value = strip_matching_quotes(trim_ws(value));

        if (ascii_ieq(key, "excludesfile") && value[0] != '\0' &&
            expand_git_path(value, out, out_sz)) {
            found = true;
        }
    }

    fclose(f);
    return found;
}

static bool resolve_xdg_git_config_dir(char *out, size_t out_sz) {
    char env[CBM_SZ_4K];
    if (cbm_safe_getenv("XDG_CONFIG_HOME", env, sizeof(env), NULL) && env[0] != '\0') {
        snprintf(out, out_sz, "%s", env);
        cbm_normalize_path_sep(out);
        return true;
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    path_join(out, out_sz, home, ".config");
    return out[0] != '\0';
}

static bool resolve_global_excludes_path(char *out, size_t out_sz) {
    char config_path[CBM_SZ_4K];

    const char *home = cbm_get_home_dir();
    if (home && home[0] != '\0') {
        path_join(config_path, sizeof(config_path), home, ".gitconfig");
        if (read_core_excludes_file(config_path, out, out_sz)) {
            return true;
        }
    }

    char xdg_config[CBM_SZ_4K];
    if (resolve_xdg_git_config_dir(xdg_config, sizeof(xdg_config))) {
        path_join(config_path, sizeof(config_path), xdg_config, "git/config");
        if (read_core_excludes_file(config_path, out, out_sz)) {
            return true;
        }
        path_join(out, out_sz, xdg_config, "git/ignore");
        return out[0] != '\0';
    }

    return false;
}

/* ── Public filter functions ─────────────────────────────────────── */

bool cbm_should_skip_dir(const char *dirname, cbm_index_mode_t mode) {
    if (!dirname) {
        return false;
    }

    if (str_in_list(dirname, ALWAYS_SKIP_DIRS)) {
        return true;
    }

    /* Fast discovery applies to both MODERATE and FAST — only FULL keeps everything. */
    if (mode != CBM_MODE_FULL) {
        if (str_in_list(dirname, FAST_SKIP_DIRS)) {
            return true;
        }
    }

    return false;
}

bool cbm_has_ignored_suffix(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    for (int i = 0; ALWAYS_IGNORED_SUFFIXES[i]; i++) {
        if (ends_with(filename, ALWAYS_IGNORED_SUFFIXES[i])) {
            return true;
        }
    }

    if (mode != CBM_MODE_FULL) {
        for (int i = 0; FAST_IGNORED_SUFFIXES[i]; i++) {
            if (ends_with(filename, FAST_IGNORED_SUFFIXES[i])) {
                return true;
            }
        }
    }

    return false;
}

bool cbm_should_skip_filename(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    if (mode != CBM_MODE_FULL) {
        if (str_in_list(filename, FAST_SKIP_FILENAMES)) {
            return true;
        }
    }

    return false;
}

bool cbm_matches_fast_pattern(const char *filename, cbm_index_mode_t mode) {
    if (!filename || mode == CBM_MODE_FULL) {
        return false;
    }

    for (int i = 0; FAST_PATTERNS[i]; i++) {
        if (str_contains(filename, FAST_PATTERNS[i])) {
            return true;
        }
    }

    return false;
}

/* ── Dynamic file list ───────────────────────────────────────────── */

/* ── Tracked files ─────────────────────────────────────────────────── */

/* Paths git tracks under the discovery root, and every directory above one.
 * git applies ignore rules to untracked paths only, so .gitignore,
 * info/exclude and the global excludes file never hide a tracked path.
 *
 * Loaded LAZILY, on the first path a git ignore rule matches: a repository
 * whose rules never fire during the walk never spawns `git ls-files`, so a
 * no-op or incremental reindex of such a repository pays nothing. Both tables
 * stay NULL when the root is not in a git repository or git cannot run, and
 * the ignore rules then apply exactly as before. */
struct discover_tracked {
    const char *repo_path; /* borrowed; NULL => not in a git repository */
    bool attempted;        /* `git ls-files` already ran (or was refused) */
    char *paths;           /* `git ls-files -z` output; holds the keys of `files` */
    CBMHashTable *files;   /* tracked path */
    CBMHashTable *dirs;    /* directory above a tracked path (owned key) */
};

static void tracked_free_key(const char *key, void *value, void *userdata) {
    (void)value;
    (void)userdata;
    free((void *)key);
}

static void tracked_free(struct discover_tracked *tracked) {
    if (tracked->dirs) {
        cbm_ht_foreach(tracked->dirs, tracked_free_key, nullptr);
    }
    cbm_ht_free(tracked->dirs);
    cbm_ht_free(tracked->files);
    free(tracked->paths);
    tracked->paths = nullptr;
    tracked->files = nullptr;
    tracked->dirs = nullptr;
}

/* The whole `git ls-files -z` output for repo_path (paths relative to it),
 * NUL-terminated; NULL when git cannot run or fails. Git runs without a
 * shell: the repository path is one argv entry, the git binary comes from an
 * absolute PATH entry, and the child environment drops repository-local
 * variables so an inherited GIT_DIR cannot redirect -C (#2003). */
static char *tracked_read(const char *repo_path, size_t *length) {
    *length = 0;
    const char *args[] = {"-C", repo_path, "-c", "core.fsmonitor=false", "ls-files", "-z", nullptr};
    FILE *fp = cbm_popen_git_argv(args);
    if (!fp) {
        return nullptr;
    }
    size_t capacity = CBM_SZ_64K;
    size_t used = 0;
    char *buffer = (char *)malloc(capacity);
    while (buffer) {
        if (capacity - used < CBM_SZ_4K) {
            char *grown = (char *)realloc(buffer, capacity * PAIR_LEN);
            if (!grown) {
                free(buffer);
                buffer = nullptr;
                break;
            }
            buffer = grown;
            capacity *= PAIR_LEN;
        }
        size_t got = fread(buffer + used, 1, capacity - used - SKIP_ONE, fp);
        if (got == 0) {
            break;
        }
        used += got;
    }
    if (cbm_pclose(fp) != 0 || !buffer) {
        free(buffer);
        return nullptr;
    }
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

/* Record every directory above `path` (its '/'-separated prefixes). */
static bool tracked_add_dirs(CBMHashTable *dirs, const char *path) {
    char prefix[CBM_SZ_4K];
    for (const char *slash = strchr(path, '/'); slash; slash = strchr(slash + SKIP_ONE, '/')) {
        size_t n = (size_t)(slash - path);
        if (n == 0 || n >= sizeof(prefix)) {
            continue;
        }
        memcpy(prefix, path, n);
        prefix[n] = '\0';
        if (cbm_ht_has(dirs, prefix)) {
            continue;
        }
        char *key = strdup(prefix);
        if (!key) {
            return false;
        }
        cbm_ht_set(dirs, key, key);
    }
    return true;
}

/* Load what git tracks under the root, once; leaves the tables empty on any
 * failure, so the ignore rules then apply exactly as they did without it. */
static void tracked_ensure(struct discover_tracked *tracked, const char *trigger) {
    if (tracked->attempted) {
        return;
    }
    tracked->attempted = true;
    uint64_t started = cbm_now_ms();
    size_t length = 0;
    tracked->paths = tracked_read(tracked->repo_path, &length);
    if (!tracked->paths || length == 0) {
        tracked_free(tracked);
        return;
    }
    tracked->files = cbm_ht_create(CBM_SZ_1K);
    tracked->dirs = cbm_ht_create(CBM_SZ_256);
    if (!tracked->files || !tracked->dirs) {
        tracked_free(tracked);
        return;
    }
    for (size_t at = 0; at < length;) {
        const char *path = tracked->paths + at;
        size_t n = strlen(path);
        if (n > 0) {
            cbm_ht_set(tracked->files, path, (void *)path);
            if (!tracked_add_dirs(tracked->dirs, path)) {
                tracked_free(tracked);
                return;
            }
        }
        at += n + SKIP_ONE;
    }
    char nbuf[CBM_SZ_32];
    char msbuf[CBM_SZ_32];
    snprintf(nbuf, sizeof(nbuf), "%u", cbm_ht_count(tracked->files));
    snprintf(msbuf, sizeof(msbuf), "%llu", (unsigned long long)(cbm_now_ms() - started));
    cbm_log_debug("discover.tracked_loaded", "files", nbuf, "elapsed_ms", msbuf, "trigger",
                  trigger);
}

static bool tracked_has_file(struct discover_tracked *tracked, const char *rel_path) {
    if (!tracked || !tracked->repo_path) {
        return false;
    }
    tracked_ensure(tracked, rel_path);
    return tracked->files && cbm_ht_has(tracked->files, rel_path);
}

static bool tracked_has_dir(struct discover_tracked *tracked, const char *rel_path) {
    if (!tracked || !tracked->repo_path) {
        return false;
    }
    tracked_ensure(tracked, rel_path);
    return tracked->dirs && cbm_ht_has(tracked->dirs, rel_path);
}

typedef struct {
    cbm_file_info_t *files;
    int count;
    int capacity;
    /* Bounded count mode (#713): count indexable files without retaining
     * them, stop as soon as max_files is passed or deadline_ms (absolute
     * cbm_now_ms, 0 = none) expires. max_files < 0 means unbounded. */
    int max_files;
    uint64_t deadline_ms;
    bool count_only;
    bool limit_exceeded;
    bool failed;
    /* Directories skipped during the walk (rel paths), so callers can surface
     * which subtrees were dropped (#411). strdup'd; freed by the caller via
     * cbm_discover_free_excluded or internally when not requested. */
    char **excluded;
    int excluded_count;
    int excluded_cap;
    /* Individual files dropped by ignore rules (#963 "purposely not
     * indexed"). Collected only when requested (collect_ignored); stored
     * entries are capped at CBM_DISCOVER_IGNORED_CAP while ignored_total
     * keeps counting so truncation is explicit. */
    bool collect_ignored;
    cbm_ignored_file_t *ignored;
    int ignored_count;
    int ignored_cap;
    int ignored_total;
    /* What git tracks under the root (#tracked); NULL outside a repository. */
    struct discover_tracked *tracked;
} file_list_t;

/* True once the walk must stop: the bounded count passed its limit, the
 * deadline expired (latched as a failure), or an earlier step failed. */
static bool file_list_should_stop(file_list_t *fl) {
    if (!fl->failed && fl->deadline_ms != 0 && cbm_now_ms() >= fl->deadline_ms) {
        fl->failed = true;
    }
    return fl->failed || fl->limit_exceeded;
}

static void file_list_add_excluded(file_list_t *fl, const char *rel_path) {
    if (fl->count_only || !rel_path || rel_path[0] == '\0') {
        return;
    }
    if (fl->excluded_count >= fl->excluded_cap) {
        int new_cap = fl->excluded_cap ? fl->excluded_cap * PAIR_LEN : CBM_SZ_64;
        char **grown = (char **)realloc(fl->excluded, new_cap * sizeof(char *));
        if (!grown) {
            return;
        }
        fl->excluded = grown;
        fl->excluded_cap = new_cap;
    }
    char *copy = strdup(rel_path);
    if (!copy) {
        return;
    }
    fl->excluded[fl->excluded_count++] = copy;
}

static void file_list_add_ignored(file_list_t *fl, const char *rel_path, const char *reason) {
    if (!fl->collect_ignored || !rel_path || rel_path[0] == '\0') {
        return;
    }
    fl->ignored_total++;
    if (fl->ignored_count >= CBM_DISCOVER_IGNORED_CAP) {
        return; /* counted above — truncation stays visible via the total */
    }
    if (fl->ignored_count >= fl->ignored_cap) {
        int new_cap = fl->ignored_cap ? fl->ignored_cap * PAIR_LEN : CBM_SZ_64;
        cbm_ignored_file_t *grown =
            (cbm_ignored_file_t *)realloc(fl->ignored, new_cap * sizeof(cbm_ignored_file_t));
        if (!grown) {
            return;
        }
        fl->ignored = grown;
        fl->ignored_cap = new_cap;
    }
    char *path_copy = strdup(rel_path);
    char *reason_copy = strdup(reason ? reason : "");
    if (!path_copy || !reason_copy) {
        free(path_copy);
        free(reason_copy);
        return;
    }
    fl->ignored[fl->ignored_count].rel_path = path_copy;
    fl->ignored[fl->ignored_count].reason = reason_copy;
    fl->ignored_count++;
}

static void fl_add(file_list_t *fl, const char *abs_path, const char *rel_path, CBMLanguage lang,
                   int64_t size) {
    if (fl->max_files >= 0 && fl->count >= fl->max_files) {
        fl->limit_exceeded = true;
        return;
    }
    if (fl->count_only) {
        fl->count++;
        return;
    }
    if (fl->count >= fl->capacity) {
        int new_cap = fl->capacity ? fl->capacity * PAIR_LEN : CBM_SZ_256;
        cbm_file_info_t *new_files =
            (cbm_file_info_t *)realloc(fl->files, new_cap * sizeof(cbm_file_info_t));
        if (!new_files) {
            return;
        }
        fl->files = new_files;
        fl->capacity = new_cap;
    }

    cbm_file_info_t *fi = &fl->files[fl->count++];
    fi->path = strdup(abs_path);
    fi->rel_path = strdup(rel_path);
    fi->language = lang;
    fi->size = size;
}

/* ── Recursive walk ──────────────────────────────────────────────── */

/* Compute path relative to a nested .gitignore's directory.
 * "webapp/src/foo.js" with prefix "webapp" → "src/foo.js". */
static const char *local_rel_path(const char *rel_path, const char *local_prefix) {
    if (!local_prefix || local_prefix[0] == '\0') {
        return rel_path;
    }
    size_t prefix_len = strlen(local_prefix);
    if (strncmp(rel_path, local_prefix, prefix_len) == 0 && rel_path[prefix_len] == '/') {
        return rel_path + prefix_len + SKIP_ONE;
    }
    return rel_path;
}

/* One .gitignore on the path from the repository root down to a walk frame.
 * Links form a root->leaf chain; every frame below the directory that owns a
 * matcher borrows a pointer to the deepest link governing it, so a directory
 * without a .gitignore of its own simply shares its parent's link. Links live
 * on the heap (the frame stack is realloc'd and popped) and walk_dir owns them
 * through the `owned_next` list. A nested matcher is owned by its link
 * (`owned_gi`); the root matcher belongs to cbm_discover.
 *
 * Every pattern is anchored to the directory its file came from, so each link
 * carries the offset between that directory and the walk-relative rel_path it
 * is asked about. The two offsets are mirror images and exactly one is ever
 * non-empty:
 *   `prefix` — the matcher's directory is at or BELOW the walk root
 *              ("" for the root itself): strip it from rel_path.
 *   `base`   — the matcher's directory is ABOVE the walk root (an enclosing
 *              repository's ignore files, when a git-less subfolder is
 *              indexed, #510): prepend the walk root's path relative to it. */
struct gitignore_link_t {
    const cbm_gitignore_t *gi;
    cbm_gitignore_t *owned_gi; /* == gi for nested/ancestor files, NULL for the root */
    const gitignore_link_t *parent;
    gitignore_link_t *owned_next;
    char *prefix;
    char *base;
};

static gitignore_link_t *gitignore_link_new(const cbm_gitignore_t *gi, const char *prefix,
                                            const char *base, const gitignore_link_t *parent,
                                            gitignore_link_t **owned_links) {
    auto *link = (gitignore_link_t *)calloc(1, sizeof(gitignore_link_t));
    if (!link) {
        return nullptr;
    }
    link->prefix = strdup(prefix);
    link->base = strdup(base);
    if (!link->prefix || !link->base) {
        free(link->prefix);
        free(link->base);
        free(link);
        return nullptr;
    }
    link->gi = gi;
    link->parent = parent;
    link->owned_next = *owned_links;
    *owned_links = link;
    return link;
}

static void gitignore_links_free(gitignore_link_t *link) {
    while (link) {
        gitignore_link_t *next = link->owned_next;
        cbm_gitignore_free(link->owned_gi);
        free(link->prefix);
        free(link->base);
        free(link);
        link = next;
    }
}

/* Verdict of every .gitignore between the repository root and the directory
 * being walked, with git's precedence: the deepest file that has an opinion
 * wins (a lower-level file takes precedence over every higher-level one), and
 * within one file the last matching pattern wins. Returns >0 ignored, <0
 * re-included by a negation, 0 when no file mentions the path. Cost is one
 * match per .gitignore on the path — O(depth), never a rescan (#1973). */
static int gitignore_chain_result(const gitignore_link_t *link, const char *rel_path, bool is_dir) {
    char based[CBM_SZ_4K];
    for (; link; link = link->parent) {
        const char *probe = local_rel_path(rel_path, link->prefix);
        if (link->base[0]) {
            /* Matcher from a directory above the walk root: ask it about the
             * path it would see, the one its own rooted patterns are anchored
             * against. */
            int written = snprintf(based, sizeof(based), "%s/%s", link->base, probe);
            if (written < 0 || (size_t)written >= sizeof(based)) {
                continue; /* longer than this matcher can address: no opinion */
            }
            probe = based;
        }
        int verdict = cbm_gitignore_match_result(link->gi, probe, is_dir);
        if (verdict != 0) {
            return verdict;
        }
    }
    return 0;
}

/* Non-negatable safety core: built-in skip dirs that a .cbmignore negation
 * can NEVER un-skip. A repo-committed .cbmignore must not be able to defeat
 * OOM/safety skips: .git holds VCS internals (and the info/exclude sources,
 * #489), node_modules explodes discovery, and the worktree-internal dirs
 * (.worktrees / .claude-worktrees, the worktree entries in ALWAYS_SKIP_DIRS)
 * contain parallel checkouts of the same repo whose indexing would duplicate
 * the whole codebase (#802). The indexer's own artifact directory
 * (CBM_ARTIFACT_DIR) too: indexed, every export would rewrite a source the
 * next run sees changed, and that run would export again. */
static bool is_safety_core_dir(const char *name) {
    static const char *const SAFETY_CORE_DIRS[] = {
        ".git", "node_modules", ".worktrees", ".claude-worktrees", ".code-cortex", NULL};
    return str_in_list(name, SAFETY_CORE_DIRS);
}

static bool discover_path_is_absolute(const char *path);

/* Whether dir is the root of a linked git worktree: its .git is a gitlink
 * file "gitdir: <common dir>/worktrees/<name>" and that admin directory
 * holds the "commondir" file git writes for every linked worktree. A
 * worktree added anywhere inside the checkout (not only under .worktrees) is
 * a parallel copy of the repository with an index of its own; walking into
 * it duplicated every symbol in the parent's graph. A submodule's gitlink
 * names ".../modules/<path>" and is still walked, as is a nested clone (.git
 * directory). The parent name alone is not enough: a submodule at
 * worktrees/foo has the gitdir .git/modules/worktrees/foo, which has no
 * commondir. Mirrors cbm_git_is_linked_worktree, which discover does not
 * link against. One open attempt per directory: it fails where there is no
 * .git, and reading a .git directory yields no "gitdir:" line. A gitdir path
 * too long to check (the line or the commondir path does not fit the
 * buffer) fails closed: the directory is taken for a linked worktree and
 * skipped, since walking a linked worktree indexes the repository twice. */
static bool is_linked_worktree_root(const char *dir) {
    char dot_git[CBM_SZ_4K];
    if (snprintf(dot_git, sizeof(dot_git), "%s/.git", dir) >= (int)sizeof(dot_git)) {
        return false;
    }
    FILE *f = cbm_fopen(dot_git, "r");
    if (!f) {
        return false;
    }
    char line[CBM_SZ_4K];
    bool got = fgets(line, sizeof(line), f) != NULL;
    bool cut = got && !strchr(line, '\n') && fgetc(f) != EOF;
    (void)fclose(f);
    if (!got || strncmp(line, "gitdir:", 7) != 0) {
        return false;
    }
    if (cut) {
        return true;
    }
    char *val = line + 7;
    size_t n = strlen(val);
    for (size_t i = 0; i < n; i++) {
        if (val[i] == '\\') {
            val[i] = '/';
        }
    }
    while (n > 0 &&
           (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ' || val[n - 1] == '/')) {
        val[--n] = '\0';
    }
    while (*val == ' ') {
        val++;
    }
    char commondir[CBM_SZ_4K];
    int cn = discover_path_is_absolute(val)
                 ? snprintf(commondir, sizeof(commondir), "%s/commondir", val)
                 : snprintf(commondir, sizeof(commondir), "%s/%s/commondir", dir, val);
    if (cn <= 0 || cn >= (int)sizeof(commondir)) {
        return true;
    }
    if (!cbm_file_exists(commondir)) {
        return false;
    }
    char *name = strrchr(val, '/');
    if (!name) {
        return false;
    }
    *name = '\0';
    const char *parent = strrchr(val, '/');
    /* NULL for a one-segment value ("x/y" cut to "x"). */
    // cppcheck-suppress knownConditionTrueFalse
    return parent && strcmp(parent + 1, "worktrees") == 0;
}

/* What the ignore rules say about a directory. DIR_GIT_IGNORED comes from
 * git's own rules (.gitignore files, info/exclude, the global excludes file),
 * which yield to tracked content; built-in skips and .cbmignore never do. */
typedef enum { DIR_KEEP, DIR_SKIP, DIR_GIT_IGNORED } dir_verdict_t;

/* The core.excludesFile matcher. git anchors it at the repository root, so
 * when the walk root is a git-less folder inside an enclosing repository,
 * `base` is that folder's path from the repository root (else ""), exactly
 * like an ancestor link's base: a rooted "/secret.py" must not hide
 * "pkg/secret.py" when indexing "pkg". */
typedef struct {
    const cbm_gitignore_t *gi;
    const char *base;
} global_ignore_t;

static bool global_ignore_matches(const global_ignore_t *global, const char *rel_path,
                                  bool is_dir) {
    if (!global || !global->gi) {
        return false;
    }
    if (!global->base[0]) {
        return cbm_gitignore_matches(global->gi, rel_path, is_dir);
    }
    char based[CBM_SZ_4K];
    int written = snprintf(based, sizeof(based), "%s/%s", global->base, rel_path);
    if (written < 0 || (size_t)written >= sizeof(based)) {
        return false; /* longer than the matcher can address: no opinion */
    }
    return cbm_gitignore_matches(global->gi, based, is_dir);
}

static dir_verdict_t directory_verdict(const char *entry_name, const char *rel_path,
                                       const cbm_discover_opts_t *opts,
                                       const gitignore_link_t *ignore_chain,
                                       const global_ignore_t *global_gi,
                                       const cbm_gitignore_t *cbmignore) {
    if (cbm_should_skip_dir(entry_name, opts ? opts->mode : CBM_MODE_FULL)) {
        /* #500: a .cbmignore negation (e.g. "!obj/") whose rule is the last
         * match for this dir un-skips a built-in skip-list dir — except the
         * non-negatable safety core. Fall through so .gitignore/global/local
         * rules still apply to the un-skipped dir. */
        bool unskipped = cbmignore && !is_safety_core_dir(entry_name) &&
                         cbm_gitignore_match_result(cbmignore, rel_path, true) < 0;
        if (!unskipped) {
            return DIR_SKIP;
        }
    }
    bool git_ignored = gitignore_chain_result(ignore_chain, rel_path, true) > 0;
    bool global_ignored = global_gi && global_ignore_matches(global_gi, rel_path, true);
    if (cbmignore) {
        int cbm_result = cbm_gitignore_match_result(cbmignore, rel_path, true);
        if (cbm_result > 0) {
            return DIR_SKIP;
        }
        if (cbm_result < 0) {
            global_ignored = false; /* a .cbmignore negation un-ignores a global match */
        }
    }
    return git_ignored || global_ignored ? DIR_GIT_IGNORED : DIR_KEEP;
}

/* Classify why a file is skipped. Returns a static reason string (the #963
 * "purposely not indexed" class) or NULL to keep the file. Check order and
 * semantics — including the .cbmignore negation un-ignoring a global-gitignore
 * match — are IDENTICAL to the original boolean predicate. git's own rules
 * never hide a tracked file; inside a git-ignored directory (entered only for
 * its tracked content) every untracked file is ignored, as in git. `tracked`
 * is asked only when a git rule matched an indexable file, so a repository
 * whose ignore rules never hit one never spawns `git ls-files`. */
static const char *file_skip_reason(const char *entry_name, const char *rel_path,
                                    const cbm_discover_opts_t *opts,
                                    const gitignore_link_t *ignore_chain,
                                    const global_ignore_t *global_gi,
                                    const cbm_gitignore_t *cbmignore, off_t file_size,
                                    struct discover_tracked *tracked, bool in_ignored_dir) {
    cbm_index_mode_t mode = opts ? opts->mode : CBM_MODE_FULL;
    if (cbm_has_ignored_suffix(entry_name, mode)) {
        return "ignored-suffix";
    }
    if (cbm_should_skip_filename(entry_name, mode)) {
        return "skip-list";
    }
    if (cbm_matches_fast_pattern(entry_name, mode)) {
        return "fast-pattern";
    }
    /* Only a file that could be indexed at all asks the tracked set: an
     * ignored build artifact (*.o, *.pyc, *.log) has no language, so it never
     * makes discovery spawn `git ls-files`. */
    bool indexable = cbm_language_for_filename(entry_name) != CBM_LANG_COUNT;
    if ((in_ignored_dir || gitignore_chain_result(ignore_chain, rel_path, false) > 0) &&
        !(indexable && tracked_has_file(tracked, rel_path))) {
        return "gitignore";
    }
    bool global_ignored = global_gi && global_ignore_matches(global_gi, rel_path, false) &&
                          !(indexable && tracked_has_file(tracked, rel_path));
    if (cbmignore) {
        int cbm_result = cbm_gitignore_match_result(cbmignore, rel_path, false);
        if (cbm_result > 0) {
            return "cbmignore";
        }
        if (cbm_result < 0 && global_ignored) {
            global_ignored = false;
        }
    }
    if (opts && opts->max_file_size > 0 && file_size > opts->max_file_size) {
        char szbuf[CBM_SZ_32];
        snprintf(szbuf, sizeof(szbuf), "%lld", (long long)file_size);
        cbm_log_debug("discover.skip.large", "path", rel_path, "bytes", szbuf);
        return "size-cap";
    }
    return global_ignored ? "gitignore" : NULL;
}

/* Detect language for a file, handling .m disambiguation and JSON filtering. */
static CBMLanguage detect_file_language(const char *entry_name, const char *abs_path) {
    CBMLanguage lang = cbm_language_for_filename(entry_name);
    if (lang == CBM_LANG_COUNT) {
        return CBM_LANG_COUNT;
    }
    /* Special: .m files need content-based disambiguation */
    const char *dot = strrchr(entry_name, '.');
    if (dot && strcmp(dot, ".m") == 0) {
        lang = cbm_disambiguate_m(abs_path);
    }
    /* Special: .cls is shared by Apex and VB6 class modules (#721) */
    if (dot && strcmp(dot, ".cls") == 0) {
        lang = cbm_disambiguate_cls(abs_path);
    }
    /* Special: .frm is shared by FORM and VB6 forms (#721) */
    if (dot && strcmp(dot, ".frm") == 0) {
        lang = cbm_disambiguate_frm(abs_path);
    }
    /* Special: .res is also a binary Godot / Windows resource (#2176) */
    if (dot && strcmp(dot, ".res") == 0) {
        lang = cbm_disambiguate_res(abs_path);
    }
    /* Special: .d is shared by D and make-style dep-info (cargo, gcc -MD).
     * Probe only while .d maps to D, so a user override to another language
     * wins. */
    if (dot && strcmp(dot, ".d") == 0 && lang == CBM_LANG_DLANG) {
        lang = cbm_disambiguate_d(abs_path);
    }
    /* Special: .inc is shared by C/C++ include fragments and BitBake. Only the
     * built-in default is refined, so a user extension mapping still wins. */
    if (dot && strcmp(dot, ".inc") == 0 && lang == CBM_LANG_BITBAKE) {
        lang = cbm_disambiguate_inc(abs_path);
    }
    if (lang == CBM_LANG_COUNT) {
        return CBM_LANG_COUNT;
    }
    /* Check ignored JSON files */
    if (lang == CBM_LANG_JSON && str_in_list(entry_name, IGNORED_JSON_FILES)) {
        return CBM_LANG_COUNT;
    }
    return lang;
}

/* UTF-8-safe stat: wide API on Windows, regular stat on POSIX. */
static int wide_stat(const char *path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    struct _stat64 wst;
    int ret = _wstat64(wpath, &wst);
    free(wpath);
    if (ret != 0) {
        return CBM_NOT_FOUND;
    }
    st->st_mode = wst.st_mode;
    st->st_size = wst.st_size;
    st->st_mtime = wst.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

/* Stat a path, skipping symlinks (POSIX) and junctions / reparse points
 * (Windows). Returns 0 on success, -1 to skip. Skipping reparse points keeps
 * discovery from walking through a junction that points outside the project
 * root, mirroring the POSIX S_ISLNK skip. *is_symlink reports whether the
 * skip (if any) was specifically the symlink/reparse-point check, as opposed
 * to some other stat failure (permissions, a race with a delete). */
static int safe_stat(const char *abs_path, struct stat *st, bool *is_symlink) {
    *is_symlink = false;
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(abs_path);
    if (wpath) {
        DWORD attr = GetFileAttributesW(wpath);
        free(wpath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            *is_symlink = true;
            return CBM_NOT_FOUND;
        }
    }
    return wide_stat(abs_path, st);
#else
    if (lstat(abs_path, st) != 0) {
        return CBM_NOT_FOUND;
    }
    if (S_ISLNK(st->st_mode)) {
        *is_symlink = true;
        return CBM_NOT_FOUND;
    }
    return 0;
#endif
}

/* Process a single regular file entry during directory walk. */
static void walk_dir_process_file(const char *abs_path, const char *rel_path, const char *name,
                                  const cbm_discover_opts_t *opts,
                                  const gitignore_link_t *ignore_chain,
                                  const global_ignore_t *global_gi,
                                  const cbm_gitignore_t *cbmignore, off_t size, bool in_ignored_dir,
                                  file_list_t *out) {
    const char *skip_reason = file_skip_reason(name, rel_path, opts, ignore_chain, global_gi,
                                               cbmignore, size, out->tracked, in_ignored_dir);
    if (skip_reason) {
        /* Deliberately not indexed (#963) — record so callers can surface it.
         * Unsupported-language files below are NOT recorded: "no grammar for
         * this extension" is not an ignore decision, and listing every
         * README/asset would drown the signal. */
        file_list_add_ignored(out, rel_path, skip_reason);
        return;
    }
    CBMLanguage lang = detect_file_language(name, abs_path);
    if (lang == CBM_LANG_COUNT) {
        return;
    }
    fl_add(out, abs_path, rel_path, lang, size);
}

typedef struct {
    char dir[CBM_SZ_4K];
    char prefix[CBM_SZ_4K];
    const gitignore_link_t *ignore_chain; /* deepest .gitignore governing this dir */
    bool git_ignored; /* ignored by git's rules, entered only for its tracked content */
} walk_frame_t;
/* Initial capacity only — the stack grows on demand. A single directory can
 * hold more pending sibling frames than any fixed cap (dotnet/runtime has 855
 * subdirs in one JIT regression dir), so a hard cap here means whole subtrees
 * silently vanish from the index, not a depth guard. */
#define WALK_STACK_CAP 512

typedef struct {
    walk_frame_t *frames;
    int top;
    int cap;
} walk_stack_t;
/* Build abs/rel paths and process one directory entry. */
/* Try to load a nested .gitignore from this directory. Returns owned pointer or
 * NULL. Every directory below the root is probed, whether or not an ancestor
 * already contributed a matcher: git stacks ALL .gitignore files on a path, and
 * skipping the deeper ones once a shallower one existed was the #1973 blow-up
 * (an EMPTY storage/.gitignore hid storage/dump/.gitignore's "*", so thousands
 * of git-ignored dumps were discovered and indexed until the OOM killer hit).
 * The root's own .gitignore is loaded by cbm_discover (merged with info/exclude). */
static cbm_gitignore_t *try_load_nested_gitignore(const walk_frame_t *frame) {
    if (frame->prefix[0] == '\0') {
        return NULL;
    }
    char gi_path[CBM_SZ_4K];
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", frame->dir);
    struct stat gi_st;
    if (wide_stat(gi_path, &gi_st) == 0 && S_ISREG(gi_st.st_mode)) {
        return cbm_gitignore_load(gi_path);
    }
    return NULL;
}

/* Push a subdirectory onto the walk stack, inheriting local gitignore
 * context. Grows the stack geometrically; the caller's `parent` must not
 * point into the stack array (walk_dir pops into a local copy).
 * Returns false only when growth fails, so the caller can record the
 * dropped subtree instead of losing it silently. */
static bool walk_push_subdir(walk_stack_t *ws, const char *abs_path, const char *rel_path,
                             const walk_frame_t *parent, bool git_ignored) {
    if (ws->top >= ws->cap) {
        int new_cap = ws->cap * 2;
        walk_frame_t *grown = (walk_frame_t *)realloc(ws->frames, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        ws->frames = grown;
        ws->cap = new_cap;
    }
    walk_frame_t *slot = &ws->frames[ws->top];
    snprintf(slot->dir, CBM_SZ_4K, "%s", abs_path);
    snprintf(slot->prefix, CBM_SZ_4K, "%s", rel_path);
    slot->ignore_chain = parent->ignore_chain;
    slot->git_ignored = git_ignored;
    ws->top++;
    return true;
}

static void walk_dir_process_entry(cbm_dirent_t *entry, const walk_frame_t *frame,
                                   const cbm_discover_opts_t *opts,
                                   const global_ignore_t *global_gi,
                                   const cbm_gitignore_t *cbmignore, walk_stack_t *ws,
                                   file_list_t *out) {
    char abs_path[CBM_SZ_4K];
    char rel_path[CBM_SZ_4K];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", frame->dir, entry->name);
    if (frame->prefix[0] != '\0') {
        snprintf(rel_path, sizeof(rel_path), "%s/%s", frame->prefix, entry->name);
    } else {
        snprintf(rel_path, sizeof(rel_path), "%s", entry->name);
    }

    struct stat st;
    bool is_symlink = false;
    if (safe_stat(abs_path, &st, &is_symlink) != 0) {
        if (is_symlink) {
            /* Deliberately not indexed (#963/#1815): record so callers can
             * surface it, matching the directory-exclusion and file
             * skip_reason paths below. */
            file_list_add_ignored(out, rel_path, "symlink");
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        dir_verdict_t verdict = directory_verdict(entry->name, rel_path, opts, frame->ignore_chain,
                                                  global_gi, cbmignore);
        if (verdict == DIR_KEEP && frame->git_ignored) {
            verdict = DIR_GIT_IGNORED; /* below a git-ignored directory, as in git */
        }
        if ((verdict == DIR_KEEP ||
             (verdict == DIR_GIT_IGNORED && tracked_has_dir(out->tracked, rel_path))) &&
            !is_linked_worktree_root(abs_path)) {
            if (!walk_push_subdir(ws, abs_path, rel_path, frame, verdict == DIR_GIT_IGNORED)) {
                /* Growth failed (OOM). This tree has no discover-level failure
                 * flag, so record the subtree as dropped rather than skipping
                 * it silently — truncation stays explicit (#411/#963). */
                file_list_add_excluded(out, rel_path);
            }
        } else {
            /* Record the excluded subtree root so callers can report it (#411). */
            file_list_add_excluded(out, rel_path);
        }
    } else if (S_ISREG(st.st_mode)) {
        walk_dir_process_file(abs_path, rel_path, entry->name, opts, frame->ignore_chain, global_gi,
                              cbmignore, st.st_size, frame->git_ignored, out);
    }
}

static void walk_dir(const char *dir_path, const char *rel_prefix, const cbm_discover_opts_t *opts,
                     const cbm_gitignore_t *gitignore, const gitignore_link_t *ancestors,
                     const global_ignore_t *global_gi, const cbm_gitignore_t *cbmignore,
                     file_list_t *out) {
    walk_stack_t ws = {(walk_frame_t *)calloc(WALK_STACK_CAP, sizeof(walk_frame_t)), 0,
                       WALK_STACK_CAP};
    if (!ws.frames) {
        return;
    }
    /* Every chain link (and the nested matcher it owns) is freed at the end,
     * because child frames on the stack hold borrowed pointers to them. */
    gitignore_link_t *owned_links = nullptr;

    snprintf(ws.frames[0].dir, CBM_SZ_4K, "%s", dir_path);
    snprintf(ws.frames[0].prefix, CBM_SZ_4K, "%s", rel_prefix);
    /* `ancestors` is the deepest link of an enclosing repository's chain (or
     * NULL); the walk root's own matcher sits below it so it still wins. */
    ws.frames[0].ignore_chain = ancestors;
    if (gitignore) {
        /* The repository root's .gitignore (merged with info/exclude) heads the
         * chain, so nested files take git's precedence over it too. On OOM the
         * root falls back to the ancestors only rather than silently
         * mis-ignoring. */
        const gitignore_link_t *root_link =
            gitignore_link_new(gitignore, rel_prefix, "", ancestors, &owned_links);
        if (root_link) {
            ws.frames[0].ignore_chain = root_link;
        }
    }
    ws.top++;

    while (ws.top > 0 && !file_list_should_stop(out)) {
        walk_frame_t frame = ws.frames[--ws.top];

        cbm_gitignore_t *loaded = try_load_nested_gitignore(&frame);
        if (loaded) {
            gitignore_link_t *link =
                gitignore_link_new(loaded, frame.prefix, "", frame.ignore_chain, &owned_links);
            if (link) {
                link->owned_gi = loaded;
                frame.ignore_chain = link;
            } else {
                cbm_gitignore_free(loaded);
            }
        }

        cbm_dir_t *d = cbm_opendir(frame.dir);
        if (!d) {
            continue;
        }

        cbm_dirent_t *entry;
        while (!file_list_should_stop(out) && (entry = cbm_readdir(d)) != NULL) {
            walk_dir_process_entry(entry, &frame, opts, global_gi, cbmignore, &ws, out);
        }
        cbm_closedir(d);
    }
    gitignore_links_free(owned_links);
    free(ws.frames);
}

/* ── Public API ──────────────────────────────────────────────────── */

static bool discover_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

/* Resolve the shared "common" git directory for repo_path.
 * Handles three layouts:
 *   1. <repo>/.git is a directory    - ordinary repo; common_dir == <repo>/.git
 *   2. <repo>/.git is a regular file - linked worktree gitlink "gitdir: <path>";
 *      the common dir is read from <git_dir>/commondir (git stores info/exclude +
 *      config there, shared across worktrees). Falls back to git_dir when no
 *      commondir file exists.
 *   3. neither - not a git repo.
 * Returns true when a git dir was resolved. Fixes the worktree case where
 * .git/info/exclude and core.excludesfile were silently dropped because the old
 * check required .git to be a directory (issue #489 only covered ordinary repos). */
static bool resolve_git_common_dir(const char *repo_path, char *common_dir, size_t cd_sz) {
    char dot_git[CBM_SZ_4K];
    snprintf(dot_git, sizeof(dot_git), "%s/.git", repo_path);
    struct stat st;
    if (wide_stat(dot_git, &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(common_dir, cd_sz, "%s", dot_git);
        cbm_normalize_path_sep(common_dir);
        return true;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }

    /* Linked worktree: parse "gitdir: <path>" from the gitlink file.
     * cbm_fopen (not raw fopen) so non-ASCII repo paths open on Windows. */
    FILE *f = cbm_fopen(dot_git, "r");
    if (!f) {
        return false;
    }
    char git_dir[CBM_SZ_4K];
    bool got_git_dir = false;
    char line[CBM_SZ_4K];
    while (fgets(line, sizeof(line), f)) {
        char *gs = trim_ws(line);
        if (strncmp(gs, "gitdir:", 7) == 0) {
            char *val = trim_ws(gs + 7);
            if (val[0] != '\0') {
                if (discover_path_is_absolute(val)) {
                    snprintf(git_dir, sizeof(git_dir), "%s", val);
                    cbm_normalize_path_sep(git_dir);
                } else {
                    path_join(git_dir, sizeof(git_dir), repo_path, val);
                }
                got_git_dir = true;
            }
            break;
        }
    }
    fclose(f);
    if (!got_git_dir) {
        return false;
    }

    /* The shared dir holding info/exclude + config is named in <git_dir>/commondir
     * (typically a relative path like "../.."). Absent in single-worktree gitdirs. */
    char commondir_path[CBM_SZ_4K];
    path_join(commondir_path, sizeof(commondir_path), git_dir, "commondir");
    FILE *cf = cbm_fopen(commondir_path, "r");
    if (cf) {
        char cbuf[CBM_SZ_4K];
        bool resolved = false;
        if (fgets(cbuf, sizeof(cbuf), cf)) {
            char *cs = trim_ws(cbuf);
            if (cs[0] != '\0') {
                if (discover_path_is_absolute(cs)) {
                    snprintf(common_dir, cd_sz, "%s", cs);
                    cbm_normalize_path_sep(common_dir);
                } else {
                    path_join(common_dir, cd_sz, git_dir, cs);
                }
                resolved = true;
            }
        }
        fclose(cf);
        if (resolved) {
            return true;
        }
    }

    snprintf(common_dir, cd_sz, "%s", git_dir);
    cbm_normalize_path_sep(common_dir);
    return true;
}

/* When repo_path itself carries no .git (resolve_git_common_dir already
 * returned false for it), walk upward looking for an enclosing repository,
 * exactly as `git` itself would when run from a subfolder. Bounded by the
 * filesystem/drive root: dir is truncated at each iteration. Returns true and
 * fills ancestor_root (for the enclosing repo's own .gitignore files) plus
 * common_dir (for its info/exclude + config). Upstream #510. */
static bool resolve_enclosing_git_root(const char *repo_path, char *ancestor_root, size_t ar_sz,
                                       char *common_dir, size_t cd_sz) {
    char dir[CBM_SZ_4K];
    snprintf(dir, sizeof(dir), "%s", repo_path);
    cbm_normalize_path_sep(dir);

    for (;;) {
        char *slash = strrchr(dir, '/');
        /* No separator left, or only the root separator (POSIX "/") or a
         * bare drive prefix (Windows "C:/"): nothing above this to check. */
        if (!slash || slash == dir || *(slash - 1) == ':') {
            return false;
        }
        *slash = '\0';
        if (resolve_git_common_dir(dir, common_dir, cd_sz)) {
            snprintf(ancestor_root, ar_sz, "%s", dir);
            return true;
        }
    }
}

/* The enclosing repository's ignore files, kept OUT of the indexed directory's
 * own matcher. Merging them in would re-anchor every rooted pattern onto the
 * indexed subfolder: "/secret.py" at the enclosing root would start hiding
 * <subfolder>/secret.py (silent index loss), and "pkg/scratch/" would stop
 * hiding pkg/scratch (a directory git excludes would be walked). Each matcher
 * instead keeps its own directory as its anchor, via the link's `base`. The
 * links own their matchers (`owned_gi`). */
typedef struct {
    const gitignore_link_t *chain; /* deepest ancestor link, or NULL */
    gitignore_link_t *links;       /* owner list backing `chain` */
} ancestor_ignores_t;

/* Add one ancestor directory's matcher to the chain, deepest last so that the
 * deepest file with an opinion wins (git's shallow-to-deep rule, negations
 * included). Takes ownership of `gi`. */
static bool ancestor_ignores_push(ancestor_ignores_t *anc, cbm_gitignore_t *gi, const char *base) {
    gitignore_link_t *link = gitignore_link_new(gi, "", base, anc->chain, &anc->links);
    if (!link) {
        cbm_gitignore_free(gi);
        return false;
    }
    link->owned_gi = gi;
    anc->chain = link;
    return true;
}

/* Load the .gitignore of EVERY directory from the enclosing repository root
 * (inclusive) down to the indexed directory (exclusive) — git consults all of
 * them, not just the root's. `root_extra` is that repository's
 * <common>/info/exclude, which git anchors at its root like the root
 * .gitignore; ownership is taken. `leaf` must be a canonical path under
 * `root`, both with '/' separators. Returns false only on allocation
 * failure. */
static bool ancestor_ignores_build(ancestor_ignores_t *anc, const char *root, const char *leaf,
                                   cbm_gitignore_t *root_extra) {
    size_t root_len = strlen(root);
    if (strncmp(leaf, root, root_len) != 0) {
        cbm_gitignore_free(root_extra);
        return true;
    }
    const char *base = leaf + root_len;
    while (*base == '/') {
        base++;
    }

    char dir[CBM_SZ_4K];
    int dir_len = snprintf(dir, sizeof(dir), "%s", root);
    if (dir_len < 0 || (size_t)dir_len >= sizeof(dir) || !*base) {
        cbm_gitignore_free(root_extra);
        return dir_len >= 0 && (size_t)dir_len < sizeof(dir);
    }

    for (;;) {
        char gi_path[CBM_SZ_4K];
        path_join(gi_path, sizeof(gi_path), dir, ".gitignore");
        cbm_gitignore_t *gi = cbm_gitignore_load(gi_path);
        if (root_extra) {
            /* First iteration: `dir` is the enclosing repository root, the one
             * directory info/exclude shares an anchor with. Merged after the
             * .gitignore patterns so it overrides them, the same precedence
             * the two already have for an ordinary repository root. */
            if (!gi) {
                gi = root_extra;
            } else {
                (void)cbm_gitignore_merge(gi, root_extra);
                cbm_gitignore_free(root_extra);
            }
            root_extra = nullptr;
        }
        if (gi && !ancestor_ignores_push(anc, gi, base)) {
            return false;
        }
        const char *slash = strchr(base, '/');
        if (!slash) {
            return true; /* the next directory down IS the indexed one */
        }
        int written = snprintf(dir + dir_len, sizeof(dir) - (size_t)dir_len, "/%.*s",
                               (int)(slash - base), base);
        if (written < 0 || (size_t)written >= sizeof(dir) - (size_t)dir_len) {
            return true; /* deeper directories are unaddressable: stop here */
        }
        dir_len += written;
        base = slash + SKIP_ONE;
    }
}

/* Absolute, symlink-resolved form of `path` with '/' separators, so the
 * lexical parent walk above sees the same directories whether the caller
 * passed `pkg` or `/abs/path/pkg`. Falls back to the input on failure. */
static void discover_canonical_path(const char *path, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", path);
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    wchar_t *full = wpath ? _wfullpath(nullptr, wpath, 0) : nullptr;
    char *utf8 = full ? cbm_wide_to_utf8(full) : nullptr;
    if (utf8) {
        snprintf(out, out_sz, "%s", utf8);
    }
    free(utf8);
    free(full);
    free(wpath);
#else
    char *real = realpath(path, nullptr);
    if (real) {
        snprintf(out, out_sz, "%s", real);
        free(real);
    }
#endif
    cbm_normalize_path_sep(out);
}

int cbm_discover(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                 int *count) {
    return cbm_discover_ex(repo_path, opts, out, count, NULL, NULL);
}

int cbm_discover_ex(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out) {
    return cbm_discover_ex2(repo_path, opts, out, count, excluded_out, excluded_count_out, NULL,
                            NULL, NULL);
}

/* Shared walk for full discovery and the bounded count (#713), so both apply
 * the identical filter policy. Returns 0 (CBM_DISCOVER_OK) on success,
 * CBM_DISCOVER_LIMIT_EXCEEDED when a bounded count passed max_files, and
 * CBM_NOT_FOUND on a bad root, an expired deadline or a failed walk. */
static int discover_impl(const char *repo_path, const cbm_discover_opts_t *opts,
                         cbm_file_info_t **out, int *count, char ***excluded_out,
                         int *excluded_count_out, cbm_ignored_file_t **ignored_out,
                         int *ignored_count_out, int *ignored_total_out, bool count_only,
                         int max_files, uint64_t deadline_ms) {
    if (excluded_out) {
        *excluded_out = NULL;
    }
    if (excluded_count_out) {
        *excluded_count_out = 0;
    }
    if (ignored_out) {
        *ignored_out = NULL;
    }
    if (ignored_count_out) {
        *ignored_count_out = 0;
    }
    if (ignored_total_out) {
        *ignored_total_out = 0;
    }
    if (!repo_path || !out || !count) {
        return CBM_NOT_FOUND;
    }

    *out = NULL;
    *count = 0;

    /* Verify directory exists */
    struct stat st;
    if (wide_stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return CBM_NOT_FOUND;
    }

    /* Load gitignore sources for ordinary repos AND linked worktrees.
     * Sources merged in order (later patterns win on conflict):
     *   1. <repo>/.gitignore     — committed exclusions
     *   2. <common>/info/exclude — per-clone exclusions, not committed
     * <common> is the git common dir, resolved via resolve_git_common_dir() so a
     * worktree (where .git is a gitlink file) reads the shared info/exclude/config
     * just like a normal checkout. Both are folded into a single matcher so all
     * downstream call paths remain unchanged. Fixes issue #489: OOM on repos whose
     * worktrees are excluded only via .git/info/exclude (e.g. Sandcastle).
     *
     * An ENCLOSING repository's ignore files (indexing a git-less subfolder,
     * upstream #510) are deliberately NOT folded in here: their patterns are
     * anchored to their own directories, so they stay separate matchers on the
     * chain with a `base` offset. See ancestor_ignores_build(). */
    cbm_gitignore_t *gitignore = NULL;
    char gi_path[CBM_SZ_4K];
    struct stat gi_stat;
    /* Resolve the git common dir, transparently following a worktree gitlink so the
     * .git/info/exclude and core.excludesfile sources are honoured inside linked
     * worktrees too (where .git is a file pointing at the shared dir, not a directory). */
    char git_common_dir[CBM_SZ_4K];
    bool is_git_repo = resolve_git_common_dir(repo_path, git_common_dir, sizeof(git_common_dir));
    bool has_git_config = false;
    /* repo_path itself is not a git repo root: walk upward for an enclosing one,
     * exactly as `git` does when run from a subfolder (upstream #510). The walk
     * is lexical, so repo_path is canonicalized first. */
    ancestor_ignores_t ancestors = {nullptr, nullptr};
    char enclosing_root[CBM_SZ_4K];
    char canonical_repo[CBM_SZ_4K];
    bool has_enclosing_repo = false;
    if (!is_git_repo) {
        discover_canonical_path(repo_path, canonical_repo, sizeof(canonical_repo));
        has_enclosing_repo =
            resolve_enclosing_git_root(canonical_repo, enclosing_root, sizeof(enclosing_root),
                                       git_common_dir, sizeof(git_common_dir));
        is_git_repo = has_enclosing_repo;
    }
    /* Always honour the .gitignore at the indexed-directory root, even when the
     * directory is not a git repo root (e.g. indexing a sub-package directly).
     * Fixes issue #510: a root .gitignore was silently ignored without .git/. */
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", repo_path);
    gitignore = cbm_gitignore_load(gi_path);
    if (is_git_repo) {
        path_join(gi_path, sizeof(gi_path), git_common_dir, "config");
        has_git_config = wide_stat(gi_path, &gi_stat) == 0 && S_ISREG(gi_stat.st_mode);

        char exc_path[CBM_SZ_4K];
        path_join(exc_path, sizeof(exc_path), git_common_dir, "info/exclude");
        cbm_gitignore_t *git_exclude = cbm_gitignore_load(exc_path);
        if (has_enclosing_repo) {
            /* git_common_dir belongs to the ENCLOSING repository, so its
             * info/exclude is anchored at that repository's root, not at the
             * indexed subfolder. Ownership passes to the ancestor chain. */
            if (!ancestor_ignores_build(&ancestors, enclosing_root, canonical_repo, git_exclude)) {
                gitignore_links_free(ancestors.links);
                cbm_gitignore_free(gitignore);
                return CBM_NOT_FOUND;
            }
        } else if (git_exclude) {
            if (!gitignore) {
                gitignore = git_exclude;
            } else {
                /* On allocation failure the merge is atomic (dst unchanged), so
                 * the .gitignore patterns still apply; the exclude patterns are
                 * simply skipped — same as if .git/info/exclude were absent. */
                (void)cbm_gitignore_merge(gitignore, git_exclude);
                cbm_gitignore_free(git_exclude);
            }
        }
    }

    cbm_gitignore_t *global_gi = NULL;
    if (has_git_config && resolve_global_excludes_path(gi_path, sizeof(gi_path))) {
        global_gi = cbm_gitignore_load(gi_path);
    }

    /* Load cbmignore if specified or exists at repo root */
    cbm_gitignore_t *cbmignore = NULL;
    if (opts && opts->ignore_file) {
        cbmignore = cbm_gitignore_load(opts->ignore_file);
    } else {
        snprintf(gi_path, sizeof(gi_path), "%s/.cbmignore", repo_path);
        cbmignore = cbm_gitignore_load(gi_path);
    }

    /* Walk */
    file_list_t fl = {};
    fl.max_files = count_only ? max_files : -1;
    fl.deadline_ms = count_only ? deadline_ms : 0;
    fl.count_only = count_only;
    fl.collect_ignored = !count_only && ignored_out != NULL;
    struct discover_tracked tracked = {};
    if (is_git_repo) {
        tracked.repo_path = repo_path;
        fl.tracked = &tracked;
    }
    /* The enclosing repository's root anchors core.excludesFile, as it does
     * the ancestor ignore files. */
    const char *global_base = "";
    if (has_enclosing_repo) {
        size_t root_len = strlen(enclosing_root);
        global_base = canonical_repo + root_len;
        while (*global_base == '/') {
            global_base++;
        }
    }
    global_ignore_t global = {global_gi, global_base};
    walk_dir(repo_path, "", opts, gitignore, ancestors.chain, global_gi ? &global : nullptr,
             cbmignore, &fl);

    /* Cleanup */
    tracked_free(&tracked);
    gitignore_links_free(ancestors.links);
    cbm_gitignore_free(gitignore);
    cbm_gitignore_free(global_gi);
    cbm_gitignore_free(cbmignore);

    if (count_only) {
        cbm_discover_free(fl.files, 0);
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
        cbm_discover_free_ignored(fl.ignored, fl.ignored_count);
        *count = fl.count;
        if (fl.failed) {
            return CBM_NOT_FOUND;
        }
        return fl.limit_exceeded ? CBM_DISCOVER_LIMIT_EXCEEDED : CBM_DISCOVER_OK;
    }

    *out = fl.files;
    *count = fl.count;

    /* Hand the excluded-dir list to the caller, or free it if not requested. */
    if (excluded_out) {
        *excluded_out = fl.excluded;
        if (excluded_count_out) {
            *excluded_count_out = fl.excluded_count;
        }
    } else {
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
    }

    /* Same handoff for the ignored-file list (#963). */
    if (ignored_out) {
        *ignored_out = fl.ignored;
        if (ignored_count_out) {
            *ignored_count_out = fl.ignored_count;
        }
        if (ignored_total_out) {
            *ignored_total_out = fl.ignored_total;
        }
    } else {
        cbm_discover_free_ignored(fl.ignored, fl.ignored_count);
    }
    return 0;
}

int cbm_discover_ex2(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     cbm_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out) {
    return discover_impl(repo_path, opts, out, count, excluded_out, excluded_count_out, ignored_out,
                         ignored_count_out, ignored_total_out, false, -1, 0);
}

cbm_discover_status_t cbm_discover_count_bounded(const char *repo_path,
                                                 const cbm_discover_opts_t *opts, int max_files,
                                                 uint64_t deadline_ms, int *count_out) {
    if (count_out) {
        *count_out = -1;
    }
    if (!repo_path || !count_out || max_files < 0) {
        return CBM_DISCOVER_ERROR;
    }
    cbm_file_info_t *files = nullptr;
    int count = 0;
    int rc = discover_impl(repo_path, opts, &files, &count, nullptr, nullptr, nullptr, nullptr,
                           nullptr, true, max_files, deadline_ms);
    if (rc == CBM_DISCOVER_OK || rc == CBM_DISCOVER_LIMIT_EXCEEDED) {
        *count_out = count;
        return (cbm_discover_status_t)rc;
    }
    return CBM_DISCOVER_ERROR;
}

void cbm_discover_free(cbm_file_info_t *files, int count) {
    if (!files) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].rel_path);
    }
    free(files);
}

void cbm_discover_free_excluded(char **excluded, int count) {
    if (!excluded) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(excluded[i]);
    }
    free(excluded);
}

void cbm_discover_free_ignored(cbm_ignored_file_t *ignored, int count) {
    if (!ignored) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(ignored[i].rel_path);
        free(ignored[i].reason);
    }
    free(ignored);
}
