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
 *   UserPromptSubmit      -> for each code-looking identifier in the prompt that
 *                            resolves in the index: kind, path:lines, defining
 *                            line, caller/callee counts and names; plus the
 *                            call chain between named functions (<= 4 KB).
 *
 * Cardinal rule: this NEVER blocks a tool call. Every error, timeout, missing
 * project, or short/odd pattern path results in `exit 0` with NO stdout
 * output (a clean pass-through). This is what makes issue #362 structurally
 * impossible to recur — the hook cannot deny a tool.
 *
 * Hook augmentation uses SQLite through in-process tool handlers (no shell),
 * with a per-event hard deadline. The separate task-context command can also
 * retrieve unindexed source with a bounded, supervised ripgrep process.
 */

#include "cli/cli.h"
#include "discover/discover.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/mem.h"
#include "foundation/subprocess.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <string>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HA_STDIN_CAP (256 * 1024) /* hook payloads are tiny; cap defensively */
#define HA_MIN_TOKEN 4            /* skip short/noisy patterns before any work */
#define HA_MAX_TOKEN 96
#define HA_MAX_WALKUP 8 /* cwd may be a subdir of the indexed root  */
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

/* Render a hookSpecificOutput additionalContext payload. Caller frees. */
static char *ha_render(const char *event, const char *text) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *hso = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, hso, "hookEventName", event);
    yyjson_mut_obj_add_str(doc, hso, "additionalContext", text);
    yyjson_mut_obj_add_val(doc, root, "hookSpecificOutput", hso);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

/* Emit a hookSpecificOutput additionalContext payload to stdout (exactly once). */
static void ha_emit(const char *event, const char *text, size_t max_bytes = 0) {
    char *json = ha_render(event, text);
    if (json) {
        if (!max_bytes || strlen(json) <= max_bytes) {
            fputs(json, stdout);
        }
        free(json);
    }
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

/* Preserve source coordinates and trust metadata, without shipping unrelated
 * relationship lists into a simple lookup's first model request. */
static char *ha_task_context(yyjson_doc *doc, size_t max_bytes) {
    if (!doc || max_bytes == 0) {
        return nullptr;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *source = yyjson_obj_get(root, "source");
    yyjson_val *index = yyjson_obj_get(root, "index");
    if (!yyjson_is_obj(yyjson_obj_get(root, "symbol")) || !yyjson_is_str(source) ||
        yyjson_get_len(source) == 0 ||
        yyjson_is_true(yyjson_obj_get(index, "file_modified_after_index")) ||
        yyjson_obj_get(root, "error")) {
        return nullptr;
    }
    yyjson_mut_doc *out = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *obj = yyjson_mut_obj(out);
    yyjson_mut_doc_set_root(out, obj);
    const char *keys[] = {"symbol",         "source", "source_end_line",
                          "source_clipped", "index",  "coverage_note"};
    for (const char *key : keys) {
        yyjson_val *value = yyjson_obj_get(root, key);
        if (value) {
            yyjson_mut_obj_add_val(out, obj, key, yyjson_val_mut_copy(out, value));
        }
    }
    size_t bytes = 0;
    char *text = yyjson_mut_write(out, 0, &bytes);
    yyjson_mut_doc_free(out);
    /* Include the final newline in the byte contract. Never clip JSON. */
    if (text && bytes >= max_bytes) {
        free(text);
        return nullptr;
    }
    return text;
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_task_context_for_testing(const char *json, size_t max_bytes) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : nullptr;
    char *text = ha_task_context(doc, max_bytes);
    yyjson_doc_free(doc);
    return text;
}
#endif

/* Unlike indexed context, these are text occurrences, not resolved symbols.
 * Read each event's own path: parallel rg output need not follow file order. */
static char *ha_source_context(const char *events, size_t max_bytes, bool complete,
                               const char *preferred_symbol = nullptr) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "backend", "source");
    yyjson_mut_obj_add_bool(doc, root, "scan_complete", complete);
    yyjson_mut_val *truncated = yyjson_mut_bool(doc, !complete);
    yyjson_mut_obj_add_val(doc, root, "truncated", truncated);
    yyjson_mut_obj_add_str(doc, root, "coverage_note",
                           "Text matches can include declarations and calls. Verify ownership "
                           "and definitions in source. Ignored files are not searched.");
    yyjson_mut_val *matches = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "source_matches", matches);
    std::vector<yyjson_mut_val *> rows;
    if (preferred_symbol) {
        yyjson_mut_obj_add_str(doc, root, "selection", "definition_candidates");
    }
    const char *cursor = events;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, '\n');
        if (!end) {
            yyjson_mut_set_bool(truncated, true);
            break;
        }
        yyjson_doc *event = yyjson_read(cursor, (size_t)(end - cursor), 0);
        cursor = end + 1;
        yyjson_val *eroot = event ? yyjson_doc_get_root(event) : nullptr;
        const char *kind = ha_obj_str(eroot, "type");
        if (kind && (strcmp(kind, "match") == 0 || strcmp(kind, "context") == 0)) {
            yyjson_val *data = yyjson_obj_get(eroot, "data");
            const char *path = ha_obj_str(yyjson_obj_get(data, "path"), "text");
            const char *source = ha_obj_str(yyjson_obj_get(data, "lines"), "text");
            yyjson_val *number = yyjson_obj_get(data, "line_number");
            if (path && source && yyjson_is_uint(number)) {
                yyjson_mut_val *row = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strcpy(doc, row, "file", path);
                yyjson_mut_obj_add_uint(doc, row, "line", yyjson_get_uint(number));
                yyjson_mut_obj_add_strcpy(doc, row, "source", source);
                yyjson_mut_obj_add_bool(doc, row, "is_match", strcmp(kind, "match") == 0);
                rows.push_back(row);
            } else {
                yyjson_mut_set_bool(truncated, true);
            }
        } else if (!event) {
            yyjson_mut_set_bool(truncated, true);
        }
        yyjson_doc_free(event);
    }
    if (preferred_symbol) {
        std::string qualified;
        for (const char *p = preferred_symbol; *p; ++p) {
            qualified += *p == '.' ? "::" : std::string(1, *p);
        }
        auto rank = [&](yyjson_mut_val *row) {
            if (!yyjson_mut_get_bool(yyjson_mut_obj_get(row, "is_match"))) {
                return 0;
            }
            const char *source = yyjson_mut_get_str(yyjson_mut_obj_get(row, "source"));
            const char *match = qualified.find("::") == std::string::npos
                                    ? nullptr
                                    : strstr(source, qualified.c_str());
            bool boundary = match && (match == source ||
                                      (!isalnum((unsigned char)match[-1]) && match[-1] != '_'));
            return boundary ? 2 : 1;
        };
        std::stable_sort(rows.begin(), rows.end(),
                         [&](auto *a, auto *b) { return rank(a) > rank(b); });
    }
    for (auto *row : rows) {
        yyjson_mut_arr_append(matches, row);
        size_t bytes = 0;
        char *candidate = yyjson_mut_write(doc, 0, &bytes);
        bool fits = candidate && bytes < max_bytes;
        free(candidate);
        if (!fits) {
            yyjson_mut_arr_remove_last(matches);
            yyjson_mut_set_bool(truncated, true);
        }
    }
    size_t bytes = 0;
    char *text = yyjson_mut_arr_size(matches) ? yyjson_mut_write(doc, 0, &bytes) : nullptr;
    yyjson_mut_doc_free(doc);
    if (text && bytes >= max_bytes) {
        free(text);
        text = nullptr;
    }
    return text;
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_source_context_for_testing(const char *events, size_t max_bytes, bool complete) {
    return ha_source_context(events, max_bytes, complete);
}
char *cbm_definition_context_for_testing(const char *events, const char *symbol, size_t max_bytes) {
    return ha_source_context(events, max_bytes, true, symbol);
}
#endif

static char *ha_unindexed_context(const char *repo, const char *symbol, size_t max_bytes,
                                  bool definitions = false) {
#ifndef _WIN32
    char resolved[4096];
    struct stat st;
    if (!realpath(repo, resolved) || stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return nullptr;
    }
    /* Accept identifiers and qualified names, never user-supplied regex syntax. */
    const char *last = symbol;
    bool start = true;
    for (const char *p = symbol; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
            (!start && c >= '0' && c <= '9')) {
            start = false;
        } else if (!start && (*p == '.' || (*p == ':' && p[1] == ':'))) {
            if (*p == ':') {
                ++p;
            }
            last = p + 1;
            start = true;
        } else {
            return nullptr;
        }
    }
    if (start) {
        return nullptr;
    }
    std::string pattern = "\\b" + std::string(last) + "\\b";
    if (definitions) {
        /* Candidate selection only: skip common call expressions before capture
         * and byte limits. Unusual or multiline signatures may be absent. */
        pattern = "^[ \t]*(?:[A-Za-z_][A-Za-z0-9_:<>,*& \t]*[ \t*&])?"
                  "(?:[A-Za-z_][A-Za-z0-9_]*::)*" +
                  std::string(last) +
                  "[ \t]*\\([^;{}]*\\)[^;{}]*(?:\\{|$)|"
                  "^[ \t]*(?:class|struct|interface|enum)[ \t]+" +
                  std::string(last) + "\\b";
    }
    const char *argv[] = {"/usr/bin/env",
                          "rg",
                          "--json",
                          "--context",
                          "2",
                          "--glob",
                          "*.{c,cc,cpp,cxx,h,hpp,hxx,py,rs,go,js,ts,tsx,java,kt,cs,swift}",
                          "--glob",
                          "!**/build/**",
                          "--glob",
                          "!**/3rdParty/**",
                          "--glob",
                          "!**/thirdparty/**",
                          "--glob",
                          "!**/third_party/**",
                          "--",
                          pattern.c_str(),
                          resolved,
                          nullptr};
    /* Stop the hook alarm before spawning: an abrupt parent exit would orphan
     * the search. The piped supervisor owns the deadline and always reaps. */
    ha_arm_deadline(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    cbm_proc_opts_t opts = {};
    opts.bin = argv[0];
    opts.argv = argv;
    opts.total_timeout_ms = 5000;
    cbm_proc_pipe_t child;
    if (cbm_subprocess_spawn_piped(&opts, &child) != 0) {
        return nullptr;
    }
    close(child.to_child);
    child.to_child = -1;
    std::string output;
    bool eof = false;
    while (output.size() < 65536) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0 || cbm_subprocess_wait_readable(&child, (int)remaining) <= 0) {
            break;
        }
        char buf[4096];
        size_t capacity = sizeof(buf);
        if (capacity > 65536 - output.size()) {
            capacity = 65536 - output.size();
        }
        ssize_t count = read(child.from_child, buf, capacity);
        if (count <= 0) {
            eof = count == 0;
            break;
        }
        output.append(buf, (size_t)count);
    }
    cbm_proc_result_t result;
    cbm_subprocess_reap(&child, !eof, 10, &result);
    bool complete = eof && result.outcome == CBM_PROC_CLEAN;
    if (eof && !complete) {
        return nullptr;
    }
    return ha_source_context(output.c_str(), max_bytes, complete, definitions ? symbol : nullptr);
#else
    (void)repo;
    (void)symbol;
    (void)max_bytes;
    (void)definitions;
    return nullptr;
#endif
}

static bool ha_request_identifier(const std::string &name) {
    bool start = true;
    if (name.empty() || name.size() > HA_MAX_TOKEN) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
            (!start && c >= '0' && c <= '9')) {
            start = false;
        } else if (!start &&
                   (c == '.' || (c == ':' && i + 1 < name.size() && name[i + 1] == ':'))) {
            i += c == ':';
            start = true;
        } else {
            return false;
        }
    }
    return !start;
}

static std::string ha_previous_word(const char *begin, const char *&end) {
    while (end > begin && isspace((unsigned char)end[-1])) {
        --end;
    }
    const char *last = end;
    while (end > begin && isalpha((unsigned char)end[-1])) {
        --end;
    }
    return std::string(end, last);
}

/* Only explicit inline identifiers are candidates. Examples, fenced code, and
 * multiple unrelated symbols must not silently select the wrong definition. */
static std::string ha_request_symbol(const char *request) {
    if (!request) {
        return {};
    }
    std::vector<std::string> names;
    std::string owner;
    for (const char *p = request; *p;) {
        if (*p != '`') {
            ++p;
            continue;
        }
        if (p[1] == '`' || (p > request && p[-1] == '\\')) {
            return {};
        }
        const char *end = strchr(p + 1, '`');
        if (!end) {
            return {};
        }
        if (end - p - 1 <= HA_MAX_TOKEN) {
            std::string name(p + 1, end);
            if (ha_request_identifier(name)) {
                bool duplicate = false;
                for (const auto &known : names) {
                    duplicate |= name == known;
                }
                if (!duplicate) {
                    names.push_back(name);
                }
                if (names.size() > 2) {
                    return {};
                }
                const char *prefix = p;
                std::string word = ha_previous_word(request, prefix);
                if (word == "template") {
                    word = ha_previous_word(request, prefix);
                }
                if (word == "class") {
                    if (!owner.empty() && owner != name) {
                        return {};
                    }
                    owner = name;
                }
            }
        }
        p = end + 1;
    }
    if (owner.empty()) {
        return names.size() == 1 ? names.front() : std::string();
    }
    if (names.size() == 1) {
        return owner;
    }
    if (names.size() != 2) {
        return {};
    }
    const std::string &member = names[0] == owner ? names[1] : names[0];
    if (member.find_first_of(".:") != std::string::npos) {
        return {};
    }
    size_t separator = owner.rfind("::");
    return owner.substr(separator == std::string::npos ? 0 : separator + 2) + "." + member;
}

/* Automatic hooks handle location questions only. Explicit task-context
 * requests do not need this intent gate. Ignore quoted names when classifying
 * prose so a function named update is not mistaken for an edit instruction. */
