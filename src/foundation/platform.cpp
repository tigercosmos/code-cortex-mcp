/*
 * platform.c — OS abstraction implementations.
 *
 * macOS, Linux, and Windows. Platform-specific code behind #ifdef guards.
 */
#include "platform.h"

#include "foundation/constants.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    wchar_t *wpath = cbm_utf8_to_wide(path);
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
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool cbm_is_dir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t cbm_file_size(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
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
    wchar_t *wpath = cbm_utf8_to_wide(path);
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
    wchar_t *wpath = path ? cbm_utf8_to_wide(path) : NULL;
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

/* ── Home directory (cross-platform) ──────────────────────────── */

const char *cbm_get_home_dir(void) {
    static char buf[CBM_SZ_1K];
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
    static char buf[CBM_SZ_1K];
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
    static char buf[CBM_SZ_1K];
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

const char *cbm_resolve_cache_dir(void) {
    static char buf[CBM_SZ_1K];
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
