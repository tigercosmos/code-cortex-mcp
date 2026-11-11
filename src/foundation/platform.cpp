/*
 * platform.c — OS abstraction implementations.
 *
 * macOS, Linux, and Windows. Platform-specific code behind #ifdef guards.
 */
#include "platform.h"

#include "foundation/constants.h"
#include "foundation/log.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <string>
#include <algorithm>

/* Canonicalize a Windows drive letter to upper-case in place: "c:/x" -> "C:/x".
 * Windows drive letters are case-insensitive, but a lowercase one (as agent
 * CWDs often report, e.g. Claude Code's "c:\...") otherwise produces a distinct
 * project key ("c-..." vs "C-...") and, on a case-insensitive FS, a colliding
 * cache file that clobbers the good index (#227/#367/#394). Folding to a single
 * canonical form here — at the one path-normalization choke point — keeps the
 * project key, cache file and integrity check consistent regardless of case.
 * Only the strict drive-root form `X:/` or bare `X:` is touched, so ordinary
 * POSIX paths (which never start that way) are unaffected. */
static void cbm_canonicalize_drive(char *path) {
    if (path && path[0] >= 'a' && path[0] <= 'z' && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\0')) {
        path[0] = (char)(path[0] - 'a' + 'A');
    }
}

#ifdef _WIN32

/* ── Windows implementation ───────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include "foundation/win_utf8.h"

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return NULL;
    }

    HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(wpath);
        return NULL;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart == 0) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    void *addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    CloseHandle(file);
    free(wpath);
    if (!addr) {
        return NULL;
    }
    *out_size = (size_t)sz.QuadPart;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    (void)size;
    if (addr) {
        UnmapViewOfFile(addr);
    }
}

uint64_t cbm_now_ns(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)count.QuadPart * 1000000000ULL / (uint64_t)freq.QuadPart;
}

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

int cbm_nprocs(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool cbm_file_exists(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool cbm_is_dir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t cbm_file_size(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) {
        return CBM_NOT_FOUND;
    }
    LARGE_INTEGER sz;
    sz.HighPart = (LONG)fad.nFileSizeHigh; // cppcheck-suppress unreadVariable
    sz.LowPart = fad.nFileSizeLow;         // cppcheck-suppress unreadVariable
    return (int64_t)sz.QuadPart;
}

int64_t cbm_file_mtime(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide_path(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) {
        return CBM_NOT_FOUND;
    }
    /* FILETIME is 100ns ticks since 1601; scale to seconds. The epoch offset
     * does not matter — this value is only ever compared against itself.
     * The suppressions mirror cbm_file_size above: cppcheck reads the two
     * half-assignments as unread because it does not model the union. */
    enum { FILETIME_TICKS_PER_SEC = 10000000 };
    ULARGE_INTEGER t;
    t.HighPart = fad.ftLastWriteTime.dwHighDateTime; // cppcheck-suppress unreadVariable
    t.LowPart = fad.ftLastWriteTime.dwLowDateTime;   // cppcheck-suppress unreadVariable
    return (int64_t)(t.QuadPart / FILETIME_TICKS_PER_SEC);
}

bool cbm_file_generation(const char *path, cbm_file_gen_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->size = CBM_NOT_FOUND;
    wchar_t *wpath = path ? cbm_utf8_to_wide_path(path) : NULL;
    if (!wpath) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) {
        return false;
    }
    enum { FILETIME_TICKS_PER_SEC = 10000000, FILETIME_NS_PER_TICK = 100 };
    ULARGE_INTEGER sz;
    sz.HighPart = fad.nFileSizeHigh; // cppcheck-suppress unreadVariable
    sz.LowPart = fad.nFileSizeLow;   // cppcheck-suppress unreadVariable
    out->size = (int64_t)sz.QuadPart;
    ULARGE_INTEGER t;
    t.HighPart = fad.ftLastWriteTime.dwHighDateTime; // cppcheck-suppress unreadVariable
    t.LowPart = fad.ftLastWriteTime.dwLowDateTime;   // cppcheck-suppress unreadVariable
    out->mtime = (int64_t)(t.QuadPart / FILETIME_TICKS_PER_SEC);
    /* FILETIME already carries 100ns resolution, which is finer than the
     * second-granularity trap this field exists to close. No file index here:
     * it would need the file opened (GetFileInformationByHandle), and the
     * sub-second stamp alone already distinguishes a rewrite. */
    out->mtime_ns = (int64_t)((t.QuadPart % FILETIME_TICKS_PER_SEC) * FILETIME_NS_PER_TICK);
    out->ino = 0;
    return true;
}