static bool ha_request_is_lookup(const char *request) {
    if (!request) {
        return false;
    }
    bool quoted = false, lookup = false, definition = false;
    std::string previous;
    for (const char *p = request; *p;) {
        if (*p == '`') {
            quoted = !quoted;
            ++p;
        } else if (quoted || !isalpha((unsigned char)*p)) {
            ++p;
        } else {
            std::string word;
            while (isalpha((unsigned char)*p)) {
                word += (char)tolower((unsigned char)*p++);
            }
            lookup |= word == "find" || word == "locate" || word == "where" || word == "which";
            definition |= word == "definition" || word == "defined" || word == "implementation" ||
                          word == "declaration" || word == "signature";
            const char *excluded[] = {"add",       "fix",        "edit",      "modify", "change",
                                      "update",    "rename",     "remove",    "delete", "refactor",
                                      "implement", "repair",     "callers",   "impact", "trace",
                                      "hierarchy", "subclasses", "overrides", "bug"};
            if (previous != "not" && previous != "never" &&
                std::ranges::any_of(excluded,
                                    [&word](const char *token) { return word == token; })) {
                return false;
            }
            previous = word;
        }
    }
    return lookup && definition;
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_request_symbol_for_testing(const char *request, bool automatic) {
    if (automatic && !ha_request_is_lookup(request)) {
        return nullptr;
    }
    std::string symbol = ha_request_symbol(request);
    return symbol.empty() ? nullptr : strdup(symbol.c_str());
}
#endif

int cbm_cmd_task_context(void) {
    ha_arm_deadline(2000);
    char *input = ha_read_stdin();
    yyjson_doc *doc = input ? yyjson_read(input, strlen(input), 0) : nullptr;
    free(input);
    if (!doc) {
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *project = ha_obj_str(root, "project");
    const char *repo = ha_obj_str(root, "repo_path");
    const char *symbol = ha_obj_str(root, "symbol");
    const char *request = ha_obj_str(root, "request");
    bool has_symbol = yyjson_obj_get(root, "symbol") != nullptr;
    bool has_request = yyjson_obj_get(root, "request") != nullptr;
    std::string selected;
    if (has_request && !has_symbol && request &&
        strlen(request) == yyjson_get_len(yyjson_obj_get(root, "request"))) {
        selected = ha_request_symbol(request);
        symbol = selected.c_str();
    }
    yyjson_val *budget = yyjson_obj_get(root, "max_bytes");
    bool has_project = yyjson_obj_get(root, "project") != nullptr;
    bool has_repo = yyjson_obj_get(root, "repo_path") != nullptr;
    if (has_symbol == has_request || has_project == has_repo ||
        (has_project && (!project || !*project)) || (has_repo && (!repo || !*repo)) || !symbol ||
        !*symbol ||
        (budget && (!yyjson_is_uint(budget) || yyjson_get_uint(budget) == 0 ||
                    yyjson_get_uint(budget) > 24000))) {
        yyjson_doc_free(doc);
        return 0;
    }
    size_t max_bytes = budget ? (size_t)yyjson_get_uint(budget) : 6000;
    if (repo) {
        char *text = ha_unindexed_context(repo, symbol, max_bytes);
        if (text) {
            fputs(text, stdout);
            fputc('\n', stdout);
            free(text);
        }
        yyjson_doc_free(doc);
        return 0;
    }
    yyjson_mut_doc *args_doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(args_doc);
    yyjson_mut_doc_set_root(args_doc, args);
    yyjson_mut_obj_add_str(args_doc, args, "project", project);
    yyjson_mut_obj_add_str(args_doc, args, "symbol", symbol);
    yyjson_mut_obj_add_int(args_doc, args, "source_lines", 12);
    yyjson_mut_obj_add_int(args_doc, args, "callers_limit", 0);
    yyjson_mut_obj_add_int(args_doc, args, "max_bytes", 4000);
    char *encoded = yyjson_mut_write(args_doc, 0, nullptr);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(nullptr);
    if (srv && encoded) {
        cbm_mcp_server_set_scan_fallback(srv, false);
        bool error = false;
        yyjson_doc *result = ha_call(srv, "inspect_symbol", encoded, &error);
        char *text = error ? nullptr : ha_task_context(result, max_bytes);
        if (text) {
            fputs(text, stdout);
            fputc('\n', stdout);
            free(text);
        }
        yyjson_doc_free(result);
    }
    if (srv) {
        cbm_mcp_server_free(srv);
    }
    free(encoded);
    yyjson_mut_doc_free(args_doc);
    yyjson_doc_free(doc);
    return 0;
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
#define HA_APPEND(buf, cap, off, ...)                                                     \
    do {                                                                                  \
        if ((off) >= 0 && (size_t)(off) < (size_t)(cap)) {                                \
            int _n = snprintf((buf) + (off), (size_t)(cap) - (size_t)(off), __VA_ARGS__); \
            (off) = _n < 0 ? (int)(cap) : (off) + _n;                                     \
        }                                                                                 \
    } while (0)

/* ── PreToolUse search augment: what grep does not know ───────────────
 * The old hook listed up to five fuzzy name matches — a list the grep output
 * already shows, which measured as a null effect in 243 sessions. This one
 * fires only on an exact symbol and says what the text search cannot: where
 * the definition is versus its declaration, how many callers exist and in
 * how many files/tests, whether other languages call it, and whether the
 * graph's answer for it is trustworthy. */
static char *ha_format_symbol_brief(yyjson_doc *d, const char *token) {
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
        HA_APPEND(text, HA_TEXT_SZ, off,
                  "[code-cortex] `%s` has %zu definitions in the graph:", token, n);
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
    if (yyjson_is_true(yyjson_obj_get(r, "metadata_omitted"))) {
        free(text);
        yyjson_doc_free(d);
        return nullptr;
    }
    const char *label = ha_obj_str(sym, "label");
    const char *file = ha_obj_str(sym, "file");
    HA_APPEND(text, HA_TEXT_SZ, off, "[code-cortex] `%s` is a %s %s at %s:%d-%d", token,
              label ? label : "symbol",
              label && strcmp(label, "Declaration") == 0 ? "declared" : "defined",
              file ? file : "?", ha_obj_int(sym, "start_line"), ha_obj_int(sym, "end_line"));
    yyjson_val *also = yyjson_obj_get(r, "also_defined_as");
    if (also && yyjson_is_arr(also) && yyjson_arr_size(also) > 0) {
        yyjson_val *a0 = yyjson_arr_get(also, 0);
        HA_APPEND(text, HA_TEXT_SZ, off, " (also %s at %s:%d%s)",
                  ha_obj_str(a0, "label") ? ha_obj_str(a0, "label") : "declared",
                  ha_obj_str(a0, "file") ? ha_obj_str(a0, "file") : "?",
                  ha_obj_int(a0, "start_line"), yyjson_arr_size(also) > 1 ? ", +more" : "");
    }
    yyjson_val *decl = yyjson_obj_get(r, "declared_in");
    if (decl && yyjson_is_arr(decl) && yyjson_arr_size(decl) > 0) {
        yyjson_val *d0 = yyjson_arr_get(decl, 0);
        HA_APPEND(text, HA_TEXT_SZ, off, ", declared in %s:%d",
                  ha_obj_str(d0, "file") ? ha_obj_str(d0, "file") : "?", ha_obj_int(d0, "line"));
    }
    int callers_total = ha_obj_int(r, "callers_total");
    int tests_total = ha_obj_int(r, "related_tests_total");
    yyjson_val *file_total = yyjson_obj_get(r, "caller_files_total");
    size_t files = yyjson_is_int(file_total) && yyjson_get_int(file_total) >= 0
                       ? (size_t)yyjson_get_int(file_total)
                       : ha_arr_size(r, "caller_files");
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
        HA_APPEND(text, HA_TEXT_SZ, off,
                  ". NOTE: the defining file was only partially parsed; treat graph "
                  "counts as lower bounds");
    }
    HA_APPEND(text, HA_TEXT_SZ, off, ". Call-site lines and further pages: inspect_symbol(\"%s\").",
              token);
    yyjson_doc_free(d);
    return text;
}

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
    return ha_format_symbol_brief(d, token);
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_hook_symbol_brief_for_testing(const char *json, const char *token) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : nullptr;
    if (!doc) {
        return nullptr;
    }
    return ha_format_symbol_brief(doc, token);
}
#endif

/* ── SessionStart brief ──────────────────────────────────────────────
 * Replaces the "ALWAYS use graph tools" directive — which measurably made
 * the agent search more without using the graph — with the one thing the
 * graph had that grep could not produce: a 1-2 KB architecture brief. */
/* Format the brief from its stored inputs (cbm_mcp_session_brief_json). The
 * inputs are computed at index time, so this is one row read even on a
 * multi-million-node graph; an index without a stored brief that is too
 * large to compute live yields a one-line brief with an approximate size. */
static char *ha_format_session_brief(const char *project, const char *json) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *ar = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!ar || !yyjson_is_obj(ar)) {
        yyjson_doc_free(doc);
        return NULL;
    }
    char *text = (char *)malloc(HA_TEXT_SZ);
    if (!text) {
        yyjson_doc_free(doc);
        return NULL;
    }
    int off = 0;
    if (yyjson_is_true(yyjson_obj_get(ar, "approximate"))) {
        HA_APPEND(text, HA_TEXT_SZ, off,
                  "code-cortex: this repository is indexed as \"%s\" (about %d symbols). Its "
                  "architecture brief is stored by the next index_repository run.",
                  project, ha_obj_int(ar, "nodes"));
    } else {
        HA_APPEND(text, HA_TEXT_SZ, off,
                  "code-cortex: this repository is indexed as \"%s\" (%d symbols, %d edges",
                  project, ha_obj_int(ar, "nodes"), ha_obj_int(ar, "edges"));
        size_t idx;
        size_t maxn;
        yyjson_val *it;
        size_t shown = 0;
        yyjson_val *langs = yyjson_obj_get(ar, "languages");
        yyjson_arr_foreach(langs, idx, maxn, it) {
            if (shown >= 4) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s %d files", shown ? ", " : "; ",
                      ha_obj_str(it, "language") ? ha_obj_str(it, "language") : "?",
                      ha_obj_int(it, "file_count"));
            shown++;
        }
        HA_APPEND(text, HA_TEXT_SZ, off, ").");
        shown = 0;
        yyjson_val *pkgs = yyjson_obj_get(ar, "packages");
        yyjson_arr_foreach(pkgs, idx, maxn, it) {
            if (shown >= 6) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s (%d)", shown ? ", " : " Largest modules: ",
                      ha_obj_str(it, "name") ? ha_obj_str(it, "name") : "?",
                      ha_obj_int(it, "node_count"));
            shown++;
        }
        if (shown) {
            HA_APPEND(text, HA_TEXT_SZ, off, ".");
        }
        shown = 0;
        yyjson_val *central = yyjson_obj_get(ar, "central");
        yyjson_arr_foreach(central, idx, maxn, it) {
            if (shown >= 8) {
                break;
            }
            HA_APPEND(text, HA_TEXT_SZ, off, "%s%s (%d callers, %s)",
                      shown ? ", " : " Most-called functions: ",
                      ha_obj_str(it, "name") ? ha_obj_str(it, "name") : "?",
                      ha_obj_int(it, "in_degree"),
                      ha_obj_str(it, "file_path") ? ha_obj_str(it, "file_path") : "?");
            shown++;
        }
        if (shown) {
            HA_APPEND(text, HA_TEXT_SZ, off, ".");
        }
    }
    yyjson_doc_free(doc);

    HA_APPEND(text, HA_TEXT_SZ, off,
              "\nUse the graph for what grep cannot do: inspect_symbol(<name>) for direct "
              "callers with call-site lines, the tests that cover a symbol and callers from other "
              "languages; trace_path for multi-hop call chains; detect_changes for the blast "
              "radius of your edits. Plain grep is fine for free text. A [code-cortex] block "
              "attached to your prompt already contains the whole-word matches for the symbols "
              "you named, read from disk when the prompt was submitted; searching for the same "
              "name again returns the same lines. The project argument is optional inside this "
              "repository. Graph answers for partially parsed files are lower bounds (results "
              "say so).");
    return text;
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_session_brief_format_for_testing(const char *project, const char *json) {
    return ha_format_session_brief(project, json);
}
#endif

static char *ha_session_brief(cbm_mcp_server_t *srv, const char *project, bool *resolved) {
    char *json = cbm_mcp_session_brief_json(srv, project, resolved);
    char *text = json ? ha_format_session_brief(project, json) : NULL;
    free(json);
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

/* ── UserPromptSubmit: graph facts for the symbols a prompt names ─────
 * Before the model's first turn, resolve every code-looking identifier the
 * user wrote against the cwd's index and hand the model what it would
 * otherwise grep for: where each symbol is defined, its first source line,
 * its direct neighbours, and — when two or more are functions — the call
 * chain between them. Prose words never reach the database: a prompt with no
 * code-looking token exits before the server is even created. */
#define HA_PROMPT_MAX_CANDIDATES 6
#define HA_PROMPT_MAX_BYTES 9000 /* Claude Code caps hook context at 10 000 chars */
#define HA_PROMPT_SRC_MAX 120    /* source line as printed */
#define HA_PROMPT_SRC_SHORT 60   /* ... when the full listing does not fit */
#define HA_PROMPT_SRC_TINY 44
#define HA_PROMPT_COMMON_NODES 20 /* a name this common loses to a rarer target */
#define HA_NAME_WINDOW 12         /* lines below a node start where its name may sit */
#define HA_NAME_WINDOW_MAX 40
#define HA_SCAN_MAX_FILE_BYTES (4 * 1024 * 1024)
#define HA_SCAN_MAX_HITS_PER_FILE 200
#define HA_SCAN_MAX_TEXT_BYTES (2 * 1024 * 1024) /* larger files are skipped and counted */
#define HA_PROMPT_ALL_LINES 80                   /* every match line is printed up to this many */
#define HA_PROMPT_MAX_ALIASES 20
#define HA_SCAN_BUDGET_MS 900 /* text scan, of the 1500 ms event deadline */
#define HA_PROMPT_MAX_PAIRS 3
#define HA_PROMPT_TRACE_DEPTH 6
#define HA_PROMPT_TRACE_WORK 5000
#define HA_DEADLINE_PROMPT_MS 1500

static bool ha_ident_start(unsigned char c) {
    return isalpha(c) || c == '_';
}

static bool ha_ident_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* Code rather than prose: a separator ('_', '::', '.'), a digit, or an inner
 * capital (camelCase, PascalCase with a hump, HTTPServer). ALLCAPS words and
 * Capitalised sentence starts are prose. */
static bool ha_token_looks_like_code(const std::string &t) {
    bool alpha = false;
    bool code = false;
    for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = (unsigned char)t[i];
        alpha |= isalpha(c) != 0;
        if (c == '_' || c == '.' || c == ':' || isdigit(c)) {
            code = true;
        } else if (i > 0 && isupper(c) &&
                   (islower((unsigned char)t[i - 1]) || isdigit((unsigned char)t[i - 1]) ||
                    (i + 1 < t.size() && islower((unsigned char)t[i + 1])))) {
            code = true;
        }
    }
    return alpha && code;
}

/* "hook_augment.cpp" names a file, not a symbol. */
static bool ha_token_is_filename(const std::string &t) {
    size_t dot = t.rfind('.');
    if (dot == std::string::npos || t.find("::") != std::string::npos) {
        return false;
    }
    static const char *const kExt[] = {
        "c",    "cc",  "cpp", "cxx", "h",     "hh",   "hpp",  "hxx",   "py",  "pyi",
        "js",   "jsx", "ts",  "tsx", "go",    "rs",   "java", "kt",    "cs",  "swift",
        "rb",   "php", "md",  "txt", "json",  "yaml", "yml",  "toml",  "sh",  "cmake",
        "html", "css", "sql", "lua", "zig",   "m",    "mm",   "scala", "ini", "cfg",
        "lock", "log", "csv", "xml", "proto", "rst",  "in",   "mk",    NULL,
    };
    std::string ext = t.substr(dot + 1);
    for (int i = 0; kExt[i]; ++i) {
        if (ext == kExt[i]) {
            return true;
        }
    }
    return false;
}

static void ha_prompt_add(std::vector<std::string> &out, const std::string &tok) {
    if (out.size() >= HA_PROMPT_MAX_CANDIDATES || tok.size() < HA_MIN_TOKEN ||
        !ha_request_identifier(tok) || ha_token_is_filename(tok) ||
        std::find(out.begin(), out.end(), tok) != out.end()) {
        return;
    }
    out.push_back(tok);
}

/* Identifier runs joined by "::" or "." in [begin, end); only code-looking
 * ones are kept. Tokens touching a path or address character ('/', '\\',
 * '@') are path components or e-mail parts, not symbols. */
static void ha_prompt_scan_plain(const char *begin, const char *end,
                                 std::vector<std::string> &out) {
    const char *p = begin;
    while (p < end && out.size() < HA_PROMPT_MAX_CANDIDATES) {
        if (!ha_ident_start((unsigned char)*p) ||
            (p > begin && ha_ident_char((unsigned char)p[-1]))) {
            ++p;
            continue;
        }
        const char *s = p;
        for (;;) {
            while (p < end && ha_ident_char((unsigned char)*p)) {
                ++p;
            }
            if (p + 2 < end && p[0] == ':' && p[1] == ':' && ha_ident_start((unsigned char)p[2])) {
                p += 2;
                continue;
            }
            if (p + 1 < end && p[0] == '.' && ha_ident_start((unsigned char)p[1])) {
                ++p;
                continue;
            }
            break;
        }
        bool pathlike = (s > begin && strchr("/\\@", s[-1])) || (p < end && strchr("/\\@", *p));
        std::string tok(s, p);
        if (!pathlike && ha_token_looks_like_code(tok)) {
            ha_prompt_add(out, tok);
        }
    }
}

/* Every code-looking identifier in the prompt, in order of appearance, at
 * most HA_PROMPT_MAX_CANDIDATES. Backtick-quoted identifiers are taken as
 * written (a plain lowercase name counts when quoted); fenced ``` blocks are
 * skipped — pasted code and logs are examples, not requests. */
static std::vector<std::string> ha_prompt_candidates(const char *prompt) {
    std::vector<std::string> out;
    if (!prompt) {
        return out;
    }
    const char *p = prompt;
    const char *plain = p; /* start of the current unquoted stretch */
    bool fenced = false;
    while (*p && out.size() < HA_PROMPT_MAX_CANDIDATES) {
        if (p[0] == '`' && p[1] == '`' && p[2] == '`') {
            if (!fenced) {
                ha_prompt_scan_plain(plain, p, out);
            }
            fenced = !fenced;
            p += 3;
            while (*p == '`') {
                ++p;
            }
            plain = p;
            continue;
        }
        if (fenced || *p != '`') {
            ++p;
            continue;
        }
        ha_prompt_scan_plain(plain, p, out);
        size_t ticks = 0;
        while (p[ticks] == '`') {
            ++ticks;
        }
        const char *body = p + ticks;
        const char *close = body;
        for (; *close; ++close) {
            size_t n = 0;
            while (close[n] == '`') {
                ++n;
            }
            if (n == ticks) {
                break;
            }
            if (n) {
                close += n - 1;
            }
        }
        if (!*close) {
            p = body; /* unterminated span: scan the rest as prose */
            plain = p;
            continue;
        }
        std::string span(body, close);
        size_t a = span.find_first_not_of(" \t");
        size_t b = span.find_last_not_of(" \t");
        span = a == std::string::npos ? std::string() : span.substr(a, b - a + 1);
        if (span.size() > 2 && span.compare(span.size() - 2, 2, "()") == 0) {
            span.resize(span.size() - 2);
        }
        if (ha_request_identifier(span)) {
            ha_prompt_add(out, span);
        } else {
            ha_prompt_scan_plain(body, close, out);
        }
        p = close + ticks;
        plain = p;
    }
    if (!fenced && out.size() < HA_PROMPT_MAX_CANDIDATES) {
        ha_prompt_scan_plain(plain, p + strlen(p), out);
    }
    return out;
}

/* ── Prompt targets ────────────────────────────────────────────────
 * Which symbols the request is about. When the prompt backticks any
 * identifier, only backticked identifiers are candidates: prose words, paths
 * and identifiers quoted as counter-examples ("not `X`", "e.g. `Y`", "such as
 * `Z`", bullets under a "Do NOT count:" header) are never resolved. A
 * qualifier is taken from the identifier itself (`Class::method`,
 * `Class.method`, `Class#method`, `pkg.Func(...)`) or from the prose around it
 * ("`m` of class `C`", "`m` method of the `C` interface", "defined in module
 * `M`"). Repo-relative paths are disambiguation hints, never candidates. */
struct ha_target {
    std::string name;              /* as written, qualifier included ("Signer.unsign") */
    std::string bare;              /* last segment ("unsign") */
    std::string qualifier;         /* "" or the owning class/module/package path */
    bool strong_qualifier = false; /* from "of class `C`"-style prose */
    std::vector<std::string> file_hints;
    size_t pos = 0;
    int para = 0; /* paragraph (blank-line separated) of the mention */
};

struct ha_prompt_parse {
    std::vector<ha_target> targets;
    std::vector<std::string> paths;
    bool backticked = false;
    bool callers_intent = false;
    bool chain_intent = false;
};

static bool ha_word_in(const std::string &w, std::initializer_list<const char *> set) {
    for (const char *s : set) {
        if (w == s) {
            return true;
        }
    }
    return false;
}

