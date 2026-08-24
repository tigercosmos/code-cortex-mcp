/*
 * subprocess.h — spawn a child process, supervise it, and classify how it ended.
 *
 * Generalized from the crash-isolating index spawn (formerly in the UI's http_server) so the
 * crash/hang supervisor (Track C) can reuse one primitive across platforms.
 *
 * Beyond a plain spawn+wait it adds the two things a supervisor needs and the
 * ad-hoc harness lacked:
 *   1. Exit CLASSIFICATION — {clean, exit-nonzero, crash, hang, killed} — from
 *      POSIX WIFSIGNALED/WTERMSIG and the Windows NTSTATUS exception exit codes
 *      (0xC0000005 access-violation, 0xC00000FD stack-overflow, …).
 *   2. Quiet and total timeouts — kill + report HANG when the child makes no
 *      progress or exceeds a hard wall-clock deadline. The latter remains
 *      effective even when a stuck child keeps emitting log lines.
 *
 * The reap loop is EINTR-safe. Line tailing keeps a partial final line buffered
 * (an incomplete, un-newline-terminated line is not yet "progress" and is not
 * mis-read as a completed marker).
 */
#ifndef CBM_SUBPROCESS_H
#define CBM_SUBPROCESS_H

#include <stdbool.h>
#include <stddef.h> /* size_t (cbm_build_win_cmdline) */

