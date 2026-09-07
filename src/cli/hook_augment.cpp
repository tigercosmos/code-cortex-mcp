/*
 * hook_augment.c — `code-cortex-mcp hook-augment`
 *
 * A non-blocking Claude Code hook that delivers the graph as context instead
 * of as a tool the agent has to choose to call. Reads the hook JSON from
 * stdin and dispatches on hook_event_name:
 *
 *   SessionStart          -> a 1-2 KB architecture brief for the cwd's project
 *                            (size, languages, modules, most-called functions)
 *                            or a one-line "not indexed" pointer.
 *   PreToolUse Grep/Glob, -> for an EXACT symbol in the search pattern, what
 *   Bash searches            grep cannot show: definition vs declaration,
 *                            caller/test/file counts with call-site lines,
 *                            cross-language callers, subclasses, trust notes.
 *   PreToolUse Read       -> coverage note when the file was not fully indexed.
 *   PostToolUse Edit/Write-> blast radius of the edited file: direct callers of
 *                            the symbols it defines, by file, tests separated.
 *
 * Cardinal rule: this NEVER blocks a tool call. Every error, timeout, missing
 * project, or short/odd pattern path results in `exit 0` with NO stdout
 * output (a clean pass-through). This is what makes issue #362 structurally
 * impossible to recur — the hook cannot deny a tool.
 *
 * Everything is pure SQLite through the in-process tool handlers (no shell),
 * with a per-event hard deadline so the hook stays invisible.
 */

#include "cli/cli.h"
#include "foundation/mem.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define HA_STDIN_CAP (256 * 1024) /* hook payloads are tiny; cap defensively */
#define HA_MIN_TOKEN 4            /* skip short/noisy patterns before any work */
#define HA_MAX_TOKEN 96
#define HA_MAX_WALKUP 8    /* cwd may be a subdir of the indexed root  */
/* Hard in-process budgets per hook event (see also: the settings.json
 * "timeout" backstop). A search augment must be invisible; a post-edit
 * note and the session brief may take a little longer. */
#define HA_DEADLINE_PRE_MS 300
#define HA_DEADLINE_POST_MS 1500
#define HA_DEADLINE_SESSION_MS 3000

/* ── Hard deadline ────────────────────────────────────────────────
 * A slow SQLite open or query must never stall the agent. When the timer
 * fires we _exit(0) immediately. Output is written exactly once at the very
 * end, so firing mid-work simply yields a clean no-op (no partial JSON). */
#ifndef _WIN32
static void ha_deadline_exit(int sig) {
    (void)sig;
    _exit(0);
}

static void ha_arm_deadline(int deadline_ms) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ha_deadline_exit;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = deadline_ms / 1000;
    it.it_value.tv_usec = (deadline_ms % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}
#else
static void ha_arm_deadline(int deadline_ms) {
    (void)deadline_ms; /* Windows: rely on settings.json timeout */
}
#endif

/* ── stdin ────────────────────────────────────────────────────────── */

static char *ha_read_stdin(void) {
    char *buf = (char *)malloc(HA_STDIN_CAP + 1);
    if (!buf) {
        return NULL;
    }
    size_t total = 0;
    size_t n;
    while (total < HA_STDIN_CAP && (n = fread(buf + total, 1, HA_STDIN_CAP - total, stdin)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    return buf;
}

/* ── pattern → tokens ─────────────────────────────────────────────
 * Up to `max` distinct identifier-like runs ([A-Za-z_][A-Za-z0-9_]*, at least
 * HA_MIN_TOKEN chars), longest first, so a pattern like "class Layer" tries
 * "class" and then "Layer". Pure-identifier output is safe to embed in JSON
 * and regexes unescaped. Returns the count (0 → the caller no-ops). */
#define HA_MAX_CANDIDATES 3
static int ha_extract_tokens(const char *pattern, char (*out)[HA_MAX_TOKEN + 1], int max) {
    int n = 0;
    if (!pattern) {
        return 0;
    }
    size_t i = 0;
    while (pattern[i]) {
        if (pattern[i] == '\\' && pattern[i + 1]) {
            /* A regex escape (\b, \w, \s, ...) is a separator, not part of an
             * identifier: '\bsdf_values\b' names sdf_values. */
            i += 2;
            continue;
        }
        if (!(isalpha((unsigned char)pattern[i]) || pattern[i] == '_')) {
            i++;
            continue;
        }
        size_t start = i;
        while (pattern[i] && (isalnum((unsigned char)pattern[i]) || pattern[i] == '_')) {
            i++;
        }
        size_t len = i - start;
        if (len < HA_MIN_TOKEN) {
            continue;
        }
        if (len > HA_MAX_TOKEN) {
            len = HA_MAX_TOKEN;
        }
        char cand[HA_MAX_TOKEN + 1];
        memcpy(cand, pattern + start, len);
        cand[len] = '\0';
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(out[k], cand) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        /* Insert keeping longest-first order; drop the shortest on overflow. */
        int pos = n;
        while (pos > 0 && strlen(out[pos - 1]) < len) {
            pos--;
        }
        if (pos >= max) {
            continue;
        }
        int last = n < max ? n : max - 1;
        for (int k = last; k > pos; k--) {
            memcpy(out[k], out[k - 1], sizeof(out[k]));
        }
        memcpy(out[pos], cand, sizeof(cand));
        if (n < max) {
            n++;
        }
    }
    return n;
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static const char *ha_obj_str(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

/* ── Read coverage note (#963) ────────────────────────────────────
 * For Read calls: if the file being read is listed in the project's
 * index_coverage table (parse_partial or a skip), inject a note so the agent
 * knows the knowledge graph may under-report this file. Best-effort and
 * non-blocking like everything else here — no entry, no output. */

/* Strip the last path component in place. Returns false at a filesystem or
 * drive root (nothing left to strip). */
static bool ha_strip_last_component(char *dir) {
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return false; /* POSIX root "/" */
    }
    if (slash == dir + 2 && dir[1] == ':') {
        return false; /* Windows drive root "X:/" — don't strip to "X:" */
    }
    *slash = '\0';
    return true;
}

/* Walk up from the file's parent directory to find the indexed project, then
 * check whether the file (repo-relative) is listed in its coverage report.
 * Mirrors ha_resolve_and_query: an MCP error means "not indexed here" →
 * climb; a valid project with no entry for this file → stop, no output. */
static char *ha_resolve_coverage(cbm_mcp_server_t *srv, const char *file_path) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", file_path);
    if (!ha_strip_last_component(dir)) {
        return NULL; /* file directly at a root — nothing to resolve against */
    }

    for (int level = 0; level < HA_MAX_WALKUP && cbm_hook_path_is_abs(dir); level++) {
        char *project = cbm_project_name_from_path(dir);
        if (project) {
            bool resolved = false;
            const char *rel = file_path + strlen(dir) + 1;
            char *ctx = cbm_mcp_coverage_note(srv, project, rel, &resolved);
            free(project);
            if (ctx) {
                return ctx; /* listed → note */
            }
            if (resolved) {
                return NULL; /* indexed project, file not listed → stop */
            }
        }
        if (!ha_strip_last_component(dir)) {
            break;
        }
    }
    return NULL;
}

/* Emit a hookSpecificOutput additionalContext payload to stdout (exactly once). */
static void ha_emit(const char *event, const char *text) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *hso = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, hso, "hookEventName", event);
    yyjson_mut_obj_add_str(doc, hso, "additionalContext", text);
    yyjson_mut_obj_add_val(doc, root, "hookSpecificOutput", hso);

    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        fputs(json, stdout);
        free(json);
    }
    yyjson_mut_doc_free(doc);
}

