/*
 * compat_fs.h — Portable directory iteration, popen, and file operations.
 *
 * POSIX: thin wrappers around opendir/readdir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#ifndef CBM_COMPAT_FS_H
#define CBM_COMPAT_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Directory iteration ──────────────────────────────────────── */

/* Max filename length (MAX_PATH on Windows, NAME_MAX on POSIX). */
#define CBM_DIRENT_NAME_MAX 260

typedef struct cbm_dir cbm_dir_t;

typedef struct {
    char name[CBM_DIRENT_NAME_MAX];
    bool is_dir;          /* a real directory: never a symlink, junction or other
                           * link to one, and resolved by lstat when the
                           * filesystem reports DT_UNKNOWN */
    unsigned char d_type; /* DT_REG, DT_DIR, DT_LNK, etc. (POSIX only, 0 on Windows) */
    /* A symbolic link (POSIX DT_LNK) or a reparse point (a Windows junction or
     * directory symbolic link; is_dir stays false for it). A walk that must
     * stay inside the repository never follows one. */
    bool is_link;
} cbm_dirent_t;

/* Open a directory for iteration. Returns NULL on error. */
cbm_dir_t *cbm_opendir(const char *path);

/* Read next entry. Returns NULL when done. The returned pointer is
 * valid until the next cbm_readdir call on the same handle. */
cbm_dirent_t *cbm_readdir(cbm_dir_t *d);

/* Close directory handle. */
void cbm_closedir(cbm_dir_t *d);

/* ── Portable popen/pclose ────────────────────────────────────── */

FILE *cbm_popen(const char *cmd, const char *mode);

/* Read-mode cbm_popen for a `git ...` command line: the child environment
 * omits git's repository-local variables (foundation/git_env.h), so an
 * inherited GIT_DIR/GIT_WORK_TREE/... can never override `git -C <repo>`.
 * Our own environment is untouched. Close with cbm_pclose. */
FILE *cbm_popen_git(const char *cmd);

/* Like cbm_popen_git, but with no shell at all: runs the git found by
 * cbm_find_git with `args` (NULL-terminated, NOT including argv[0]) as its
 * arguments, stdin and stderr on the null device. NULL when git is not found
 * or cannot be spawned. Close with cbm_pclose. */
FILE *cbm_popen_git_argv(const char *const *args);

/* Close a stream from cbm_popen / cbm_popen_git / cbm_popen_git_argv. Returns
 * the pclose()-style wait status on POSIX, the exit code on Windows. */
int cbm_pclose(FILE *f);

/* Absolute path of the git executable, searched in the absolute PATH entries
 * only (never the current directory). False when none is found. */
bool cbm_find_git(char *out, size_t out_sz);

/* ── File operations ──────────────────────────────────────────── */

/* Create directory (and parents). mode is ignored on Windows. Returns true on success. */
bool cbm_mkdir_p(const char *path, int mode);

/* Process-shared indexing lease on a canonical database target. Returns 0,
 * CBM_INDEX_BUSY (nonblocking contention), or -1 (fail closed). Creates parent
 * directories and a persistent <target>.index.lock; NEVER unlink that file.
 * Only cooperating binaries are excluded. Handles are not inherited by exec.
 * Existing hard-linked DBs are rejected; local OS advisory-lock semantics apply.
 * The caller owns the lease and its borrowed canonical target until release. */
typedef struct cbm_db_lease cbm_db_lease_t;
int cbm_db_lease_try_acquire(const char *db_path, cbm_db_lease_t **out);
const char *cbm_db_lease_target(const cbm_db_lease_t *lease);
void cbm_db_lease_release(cbm_db_lease_t *lease);

/* Reader gate of an existing database file: a second lock file,
 * <target>.read.lock, persistent like the lease's (NEVER unlink it). A
 * connection that serves the live database holds it shared for its lifetime;
 * whoever moves or deletes the file holds it exclusive across the move.
 * Exclusive never waits; shared waits up to about 2 s for a move in progress.
 * Returns 0, CBM_INDEX_BUSY (held the other way), or -1 (no such database, or
 * no lock file can be opened). The lock is on its own file, never the .db:
 * SQLite's POSIX locks there are per process, and closing any other
 * descriptor of the file would drop them. Release with cbm_db_lease_release. */
int cbm_db_read_gate_acquire(const char *db_path, bool exclusive, cbm_db_lease_t **out);

/* Non-mutating probes of the two locks, for previews: whether the index
 * lease of db_path is held (0 free, CBM_INDEX_BUSY held), and whether any
 * reader holds its gate (0 none, CBM_INDEX_BUSY some). Neither creates a
 * directory or a lock file: a lock file that does not exist is free, and
 * an existing one is tried exclusively and let go at once. -1: cannot
 * tell (the database path cannot be resolved). */
int cbm_db_lease_probe(const char *db_path);
int cbm_db_read_gate_probe(const char *db_path);

/* Delete a file. Returns 0 on success. */
int cbm_unlink(const char *path);
/* Remove <db_path>-wal/-shm. Any path installing a fresh DB generation must
 * do this before the new generation can be opened; a leftover WAL is
 * otherwise replayed on top of the new file (#897). */
void cbm_remove_db_sidecars(const char *db_path);
/* rename() that replaces an existing destination on every platform
 * (Windows rename fails with EEXIST; this uses write-through MoveFileExW). */
int cbm_rename_replace(const char *src, const char *dst);

/* Delete an empty directory. Returns 0 on success. */
int cbm_rmdir(const char *path);

/* Open a file by UTF-8 path.
 * On Windows, converts to wide-char and calls _wfopen so paths with
 * non-ASCII characters (accents, CJK, etc.) are handled correctly.
 * On POSIX, delegates to fopen. mode must be an ASCII string. */
FILE *cbm_fopen(const char *path, const char *mode);

/* Return true when a UTF-8 path names a regular file. */
bool cbm_is_regular_file(const char *path);

/* Return true when the path itself (not what it points to) is a link: a
 * symlink on POSIX (lstat), any reparse point on Windows -- a directory
 * junction, a directory or file symlink, a mount point. cbm_is_dir follows
 * links on every platform, so a walker that classifies a path it built
 * (rather than a cbm_readdir entry) must ask this before it descends. */
bool cbm_is_link(const char *path);

/* stat() by UTF-8 path; st_size and st_mtime (Unix seconds) as stat gives
 * them. Windows goes through _wstat64, so non-ASCII paths and paths past
 * MAX_PATH work there; POSIX is plain stat(). On Windows a path that cannot
 * be converted fails with errno ENOMEM/EINVAL, never ENOENT, so "absent"
 * stays distinct from "could not check". */
#ifdef _WIN32
typedef struct _stat64 cbm_stat_t;
int cbm_stat(const char *path, cbm_stat_t *st);
#else
typedef struct stat cbm_stat_t;
static inline int cbm_stat(const char *path, cbm_stat_t *st) {
    return stat(path, st);
}
#endif

/* Execute a command without shell interpretation.
 * argv is a NULL-terminated array: {"cmd", "arg1", "arg2", NULL}.
 * Returns the process exit code, or -1 on fork/exec failure.
 * POSIX: fork() + execvp(). Windows: CreateProcess with proper quoting. */
int cbm_exec_no_shell(const char *const *argv);

#ifdef __cplusplus
}
#endif

#endif /* CBM_COMPAT_FS_H */
