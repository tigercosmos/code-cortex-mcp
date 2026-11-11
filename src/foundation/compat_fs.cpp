/*
 * compat_fs.c — Portable file system operations.
 *
 * POSIX: direct wrappers around opendir/readdir/closedir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_fs_internal.h"
#include "foundation/git_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ── Windows implementation ───────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h> /* _wmkdir */
#include <errno.h>  /* errno for spawn-failure logging */
#include <fcntl.h>  /* _O_RDONLY */
#include <io.h>     /* _wunlink, _open_osfhandle, _close */
#include <stdint.h> /* intptr_t */
/* struct _stat64 and _wstat64 arrive transitively on MinGW, but the _S_IFMT /
 * _S_IFREG macros cbm_is_regular_file tests with do not — without this the
 * Windows build fails on "use of undeclared identifier '_S_IFMT'". */
#include <sys/stat.h>
#include "foundation/log.h"
#include "foundation/win_utf8.h"

struct cbm_dir {
    HANDLE find_handle;
    WIN32_FIND_DATAW find_data;
    wchar_t wide_pattern[CBM_PATH_MAX];
    cbm_dirent_t entry;
    bool first;
    bool done;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return NULL;
    }

    size_t wlen = wcslen(wpath);
    if (wlen == 0 || wlen + 2 >= CBM_PATH_MAX) {
        free(wpath);
        return NULL;
    }

    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        free(wpath);
        return NULL;
    }

    wmemcpy(d->wide_pattern, wpath, wlen + 1);
    wchar_t *p = d->wide_pattern + wlen - SKIP_ONE;
    if (*p != L'\\' && *p != L'/') {
        ++p;
        *p++ = L'\\';
    } else {
        ++p;
    }
    *p++ = L'*';
    *p = L'\0';
    free(wpath);

    d->find_handle = FindFirstFileW(d->wide_pattern, &d->find_data);
    if (d->find_handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = true;
    d->done = false;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || d->done) {
        return NULL;
    }
    if (!d->first) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }
    d->first = false;

    while (d->find_data.cFileName[0] == L'.' &&
           (d->find_data.cFileName[1] == L'\0' ||
            (d->find_data.cFileName[1] == L'.' && d->find_data.cFileName[2] == L'\0'))) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            return NULL;
        }
    }

    char *u8 = cbm_wide_to_utf8(d->find_data.cFileName);
    if (!u8) {
        d->done = true;
        return NULL;
    }
    size_t nlen = strlen(u8);
    if (nlen >= CBM_DIRENT_NAME_MAX) {
        nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
    }
    memcpy(d->entry.name, u8, nlen);
    d->entry.name[nlen] = '\0';
    free(u8);
    /* A junction or directory symlink carries FILE_ATTRIBUTE_DIRECTORY too;
     * it is a link, not a directory a walker may descend into. */
    DWORD attr = d->find_data.dwFileAttributes;
    d->entry.is_dir =
        (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    d->entry.d_type = 0;
    d->entry.is_link = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return &d->entry;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(d->find_handle);
        }
        free(d);
    }
}

/* Windows _popen replacement that inherits ONLY the child's stdout pipe.
 *
 * The CRT's _popen uses CreateProcess(bInheritHandles=TRUE), which leaks EVERY
 * inheritable handle we hold into the child — listening/client sockets, the
 * Winsock/AFD helper handles created by WSAStartup, the MCP stdio pipe, etc.
 * When the child is git-for-Windows (MSYS2/Cygwin runtime), its startup walks
 * every inherited handle and calls NtQueryObject on each to classify it; on an
 * inherited socket/AFD handle NtQueryObject deadlocks. Since our UI server runs
 * requests on a single thread, that wedges the whole server (list_projects,
 * which shells out to git per project, never returns → the web UI hangs).
 *
 * The fix: spawn via CreateProcessW with STARTUPINFOEXW + an explicit
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST containing only the stdout write-end and a
 * NUL handle for stdin/stderr. Nothing else crosses into git, so there is no
 * foreign handle to deadlock on. POSIX popen() already sets O_CLOEXEC on its
 * pipe, so the POSIX path is unchanged.
 *
 * There is deliberately NO fallback to _popen when the isolated spawn fails:
 * falling back would silently re-arm the deadlock. cbm_popen logs a structured
 * warning and returns NULL instead (every call site handles NULL). */

enum { CBM_POPEN_MAX = 16 };
static struct {
    FILE *fp;
    HANDLE proc;
} g_popen_tab[CBM_POPEN_MAX];
static CRITICAL_SECTION g_popen_lock;
static INIT_ONCE g_popen_once = INIT_ONCE_STATIC_INIT;

/* Test hook (declared in compat_fs_internal.h): 1 when the most recent
 * cbm_popen(..., "r") stream came from the isolated spawn. Test-only
 * observable; not synchronized across threads. */
static volatile LONG g_popen_last_isolated = 0;

int cbm_popen_last_was_isolated(void) {
    return (int)g_popen_last_isolated;
}

static BOOL CALLBACK cbm_popen_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_popen_lock);
    return TRUE;
}

/* Resolve the shell explicitly — %COMSPEC%, else <system dir>\cmd.exe — so it
 * can be passed as lpApplicationName and CreateProcess never walks the search
 * path (no cmd.exe planting from a hostile CWD). Heap string; caller frees. */
static wchar_t *cbm_resolve_comspec(void) {
    wchar_t buf[MAX_PATH];
    const wchar_t suffix[] = L"\\cmd.exe";
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        UINT sn = GetSystemDirectoryW(buf, MAX_PATH);
        if (sn == 0 || (size_t)sn + wcslen(suffix) >= MAX_PATH) {
            return NULL;
        }
        wmemcpy(buf + sn, suffix, wcslen(suffix) + 1);
    }
    return _wcsdup(buf);
}