/* Words after which, in the same clause, a quoted identifier is an example or
 * an exclusion rather than the subject. */
static bool ha_negation_word(const std::string &w) {
    return ha_word_in(w, {"not",       "never",  "nor",       "eg",      "e.g",      "example",
                          "such",      "merely", "different", "ignore",  "ignoring", "exclude",
                          "excluding", "except", "unlike",    "instead", "similar",  "similarly",
                          "without",   "don't",  "doesn't",   "isn't",   "opposed"});
}

static bool ha_kind_word(const std::string &w) {
    return ha_word_in(w, {"class", "struct", "interface", "module", "namespace", "package", "type",
                          "enum", "trait", "protocol", "record", "object", "mixin", "inside",
                          "within"});
}

static bool ha_code_keyword(const std::string &w) {
    return ha_word_in(w, {"func",   "def",       "fn",        "function", "class",     "struct",
                          "return", "static",    "public",    "private",  "protected", "void",
                          "int",    "self",      "this",      "cls",      "yield",     "delegate",
                          "let",    "var",       "const",     "new",      "import",    "from",
                          "module", "package",   "interface", "type",     "enum",      "boolean",
                          "bool",   "string",    "true",      "false",    "null",      "nil",
                          "none",   "generated", "export",    "async",    "await",     "extern",
                          "inline", "virtual",   "override",  "final"});
}

/* Intent words. A request about callers, usages or the impact of a change
 * names them strongly; "used" or "signature" alone also appear in "where is
 * the function used in many places DEFINED?", so they count only when the
 * request is not a definition lookup. */
static bool ha_callers_word(const std::string &w) {
    return ha_word_in(w, {"call",    "calls",   "caller",     "callers",    "called",
                          "calling", "usages",  "impact",     "impacted",   "affected",
                          "affect",  "affects", "change",     "changes",    "changing",
                          "edit",    "modify",  "references", "referenced", "referencing"});
}

static bool ha_weak_callers_word(const std::string &w) {
    return ha_word_in(w, {"used", "usage", "uses", "use", "signature"});
}

static bool ha_locate_word(const std::string &w) {
    return ha_word_in(w, {"defined", "definition", "implementation", "implemented"});
}

/* A repository-relative file path ("src/db.c", "db/builder.h"): a slash, no
 * spaces, and a file extension. Directories ("deps/") are not file hints. */
static bool ha_is_repo_path(const std::string &t) {
    if (t.find('/') == std::string::npos || t.find(' ') != std::string::npos || t[0] == '/' ||
        t.back() == '/') {
        return false;
    }
    size_t slash = t.rfind('/');
    size_t dot = t.rfind('.');
    return dot != std::string::npos && dot > slash + 1 && dot + 1 < t.size();
}

/* Split "A::B.c#d" into segments. */
static std::vector<std::string> ha_qual_segments(const std::string &q) {
    std::vector<std::string> segs;
    std::string cur;
    for (size_t i = 0; i < q.size(); ++i) {
        char c = q[i];
        if (c == '.' || c == '#' || (c == ':' && i + 1 < q.size() && q[i + 1] == ':')) {
            if (!cur.empty()) {
                segs.push_back(cur);
            }
            cur.clear();
            i += c == ':';
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        segs.push_back(cur);
    }
    return segs;
}

/* The identifier a backtick span names, or "" when it is not one: a plain or
 * qualified identifier (`a`, `A::b`, `A.b`, `A#b`), or the callee of a call
 * or signature written as `name(...)` / `A.b(...)`. Receivers that are not
 * owners (`self.`, `this.`) and declarations (`int f(void)`) yield "". */
static std::string ha_span_identifier(std::string span) {
    size_t a = span.find_first_not_of(" \t");
    size_t b = span.find_last_not_of(" \t");
    if (a == std::string::npos) {
        return {};
    }
    span = span.substr(a, b - a + 1);
    size_t paren = span.find('(');
    if (paren != std::string::npos) {
        span.resize(paren);
    }
    /* Template arguments name no symbol: `CryptoDataReader<T>::fromPEMFile`. */
    std::string untemplated;
    int depth = 0;
    for (char c : span) {
        if (c == '<') {
            depth++;
        } else if (c == '>' && depth > 0) {
            depth--;
        } else if (depth == 0) {
            untemplated += c;
        }
    }
    span = untemplated;
    std::string norm;
    for (char c : span) {
        norm += c == '#' ? '.' : c;
    }
    if (!ha_request_identifier(norm)) {
        return {};
    }
    std::vector<std::string> segs = ha_qual_segments(norm);
    if (segs.size() > 1 && ha_word_in(segs[0], {"self", "this", "cls", "super"})) {
        return {};
    }
    if (segs.size() == 1 && ha_code_keyword(segs[0])) {
        return {};
    }
    return span; /* keep the '#' spelling for display; segments split on it */
}

static void ha_prompt_add_target(ha_prompt_parse &out, const std::string &name, size_t pos) {
    ha_target t;
    t.name = name;
    t.para = 0;
    std::vector<std::string> segs = ha_qual_segments(name);
    t.bare = segs.empty() ? name : segs.back();
    if (segs.size() > 1) {
        size_t cut = name.size() - t.bare.size();
        while (cut > 0 && (name[cut - 1] == '.' || name[cut - 1] == ':' || name[cut - 1] == '#')) {
            cut--;
        }
        t.qualifier = name.substr(0, cut);
    }
    t.pos = pos;
    out.targets.push_back(t);
}

static ha_prompt_parse ha_prompt_parse_request(const char *prompt) {
    ha_prompt_parse out;
    if (!prompt) {
        return out;
    }
    const std::string text = prompt;
    struct span_t {
        size_t pos;
        size_t end;
        std::string body;
        bool negated;
        std::string prev_word;  /* word before the span */
        std::string prev2_word; /* the one before that */
        std::string next_word;  /* word right after the span */
    };
    std::vector<span_t> spans;
    std::vector<std::pair<size_t, std::string>> path_tokens;

    bool fenced = false;
    bool neg = false;
    bool line_neg = false; /* this bullet sits under a "Do NOT ...:" header */
    bool list_neg = false; /* the previous header line was a negated list intro */
    bool line_start = true;
    bool line_has_neg = false;
    std::vector<bool> paren;
    std::string w1, w2; /* last two words, lowercased */
    bool weak_callers = false;
    bool locate_words = false;
    for (size_t i = 0; i < text.size();) {
        char c = text[i];
        if (text.compare(i, 3, "```") == 0) {
            fenced = !fenced;
            i += 3;
            continue;
        }
        if (fenced) {
            i++;
            continue;
        }
        if (line_start) {
            size_t j = i;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) {
                j++;
            }
            bool bullet =
                j + 1 < text.size() && (text[j] == '-' || text[j] == '*') && text[j + 1] == ' ';
            bool blank = j >= text.size() || text[j] == '\n';
            if (!bullet && !blank) {
                list_neg = false;
            }
            line_neg = bullet && list_neg;
            neg = line_neg;
            line_has_neg = false;
            line_start = false;
        }
        if (c == '\n') {
            /* A line that ends with ':' after a negation introduces a list of
             * exclusions ("Do NOT count:"). */
            size_t k = i;
            while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '\t' || text[k - 1] == '\r')) {
                k--;
            }
            if (k > 0 && text[k - 1] == ':' && line_has_neg) {
                list_neg = true;
            }
            paren.clear();
            line_start = true;
            w1.clear();
            w2.clear();
            i++;
            continue;
        }
        if (c == '(') {
            paren.push_back(neg);
            i++;
            continue;
        }
        if (c == ')') {
            if (!paren.empty()) {
                neg = paren.back();
                paren.pop_back();
            }
            i++;
            continue;
        }
        if ((c == '.' || c == '?' || c == '!' || c == ';') && i + 1 < text.size() &&
            isspace((unsigned char)text[i + 1]) && w1 != "e.g" && w1 != "i.e" && w1 != "eg" &&
            w1 != "ie" && w1 != "etc" && w1 != "vs") {
            neg = paren.empty() ? line_neg : neg;
            if (paren.empty()) {
                w1.clear();
                w2.clear();
            }
            i++;
            continue;
        }
        if (c == '`') {
            size_t ticks = 0;
            while (i + ticks < text.size() && text[i + ticks] == '`') {
                ticks++;
            }
            size_t close = text.find(std::string(ticks, '`'), i + ticks);
            if (close == std::string::npos) {
                i += ticks;
                continue;
            }
            span_t s;
            s.pos = i;
            s.end = close + ticks;
            s.body = text.substr(i + ticks, close - i - ticks);
            s.negated = neg;
            s.prev_word = w1;
            s.prev2_word = w2;
            size_t k = s.end;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) {
                k++;
            }
            while (k < text.size() && isalpha((unsigned char)text[k])) {
                s.next_word += (char)tolower((unsigned char)text[k++]);
            }
            spans.push_back(s);
            w2 = w1;
            w1 = "`";
            i = s.end;
            continue;
        }
        if (isalpha((unsigned char)c)) {
            size_t j = i;
            std::string w;
            while (j < text.size() &&
                   (isalnum((unsigned char)text[j]) || text[j] == '\'' ||
                    (text[j] == '.' && j + 1 < text.size() && isalpha((unsigned char)text[j + 1]) &&
                     j + 2 < text.size() && text[j + 2] == '.'))) {
                w += (char)tolower((unsigned char)text[j++]);
            }
            /* Unquoted repository paths ride along as hints. */
            size_t pe = j;
            while (pe < text.size() &&
                   (isalnum((unsigned char)text[pe]) || strchr("_./-+@", text[pe]) != nullptr)) {
                pe++;
            }
            std::string tok = text.substr(i, pe - i);
            while (!tok.empty() && strchr(".,;:", tok.back())) {
                tok.pop_back();
            }
            if (ha_is_repo_path(tok)) {
                path_tokens.emplace_back(i, tok);
                i += tok.size();
                continue;
            }
            if (ha_negation_word(w)) {
                neg = true;
                line_has_neg = true;
            }
            if (!neg && ha_callers_word(w)) {
                out.callers_intent = true;
            }
            if (!neg && ha_weak_callers_word(w)) {
                weak_callers = true;
            }
            if (!neg && ha_locate_word(w)) {
                locate_words = true;
            }
            if (!neg && (w == "chain" || w == "chains")) {
                out.chain_intent = true;
            }
            w2 = w1;
            w1 = w;
            i = j;
            continue;
        }
        i++;
    }

    out.callers_intent |= weak_callers && !locate_words;

    /* Classify spans. */
    std::vector<size_t> target_span; /* index into spans per target */
    for (size_t si = 0; si < spans.size(); ++si) {
        const span_t &s = spans[si];
        std::string body = s.body;
        size_t a = body.find_first_not_of(" \t");
        size_t b = body.find_last_not_of(" \t");
        body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
        if (ha_is_repo_path(body)) {
            path_tokens.emplace_back(s.pos, body);
            continue;
        }
        std::string ident = ha_span_identifier(body);
        if (ident.empty()) {
            continue;
        }
        /* "class `C`", "inside `f`", "the `C` interface": an owner, not a target. */
        bool owner = ha_kind_word(s.prev_word) ||
                     (ha_kind_word(s.next_word) && s.next_word != "inside" &&
                      s.next_word != "within" && (s.prev_word == "the" || s.prev_word == "of"));
        if (owner) {
            /* Bind to the most recent target in the same clause when the prose
             * says so ("`m` of class `C`", "`m` method of the `C` interface",
             * "`m` ... defined in module `C`"). */
            if (!out.targets.empty() && !s.negated) {
                ha_target &t = out.targets.back();
                size_t gap_start = spans[target_span.back()].end;
                std::string gap = text.substr(gap_start, s.pos - gap_start);
                bool linked = gap.size() < 80 && gap.find('\n') == std::string::npos &&
                              (gap.find(" of ") != std::string::npos ||
                               gap.find(" in ") != std::string::npos);
                if (linked && !t.strong_qualifier &&
                    !ha_word_in(s.prev_word, {"inside", "within"})) {
                    t.qualifier = ha_span_identifier(body);
                    t.strong_qualifier = true;
                }
            }
            continue;
        }
        if (s.negated) {
            continue;
        }
        out.backticked = true;
        ha_prompt_add_target(out, ident, s.pos);
        target_span.push_back(si);
    }

    /* File hints: paths after a target and before the next one. */
    std::sort(path_tokens.begin(), path_tokens.end());
    for (const auto &pt : path_tokens) {
        if (std::find(out.paths.begin(), out.paths.end(), pt.second) == out.paths.end()) {
            out.paths.push_back(pt.second);
        }
    }
    for (size_t ti = 0; ti < out.targets.size(); ++ti) {
        size_t from = out.targets[ti].pos;
        size_t to = ti + 1 < out.targets.size() ? out.targets[ti + 1].pos : from + 240;
        for (const auto &pt : path_tokens) {
            if (pt.first > from && pt.first < to && pt.first < from + 240) {
                out.targets[ti].file_hints.push_back(pt.second);
            }
        }
    }

    /* Paragraphs: the subject of a request is named in the first paragraph
     * that names anything; later paragraphs (rules, output format) only add
     * qualifiers and hints to it ("`Run`" -> "`Repairer::Run`") and never
     * new targets ("`snapshotRestoreCommandFunc` and `SnapshotRestoreCommandFunc`
     * are different"). */
    for (auto &t : out.targets) {
        int para = 0;
        for (size_t k = 0; k + 1 < t.pos && k + 1 < text.size(); ++k) {
            if (text[k] == '\n') {
                size_t m = k + 1;
                while (m < text.size() && (text[m] == ' ' || text[m] == '\t' || text[m] == '\r')) {
                    m++;
                }
                if (m < text.size() && text[m] == '\n' && m < t.pos) {
                    para++;
                    k = m - 1;
                }
            }
        }
        t.para = para;
    }
    /* ... and the subject is code: a branch name (`main`) or a file name
     * (`NEWS`) in an earlier paragraph does not make that paragraph the
     * subject when a later one names `CGAL::sdf_values`. */
    int subject_para = out.targets.empty() ? 0 : out.targets.front().para;
    for (const auto &t : out.targets) {
        if (!t.qualifier.empty() || t.strong_qualifier || ha_token_looks_like_code(t.bare)) {
            subject_para = t.para;
            break;
        }
    }

    /* Merge: an identifier that is the owner of another target is not a
     * target; repeated names keep the first mention, gaining the best
     * qualifier and every file hint. */
    std::vector<ha_target> merged;
    for (const auto &t : out.targets) {
        bool is_owner = false;
        for (const auto &o : out.targets) {
            std::vector<std::string> os = ha_qual_segments(o.name);
            std::vector<std::string> ts = ha_qual_segments(t.name);
            if (&o != &t && os.size() > ts.size() && std::equal(ts.begin(), ts.end(), os.begin())) {
                is_owner = true;
            }
            if (&o != &t && !o.qualifier.empty()) {
                /* "`m` of class `C`" then "`C`" again, or the enclosing
                 * module of a qualifier ("`A::B`" for owner "`A::B::C`"). */
                std::vector<std::string> qs = ha_qual_segments(o.qualifier);
                if ((ts.size() == 1 && qs.back() == ts.back()) ||
                    (ts.size() <= qs.size() && std::equal(ts.begin(), ts.end(), qs.begin()))) {
                    is_owner = true;
                }
            }
        }
        if (is_owner) {
            continue;
        }
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const ha_target &m) { return m.bare == t.bare; });
        if (it == merged.end()) {
            if (t.para == subject_para) {
                merged.push_back(t);
            }
            continue;
        }
        bool better = !t.qualifier.empty() &&
                      (it->qualifier.empty() || (t.strong_qualifier && !it->strong_qualifier));
        if (better) {
            it->qualifier = t.qualifier;
            it->strong_qualifier = t.strong_qualifier;
            if (it->name == it->bare) {
                it->name = t.name;
            }
        }
        for (const auto &h : t.file_hints) {
            if (std::find(it->file_hints.begin(), it->file_hints.end(), h) ==
                it->file_hints.end()) {
                it->file_hints.push_back(h);
            }
        }
    }
    for (auto &t : merged) {
        if (t.strong_qualifier && t.name == t.bare && !t.qualifier.empty()) {
            t.name = t.qualifier + "." + t.bare;
        }
    }
    if (merged.size() > HA_PROMPT_MAX_CANDIDATES) {
        merged.resize(HA_PROMPT_MAX_CANDIDATES);
    }
    out.targets = merged;

    /* No backticked identifier: fall back to code-looking prose tokens. */
    if (!out.backticked) {
        for (const auto &c : ha_prompt_candidates(prompt)) {
            ha_prompt_add_target(out, c, 0);
        }
    }
    for (auto &t : out.targets) {
        if (t.bare.size() < 2) {
            t.bare.clear();
        }
    }
    out.targets.erase(std::remove_if(out.targets.begin(), out.targets.end(),
                                     [](const ha_target &t) { return t.bare.empty(); }),
                      out.targets.end());
    return out;
}

/* ── Reading files ───────────────────────────────────────────────────
 * Everything the prompt context prints as `path:line: <source>` is read from
 * disk at prompt time, never from the index. */
struct ha_file_lines {
    bool ok = false;
    std::vector<std::string> lines;
};

struct ha_reader {
    std::string root;
    std::vector<std::pair<std::string, ha_file_lines>> cache;
};

static bool ha_safe_rel(const std::string &rel) {
    return !rel.empty() && rel[0] != '/' && rel.find('\\') == std::string::npos && rel != ".." &&
           rel.rfind("../", 0) != 0 && rel.find("/../") == std::string::npos;
}

