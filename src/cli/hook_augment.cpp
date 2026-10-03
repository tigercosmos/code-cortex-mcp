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
              "radius of your edits. Plain grep is fine for free text. When a [code-cortex] note "
              "marks a location \"(verified now)\", that path:line was re-read from disk when "
              "your prompt was submitted; a grep for the same name returns the same line. The "
              "project argument is optional inside this repository. Graph answers for partially "
              "parsed files are lower bounds (results say so).");
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
#define HA_PROMPT_MAX_BYTES 6000 /* Claude Code caps hook context at 10 000 chars */
#define HA_PROMPT_MAX_CALLER_FILES 40
#define HA_PROMPT_MAX_MENTION_FILES 20
#define HA_SCAN_MAX_FILE_BYTES (4 * 1024 * 1024)
#define HA_SCAN_BUDGET_MS 900 /* text scan, of the 1500 ms event deadline */
#define HA_PROMPT_MAX_PAIRS 3
#define HA_PROMPT_NEIGHBOURS 3
#define HA_PROMPT_SIG_MAX 200
#define HA_PROMPT_TRACE_DEPTH 6
#define HA_PROMPT_TRACE_WORK 5000
#define HA_DEADLINE_PROMPT_MS 1500
/* The label states exactly what was checked, so the model has nothing left
 * to verify with a grep of its own:
 *  - VERIFIED: every location printed was re-read from its file just now
 *    (ha_verify_loc) and still holds the symbol at that line.
 *  - FRESH/STALE: the fallback when a file could not be re-read (unreadable,
 *    over the read caps, or out of time). It reports inspect_symbol's
 *    file_modified_after_index, which compares the file's mtime with the
 *    project's indexed_at — an mtime check, not a content check. */
#define HA_PROMPT_LABEL_VERIFIED                                                              \
    "[code-cortex] graph facts for symbols in your request. Each location below was re-read " \
    "from the file just now and matches the index, so a grep for these names would return "   \
    "the same path:line."
#define HA_PROMPT_LABEL_FRESH                                                                 \
    "[code-cortex] graph facts for symbols in your request (from the index; source files of " \
    "these symbols unchanged since indexing):"
#define HA_PROMPT_LABEL_STALE                                                                \
    "[code-cortex] graph facts for symbols in your request (from the index; entries marked " \
    "stale have a file modified after indexing, re-check those):"
#define HA_VERIFY_MAX_FILES 16
#define HA_VERIFY_MAX_FILE_BYTES (2 * 1024 * 1024)
#define HA_VERIFY_BUDGET_MS 800 /* of the 1500 ms event deadline */

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
                          "without",   "don't",  "doesn't",   "isn't"});
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