/* On failure returns NULL with *stage naming the failing step and *gle the
 * GetLastError value captured at that step (0 when errno is the signal).
 * app/wcmdline: the image and its command line (NULL => "cmdline" failure);
 * env: a CREATE_UNICODE_ENVIRONMENT block for the child, or NULL to inherit. */
static FILE *cbm_popen_isolated_w(const wchar_t *app, wchar_t *wcmdline, const wchar_t *env,
                                  const char **stage, DWORD *gle) {
    *stage = "";
    *gle = 0;
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        *stage = "pipe";
        *gle = GetLastError();
        return NULL;
    }
    /* The parent read-end must never cross into the child. */
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* NUL for the child's stdin/stderr so it never touches our real stdin
     * pipe. If NUL cannot be opened, fail: STARTF_USESTDHANDLES slots must
     * never carry INVALID_HANDLE_VALUE. */
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        *stage = "nul";
        *gle = GetLastError();
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }

    HANDLE inherit[2];
    inherit[0] = wr;
    inherit[1] = nul;

    SIZE_T attr_sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
    LPPROC_THREAD_ATTRIBUTE_LIST attr = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_sz);
    BOOL attr_init = attr && InitializeProcThreadAttributeList(attr, 1, 0, &attr_sz);
    BOOL prepared =
        attr_init && UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               sizeof(inherit), NULL, NULL);
    DWORD attr_gle = prepared ? 0 : GetLastError();

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attr;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL created = FALSE;
    if (!prepared) {
        *stage = "attr";
        *gle = attr_gle;
    } else if (!app || !wcmdline) {
        *stage = "cmdline";
        *gle = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
        if (env) {
            flags |= CREATE_UNICODE_ENVIRONMENT;
        }
        created = CreateProcessW(app, wcmdline, NULL, NULL, TRUE, flags, (LPVOID)env, NULL,
                                 &si.StartupInfo, &pi);
        if (!created) {
            *stage = "spawn";
            *gle = GetLastError();
        }
    }

    if (attr) {
        if (attr_init) {
            DeleteProcThreadAttributeList(attr);
        }
        free(attr);
    }
    CloseHandle(wr); /* the child owns the write-end now */
    CloseHandle(nul);
    if (!created) {
        CloseHandle(rd);
        return NULL;
    }
    CloseHandle(pi.hThread);

    int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY);
    if (fd == -1) {
        *stage = "osfhandle";
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    FILE *fp = _fdopen(fd, "r"); /* takes ownership of fd/rd */
    if (!fp) {
        *stage = "fdopen";
        _close(fd);
        CloseHandle(pi.hProcess);
        return NULL;
    }

    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (!g_popen_tab[i].fp) {
            g_popen_tab[i].fp = fp;
            g_popen_tab[i].proc = pi.hProcess;
            LeaveCriticalSection(&g_popen_lock);
            return fp;
        }
    }
    LeaveCriticalSection(&g_popen_lock);
    /* Table full (shouldn't happen): don't leak the process handle. */
    *stage = "table";
    CloseHandle(pi.hProcess);
    fclose(fp);
    return NULL;
}

/* Run `cmd` through cmd.exe /c so command quoting and `2>NUL` behave as
 * under _popen. The command line is heap-composed (no fixed-size truncation)
 * and widened via UTF-8 so non-ASCII repo paths survive intact. */
static FILE *cbm_popen_isolated(const char *cmd, const wchar_t *env, const char **stage,
                                DWORD *gle) {
    wchar_t *app = cbm_resolve_comspec();
    wchar_t *wcmdline = NULL;
    if (app) {
        size_t u8len = strlen(cmd) + sizeof("cmd.exe /c ");
        char *u8 = (char *)malloc(u8len);
        if (u8) {
            snprintf(u8, u8len, "cmd.exe /c %s", cmd);
            wcmdline = cbm_utf8_to_wide(u8);
            free(u8);
        }
    }
    FILE *fp = cbm_popen_isolated_w(app, wcmdline, env, stage, gle);
    free(app);
    free(wcmdline);
    return fp;
}

static void cbm_popen_log_failure(const char *stage, DWORD gle) {
    char glebuf[CBM_SZ_16];
    char errnobuf[CBM_SZ_16];
    snprintf(glebuf, sizeof(glebuf), "%lu", (unsigned long)gle);
    snprintf(errnobuf, sizeof(errnobuf), "%d", errno);
    cbm_log_warn("compat.popen_isolated_failed", "stage", stage, "gle", glebuf, "errno", errnobuf);
}

static FILE *cbm_popen_read_isolated(const char *cmd, const wchar_t *env) {
    const char *stage = "";
    DWORD gle = 0;
    FILE *fp = cbm_popen_isolated(cmd, env, &stage, &gle);
    g_popen_last_isolated = (fp != NULL);
    if (!fp) {
        cbm_popen_log_failure(stage, gle);
    }
    return fp;
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    /* Our git shell-outs are all read-mode; they MUST use the isolated
     * spawn. On failure, log and fail the call — never fall back to
     * _popen, whose full handle inheritance re-arms the UI hang (#798). */
    if (mode && mode[0] == 'r' && mode[1] == '\0') {
        return cbm_popen_read_isolated(cmd, NULL);
    }
    g_popen_last_isolated = 0;
    return _popen(cmd, mode);
}

