/*
 * subprocess.c — cross-platform spawn + supervise + classify.
 * See subprocess.h. The spawn/reap skeleton was generalized from the former
 * UI http_server's index subprocess, adding crash/hang classification.
 */
#include "subprocess.h"

#include "compat.h"    /* cbm_nanosleep */
#include "compat_fs.h" /* cbm_fopen, cbm_unlink */
#include "platform.h"  /* cbm_now_ms */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "win_utf8.h" /* cbm_utf8_to_wide — spawn the worker with a wide command line so a
                       * non-ASCII repo path survives CreateProcess (#423/#20) */
#include <stdlib.h>   /* free */
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __APPLE__
#include <crt_externs.h> /* _NSGetEnviron — macOS does not declare `environ` */
#include <spawn.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#endif

/* NTSTATUS severity ERROR (top two bits set) covers the Windows crash exception
 * exit codes: 0xC0000005 (access violation), 0xC00000FD (stack overflow),
 * 0xC000001D (illegal instruction), 0xC0000094 (integer divide by zero), … */
#define CBM_WIN_CRASH_CODE_MIN 0xC0000000u

#ifndef _WIN32
static bool cbm_is_fault_signal(int sig) {
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGABRT:
    case SIGSYS:
        return true;
    default:
        return false;
    }
}
#endif

cbm_proc_outcome_t cbm_proc_classify(bool exited_normally, int exit_code, int term_signal,
                                     bool timed_out) {
    if (timed_out) {
        return CBM_PROC_HANG;
    }
    if (!exited_normally) {
        /* POSIX signal death. */
#ifndef _WIN32
        if (cbm_is_fault_signal(term_signal)) {
            return CBM_PROC_CRASH;
        }
#else
        (void)term_signal;
#endif
        return CBM_PROC_KILLED;
    }
    /* Exited with a code. A Windows NTSTATUS exception code is a crash; on POSIX
     * exit codes are 0..255 so this branch never misfires there. */
    if ((unsigned)exit_code >= CBM_WIN_CRASH_CODE_MIN) {
        return CBM_PROC_CRASH;
    }
    return (exit_code == 0) ? CBM_PROC_CLEAN : CBM_PROC_EXIT_NONZERO;
}

const char *cbm_proc_outcome_str(cbm_proc_outcome_t o) {
    switch (o) {
    case CBM_PROC_CLEAN:
        return "clean";
    case CBM_PROC_EXIT_NONZERO:
        return "exit_nonzero";
    case CBM_PROC_CRASH:
        return "crash";
    case CBM_PROC_HANG:
        return "hang";
    case CBM_PROC_KILLED:
        return "killed";
    case CBM_PROC_SPAWN_FAILED:
    default:
        return "spawn_failed";
    }
}

/* Tail newly-appended complete lines from the child log, starting at *tail_pos.
 * A partial (non-newline-terminated) final line is left buffered: *tail_pos is
 * not advanced past it, so it is re-read once completed. Returns true if any
 * complete line was consumed (i.e. there was progress). */
static bool cbm_tail_log(const char *log_file, long *tail_pos, cbm_proc_log_cb cb, void *ud) {
    if (!log_file) {
        return false;
    }
    FILE *lf = cbm_fopen(log_file, "r");
    if (!lf) {
        return false;
    }
    bool progressed = false;
    if (fseek(lf, *tail_pos, SEEK_SET) == 0) {
        char line[1024];
        for (;;) {
            long before = ftell(lf);
            if (!fgets(line, sizeof(line), lf)) {
                break;
            }
            size_t l = strlen(line);
            bool complete = (l > 0 && line[l - 1] == '\n');
            if (complete) {
                line[l - 1] = '\0';
                *tail_pos = ftell(lf);
                progressed = true;
                if (line[0] && cb) {
                    cb(line, ud);
                }
            } else if (l == sizeof(line) - 1) {
                /* Oversized line filled the buffer without a newline — consume it
                 * anyway (counts as progress) so we never stall on one long line. */
                *tail_pos = ftell(lf);
                progressed = true;
                if (cb) {
                    cb(line, ud);
                }
            } else {
                /* Genuine partial final line — keep it buffered for next poll. */
                *tail_pos = before;
                break;
            }
        }
    }
    fclose(lf);
    return progressed;
}