/* True for an absolute path we can walk up: POSIX "/..." or a Windows drive
 * root — "X:/..." or a bare "X:" (callers normalize '\\' to '/' first).
 * Declared in cli.h so the Windows drive-letter handling (#618) has direct
 * regression coverage. */
bool cbm_hook_path_is_abs(const char *d) {
    if (!d || !d[0]) {
        return false;
    }
    if (d[0] == '/') {
        return true;
    }
    return isalpha((unsigned char)d[0]) && d[1] == ':' && (d[2] == '/' || d[2] == '\0');
}

/* ── Bash search-command pattern extractor ────────────────────────────────
 * Tokenises and walks a Bash tool command to extract a search pattern for
 * graph augmentation.  Returns true and fills out when one clear pattern is
 * found; false on unrecognised binary, -f pattern-file, multiple -e, or any
 * other ambiguity.  Never executes or rewrites the command. */

#define HA_BASH_TOK_MAX 32
#define HA_BASH_TOK_SZ 256

/* Directory named by a leading "cd <dir>;" in the last parsed Bash search
 * command ("" when none). The augment starts its project walk-up there. */
static char g_ha_cd_dir[HA_BASH_TOK_SZ];

static int ha_tokenize(const char *cmd, char toks[][HA_BASH_TOK_SZ], int max) {
    int n = 0;
    const char *p = cmd;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        char *d = toks[n];
        int dlen = 0;
        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '\'') {
                for (p++; *p && *p != '\''; p++) {
                    if (dlen < HA_BASH_TOK_SZ - 1) {
                        d[dlen++] = *p;
                    }
                }
                if (*p == '\'') {
                    p++;
                }
            } else if (*p == '"') {
                for (p++; *p && *p != '"'; p++) {
                    if (*p == '\\' && p[1] && strchr("\\\"$`", p[1])) {
                        p++;
                    }
                    if (dlen < HA_BASH_TOK_SZ - 1) {
                        d[dlen++] = *p;
                    }
                }
                if (*p == '"') {
                    p++;
                }
            } else if (*p == '\\' && p[1]) {
                p++;
                if (dlen < HA_BASH_TOK_SZ - 1) {
                    d[dlen++] = *p++;
                } else {
                    p++;
                }
            } else {
                if (dlen < HA_BASH_TOK_SZ - 1) {
                    d[dlen++] = *p++;
                } else {
                    p++;
                }
            }
        }
        d[dlen] = '\0';
        if (dlen > 0) {
            n++;
        }
    }
    return n;
}

static bool ha_is_env_assign(const char *t) {
    if (!t || !t[0]) {
        return false;
    }
    if (!isalpha((unsigned char)t[0]) && t[0] != '_') {
        return false;
    }
    const char *p = t + 1;
    while (isalnum((unsigned char)*p) || *p == '_') {
        p++;
    }
    return *p == '=';
}

typedef enum { HA_BIN_GREP, HA_BIN_RG, HA_BIN_AG, HA_BIN_ACK, HA_BIN_UGREP } ha_bin_t;