static void ha_split_lines(const std::string &data, std::vector<std::string> *out) {
    size_t start = 0;
    while (start < data.size()) {
        size_t eol = data.find('\n', start);
        if (eol == std::string::npos) {
            out->push_back(data.substr(start));
            break;
        }
        size_t len = eol - start;
        if (len && data[eol - 1] == '\r') {
            len--;
        }
        out->push_back(data.substr(start, len));
        start = eol + 1;
    }
}

static const ha_file_lines &ha_read_file(ha_reader *r, const std::string &rel) {
    for (const auto &f : r->cache) {
        if (f.first == rel) {
            return f.second;
        }
    }
    ha_file_lines fl;
    FILE *fp = ha_safe_rel(rel) ? fopen((r->root + "/" + rel).c_str(), "rb") : nullptr;
    if (fp) {
        std::string data;
        data.resize(HA_SCAN_MAX_FILE_BYTES);
        size_t n = fread(&data[0], 1, data.size(), fp);
        fl.ok = !ferror(fp);
        fclose(fp);
        data.resize(n);
        ha_split_lines(data, &fl.lines);
    }
    r->cache.emplace_back(rel, std::move(fl));
    return r->cache.back().second;
}

static bool ha_line_has_ident(const std::string &line, const std::string &name) {
    if (name.empty()) {
        return false;
    }
    for (size_t pos = line.find(name); pos != std::string::npos; pos = line.find(name, pos + 1)) {
        bool left = pos == 0 || !ha_ident_char((unsigned char)line[pos - 1]);
        size_t end = pos + name.size();
        bool right = end >= line.size() || !ha_ident_char((unsigned char)line[end]);
        if (left && right) {
            return true;
        }
    }
    return false;
}

/* A source line as printed: leading whitespace dropped, ~120 chars kept. */
static std::string ha_src(const std::string &line, size_t max = HA_PROMPT_SRC_MAX);

static std::string ha_src(const std::string &line, size_t max) {
    size_t a = line.find_first_not_of(" \t");
    std::string s = a == std::string::npos ? std::string() : line.substr(a);
    while (!s.empty() && isspace((unsigned char)s.back())) {
        s.pop_back();
    }
    if (s.size() > max) {
        size_t cut = max;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) {
            cut--; /* never split a UTF-8 sequence */
        }
        s.resize(cut);
        s += "...";
    }
    return s;
}

/* The first line in [line, line + window) of `rel` that holds `name` as a
 * whole word (a node's start line can be a template header, an annotation or
 * a return type; the name follows). 0 when the file cannot be read or the
 * name is not there. */
static int ha_name_line(ha_reader *r, const std::string &rel, int line, const std::string &name,
                        int window) {
    const ha_file_lines &fl = ha_read_file(r, rel);
    if (!fl.ok || line < 1) {
        return 0;
    }
    for (int k = line; k < line + window && (size_t)k <= fl.lines.size(); ++k) {
        if (ha_line_has_ident(fl.lines[(size_t)k - 1], name)) {
            return k;
        }
    }
    return 0;
}

static std::string ha_line_text(ha_reader *r, const std::string &rel, int line) {
    const ha_file_lines &fl = ha_read_file(r, rel);
    return fl.ok && line >= 1 && (size_t)line <= fl.lines.size() ? fl.lines[(size_t)line - 1]
                                                                 : std::string();
}

/* ── Resolution ──────────────────────────────────────────────────────
 * The hook picks the node itself instead of letting a bare-name lookup guess.
 * Overloads of one function are one target; a qualifier that names a
 * different owner rejects the node. */
struct ha_node {
    std::string qn, name, label, file;
    int start_line = 0;
    int end_line = 0;
    std::vector<std::string> owner; /* enclosing class/namespace path, if the QN carries one */
    std::string display;            /* owner path + name, no project/module/synthetic parts */
    std::string def_key;            /* one function's identity across its overloads */
};

/* How far below a node's recorded start its name may sit: template headers,
 * annotations and preprocessor blocks come first (CGAL's sdf_values: 13
 * lines). Bounded by the node's own extent. */
static int ha_node_window(const ha_node &n) {
    int extent = n.end_line >= n.start_line ? n.end_line - n.start_line + 1 : 0;
    return std::max(HA_NAME_WINDOW, std::min(extent, HA_NAME_WINDOW_MAX));
}

static bool ha_label_is_symbol(const std::string &l) {
    return ha_word_in(l, {"Function", "Method", "Class", "Interface", "Struct", "Enum", "Type",
                          "Trait", "Declaration", "Macro", "Constructor", "Protocol", "Union",
                          "TypeAlias", "Object", "Record"});
}

static bool ha_label_is_callable(const std::string &l) {
    return l == "Function" || l == "Method" || l == "Constructor" || l == "Macro";
}

static std::string ha_strip_overload(const std::string &s) {
    size_t at = s.find("@overload");
    return at == std::string::npos ? s : s.substr(0, at);
}

static bool ha_all_caps(const std::string &s) {
    bool alpha = false;
    for (char c : s) {
        if (islower((unsigned char)c)) {
            return false;
        }
        alpha |= isupper((unsigned char)c) != 0;
    }
    return alpha;
}

/* Fill owner/display/def_key from the QN: drop the project prefix, the
 * file's module path, synthetic `__decl_*` scopes and `@overload_*` tags. */
static void ha_node_names(ha_node &n, const std::string &project) {
    std::string rest = ha_strip_overload(n.qn);
    if (!project.empty() && rest.compare(0, project.size() + 1, project + ".") == 0) {
        rest = rest.substr(project.size() + 1);
    }
    std::string module = n.file;
    size_t dot = module.rfind('.');
    size_t slash = module.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        module.resize(dot);
    }
    for (char &c : module) {
        if (c == '/') {
            c = '.';
        }
    }
    if (!module.empty() && rest.compare(0, module.size() + 1, module + ".") == 0) {
        rest = rest.substr(module.size() + 1);
    }
    std::vector<std::string> segs;
    for (const auto &s : ha_qual_segments(rest)) {
        if (s.rfind("__decl_", 0) != 0 && s.rfind("__", 0) != 0) {
            segs.push_back(s);
        }
    }
    std::string bare = n.name.empty() ? (segs.empty() ? rest : segs.back()) : n.name;
    if (!segs.empty() && segs.back() == bare) {
        segs.pop_back();
    }
    n.owner = segs;
    n.display.clear();
    for (const auto &s : segs) {
        n.display += s + ".";
    }
    n.display += bare;
    n.def_key = n.file + "|" + n.display;
}

/* Qualifier vs owner: -1 rejects (the node is owned by something else),
 * 1 is compatible (the QN carries no owner: a free function, whose namespace
 * the index may not record), 2+ matches that many trailing segments. */
static int ha_owner_score(const ha_node &n, const std::vector<std::string> &q) {
    if (q.empty()) {
        return 1;
    }
    if (n.owner.empty()) {
        /* No owner recorded: a namespace qualifier may still show in the
         * path (CGAL::Polygon_mesh_processing::centroid lives under
         * Polygon_mesh_processing/include/CGAL/Polygon_mesh_processing/). */
        int hits = 0;
        std::string path = "/" + n.file + "/";
        for (const auto &seg : q) {
            hits += path.find("/" + seg + "/") != std::string::npos;
        }
        return 1 + hits;
    }
    int k = 0;
    while (k < (int)q.size() && k < (int)n.owner.size() &&
           q[q.size() - 1 - (size_t)k] == n.owner[n.owner.size() - 1 - (size_t)k]) {
        k++;
    }
    if (k > 0) {
        return 1 + k;
    }
    /* A namespace qualifier ("pcpp::f", "CGAL::f") anywhere on the owner path. */
    for (const auto &o : n.owner) {
        if (o == q.back()) {
            return 2;
        }
    }
    return -1;
}

static std::string ha_file_stem(const std::string &file) {
    size_t slash = file.rfind('/');
    std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
    size_t dot = base.find('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

static bool ha_path_matches(const std::string &file, const std::string &hint) {
    if (file == hint) {
        return true;
    }
    return file.size() > hint.size() &&
           file.compare(file.size() - hint.size(), hint.size(), hint) == 0 &&
           file[file.size() - hint.size() - 1] == '/';
}

/* The qualifier segments that can be checked against an owner: macro
 * namespaces (ROCKSDB_NAMESPACE) and lower-case receivers/packages written
 * inline (`meta.f`, `schema.F`) are not owners the index records. */
static std::vector<std::string> ha_checkable_qualifier(const ha_target &t) {
    std::vector<std::string> q;
    for (const auto &s : ha_qual_segments(t.qualifier)) {
        if (!ha_all_caps(s)) {
            q.push_back(s);
        }
    }
    /* `obj.method` / `pkg.Func` written with a dot: a receiver or package,
     * not an owner. `ns::f` keeps a lower-case namespace (pcpp::, cv::). */
    if (!q.empty() && !t.strong_qualifier && islower((unsigned char)q.back()[0]) &&
        t.name.find("::") == std::string::npos) {
        q.clear();
    }
    return q;
}

/* Distinct definitions among nodes (overloads and declarations of one
 * function count once). */
static size_t ha_distinct_defs(const std::vector<ha_node> &nodes) {
    std::vector<std::string> keys;
    for (const auto &n : nodes) {
        if (n.label != "Declaration" &&
            std::find(keys.begin(), keys.end(), n.def_key) == keys.end()) {
            keys.push_back(n.def_key);
        }
    }
    return keys.size();
}

/* Narrow same-named nodes to the function the request means. The result is
 * one family (all definitions share one def_key; declarations ride along) or
 * an ambiguous set. */
static std::vector<ha_node> ha_disambiguate(std::vector<ha_node> nodes, const ha_target &t,
                                            const std::vector<std::string> &prompt_paths) {
    auto keep_if = [&nodes](auto pred, bool allow_empty) {
        std::vector<ha_node> kept;
        for (const auto &n : nodes) {
            if (pred(n)) {
                kept.push_back(n);
            }
        }
        if (!kept.empty() || allow_empty) {
            nodes = kept;
        }
    };
    /* Only symbols: an identifier that names only fields or variables is
     * "not in the code graph" as a function or type. */
    keep_if([](const ha_node &n) { return ha_label_is_symbol(n.label); }, true);
    /* One location reported twice (a Ruby method also emitted as a Function
     * by older indexes): keep the owned Method. */
    std::vector<ha_node> uniq;
    for (const auto &n : nodes) {
        auto dup = std::find_if(uniq.begin(), uniq.end(), [&](const ha_node &u) {
            return u.file == n.file && u.start_line == n.start_line;
        });
        if (dup == uniq.end()) {
            uniq.push_back(n);
        } else if (n.label == "Method" && dup->label != "Method") {
            *dup = n;
        }
    }
    nodes = uniq;
    /* Qualifier: a different owner rejects; a recorded matching owner wins,
     * keeping owner-less definitions only beside a matching declaration
     * (pcpp::fnvHash: declared in namespace pcpp in PacketUtils.h, defined
     * without the namespace in PacketUtils.cpp). */
    std::vector<std::string> q = ha_checkable_qualifier(t);
    if (!q.empty()) {
        /* Owned nodes match when the qualifier names their owner; owner-less
         * ones (free functions whose namespace the QN may not carry) are
         * judged by the qualifier segments in their path, or ride along
         * beside a matching declaration in the same-stem file. */
        std::vector<ha_node> kept;
        std::vector<std::string> stems;
        int best_hits = 0;
        for (const auto &n : nodes) {
            if (!n.owner.empty() && ha_owner_score(n, q) >= 2) {
                kept.push_back(n);
                stems.push_back(ha_file_stem(n.file));
            } else if (n.owner.empty()) {
                best_hits = std::max(best_hits, ha_owner_score(n, q) - 1);
            }
        }
        for (const auto &n : nodes) {
            if (!n.owner.empty()) {
                continue;
            }
            int hits = ha_owner_score(n, q) - 1;
            bool keep = best_hits > 0  ? hits == best_hits
                        : kept.empty() ? true
                                       : std::find(stems.begin(), stems.end(),
                                                   ha_file_stem(n.file)) != stems.end();
            if (keep) {
                kept.push_back(n);
            }
        }
        nodes = kept; /* may be empty: the qualifier names nothing in the graph */
    }
    if (ha_distinct_defs(nodes) > 1 && !t.file_hints.empty()) {
        keep_if(
            [&](const ha_node &n) {
                for (const auto &h : t.file_hints) {
                    if (ha_path_matches(n.file, h)) {
                        return true;
                    }
                }
                return false;
            },
            false);
    }
    if (ha_distinct_defs(nodes) > 1 && !prompt_paths.empty()) {
        keep_if(
            [&](const ha_node &n) {
                for (const auto &h : prompt_paths) {
                    if (ha_path_matches(n.file, h)) {
                        return true;
                    }
                }
                return false;
            },
            false);
    }
    if (ha_distinct_defs(nodes) > 1) {
        keep_if([](const ha_node &n) { return ha_label_is_callable(n.label); }, false);
    }
    return nodes;
}

static std::vector<ha_node> ha_parse_nodes(yyjson_doc *d, const std::string &project,
                                           std::string *root, int *total) {
    std::vector<ha_node> out;
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (root && ha_obj_str(r, "root")) {
        *root = ha_obj_str(r, "root");
    }
    if (total) {
        *total = ha_obj_int(r, "total");
    }
    size_t idx;
    size_t maxn;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(r, "nodes"), idx, maxn, v) {
        ha_node n;
        n.qn = ha_obj_str(v, "qualified_name") ? ha_obj_str(v, "qualified_name") : "";
        n.name = ha_obj_str(v, "name") ? ha_obj_str(v, "name") : "";
        n.label = ha_obj_str(v, "label") ? ha_obj_str(v, "label") : "";
        n.file = ha_obj_str(v, "file") ? ha_obj_str(v, "file") : "";
        n.start_line = ha_obj_int(v, "start_line");
        n.end_line = ha_obj_int(v, "end_line");
        if (!n.qn.empty()) {
            ha_node_names(n, project);
            out.push_back(n);
        }
    }
    return out;
}

/* ── Text files and the whole-word scan ───────────────────────────────
 * The scan covers every file git would track: the whole tree under the root
 * minus .git and whatever the .gitignore chain (and .git/info/exclude)
 * ignores, any extension. Binary files (a NUL byte in the first 8 KB) and
 * files over 2 MB are skipped and counted. The list is memoized under the
 * cache directory keyed by a signature over directory and .gitignore mtimes
 * (which a directory-only walk computes), so a warm prompt does not re-match
 * every file against the ignore rules. Parallel scan, bounded by a deadline. */
struct ha_match {
    int line;
    std::string text;
};

struct ha_scan_result {
    std::vector<std::pair<std::string, std::vector<ha_match>>> files; /* sorted by path */
    size_t files_read = 0;
    size_t files_total = 0;
    size_t not_indexed = 0; /* scanned files the index does not hold */
    size_t skipped_binary = 0;
    size_t skipped_large = 0;
    bool complete = false;
    long elapsed_ms = 0;
};

int cbm_gitignore_match_result(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir);

#ifndef _WIN32
static uint64_t ha_fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
}

struct ha_gi_link {
    cbm_gitignore_t *gi;
    std::string prefix; /* directory the .gitignore lives in ("" = root) */
    int parent;
};

/* Walk the tree under root the way git sees it. With files == nullptr only
 * directories are matched against the ignore rules (enough for the
 * signature); otherwise every non-ignored regular file is collected. */
static uint64_t ha_git_walk(const std::string &root, std::vector<std::string> *files) {
    uint64_t h = 1469598103934665603ULL;
    std::vector<ha_gi_link> links;
    cbm_gitignore_t *root_gi = cbm_gitignore_load((root + "/.gitignore").c_str());
    cbm_gitignore_t *excl = cbm_gitignore_load((root + "/.git/info/exclude").c_str());
    if (root_gi && excl) {
        (void)cbm_gitignore_merge(root_gi, excl);
        cbm_gitignore_free(excl);
    } else if (!root_gi) {
        root_gi = excl;
    }
    links.push_back({root_gi, "", -1});
    auto verdict = [&](int link, const std::string &rel, bool is_dir) {
        for (; link >= 0; link = links[(size_t)link].parent) {
            const ha_gi_link &l = links[(size_t)link];
            if (!l.gi) {
                continue;
            }
            const char *local = l.prefix.empty() ? rel.c_str() : rel.c_str() + l.prefix.size() + 1;
            int v = cbm_gitignore_match_result(l.gi, local, is_dir);
            if (v != 0) {
                return v;
            }
        }
        return 0;
    };
    std::vector<std::pair<std::string, int>> stack{{"", 0}};
    size_t dirs = 0;
    while (!stack.empty() && dirs < 400000) {
        auto [rel, link] = stack.back();
        stack.pop_back();
        std::string abs = rel.empty() ? root : root + "/" + rel;
        struct stat st;
        if (stat(abs.c_str(), &st) != 0) {
            continue;
        }
        dirs++;
        int64_t mt = (int64_t)st.st_mtime * 1000000000LL;
#if defined(__APPLE__)
        mt += st.st_mtimespec.tv_nsec;
#else
        mt += st.st_mtim.tv_nsec;
#endif
        h = ha_fnv(h, rel.data(), rel.size());
        h = ha_fnv(h, &mt, sizeof(mt));
        if (!rel.empty()) {
            struct stat gs;
            std::string gpath = abs + "/.gitignore";
            if (stat(gpath.c_str(), &gs) == 0 && S_ISREG(gs.st_mode)) {
                int64_t gm = (int64_t)gs.st_mtime;
                h = ha_fnv(h, &gm, sizeof(gm));
                h = ha_fnv(h, &gs.st_size, sizeof(gs.st_size));
                links.push_back({cbm_gitignore_load(gpath.c_str()), rel, link});
                link = (int)links.size() - 1;
            }
        } else {
            struct stat gs;
            if (stat((root + "/.gitignore").c_str(), &gs) == 0) {
                int64_t gm = (int64_t)gs.st_mtime;
                h = ha_fnv(h, &gm, sizeof(gm));
                h = ha_fnv(h, &gs.st_size, sizeof(gs.st_size));
            }
        }
        DIR *d = opendir(abs.c_str());
        if (!d) {
            continue;
        }
        while (struct dirent *e = readdir(d)) {
            const char *nm = e->d_name;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0 || strcmp(nm, ".git") == 0) {
                continue;
            }
            unsigned char type = e->d_type;
            std::string child = rel.empty() ? std::string(nm) : rel + "/" + nm;
            if (type == DT_UNKNOWN) {
                struct stat cs;
                if (lstat((root + "/" + child).c_str(), &cs) != 0) {
                    continue;
                }
                type = S_ISDIR(cs.st_mode) ? DT_DIR : S_ISREG(cs.st_mode) ? DT_REG : DT_LNK;
            }
            if (type == DT_DIR) {
                if (verdict(link, child, true) <= 0) {
                    stack.emplace_back(child, link);
                }
            } else if (type == DT_REG && files && verdict(link, child, false) <= 0) {
                files->push_back(child);
            }
        }
        closedir(d);
    }
    for (auto &l : links) {
        cbm_gitignore_free(l.gi);
    }
    return h;
}
#endif