/* ── Windows command-line quoting (pure; unit-tested on every platform) ─────── */

/* Append char `c` to buf[cap], reserving the final byte for a NUL terminator.
 * On overflow: sets *ovf, stops writing, and returns pos UNCHANGED — callers detect
 * the overflow via the *ovf flag (not via the return value). */
static size_t cbm_cmdline_put(char *buf, size_t cap, size_t pos, char c, bool *ovf) {
    if (pos + 1 >= cap) {
        *ovf = true;
        return pos;
    }
    buf[pos] = c;
    return pos + 1;
}

/* Append one argv element to the command line using the Microsoft C runtime
 * quoting rules (see MS "Parsing C Command-Line Arguments"). CreateProcess takes
 * a SINGLE string that the child re-parses back into argv, so any element with a
 * space, tab or double-quote must be wrapped in quotes and its embedded quotes /
 * preceding backslashes escaped. Without this a JSON argument like
 * {"repo_path":"C:/r"} loses its inner quotes and the child receives the invalid
 * {repo_path:C:/r} — the Windows-only index-worker cmdline-quoting bug (the worker exited
 * non-zero at JSON-arg parse, misattributed to the last-marked file). POSIX is
 * unaffected: cbm_run_posix passes the argv array straight to execv. */
static size_t cbm_cmdline_append_arg(char *buf, size_t cap, size_t pos, const char *arg, bool first,
                                     bool *ovf) {
    if (!first) {
        pos = cbm_cmdline_put(buf, cap, pos, ' ', ovf);
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    for (const char *p = arg; *p;) {
        size_t nbs = 0;
        while (*p == '\\') {
            nbs++;
            p++;
        }
        if (*p == '\0') {
            /* Trailing backslashes precede the closing quote: double them so the
             * quote stays a delimiter, not an escaped literal. */
            for (size_t k = 0; k < nbs * 2; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            break;
        }
        if (*p == '"') {
            /* N backslashes then a quote -> 2N+1 backslashes then an escaped quote. */
            for (size_t k = 0; k < nbs * 2 + 1; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
            p++;
        } else {
            for (size_t k = 0; k < nbs; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, *p, ovf);
            p++;
        }
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    return pos;
}

/* Build a full Windows CreateProcess command line from a NULL-terminated argv,
 * applying the MS C runtime quoting rules so the child re-parses byte-identical
 * argv. Returns true on success, false if the result would overflow `buf`.
 *
 * Defined unconditionally (pure string logic, no Windows headers) so the quoting
 * contract is unit-tested on Linux/macOS CI too — even though the real spawn path
 * only runs on Windows. Every spawn site escapes through this one implementation;
 * a naive `"%s"` wrap silently corrupts any argument
 * containing a quote (e.g. the index JSON {"repo_path":"…"}), corrupting the
 * spawned child's argv. */
bool cbm_build_win_cmdline(char *buf, size_t cap, const char *const *argv) {
    if (!buf || cap == 0 || !argv) {
        return false;
    }
    size_t pos = 0;
    bool ovf = false;
    for (int i = 0; argv[i]; i++) {
        pos = cbm_cmdline_append_arg(buf, cap, pos, argv[i], i == 0, &ovf);
        if (ovf) {
            buf[0] = '\0'; /* overflow: leave buf a valid (empty) string, never unterminated */
            return false;
        }
    }
    buf[pos] = '\0';
    return true;
}

#ifdef _WIN32

static int cbm_run_win(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;

    char cmdline[8192];
    if (!cbm_build_win_cmdline(cmdline, sizeof(cmdline), argv)) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    /* Spawn via CreateProcessW with a WIDE command line. CreateProcessA would
     * re-interpret our UTF-8 cmdline bytes through the ANSI code page (CP_ACP),
     * re-mangling a non-ASCII repo path at the parent->worker boundary — so the
     * worker's own wide-argv read could never recover it (#423/#20). */
    wchar_t *wcmd = cbm_utf8_to_wide(cmdline);
    if (!wcmd) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    HANDLE hlog = INVALID_HANDLE_VALUE;
    STARTUPINFOW si = {.cb = sizeof(si)};
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    if (opts->log_file) {
        wchar_t *wlog = cbm_utf8_to_wide(opts->log_file);
        if (wlog) {
            hlog = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
            free(wlog);
        }
    }
    if (hlog == INVALID_HANDLE_VALUE) {
        hlog = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hlog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdError = hlog;
        si.hStdOutput = hlog;
    }

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(wcmd);
    if (hlog != INVALID_HANDLE_VALUE) {
        CloseHandle(hlog);
    }
    if (!ok) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    long tail_pos = 0;
    uint64_t started_at = cbm_now_ms();
    uint64_t last_activity = started_at;
    bool timed_out = false;
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (cbm_tail_log(opts->log_file, &tail_pos, opts->on_log_line, opts->log_ud)) {
            last_activity = cbm_now_ms();
        }
        if (w == WAIT_OBJECT_0) {
            break;
        }
        uint64_t now = cbm_now_ms();
        bool quiet_expired =
            opts->quiet_timeout_ms > 0 && (now - last_activity) >= (uint64_t)opts->quiet_timeout_ms;
        bool total_expired =
            opts->total_timeout_ms > 0 && (now - started_at) >= (uint64_t)opts->total_timeout_ms;
        if (quiet_expired || total_expired) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            timed_out = true;
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (opts->log_file && opts->delete_log_on_exit) {
        (void)cbm_unlink(opts->log_file);
    }

    out->exit_code = (int)code;
    out->term_signal = 0;
    out->outcome = cbm_proc_classify(true, (int)code, 0, timed_out);
    return 0;
}

#else /* POSIX */

/* Transient spawn-failure retry (see the EAGAIN note in cbm_posix_spawn_apple):
 * long enough to ride out a burst of process creation, short enough that a
 * genuinely exhausted system still fails fast. The budget below is the single
 * source of truth for both — see cbm_spawn_backoff for the resulting waits.
 *
 * A sanitized build needs a wider window than an ordinary one, and only a
 * sanitized one does.
 *
 * The exponential backoff (10/20/40/80/160/320ms, ~0.6s) fixed the ordinary
 * case. Spawns then kept failing under ThreadSanitizer on CI — twice on one
 * SHA, on a change that could not have caused it. A sanitizer runs several
 * times slower and holds far more process state, so the pressure window it
 * creates is simply longer than 0.6s.
 *
 * Raising the budget for everyone would be the wrong fix: an unsanitized
 * machine that is genuinely out of capacity should still fail fast rather than
 * hang for seconds. So the extra patience is scoped to the builds that need it.
 * Three more doublings take the sanitized ceiling to roughly 5s.
 *
 * CBM_SPAWN_SANITIZED is 1 when any sanitizer is active. The build system may
 * say so with CBM_SANITIZED_BUILD; failing that we ask the compiler, which is
 * what makes this correct even in a tree that never defines it. GCC defines
 * __SANITIZE_ADDRESS__/__SANITIZE_THREAD__; older clang exposes the fact only
 * through __has_feature. Toolchains without __has_feature (and cppcheck's
 * preprocessor, which refuses to evaluate an unknown function-like macro) get
 * the usual always-false stub, so the query below stays one flat expression. */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(CBM_SANITIZED_BUILD) || defined(__SANITIZE_ADDRESS__) ||       \
    defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__) ||        \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define CBM_SPAWN_SANITIZED 1
#else
#define CBM_SPAWN_SANITIZED 0
#endif

#if CBM_SPAWN_SANITIZED
enum { CBM_SPAWN_RETRY = 2, CBM_SPAWN_RETRY_ATTEMPTS = 9 };
#else
enum { CBM_SPAWN_RETRY = 2, CBM_SPAWN_RETRY_ATTEMPTS = 6 };
#endif

/* Exponential backoff, doubling from CBM_SPAWN_BACKOFF_BASE_MS: 10, 20, 40, 80,
 * 160, 320 — ~630ms of total patience on an ordinary build, and three further
 * doublings (640, 1280, 2560) to roughly 5s on a sanitized one. The waits follow
 * from CBM_SPAWN_RETRY_ATTEMPTS above rather than being listed separately here,
 * so changing the budget cannot leave this description behind.
 *
 * The first version waited a flat 3 x 10ms, which was enough for a momentary
 * dip and NOT enough for the real thing: a CI runner building and testing in
 * parallel stays process-starved for hundreds of milliseconds at a stretch. A
 * fixed short delay samples the same congested instant repeatedly; doubling
 * walks out of it.
 *
 * The ceiling is deliberate. ~0.6s is invisible next to spawning a process that
 * does real work, and a machine still refusing after that is genuinely out of
 * capacity — at which point failing IS the correct answer, and failing fast
 * beats hanging. The sanitized ceiling is ~5s for the same reason in reverse:
 * under instrumentation the starved window really does last that long, and only
 * a build that already accepts a large slowdown pays for the extra wait. */
enum { CBM_SPAWN_BACKOFF_BASE_MS = 10, CBM_SPAWN_MS_PER_SEC = 1000, CBM_SPAWN_NS_PER_MS = 1000000 };

#ifdef CBM_ENABLE_TEST_SEAMS
/* Deterministic EAGAIN injection: see the header. Counts DOWN, so a test asks
 * for N simulated refusals and the (N+1)th attempt proceeds for real.
 * thread_local, not a plain static: a spawn on a worker thread must not consume
 * a refusal the test armed for itself, and the counter then needs no lock. */
static thread_local int g_force_spawn_eagain = 0;

void cbm_subprocess_force_spawn_eagain_for_testing(int attempts) {
    g_force_spawn_eagain = attempts > 0 ? attempts : 0;
}

int cbm_subprocess_pending_spawn_eagain_for_testing(void) {
    return g_force_spawn_eagain;
}

static bool cbm_spawn_eagain_injected(void) {
    if (g_force_spawn_eagain > 0) {
        g_force_spawn_eagain--;
        return true;
    }
    return false;
}
#endif

static long cbm_spawn_backoff_ms(int attempt) {
    /* Cap the shift at the budget, not at a constant: with the sanitized budget
     * of 9 a hard-coded 6 would flatten the last three waits to 320ms each
     * instead of continuing to double. The budget is the only bound needed —
     * callers never exceed it, and a second clamp would be dead code. */
    int shift = attempt < CBM_SPAWN_RETRY_ATTEMPTS ? attempt : CBM_SPAWN_RETRY_ATTEMPTS;
    return static_cast<long>(CBM_SPAWN_BACKOFF_BASE_MS) << shift;
}

static void cbm_spawn_backoff(int attempt) {
    long ms = cbm_spawn_backoff_ms(attempt);
    struct timespec delay = {static_cast<time_t>(ms / CBM_SPAWN_MS_PER_SEC),
                             static_cast<long>(ms % CBM_SPAWN_MS_PER_SEC) * CBM_SPAWN_NS_PER_MS};
    (void)cbm_nanosleep(&delay, nullptr);
}

/* Wait out this attempt's backoff, but only if the caller's budget can still
 * pay for BOTH the wait and the spawn that follows it. Returns false when it
 * cannot — the caller must then stop retrying.
 *
 * The ladder runs BEFORE the child exists, so it sits outside every deadline
 * the caller set unless it is checked here: cbm_index_spawn_worker passes the
 * REMAINING budget, and a ladder that ignores it starts a worker after the
 * deadline has already passed.
 *
 * Checking only on entry to the loop is not enough, which is the whole reason
 * this returns a bool instead of being a plain sleep: with a 25ms budget the
 * entry check passes at 0ms, the code sleeps 10ms, passes again at 10ms, sleeps
 * 20ms, and spawns at 30ms — after the deadline. The wait itself has to be
 * priced in, so a backoff that would outlast the budget is not taken at all. */
static bool cbm_spawn_backoff_within_budget(int attempt, const cbm_proc_opts_t *opts,
                                            uint64_t started_at) {
    if (opts && opts->total_timeout_ms > 0) {
        uint64_t elapsed = cbm_now_ms() - started_at;
        if (elapsed >= (uint64_t)opts->total_timeout_ms) {
            return false;
        }
        uint64_t remaining = (uint64_t)opts->total_timeout_ms - elapsed;
        if ((uint64_t)cbm_spawn_backoff_ms(attempt) >= remaining) {
            return false; /* the wait alone would spend what is left */
        }
    }
    cbm_spawn_backoff(attempt);
    return true;
}

/* fork() fails with EAGAIN under the same pressure posix_spawn does, and the
 * fallback path must not be less robust than the primary one. */
static pid_t cbm_fork_with_retry(const cbm_proc_opts_t *opts, uint64_t started_at) {
    /* CBM_SPAWN_RETRY_ATTEMPTS backoffs means ATTEMPTS+1 tries. Every try goes
     * through the same branch — including the last — so the injection seam
     * models production exactly rather than leaving a final unguarded fork() the
     * tests could never reach. */
    for (int attempt = 0;; attempt++) {
#ifdef CBM_ENABLE_TEST_SEAMS
        if (cbm_spawn_eagain_injected()) {
            if (attempt >= CBM_SPAWN_RETRY_ATTEMPTS) {
                errno = EAGAIN;
                return -1;
            }
            if (!cbm_spawn_backoff_within_budget(attempt, opts, started_at)) {
                errno = EAGAIN;
                return -1;
            }
            continue;
        }
#endif
        pid_t pid = fork();
        if (pid >= 0 || (errno != EAGAIN && errno != ENOMEM)) {
            return pid;
        }
        if (attempt >= CBM_SPAWN_RETRY_ATTEMPTS ||
            !cbm_spawn_backoff_within_budget(attempt, opts, started_at)) {
            errno = EAGAIN;
            return -1;
        }
    }
}

/* fork+exec child setup. On Apple this runs ONLY for the exec-failure fallback
 * (see cbm_posix_spawn_apple), which preserves the documented "unusable binary
 * => child exits 127" contract across platforms. */
static void cbm_posix_child_exec(const cbm_proc_opts_t *opts) {
    /* Child: redirect stdout+stderr to the log (or discard), then exec.
     * Use open()+dup2() (async-signal-safe, no malloc) rather than freopen():
     * the parent may be multithreaded (the MCP server holds worker/watcher/http
     * threads plus mimalloc/sqlite global state), and a fork() copies
     * only the calling thread — a malloc between fork and exec could deadlock on
     * a lock another thread held at fork time. open/dup2/execv touch no heap. */
    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;
    const char *target = opts->log_file ? opts->log_file : "/dev/null";
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 && opts->log_file) {
        fd = open("/dev/null", O_WRONLY);
    }
    if (fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            (void)close(fd);
        }
    }
    execv(bin, (char *const *)argv);
    _exit(127); /* exec failed */
}

#ifdef __APPLE__
/* macOS: spawn instead of fork+exec.
 *
 * fork() duplicates the parent's whole address-space bookkeeping, and an
 * ASan-instrumented parent carries an enormous shadow mapping. Past a footprint
 * threshold the child is jetsam-killed BEFORE exec replaces the image, so the
 * call fails with the child already gone — a spawn defect that reads as the
 * launched tool dying (a fixture's `git init` "failing" only once enough suites
 * have run ahead of it). posix_spawn never copies the parent address space, so
 * the parent's footprint stops being a variable.
 *
 * Every guarantee of THIS fork path is carried over exactly:
 *   - stdout+stderr wired to the log (or /dev/null) via adddup2
 *   - stdin left inherited (this fork's child never rebinds it)
 *   - default signal dispositions and an empty mask (SETSIGDEF/SETSIGMASK)
 *   - exact-path exec: posix_spawn, not posix_spawnp, matches execv's semantics
 * The log descriptor is opened here rather than in the child (posix_spawn has
 * no child context to run code in); O_CLOEXEC keeps the spare copy out of the
 * exec'd image, while the two dup2'd descriptors survive because dup2 clears
 * close-on-exec.
 *
 * Returns 0 on success (*pid_out set), 1 for an exec-class failure the caller
 * should reproduce with fork+exec, CBM_SPAWN_RETRY when the kernel refused for
 * load, -1 for a hard spawn failure. */
static int cbm_posix_spawn_apple(const cbm_proc_opts_t *opts, pid_t *pid_out) {
    /* posix_spawn is the PRIMARY path on macOS; fork+exec is only the
     * exec-class fallback. Injection therefore has to live here too, or a test
     * on macOS exercises nothing. */
#ifdef CBM_ENABLE_TEST_SEAMS
    if (cbm_spawn_eagain_injected()) {
        return CBM_SPAWN_RETRY;
    }
#endif
    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;
    const char *target = opts->log_file ? opts->log_file : "/dev/null";
    int log_flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    log_flags |= O_CLOEXEC;
#endif
    int fd = open(target, log_flags, 0644);
    if (fd < 0 && opts->log_file) {
        fd = open("/dev/null", log_flags, 0644);
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return -1;
    }
    if (posix_spawnattr_init(&attr) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        if (fd >= 0) {
            (void)close(fd);
        }
        return -1;
    }
    sigset_t empty_mask;
    sigset_t all_signals;
    (void)sigemptyset(&empty_mask);
    (void)sigfillset(&all_signals);
    short flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    bool configured = posix_spawnattr_setflags(&attr, flags) == 0 &&
                      posix_spawnattr_setsigmask(&attr, &empty_mask) == 0 &&
                      posix_spawnattr_setsigdefault(&attr, &all_signals) == 0;
    if (configured && fd >= 0) {
        configured = posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO) == 0 &&
                     posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO) == 0;
    }
    pid_t pid = -1;
    int rc = configured
                 ? posix_spawn(&pid, bin, &actions, &attr, (char *const *)argv, *_NSGetEnviron())
                 : -1;
    (void)posix_spawn_file_actions_destroy(&actions);
    (void)posix_spawnattr_destroy(&attr);
    if (fd >= 0) {
        (void)close(fd);
    }
    if (configured && rc == 0 && pid > 0) {
        *pid_out = pid;
        return 0;
    }
    /* posix_spawn reports an unusable binary to the PARENT, where fork+exec
     * instead produces a child that exits 127. cbm_subprocess_run's contract
     * treats a -1 return as "the spawn MECHANISM failed", not "the tool was
     * missing", so exec-class errors fall back to fork+exec and macOS keeps
     * classifying a bogus binary exactly as Linux does. That child execs (or
     * _exits) immediately, so it does not reintroduce the jetsam hazard. */
    if (configured && (rc == ENOENT || rc == EACCES || rc == ENOEXEC || rc == EISDIR ||
                       rc == ELOOP || rc == ENAMETOOLONG || rc == ENOTDIR)) {
        return 1;
    }
    /* EAGAIN/ENOMEM are the kernel saying "not right now", not "never": the
     * process table or a per-user limit is momentarily full. Reporting
     * spawn_failed for that turns transient load into a user-visible error —
     * a git or LSP probe failing on a busy laptop for no reason the user can
     * see or act on. Retry briefly. Everything else stays a hard failure. */
    if (configured && (rc == EAGAIN || rc == ENOMEM)) {
        return CBM_SPAWN_RETRY;
    }
    return -1;
}
#endif