FILE *cbm_popen_git(const char *cmd) {
    if (!cmd) {
        return NULL;
    }
    /* Fail closed: without the scrubbed block the child would inherit a
     * caller's GIT_DIR and read the wrong repository (#2003). */
    wchar_t *env = cbm_git_child_env_block();
    if (!env) {
        cbm_log_warn("compat.popen_git_env_failed", "stage", "env_block");
        return NULL;
    }
    FILE *fp = cbm_popen_read_isolated(cmd, env);
    cbm_git_child_env_free(env); /* CreateProcessW copies the block into the child */
    return fp;
}

FILE *cbm_popen_git_argv(const char *const *args) {
    if (!args) {
        return NULL;
    }
    char git[CBM_SZ_4K];
    if (!cbm_find_git(git, sizeof(git))) {
        return NULL;
    }
    enum { GIT_ARGV_MAX = 64 };
    const char *argv[GIT_ARGV_MAX];
    size_t n = 0;
    argv[n++] = "git";
    for (const char *const *a = args; *a; a++) {
        if (n + SKIP_ONE >= GIT_ARGV_MAX) {
            return NULL;
        }
        argv[n++] = *a;
    }
    argv[n] = NULL;
    wchar_t *env = cbm_git_child_env_block();
    wchar_t *app = env ? cbm_utf8_to_wide(git) : NULL;
    /* No shell: the image is the resolved absolute git.exe and every argument
     * is quoted by the CRT convention, so a path never reaches cmd.exe. */
    wchar_t *wcmdline = app ? cbm_build_cmdline(argv) : NULL;
    const char *stage = "";
    DWORD gle = 0;
    FILE *fp = env ? cbm_popen_isolated_w(app, wcmdline, env, &stage, &gle) : NULL;
    if (!fp) {
        cbm_popen_log_failure(env ? stage : "env_block", gle);
    }
    free(wcmdline);
    free(app);
    cbm_git_child_env_free(env);
    return fp;
}

int cbm_pclose(FILE *f) {
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    HANDLE proc = NULL;
    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (g_popen_tab[i].fp == f) {
            proc = g_popen_tab[i].proc;
            g_popen_tab[i].fp = NULL;
            g_popen_tab[i].proc = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_popen_lock);

    if (!proc) {
        return _pclose(f); /* opened via _popen (non-read mode) */
    }
    fclose(f);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    BOOL got = GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return got ? (int)code : -1;
}

FILE *cbm_fopen(const char *path, const char *mode) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return NULL;
    }
    wchar_t *wmode = cbm_utf8_to_wide(mode);
    if (!wmode) {
        free(wpath);
        return NULL;
    }
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

bool cbm_is_regular_file(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return false;
    }
    struct _stat64 wst;
    int ret = _wstat64(wpath, &wst);
    free(wpath);
    return ret == 0 && (wst.st_mode & _S_IFMT) == _S_IFREG;
}

bool cbm_is_link(const char *path) {
    wchar_t *wpath = path ? cbm_utf8_to_wide_path(path) : NULL;
    if (!wpath) {
        return false;
    }
    /* GetFileAttributesW reports the reparse point itself, never its target. */
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

int cbm_stat(const char *path, cbm_stat_t *st) {
    wchar_t *wpath = path ? cbm_utf8_to_wide_path(path) : NULL;
    if (!wpath) {
        /* Not "no such file": the path was never looked at (no path, or the
         * wide copy could not be made). A caller that prunes on ENOENT (the
         * watcher, cache expiry) must read this as could-not-check. */
        errno = path ? ENOMEM : EINVAL;
        return CBM_NOT_FOUND;
    }
    int ret = _wstat64(wpath, st);
    free(wpath);
    return ret;
}

bool cbm_mkdir_p(const char *path, int mode) {
    (void)mode;
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return false;
    }

    if (_wmkdir(wpath) == 0) {
        free(wpath);
        return true;
    }
    size_t wlen = wcslen(wpath);
    wchar_t *tmp = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!tmp) {
        free(wpath);
        return false;
    }
    wmemcpy(tmp, wpath, wlen + 1);
    /* An extended-length path walks from its root: \\?\C:\ or the share of
     * \\?\UNC\server\share\. Its prefix components are not directories. */
    wchar_t *start = tmp + SKIP_ONE;
    if (cbm_wide_has_long_prefix(tmp)) {
        start = tmp + 4; /* past \\?\ */
        if (wcsncmp(start, L"UNC\\", 4) == 0) {
            start += 4;
            for (int seps = 0; *start && seps < 2; start++) {
                seps += *start == L'\\';
            }
        } else if (start[0] && start[1] == L':' && start[2] == L'\\') {
            start += 3;
        }
    }
    for (wchar_t *p = start; *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            *p = L'\0';
            _wmkdir(tmp);
            *p = L'\\';
        }
    }
    bool ok = _wmkdir(tmp) == 0 || GetLastError() == ERROR_ALREADY_EXISTS;
    free(tmp);
    free(wpath);
    return ok;
}

int cbm_unlink(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wunlink(wpath);
    free(wpath);
    return ret;
}

int cbm_rmdir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wrmdir(wpath);
    free(wpath);
    return ret;
}

/* Build a properly-quoted Windows command line from an argv array.
 * Returns a heap-allocated wide string, or NULL on allocation failure.
 * Quoting follows the MSVC CRT convention: arguments containing spaces,
 * tabs, or double-quotes are wrapped in double-quotes, with backslashes
 * before a closing quote doubled and the quote itself escaped. Argument
 * bytes are treated as UTF-8 and converted to wide via cbm_utf8_to_wide,
 * so non-ASCII arguments (e.g. a non-ASCII %USERPROFILE%) survive intact.
 * Declared in compat_fs_internal.h so the test suite can drive it. */