char *cbm_normalize_path_sep(char *path) {
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#else /* POSIX (macOS + Linux) */

/* ── POSIX implementation ─────────────────────────────────────── */

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#else
#include <sched.h>
#endif

/* ── Memory mapping ────────────────────────────────────────────── */

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return NULL;
    }

    void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        return NULL;
    }
    *out_size = (size_t)st.st_size;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    if (addr && size > 0) {
        munmap(addr, size);
    }
}

/* ── Timing ────────────────────────────────────────────────────── */

#ifdef __APPLE__
static mach_timebase_info_data_t timebase_info;
static int timebase_init = 0;

uint64_t cbm_now_ns(void) {
    if (!timebase_init) {
        mach_timebase_info(&timebase_info);
        timebase_init = SKIP_ONE;
    }
    uint64_t ticks = mach_absolute_time();
    return ticks * timebase_info.numer / timebase_info.denom;
}
#else
uint64_t cbm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

/* ── System info ───────────────────────────────────────────────── */

int cbm_nprocs(void) {
#ifdef __APPLE__
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        return ncpu;
    }
    enum { FILE_EXISTS = 1 };
    return FILE_EXISTS;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ── File system ───────────────────────────────────────────────── */

bool cbm_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool cbm_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t cbm_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return CBM_NOT_FOUND;
    }
    return (int64_t)st.st_size;
}

int64_t cbm_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return CBM_NOT_FOUND;
    }
    return (int64_t)st.st_mtime;
}

bool cbm_file_generation(const char *path, cbm_file_gen_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->size = CBM_NOT_FOUND;
    struct stat st;
    if (!path || stat(path, &st) != 0) {
        return false;
    }
    out->size = (int64_t)st.st_size;
    out->mtime = (int64_t)st.st_mtime;
    out->ino = (uint64_t)st.st_ino;
#if defined(__APPLE__)
    out->mtime_ns = (int64_t)st.st_mtimespec.tv_nsec;
#elif defined(st_mtime) || defined(_POSIX_C_SOURCE)
    /* POSIX.1-2008 spells the sub-second field st_mtim; the `st_mtime` macro
     * that aliases st_mtim.tv_sec is what marks its presence. */
    out->mtime_ns = (int64_t)st.st_mtim.tv_nsec;
#endif
    return true;
}