/* Short flags that take a VALUE, so the next token is not the pattern. */
static const char *ha_search_bin_val_flags(ha_bin_t bin) {
    switch (bin) {
    case HA_BIN_RG:
        return "ABCmtTgMP";
    case HA_BIN_AG:
        return "ABCmpG";
    default:
        return "ABCmdD";
    }
}

/* Long options whose value is a SEPARATE token (`--glob '*.cpp'`). Skipping one
 * without consuming its value hands the value to the pattern slot, so
 * `rg --glob '*.cpp' Symbol .` would search the graph for "*.cpp". Anything not
 * listed here and not a known pattern option is treated as unknown and declines
 * the whole command — the extractor's contract is to fail closed rather than
 * guess which token is the pattern. */
static bool ha_long_opt_takes_value(const char *name, size_t nlen) {
    static const char *const kValued[] = {
        "glob",
        "iglob",
        "type",
        "type-not",
        "type-add",
        "type-clear",
        "max-count",
        "after-context",
        "before-context",
        "context",
        "max-depth",
        "maxdepth",
        "threads",
        "encoding",
        "engine",
        "pre",
        "sort",
        "sortr",
        "colors",
        "color",
        "replace",
        "ignore-file",
        "path-separator",
        "field-context-separator",
        "field-match-separator",
        "include",
        "exclude",
        "exclude-dir",
        "include-dir",
        "binary-files",
        "devices",
        "directories",
        "label",
        "group-separator",
        "ignore-case-fallback",
        NULL,
    };
    for (int i = 0; kValued[i]; i++) {
        if (strlen(kValued[i]) == nlen && strncmp(kValued[i], name, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* Long options that are pure booleans — safe to skip without consuming a value.
 * Everything outside both lists is unknown, and unknown declines. */
static bool ha_long_opt_is_boolean(const char *name, size_t nlen) {
    static const char *const kBoolean[] = {
        "ignore-case",
        "smart-case",
        "case-sensitive",
        "word-regexp",
        "line-regexp",
        "fixed-strings",
        "invert-match",
        "line-number",
        "no-line-number",
        "with-filename",
        "no-filename",
        "files-with-matches",
        "files-without-match",
        "count",
        "count-matches",
        "hidden",
        "no-ignore",
        "follow",
        "multiline",
        "null",
        "no-heading",
        "heading",
        "vimgrep",
        "json",
        "quiet",
        "text",
        "recursive",
        "extended-regexp",
        "basic-regexp",
        "perl-regexp",
        "only-matching",
        "no-messages",
        "binary",
        "crlf",
        "debug",
        "stats",
        "trim",
        "one-file-system",
        "no-config",
        "column",
        "byte-offset",
        "untracked",
        "cached",
        "no-index",
        NULL,
    };
    for (int i = 0; kBoolean[i]; i++) {
        if (strlen(kBoolean[i]) == nlen && strncmp(kBoolean[i], name, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static bool ha_parse_bash_search_pattern(const char *cmd, char *out, size_t out_sz) {
    if (!cmd || !out || out_sz == 0) {
        return false;
    }
    char toks[HA_BASH_TOK_MAX][HA_BASH_TOK_SZ];
    int n = ha_tokenize(cmd, toks, HA_BASH_TOK_MAX);
    if (n == 0) {
        return false;
    }

    int i = 0;
    while (i < n && ha_is_env_assign(toks[i])) {
        i++;
    }
    if (i >= n) {
        return false;
    }

    bool rtk = false;
    for (;;) {
        const char *t = toks[i];
        if (strcmp(t, "env") == 0 || strcmp(t, "nice") == 0 || strcmp(t, "time") == 0 ||
            strcmp(t, "command") == 0) {
            i++;
        } else if (strcmp(t, "rtk") == 0) {
            rtk = true;
            i++;
        } else if (strcmp(t, "tokf") == 0 && i + 1 < n && strcmp(toks[i + 1], "run") == 0) {
            i += 2;
        } else if (strcmp(t, "cd") == 0) {
            /* "cd <dir>; grep ..." / "cd <dir> && rg ...": skip to the command
             * after the separator, remembering <dir> so the augment resolves
             * the project the search actually runs in (not the payload cwd). */
            i++;
            g_ha_cd_dir[0] = '\0';
            bool sep = false;
            while (i < n && !sep) {
                size_t tl = strlen(toks[i]);
                bool bare_sep = strcmp(toks[i], ";") == 0 || strcmp(toks[i], "&&") == 0;
                bool trailing = tl > 0 && toks[i][tl - 1] == ';';
                if (!bare_sep && !g_ha_cd_dir[0]) {
                    snprintf(g_ha_cd_dir, sizeof(g_ha_cd_dir), "%.*s",
                             (int)(trailing ? tl - 1 : tl), toks[i]);
                }
                sep = bare_sep || trailing;
                i++;
            }
            if (!sep) {
                return false;
            }
        } else {
            break;
        }
        while (i < n && ha_is_env_assign(toks[i])) {
            i++;
        }
        if (i >= n) {
            return false;
        }
    }

    const char *bin_tok = toks[i++];
    ha_bin_t bin;

    if (strcmp(bin_tok, "grep") == 0 || strcmp(bin_tok, "egrep") == 0 ||
        strcmp(bin_tok, "fgrep") == 0) {
        bin = HA_BIN_GREP;
    } else if (strcmp(bin_tok, "rg") == 0) {
        bin = HA_BIN_RG;
    } else if (strcmp(bin_tok, "ag") == 0) {
        bin = HA_BIN_AG;
    } else if (strcmp(bin_tok, "ack") == 0) {
        bin = HA_BIN_ACK;
    } else if (strcmp(bin_tok, "ugrep") == 0 || strcmp(bin_tok, "ug") == 0) {
        bin = HA_BIN_UGREP;
    } else if (strcmp(bin_tok, "git") == 0) {
        if (i >= n || strcmp(toks[i], "grep") != 0) {
            return false;
        }
        i++;
        bin = HA_BIN_GREP;
    } else {
        return false;
    }

    const char *val_flags = ha_search_bin_val_flags(bin);
    const char *pattern = NULL;
    int e_count = 0;
    bool end_of_flags = false;

    for (; i < n; i++) {
        const char *t = toks[i];

        if (end_of_flags || t[0] != '-' || t[1] == '\0') {
            if (!pattern) {
                pattern = t;
            } else {
                break;
            }
            continue;
        }

        if (t[1] == '-') {
            if (t[2] == '\0') {
                end_of_flags = true;
                continue;
            }
            const char *name = t + 2;
            const char *eq = strchr(name, '=');
            size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
            if ((nlen == 6 && strncmp(name, "regexp", 6) == 0) ||
                (nlen == 7 && strncmp(name, "pattern", 7) == 0)) {
                pattern = eq ? eq + 1 : (i + 1 < n ? toks[++i] : NULL);
                e_count++;
            } else if (nlen == 4 && strncmp(name, "file", 4) == 0) {
                return false; /* pattern file: the patterns are not in the command */
            } else if (eq) {
                continue; /* --opt=value carries its own value */
            } else if (ha_long_opt_takes_value(name, nlen)) {
                if (i + 1 >= n) {
                    return false;
                }
                i++; /* the value is a separate token — never the pattern */
            } else if (!ha_long_opt_is_boolean(name, nlen)) {
                return false; /* unknown long option: cannot tell where the pattern is */
            }
            continue;
        }

        const char *f = t + 1;
        bool consumed_next = false;
        while (*f) {
            char flag = *f++;
            if (flag == 'e') {
                if (*f) {
                    pattern = f;
                    f += strlen(f);
                } else if (!consumed_next && i + 1 < n) {
                    pattern = toks[++i];
                    consumed_next = true;
                }
                e_count++;
            } else if (flag == 'f') {
                return false;
            } else if (rtk && bin == HA_BIN_GREP && flag == 'l') {
                return false;
            } else if (strchr(val_flags, flag)) {
                if (*f) {
                    f += strlen(f);
                } else if (!consumed_next && i + 1 < n) {
                    i++;
                    consumed_next = true;
                }
            }
        }
    }

    if (e_count > 1 || !pattern || !pattern[0]) {
        return false;
    }
    int w = snprintf(out, out_sz, "%s", pattern);
    return w > 0 && (size_t)w < out_sz;
}

bool cbm_hook_augment_parse_bash_pattern_for_testing(const char *cmd, char *out, size_t out_sz) {
    return ha_parse_bash_search_pattern(cmd, out, out_sz);
}

/* ── Tool-call plumbing ───────────────────────────────────────────── */

#define HA_BRIEF_CALLERS 5
#define HA_TEXT_SZ 2048

/* Run one tool in-process and return its inner JSON payload (the parsed text
 * of content[0]); *is_error mirrors the envelope's isError. Caller frees. */
static yyjson_doc *ha_call(cbm_mcp_server_t *srv, const char *tool, const char *args,
                           bool *is_error) {
    *is_error = false;
    char *env = cbm_mcp_handle_tool(srv, tool, args);
    if (!env) {
        return NULL;
    }
    yyjson_doc *edoc = yyjson_read(env, strlen(env), 0);
    free(env);
    if (!edoc) {
        return NULL;
    }
    yyjson_val *eroot = yyjson_doc_get_root(edoc);
    yyjson_val *err = yyjson_obj_get(eroot, "isError");
    if (err && yyjson_is_true(err)) {
        *is_error = true;
    }
    yyjson_val *content = yyjson_obj_get(eroot, "content");
    yyjson_val *item0 = (content && yyjson_is_arr(content)) ? yyjson_arr_get(content, 0) : NULL;
    const char *inner = ha_obj_str(item0, "text");
    yyjson_doc *idoc = inner ? yyjson_read(inner, strlen(inner), 0) : NULL;
    yyjson_doc_free(edoc);
    return idoc;
}

/* An error payload that means "this project is not indexed" (climb to the
 * parent directory), as opposed to "the project is fine, the symbol is not"
 * (stop, say nothing). */
static bool ha_error_is_project_miss(yyjson_doc *idoc) {
    if (!idoc) {
        return true;
    }
    const char *e = ha_obj_str(yyjson_doc_get_root(idoc), "error");
    if (!e) {
        return true;
    }
    return strstr(e, "project") != NULL || strstr(e, "not indexed") != NULL;
}

static int ha_obj_int(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return (v && yyjson_is_int(v)) ? yyjson_get_int(v) : 0;
}

static size_t ha_arr_size(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return (v && yyjson_is_arr(v)) ? yyjson_arr_size(v) : 0;
}

/* Append with bounds; keeps `off` valid. `cap` is the buffer capacity (the
 * buffers here are heap blocks, so sizeof would be the pointer size). */
#define HA_APPEND(buf, cap, off, ...)                                                    \
    do {                                                                                 \
        if ((off) >= 0 && (size_t)(off) < (size_t)(cap)) {                               \
            int _n = snprintf((buf) + (off), (size_t)(cap) - (size_t)(off), __VA_ARGS__); \
            (off) = _n < 0 ? (int)(cap) : (off) + _n;                                    \
        }                                                                                \
    } while (0)

/* ── PreToolUse search augment: what grep does not know ───────────────
 * The old hook listed up to five fuzzy name matches — a list the grep output
 * already shows, which measured as a null effect in 243 sessions. This one
 * fires only on an exact symbol and says what the text search cannot: where
 * the definition is versus its declaration, how many callers exist and in
 * how many files/tests, whether other languages call it, and whether the
 * graph's answer for it is trustworthy. */
static char *ha_symbol_brief(cbm_mcp_server_t *srv, const char *project, const char *token,
                             bool *resolved) {
    *resolved = false;
    /* Project names are validated to [A-Za-z0-9._-] and tokens to identifier
     * characters, so neither needs JSON escaping; the buffer must still hold
     * the longest legal pair (project up to 1 KB) or the request is cut into
     * malformed JSON and the augment silently vanishes. */
    char args[HA_MAX_TOKEN + 1024 + 128];
    int need = snprintf(args, sizeof(args),
                        "{\"project\":\"%s\",\"symbol\":\"%s\",\"source_lines\":0,"
                        "\"callers_limit\":%d,\"callees_limit\":0,\"max_bytes\":8000}",
                        project, token, HA_BRIEF_CALLERS);
    if (need < 0 || (size_t)need >= sizeof(args)) {
        return NULL;
    }
    bool is_error = false;
    yyjson_doc *d = ha_call(srv, "inspect_symbol", args, &is_error);
    if (is_error) {
        if (!ha_error_is_project_miss(d)) {
            *resolved = true;
        }
        yyjson_doc_free(d);
        return NULL;
    }
    *resolved = true;
    if (!d) {
        return NULL;
    }
    yyjson_val *r = yyjson_doc_get_root(d);
    char *text = (char *)malloc(HA_TEXT_SZ);
    if (!text) {
        yyjson_doc_free(d);
        return NULL;
    }
    int off = 0;

    const char *status = ha_obj_str(r, "status");
    if (status && strcmp(status, "ambiguous") == 0) {
        yyjson_val *sugg = yyjson_obj_get(r, "suggestions");
        size_t n = (sugg && yyjson_is_arr(sugg)) ? yyjson_arr_size(sugg) : 0;
        HA_APPEND(text, HA_TEXT_SZ, off, "[code-cortex] `%s` has %zu definitions in the graph:", token, n);
        size_t idx;
        size_t maxn;
        yyjson_val *s;
        size_t shown = 0;
        yyjson_arr_foreach(sugg, idx, maxn, s) {
            if (shown++ >= 4) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s %s (%s, %s)", shown > 1 ? "," : "",
                      ha_obj_str(s, "qualified_name") ? ha_obj_str(s, "qualified_name") : "?",
                      ha_obj_str(s, "label") ? ha_obj_str(s, "label") : "?",
                      ha_obj_str(s, "file_path") ? ha_obj_str(s, "file_path") : "?");
        }
        HA_APPEND(text, HA_TEXT_SZ, off, ". inspect_symbol(<qualified_name>) picks one.");
        yyjson_doc_free(d);
        return text;
    }

    yyjson_val *sym = yyjson_obj_get(r, "symbol");
    if (!sym) {
        free(text);
        yyjson_doc_free(d);
        return NULL;
    }
    const char *label = ha_obj_str(sym, "label");
    const char *file = ha_obj_str(sym, "file");
    HA_APPEND(text, HA_TEXT_SZ, off, "[code-cortex] `%s` is a %s defined at %s:%d-%d", token,
              label ? label : "symbol", file ? file : "?", ha_obj_int(sym, "start_line"),
              ha_obj_int(sym, "end_line"));
    yyjson_val *also = yyjson_obj_get(r, "also_defined_as");
    if (also && yyjson_is_arr(also) && yyjson_arr_size(also) > 0) {
        yyjson_val *a0 = yyjson_arr_get(also, 0);
        HA_APPEND(text, HA_TEXT_SZ, off, " (also %s at %s:%d%s)",
                  ha_obj_str(a0, "label") ? ha_obj_str(a0, "label") : "declared",
                  ha_obj_str(a0, "file") ? ha_obj_str(a0, "file") : "?", ha_obj_int(a0, "start_line"),
                  yyjson_arr_size(also) > 1 ? ", +more" : "");
    }
    yyjson_val *decl = yyjson_obj_get(r, "declared_in");
    if (decl && yyjson_is_arr(decl) && yyjson_arr_size(decl) > 0) {
        yyjson_val *d0 = yyjson_arr_get(decl, 0);
        HA_APPEND(text, HA_TEXT_SZ, off, ", declared in %s:%d",
                  ha_obj_str(d0, "file") ? ha_obj_str(d0, "file") : "?", ha_obj_int(d0, "line"));
    }
    int callers_total = ha_obj_int(r, "callers_total");
    int tests_total = ha_obj_int(r, "related_tests_total");
    size_t files = ha_arr_size(r, "caller_files");
    HA_APPEND(text, HA_TEXT_SZ, off, ". %d direct caller(s) in %zu file(s)", callers_total, files);
    if (tests_total > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, ", %d test(s)", tests_total);
    }
    yyjson_val *callers = yyjson_obj_get(r, "callers");
    if (callers && yyjson_is_arr(callers) && yyjson_arr_size(callers) > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, ": ");
        size_t idx;
        size_t maxn;
        yyjson_val *c;
        size_t shown = 0;
        yyjson_arr_foreach(callers, idx, maxn, c) {
            yyjson_val *lines = yyjson_obj_get(c, "call_lines");
            int line = (lines && yyjson_is_arr(lines) && yyjson_arr_size(lines) > 0)
                           ? (int)yyjson_get_int(yyjson_arr_get(lines, 0))
                           : ha_obj_int(c, "start_line");
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s:%d", shown ? ", " : "",
                      ha_obj_str(c, "file") ? ha_obj_str(c, "file") : "?", line);
            shown++;
        }
        if ((int)shown < callers_total) {
            HA_APPEND(text, HA_TEXT_SZ, off, ", +%d more", callers_total - (int)shown);
        }
    }
    int heuristic = ha_obj_int(r, "callers_heuristic");
    if (heuristic > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, " [%d resolved by name pattern only]", heuristic);
    }
    int subs = ha_obj_int(r, "subclasses_total");
    if (subs > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, ". %d subclass(es)", subs);
    }
    int cross = ha_obj_int(r, "cross_language_callers_total");
    if (cross > 0) {
        yyjson_val *cl = yyjson_obj_get(r, "cross_language_callers");
        yyjson_val *c0 = (cl && yyjson_is_arr(cl)) ? yyjson_arr_get(cl, 0) : NULL;
        HA_APPEND(text, HA_TEXT_SZ, off, ". %d caller(s) from %s (e.g. %s)", cross,
                  ha_obj_str(c0, "language") ? ha_obj_str(c0, "language") : "another language",
                  ha_obj_str(c0, "file") ? ha_obj_str(c0, "file") : "?");
    }
    yyjson_val *index = yyjson_obj_get(r, "index");
    yyjson_val *stale = index ? yyjson_obj_get(index, "file_modified_after_index") : NULL;
    if (stale && yyjson_is_true(stale)) {
        HA_APPEND(text, HA_TEXT_SZ, off, ". NOTE: the defining file changed after indexing");
    }
    if (ha_obj_str(r, "coverage_note")) {
        HA_APPEND(text, HA_TEXT_SZ, off, ". NOTE: the defining file was only partially parsed; treat graph "
                             "counts as lower bounds");
    }
    HA_APPEND(text, HA_TEXT_SZ, off, ". Full list with call-site lines: inspect_symbol(\"%s\").", token);
    yyjson_doc_free(d);
    return text;
}

/* ── SessionStart brief ──────────────────────────────────────────────
 * Replaces the "ALWAYS use graph tools" directive — which measurably made
 * the agent search more without using the graph — with the one thing the
 * graph had that grep could not produce: a 1-2 KB architecture brief. */
static int ha_cmp_degree_desc(const void *a, const void *b) {
    const int *x = (const int *)a;
    const int *y = (const int *)b;
    return (y[1] > x[1]) - (y[1] < x[1]);
}

static char *ha_session_brief(cbm_mcp_server_t *srv, const char *project, bool *resolved) {
    *resolved = false;
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", project);
    bool is_error = false;
    yyjson_doc *arch = ha_call(srv, "get_architecture", args, &is_error);
    if (is_error || !arch) {
        yyjson_doc_free(arch);
        return NULL;
    }
    *resolved = true;
    yyjson_val *ar = yyjson_doc_get_root(arch);
    char *text = (char *)malloc(HA_TEXT_SZ);
    if (!text) {
        yyjson_doc_free(arch);
        return NULL;
    }
    int off = 0;
    HA_APPEND(text, HA_TEXT_SZ, off, "code-cortex: this repository is indexed as \"%s\" (%d symbols, %d edges",
              project, ha_obj_int(ar, "total_nodes"), ha_obj_int(ar, "total_edges"));
    yyjson_val *langs = yyjson_obj_get(ar, "languages");
    if (langs && yyjson_is_arr(langs) && yyjson_arr_size(langs) > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, "; ");
        size_t idx;
        size_t maxn;
        yyjson_val *l;
        size_t shown = 0;
        yyjson_arr_foreach(langs, idx, maxn, l) {
            if (shown >= 4) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s %d files", shown ? ", " : "",
                      ha_obj_str(l, "language") ? ha_obj_str(l, "language") : "?",
                      ha_obj_int(l, "file_count"));
            shown++;
        }
    }
    HA_APPEND(text, HA_TEXT_SZ, off, ").");
    yyjson_val *pkgs = yyjson_obj_get(ar, "packages");
    if (pkgs && yyjson_is_arr(pkgs) && yyjson_arr_size(pkgs) > 0) {
        HA_APPEND(text, HA_TEXT_SZ, off, " Largest modules: ");
        size_t idx;
        size_t maxn;
        yyjson_val *p;
        size_t shown = 0;
        yyjson_arr_foreach(pkgs, idx, maxn, p) {
            if (shown >= 6) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s (%d)", shown ? ", " : "",
                      ha_obj_str(p, "name") ? ha_obj_str(p, "name") : "?", ha_obj_int(p, "node_count"));
            shown++;
        }
        HA_APPEND(text, HA_TEXT_SZ, off, ".");
    }
    yyjson_doc_free(arch);

    /* Most-called functions: the centrality signal nothing in a shell has. */
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"label\":\"Function\",\"min_degree\":8,\"relationship\":"
             "\"CALLS\",\"direction\":\"inbound\",\"limit\":120}",
             project);
    yyjson_doc *central = ha_call(srv, "search_graph", args, &is_error);
    if (!is_error && central) {
        yyjson_val *cr = yyjson_doc_get_root(central);
        yyjson_val *results = yyjson_obj_get(cr, "results");
        size_t n = (results && yyjson_is_arr(results)) ? yyjson_arr_size(results) : 0;
        if (n > 0) {
            enum { MAX_CENTRAL = 120 };
            int order[MAX_CENTRAL][2];
            size_t cnt = 0;
            size_t idx;
            size_t maxn;
            yyjson_val *item;
            yyjson_arr_foreach(results, idx, maxn, item) {
                if (cnt >= MAX_CENTRAL) {
                    break;
                }
                order[cnt][0] = (int)idx;
                order[cnt][1] = ha_obj_int(item, "in_degree");
                cnt++;
            }
            qsort(order, cnt, sizeof(order[0]), ha_cmp_degree_desc);
            HA_APPEND(text, HA_TEXT_SZ, off, " Most-called functions: ");
            for (size_t i = 0; i < cnt && i < 8; i++) {
                yyjson_val *it = yyjson_arr_get(results, (size_t)order[i][0]);
                HA_APPEND(text, HA_TEXT_SZ, off, "%s%s (%d callers, %s)", i ? ", " : "",
                          ha_obj_str(it, "name") ? ha_obj_str(it, "name") : "?", order[i][1],
                          ha_obj_str(it, "file_path") ? ha_obj_str(it, "file_path") : "?");
            }
            HA_APPEND(text, HA_TEXT_SZ, off, ".");
        }
    }
    yyjson_doc_free(central);

    HA_APPEND(text, HA_TEXT_SZ, off,
              "\nUse the graph for what grep cannot do: inspect_symbol(<name>) for direct "
              "callers with call-site lines, the tests that cover a symbol and callers from other "
              "languages; trace_path for multi-hop call chains; detect_changes for the blast "
              "radius of your edits. Plain grep is fine for text and for an exact identifier. The "
              "project argument is optional inside this repository. Graph answers for partially "
              "parsed files are lower bounds (results say so).");
    return text;
}