wchar_t *cbm_build_cmdline(const char *const *argv) {
    /* First pass: compute required buffer size. */
    size_t total = 1; /* NUL terminator */
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
            }
        }
        if (i > 0) {
            total++; /* space separator */
        }
        if (needs_quote) {
            total += 2; /* opening and closing quote */
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                } else if (*p == '"') {
                    total += backslashes + 1; /* double backslashes + escape backslash */
                    backslashes = 0;
                } else {
                    backslashes = 0;
                }
                total++;
            }
            /* Trailing backslashes before closing quote must be doubled. */
            total += backslashes;
        } else {
            total += strlen(arg);
        }
    }

    /* Build the quoted command line in UTF-8 first, then widen it as a
     * whole via cbm_utf8_to_wide. Every character the quoting logic acts
     * on (space, tab, '"', '\\') is ASCII and, by UTF-8's design, never
     * appears inside a multibyte sequence, so operating on raw bytes here
     * is safe and keeps multibyte argument bytes intact for conversion. */
    char *buf = (char *)malloc(total);
    if (!buf) {
        return NULL;
    }

    /* Second pass: write the command line bytes. */
    char *w = buf;
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
                break;
            }
        }
        if (i > 0) {
            *w++ = ' ';
        }
        if (needs_quote) {
            *w++ = '"';
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                    *w++ = '\\';
                } else if (*p == '"') {
                    /* Double the preceding backslashes, then escape the quote. */
                    for (size_t b = 0; b < backslashes; b++) {
                        *w++ = '\\';
                    }
                    *w++ = '\\';
                    *w++ = '"';
                    backslashes = 0;
                } else {
                    backslashes = 0;
                    *w++ = *p;
                }
            }
            /* Double trailing backslashes before the closing quote. */
            for (size_t b = 0; b < backslashes; b++) {
                *w++ = '\\';
            }
            *w++ = '"';
        } else {
            for (const char *p = arg; *p; p++) {
                *w++ = *p;
            }
        }
    }
    *w = '\0';

    wchar_t *out = cbm_utf8_to_wide(buf);
    free(buf);
    return out;
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }

    wchar_t *cmdline = cbm_build_cmdline(argv);
    if (!cmdline) {
        return CBM_NOT_FOUND;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        return CBM_NOT_FOUND;
    }
    free(cmdline);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = (DWORD)CBM_NOT_FOUND;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

#else /* POSIX */

/* ── POSIX implementation ─────────────────────────────────────── */

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct cbm_dir {
    DIR *dir;
    cbm_dirent_t entry;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return NULL;
    }
    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->dir = dir;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || !d->dir) {
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d->dir)) != NULL) {
        /* Skip "." and ".." */
        if (de->d_name[0] == '.' &&
            (de->d_name[SKIP_ONE] == '\0' ||
             (de->d_name[SKIP_ONE] == '.' && de->d_name[PAIR_LEN] == '\0'))) {
            continue;
        }
        size_t nlen = strlen(de->d_name);
        if (nlen >= CBM_DIRENT_NAME_MAX) {
            nlen = CBM_DIRENT_NAME_MAX - SKIP_ONE;
        }
        memcpy(d->entry.name, de->d_name, nlen);
        d->entry.name[nlen] = '\0';
        d->entry.d_type = de->d_type;
        if (de->d_type == DT_UNKNOWN) {
            /* NFS, FUSE and some XFS/ReiserFS mounts leave d_type unset;
             * ask the entry itself, without following a symlink, so the
             * answer matches what DT_DIR / DT_LNK would have said. */
            struct stat st;
            bool got = fstatat(dirfd(d->dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0;
            d->entry.is_dir = got && S_ISDIR(st.st_mode);
            d->entry.is_link = got && S_ISLNK(st.st_mode);
        } else {
            d->entry.is_dir = (de->d_type == DT_DIR);
            d->entry.is_link = (de->d_type == DT_LNK);
        }
        return &d->entry;
    }
    return NULL;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->dir) {
            closedir(d->dir);
        }
        free(d);
    }
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    return popen(cmd, mode);
}

/* Streams opened by cbm_popen_git / cbm_popen_git_argv: popen() cannot take an
 * environment, so those children are spawned here and reaped by cbm_pclose
 * through this list. */
typedef struct cbm_popen_git_entry {
    FILE *fp;
    pid_t pid;
    struct cbm_popen_git_entry *next;
} cbm_popen_git_entry_t;

static cbm_popen_git_entry_t *g_popen_git_list = NULL;
static pthread_mutex_t g_popen_git_lock = PTHREAD_MUTEX_INITIALIZER;

static bool cbm_fd_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

/* Spawn `path argv` with stdout on a pipe, stdin on /dev/null (never the MCP
 * transport), stderr inherited (or /dev/null when quiet_stderr), and `envp`
 * as the environment. Returns the pid and the parent's read end, or -1. */
static pid_t cbm_popen_spawn(const char *path, char *const argv[], char **envp, bool quiet_stderr,
                             int *read_fd) {
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    if (!cbm_fd_set_cloexec(fds[0]) || !cbm_fd_set_cloexec(fds[1])) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    /* dup2 clears close-on-exec on the child's stdout; both pipe ends
     * themselves stay close-on-exec, so no other child ever holds them. */
    bool configured =
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO) == 0 &&
        (!quiet_stderr ||
         posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0);
    pid_t pid = -1;
    int rc = configured ? posix_spawn(&pid, path, &actions, NULL, argv, envp) : -1;
    (void)posix_spawn_file_actions_destroy(&actions);
    (void)close(fds[1]);
    if (rc != 0 || pid <= 0) {
        (void)close(fds[0]);
        return -1;
    }
    *read_fd = fds[0];
    return pid;
}

static int cbm_popen_git_wait(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return status;
}