char *cbm_normalize_path_sep(char *path) {
    /* Normalize on ALL platforms — backslash paths can arrive via stored
     * data, cross-platform DB files, or Windows-style arguments. */
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#endif /* _WIN32 */

/* ── Environment variables ────────────────────────────────────── */

/* Thread-safe getenv: iterates environ directly instead of calling getenv().
 * getenv() is flagged by concurrency-mt-unsafe because the returned pointer
 * can be invalidated by setenv/putenv in another thread. We copy to a
 * caller-owned buffer immediately. */
#ifdef _WIN32
#include <stdlib.h>
#define CBM_ENVIRON _environ
#elif defined(__APPLE__)
#include <crt_externs.h>
#define CBM_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define CBM_ENVIRON environ
#endif

const char *cbm_safe_getenv(const char *name, char *buf, size_t buf_sz, const char *fallback) {
#ifdef _WIN32
    /* #996 Layer 2: _environ holds ANSI-code-page bytes, not UTF-8, so a
     * non-ASCII value (a CBM_CACHE_DIR or USERPROFILE with Greek/CJK/accented
     * characters) arrives mojibake'd or with '?' substitutes, which every
     * downstream UTF-8 file API then rejects. Read the process environment
     * wide and convert to UTF-8, the convention cbm_fopen / _wmkdir / the
     * SQLite VFS already assume (upstream 25546312 and its Layer 2 base). */
    if (!name || !name[0] || !buf || buf_sz == 0) {
        return NULL;
    }
    buf[0] = '\0';
    wchar_t *wname = cbm_utf8_to_wide(name);
    if (wname) {
        SetLastError(ERROR_SUCCESS);
        DWORD needed = GetEnvironmentVariableW(wname, NULL, 0U);
        DWORD lookup_error = GetLastError();
        if (needed == 0U) {
            free(wname);
            if (lookup_error == ERROR_ENVVAR_NOT_FOUND) {
                if (fallback) {
                    snprintf(buf, buf_sz, "%s", fallback);
                    return buf;
                }
                return NULL;
            }
            return buf; /* an existing empty variable is not a missing one */
        }
        wchar_t *wval = (wchar_t *)calloc((size_t)needed, sizeof(*wval));
        if (!wval) {
            free(wname);
            return NULL;
        }
        SetLastError(ERROR_SUCCESS);
        DWORD got = GetEnvironmentVariableW(wname, wval, needed);
        DWORD read_error = GetLastError();
        free(wname);
        if (got >= needed || (got == 0U && read_error != ERROR_SUCCESS)) {
            free(wval);
            return NULL;
        }
        char *utf8 = cbm_wide_to_utf8(wval);
        free(wval);
        if (!utf8) {
            return NULL;
        }
        snprintf(buf, buf_sz, "%s", utf8);
        free(utf8);
        return buf;
    }
    /* A name that is not valid UTF-8 cannot be looked up wide: fall through to
     * the narrow scan below. */
#endif
    char **env = CBM_ENVIRON;
    if (env) {
        size_t nlen = strlen(name);
        for (; *env; env++) {
            if (strncmp(*env, name, nlen) == 0 && (*env)[nlen] == '=') {
                snprintf(buf, buf_sz, "%s", *env + nlen + SKIP_ONE);
                return buf;
            }
        }
    }
    if (fallback) {
        snprintf(buf, buf_sz, "%s", fallback);
        return buf;
    }
    buf[0] = '\0';
    return NULL;
}

/* See platform.h: an end pointer says where the read stopped, errno catches a
 * number too large, and *end == '\0' catches anything left over. */
bool cbm_env_long(const char *name, long *out) {
    if (!out) {
        return false;
    }
    char raw[CBM_SZ_64] = {0};
    if (!cbm_safe_getenv(name, raw, sizeof(raw), NULL) || !raw[0]) {
        return false;
    }
    /* strtol skips leading blanks of its own accord, so " 5" would read as 5.
     * A blank in front of a setting is a slip, not a number. */
    if (isspace((unsigned char)raw[0])) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    long value = strtol(raw, &end, CBM_DECIMAL_BASE);
    if (errno != 0 || !end || end == raw || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

/* See platform.h. Re-reads the raw text only on the rare path where it is
 * dropped, so the warning can quote what the person actually typed. */
bool cbm_env_long_warn(const char *name, long min, long max, long *out, const char *ignored_event) {
    long value = 0;
    if (out && cbm_env_long(name, &value) && value >= min && value <= max) {
        *out = value;
        return true;
    }
    char raw[CBM_SZ_64] = {0};
    if (cbm_safe_getenv(name, raw, sizeof(raw), NULL) && raw[0]) {
        cbm_log_warn(ignored_event, "value", raw, "action", "using_default");
    }
    return false;
}

/* ── Home directory (cross-platform) ──────────────────────────── */

const char *cbm_get_home_dir(void) {
    static thread_local char buf[CBM_SZ_1K]; /* per thread: see cbm_resolve_cache_dir */
    char tmp[CBM_SZ_256] = "";

    cbm_safe_getenv("HOME", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        cbm_normalize_path_sep(buf);
        return buf;
    }

    cbm_safe_getenv("USERPROFILE", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        cbm_normalize_path_sep(buf);
        return buf;
    }
    return NULL;
}

/* ── App config directories (cross-platform) ─────────────────── */

const char *cbm_app_config_dir(void) {
    static thread_local char buf[CBM_SZ_1K]; /* per thread: see cbm_resolve_cache_dir */
    char tmp[CBM_SZ_256] = "";
#ifdef _WIN32
    cbm_safe_getenv("APPDATA", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        cbm_normalize_path_sep(buf);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (home) {
        snprintf(buf, sizeof(buf), "%s/AppData/Roaming", home);
        return buf;
    }
    return NULL;
#else
    /* Linux: XDG_CONFIG_HOME or ~/.config */
    cbm_safe_getenv("XDG_CONFIG_HOME", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.config", home);
        return buf;
    }
    return NULL;
#endif /* _WIN32 */
}

const char *cbm_app_local_dir(void) {
#ifdef _WIN32
    static thread_local char buf[CBM_SZ_1K]; /* per thread: see cbm_resolve_cache_dir */
    char tmp[CBM_SZ_256] = "";
    cbm_safe_getenv("LOCALAPPDATA", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        cbm_normalize_path_sep(buf);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (home) {
        snprintf(buf, sizeof(buf), "%s/AppData/Local", home);
        return buf;
    }
    return NULL;
#else
    return cbm_app_config_dir();
#endif
}

/* ── Cache directory ─────────────────────────────────────────── */

/* These path helpers return a buffer the caller only reads. It is per
 * thread: the server resolves paths on its main, sweep and auto-index
 * threads at once, and one shared buffer, rewritten by every call, handed a
 * reader a half-written path ("unable to open database file", ENOENT, on
 * _config.db -- a use left unrecorded). */

const char *cbm_resolve_cache_dir(void) {
    static thread_local char buf[CBM_SZ_1K]; /* per thread: see cbm_resolve_cache_dir */
    char tmp[CBM_SZ_256] = "";
    cbm_safe_getenv("CBM_CACHE_DIR", tmp, sizeof(tmp), NULL);
    if (tmp[0]) {
        snprintf(buf, sizeof(buf), "%s", tmp);
        cbm_normalize_path_sep(buf);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    snprintf(buf, sizeof(buf), "%s/.cache/code-cortex-mcp", home);
    return buf;
}

/* ── Self-executable path ─────────────────────────────────────── */

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef _WIN32
/* #1204: an OS-reported self path is only usable if it still names a runnable
 * image. After an installer's atomic rename-over it does not. */
static bool self_path_is_executable(const char *path) {
    struct stat st;
    if (!path || !path[0] || stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}
#endif

bool cbm_resolve_self_exe_path(const char *argv0, char *out, size_t outsz) {
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = '\0';

    /* Prefer an explicit, usable argv0 path. */
#ifndef _WIN32
    if (argv0 && strchr(argv0, '/')) {
        snprintf(out, outsz, "%s", argv0);
        return out[0] != '\0';
    }
#else
    if (argv0 && argv0[0]) {
        DWORD attrs = GetFileAttributesA(argv0);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(out, outsz, "%s", argv0);
            return out[0] != '\0';
        }
    }
#endif

    /* Fall back to the OS-reported path of this executable. */
#ifdef _WIN32
    if (GetModuleFileNameA(NULL, out, (DWORD)outsz) > 0) {
        return out[0] != '\0';
    }
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)outsz;
    if (_NSGetExecutablePath(out, &sz) == 0 && out[0] != '\0') {
        if (self_path_is_executable(out)) {
            return true;
        }
        /* #1204: the image was replaced under us (an install/upgrade renames
         * over the running binary), so this path no longer exists and handing
         * it back turns into a doomed worker spawn (ENOENT). macOS has no
         * /proc magic link to fall back to, so fail CLOSED: the supervisor
         * logs index.supervisor.no_self_path and degrades in-process instead
         * of exec'ing a missing binary. Deliberately NOT argv0 either — after
         * a replacement that path holds a DIFFERENT build, which only swaps
         * the ENOENT for a build-fingerprint refusal. */
        out[0] = '\0';
        return false;
    }
#else
    ssize_t len = readlink("/proc/self/exe", out, outsz - 1);
    if (len > 0) {
        out[len] = '\0';
        if (self_path_is_executable(out)) {
            return true;
        }
        /* #1204: deleted image — readlink reports "<path> (deleted)". The magic
         * link itself still executes the in-memory OLD build, which is the only
         * spawn the worker's build-fingerprint gate accepts, so hand back the
         * link rather than the stale path. */
        snprintf(out, outsz, "/proc/self/exe");
        return true;
    }
#endif

    /* Last resort: echo argv0 as-is (may be a bare name resolved via PATH). */
    if (argv0 && argv0[0]) {
        snprintf(out, outsz, "%s", argv0);
        return out[0] != '\0';
    }
    return false;
}

/* Platform-independent: both generations were produced by the code above. */
bool cbm_file_gen_equal(const cbm_file_gen_t *a, const cbm_file_gen_t *b) {
    if (!a || !b || a->size < 0 || b->size < 0) {
        return false; /* an unstattable file is never "the same" as anything */
    }
    return a->size == b->size && a->mtime == b->mtime && a->mtime_ns == b->mtime_ns &&
           a->ino == b->ino;
}

bool cbm_root_normalize(const char *in, bool fold_case, char *out, size_t out_sz) {
    /* Room for the longest prefix and its terminator: "c:/" or "//" + NUL.
     * Below that, a UNC prefix wrote its terminator past the buffer. */
    if (!in || !out || out_sz < 4) {
        return false;
    }
#ifdef _WIN32
    /* The volume folds case by Unicode rules, not ASCII ones: "C:\\Ä\\repo"
     * and "c:\\ä\\repo" are one directory. Fold the whole string the way
     * the filesystem does (CharLowerW), then the byte pass below is a no-op
     * for the non-ASCII characters. */
    char *folded = NULL;
    if (fold_case) {
        wchar_t *w = cbm_utf8_to_wide(in);
        if (w) {
            CharLowerW(w);
            folded = cbm_wide_to_utf8(w);
            free(w);
        }
    }
    if (folded) {
        in = folded;
    }
#endif
    size_t n = strlen(in);
    char *work = (char *)malloc(n + 1);
    if (!work) {
#ifdef _WIN32
        free(folded);
#endif
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
#ifdef _WIN32
        char c = in[i] == '\\' ? '/' : in[i];
#else
        char c = in[i]; /* POSIX: a backslash is a filename character */
#endif
        work[i] = fold_case ? (char)tolower((unsigned char)c) : c;
    }
#ifdef _WIN32
    free(folded);
#endif
    /* Prefix: on Windows a drive ("c:", its letter's case never matters) or
     * a UNC share ("//"); a root ("/"); none for a relative path. POSIX has
     * neither drives nor shares -- "C:/repo" is a relative path and "//x"
     * is "/x" -- so those rules stay Windows-only, or distinct POSIX paths
     * would fold into one checkout. */
    size_t o = 0;
    const char *p = work;
    bool absolute = false;
#ifdef _WIN32
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        if (o + 2 >= out_sz) {
            free(work);
            return false;
        }
        out[o++] = (char)tolower((unsigned char)p[0]);
        out[o++] = ':';
        p += 2;
    }
    if (p[0] == '/' && p[1] == '/' && o == 0) {
        out[o++] = '/';
        p++;
    }
#endif
    if (p[0] == '/') {
        absolute = true;
    }
    size_t base = o; /* never pop below the prefix */
    if (absolute) {
        out[o++] = '/';
        base = o;
    }
    out[o] = '\0';
    /* Segments: drop "" and ".", let ".." remove the previous one. */
    while (*p) {
        while (*p == '/') {
            p++;
        }
        const char *seg = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - seg);
        if (len == 0 || (len == 1 && seg[0] == '.')) {
            continue;
        }
        bool dotdot = len == 2 && seg[0] == '.' && seg[1] == '.';
        if (dotdot && o == base && absolute) {
            continue; /* "/.." is "/" */
        }
        /* ".." pops the previous segment -- unless that segment is itself an
         * unresolved ".." of a relative path ("../../repo"): popping it would
         * make "../../repo" into "repo", one checkout's identity for
         * another's. Such a ".." is kept and this one appended after it. */
        bool prev_is_dotdot = false;
        if (dotdot && o > base) {
            size_t start = o;
            while (start > base && out[start - 1] != '/') {
                start--;
            }
            prev_is_dotdot = o - start == 2 && out[start] == '.' && out[start + 1] == '.';
        }
        if (dotdot && o > base && !prev_is_dotdot) {
            while (o > base && out[o - 1] != '/') {
                o--;
            }
            if (o > base) {
                o--; /* the separator before the dropped segment */
            }
            out[o] = '\0';
            continue;
        }
        size_t need = (o > base ? 1 : 0) + len;
        if (o + need >= out_sz) {
            free(work);
            return false;
        }
        if (o > base) {
            out[o++] = '/';
        }
        memcpy(out + o, seg, len);
        o += len;
        out[o] = '\0';
    }
    free(work);
    return true;
}

#ifdef _WIN32
/* The final path of an existing directory, as the volume spells it (a
 * junction or directory symlink resolved), in UTF-8 with the \\?\ prefix
 * stripped; NULL when it cannot be opened. Caller frees. */
static char *win_dir_final_path(const char *path) {
    wchar_t *wide = cbm_utf8_to_wide_path(path);
    if (!wide) {
        return NULL;
    }
    HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    wchar_t final_path[CBM_ROOT_MAX];
    DWORD n = GetFinalPathNameByHandleW(h, final_path, CBM_ROOT_MAX, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= CBM_ROOT_MAX) {
        return NULL;
    }
    const wchar_t *plain = final_path;
    if (wcsncmp(plain, L"\\\\?\\UNC\\", 8) == 0) {
        final_path[6] = L'\\';
        plain = final_path + 6;
    } else if (wcsncmp(plain, L"\\\\?\\", 4) == 0) {
        plain += 4;
    }
    return cbm_wide_to_utf8(plain);
}

/* The volume serial and file index of a directory: its identity on the
 * volume, the same through every spelling, junction and symlink. */
static bool win_dir_identity(const char *path, DWORD *serial, DWORD *hi, DWORD *lo) {
    wchar_t *wide = cbm_utf8_to_wide_path(path);
    if (!wide) {
        return false;
    }
    HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (ok) {
        *serial = info.dwVolumeSerialNumber;
        *hi = info.nFileIndexHigh;
        *lo = info.nFileIndexLow;
    }
    return ok;
}
#endif

bool cbm_root_canonical(const char *root, char *out, size_t out_sz) {
    if (!root || !root[0]) {
        return false;
    }
#ifdef _WIN32
    /* The directory's final path where it exists (a junction or symlink
     * resolved, 8.3 names expanded, the volume's own spelling of the case),
     * else _fullpath: absolute, "." and ".." folded, lexically. */
    char *final_utf8 = win_dir_final_path(root);
    if (final_utf8) {
        bool ok = cbm_root_normalize(final_utf8, true, out, out_sz);
        free(final_utf8);
        return ok;
    }
    /* Gone (a removed worktree): the longest existing ancestor's final path
     * plus the missing tail, so the key is the one the checkout had while
     * it existed -- a junctioned ancestor resolved, as then. */
    /* Absolute, "." and ".." folded, at any length (_fullpath stops at
     * MAX_PATH, and a long removed checkout then lost its identity). */
    std::string full;
    {
        wchar_t *w = cbm_utf8_to_wide(root);
        DWORD need = w ? GetFullPathNameW(w, 0, NULL, NULL) : 0;
        wchar_t *buf = need ? (wchar_t *)malloc((size_t)need * sizeof(wchar_t)) : NULL;
        DWORD got = buf ? GetFullPathNameW(w, need, buf, NULL) : 0;
        char *u = got && got < need ? cbm_wide_to_utf8(buf) : NULL;
        if (u) {
            full = u;
        }
        free(u);
        free(buf);
        free(w);
    }
    if (full.empty()) {
        return cbm_root_normalize(root, true, out, out_sz);
    }
    if (full.compare(0, 8, "\\\\?\\UNC\\") == 0) {
        full.replace(0, 8, "\\\\"); /* \\?\UNC\server\share -> \\server\share */
    } else if (full.compare(0, 4, "\\\\?\\") == 0) {
        full.erase(0, 4);
    }
    std::replace(full.begin(), full.end(), '\\', '/');
    std::string head(full);
    /* Grows across iterations: every cut segment, outermost first. */
    // cppcheck-suppress variableScope
    std::string tail;
    for (;;) {
        size_t cut = head.find_last_of('/');
        if (cut == std::string::npos || cut < 3) {
            return cbm_root_normalize(full.c_str(), true, out, out_sz);
        }
        tail = head.substr(cut) + tail;
        head.resize(cut);
        char *anc = win_dir_final_path(head.c_str());
        if (anc) {
            std::string joined = std::string(anc) + tail;
            free(anc);
            return cbm_root_normalize(joined.c_str(), true, out, out_sz);
        }
    }
#else
    /* The path as given, where it exists: the filesystem applies ".." after
     * following a symlink ("a/link/../repo" with link -> other/sub is
     * other/repo), so nothing is folded before it is resolved. */
    char real[PATH_MAX];
    if (realpath(root, real)) {
        return cbm_root_normalize(real, false, out, out_sz);
    }
    /* Gone (a removed worktree): made absolute, then the longest existing
     * ancestor OF THE SPELLING AS GIVEN resolved and the rest appended raw
     * -- a ".." in the rest then applies to a resolved directory, as the
     * filesystem would apply it -- and only the result folded. The key is
     * the one the checkout had while it existed (macOS /tmp ->
     * /private/tmp, a relative root against the cwd); folding first could
     * give it another, existing checkout's identity. A root that folds to
     * nothing ("sub/..") is the current directory, never "". */
    std::string abs_path = root;
    if (abs_path.empty()) {
        return false;
    }
    if (abs_path[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            return cbm_root_normalize(root, false, out, out_sz);
        }
        abs_path = std::string(cwd) + "/" + abs_path;
    }
    while (abs_path.size() > 1 && abs_path.back() == '/') {
        abs_path.pop_back();
    }
    std::string head = abs_path;
    /* Grows across iterations: every cut segment, outermost first. */
    // cppcheck-suppress variableScope
    std::string tail;
    for (;;) {
        size_t cut = head.find_last_of('/');
        if (cut == std::string::npos) {
            return cbm_root_normalize(abs_path.c_str(), false, out, out_sz);
        }
        tail = head.substr(cut) + tail;
        head.resize(cut == 0 ? 1 : cut);
        if (realpath(head.c_str(), real)) {
            std::string joined = std::string(real) + tail; /* "//x" folds to "/x" */
            return cbm_root_normalize(joined.c_str(), false, out, out_sz);
        }
        if (head == "/") {
            return cbm_root_normalize(abs_path.c_str(), false, out, out_sz);
        }
    }
#endif
}

bool cbm_root_matches(const char *recorded, const char *dir) {
    if (!dir || !dir[0] || !recorded || !recorded[0]) {
        return true; /* nothing expected, or a legacy database with no root */
    }
    if (strcmp(recorded, dir) == 0) {
        return true;
    }
    /* Both exist: their file identities decide. One is gone: their
     * canonical forms (cbm_root_canonical) -- separators, drive-letter case,
     * "."/".." and trailing separators folded after the longest existing
     * ancestor is resolved (macOS /tmp -> /private/tmp; on Windows the final
     * path and case folding). */
    char a[CBM_ROOT_MAX];
    char b[CBM_ROOT_MAX];
#ifdef _WIN32
    /* The fold assumes the volume ignores case. A directory can opt into
     * case sensitivity (per-directory on NTFS), where "Repo" and "repo"
     * are two directories: when both paths exist, their file identities
     * decide -- a folded match between two different directories is no
     * match. Identity unknown for either (one is gone): the fold stands. */
    DWORD sa, ha, la, sb, hb, lb;
    bool ida = win_dir_identity(recorded, &sa, &ha, &la);
    bool idb = win_dir_identity(dir, &sb, &hb, &lb);
    if (ida && idb) {
        return sa == sb && ha == hb && la == lb;
    }
#else
    /* Both exist: device and inode decide, before any spelling is folded.
     * The filesystem applies ".." after following a symlink, so a lexical
     * fold ("a/link/../repo" -> "a/repo") could name another checkout. */
    struct stat ia;
    struct stat ib;
    if (stat(recorded, &ia) == 0 && stat(dir, &ib) == 0) {
        return ia.st_dev == ib.st_dev && ia.st_ino == ib.st_ino;
    }
#endif
    /* One of them is gone: their canonical forms decide, each resolved
     * through its longest existing ancestor before anything is folded. (A
     * lexical fold of the spellings alone took "a/link/../repo" for
     * "a/repo".) */
    return cbm_root_canonical(recorded, a, sizeof(a)) && cbm_root_canonical(dir, b, sizeof(b)) &&
           strcmp(a, b) == 0;
}

/* ── CPU scheduling priority ────────────────── */

#ifndef _WIN32
#include <sys/resource.h>
#endif

bool cbm_lower_cpu_priority(void) {
#ifdef _WIN32
    HANDLE self = GetCurrentProcess();
    DWORD current = GetPriorityClass(self);
    if (current == IDLE_PRIORITY_CLASS || current == BELOW_NORMAL_PRIORITY_CLASS) {
        return true; /* already at least this low: never raise priority */
    }
    return SetPriorityClass(self, BELOW_NORMAL_PRIORITY_CLASS) != 0;
#else
    /* getpriority can legitimately return -1, so errno is the only error signal. */
    errno = 0;
    int current = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && current >= CBM_LOW_CPU_PRIORITY_NICE) {
        return true; /* already at least this nice: never raise priority */
    }
    return setpriority(PRIO_PROCESS, 0, CBM_LOW_CPU_PRIORITY_NICE) == 0;
#endif
}