static bool ha_callers_word(const std::string &w) {
    return ha_word_in(w, {"call",    "calls",     "caller",     "callers",    "called",
                          "calling", "used",      "usage",      "usages",     "uses",
                          "use",     "impact",    "impacted",   "affected",   "affect",
                          "affects", "signature", "change",     "changes",    "changing",
                          "edit",    "modify",    "references", "referenced", "referencing"});
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
    const int subject_para = out.targets.empty() ? 0 : out.targets.front().para;

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

/* Up to HA_PROMPT_NEIGHBOURS names from an inspect_symbol neighbour list. */
static void ha_prompt_names(std::string &line, yyjson_val *arr, int total) {
    size_t idx;
    size_t maxn;
    yyjson_val *v;
    int shown = 0;
    if (!arr || !yyjson_is_arr(arr)) {
        return;
    }
    yyjson_arr_foreach(arr, idx, maxn, v) {
        const char *name = ha_obj_str(v, "name");
        if (!name || !*name || shown >= HA_PROMPT_NEIGHBOURS) {
            continue;
        }
        line += shown ? ", " : ": ";
        line += std::string(name).substr(0, HA_MAX_TOKEN);
        shown++;
    }
    if (shown && total > shown) {
        line += ", +" + std::to_string(total - shown) + " more";
    }
}

/* ── Location re-check ──────────────────────────────────────────────
 * Re-read each printed location from disk and confirm the indexed line still
 * holds the symbol, so the context can say "verified now" instead of inviting
 * a confirmation grep. Bounded: at most HA_VERIFY_MAX_FILES files, each read
 * sequentially up to HA_VERIFY_MAX_FILE_BYTES, within HA_VERIFY_BUDGET_MS.
 * Anything not checked stays UNCHECKED and falls back to the mtime wording. */
enum { HA_LOC_UNCHECKED = -1, HA_LOC_MISMATCH = 0, HA_LOC_VERIFIED = 1 };

struct ha_file_lines {
    std::string rel;
    bool ok = false;
    bool truncated = false; /* the byte cap cut the file: lines past it are unknown */
    std::vector<std::string> lines;
};

struct ha_verifier {
    std::string root; /* empty: verification off */
    std::chrono::steady_clock::time_point deadline;
    std::vector<ha_file_lines> files;
    int verified = 0;
    int mismatched = 0;
    int unchecked = 0;
    bool mtime_stale = false; /* an unchecked location whose file changed by mtime */
    std::vector<std::string> recheck;
};

static const ha_file_lines *ha_verify_load(ha_verifier *v, const std::string &rel) {
    for (const auto &f : v->files) {
        if (f.rel == rel) {
            return &f;
        }
    }
    if ((int)v->files.size() >= HA_VERIFY_MAX_FILES ||
        std::chrono::steady_clock::now() >= v->deadline) {
        return nullptr;
    }
    ha_file_lines fl;
    fl.rel = rel;
    /* Repository-relative paths only: never follow ".." or an absolute path
     * out of the project root. */
    bool safe = !rel.empty() && rel[0] != '/' && rel.find('\\') == std::string::npos &&
                rel != ".." && rel.rfind("../", 0) != 0 && rel.find("/../") == std::string::npos;
    FILE *fp = safe ? fopen((v->root + "/" + rel).c_str(), "rb") : nullptr;
    if (fp) {
        std::string data;
        data.resize(HA_VERIFY_MAX_FILE_BYTES + 1);
        size_t n = fread(&data[0], 1, data.size(), fp);
        fl.ok = !ferror(fp);
        fclose(fp);
        fl.truncated = n > HA_VERIFY_MAX_FILE_BYTES;
        data.resize(fl.truncated ? HA_VERIFY_MAX_FILE_BYTES : n);
        size_t start = 0;
        while (fl.ok && start <= data.size()) {
            size_t eol = data.find('\n', start);
            if (eol == std::string::npos) {
                if (start < data.size() && !fl.truncated) {
                    fl.lines.push_back(data.substr(start));
                }
                break;
            }
            size_t len = eol - start;
            if (len && data[eol - 1] == '\r') {
                len--;
            }
            fl.lines.push_back(data.substr(start, len));
            start = eol + 1;
        }
    }
    v->files.push_back(std::move(fl));
    return &v->files.back();
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

/* The identifier a location's line must contain: the last segment of a
 * qualified name ("ns::Widget.render" -> "render"). */
static std::string ha_short_name(const std::string &name) {
    size_t cut = name.find_last_of(".:");
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

/* Check one location: the symbol's name must be on the indexed line or one
 * of the next two (a signature can start with an export keyword, a return
 * type or an annotation on the line the index records, and continue below).
 * *line_out gets the indexed line. `expected` is unused: inspect_symbol reads
 * its first source line from the file at query time, so comparing the two
 * could only ever fail on whitespace or clipping — the false "stale" marks
 * this replaced. Updates the verifier's tallies. */
static int ha_verify_loc(ha_verifier *v, const char *rel, int line, const std::string &name,
                         const std::string *expected = nullptr, std::string *line_out = nullptr) {
    int state = HA_LOC_UNCHECKED;
    if (v && !v->root.empty() && rel && *rel && line > 0 && !name.empty()) {
        const ha_file_lines *fl = ha_verify_load(v, rel);
        if (fl && fl->ok) {
            if ((size_t)line > fl->lines.size()) {
                state = fl->truncated ? HA_LOC_UNCHECKED : HA_LOC_MISMATCH;
            } else {
                (void)expected;
                const std::string &text = fl->lines[(size_t)line - 1];
                if (line_out) {
                    *line_out = text;
                }
                state = HA_LOC_MISMATCH;
                for (size_t k = (size_t)line - 1; k < fl->lines.size() && k < (size_t)line + 2;
                     ++k) {
                    if (ha_line_has_ident(fl->lines[k], ha_short_name(name))) {
                        state = HA_LOC_VERIFIED;
                        break;
                    }
                }
            }
        }
    }
    if (v) {
        (state == HA_LOC_VERIFIED   ? v->verified
         : state == HA_LOC_MISMATCH ? v->mismatched
                                    : v->unchecked)++;
    }
    return state;
}

static const char *ha_loc_suffix(int state) {
    return state == HA_LOC_VERIFIED   ? " (verified now)"
           : state == HA_LOC_MISMATCH ? " (stale: line changed since indexing, re-check)"
                                      : "";
}

/* The label for what the verifier saw (see HA_PROMPT_LABEL_VERIFIED). */
static std::string ha_prompt_label(const ha_verifier &v) {
    if (v.mismatched > 0 || v.mtime_stale) {
        std::string names;
        for (const auto &n : v.recheck) {
            if (names.find(n) == std::string::npos) {
                names += (names.empty() ? "" : ", ") + n;
            }
        }
        std::string label = "[code-cortex] graph facts for symbols in your request (from the "
                            "index). Re-check before relying on: " +
                            names + " (marked stale).";
        if (v.verified > 0) {
            label += " Locations marked (verified now) were re-read from the file just now and "
                     "match the index.";
        }
        return label;
    }
    if (v.verified > 0 && v.unchecked == 0) {
        return HA_PROMPT_LABEL_VERIFIED;
    }
    std::string label = HA_PROMPT_LABEL_FRESH;
    if (v.verified > 0) {
        label += " Locations marked (verified now) were also re-read from the file just now.";
    }
    return label;
}

/* ── Resolution ──────────────────────────────────────────────────────
 * The hook picks the node itself instead of letting a bare-name lookup guess:
 * qualifier first (the owner must appear among the node's qualified-name
 * segments), then a file the prompt mentions, then definition over
 * declaration. Anything still ambiguous is listed, never traced. */
struct ha_node {
    std::string qn, name, label, file;
    int start_line = 0;
    int end_line = 0;
};

static bool ha_label_is_symbol(const std::string &l) {
    return ha_word_in(l, {"Function", "Method", "Class", "Interface", "Struct", "Enum", "Type",
                          "Trait", "Declaration", "Macro", "Constructor", "Protocol", "Union",
                          "TypeAlias", "Object", "Module", "Record"}) &&
           l != "Module";
}

static bool ha_label_is_callable(const std::string &l) {
    return l == "Function" || l == "Method" || l == "Constructor" || l == "Macro";
}

/* Qualified-name segments, split on '.' and '::'. */
static std::vector<std::string> ha_qn_segments(const std::string &qn) {
    return ha_qual_segments(qn);
}

/* How many trailing qualifier segments sit, in order, right before the
 * node's own name in its qualified name (0: the qualifier is not there). */
static int ha_qualifier_score(const ha_node &n, const std::string &qualifier) {
    std::vector<std::string> q = ha_qual_segments(qualifier);
    std::vector<std::string> s = ha_qn_segments(n.qn);
    if (q.empty() || s.size() < 2) {
        return 0;
    }
    s.pop_back(); /* the name itself */
    int best = 0;
    /* Immediate owner match counts most; a segment anywhere (Go package
     * directories) counts once. */
    for (size_t k = 1; k <= q.size() && k <= s.size(); ++k) {
        if (std::equal(q.end() - (long)k, q.end(), s.end() - (long)k)) {
            best = (int)k + 1;
        }
    }
    if (best == 0 && std::find(s.begin(), s.end(), q.back()) != s.end()) {
        best = 1;
    }
    return best;
}

static bool ha_path_matches(const std::string &file, const std::string &hint) {
    if (file == hint) {
        return true;
    }
    return file.size() > hint.size() &&
           file.compare(file.size() - hint.size(), hint.size(), hint) == 0 &&
           file[file.size() - hint.size() - 1] == '/';
}

/* Narrow the same-named nodes to the one the request means. Returns the
 * remaining set (one node: resolved). */
static std::vector<ha_node> ha_disambiguate(std::vector<ha_node> nodes, const ha_target &t,
                                            const std::vector<std::string> &prompt_paths) {
    auto keep_if = [&nodes](auto pred) {
        std::vector<ha_node> kept;
        for (const auto &n : nodes) {
            if (pred(n)) {
                kept.push_back(n);
            }
        }
        if (!kept.empty()) {
            nodes = kept;
        }
    };
    keep_if([](const ha_node &n) { return ha_label_is_symbol(n.label); });
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
    if (!t.qualifier.empty() && nodes.size() > 1) {
        int best = 0;
        for (const auto &n : nodes) {
            best = std::max(best, ha_qualifier_score(n, t.qualifier));
        }
        if (best > 0) {
            keep_if([&](const ha_node &n) { return ha_qualifier_score(n, t.qualifier) == best; });
        }
    }
    if (nodes.size() > 1 && !t.file_hints.empty()) {
        keep_if([&](const ha_node &n) {
            for (const auto &h : t.file_hints) {
                if (ha_path_matches(n.file, h)) {
                    return true;
                }
            }
            return false;
        });
    }
    if (nodes.size() > 1 && !prompt_paths.empty()) {
        keep_if([&](const ha_node &n) {
            for (const auto &h : prompt_paths) {
                if (ha_path_matches(n.file, h)) {
                    return true;
                }
            }
            return false;
        });
    }
    if (nodes.size() > 1) {
        keep_if([](const ha_node &n) { return n.label != "Declaration"; });
    }
    if (nodes.size() > 1) {
        keep_if([](const ha_node &n) { return ha_label_is_callable(n.label); });
    }
    return nodes;
}

/* "Signer.unsign" for "proj.django.core.signing.Signer.unsign": the qualified
 * name without the project and the file's module path. */
static std::string ha_display_name(const std::string &qn, const std::string &file,
                                   const std::string &project) {
    std::string rest = qn;
    if (!project.empty() && rest.compare(0, project.size() + 1, project + ".") == 0) {
        rest = rest.substr(project.size() + 1);
    }
    std::string module = file;
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
    return rest.empty() ? qn : rest;
}

static std::vector<ha_node> ha_parse_nodes(yyjson_doc *d, std::string *root) {
    std::vector<ha_node> out;
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (root && ha_obj_str(r, "root")) {
        *root = ha_obj_str(r, "root");
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
            out.push_back(n);
        }
    }
    return out;
}

/* ── Whole-word text scan ────────────────────────────────────────────
 * Which indexed files mention `name` as a whole word: what a grep would show
 * next to the graph's resolved callers. Parallel, bounded by a deadline;
 * *complete is false when the deadline stopped it. */
static std::vector<std::string> ha_scan_mentions(const std::string &root,
                                                 const std::vector<std::string> &files,
                                                 const std::string &name,
                                                 std::chrono::steady_clock::time_point deadline,
                                                 bool *complete) {
    std::atomic<size_t> next{0};
    std::atomic<bool> timed_out{false};
    std::mutex mu;
    std::vector<std::string> hits;
    auto worker = [&]() {
        std::string buf;
        std::vector<std::string> local;
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
            buf.resize(HA_SCAN_MAX_FILE_BYTES);
            size_t n = fread(&buf[0], 1, buf.size(), fp);
            fclose(fp);
            std::string_view hay(buf.data(), n);
            for (size_t pos = hay.find(name); pos != std::string_view::npos;
                 pos = hay.find(name, pos + 1)) {
                bool left = pos == 0 || !ha_ident_char((unsigned char)hay[pos - 1]);
                size_t end = pos + name.size();
                bool right = end >= hay.size() || !ha_ident_char((unsigned char)hay[end]);
                if (left && right) {
                    local.push_back(files[i]);
                    break;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        hits.insert(hits.end(), local.begin(), local.end());
    };
    unsigned hw = std::thread::hardware_concurrency();
    unsigned nthreads = hw == 0 ? 4 : std::min(hw, 16u);
    std::vector<std::thread> pool;
    for (unsigned k = 1; k < nthreads; ++k) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto &th : pool) {
        th.join();
    }
    *complete = !timed_out;
    std::sort(hits.begin(), hits.end());
    return hits;
}

/* One compact block for an inspect_symbol result; empty when it says nothing
 * useful (not found, metadata omitted). *trace_name gets the name to trace
 * from/to when the symbol is a function or method, else stays empty. */
static std::string ha_prompt_symbol_block(yyjson_doc *d, const std::string &token,
                                          std::string *trace_name, bool *stale = nullptr,
                                          ha_verifier *v = nullptr) {
    if (trace_name) {
        trace_name->clear();
    }
    if (stale) {
        *stale = false;
    }
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (!r || !yyjson_is_obj(r) || yyjson_obj_get(r, "error")) {
        return {};
    }
    char buf[640];
    const char *status = ha_obj_str(r, "status");
    if (status && strcmp(status, "ambiguous") == 0) {
        yyjson_val *sugg = yyjson_obj_get(r, "suggestions");
        size_t n = (sugg && yyjson_is_arr(sugg)) ? yyjson_arr_size(sugg) : 0;
        if (n == 0) {
            return {};
        }
        std::string block = "- " + token + ": " + std::to_string(n) + " matches";
        size_t idx;
        size_t maxn;
        yyjson_val *s;
        size_t shown = 0;
        yyjson_arr_foreach(sugg, idx, maxn, s) {
            if (shown >= HA_PROMPT_NEIGHBOURS) {
                break;
            }
            const char *qn = ha_obj_str(s, "qualified_name");
            const char *label = ha_obj_str(s, "label");
            const char *file = ha_obj_str(s, "file_path");
            snprintf(buf, sizeof(buf), "%s %.160s (%.32s, %.200s)", shown ? "," : ":",
                     qn ? qn : "?", label ? label : "?", file ? file : "?");
            block += buf;
            shown++;
        }
        if (n > shown) {
            block += ", +" + std::to_string(n - shown) + " more";
        }
        return block;
    }
    yyjson_val *sym = yyjson_obj_get(r, "symbol");
    const char *file = ha_obj_str(sym, "file");
    if (!sym || !file || !*file || yyjson_is_true(yyjson_obj_get(r, "metadata_omitted"))) {
        return {};
    }
    const char *label = ha_obj_str(sym, "label");
    bool declaration = label && strcmp(label, "Declaration") == 0;
    const char *sym_name = ha_obj_str(sym, "name");
    std::string name = sym_name && *sym_name ? sym_name : token;

    /* First source line as inspect_symbol returned it (the defining line). */
    std::string first;
    const char *source = ha_obj_str(r, "source");
    if (source) {
        const char *eol = strchr(source, '\n');
        first.assign(source, eol ? (size_t)(eol - source) : strlen(source));
    }
    int unchecked_before = v ? v->unchecked : 0;
    std::string read_line;
    int loc = ha_verify_loc(v, file, ha_obj_int(sym, "start_line"), name, &first, &read_line);
    bool needs_recheck = loc == HA_LOC_MISMATCH;

    snprintf(buf, sizeof(buf), "- %s: %.32s at %.300s:%d-%d", token.c_str(),
             label && *label ? label : "symbol", file, ha_obj_int(sym, "start_line"),
             ha_obj_int(sym, "end_line"));
    std::string block = buf;
    block += ha_loc_suffix(loc);
    if (declaration) {
        block += " (declaration only; no definition indexed)";
    }
    yyjson_val *decl = yyjson_obj_get(r, "declared_in");
    size_t ndecl = (decl && yyjson_is_arr(decl)) ? yyjson_arr_size(decl) : 0;
    if (ndecl > 0) {
        yyjson_val *d0 = yyjson_arr_get(decl, 0);
        const char *df = ha_obj_str(d0, "file");
        int total = ha_obj_int(r, "declared_in_total");
        snprintf(buf, sizeof(buf), "; definition, declared at %.300s:%d", df ? df : "?",
                 ha_obj_int(d0, "line"));
        block += buf;
        int dloc = ha_verify_loc(v, df, ha_obj_int(d0, "line"), name);
        needs_recheck |= dloc == HA_LOC_MISMATCH;
        block += ha_loc_suffix(dloc);
        block += total > 1 ? " (+more)" : "";
    }
    yyjson_val *also = yyjson_obj_get(r, "also_defined_as");
    if (also && yyjson_is_arr(also) && yyjson_arr_size(also) > 0) {
        yyjson_val *a0 = yyjson_arr_get(also, 0);
        const char *al = ha_obj_str(a0, "label");
        const char *af = ha_obj_str(a0, "file");
        snprintf(buf, sizeof(buf), "; also %.32s at %.300s:%d", al ? al : "symbol", af ? af : "?",
                 ha_obj_int(a0, "start_line"));
        block += buf;
        const char *aqn = ha_obj_str(a0, "qualified_name");
        int aloc = ha_verify_loc(v, af, ha_obj_int(a0, "start_line"), aqn ? aqn : name);
        needs_recheck |= aloc == HA_LOC_MISMATCH;
        block += ha_loc_suffix(aloc);
        block += yyjson_arr_size(also) > 1 ? " (+more)" : "";
    }
    bool modified =
        yyjson_is_true(yyjson_obj_get(yyjson_obj_get(r, "index"), "file_modified_after_index"));
    bool any_unchecked = v ? v->unchecked > unchecked_before : true;
    if (modified && any_unchecked && !needs_recheck) {
        /* Not re-read: only the mtime says the file changed. */
        block += "; stale: file modified after indexing, re-check";
        needs_recheck = true;
        if (v) {
            v->mtime_stale = true;
        }
    } else if (modified && loc == HA_LOC_VERIFIED) {
        block += "; file edited after indexing (location still matches; callers may lag)";
    }
    if (needs_recheck) {
        if (stale) {
            *stale = true;
        }
        if (v) {
            v->recheck.push_back(token);
        }
    }

    /* The defining line: as just re-read when it was, else as inspect_symbol
     * returned it, else the stored signature. */
    std::string sig = loc != HA_LOC_UNCHECKED && !read_line.empty() ? read_line : first;
    if (sig.empty()) {
        if (const char *s = ha_obj_str(sym, "signature")) {
            sig = s;
        }
    }
    size_t a = sig.find_first_not_of(" \t\r");
    size_t b = sig.find_last_not_of(" \t\r");
    sig = a == std::string::npos ? std::string() : sig.substr(a, b - a + 1);
    if (sig.size() > HA_PROMPT_SIG_MAX) {
        sig.resize(HA_PROMPT_SIG_MAX);
        sig += "...";
    }
    if (!sig.empty()) {
        /* A failed re-check shows what the indexed line holds now, so the
         * model does not take it for the definition. */
        block +=
            loc == HA_LOC_MISMATCH
                ? "\n  line " + std::to_string(ha_obj_int(sym, "start_line")) + " now reads: " + sig
                : "\n  " + sig;
    }

    int callers = ha_obj_int(r, "callers_total");
    int tests = ha_obj_int(r, "related_tests_total");
    int callees = ha_obj_int(r, "callees_total");
    std::string rel = "\n  callers " + std::to_string(callers);
    if (tests > 0) {
        rel += " (+" + std::to_string(tests) + " in tests)";
    }
    ha_prompt_names(rel, yyjson_obj_get(r, "callers"), callers);
    rel += "; callees " + std::to_string(callees);
    ha_prompt_names(rel, yyjson_obj_get(r, "callees"), callees);
    block += rel;

    if (trace_name && label && (strcmp(label, "Function") == 0 || strcmp(label, "Method") == 0)) {
        const char *qn = ha_obj_str(sym, "qualified_name");
        *trace_name = qn && *qn ? qn : token;
    }
    return block;
}

/* "a -> b -> c" with each hop's file:line, from a trace_path result. */
static std::string ha_prompt_chain_line(yyjson_doc *d, const std::string &from,
                                        const std::string &to, ha_verifier *v = nullptr,
                                        const std::string &project = std::string()) {
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (!r || !yyjson_is_true(yyjson_obj_get(r, "path_found"))) {
        return {};
    }
    yyjson_val *path = yyjson_obj_get(r, "path");
    size_t n = (path && yyjson_is_arr(path)) ? yyjson_arr_size(path) : 0;
    if (n < 2) {
        return {};
    }
    std::string line = "- call chain " + from + " -> " + to + " (" + std::to_string(n - 1) +
                       " hop" + (n - 1 == 1 ? "" : "s") + "): ";
    size_t idx;
    size_t maxn;
    yyjson_val *hop;
    char buf[512];
    size_t verified = 0;
    yyjson_arr_foreach(path, idx, maxn, hop) {
        const char *name = ha_obj_str(hop, "name");
        const char *file = ha_obj_str(hop, "file");
        const char *hqn = ha_obj_str(hop, "qualified_name");
        std::string shown = hqn && *hqn && !project.empty()
                                ? ha_display_name(hqn, file ? file : "", project)
                                : std::string(name ? name : "?");
        snprintf(buf, sizeof(buf), "%s%.160s (%.300s:%d)", idx ? " -> " : "", shown.c_str(),
                 file ? file : "?", ha_obj_int(hop, "start_line"));
        line += buf;
        int state = ha_verify_loc(v, file, ha_obj_int(hop, "start_line"), name ? name : "");
        if (state == HA_LOC_MISMATCH) {
            line += " [stale: line changed since indexing, re-check]";
            if (v && name) {
                v->recheck.push_back(name);
            }
        }
        verified += state == HA_LOC_VERIFIED;
    }
    if (verified == n) {
        line += " (verified now)";
    }
    return line;
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
                                     const std::string &token, bool callers_mode, bool *is_error) {
    yyjson_mut_doc *ad = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(ad);
    yyjson_mut_doc_set_root(ad, args);
    yyjson_mut_obj_add_str(ad, args, "project", project);
    yyjson_mut_obj_add_strncpy(ad, args, "symbol", token.data(), token.size());
    yyjson_mut_obj_add_int(ad, args, "source_lines", 1);
    yyjson_mut_obj_add_int(ad, args, "callers_limit", HA_PROMPT_NEIGHBOURS);
    yyjson_mut_obj_add_int(ad, args, "callees_limit", HA_PROMPT_NEIGHBOURS);
    /* A callers request needs the complete caller-file rollup, so the reply
     * must not be shrunk to fit a small budget. */
    yyjson_mut_obj_add_int(ad, args, "max_bytes", callers_mode ? 400000 : 8000);
    char *encoded = yyjson_mut_write(ad, 0, nullptr);
    yyjson_mut_doc_free(ad);
    *is_error = false;
    yyjson_doc *d = encoded ? ha_call(srv, "inspect_symbol", encoded, is_error) : nullptr;
    free(encoded);
    return d;
}

static std::string ha_prompt_trace(cbm_mcp_server_t *srv, const char *project,
                                   const std::string &from, const std::string &to,
                                   const std::string &from_label, const std::string &to_label,
                                   ha_verifier *v) {
    yyjson_mut_doc *ad = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *args = yyjson_mut_obj(ad);
    yyjson_mut_doc_set_root(ad, args);
    yyjson_mut_obj_add_str(ad, args, "project", project);
    yyjson_mut_obj_add_strncpy(ad, args, "function_name", to.data(), to.size());
    yyjson_mut_obj_add_strncpy(ad, args, "from_function", from.data(), from.size());
    yyjson_mut_obj_add_str(ad, args, "direction", "inbound");
    yyjson_mut_obj_add_int(ad, args, "depth", HA_PROMPT_TRACE_DEPTH);
    yyjson_mut_obj_add_int(ad, args, "max_work", HA_PROMPT_TRACE_WORK);
    yyjson_mut_obj_add_int(ad, args, "max_bytes", 8000);
    char *encoded = yyjson_mut_write(ad, 0, nullptr);
    yyjson_mut_doc_free(ad);
    bool error = false;
    yyjson_doc *d = encoded ? ha_call(srv, "trace_path", encoded, &error) : nullptr;
    free(encoded);
    /* Tally the hops only for the orientation that is kept: a failed trace
     * prints nothing, so it verifies nothing. */
    ha_verifier scratch = *v;
    std::string line =
        error ? std::string() : ha_prompt_chain_line(d, from_label, to_label, &scratch, project);
    if (!line.empty()) {
        *v = std::move(scratch);
    }
    yyjson_doc_free(d);
    return line;
}

/* Files a caller rollup lists, re-read: does each still contain `name` as a
 * whole word? Reads at most the first HA_SCAN_MAX_FILE_BYTES of each. */
static bool ha_file_mentions(const std::string &root, const std::string &rel,
                             const std::string &name) {
    FILE *fp = fopen((root + "/" + rel).c_str(), "rb");
    if (!fp) {
        return false;
    }
    std::string buf;
    buf.resize(HA_SCAN_MAX_FILE_BYTES);
    size_t n = fread(&buf[0], 1, buf.size(), fp);
    fclose(fp);
    buf.resize(n);
    return ha_line_has_ident(buf, name);
}

/* For a callers/usages/impact request: the complete caller-file rollup from
 * the graph (each file re-checked on disk), the declaring headers, and the
 * files a whole-word grep would add that the graph does not resolve as calls.
 * Enough to answer "which files call X" without a grep, and to show where the
 * graph and the text disagree. */
static std::string ha_prompt_callers_block(yyjson_doc *d, const std::string &bare,
                                           const std::string &root,
                                           const std::vector<std::string> &mentions,
                                           bool scan_complete, bool scanned) {
    yyjson_val *r = d ? yyjson_doc_get_root(d) : nullptr;
    if (!r) {
        return {};
    }
    std::string out;
    std::vector<std::string> listed;
    yyjson_val *sym = yyjson_obj_get(r, "symbol");
    if (const char *f = ha_obj_str(sym, "file")) {
        listed.push_back(f);
    }
    /* Declarations. */
    std::string decls;
    size_t idx;
    size_t maxn;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(r, "declared_in"), idx, maxn, v) {
        const char *f = ha_obj_str(v, "file");
        if (!f || std::find(listed.begin(), listed.end(), f) != listed.end()) {
            continue;
        }
        listed.push_back(f);
        decls += (decls.empty() ? "" : ", ") + std::string(f) + ":" +
                 std::to_string(ha_obj_int(v, "line"));
    }
    if (!decls.empty()) {
        out += "\n  declared in: " + decls;
    }
    /* Caller files: the whole rollup, production and test. */
    yyjson_val *files = yyjson_obj_get(r, "caller_files");
    int total = ha_obj_int(r, "caller_files_total");
    std::string list;
    std::string missing;
    int considered = 0;
    int tests = 0;
    yyjson_arr_foreach(files, idx, maxn, v) {
        const char *f = ha_obj_str(v, "file");
        if (!f) {
            continue;
        }
        bool test = yyjson_is_true(yyjson_obj_get(v, "test"));
        tests += test;
        listed.push_back(f);
        if (considered >= HA_PROMPT_MAX_CALLER_FILES) {
            continue;
        }
        considered++;
        bool on_disk = scanned && scan_complete
                           ? std::binary_search(mentions.begin(), mentions.end(), std::string(f))
                           : ha_file_mentions(root, f, bare);
        if (!on_disk) {
            missing += (missing.empty() ? "" : ", ") + std::string(f);
            continue;
        }
        list += (list.empty() ? "" : ", ") + std::string(f) + (test ? " (test)" : "");
    }
    if (total < considered) {
        total = considered;
    }
    if (total == 0) {
        out += "\n  caller files: none resolved in the graph";
    } else {
        out += "\n  caller files (" + std::to_string(total) + ", " +
               (considered >= total
                    ? std::string("complete as indexed")
                    : "showing " + std::to_string(considered) + " of " + std::to_string(total)) +
               (tests ? "; " + std::to_string(tests) + " tests" : std::string()) +
               "; each re-read and contains `" + bare + "`): " + list;
    }
    if (!missing.empty()) {
        out += "\n  graph lists as callers but `" + bare +
               "` is no longer in the file (stale, re-check): " + missing;
    }
    /* What a grep would add. */
    if (scanned) {
        std::vector<std::string> extra;
        for (const auto &m : mentions) {
            if (std::find(listed.begin(), listed.end(), m) == listed.end()) {
                extra.push_back(m);
            }
        }
        if (extra.empty()) {
            out += "\n  also mention the name (not resolved as calls): none" +
                   std::string(scan_complete ? "" : " found before the time budget");
        } else {
            out += "\n  also mention the name (not resolved as calls) (" +
                   std::to_string(extra.size()) + "): ";
            size_t k = 0;
            for (; k < extra.size() && k < HA_PROMPT_MAX_MENTION_FILES; ++k) {
                out += (k ? ", " : "") + extra[k];
            }
            if (k < extra.size()) {
                out += ", +" + std::to_string(extra.size() - k) + " more";
            }
        }
        if (!scan_complete) {
            out += "\n  (the text scan stopped at its time budget; the list above may be "
                   "incomplete)";
        }
    }
    return out;
}

/* Resolve the targets against the first indexed project at or above cwd.
 * *indexed reports whether such a project exists (so the caller can choose
 * the unindexed fallback). Returns the hook JSON or NULL. */
static char *ha_prompt_context(cbm_mcp_server_t *srv, const char *cwd, const ha_prompt_parse &req,
                               bool *indexed) {
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

    std::string root;
    ha_verifier v;
    v.deadline = start + std::chrono::milliseconds(HA_VERIFY_BUDGET_MS);
    const bool callers_mode = req.callers_intent && !req.chain_intent;
    std::vector<std::string> symbols;
    struct fn_t {
        std::string qn, display;
    };
    std::vector<fn_t> functions;
    std::vector<std::string> files;
    bool files_loaded = false;
    for (size_t i = 0; i < req.targets.size(); ++i) {
        const ha_target &t = req.targets[i];
        char *json =
            i == 0 ? first : cbm_mcp_symbol_nodes(srv, proj.c_str(), t.bare.c_str(), nullptr);
        yyjson_doc *nd = json ? yyjson_read(json, strlen(json), 0) : nullptr;
        free(json);
        std::vector<ha_node> nodes = ha_parse_nodes(nd, root.empty() ? &root : nullptr);
        yyjson_doc_free(nd);
        if (v.root.empty()) {
            v.root = root.empty() ? std::string(dir) : root;
        }
        nodes = ha_disambiguate(nodes, t, req.paths);
        if (nodes.empty()) {
            continue;
        }
        if (nodes.size() > 1) {
            /* Still ambiguous: list where they are, guess nothing. */
            std::string block = "- " + t.name + ": " + std::to_string(nodes.size()) +
                                " matches, none chosen (name the class or file to pick one):";
            for (size_t k = 0; k < nodes.size() && k < 4; ++k) {
                int st = ha_verify_loc(&v, nodes[k].file.c_str(), nodes[k].start_line, t.bare);
                block += (k ? "; " : " ") + ha_display_name(nodes[k].qn, nodes[k].file, proj) +
                         " (" + nodes[k].label + ", " + nodes[k].file + ":" +
                         std::to_string(nodes[k].start_line) + ")" + ha_loc_suffix(st);
            }
            if (nodes.size() > 4) {
                block += "; +" + std::to_string(nodes.size() - 4) + " more";
            }
            symbols.push_back(block);
            continue;
        }
        const ha_node &n = nodes.front();
        std::string display = ha_display_name(n.qn, n.file, proj);
        bool error = false;
        yyjson_doc *d = ha_prompt_inspect(srv, proj.c_str(), n.qn, callers_mode, &error);
        std::string trace_name;
        std::string block =
            error ? std::string() : ha_prompt_symbol_block(d, display, &trace_name, nullptr, &v);
        if (!block.empty() && callers_mode) {
            bool complete = false;
            std::vector<std::string> mentions;
            if (!files_loaded) {
                char *froot = nullptr;
                char **flist = nullptr;
                int fc = cbm_mcp_project_files(srv, proj.c_str(), &froot, &flist);
                for (int k = 0; k < fc; ++k) {
                    files.emplace_back(flist[k]);
                }
                if (froot && *froot && v.root.empty()) {
                    v.root = froot;
                }
                cbm_mcp_free_project_files(froot, flist, fc < 0 ? 0 : fc);
                files_loaded = true;
            }
            bool scanned = !files.empty();
            if (scanned) {
                mentions = ha_scan_mentions(v.root, files, t.bare,
                                            start + std::chrono::milliseconds(HA_SCAN_BUDGET_MS),
                                            &complete);
            }
            block += ha_prompt_callers_block(d, t.bare, v.root, mentions, complete, scanned);
        }
        yyjson_doc_free(d);
        if (!block.empty()) {
            symbols.push_back(block);
        }
        if (!trace_name.empty()) {
            functions.push_back({trace_name, display});
        }
    }

    std::vector<std::string> blocks;
    int pairs = 0;
    for (size_t i = 0; i < functions.size() && pairs < HA_PROMPT_MAX_PAIRS && !callers_mode; ++i) {
        for (size_t j = i + 1; j < functions.size() && pairs < HA_PROMPT_MAX_PAIRS; ++j) {
            pairs++;
            const auto &a = functions[i];
            const auto &b = functions[j];
            std::string line =
                ha_prompt_trace(srv, proj.c_str(), a.qn, b.qn, a.display, b.display, &v);
            if (line.empty()) {
                line = ha_prompt_trace(srv, proj.c_str(), b.qn, a.qn, b.display, a.display, &v);
            }
            if (!line.empty()) {
                blocks.push_back(line);
            }
        }
    }
    blocks.insert(blocks.end(), symbols.begin(), symbols.end());
    return blocks.empty() ? nullptr
                          : ha_prompt_payload(blocks, HA_PROMPT_MAX_BYTES, ha_prompt_label(v));
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
    std::vector<ha_node> all = ha_parse_nodes(nd, nullptr);
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
char *cbm_prompt_callers_block_for_testing(const char *inspect_json, const char *bare,
                                           const char *root, const char *const *files, int count) {
    yyjson_doc *doc = inspect_json ? yyjson_read(inspect_json, strlen(inspect_json), 0) : nullptr;
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i) {
        list.emplace_back(files[i]);
    }
    bool complete = false;
    std::vector<std::string> mentions = ha_scan_mentions(
        root, list, bare, std::chrono::steady_clock::now() + std::chrono::seconds(5), &complete);
    std::string block = ha_prompt_callers_block(doc, bare, root, mentions, complete, true);
    yyjson_doc_free(doc);
    return block.empty() ? nullptr : strdup(block.c_str());
}
static ha_verifier ha_test_verifier(const char *root) {
    ha_verifier v;
    v.root = root ? root : "";
    v.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(HA_VERIFY_BUDGET_MS);
    return v;
}
char *cbm_prompt_symbol_block_for_testing(const char *inspect_json, const char *token,
                                          char **trace_name, bool *stale, const char *root,
                                          char **label) {
    yyjson_doc *doc = inspect_json ? yyjson_read(inspect_json, strlen(inspect_json), 0) : nullptr;
    std::string name;
    ha_verifier v = ha_test_verifier(root);
    std::string block = ha_prompt_symbol_block(doc, token ? token : "", &name, stale, &v);
    if (label) {
        *label = strdup(ha_prompt_label(v).c_str());
    }
    yyjson_doc_free(doc);
    if (trace_name) {
        *trace_name = name.empty() ? nullptr : strdup(name.c_str());
    }
    return block.empty() ? nullptr : strdup(block.c_str());
}
char *cbm_prompt_chain_line_for_testing(const char *trace_json, const char *from, const char *to,
                                        const char *root, char **label) {
    yyjson_doc *doc = trace_json ? yyjson_read(trace_json, strlen(trace_json), 0) : nullptr;
    ha_verifier v = ha_test_verifier(root);
    std::string line = ha_prompt_chain_line(doc, from, to, &v);
    if (label) {
        *label = strdup(ha_prompt_label(v).c_str());
    }
    yyjson_doc_free(doc);
    return line.empty() ? nullptr : strdup(line.c_str());
}
char *cbm_prompt_payload_for_testing(const char *const *blocks, int count, size_t max_bytes,
                                     bool any_stale) {
    std::vector<std::string> v;
    for (int i = 0; i < count; ++i) {
        v.emplace_back(blocks[i]);
    }
    return ha_prompt_payload(v, max_bytes,
                             any_stale ? HA_PROMPT_LABEL_STALE : HA_PROMPT_LABEL_FRESH);
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
            char *json = nullptr;
            cbm_mcp_server_t *srv = cbm_mcp_server_new(nullptr);
            if (srv) {
                cbm_mcp_server_set_scan_fallback(srv, false);
                json = ha_prompt_context(srv, cwd, req, &indexed);
                probed = true;
                cbm_mcp_server_free(srv);
            }
            if (json) {
                fputs(json, stdout);
                fflush(stdout);
                free(json);
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