/* Every file git would track under root, sorted. */
static std::vector<std::string> ha_text_files(const std::string &root, const std::string &project) {
    std::vector<std::string> found;
#ifndef _WIN32
    const char *cache = cbm_resolve_cache_dir();
    std::string memo;
    uint64_t sig = ha_git_walk(root, nullptr);
    if (cache && *cache && !project.empty()) {
        memo = std::string(cache) + "/hook-files/" + project + ".lst";
        FILE *fp = fopen(memo.c_str(), "r");
        if (fp) {
            char line[4096];
            bool valid = false;
            if (fgets(line, sizeof(line), fp)) {
                unsigned long long stored = 0;
                valid = sscanf(line, "sig %llx", &stored) == 1 && stored == sig;
            }
            while (valid && fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0]) {
                    found.emplace_back(line);
                }
            }
            fclose(fp);
            if (valid) {
                return found;
            }
            found.clear();
        }
    }
    (void)ha_git_walk(root, &found);
    std::sort(found.begin(), found.end());
    if (!memo.empty()) {
        std::string dir = memo.substr(0, memo.rfind('/'));
        cbm_mkdir_p(dir.c_str(), 0700);
        std::string tmp = memo + ".tmp." + std::to_string((long)getpid());
        FILE *fp = fopen(tmp.c_str(), "w");
        if (fp) {
            fprintf(fp, "sig %llx\n", (unsigned long long)sig);
            for (const auto &f : found) {
                fprintf(fp, "%s\n", f.c_str());
            }
            bool ok = fclose(fp) == 0;
            if (!ok || rename(tmp.c_str(), memo.c_str()) != 0) {
                unlink(tmp.c_str());
            }
        }
    }
#else
    /* No POSIX walk: fall back to full-index discovery (source files only). */
    cbm_discover_opts_t opts = {};
    opts.mode = CBM_MODE_FULL;
    opts.max_file_size = HA_SCAN_MAX_FILE_BYTES;
    cbm_file_info_t *list = nullptr;
    int count = 0;
    if (!root.empty() && cbm_discover(root.c_str(), &opts, &list, &count) == 0) {
        for (int i = 0; i < count; ++i) {
            if (list[i].rel_path) {
                found.emplace_back(list[i].rel_path);
            }
        }
        cbm_discover_free(list, count);
    }
    std::sort(found.begin(), found.end());
#endif
    return found;
}

/* The scan set: every tracked text file, plus indexed files the walk did not
 * produce (none in a normal checkout). *not_indexed counts scanned files the
 * index does not hold. */
static std::vector<std::string> ha_scan_files(const std::string &root, const std::string &project,
                                              const std::vector<std::string> &indexed,
                                              size_t *not_indexed) {
    std::vector<std::string> files = ha_text_files(root, project);
    std::vector<std::string> idx = indexed;
    std::sort(idx.begin(), idx.end());
    *not_indexed = 0;
    for (const auto &f : files) {
        *not_indexed += !std::binary_search(idx.begin(), idx.end(), f);
    }
    for (const auto &f : idx) {
        if (!std::binary_search(files.begin(), files.end(), f)) {
            files.push_back(f);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static void ha_scan_matches(const std::string &root, const std::vector<std::string> &files,
                            const std::string &name, std::chrono::steady_clock::time_point deadline,
                            ha_scan_result *out) {
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> next{0};
    std::atomic<size_t> read{0};
    std::atomic<size_t> binary{0};
    std::atomic<size_t> large{0};
    std::atomic<bool> timed_out{false};
    std::mutex mu;
    auto worker = [&]() {
        std::string buf;
        std::vector<std::pair<std::string, std::vector<ha_match>>> local;
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= files.size()) {
                break;
            }
            if ((i & 15) == 0 && std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            FILE *fp = fopen((root + "/" + files[i]).c_str(), "rb");
            if (!fp) {
                continue;
            }
            buf.resize(HA_SCAN_MAX_TEXT_BYTES + 1);
            size_t n = fread(&buf[0], 1, buf.size(), fp);
            fclose(fp);
            read++;
            if (n > HA_SCAN_MAX_TEXT_BYTES) {
                large++;
                continue;
            }
            if (memchr(buf.data(), '\0', std::min<size_t>(n, 8192))) {
                binary++;
                continue;
            }
            std::string_view hay(buf.data(), n);
            std::vector<ha_match> hits;
            int line_no = 0;
            size_t line_start = 0;
            size_t scanned_to = 0;
            for (size_t pos = hay.find(name); pos != std::string_view::npos;
                 pos = hay.find(name, pos + 1)) {
                bool left = pos == 0 || !ha_ident_char((unsigned char)hay[pos - 1]);
                size_t end = pos + name.size();
                bool right = end >= hay.size() || !ha_ident_char((unsigned char)hay[end]);
                if (!left || !right) {
                    continue;
                }
                while (scanned_to < pos) {
                    size_t nl = hay.find('\n', scanned_to);
                    if (nl == std::string_view::npos || nl >= pos) {
                        scanned_to = pos;
                        break;
                    }
                    line_no++;
                    line_start = nl + 1;
                    scanned_to = nl + 1;
                }
                int this_line = line_no + 1;
                if (!hits.empty() && hits.back().line == this_line) {
                    continue;
                }
                size_t eol = hay.find('\n', line_start);
                std::string text(hay.substr(
                    line_start, (eol == std::string_view::npos ? hay.size() : eol) - line_start));
                if (!text.empty() && text.back() == '\r') {
                    text.pop_back();
                }
                hits.push_back({this_line, text});
                if (hits.size() >= HA_SCAN_MAX_HITS_PER_FILE) {
                    break;
                }
            }
            if (!hits.empty()) {
                local.emplace_back(files[i], std::move(hits));
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        for (auto &l : local) {
            out->files.push_back(std::move(l));
        }
    };
    unsigned hw = std::thread::hardware_concurrency();
    unsigned nthreads = hw == 0 ? 4 : std::min(hw, 16u);
    std::vector<std::thread> pool;
    try {
        for (unsigned k = 1; k < nthreads; ++k) {
            pool.emplace_back(worker);
        }
    } catch (...) {
        /* fewer threads; the caller's thread still scans */
    }
    worker();
    for (auto &th : pool) {
        th.join();
    }
    std::sort(out->files.begin(), out->files.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    out->files_read = read;
    out->files_total = files.size();
    out->skipped_binary = binary;
    out->skipped_large = large;
    out->complete = !timed_out;
    out->elapsed_ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
}

/* Cheap, certain classification of a match line that is not a resolved
 * call: a comment, a string literal, an import/include. "" when unsure. */
static std::string ha_match_tag(const std::string &file, const std::string &text,
                                const std::string &name) {
    size_t a = text.find_first_not_of(" \t");
    std::string t = a == std::string::npos ? std::string() : text.substr(a);
    std::string ext =
        file.substr(file.rfind('.') == std::string::npos ? file.size() : file.rfind('.') + 1);
    bool hash_comment =
        ha_word_in(ext, {"py", "rb", "sh", "bash", "cmake", "yml", "yaml", "toml", "pl", "r", "txt",
                         "tcl", "pyi", "mk", "conf", "cfg", "ini", "dockerfile"});
    if (t.rfind("//", 0) == 0 || t.rfind("/*", 0) == 0 || t.rfind("* ", 0) == 0 || t == "*" ||
        t.rfind("--", 0) == 0 || (hash_comment && t.rfind("#", 0) == 0)) {
        return "comment";
    }
    if (t.rfind("#include", 0) == 0 || t.rfind("import ", 0) == 0 || t.rfind("from ", 0) == 0 ||
        t.rfind("require", 0) == 0 || t.rfind("using ", 0) == 0) {
        return "import";
    }
    /* The first whole-word occurrence: inside "..." or after //? */
    size_t pos = std::string::npos;
    for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1)) {
        bool left = p == 0 || !ha_ident_char((unsigned char)text[p - 1]);
        size_t e = p + name.size();
        bool right = e >= text.size() || !ha_ident_char((unsigned char)text[e]);
        if (left && right) {
            pos = p;
            break;
        }
    }
    if (pos == std::string::npos) {
        return {};
    }
    bool in_str = false;
    for (size_t k = 0; k < pos; ++k) {
        if (text[k] == '\\') {
            k++;
            continue;
        }
        if (text[k] == '"') {
            in_str = !in_str;
        }
        if (!in_str && text.compare(k, 2, "//") == 0) {
            return "comment";
        }
    }
    return in_str ? "string" : std::string();
}

/* ── Evidence blocks ──────────────────────────────────────────────────── */
struct ha_loc {
    std::string file;
    int line = 0;      /* where the name is (0: not found near the indexed line) */
    int node_line = 0; /* the line the index records */
    std::string label;
};

struct ha_symbol_facts {
    std::string display;
    std::string bare;
    std::string label;
    std::vector<ha_loc> defs;
    std::vector<ha_loc> decls;
    std::vector<std::pair<std::string, int>> call_lines; /* graph-resolved call sites */
    std::vector<std::string> caller_files;               /* graph rollup */
    std::vector<std::string> caller_names;               /* direct callers, by name */
    int callers_total = 0;
    int callees_total = 0;
    bool in_graph = true;
};

static bool ha_is_call_line(const ha_symbol_facts &f, const std::string &file, int line) {
    for (const auto &c : f.call_lines) {
        if (c.first == file && line >= c.second - 2 && line <= c.second + 2) {
            return true;
        }
    }
    return false;
}

static bool ha_is_loc(const std::vector<ha_loc> &locs, const std::string &file, int line) {
    for (const auto &l : locs) {
        if (l.file == file && (l.line == line || l.node_line == line)) {
            return true;
        }
    }
    return false;
}

/* Is some whole-word occurrence of name on this line followed by "(" ? */
static bool ha_line_calls(const std::string &text, const std::string &name) {
    for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1)) {
        bool left = p == 0 || !ha_ident_char((unsigned char)text[p - 1]);
        size_t e = p + name.size();
        if (!left || (e < text.size() && ha_ident_char((unsigned char)text[e]))) {
            continue;
        }
        while (e < text.size() && (text[e] == ' ' || text[e] == '\t')) {
            e++;
        }
        if (e < text.size() && text[e] == '(') {
            return true;
        }
    }
    return false;
}

/* What a match line is, from the graph first and the text second:
 * definition, declaration, call (graph-resolved), comment, string, import,
 * call (text only: `name(` the graph did not resolve), reference (no `(`:
 * a function value, address-of, macro argument, doc mention). */
static std::string ha_classify(const ha_symbol_facts &f, const std::string &file,
                               const ha_match &m) {
    if (ha_is_loc(f.defs, file, m.line)) {
        return "definition";
    }
    if (ha_is_loc(f.decls, file, m.line)) {
        return "declaration";
    }
    if (ha_is_call_line(f, file, m.line) && ha_line_calls(m.text, f.bare)) {
        return "call";
    }
    std::string tag = ha_match_tag(file, m.text, f.bare);
    if (!tag.empty()) {
        return tag;
    }
    if (ha_is_call_line(f, file, m.line)) {
        return "call";
    }
    return ha_line_calls(m.text, f.bare) ? "call (text only, not resolved by the graph)"
                                         : "reference";
}

static bool ha_tag_is_call(const std::string &t) {
    return t.rfind("call", 0) == 0;
}

static std::string ha_ext_of(const std::string &file) {
    size_t slash = file.rfind('/');
    size_t dot = file.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot == (slash == std::string::npos ? 0 : slash + 1)) {
        return "(none)";
    }
    return file.substr(dot);
}

/* Aliases a name-only grep would miss: `import ... NAME as A`, `A = NAME`,
 * `auto A = &NAME`, `#define A NAME`, `using A = NAME`, and imports of the
 * defining module (Python/JS/TS) followed by `module.NAME`. */
struct ha_alias_hit {
    std::string file;
    int line;
    std::string text;
    std::string kind;
};

static std::vector<ha_alias_hit> ha_find_aliases(const ha_symbol_facts &f,
                                                 const ha_scan_result &scan) {
    std::vector<ha_alias_hit> out;
    const std::string &n = f.bare;
    for (const auto &fm : scan.files) {
        for (const auto &m : fm.second) {
            std::string t = m.text;
            size_t a = t.find_first_not_of(" \t");
            t = a == std::string::npos ? std::string() : t.substr(a);
            size_t at = t.find(n);
            if (at == std::string::npos) {
                continue;
            }
            std::string after = t.substr(at + n.size());
            size_t k = after.find_first_not_of(" \t");
            std::string rest = k == std::string::npos ? std::string() : after.substr(k);
            if ((t.rfind("import ", 0) == 0 || t.rfind("from ", 0) == 0 ||
                 t.find("import {") != std::string::npos) &&
                rest.rfind("as ", 0) == 0) {
                out.push_back({fm.first, m.line, m.text, "import-as"});
                continue;
            }
            if (t.rfind("#define ", 0) == 0) {
                /* #define ALIAS NAME */
                std::string body = t.substr(8);
                size_t sp = body.find_first_of(" \t");
                std::string val = sp == std::string::npos ? std::string() : body.substr(sp);
                size_t vs = val.find_first_not_of(" \t");
                val = vs == std::string::npos ? std::string() : val.substr(vs);
                while (!val.empty() && isspace((unsigned char)val.back())) {
                    val.pop_back();
                }
                if (val == n && body.compare(0, n.size(), n) != 0) {
                    out.push_back({fm.first, m.line, m.text, "#define"});
                }
                continue;
            }
            if (t.rfind("using ", 0) == 0 && t.find('=') != std::string::npos && t.find('=') < at) {
                out.push_back({fm.first, m.line, m.text, "using"});
                continue;
            }
            /* ALIAS = NAME / ALIAS = &NAME / auto ALIAS = &NAME: the name is the
             * whole right-hand side. */
            size_t eq = t.find('=');
            if (eq != std::string::npos && eq < at && eq > 0 && t[eq - 1] != '=' &&
                t[eq - 1] != '!' && t[eq - 1] != '<' && t[eq - 1] != '>' &&
                (eq + 1 >= t.size() || t[eq + 1] != '=')) {
                std::string rhs = t.substr(eq + 1);
                std::string cleaned;
                for (char c : rhs) {
                    if (!isspace((unsigned char)c) && c != '&' && c != ';' && c != ',') {
                        cleaned += c;
                    }
                }
                if (cleaned == n) {
                    out.push_back({fm.first, m.line, m.text, "assignment"});
                }
            }
        }
    }
    return out;
}