static int cbm_run_posix(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    pid_t pid = -1;
    /* The caller's total budget covers the WHOLE call, spawn retries included —
     * see cbm_spawn_backoff_within_budget. Start the clock here, not after the child
     * exists. */
    uint64_t started_at = cbm_now_ms();
#ifdef __APPLE__
    int spawn_rc = cbm_posix_spawn_apple(opts, &pid);
    for (int attempt = 0; spawn_rc == CBM_SPAWN_RETRY && attempt < CBM_SPAWN_RETRY_ATTEMPTS;
         attempt++) {
        if (!cbm_spawn_backoff_within_budget(attempt, opts, started_at)) {
            break;
        }
        spawn_rc = cbm_posix_spawn_apple(opts, &pid);
    }
    if (spawn_rc == CBM_SPAWN_RETRY) {
        spawn_rc = -1; /* still exhausted after backoff: a real failure */
    }
    if (spawn_rc < 0) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    if (spawn_rc > 0) { /* exec-class failure: reproduce the fork+exec 127 */
        pid = cbm_fork_with_retry(opts, started_at);
        if (pid < 0) {
            out->outcome = CBM_PROC_SPAWN_FAILED;
            out->exit_code = -1;
            out->term_signal = 0;
            return -1;
        }
        if (pid == 0) {
            cbm_posix_child_exec(opts);
        }
    }
#else
    pid = cbm_fork_with_retry(opts, started_at);
    if (pid < 0) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    if (pid == 0) {
        cbm_posix_child_exec(opts);
    }
#endif

    long tail_pos = 0;
    /* started_at was taken BEFORE the spawn, so the caller's total budget covers
     * the retry ladder too. last_activity is sampled fresh: time spent waiting
     * for the kernel to hand us a process is not idleness on the child's part
     * and must not count against the idle timeout. */
    uint64_t last_activity = cbm_now_ms();
    bool timed_out = false;
    long poll_delay_ns = 1000000L;
    int wstatus = 0;
    for (;;) {
        pid_t wr;
        do {
            wr = waitpid(pid, &wstatus, WNOHANG);
        } while (wr < 0 && errno == EINTR);
        bool done = (wr == pid);

        if (cbm_tail_log(opts->log_file, &tail_pos, opts->on_log_line, opts->log_ud)) {
            last_activity = cbm_now_ms();
        }
        if (done) {
            break;
        }
        uint64_t now = cbm_now_ms();
        bool quiet_expired =
            opts->quiet_timeout_ms > 0 && (now - last_activity) >= (uint64_t)opts->quiet_timeout_ms;
        bool total_expired =
            opts->total_timeout_ms > 0 && (now - started_at) >= (uint64_t)opts->total_timeout_ms;
        if (quiet_expired || total_expired) {
            kill(pid, SIGKILL);
            do {
                wr = waitpid(pid, &wstatus, 0);
            } while (wr < 0 && errno == EINTR);
            timed_out = true;
            break;
        }
        struct timespec ts = {0, poll_delay_ns};
        cbm_nanosleep(&ts, NULL);
        if (poll_delay_ns < 100000000L) {
            poll_delay_ns *= 2;
            if (poll_delay_ns > 100000000L) {
                poll_delay_ns = 100000000L;
            }
        }
    }

    if (opts->log_file && opts->delete_log_on_exit) {
        (void)unlink(opts->log_file);
    }

    if (WIFEXITED(wstatus)) {
        out->exit_code = WEXITSTATUS(wstatus);
        out->term_signal = 0;
        out->outcome = cbm_proc_classify(true, out->exit_code, 0, timed_out);
    } else if (WIFSIGNALED(wstatus)) {
        out->exit_code = -1;
        out->term_signal = WTERMSIG(wstatus);
        out->outcome = cbm_proc_classify(false, -1, out->term_signal, timed_out);
    } else {
        out->exit_code = -1;
        out->term_signal = 0;
        out->outcome = timed_out ? CBM_PROC_HANG : CBM_PROC_KILLED;
    }
    return 0;
}

#endif

int cbm_subprocess_run(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    cbm_proc_result_t local;
    if (!out) {
        out = &local;
    }
    out->outcome = CBM_PROC_SPAWN_FAILED;
    out->exit_code = -1;
    out->term_signal = 0;
    if (!opts || !opts->bin || !opts->bin[0]) {
        return -1;
    }
#ifdef _WIN32
    return cbm_run_win(opts, out);
#else
    return cbm_run_posix(opts, out);
#endif
}