static FILE *cbm_popen_git_spawn(const char *path, char *const argv[], bool quiet_stderr) {
    /* Fail closed: without the scrubbed environment the child would inherit
     * a caller's GIT_DIR and read the wrong repository (#2003). */
    char **envp = cbm_git_child_envp();
    if (!envp) {
        return NULL;
    }
    int read_fd = -1;
    pid_t pid = cbm_popen_spawn(path, argv, envp, quiet_stderr, &read_fd);
    cbm_git_child_env_free(envp); /* posix_spawn copied it into the child */
    if (pid < 0) {
        return NULL;
    }
    cbm_popen_git_entry_t *entry = (cbm_popen_git_entry_t *)malloc(sizeof(*entry));
    FILE *fp = entry ? fdopen(read_fd, "r") : NULL;
    if (!fp) {
        free(entry);
        (void)close(read_fd);
        (void)cbm_popen_git_wait(pid);
        return NULL;
    }
    entry->fp = fp;
    entry->pid = pid;
    pthread_mutex_lock(&g_popen_git_lock);
    entry->next = g_popen_git_list;
    g_popen_git_list = entry;
    pthread_mutex_unlock(&g_popen_git_lock);
    return fp;
}

FILE *cbm_popen_git(const char *cmd) {
    if (!cmd) {
        return NULL;
    }
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    return cbm_popen_git_spawn("/bin/sh", argv, false);
}

FILE *cbm_popen_git_argv(const char *const *args) {
    if (!args) {
        return NULL;
    }
    char git[CBM_SZ_4K];
    if (!cbm_find_git(git, sizeof(git))) {
        return NULL;
    }
    enum { GIT_ARGV_MAX = 64 };
    char *argv[GIT_ARGV_MAX];
    size_t n = 0;
    argv[n++] = (char *)"git";
    for (const char *const *a = args; *a; a++) {
        if (n + SKIP_ONE >= GIT_ARGV_MAX) {
            return NULL;
        }
        argv[n++] = (char *)*a;
    }
    argv[n] = NULL;
    return cbm_popen_git_spawn(git, argv, true);
}

int cbm_pclose(FILE *f) {
    cbm_popen_git_entry_t *found = NULL;
    pthread_mutex_lock(&g_popen_git_lock);
    for (cbm_popen_git_entry_t **link = &g_popen_git_list; *link; link = &(*link)->next) {
        if ((*link)->fp == f) {
            found = *link;
            *link = found->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_popen_git_lock);
    if (!found) {
        return pclose(f); /* opened by popen() */
    }
    pid_t pid = found->pid;
    free(found);
    (void)fclose(f);
    return cbm_popen_git_wait(pid); /* the raw wait status, as pclose() */
}

FILE *cbm_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

bool cbm_is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool cbm_is_link(const char *path) {
    struct stat st;
    return path && lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

bool cbm_mkdir_p(const char *path, int mode) {
    /* Try direct mkdir first */
    if (mkdir(path, (mode_t)mode) == 0) {
        return true;
    }
    /* Walk path and create each component */
    char *tmp = strdup(path);
    if (!tmp) {
        return false;
    }
    for (char *p = tmp + SKIP_ONE; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, (mode_t)mode); /* ignore intermediate errors */
            *p = '/';
        }
    }
    bool ok = (mkdir(tmp, (mode_t)mode) == 0 || errno == EEXIST) != 0;
    free(tmp);
    return ok;
}

int cbm_unlink(const char *path) {
    return unlink(path);
}

int cbm_rmdir(const char *path) {
    return rmdir(path);
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return CBM_NOT_FOUND;
    }
    if (pid == 0) {
        /* Child: exec directly — no shell interpretation */
        /* 127 = standard "command not found" exit code (POSIX convention) */
        enum { EXEC_NOT_FOUND = 127 };
        execvp(argv[0], (char *const *)argv);
        _exit(EXEC_NOT_FOUND);
    }
    /* Parent: wait for child */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return CBM_NOT_FOUND;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return CBM_NOT_FOUND; /* killed by signal */
}

#endif /* _WIN32 */

/* Absolute path of git from the ABSOLUTE entries of PATH only: an empty or
 * relative entry ("", ".", "bin") would resolve against the current
 * directory, where a repository being indexed could plant its own "git". */
bool cbm_find_git(char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    const char *path = getenv("PATH");
    if (!path) {
        return false;
    }
#ifdef _WIN32
    const char sep = ';';
    const char *const names[] = {"git.exe", NULL};
#else
    const char sep = ':';
    const char *const names[] = {"git", NULL};
#endif
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, sep);
        size_t len = end ? (size_t)(end - p) : strlen(p);