#ifdef __cplusplus
extern "C" {
#endif

/* How a supervised child ended. */
typedef enum {
    CBM_PROC_CLEAN = 0,    /* exited with code 0 */
    CBM_PROC_EXIT_NONZERO, /* exited with a nonzero code (a graceful failure) */
    CBM_PROC_CRASH,        /* died from a fault: POSIX SIGSEGV/BUS/ILL/FPE/ABRT/SYS,
                            * or a Windows NTSTATUS exception exit code (>= 0xC0000000) */
    CBM_PROC_HANG,         /* made no progress within the quiet-timeout; we killed it */
    CBM_PROC_KILLED,       /* terminated by a non-fault signal we did not initiate */
    CBM_PROC_SPAWN_FAILED  /* fork/exec/CreateProcess failed — no child ever ran */
} cbm_proc_outcome_t;

typedef struct {
    cbm_proc_outcome_t outcome;
    int exit_code;   /* WEXITSTATUS / GetExitCodeProcess; -1 when terminated by a POSIX signal */
    int term_signal; /* WTERMSIG on POSIX; 0 otherwise */
} cbm_proc_result_t;

/* Called for each newly-completed (newline-terminated) log line while the child
 * runs. A completed line also resets the quiet-timeout (it is progress). */
typedef void (*cbm_proc_log_cb)(const char *line, void *ud);

typedef struct {
    const char *bin;             /* executable path; also argv[0] when argv is NULL */
    const char *const *argv;     /* NULL-terminated argv; NULL => { bin, NULL } */
    const char *log_file;        /* child stdout+stderr are redirected here and tailed;
                                  * NULL => discard child output, no tailing */
    cbm_proc_log_cb on_log_line; /* optional per-line callback */
    void *log_ud;                /* user data for on_log_line */
    int quiet_timeout_ms;        /* <= 0 => no timeout; else kill+HANG after this many
                                  * ms with no new completed log line */
    int total_timeout_ms;        /* <= 0 => no total cap; else kill+HANG after this many
                                  * ms regardless of log activity */
    bool delete_log_on_exit;     /* unlink log_file after reaping */
    int poll_cap_ms;             /* upper bound on the reap-loop sleep between waitpid
                                    probes; <= 0 => CBM_PROC_POLL_CAP_DEFAULT_MS. Short-lived
                                    children (tool workers) pass a small value so their exit
                                    is noticed within milliseconds; long-running ones (the
                                    index worker) keep the default so the parent does not
                                    spin for minutes. */
} cbm_proc_opts_t;

/* Default reap-loop sleep cap (see cbm_proc_opts_t.poll_cap_ms). */
#define CBM_PROC_POLL_CAP_DEFAULT_MS 100

/* Spawn opts->bin, supervise (tail + optional quiet/total timeouts), block until
 * it ends, and classify the result into *out. Returns 0 if a child was spawned
 * and reaped (out filled), or -1 if the spawn itself failed
 * (out->outcome == CBM_PROC_SPAWN_FAILED). */
int cbm_subprocess_run(const cbm_proc_opts_t *opts, cbm_proc_result_t *out);

/* ── Long-lived child with pipes (POSIX) ─────────────────────────────
 * cbm_subprocess_run is one-shot: spawn, wait, classify. A persistent worker
 * (the MCP tool server) needs the child to outlive many requests, so it is
 * spawned with its stdin/stdout connected to pipes and supervised by the
 * caller: write a request, wait for the reply with a deadline, and reap or
 * kill it when it misbehaves. stderr is inherited from the parent.
 *
 * Windows: not implemented — cbm_subprocess_spawn_piped returns -1 and the
 * caller falls back to the one-shot path. */
typedef struct {
    int pid;        /* child pid; -1 when not running */
    int to_child;   /* parent writes the child's stdin here; -1 when closed */
    int from_child; /* parent reads the child's stdout here; -1 when closed */
} cbm_proc_pipe_t;

/* Spawn opts->bin / opts->argv with piped stdin/stdout. Uses only bin, argv and
 * total_timeout_ms (the spawn-retry budget) from opts; log_file is ignored.
 * Returns 0 and fills *h, or -1 (h zeroed to the not-running state). The
 * parent-side pipe ends are close-on-exec so other children never inherit them. */
int cbm_subprocess_spawn_piped(const cbm_proc_opts_t *opts, cbm_proc_pipe_t *h);

/* Block until h->from_child has data or was closed by the child, or until
 * timeout_ms elapses. Returns 1 readable/closed, 0 timeout, -1 error. */
int cbm_subprocess_wait_readable(const cbm_proc_pipe_t *h, int timeout_ms);

/* Block until h->to_child can accept more bytes (or the child closed its end),
 * or until timeout_ms elapses. Returns 1 writable/closed, 0 timeout, -1 error.
 *
 * h->to_child is opened NON-BLOCKING, so a caller writing a request larger than
 * the pipe buffer (64KB on most systems) gets a short write and EAGAIN instead
 * of blocking forever against a child that has stopped reading. Pair every
 * partial write with this call and a deadline: a supervisor that can block
 * indefinitely on the write side has no hard deadline at all, whatever its
 * read-side timeout says. */
int cbm_subprocess_wait_writable(const cbm_proc_pipe_t *h, int timeout_ms);

/* Reap the child, sending SIGKILL first when `force` is set (or when it is
 * still running after `grace_ms` without force). Closes both pipe ends and
 * classifies the exit into *out (timed_out is reported as HANG when force).
 * Safe to call on an already not-running handle (out becomes CLEAN/0). */
int cbm_subprocess_reap(cbm_proc_pipe_t *h, bool force, int grace_ms, cbm_proc_result_t *out);

/* Non-blocking: if the child has already exited, reap + classify it into *out
 * and return 1 (pipes closed); 0 while it is still running; -1 on error. */
int cbm_subprocess_poll_exit(cbm_proc_pipe_t *h, cbm_proc_result_t *out);

/* Close both pipe ends without reaping (idempotent). */
void cbm_subprocess_pipe_close(cbm_proc_pipe_t *h);

/* Pure outcome classifier — exposed so the platform-specific exit-code mapping
 * (notably the Windows NTSTATUS crash codes) is unit-testable on every platform.
 *   exited_normally: the child returned an exit code (POSIX WIFEXITED; always true
 *                    on Windows, which has no signals — crashes surface as codes).
 *   exit_code:       the exit / exception code (meaningful when exited_normally).
 *   term_signal:     POSIX terminating signal (meaningful when !exited_normally).
 *   timed_out:       we killed the child for exceeding the quiet-timeout. */
cbm_proc_outcome_t cbm_proc_classify(bool exited_normally, int exit_code, int term_signal,
                                     bool timed_out);

/* Stable lowercase name for an outcome (for structured logs / skip reasons). */
const char *cbm_proc_outcome_str(cbm_proc_outcome_t o);

/* Build a Windows CreateProcess command line from a NULL-terminated argv, applying
 * the Microsoft C runtime quoting rules (quote-wrap + escape embedded quotes and
 * their preceding backslashes) so the spawned child re-parses byte-identical argv.
 * Returns true on success, false on overflow (on overflow buf is set to an empty
 * string, never left unterminated).
 *
 * CreateProcess re-parses a SINGLE command string into argv, so a naive `"%s"` wrap
 * silently corrupts any element containing a double-quote — e.g. the index worker's
 * JSON arg {"repo_path":"…"} arrives as {repo_path:…}, the Windows index-worker bug.
 * Exposed (and compiled on every platform — it is pure string logic) so the quoting
 * is unit-tested on Linux/macOS CI, and so every spawn site escapes through one
 * shared, tested implementation. */
bool cbm_build_win_cmdline(char *buf, size_t cap, const char *const *argv);

#ifdef CBM_ENABLE_TEST_SEAMS
/* Force the next N spawn attempts to behave as if the kernel returned EAGAIN
 * ("try again"), so the retry path can be exercised deterministically instead
 * of hoping a loaded machine reproduces it. The counter is per-thread and
 * counts DOWN: after N simulated refusals the next attempt spawns for real.
 * POSIX test builds only — compiled out entirely otherwise. */
void cbm_subprocess_force_spawn_eagain_for_testing(int attempts);
int cbm_subprocess_pending_spawn_eagain_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CBM_SUBPROCESS_H */