static bool ha_is_script_ext(const std::string &ext) {
    return ha_word_in(ext, {".py", ".pyi", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs"});
}

/* Module imports of the defining file whose file then uses `X.NAME`, where X
 * is the module object the import binds: Python `import pkg.mod [as X]`,
 * `from pkg import mod [as X]`; JS/TS `import * as X from './mod'`,
 * `const X = require('./mod')`. A name-only grep finds those uses only by
 * the qualified spelling, which the scan already prints; this says which
 * files hold one. */
static std::vector<ha_alias_hit> ha_find_module_imports(
    const ha_symbol_facts &f, const std::string &root, const std::vector<std::string> &files,
    std::chrono::steady_clock::time_point deadline, ha_reader *rd) {
    std::vector<ha_alias_hit> out;
    if (f.defs.empty() || !ha_is_script_ext(ha_ext_of(f.defs.front().file))) {
        return out;
    }
    const std::string def_file = f.defs.front().file;
    const bool python = ha_ext_of(def_file) == ".py" || ha_ext_of(def_file) == ".pyi";
    std::string stem = ha_file_stem(def_file);
    if (stem.empty() || stem == f.bare) {
        return out;
    }
    std::string dotted = def_file.substr(0, def_file.rfind('.'));
    for (char &c : dotted) {
        if (c == '/') {
            c = '.';
        }
    }
    std::string parent = dotted.size() > stem.size()
                             ? dotted.substr(0, dotted.size() - stem.size() - 1)
                             : std::string();
    std::vector<std::string> scripts;
    for (const auto &file : files) {
        std::string ext = ha_ext_of(file);
        if (python ? (ext == ".py" || ext == ".pyi") : ha_is_script_ext(ext)) {
            scripts.push_back(file);
        }
    }
    ha_scan_result scan;
    ha_scan_matches(root, scripts, stem, deadline, &scan);
    auto word_after = [](const std::string &t, const std::string &key) {
        size_t k = t.find(key);
        if (k == std::string::npos) {
            return std::string();
        }
        k += key.size();
        size_t e = k;
        while (e < t.size() && ha_ident_char((unsigned char)t[e])) {
            e++;
        }
        return t.substr(k, e - k);
    };
    for (const auto &fm : scan.files) {
        for (const auto &m : fm.second) {
            std::string t = m.text;
            size_t a = t.find_first_not_of(" \t");
            t = a == std::string::npos ? std::string() : t.substr(a);
            std::string binding;
            if (python) {
                if (t.rfind("import " + dotted, 0) == 0) {
                    std::string as = word_after(t, " as ");
                    binding = as.empty() ? dotted : as;
                } else if (!parent.empty() && t.rfind("from " + parent + " import ", 0) == 0 &&
                           ha_line_has_ident(t.substr(t.find(" import ")), stem)) {
                    std::string as = word_after(t, stem + " as ");
                    binding = as.empty() ? stem : as;
                }
            } else {
                bool names_module = t.find("/" + stem + "'") != std::string::npos ||
                                    t.find("/" + stem + "\"") != std::string::npos ||
                                    t.find("/" + stem + ".js") != std::string::npos;
                if (names_module && t.find("* as ") != std::string::npos) {
                    binding = word_after(t, "* as ");
                } else if (names_module && t.find("require(") != std::string::npos) {
                    std::string lhs = t.substr(0, t.find('='));
                    size_t e = lhs.find_last_not_of(" \t");
                    size_t b2 =
                        e == std::string::npos ? std::string::npos : lhs.find_last_of(" \t", e);
                    binding = e == std::string::npos
                                  ? std::string()
                                  : lhs.substr(b2 == std::string::npos ? 0 : b2 + 1,
                                               e - (b2 == std::string::npos ? 0 : b2 + 1) + 1);
                }
            }
            if (binding.empty()) {
                continue;
            }
            std::string use = binding + "." + f.bare;
            const ha_file_lines &fl = ha_read_file(rd, fm.first);
            int used_at = 0;
            for (size_t L = 0; L < fl.lines.size() && !used_at; ++L) {
                if (fl.lines[L].find(use) != std::string::npos) {
                    used_at = (int)L + 1;
                }
            }
            if (used_at) {
                out.push_back(
                    {fm.first, used_at, fl.lines[(size_t)used_at - 1],
                     "module import at line " + std::to_string(m.line) + ", used as " + use});
            }
        }
    }
    return out;
}

/* Callers/usages/impact block. level 0: every match line (when there are at
 * most HA_PROMPT_ALL_LINES); 1: one summary line per file; 2: no per-file
 * listing (the table at the end still names every file). */
static std::string ha_callers_block(ha_reader *rd, const ha_symbol_facts &f,
                                    const ha_scan_result &scan,
                                    const std::vector<ha_alias_hit> &aliases, int level) {
    std::string out;
    if (!f.in_graph) {
        out = "- " + f.bare + " (not in the code graph; whole-word matches:)";
    } else {
        out = "- " + f.display + " (" + f.label;
        if (f.callers_total || f.callees_total) {
            out += "; graph: " + std::to_string(f.callers_total) + " callers, " +
                   std::to_string(f.callees_total) + " callees";
        }
        out += ")";
    }
    for (const auto &l : f.defs) {
        int L = l.line ? l.line : l.node_line;
        out += "\n  definition: " + l.file + ":" + std::to_string(L) + ": " +
               ha_src(ha_line_text(rd, l.file, L)) +
               (l.line ? std::string() : "  [stale: name not found near the indexed start]");
    }
    for (const auto &l : f.decls) {
        int L = l.line ? l.line : l.node_line;
        out += "\n  declaration: " + l.file + ":" + std::to_string(L) + ": " +
               ha_src(ha_line_text(rd, l.file, L));
    }
    size_t total = 0;
    struct row {
        std::string file;
        std::vector<std::pair<const ha_match *, std::string>> lines;
        int calls = 0;
    };
    std::vector<row> rows;
    std::vector<std::pair<std::string, int>> ext_counts;
    for (const auto &fm : scan.files) {
        row r;
        r.file = fm.first;
        for (const auto &m : fm.second) {
            std::string tag = ha_classify(f, fm.first, m);
            r.calls += ha_tag_is_call(tag);
            r.lines.emplace_back(&m, tag);
            total++;
        }
        std::string ext = ha_ext_of(fm.first);
        auto it = std::find_if(ext_counts.begin(), ext_counts.end(),
                               [&](const auto &e) { return e.first == ext; });
        if (it == ext_counts.end()) {
            ext_counts.emplace_back(ext, 1);
        } else {
            it->second++;
        }
        rows.push_back(std::move(r));
    }
    /* Every match line, or per-file summaries. */
    if (level == 0 && total <= HA_PROMPT_ALL_LINES) {
        out += "\n  every whole-word match (" + std::to_string(total) + " lines):";
        for (const auto &r : rows) {
            for (const auto &l : r.lines) {
                out += "\n    " + r.file + ":" + std::to_string(l.first->line) + ": " +
                       ha_src(l.first->text) + "  [" + l.second + "]";
            }
        }
    } else if (level <= 1) {
        out += "\n  matches per file (" + std::to_string(total) + " lines; listed by line number):";
        for (const auto &r : rows) {
            std::vector<std::pair<std::string, int>> kinds;
            for (const auto &l : r.lines) {
                std::string k = ha_tag_is_call(l.second) ? "calls" : l.second + "s";
                auto it = std::find_if(kinds.begin(), kinds.end(),
                                       [&](const auto &x) { return x.first == k; });
                if (it == kinds.end()) {
                    kinds.emplace_back(k, 1);
                } else {
                    it->second++;
                }
            }
            std::string kinds_s;
            for (const auto &k : kinds) {
                kinds_s += (kinds_s.empty() ? "" : ", ") + std::to_string(k.second) + " " + k.first;
            }
            std::string lines_s;
            for (const auto &l : r.lines) {
                lines_s += (lines_s.empty() ? "" : ", ") + std::to_string(l.first->line);
            }
            out += "\n    " + r.file + ": " + std::to_string(r.lines.size()) + " matches (" +
                   kinds_s + ") lines " + lines_s;
        }
    } else {
        out += "\n  (per-line listing dropped to fit the context budget; the file table below is "
               "complete)";
    }
    /* Non-call references. */
    std::string refs;
    size_t nrefs = 0;
    for (const auto &r : rows) {
        for (const auto &l : r.lines) {
            if (l.second == "reference") {
                nrefs++;
                if (level == 0) {
                    refs += "\n    " + r.file + ":" + std::to_string(l.first->line) + ": " +
                            ha_src(l.first->text);
                }
            }
        }
    }
    if (nrefs == 0) {
        out += "\n  non-call references (name not followed by \"(\", outside comments, strings, "
               "imports, definitions and declarations): none";
    } else {
        out += "\n  non-call references (name not followed by \"(\"): " + std::to_string(nrefs) +
               (level == 0 ? std::string(":") + refs
                           : std::string(" (lines tagged reference in the listing)"));
    }
    /* Aliases. */
    if (aliases.empty()) {
        out += "\n  aliases: none found (checked import-as, assignment, #define, using, module "
               "import)";
    } else {
        out += "\n  aliases (" + std::to_string(aliases.size()) + "):";
        size_t shown_aliases = 0;
        for (const auto &a : aliases) {
            if (shown_aliases++ >= HA_PROMPT_MAX_ALIASES) {
                out += "\n    and " + std::to_string(aliases.size() - HA_PROMPT_MAX_ALIASES) +
                       " more alias lines";
                break;
            }
            out += "\n    " + a.file + ":" + std::to_string(a.line) + ": " + ha_src(a.text) +
                   "  [" + a.kind + "]";
        }
    }
    /* Graph callers whose file no longer holds the name. */
    std::string missing;
    for (const auto &cf : f.caller_files) {
        bool seen = false;
        for (const auto &fm : scan.files) {
            seen |= fm.first == cf;
        }
        if (!seen && scan.complete) {
            missing += (missing.empty() ? "" : ", ") + cf;
        }
    }
    if (!missing.empty()) {
        out += "\n  graph-resolved callers with no whole-word `" + f.bare +
               "` on disk now (stale, re-check): " + missing;
    }
    /* The complete file table. */
    out += "\n  files containing `" + f.bare + "` (" + std::to_string(rows.size()) + "):";
    for (const auto &r : rows) {
        out += "\n    " + r.file + "  calls=" + std::to_string(r.calls) +
               " other=" + std::to_string((int)r.lines.size() - r.calls);
    }
    /* Totals. */
    std::sort(ext_counts.begin(), ext_counts.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::string by_ext;
    for (const auto &e : ext_counts) {
        by_ext += (by_ext.empty() ? "" : ", ") + e.first + " " + std::to_string(e.second);
    }
    out += "\n  scanned " +
           std::to_string(scan.files_read - scan.skipped_binary - scan.skipped_large) +
           " text files (all extensions, the files git would track; skipped: " +
           std::to_string(scan.skipped_binary) + " binary, " + std::to_string(scan.skipped_large) +
           " over 2 MB; vendored dirs: none skipped";
    if (scan.not_indexed) {
        out += "; " + std::to_string(scan.not_indexed) + " of them are not in the index";
    }
    out += "); " + std::to_string(rows.size()) + " files contain the whole word" +
           (by_ext.empty() ? std::string() : "; by extension: " + by_ext);
    if (!scan.complete) {
        out += "\n  (scan stopped at its time budget after " + std::to_string(scan.elapsed_ms) +
               " ms: " + std::to_string(scan.files_read) + " of " +
               std::to_string(scan.files_total) + " files read; the lists above are partial)";
    }
    return out;
}

static std::string ha_fit_callers(ha_reader *rd, const ha_symbol_facts &f,
                                  const ha_scan_result &scan,
                                  const std::vector<ha_alias_hit> &aliases, size_t budget) {
    std::string block;
    for (int level = 0; level <= 2; ++level) {
        block = ha_callers_block(rd, f, scan, aliases, level);
        if (block.size() <= budget) {
            return block;
        }
    }
    return block;
}

/* Locate block: the definition with its surroundings, every other
 * exact-name definition and declaration, and similarly named definitions. */
static std::string ha_locate_block(ha_reader *rd, const ha_symbol_facts &f,
                                   const std::vector<ha_node> &all_exact,
                                   const std::vector<ha_node> &similar, int level) {
    std::string out = "- " + f.display + " (" + f.label + ")";
    auto context = [&](const std::string &file, int L, int before, int after) {
        std::string s;
        const ha_file_lines &fl = ha_read_file(rd, file);
        for (int k = L - before; k <= L + after; ++k) {
            if (k < 1 || !fl.ok || (size_t)k > fl.lines.size()) {
                continue;
            }
            s += "\n    " + file + (k == L ? ":" : "-") + std::to_string(k) +
                 (k == L ? ": " : "- ") + ha_src(fl.lines[(size_t)k - 1]);
        }
        return s;
    };
    for (const auto &l : f.defs) {
        int L = l.line ? l.line : l.node_line;
        out += "\n  definition:";
        out += level == 0 ? context(l.file, L, 2, 3) : context(l.file, L, 0, 0);
        if (!l.line) {
            out += "  [stale: name not found near the indexed start]";
        }
    }
    /* Every exact-name node not already shown. */
    size_t ndefs = 0, ndecls = 0;
    std::string others, decls;
    for (const auto &n : all_exact) {
        if (!ha_label_is_symbol(n.label)) {
            continue;
        }
        int L = ha_name_line(rd, n.file, n.start_line, f.bare, ha_node_window(n));
        bool is_decl = n.label == "Declaration";
        (is_decl ? ndecls : ndefs)++;
        bool shown = false;
        for (const auto &l : f.defs) {
            shown |= l.file == n.file && (l.line == L || l.node_line == n.start_line);
        }
        if (shown) {
            continue;
        }
        std::string line = "\n    " + n.file + ":" + std::to_string(L ? L : n.start_line) + ": " +
                           ha_src(ha_line_text(rd, n.file, L ? L : n.start_line)) + "  (" +
                           n.display + ", " + n.label + ")";
        (is_decl ? decls : others) += line;
    }
    for (const auto &l : f.decls) {
        bool listed = false;
        for (const auto &n : all_exact) {
            listed |= n.label == "Declaration" && n.file == l.file && n.start_line == l.node_line;
        }
        if (!listed) {
            ndecls++;
            int L = l.line ? l.line : l.node_line;
            decls += "\n    " + l.file + ":" + std::to_string(L) + ": " +
                     ha_src(ha_line_text(rd, l.file, L));
        }
    }
    out += "\n  other exact-name definitions:" + (others.empty() ? std::string(" none") : others);
    out += "\n  declarations / prototypes:" + (decls.empty() ? std::string(" none") : decls);
    out += "\n  exact-name definitions: " + std::to_string(ndefs) +
           "; declarations: " + std::to_string(ndecls);
    std::string sim;
    for (const auto &n : similar) {
        int L = ha_name_line(rd, n.file, n.start_line, n.name, ha_node_window(n));
        sim += (sim.empty() ? " " : ", ") + std::string("`") + n.name + "` at " + n.file + ":" +
               std::to_string(L ? L : n.start_line);
    }
    out += "\n  similar names defined:" + (sim.empty() ? std::string(" none") : sim);
    return out;
}

/* The hook JSON for the label plus as many blocks as fit in max_bytes of
 * rendered output. A block that does not fit whole is cut at a line
 * boundary; nothing is ever cut inside a line or the JSON. NULL when no
 * block fits. */
static char *ha_prompt_payload(const std::vector<std::string> &blocks, size_t max_bytes,
                               const std::string &label) {
    std::string text = label;
    bool any = false;
    char *best = nullptr;
    for (const auto &block : blocks) {
        std::string candidate = block;
        bool fitted = false;
        for (;;) {
            std::string trial = text + "\n" + candidate;
            char *json = ha_render("UserPromptSubmit", trial.c_str());
            if (json && strlen(json) <= max_bytes) {
                free(best);
                best = json;
                text = trial;
                any = true;
                fitted = true;
                break;
            }
            free(json);
            size_t cut = candidate.rfind('\n');
            if (cut == std::string::npos) {
                break;
            }
            candidate.resize(cut);
        }
        if (!fitted || candidate.size() != block.size()) {
            break; /* budget exhausted */
        }
    }
    if (!any) {
        free(best);
        return nullptr;
    }
    return best;
}

static yyjson_doc *ha_prompt_inspect(cbm_mcp_server_t *srv, const char *project,
                                     const std::string &qn, bool *is_error) {
    yyjson_mut_doc *ad = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(ad);
    yyjson_mut_doc_set_root(ad, args);
    yyjson_mut_obj_add_str(ad, args, "project", project);
    yyjson_mut_obj_add_strncpy(ad, args, "symbol", qn.data(), qn.size());
    yyjson_mut_obj_add_int(ad, args, "source_lines", 0);
    yyjson_mut_obj_add_int(ad, args, "callers_limit", 2000);
    yyjson_mut_obj_add_int(ad, args, "callees_limit", 0);
    yyjson_mut_obj_add_int(ad, args, "max_bytes", 2000000);
    char *encoded = yyjson_mut_write(ad, 0, nullptr);
    yyjson_mut_doc_free(ad);
    *is_error = false;
    yyjson_doc *d = encoded ? ha_call(srv, "inspect_symbol", encoded, is_error) : nullptr;
    free(encoded);
    return d;
}

/* Graph facts for one resolved family: every definition and declaration
 * location (the line where the name actually is), the union of the graph's
 * call sites and caller files across overloads. */
static ha_symbol_facts ha_collect_facts(cbm_mcp_server_t *srv, const std::string &project,
                                        const std::vector<ha_node> &family, ha_reader *rd) {
    ha_symbol_facts f;
    f.bare = family.front().name.empty() ? family.front().display : family.front().name;
    std::vector<const ha_node *> defs, decls;
    for (const auto &n : family) {
        (n.label == "Declaration" ? decls : defs).push_back(&n);
    }
    const ha_node &lead = defs.empty() ? family.front() : *defs.front();
    f.display = lead.display;
    f.label = lead.label;
    if (defs.size() > 1) {
        f.label += ", " + std::to_string(defs.size()) + " overloads";
    }
    auto add_loc = [&](std::vector<ha_loc> &v, const std::string &file, int line,
                       const std::string &label, int window = HA_NAME_WINDOW) {
        for (const auto &l : v) {
            if (l.file == file && l.node_line == line) {
                return;
            }
        }
        ha_loc l;
        l.file = file;
        l.node_line = line;
        l.label = label;
        l.line = ha_name_line(rd, file, line, f.bare, window);
        v.push_back(l);
    };
    for (const auto *n : defs) {
        add_loc(f.defs, n->file, n->start_line, n->label, ha_node_window(*n));
    }
    for (const auto *n : decls) {
        add_loc(f.decls, n->file, n->start_line, n->label);
    }
    /* Inspect each definition (each overload carries its own callers). */
    std::vector<const ha_node *> probe = defs.empty() ? decls : defs;
    int probed = 0;
    for (const auto *n : probe) {
        if (probed++ >= 4) {
            break;
        }
        bool error = false;
        yyjson_doc *d = ha_prompt_inspect(srv, project.c_str(), n->qn, &error);
        yyjson_val *r = d && !error ? yyjson_doc_get_root(d) : nullptr;
        if (r && yyjson_obj_get(r, "symbol")) {
            f.callers_total +=
                ha_obj_int(r, "callers_total") + ha_obj_int(r, "related_tests_total");
            f.callees_total += ha_obj_int(r, "callees_total");
            size_t idx;
            size_t maxn;
            yyjson_val *v;
            for (const char *key : {"callers", "related_tests"}) {
                yyjson_arr_foreach(yyjson_obj_get(r, key), idx, maxn, v) {
                    const char *file = ha_obj_str(v, "file");
                    const char *cname = ha_obj_str(v, "name");
                    if (cname && std::find(f.caller_names.begin(), f.caller_names.end(), cname) ==
                                     f.caller_names.end()) {
                        f.caller_names.emplace_back(cname);
                    }
                    if (!file) {
                        continue;
                    }
                    size_t li;
                    size_t lm;
                    yyjson_val *lv;
                    yyjson_arr_foreach(yyjson_obj_get(v, "call_lines"), li, lm, lv) {
                        f.call_lines.emplace_back(file, (int)yyjson_get_int(lv));
                    }
                }
            }
            yyjson_arr_foreach(yyjson_obj_get(r, "caller_files"), idx, maxn, v) {
                const char *file = ha_obj_str(v, "file");
                if (file && std::find(f.caller_files.begin(), f.caller_files.end(), file) ==
                                f.caller_files.end()) {
                    f.caller_files.emplace_back(file);
                }
            }
            yyjson_arr_foreach(yyjson_obj_get(r, "declared_in"), idx, maxn, v) {
                const char *file = ha_obj_str(v, "file");
                if (file) {
                    add_loc(f.decls, file, ha_obj_int(v, "line"), "Declaration");
                }
            }
        }
        yyjson_doc_free(d);
    }
    std::sort(f.caller_files.begin(), f.caller_files.end());
    return f;
}

/* ── Chains ───────────────────────────────────────────────────────────
 * The chain, each hop's caller signature and call-site line (with one line
 * of context either side, level 0), and the facts behind "shortest": the
 * breadth-first frontier from the start, the target's direct callers, and
 * whether the target's name appears by text in an earlier hop's body. */
static yyjson_doc *ha_trace_outbound(cbm_mcp_server_t *srv, const char *project,
                                     const std::string &qn, int depth) {
    if (!srv || depth < 1) {
        return nullptr;
    }
    yyjson_mut_doc *ad = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(ad);
    yyjson_mut_doc_set_root(ad, args);
    yyjson_mut_obj_add_str(ad, args, "project", project);
    yyjson_mut_obj_add_strncpy(ad, args, "function_name", qn.data(), qn.size());
    yyjson_mut_obj_add_str(ad, args, "direction", "outbound");
    yyjson_mut_obj_add_int(ad, args, "depth", depth);
    yyjson_mut_obj_add_int(ad, args, "max_bytes", 400000);
    char *encoded = yyjson_mut_write(ad, 0, nullptr);
    yyjson_mut_doc_free(ad);
    bool error = false;
    yyjson_doc *d = encoded ? ha_call(srv, "trace_path", encoded, &error) : nullptr;
    free(encoded);
    if (error) {
        yyjson_doc_free(d);
        return nullptr;
    }
    return d;
}

static std::string ha_chain_block(cbm_mcp_server_t *srv, yyjson_doc *d, const std::string &project,
                                  ha_reader *rd, int level) {
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (!r || !yyjson_is_true(yyjson_obj_get(r, "path_found"))) {
        return {};
    }
    yyjson_val *path = yyjson_obj_get(r, "path");
    size_t n = (path && yyjson_is_arr(path)) ? yyjson_arr_size(path) : 0;
    if (n < 2) {
        return {};
    }
    std::vector<ha_node> hops;
    size_t idx;
    size_t maxn;
    yyjson_val *hop;
    yyjson_arr_foreach(path, idx, maxn, hop) {
        ha_node h;
        h.qn = ha_obj_str(hop, "qualified_name") ? ha_obj_str(hop, "qualified_name") : "";
        h.name = ha_obj_str(hop, "name") ? ha_obj_str(hop, "name") : "";
        h.file = ha_obj_str(hop, "file") ? ha_obj_str(hop, "file") : "";
        h.start_line = ha_obj_int(hop, "start_line");
        h.end_line = ha_obj_int(hop, "end_line");
        ha_node_names(h, project);
        hops.push_back(h);
    }
    const size_t H = n - 1;
    std::vector<int> name_at(n, 0);
    std::string out = "- call chain " + hops.front().display + " -> " + hops.back().display + " (" +
                      std::to_string(H) + " hop" + (H == 1 ? "" : "s") + "): ";
    for (size_t k = 0; k < n; ++k) {
        name_at[k] = ha_name_line(rd, hops[k].file, hops[k].start_line, hops[k].name,
                                  ha_node_window(hops[k]));
        out += (k ? " -> " : "") + hops[k].display + " (" + hops[k].file + ":" +
               std::to_string(name_at[k] ? name_at[k] : hops[k].start_line) + ")";
    }
    std::vector<int> edge_lines(n, 0);
    yyjson_arr_foreach(yyjson_obj_get(r, "caller_edges"), idx, maxn, hop) {
        yyjson_val *to = yyjson_obj_get(hop, "to_step");
        size_t step = to && yyjson_is_uint(to) ? (size_t)yyjson_get_uint(to) : 0;
        if (step > 0 && step < n) {
            edge_lines[step] = ha_obj_int(hop, "line");
        }
    }
    auto body_range = [](const ha_node &a) {
        int from = a.start_line > 0 ? a.start_line : 1;
        int to = a.end_line >= from ? a.end_line : from + 400;
        return std::make_pair(from, to);
    };
    /* First line in a's body (after its first line) naming `callee`. */
    auto find_in_body = [&](const ha_node &a, const std::string &callee) {
        auto [from, to] = body_range(a);
        const ha_file_lines &fl = ha_read_file(rd, a.file);
        for (int L = from + 1; fl.ok && L <= to && (size_t)L <= fl.lines.size(); ++L) {
            if (ha_line_has_ident(fl.lines[(size_t)L - 1], callee)) {
                return L;
            }
        }
        return 0;
    };
    for (size_t k = 1; k < n; ++k) {
        const ha_node &a = hops[k - 1];
        const ha_node &b = hops[k];
        int at = 0;
        if (edge_lines[k] > 0 &&
            ha_line_has_ident(ha_line_text(rd, a.file, edge_lines[k]), b.name)) {
            at = edge_lines[k];
        }
        if (!at) {
            at = find_in_body(a, b.name);
        }
        out += "\n    " + a.display + " -> " + b.display + ": ";
        if (!at) {
            out += "no line naming `" + b.name + "` found in " + a.display +
                   "'s body on disk (stale or indirect call, re-check)";
            continue;
        }
        out += a.file + ":" + std::to_string(at) + ": " + ha_src(ha_line_text(rd, a.file, at));
        if (level == 0) {
            int sig = name_at[k - 1] ? name_at[k - 1] : a.start_line;
            out += "\n      caller signature " + a.file + ":" + std::to_string(sig) + ": " +
                   ha_src(ha_line_text(rd, a.file, sig));
            for (int c : {at - 1, at + 1}) {
                std::string txt = ha_line_text(rd, a.file, c);
                if (c >= 1 && c != sig && !txt.empty()) {
                    out += "\n      " + a.file + "-" + std::to_string(c) + "- " + ha_src(txt);
                }
            }
        }
    }
    /* Shortest: the breadth-first frontier from the start. */
    const ha_node &first = hops.front();
    const ha_node &last = hops.back();
    std::string shortest = "\n  shortest: the chain above was found breadth-first over CALLS "
                           "edges, so no path of length 1";
    if (H > 2) {
        shortest += " to " + std::to_string(H - 1);
    }
    shortest += " exists in the graph";
    if (H == 1) {
        shortest = "\n  shortest: " + first.display + " calls " + last.display + " directly";
    }
    out += shortest +
           (yyjson_is_true(yyjson_obj_get(r, "traversal_truncated"))
                ? " (search bounded by its work limit)"
                : "") +
           ".";
    yyjson_doc *bfs =
        H > 1 ? ha_trace_outbound(srv, project.c_str(), first.qn, std::min<int>((int)H - 1, 3))
              : nullptr;
    if (bfs) {
        std::vector<std::string> depth1;
        std::vector<size_t> per_depth(4, 0);
        bool target_early = false;
        yyjson_val *callees = yyjson_obj_get(yyjson_doc_get_root(bfs), "callees");
        yyjson_arr_foreach(callees, idx, maxn, hop) {
            int h = ha_obj_int(hop, "hop");
            if (h < 1 || h > 3) {
                continue;
            }
            per_depth[(size_t)h]++;
            const char *nm = ha_obj_str(hop, "name");
            if (h == 1 && nm && std::find(depth1.begin(), depth1.end(), nm) == depth1.end()) {
                depth1.push_back(nm);
            }
            const char *q = ha_obj_str(hop, "qualified_name");
            target_early |= q && last.qn == q;
        }
        std::string d1;
        for (size_t k = 0; k < depth1.size() && k < 20; ++k) {
            d1 += (k ? ", " : "") + depth1[k];
        }
        if (depth1.size() > 20) {
            d1 += ", and " + std::to_string(depth1.size() - 20) + " others";
        }
        out += "\n  breadth-first from " + first.display + ": depth 1 = [" + d1 + "]";
        for (size_t dd = 2; dd < per_depth.size() && dd < H; ++dd) {
            out += "; depth " + std::to_string(dd) + " reached " + std::to_string(per_depth[dd]) +
                   " functions";
        }
        out += "; " + last.display + " first reached at depth " + std::to_string(H) +
               (target_early ? " (NOTE: the graph also reaches it earlier, re-check)" : "");
        yyjson_doc_free(bfs);
    }
    /* Direct callers of the target. */
    if (srv) {
        bool error = false;
        yyjson_doc *ins = ha_prompt_inspect(srv, project.c_str(), last.qn, &error);
        yyjson_val *ir = ins && !error ? yyjson_doc_get_root(ins) : nullptr;
        if (ir) {
            std::vector<std::string> names;
            for (const char *key : {"callers", "related_tests"}) {
                yyjson_arr_foreach(yyjson_obj_get(ir, key), idx, maxn, hop) {
                    const char *nm = ha_obj_str(hop, "name");
                    if (nm && std::find(names.begin(), names.end(), nm) == names.end()) {
                        names.push_back(nm);
                    }
                }
            }
            std::string list;
            for (size_t k = 0; k < names.size() && k < 30; ++k) {
                list += (k ? ", " : "") + names[k];
            }
            if (names.size() > 30) {
                list += ", and " + std::to_string(names.size() - 30) + " others";
            }
            out += "\n  direct callers of " + last.display + " (" + std::to_string(names.size()) +
                   "): " + (list.empty() ? std::string("none in the graph") : list);
        }
        yyjson_doc_free(ins);
    }
    /* Text check: does the target's name appear in an earlier hop's body? */
    std::string absent, present;
    for (size_t k = 0; k + 1 < H; ++k) {
        int L = find_in_body(hops[k], last.name);
        auto [from, to] = body_range(hops[k]);
        if (L) {
            present += (present.empty() ? "" : "; ") + hops[k].display + " at " + hops[k].file +
                       ":" + std::to_string(L) + ": " + ha_src(ha_line_text(rd, hops[k].file, L));
        } else {
            absent += (absent.empty() ? "" : ", ") + hops[k].display + " (" + hops[k].file + ":" +
                      std::to_string(from) + "-" + std::to_string(to) + ")";
        }
    }
    if (!absent.empty()) {
        out += "\n  `" + last.name + "` does not appear in the body of " + absent;
    }
    if (!present.empty()) {
        out += "\n  `" + last.name + "` appears by text in the body of " + present +
               " (not a graph edge; check whether it is a call)";
    }
    return out;
}

static std::string ha_prompt_trace(cbm_mcp_server_t *srv, const char *project,
                                   const std::string &from, const std::string &to, ha_reader *rd,
                                   size_t budget) {
    yyjson_mut_doc *ad = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(ad);
    yyjson_mut_doc_set_root(ad, args);
    yyjson_mut_obj_add_str(ad, args, "project", project);
    yyjson_mut_obj_add_strncpy(ad, args, "function_name", to.data(), to.size());
    yyjson_mut_obj_add_strncpy(ad, args, "from_function", from.data(), from.size());
    yyjson_mut_obj_add_str(ad, args, "direction", "inbound");
    yyjson_mut_obj_add_int(ad, args, "depth", HA_PROMPT_TRACE_DEPTH);
    yyjson_mut_obj_add_int(ad, args, "max_work", HA_PROMPT_TRACE_WORK);
    yyjson_mut_obj_add_int(ad, args, "max_bytes", 20000);
    char *encoded = yyjson_mut_write(ad, 0, nullptr);
    yyjson_mut_doc_free(ad);
    bool error = false;
    yyjson_doc *d = encoded ? ha_call(srv, "trace_path", encoded, &error) : nullptr;
    free(encoded);
    std::string block;
    if (!error) {
        block = ha_chain_block(srv, d, project, rd, 0);
        if (block.size() > budget) {
            block = ha_chain_block(srv, d, project, rd, 1);
        }
    }
    yyjson_doc_free(d);
    return block;
}

/* ── Session state for the PreToolUse dedupe ─────────────────────────
 * The UserPromptSubmit handler records which names its block covered; a
 * later search for one of them in the same session adds nothing new. */
static std::string ha_session_file(const char *session_id) {
    if (!session_id || !*session_id) {
        return {};
    }
    std::string id;
    for (const char *p = session_id; *p && id.size() < 128; ++p) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
            id += *p;
        }
    }
    const char *cache = cbm_resolve_cache_dir();
    if (id.empty() || !cache || !*cache) {
        return {};
    }
    return std::string(cache) + "/hook-sessions/" + id;
}

static void ha_session_record(const char *session_id, const std::vector<std::string> &names) {
    std::string path = ha_session_file(session_id);
    if (path.empty() || names.empty()) {
        return;
    }
    std::string dir = path.substr(0, path.rfind('/'));
    cbm_mkdir_p(dir.c_str(), 0700);
    FILE *fp = fopen(path.c_str(), "a");
    if (!fp) {
        return;
    }
    for (const auto &n : names) {
        fprintf(fp, "%s\n", n.c_str());
    }
    fclose(fp);
}

static bool ha_session_covers(const char *session_id, const char *name) {
    std::string path = ha_session_file(session_id);
    if (path.empty() || !name) {
        return false;
    }
    FILE *fp = fopen(path.c_str(), "r");
    if (!fp) {
        return false;
    }
    char line[512];
    bool hit = false;
    while (!hit && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        hit = strcmp(line, name) == 0;
    }
    fclose(fp);
    return hit;
}

/* ── The prompt context ───────────────────────────────────────────────── */
static bool ha_generic_name(const std::string &n) {
    return ha_word_in(
        n, {"main",    "init",    "run",    "get",     "set",      "test",   "value",  "data",
            "start",   "stop",    "open",   "close",   "read",     "write",  "update", "create",
            "add",     "remove",  "delete", "find",    "size",     "name",   "type",   "call",
            "apply",   "process", "handle", "execute", "load",     "save",   "reset",  "clear",
            "next",    "begin",   "end",    "push",    "pop",      "put",    "map",    "list",
            "key",     "item",    "result", "error",   "info",     "debug",  "log",    "print",
            "parse",   "format",  "config", "options", "setup",    "build",  "make",   "copy",
            "compare", "equals",  "hash",   "check",   "validate", "render", "send",   "receive",
            "connect", "flush",   "index",  "count",   "length",   "self"});
}

/* Resolve the targets against the first indexed project at or above cwd.
 * *indexed reports whether such a project exists (so the caller can choose
 * the unindexed fallback); *covered gets the names the block covers.
 * Returns the hook JSON or NULL. */
static char *ha_prompt_context(cbm_mcp_server_t *srv, const char *cwd, const ha_prompt_parse &req,
                               bool *indexed, std::vector<std::string> *covered) {
    *indexed = false;
    const auto start = std::chrono::steady_clock::now();
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", cwd);
    char *project = nullptr;
    char *first = nullptr;
    for (int level = 0; level < HA_MAX_WALKUP && cbm_hook_path_is_abs(dir); level++) {
        char *name = cbm_project_name_from_path(dir);
        if (name) {
            bool resolved = false;
            char *json = cbm_mcp_symbol_nodes(srv, name, req.targets[0].bare.c_str(), &resolved);
            if (resolved) {
                project = name;
                first = json;
                break;
            }
            free(json);
            free(name);
        }
        if (!ha_strip_last_component(dir)) {
            break;
        }
    }
    if (!project) {
        return nullptr;
    }
    *indexed = true;
    const std::string proj = project;
    free(project);

    const bool chain_mode = req.chain_intent;
    const bool callers_mode = req.callers_intent && !chain_mode;
    std::string root;
    ha_reader rd;

    /* Resolve every target first: generic or very common names lose to a
     * qualified or rarer target. */
    struct resolved_t {
        const ha_target *t;
        std::vector<ha_node> nodes; /* the chosen family (or the ambiguous set) */
        std::vector<ha_node> all;   /* every node with the exact name */
        int total;
    };
    std::vector<resolved_t> res;
    for (size_t i = 0; i < req.targets.size(); ++i) {
        const ha_target &t = req.targets[i];
        char *json =
            i == 0 ? first : cbm_mcp_symbol_nodes(srv, proj.c_str(), t.bare.c_str(), nullptr);
        yyjson_doc *nd = json ? yyjson_read(json, strlen(json), 0) : nullptr;
        free(json);
        int total = 0;
        std::vector<ha_node> nodes =
            ha_parse_nodes(nd, proj, root.empty() ? &root : nullptr, &total);
        yyjson_doc_free(nd);
        res.push_back({&t, ha_disambiguate(nodes, t, req.paths), nodes, total});
    }
    rd.root = root.empty() ? std::string(dir) : root;
    auto weak = [](const resolved_t &r) {
        bool qualified = r.t->strong_qualifier || !ha_checkable_qualifier(*r.t).empty();
        return !qualified && (ha_generic_name(r.t->bare) || r.total >= HA_PROMPT_COMMON_NODES);
    };
    bool has_specific = false;
    for (const auto &r : res) {
        has_specific |= !weak(r);
    }
    if (has_specific) {
        res.erase(std::remove_if(res.begin(), res.end(), weak), res.end());
    }

    /* Files for the scan: every file git would track, listed once. */
    std::vector<std::string> files;
    size_t not_indexed = 0;
    auto load_files = [&]() {
        if (!files.empty()) {
            return;
        }
        char *froot = nullptr;
        char **flist = nullptr;
        int fc = cbm_mcp_project_files(srv, proj.c_str(), &froot, &flist);
        std::vector<std::string> indexed_files;
        for (int k = 0; k < fc; ++k) {
            indexed_files.emplace_back(flist[k]);
        }
        cbm_mcp_free_project_files(froot, flist, fc < 0 ? 0 : fc);
        auto t_disc = std::chrono::steady_clock::now();
        files = ha_scan_files(rd.root, proj, indexed_files, &not_indexed);
        if (getenv("CBM_HOOK_TIMING")) {
            fprintf(stderr, "hook.timing files_ms=%lld files=%zu\n",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_disc)
                        .count(),
                    files.size());
        }
    };

    bool any_resolved = false;
    bool prose_listed = false;
    for (const auto &r : res) {
        any_resolved |= !r.nodes.empty();
    }
    std::vector<std::string> blocks;
    std::vector<std::string> names;
    struct fn_t {
        std::vector<std::string> qns;
    };
    std::vector<fn_t> chain_ends;
    size_t budget = HA_PROMPT_MAX_BYTES - 1200;
    size_t per_target = res.empty() ? budget : budget / res.size();
    bool scanned_any = false;
    size_t scanned_files = 0;
    for (const auto &r : res) {
        const ha_target &t = *r.t;
        if (r.nodes.empty() && !req.backticked &&
            (any_resolved || prose_listed || t.bare.find('_') == std::string::npos)) {
            /* A prose token the graph does not hold: worth a text listing
             * only when it is unmistakably an identifier (seq_pass_fn) and
             * nothing else in the request resolved. */
            continue;
        }
        if (ha_distinct_defs(r.nodes) > 1) {
            std::string block = "- " + t.name + ": " + std::to_string(ha_distinct_defs(r.nodes)) +
                                " definitions, none chosen (name the class, namespace or file):";
            for (const auto &n : r.nodes) {
                if (n.label == "Declaration") {
                    continue;
                }
                int at = ha_name_line(&rd, n.file, n.start_line, t.bare, ha_node_window(n));
                int L = at ? at : n.start_line;
                block += "\n    " + n.file + ":" + std::to_string(L) + ": " +
                         ha_src(ha_line_text(&rd, n.file, L)) + "  (" + n.display + ", " + n.label +
                         ")";
            }
            size_t ndecl = 0;
            for (const auto &n : r.all) {
                ndecl += n.label == "Declaration";
            }
            block += "\n  exact-name definitions: " + std::to_string(ha_distinct_defs(r.nodes)) +
                     "; declarations: " + std::to_string(ndecl);
            blocks.push_back(block);
            names.push_back(t.bare);
            continue;
        }
        ha_symbol_facts f;
        if (r.nodes.empty()) {
            prose_listed |= !req.backticked;
            f.in_graph = false;
            f.bare = t.bare;
            f.display = t.name;
        } else {
            f = ha_collect_facts(srv, proj, r.nodes, &rd);
        }
        names.push_back(f.bare);
        if (chain_mode) {
            if (!r.nodes.empty()) {
                fn_t fn;
                for (const auto &n : r.nodes) {
                    if (n.label != "Declaration") {
                        fn.qns.push_back(n.qn);
                    }
                }
                if (fn.qns.empty()) {
                    fn.qns.push_back(r.nodes.front().qn);
                }
                chain_ends.push_back(fn);
                std::string block = "- " + f.display + " (" + f.label + ")";
                for (const auto &l : f.defs) {
                    int L = l.line ? l.line : l.node_line;
                    block += "\n  definition: " + l.file + ":" + std::to_string(L) + ": " +
                             ha_src(ha_line_text(&rd, l.file, L));
                }
                blocks.push_back(block);
            }
            continue;
        }
        if (!callers_mode && f.in_graph) {
            /* Locate: the definition in context, every exact-name node, and
             * names that share the stem. */
            std::vector<ha_node> similar;
            char *json = cbm_mcp_symbol_nodes(srv, proj.c_str(), f.bare.c_str(), nullptr, true);
            yyjson_doc *nd = json ? yyjson_read(json, strlen(json), 0) : nullptr;
            free(json);
            for (auto &n : ha_parse_nodes(nd, proj, nullptr, nullptr)) {
                if (ha_label_is_symbol(n.label) && n.label != "Declaration" &&
                    similar.size() < 12) {
                    similar.push_back(n);
                }
            }
            yyjson_doc_free(nd);
            std::string block = ha_locate_block(&rd, f, r.all, similar, 0);
            if (block.size() > per_target) {
                block = ha_locate_block(&rd, f, r.all, similar, 1);
            }
            blocks.push_back(block);
            continue;
        }
        load_files();
        ha_scan_result scan;
        std::vector<ha_alias_hit> aliases;
        if (!files.empty()) {
            const auto deadline = start + std::chrono::milliseconds(HA_SCAN_BUDGET_MS);
            ha_scan_matches(rd.root, files, f.bare, deadline, &scan);
            scan.not_indexed = not_indexed;
            aliases = ha_find_aliases(f, scan);
            for (auto &m : ha_find_module_imports(f, rd.root, files, deadline, &rd)) {
                aliases.push_back(m);
            }
            if (getenv("CBM_HOOK_TIMING")) {
                fprintf(stderr, "hook.timing scan_ms=%ld read=%zu\n", scan.elapsed_ms,
                        scan.files_read);
            }
            scanned_any = true;
            scanned_files = scan.files_read - scan.skipped_binary - scan.skipped_large;
        }
        blocks.push_back(ha_fit_callers(&rd, f, scan, aliases, per_target));
    }

    std::vector<std::string> chains;
    int pairs = 0;
    for (size_t i = 0; i < chain_ends.size() && pairs < HA_PROMPT_MAX_PAIRS; ++i) {
        for (size_t j = i + 1; j < chain_ends.size() && pairs < HA_PROMPT_MAX_PAIRS; ++j) {
            pairs++;
            std::string line;
            for (int orient = 0; orient < 2 && line.empty(); ++orient) {
                const fn_t &a = orient == 0 ? chain_ends[i] : chain_ends[j];
                const fn_t &b = orient == 0 ? chain_ends[j] : chain_ends[i];
                for (size_t x = 0; x < a.qns.size() && x < 3 && line.empty(); ++x) {
                    for (size_t y = 0; y < b.qns.size() && y < 3 && line.empty(); ++y) {
                        line =
                            ha_prompt_trace(srv, proj.c_str(), a.qns[x], b.qns[y], &rd, budget / 2);
                    }
                }
            }
            if (!line.empty()) {
                chains.push_back(line);
            }
        }
    }
    blocks.insert(blocks.begin(), chains.begin(), chains.end());
    if (blocks.empty()) {
        return nullptr;
    }
    std::string joined;
    std::vector<std::string> uniq;
    for (const auto &n : names) {
        if (std::find(uniq.begin(), uniq.end(), n) == uniq.end()) {
            uniq.push_back(n);
            joined += (joined.empty() ? "" : " ") + n;
        }
    }
    std::string label =
        scanned_any
            ? "[code-cortex] Read from disk at prompt time (equivalent to `grep -rnw " + joined +
                  "` over the " + std::to_string(scanned_files) +
                  " text files git would track, plus call resolution from the code graph). The "
                  "files were read when this prompt was submitted, so running that search again "
                  "returns these same lines; answer from them, and search only for what is not "
                  "shown:"
            : std::string(
                  "[code-cortex] Read from disk at prompt time (each line below re-read "
                  "from its file, plus call resolution from the code graph). The files were "
                  "read when this prompt was submitted, so reading them again returns "
                  "these same lines; answer from them, and search only for what is not "
                  "shown:");
    *covered = names;
    return ha_prompt_payload(blocks, HA_PROMPT_MAX_BYTES, label);
}