#ifdef _WIN32
        bool absolute =
            (len >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
            (len >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'));
#else
        bool absolute = len > 0 && p[0] == '/';
#endif
        while (absolute && len > 1 && (p[len - 1] == '/' || p[len - 1] == '\\')) {
            len--;
        }
        for (int i = 0; absolute && names[i]; i++) {
            int n = snprintf(out, out_sz, "%.*s/%s", (int)len, p, names[i]);
            if (n <= 0 || (size_t)n >= out_sz) {
                continue;
            }
#ifdef _WIN32
            if (cbm_is_regular_file(out)) {
                return true;
            }
#else
            if (cbm_is_regular_file(out) && access(out, X_OK) == 0) {
                return true;
            }
#endif
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    out[0] = '\0';
    return false;
}

/* rename() with overwrite semantics on every platform: POSIX rename already
 * replaces atomically; Windows rename fails with EEXIST when the target
 * exists, so use write-through MoveFileExW(MOVEFILE_REPLACE_EXISTING) there
 * (wide paths — raw MoveFileExA would re-mangle non-ASCII cache paths). */
int cbm_rename_replace(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t *wsrc = cbm_utf8_to_wide_path(src);
    wchar_t *wdst = cbm_utf8_to_wide_path(dst);
    int ret = CBM_NOT_FOUND;
    if (wsrc && wdst) {
        if (MoveFileExW(wsrc, wdst,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED |
                            MOVEFILE_WRITE_THROUGH)) {
            ret = 0;
        } else {
            /* Translate the Win32 error into errno so a caller can report WHY
             * the publish failed (#1620): otherwise errno is whatever an
             * unrelated CRT call left behind, and an ACL denial reaches the
             * user as a generic pipeline failure. MoveFileEx needs DELETE on
             * the destination, which a cache file under an empty or foreign
             * DACL does not grant — hence ERROR_ACCESS_DENIED. */
            switch (GetLastError()) {
            case ERROR_ACCESS_DENIED:
            case ERROR_WRITE_PROTECT:
                errno = EACCES;
                break;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                errno = ENOENT;
                break;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
            case ERROR_USER_MAPPED_FILE:
                errno = EBUSY;
                break;
            case ERROR_NOT_SAME_DEVICE:
                errno = EXDEV;
                break;
            case ERROR_DISK_FULL:
                errno = ENOSPC;
                break;
            case ERROR_INVALID_NAME:
            case ERROR_FILENAME_EXCED_RANGE:
                errno = ENAMETOOLONG;
                break;
            default:
                errno = EIO;
                break;
            }
        }
    }
    free(wsrc);
    free(wdst);
    return ret;
#else
    return rename(src, dst);
#endif
}

/* Remove a SQLite database's -wal/-shm sidecars (both platforms). Any code
 * path that installs a FRESH database file where a previous generation lived
 * must remove them before the new generation can be opened: SQLite decides
 * whether to replay a WAL purely from the sidecar's own header/checksums, so
 * a leftover WAL can splice old-generation pages into the new file (#897). */
void cbm_remove_db_sidecars(const char *db_path) {
    if (!db_path || !db_path[0]) {
        return;
    }
    enum { SIDECAR_PATH_MAX = 4096 };
    char side[SIDECAR_PATH_MAX];
    int n = snprintf(side, sizeof(side), "%s-wal", db_path);
    if (n > 0 && (size_t)n < sizeof(side)) {
        (void)cbm_unlink(side);
    }
    n = snprintf(side, sizeof(side), "%s-shm", db_path);
    if (n > 0 && (size_t)n < sizeof(side)) {
        (void)cbm_unlink(side);
    }
}

/* The lock file names the DATABASE, not its current SQLite generation. Never
 * unlink it: a second inode would admit another writer while the first is held.
 * OS handles release on process death; no PID files or stale-lock timeout. */
struct cbm_db_lease {
    char *target;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

enum { DB_LEASE_PATH_MAX = 4096 };

/* Resolve existing symlinks and parent aliases before choosing the lock name.
 * Multiple hard links to a SQLite file are unsupported: SQLite sidecars already
 * require one filename. Fail closed rather than give them independent leases. */
#ifdef _WIN32
/* Ordinary Win32 paths trim terminal dots/spaces. A distinct sibling lock for
 * such a spelling would protect the wrong database. Check input and the final
 * handle path before stripping its extended prefix; resolved parents matter. */
template <typename Char>
static bool db_lease_windows_components_safe(const Char *path, bool allow_navigation) {
    if (!path || !*path) {
        return false;
    }
    const Char *part = path;
    for (const Char *p = path;; ++p) {
        if (*p && *p != '/' && *p != '\\') {
            continue;
        }
        size_t count = (size_t)(p - part);
        if (count) {
            bool navigation = allow_navigation && part[0] == '.' &&
                              (count == 1 || (count == 2 && part[1] == '.'));
            if (!navigation && (part[count - 1] == '.' || part[count - 1] == ' ')) {
                return false;
            }
        }
        if (!*p) {
            return true;
        }
        part = p + 1;
    }
}
#endif

static char *db_lease_real_path(const char *path, bool *missing) {
    *missing = false;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide_path(path);
    if (!wide) {
        return NULL;
    }
    HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD open_error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    DWORD attributes = h == INVALID_HANDLE_VALUE ? GetFileAttributesW(wide) : 0;
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        *missing =
            (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND) &&
            (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION info;
    wchar_t final_path[DB_LEASE_PATH_MAX];
    DWORD n = GetFinalPathNameByHandleW(h, final_path, DB_LEASE_PATH_MAX, FILE_NAME_NORMALIZED);
    bool valid = GetFileInformationByHandle(h, &info) &&
                 ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nNumberOfLinks == 1);
    CloseHandle(h);
    if (!valid || n == 0 || n >= DB_LEASE_PATH_MAX) {
        return NULL;
    }
    if (!db_lease_windows_components_safe(final_path, false)) {
        return NULL;
    }
    // Return ordinary normalized UTF-8 paths for the pipeline's existing path
    // helpers. They append '/' components, which extended-length paths forbid.
    const wchar_t *plain = final_path;
    if (wcsncmp(plain, L"\\\\?\\UNC\\", 8) == 0) {
        final_path[6] = L'\\';
        plain = final_path + 6;
    } else if (wcsncmp(plain, L"\\\\?\\", 4) == 0) {
        plain += 4;
    }
    char *utf8 = cbm_wide_to_utf8(plain);
    if (utf8) {
        for (char *p = utf8; *p; ++p) {
            if (*p == '\\') {
                *p = '/';
            }
        }
    }
    return utf8;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        *missing = errno == ENOENT;
        return NULL;
    }
    char *result = realpath(path, NULL);
    if (!result) {
        return NULL; // Includes dangling final symlinks: do not choose an alias lock.
    }
    if (stat(result, &st) != 0 || (S_ISREG(st.st_mode) && st.st_nlink != 1)) {
        free(result);
        return NULL;
    }
    return result;
#endif
}

static char *db_lease_canonical_target(const char *path) {
    if (!path || !path[0] || strlen(path) >= DB_LEASE_PATH_MAX - 32) {
        return NULL;
    }
#ifdef _WIN32
    if (!db_lease_windows_components_safe(path, true)) {
        return NULL;
    }
#endif
    bool missing = false;
    char *canonical = db_lease_real_path(path, &missing);
    if (canonical || !missing) {
        return canonical;
    }
    char parent[DB_LEASE_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
#ifdef _WIN32
    char *backslash = strrchr(parent, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    // A drive-relative filename (C:foo) is not a stable cache identity.
    if (!slash && strchr(parent, ':')) {
        return NULL;
    }
#endif
    const char *basename = slash ? slash + 1 : path;
    if (!basename[0] || strcmp(basename, ".") == 0 || strcmp(basename, "..") == 0) {
        return NULL;
    }
    char leaf[DB_LEASE_PATH_MAX];
    snprintf(leaf, sizeof(leaf), "%s", basename);
    if (slash) {
        // Preserve the root separator, including a Windows drive root.
#ifdef _WIN32
        if (slash == parent + 2 && parent[1] == ':') {
            slash[1] = '\0';
        } else
#endif
            if (slash == parent) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        snprintf(parent, sizeof(parent), ".");
    }
    if (!cbm_mkdir_p(parent, 0755)) {
        return NULL;
    }
    char *real_parent = db_lease_real_path(parent, &missing);
    if (!real_parent) {
        return NULL;
    }
    char target[DB_LEASE_PATH_MAX];
    int n = snprintf(target, sizeof(target), "%s/%s", real_parent, leaf);
    free(real_parent);
    if (n <= 0 || (size_t)n >= sizeof(target) - 32) {
        return NULL;
    }
    // Recheck after parent creation: another participant may have installed a
    // DB or final symlink in the meantime. An error other than missing is fatal.
    canonical = db_lease_real_path(target, &missing);
    return canonical ? canonical : (missing ? strdup(target) : NULL);
}

int cbm_db_lease_try_acquire(const char *db_path, cbm_db_lease_t **out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;
    char *target = db_lease_canonical_target(db_path);
    // All existing pipeline DB/sidecar buffers are CBM_PATH_MAX. Do not admit
    // a canonical target that those consumers would silently truncate.
    if (!target || strlen(target) >= CBM_PATH_MAX - 32) {
        free(target);
        return CBM_NOT_FOUND;
    }
    char lock_path[DB_LEASE_PATH_MAX];
    int n = snprintf(lock_path, sizeof(lock_path), "%s.index.lock", target);
    if (n <= 0 || (size_t)n >= sizeof(lock_path)) {
        free(target);
        return CBM_NOT_FOUND;
    }
    cbm_db_lease_t *lease = (cbm_db_lease_t *)calloc(1, sizeof(*lease));
    if (!lease) {
        free(target);
        return CBM_NOT_FOUND;
    }
    lease->target = target;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide_path(lock_path);
    lease->handle =
        wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
             : INVALID_HANDLE_VALUE;
    free(wide);
    if (lease->handle == INVALID_HANDLE_VALUE) {
        cbm_db_lease_release(lease);
        return CBM_NOT_FOUND;
    }
    OVERLAPPED overlap = {};
    if (!LockFileEx(lease->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &overlap)) {
        DWORD error = GetLastError();
        cbm_db_lease_release(lease);
        return error == ERROR_LOCK_VIOLATION ? CBM_INDEX_BUSY : CBM_NOT_FOUND;
    }
#else
    // A distinct open description also excludes concurrent callers in this
    // process. CLOEXEC keeps unrelated subprocesses from retaining the lease.
    lease->fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lease->fd < 0) {
        cbm_db_lease_release(lease);
        return CBM_NOT_FOUND;
    }
    if (flock(lease->fd, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        cbm_db_lease_release(lease);
        return error == EWOULDBLOCK || error == EAGAIN ? CBM_INDEX_BUSY : CBM_NOT_FOUND;
    }
#endif
    *out = lease;
    return 0;
}

/* Shared body of the probes: the lock file <real db path><suffix>, opened
 * only if it exists, tried exclusively and released. */
static int db_lock_probe(const char *db_path, const char *suffix) {
    bool missing = false;
    char *target = db_path && db_path[0] && strlen(db_path) < DB_LEASE_PATH_MAX - 32
                       ? db_lease_real_path(db_path, &missing)
                       : NULL;
    if (!target && missing) {
        /* No database yet -- but an index run or a trash restore takes the
         * lease BEFORE the file appears, so the lock file can be held
         * already. It sits beside the database under the parent's real
         * path, as the lease derives it; resolve that without creating
         * anything (no parent: no lock file can exist). */
        char parent[DB_LEASE_PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", db_path);
        char *slash = strrchr(parent, '/');
#ifdef _WIN32
        char *backslash = strrchr(parent, '\\');
        if (backslash && (!slash || backslash > slash)) {
            slash = backslash;
        }
#endif
        if (!slash || !slash[1]) {
            return 0;
        }
        char leaf[DB_LEASE_PATH_MAX];
        snprintf(leaf, sizeof(leaf), "%s", slash + 1);
        if (slash == parent) {
            slash[1] = '\0'; /* "/x.db": the parent is "/" */
        } else if (slash == parent + 2 && parent[1] == ':') {
            slash[1] = '\0'; /* "C:\x.db": the parent is "C:\", not drive-relative "C:" */
        } else {
            *slash = '\0';
        }
        bool parent_missing = false;
        char *real_parent = db_lease_real_path(parent, &parent_missing);
        if (!real_parent) {
            return parent_missing ? 0 : -1;
        }
        size_t need = strlen(real_parent) + 1 + strlen(leaf) + 1;
        target = (char *)malloc(need);
        if (!target) {
            free(real_parent);
            return -1;
        }
        snprintf(target, need, "%s/%s", real_parent, leaf);
        free(real_parent);
    }
    if (!target) {
        return -1;
    }
    char lock_path[DB_LEASE_PATH_MAX];
    int n = snprintf(lock_path, sizeof(lock_path), "%s%s", target, suffix);
    free(target);
    if (n <= 0 || (size_t)n >= sizeof(lock_path)) {
        return -1;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide_path(lock_path);
    HANDLE h =
        wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
             : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE && wide) {
        h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    }
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    OVERLAPPED overlap = {};
    int rc = 0;
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
        OVERLAPPED unlock = {};
        UnlockFileEx(h, 0, 1, 0, &unlock);
    } else {
        rc = GetLastError() == ERROR_LOCK_VIOLATION ? CBM_INDEX_BUSY : -1;
    }
    CloseHandle(h);
    return rc;
#else
    int fd = open(lock_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = open(lock_path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    int rc = 0;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        (void)flock(fd, LOCK_UN);
    } else {
        rc = errno == EWOULDBLOCK || errno == EAGAIN ? CBM_INDEX_BUSY : -1;
    }
    close(fd);
    return rc;
#endif
}

int cbm_db_lease_probe(const char *db_path) {
    return db_lock_probe(db_path, ".index.lock");
}

int cbm_db_read_gate_probe(const char *db_path) {
    return db_lock_probe(db_path, ".read.lock");
}

const char *cbm_db_lease_target(const cbm_db_lease_t *lease) {
    return lease ? lease->target : NULL;
}

void cbm_db_lease_release(cbm_db_lease_t *lease) {
    if (!lease) {
        return;
    }
#ifdef _WIN32
    if (lease->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(lease->handle);
    }
#else
    if (lease->fd >= 0) {
        close(lease->fd);
    }
#endif
    free(lease->target);
    free(lease);
}

enum { DB_READ_GATE_WAIT_MS = 2000, DB_READ_GATE_POLL_MS = 5 };

int cbm_db_read_gate_acquire(const char *db_path, bool exclusive, cbm_db_lease_t **out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;
    // Only an existing database has readers to protect: never create a
    // parent directory or a lock file for a path that holds nothing.
    bool missing = false;
    char *target = db_path && db_path[0] && strlen(db_path) < DB_LEASE_PATH_MAX - 32
                       ? db_lease_real_path(db_path, &missing)
                       : NULL;
    if (!target) {
        return CBM_NOT_FOUND;
    }
    char lock_path[DB_LEASE_PATH_MAX];
    int n = snprintf(lock_path, sizeof(lock_path), "%s.read.lock", target);
    if (n <= 0 || (size_t)n >= sizeof(lock_path)) {
        free(target);
        return CBM_NOT_FOUND;
    }
    cbm_db_lease_t *gate = (cbm_db_lease_t *)calloc(1, sizeof(*gate));
    if (!gate) {
        free(target);
        return CBM_NOT_FOUND;
    }
    gate->target = target;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide_path(lock_path);
    gate->handle =
        wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
             : INVALID_HANDLE_VALUE;
    if (gate->handle == INVALID_HANDLE_VALUE && wide && !exclusive) {
        // A read-only cache directory: lock an existing file read-only.
        gate->handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    free(wide);
    if (gate->handle == INVALID_HANDLE_VALUE) {
        cbm_db_lease_release(gate);
        return CBM_NOT_FOUND;
    }
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    for (int waited = 0;; waited += DB_READ_GATE_POLL_MS) {
        OVERLAPPED overlap = {};
        if (LockFileEx(gate->handle, flags, 0, 1, 0, &overlap)) {
            break;
        }
        DWORD error = GetLastError();
        if (error != ERROR_LOCK_VIOLATION || exclusive || waited >= DB_READ_GATE_WAIT_MS) {
            cbm_db_lease_release(gate);
            return error == ERROR_LOCK_VIOLATION ? CBM_INDEX_BUSY : CBM_NOT_FOUND;
        }
        Sleep(DB_READ_GATE_POLL_MS);
    }
#else
    gate->fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (gate->fd < 0 && !exclusive) {
        // A read-only cache directory: flock works on a read-only descriptor.
        gate->fd = open(lock_path, O_RDONLY | O_CLOEXEC);
    }
    if (gate->fd < 0) {
        cbm_db_lease_release(gate);
        return CBM_NOT_FOUND;
    }
    // Polled, not blocking: a holder of the exclusive side never waits on
    // anything, so the wait is short, and a bounded one cannot deadlock.
    int op = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    for (int waited = 0;; waited += DB_READ_GATE_POLL_MS) {
        if (flock(gate->fd, op) == 0) {
            break;
        }
        int error = errno;
        bool busy = error == EWOULDBLOCK || error == EAGAIN;
        if ((!busy && error != EINTR) || exclusive || waited >= DB_READ_GATE_WAIT_MS) {
            cbm_db_lease_release(gate);
            return busy ? CBM_INDEX_BUSY : CBM_NOT_FOUND;
        }
        usleep(DB_READ_GATE_POLL_MS * 1000);
    }
#endif
    *out = gate;
    return 0;
}