/* ── Walk-up drivers ─────────────────────────────────────────────────
 * Each derives a project name per directory level and stops at the first
 * level that resolves to an indexed project — hits or not. */

static char *ha_walk_symbol(cbm_mcp_server_t *srv, const char *start, const char *token) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", start);
    for (int level = 0; level < HA_MAX_WALKUP && cbm_hook_path_is_abs(dir); level++) {
        char *project = cbm_project_name_from_path(dir);
        if (project) {
            bool resolved = false;
            char *ctx = ha_symbol_brief(srv, project, token, &resolved);
            free(project);
            if (ctx) {
                return ctx;
            }
            if (resolved) {
                return NULL;
            }
        }
        if (!ha_strip_last_component(dir)) {
            break;
        }
    }
    return NULL;
}

static char *ha_walk_session(cbm_mcp_server_t *srv, const char *start) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", start);
    for (int level = 0; level < HA_MAX_WALKUP && cbm_hook_path_is_abs(dir); level++) {
        char *project = cbm_project_name_from_path(dir);
        if (project) {
            bool resolved = false;
            char *ctx = ha_session_brief(srv, project, &resolved);
            free(project);
            if (ctx) {
                return ctx;
            }
            if (resolved) {
                return NULL;
            }
        }
        if (!ha_strip_last_component(dir)) {
            break;
        }
    }
    return NULL;
}