#ifdef CBM_ENABLE_TEST_SEAMS
char *cbm_prompt_candidates_for_testing(const char *prompt) {
    std::vector<std::string> cands = ha_prompt_candidates(prompt);
    if (cands.empty()) {
        return nullptr;
    }
    std::string joined;
    for (const auto &c : cands) {
        joined += (joined.empty() ? "" : "\n") + c;
    }
    return strdup(joined.c_str());
}
char *cbm_prompt_targets_for_testing(const char *prompt) {
    ha_prompt_parse req = ha_prompt_parse_request(prompt);
    std::string out = std::string("intent=") + (req.callers_intent ? "callers" : "-") +
                      (req.chain_intent ? ",chain" : "");
    for (const auto &t : req.targets) {
        out += "\n" + t.name + "|" + t.qualifier + "|";
        for (size_t k = 0; k < t.file_hints.size(); ++k) {
            out += (k ? "," : "") + t.file_hints[k];
        }
    }
    return strdup(out.c_str());
}
char *cbm_prompt_resolve_for_testing(const char *nodes_json, const char *prompt) {
    ha_prompt_parse req = ha_prompt_parse_request(prompt);
    yyjson_doc *nd = nodes_json ? yyjson_read(nodes_json, strlen(nodes_json), 0) : nullptr;
    std::vector<ha_node> all = ha_parse_nodes(nd, "p", nullptr, nullptr);
    yyjson_doc_free(nd);
    if (req.targets.empty()) {
        return nullptr;
    }
    std::vector<ha_node> picked = ha_disambiguate(all, req.targets[0], req.paths);
    std::string out;
    for (const auto &n : picked) {
        out += (out.empty() ? "" : "\n") + n.qn;
    }
    return out.empty() ? nullptr : strdup(out.c_str());
}
char *cbm_prompt_display_for_testing(const char *qn, const char *file) {
    ha_node n;
    n.qn = qn ? qn : "";
    n.file = file ? file : "";
    ha_node_names(n, "p");
    return strdup(n.display.c_str());
}
/* facts_json: {"display","bare","label","defs":[[file,line]],"decls":[[file,line]],
 *              "calls":[[file,line]],"caller_files":[...],"in_graph":bool} */
static bool ha_test_facts(const char *facts_json, ha_reader &rd, ha_symbol_facts &f) {
    yyjson_doc *doc = facts_json ? yyjson_read(facts_json, strlen(facts_json), 0) : nullptr;
    yyjson_val *r = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!r) {
        return false;
    }
    f.display = ha_obj_str(r, "display") ? ha_obj_str(r, "display") : "";
    f.bare = ha_obj_str(r, "bare") ? ha_obj_str(r, "bare") : "";
    f.label = ha_obj_str(r, "label") ? ha_obj_str(r, "label") : "";
    f.in_graph = !yyjson_is_false(yyjson_obj_get(r, "in_graph"));
    auto pairs = [&](const char *key, auto add) {
        size_t idx;
        size_t maxn;
        yyjson_val *v;
        yyjson_arr_foreach(yyjson_obj_get(r, key), idx, maxn, v) {
            add(yyjson_get_str(yyjson_arr_get(v, 0)), (int)yyjson_get_int(yyjson_arr_get(v, 1)));
        }
    };
    pairs("defs", [&](const char *file, int line) {
        ha_loc l;
        l.file = file;
        l.node_line = line;
        l.line = ha_name_line(&rd, file, line, f.bare, 12);
        f.defs.push_back(l);
    });
    pairs("decls", [&](const char *file, int line) {
        ha_loc l;
        l.file = file;
        l.node_line = line;
        l.line = ha_name_line(&rd, file, line, f.bare, 12);
        f.decls.push_back(l);
    });
    pairs("calls", [&](const char *file, int line) { f.call_lines.emplace_back(file, line); });
    size_t idx;
    size_t maxn;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(r, "caller_files"), idx, maxn, v) {
        f.caller_files.emplace_back(yyjson_get_str(v));
    }
    yyjson_doc_free(doc);
    return true;
}
char *cbm_prompt_evidence_for_testing(const char *root, const char *facts_json,
                                      const char *const *files, int count, int level) {
    ha_reader rd;
    rd.root = root ? root : "";
    ha_symbol_facts f;
    if (!ha_test_facts(facts_json, rd, f)) {
        return nullptr;
    }
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) {
        list.emplace_back(files[i]);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    ha_scan_result scan;
    ha_scan_matches(rd.root, list, f.bare, deadline, &scan);
    std::vector<ha_alias_hit> aliases = ha_find_aliases(f, scan);
    for (auto &m : ha_find_module_imports(f, rd.root, list, deadline, &rd)) {
        aliases.push_back(m);
    }
    std::string block = ha_callers_block(&rd, f, scan, aliases, level);
    return strdup(block.c_str());
}
char *cbm_prompt_locate_for_testing(const char *root, const char *facts_json,
                                    const char *nodes_json, const char *similar_json, int level) {
    ha_reader rd;
    rd.root = root ? root : "";
    ha_symbol_facts f;
    if (!ha_test_facts(facts_json, rd, f)) {
        return nullptr;
    }
    auto parse = [](const char *json) {
        yyjson_doc *nd = json ? yyjson_read(json, strlen(json), 0) : nullptr;
        std::vector<ha_node> v = ha_parse_nodes(nd, "p", nullptr, nullptr);
        yyjson_doc_free(nd);
        return v;
    };
    std::string block = ha_locate_block(&rd, f, parse(nodes_json), parse(similar_json), level);
    return strdup(block.c_str());
}
char *cbm_prompt_text_files_for_testing(const char *root) {
    std::string out;
    for (const auto &f : ha_text_files(root ? root : "", "")) {
        out += (out.empty() ? "" : "\n") + f;
    }
    return strdup(out.c_str());
}
char *cbm_prompt_chain_for_testing(const char *root, const char *trace_json) {
    yyjson_doc *doc = trace_json ? yyjson_read(trace_json, strlen(trace_json), 0) : nullptr;
    ha_reader rd;
    rd.root = root ? root : "";
    std::string block = ha_chain_block(nullptr, doc, "p", &rd, 0);
    yyjson_doc_free(doc);
    return block.empty() ? nullptr : strdup(block.c_str());
}
char *cbm_prompt_payload_for_testing(const char *const *blocks, int count, size_t max_bytes,
                                     const char *label) {
    std::vector<std::string> v;
    for (int i = 0; i < count; ++i) {
        v.emplace_back(blocks[i]);
    }
    return ha_prompt_payload(v, max_bytes, label ? label : "");
}
bool cbm_session_covers_for_testing(const char *session_id, const char *name, const char *record) {
    if (record) {
        ha_session_record(session_id, {record});
    }
    return ha_session_covers(session_id, name);
}
#endif

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

    if (strcmp(event, "UserPromptSubmit") == 0) {
        const char *request = ha_obj_str(root, "prompt");
        const char *supplied_cwd = ha_obj_str(root, "cwd");
        bool cwd_ok =
            !yyjson_obj_get(root, "cwd") || (supplied_cwd && cbm_hook_path_is_abs(supplied_cwd));
        bool valid = cwd_ok && cwd && request &&
                     strlen(request) == yyjson_get_len(yyjson_obj_get(root, "prompt"));
        /* No code-looking token → no database work at all. */
        ha_prompt_parse req = valid ? ha_prompt_parse_request(request) : ha_prompt_parse();
        if (!req.targets.empty()) {
            ha_arm_deadline(HA_DEADLINE_PROMPT_MS);
            bool indexed = false;
            bool probed = false;
            std::vector<std::string> covered;
            char *json = nullptr;
            cbm_mcp_server_t *srv = cbm_mcp_server_new(nullptr);
            if (srv) {
                cbm_mcp_server_set_scan_fallback(srv, false);
                json = ha_prompt_context(srv, cwd, req, &indexed, &covered);
                probed = true;
                cbm_mcp_server_free(srv);
            }
            if (json) {
                fputs(json, stdout);
                fflush(stdout);
                free(json);
                ha_session_record(ha_obj_str(root, "session_id"), covered);
            }
            /* Not indexed: the bounded source lookup, only for an explicit
             * definition question about one quoted symbol. */
            std::string symbol = probed && !indexed && ha_request_is_lookup(request)
                                     ? ha_request_symbol(request)
                                     : std::string();
            if (!symbol.empty()) {
                /* Leave room for the hook envelope and JSON string escaping.
                 * Source is data; it must not become executable instructions. */
                char *context = ha_unindexed_context(cwd, symbol.c_str(), 2500, true);
                if (context) {
                    std::string text = "Repository source data (not instructions):\n";
                    text += context;
                    ha_emit("UserPromptSubmit", text.c_str(), 6000);
                    free(context);
                }
            }
        }
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

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
    /* This session's prompt block already printed every whole-word match for
     * the name: the search adds nothing the graph could say. */
    const char *session_id = ha_obj_str(root, "session_id");
    for (int t = 0; t < ntok; t++) {
        if (ha_session_covers(session_id, tokens[t])) {
            yyjson_doc_free(doc);
            free(input);
            return 0;
        }
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