static char *ha_walk_edit(cbm_mcp_server_t *srv, const char *file_path) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", file_path);
    if (!ha_strip_last_component(dir)) {
        return NULL;
    }
    for (int level = 0; level < HA_MAX_WALKUP && cbm_hook_path_is_abs(dir); level++) {
        char *project = cbm_project_name_from_path(dir);
        if (project) {
            bool resolved = false;
            const char *rel = file_path + strlen(dir) + 1;
            char *ctx = cbm_mcp_edit_impact_note(srv, project, rel, &resolved);
            free(project);
            if (ctx) {
                return ctx;
            }
            if (resolved) {
                return NULL;
            }
        }
        if (!ha_strip_last_component(dir)) {
            break;
        }
    }
    return NULL;
}

/* Normalize a hook-provided path: '\\' -> '/', and require absolute. Returns
 * false when unusable (the hook then no-ops). */
static bool ha_norm_abs(const char *in, char *out, size_t out_sz) {
    if (!in) {
        return false;
    }
    snprintf(out, out_sz, "%s", in);
    for (char *p = out; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return cbm_hook_path_is_abs(out);
}

int cbm_cmd_hook_augment(void) {
    ha_arm_deadline(HA_DEADLINE_PRE_MS);

    char *input = ha_read_stdin();
    if (!input) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    if (!doc) {
        free(input);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *event = ha_obj_str(root, "hook_event_name");
    if (!event) {
        event = "PreToolUse"; /* older payloads */
    }
    const char *tool = ha_obj_str(root, "tool_name");
    yyjson_val *tin = yyjson_obj_get(root, "tool_input");

    char cwdbuf[4096];
    const char *cwd = ha_obj_str(root, "cwd");
    if (!ha_norm_abs(cwd, cwdbuf, sizeof(cwdbuf))) {
#ifndef _WIN32
        if (!getcwd(cwdbuf, sizeof(cwdbuf))) {
            cwdbuf[0] = '\0';
        }
#else
        cwdbuf[0] = '\0';
#endif
    }
    cwd = cwdbuf[0] ? cwdbuf : NULL;

    /* SessionStart → architecture brief (plain stdout is the context). */
    if (strcmp(event, "SessionStart") == 0) {
        ha_arm_deadline(HA_DEADLINE_SESSION_MS);
        if (cwd) {
            cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
            if (srv) {
                cbm_mcp_server_set_scan_fallback(srv, false);
                char *brief = ha_walk_session(srv, cwd);
                if (brief) {
                    fputs(brief, stdout);
                    fputc('\n', stdout);
                    free(brief);
                } else {
                    fputs("code-cortex: this repository is not indexed, so callers, impact and "
                          "cross-language queries are unavailable. Index it once with the "
                          "index_repository tool (repo_path = the repository root); it takes "
                          "seconds for most repositories and stays current afterwards.\n",
                          stdout);
                }
                cbm_mcp_server_free(srv);
            }
        }
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    /* PostToolUse(Edit|Write|MultiEdit) → blast radius of the edited file. */
    if (strcmp(event, "PostToolUse") == 0) {
        if (!tool || (strcmp(tool, "Edit") != 0 && strcmp(tool, "Write") != 0 &&
                      strcmp(tool, "MultiEdit") != 0)) {
            yyjson_doc_free(doc);
            free(input);
            return 0;
        }
        ha_arm_deadline(HA_DEADLINE_POST_MS);
        char fpbuf[4096];
        if (ha_norm_abs(ha_obj_str(tin, "file_path"), fpbuf, sizeof(fpbuf))) {
            cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
            if (srv) {
                cbm_mcp_server_set_scan_fallback(srv, false);
                char *note = ha_walk_edit(srv, fpbuf);
                if (note) {
                    ha_emit("PostToolUse", note);
                    free(note);
                }
                cbm_mcp_server_free(srv);
            }
        }
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    if (strcmp(event, "PreToolUse") != 0 || !tool ||
        (strcmp(tool, "Grep") != 0 && strcmp(tool, "Glob") != 0 && strcmp(tool, "Bash") != 0 &&
         strcmp(tool, "Read") != 0)) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    /* Read → coverage note (#963): warn when the file being read is listed as
     * not fully indexed. Independent of the search augment below. */
    if (strcmp(tool, "Read") == 0) {
        char fpbuf[4096];
        if (ha_norm_abs(ha_obj_str(tin, "file_path"), fpbuf, sizeof(fpbuf))) {
            cbm_mcp_server_t *rsrv = cbm_mcp_server_new(NULL);
            if (rsrv) {
                cbm_mcp_server_set_scan_fallback(rsrv, false);
                char *note = ha_resolve_coverage(rsrv, fpbuf);
                if (note) {
                    ha_emit("PreToolUse", note);
                    free(note);
                }
                cbm_mcp_server_free(rsrv);
            }
        }
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    /* Bash searches carry the pattern inside the command line, not a "pattern"
     * field; anything the extractor cannot read unambiguously is not a search
     * and gets the ordinary silent pass-through. */
    char bash_pattern[HA_BASH_TOK_SZ];
    const char *pattern;
    char startbuf[4096];
    if (strcmp(tool, "Bash") == 0) {
        const char *cmd = ha_obj_str(tin, "command");
        g_ha_cd_dir[0] = '\0';
        if (!ha_parse_bash_search_pattern(cmd, bash_pattern, sizeof(bash_pattern))) {
            yyjson_doc_free(doc);
            free(input);
            return 0;
        }
        pattern = bash_pattern;
        /* "cd <dir>; grep ..." searches <dir>, so resolve the project from
         * there; a relative <dir> is joined onto the payload cwd. */
        if (g_ha_cd_dir[0] && cwd) {
            if (cbm_hook_path_is_abs(g_ha_cd_dir)) {
                snprintf(startbuf, sizeof(startbuf), "%s", g_ha_cd_dir);
            } else {
                snprintf(startbuf, sizeof(startbuf), "%s/%s", cwd, g_ha_cd_dir);
            }
            for (char *p = startbuf; *p; p++) {
                if (*p == '\\') {
                    *p = '/';
                }
            }
            cwd = startbuf;
        }
    } else {
        pattern = ha_obj_str(tin, "pattern");
    }
    char tokens[HA_MAX_CANDIDATES][HA_MAX_TOKEN + 1];
    int ntok = ha_extract_tokens(pattern, tokens, HA_MAX_CANDIDATES);
    if (ntok == 0 || !cwd) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }
    cbm_mcp_server_set_scan_fallback(srv, false);
    for (int t = 0; t < ntok; t++) {
        char *ctx = ha_walk_symbol(srv, cwd, tokens[t]);
        if (ctx) {
            ha_emit("PreToolUse", ctx);
            free(ctx);
            break;
        }
    }
    cbm_mcp_server_free(srv);
    yyjson_doc_free(doc);
    free(input);
    return 0;
}
