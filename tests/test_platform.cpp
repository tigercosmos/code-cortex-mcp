/*
 * test_platform.c — RED phase tests for foundation/platform.
 */
#include "test_framework.h"
#include "../src/foundation/compat.h" /* cbm_setenv / cbm_unsetenv (Windows-portable) */
#include "../src/foundation/compat_regex.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/dump_verify.h"
#include "../src/foundation/system_info_internal.h"
#include "../src/foundation/compat_fs.h"
#include "../src/store/store.h"
#include "test_helpers.h"
#include <pipeline/branch_slots.h> /* cbm_branch_root_key */
#include <stdlib.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/resource.h> /* getpriority for the worker-priority leg */
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath — deleted-self driver */
#endif

#ifdef __linux__
/* Linux-only cgroup tests need stdio for FILE*, stdlib for mkdtemp,
 * string for strncpy/strchr, sys/stat for mkdir, dirent for the
 * shell-free recursive teardown. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#endif

TEST(platform_now_ns) {
    uint64_t t1 = cbm_now_ns();
    ASSERT_GT(t1, 0);
    /* Busy-wait a tiny bit. The counter is plain and the volatile store lives in
     * the body: C++20 (P1152R4) deprecated both `++` on a volatile operand and
     * assignment to one, so neither belongs in the loop header. Initialising a
     * volatile is untouched by that, and still forces a store per iteration. */
    for (int i = 0; i < 100000; i++) {
        volatile int sink = i;
        (void)sink;
    }
    uint64_t t2 = cbm_now_ns();
    ASSERT_GT(t2, t1);
    PASS();
}

TEST(platform_now_ms) {
    uint64_t t1 = cbm_now_ms();
    ASSERT_GT(t1, 0);
    PASS();
}

TEST(platform_nprocs) {
    int n = cbm_nprocs();
    ASSERT_GT(n, 0);
    ASSERT_LT(n, 10000); /* sanity */
    PASS();
}

TEST(platform_file_exists) {
    /* This test file should exist */
    ASSERT_TRUE(cbm_file_exists("tests/test_platform.cpp"));
    ASSERT_FALSE(cbm_file_exists("nonexistent_file_xyz.txt"));
    PASS();
}

TEST(platform_is_dir) {
    ASSERT_TRUE(cbm_is_dir("tests"));
    ASSERT_FALSE(cbm_is_dir("tests/test_platform.cpp"));
    ASSERT_FALSE(cbm_is_dir("nonexistent_dir"));
    PASS();
}

TEST(platform_file_size) {
    int64_t sz = cbm_file_size("tests/test_platform.cpp");
    ASSERT_GT(sz, 0);
    ASSERT_EQ(cbm_file_size("nonexistent_file_xyz.txt"), -1);
    PASS();
}

TEST(platform_mmap) {
    /* mmap this test file and verify first bytes */
    size_t sz = 0;
    void *data = cbm_mmap_read("tests/test_platform.cpp", &sz);
    ASSERT_NOT_NULL(data);
    ASSERT_GT(sz, 0);
    /* First line should be the comment */
    ASSERT(memcmp(data, "/*", 2) == 0);
    cbm_munmap(data, sz);
    PASS();
}

TEST(platform_mmap_nonexistent) {
    size_t sz = 0;
    void *data = cbm_mmap_read("nonexistent_xyz.txt", &sz);
    ASSERT_NULL(data);
    PASS();
}

/*
 * CBM_WORKERS env override for cbm_default_worker_count.
 *
 * Containers running cbm on a host with more CPUs than the cgroup's
 * effective quota currently see ~host_cpu workers spawned because
 * sysconf(_SC_NPROCESSORS_ONLN) is not cgroup-aware (see GitHub
 * issue for the cgroup-detection ask). CBM_WORKERS is the smaller,
 * explicit-override path that ships independently.
 */
TEST(platform_default_workers_env_override) {
    cbm_setenv("CBM_WORKERS", "4", 1);
    int n = cbm_default_worker_count(true);
    ASSERT_EQ(n, 4);
    /* initial=false should also honor the explicit override. */
    int m = cbm_default_worker_count(false);
    ASSERT_EQ(m, 4);
    cbm_unsetenv("CBM_WORKERS");
    PASS();
}

TEST(platform_default_workers_env_invalid) {
    /* Out-of-range values (< 1 or > 256) and non-numeric strings
     * fall back to the sysconf-derived default. */
    int baseline = cbm_default_worker_count(true);
    ASSERT_GT(baseline, 0);

    cbm_setenv("CBM_WORKERS", "0", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "-1", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "9999", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "not-a-number", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_unsetenv("CBM_WORKERS");
    PASS();
}

TEST(platform_default_workers_env_unset) {
    /* When CBM_WORKERS is unset the result matches today's behaviour
     * (info.total_cores for initial=true, perf_cores-1 for false). */
    cbm_unsetenv("CBM_WORKERS");
    cbm_system_info_t info = cbm_system_info();
    ASSERT_EQ(cbm_default_worker_count(true), info.total_cores);
    PASS();
}

/* ── cgroup-aware detection (Linux only) ─────────────────────────── */

#ifdef __linux__

/* Create a unique tmp directory the caller will own; returns 0 on success. */
static int cgroup_test_setup(char *root, size_t root_sz) {
    strncpy(root, "/tmp/cbm_cgroup_test_XXXXXX", root_sz);
    return mkdtemp(root) != NULL ? 0 : -1;
}

/* Write `content` to "<root>/<relpath>". Creates parent subdir if needed.
 * Returns 0 on success, -1 on any failure. */
static int cgroup_test_write(const char *root, const char *relpath, const char *content) {
    char path[1024];
    const char *slash = strchr(relpath, '/');
    if (slash != NULL) {
        char subdir[1024];
        size_t n = (size_t)(slash - relpath);
        if (n >= sizeof(subdir)) {
            return -1;
        }
        memcpy(subdir, relpath, n);
        subdir[n] = '\0';
        snprintf(path, sizeof(path), "%s/%s", root, subdir);
        (void)mkdir(path, S_IRWXU);
    }
    snprintf(path, sizeof(path), "%s/%s", root, relpath);
    FILE *fp = fopen(path, "we");
    if (fp == NULL) {
        return -1;
    }
    size_t n = strlen(content);
    int rc = (fwrite(content, 1, n, fp) == n) ? 0 : -1;
    fclose(fp);
    return rc;
}

/* Recursively remove a tmp dir created by cgroup_test_setup. Best-effort.
 * Uses opendir/unlink/rmdir rather than system("rm -rf ...") to avoid
 * spawning a shell from the test binary. */
static void cgroup_test_teardown(const char *root) {
    DIR *d = opendir(root);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", root, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                cgroup_test_teardown(child); /* recurse into subdir */
            } else {
                (void)unlink(child);
            }
        }
        closedir(d);
    }
    (void)rmdir(root);
}

TEST(cgroup_v2_cpu_quota) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 200ms quota in a 100ms period → 2 effective CPUs. */
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "200000 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_cpu_quota_rounds_up) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 150ms quota / 100ms period = 1.5 → ceil = 2. */
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "150000 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_cpu_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "max 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_cpu_quota) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_quota_us", "200000"), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_period_us", "100000"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_cpu_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* quota=-1 is the cgroup-v1 sentinel for "no quota". */
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_quota_us", "-1"), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_period_us", "100000"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_no_cpu_files) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* Empty tmp dir: no v2 file, no v1 file → fall through to sysconf. */
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_mem) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 2 GiB. */
    ASSERT_EQ(cgroup_test_write(root, "memory.max", "2147483648\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)2147483648UL);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_mem_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "memory.max", "max\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_mem) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 1 GiB. */
    ASSERT_EQ(cgroup_test_write(root, "memory/memory.limit_in_bytes", "1073741824"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)1073741824UL);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_mem_unlimited_sentinel) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* cgroup v1 reports a huge near-ULLONG_MAX value when unlimited
     * (PAGE_COUNTER_MAX). Our parser treats anything >= ULLONG_MAX/2
     * as effectively unlimited. */
    ASSERT_EQ(cgroup_test_write(root, "memory/memory.limit_in_bytes", "9223372036854775807"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_no_mem_files) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

#endif /* __linux__ */

#if defined(__linux__) || defined(__APPLE__)
/* Byte copy — the driver needs an independent on-disk image of this binary it
 * can rename over without touching the build tree's artifact. */
static int ds_copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            rc = -1;
            break;
        }
    }
    if (n < 0) {
        rc = -1;
    }
    close(in);
    close(out);
    return rc;
}
#endif

/* #1204: an installer's atomic rename-over deletes the running image out from
 * under us. cbm_resolve_self_exe_path used to hand back the now-dead path,
 * which index_supervisor turned straight into a doomed worker spawn (ENOENT).
 * Drive the real state: exec a COPY of this binary as a probe, replace/remove
 * that copy while it runs, and check what the resolver answers. */
TEST(platform_resolve_self_exe_survives_deleted_image_issue1204) {
#if defined(__linux__) || defined(__APPLE__)
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_deleted_self_XXXXXX");
    ASSERT_NOT_NULL(cbm_mkdtemp(tmpdir));

    char current[1024];
#if defined(__linux__)
    ssize_t current_len = readlink("/proc/self/exe", current, sizeof(current) - 1);
    ASSERT_GT(current_len, 0);
    current[current_len] = '\0';
#else
    uint32_t current_sz = sizeof(current);
    ASSERT_EQ(_NSGetExecutablePath(current, &current_sz), 0);
#endif

    char launch_path[512];
    char replacement_path[512];
    snprintf(launch_path, sizeof(launch_path), "%s/test-runner", tmpdir);
    snprintf(replacement_path, sizeof(replacement_path), "%s/test-runner.new", tmpdir);
    ASSERT_EQ(ds_copy_file(current, launch_path), 0);
    ASSERT_EQ(chmod(launch_path, 0755), 0);

    int ready_pipe[2];
    int continue_pipe[2];
    ASSERT_EQ(pipe(ready_pipe), 0);
    ASSERT_EQ(pipe(continue_pipe), 0);

    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        close(ready_pipe[0]);
        close(continue_pipe[1]);
        char ready_fd[32];
        char continue_fd[32];
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
        snprintf(continue_fd, sizeof(continue_fd), "%d", continue_pipe[0]);
        execl(launch_path, launch_path, "__cbm_deleted_self_probe", ready_fd, continue_fd,
              (char *)NULL);
        _exit(127);
    }
    ASSERT_GT(child, 0);

    close(ready_pipe[1]);
    close(continue_pipe[0]);
    char ready = '\0';
    ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(ready, 'R');

#if defined(__linux__)
    /* Atomic rename-over: the installer's real move. /proc/self/exe now reads
     * "(deleted)" in the child, which is the state the magic-link branch
     * covers. */
    ASSERT_EQ(ds_copy_file(current, replacement_path), 0);
    ASSERT_EQ(chmod(replacement_path, 0755), 0);
    ASSERT_EQ(rename(replacement_path, launch_path), 0);
#else
    /* macOS: a rename-over leaves a VALID executable at the launch path, which
     * the resolver is right to return (the worker's build-fingerprint gate is
     * the layer that refuses a mismatched build). The fail-closed branch
     * guards the image-GONE case, so remove the image. */
    (void)replacement_path;
    ASSERT_EQ(unlink(launch_path), 0);
#endif
    ASSERT_EQ(write(continue_pipe[1], "G", 1), 1);
    close(ready_pipe[0]);
    close(continue_pipe[1]);

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    unlink(launch_path);
    rmdir(tmpdir);
    PASS();
#else
    SKIP_PLATFORM("deleted-self image probe needs POSIX fork/exec");
#endif
}

/* atoi/atol answer 0 for text they cannot read, and 0 is a real setting at
 * every caller. cbm_env_long answers true only for a clean whole number. */
TEST(platform_env_long_refuses_unreadable_values) {
    const char *name = "CBM_TEST_ENV_LONG_PROBE";
    long v = 7;
    cbm_unsetenv(name);
    ASSERT_FALSE(cbm_env_long(name, &v));
    ASSERT_EQ(v, 7);
    const char *unreadable[] = {"", "abc", "30s", " 5", "5 ", "99999999999999999999999"};
    for (size_t i = 0; i < sizeof(unreadable) / sizeof(unreadable[0]); i++) {
        cbm_setenv(name, unreadable[i], 1);
        v = 7;
        ASSERT_FALSE(cbm_env_long(name, &v));
        ASSERT_EQ(v, 7); /* untouched */
    }
    cbm_setenv(name, "0", 1);
    ASSERT_TRUE(cbm_env_long(name, &v));
    ASSERT_EQ(v, 0);
    cbm_setenv(name, "-12", 1);
    ASSERT_TRUE(cbm_env_long(name, &v));
    ASSERT_EQ(v, -12);
    cbm_setenv(name, "30", 1);
    ASSERT_TRUE(cbm_env_long(name, &v));
    ASSERT_EQ(v, 30);
    ASSERT_FALSE(cbm_env_long(name, NULL));
    cbm_unsetenv(name);
    PASS();
}

/* A sparse repository skips the ratio gate, but losing every committed node
 * is degraded at any positive size (upstream ffc29f73). */
TEST(platform_dump_verify_sparse_zero_persisted) {
    ASSERT_TRUE(cbm_dump_verify_is_degraded(12, 0, 0.5, CBM_DUMP_VERIFY_MIN_FLOOR));
    ASSERT_FALSE(cbm_dump_verify_is_degraded(12, 11, 0.5, CBM_DUMP_VERIFY_MIN_FLOOR));
    ASSERT_FALSE(cbm_dump_verify_is_degraded(12, 0, 0.0, CBM_DUMP_VERIFY_MIN_FLOOR));
    ASSERT_FALSE(cbm_dump_verify_is_degraded(0, 0, 0.5, CBM_DUMP_VERIFY_MIN_FLOOR));
    PASS();
}

/* ── compat_regex: compile-size guard ──────────────────────────── */

static uint64_t regex_units_ere(const char *pattern) {
    return cbm_regcomp_estimate_units(pattern, CBM_REG_EXTENDED);
}

static uint64_t regex_units_bre(const char *pattern) {
    return cbm_regcomp_estimate_units(pattern, 0);
}

/* Writes "(a|b|c|...)" with `branches` one-letter alternatives into buf, which
 * must hold 2 * branches + 2 bytes. */
static void regex_one_letter_alternation(char *buf, int branches) {
    char *w = buf;
    *w++ = '(';
    for (int i = 0; i < branches; i++) {
        if (i) {
            *w++ = '|';
        }
        *w++ = (char)('a' + i % 26);
    }
    *w++ = ')';
    *w = '\0';
}

/* Both regex backends expand `X{m,n}` into n copies of X at compile time, so
 * nested intervals multiply and a 25-byte pattern can ask for millions of
 * atoms. The wrapper sizes the pattern first and refuses one over the budget
 * with CBM_REG_ETOOBIG before the platform compiler allocates anything, so
 * there is nothing to free on refusal. The over-budget shapes here are small
 * enough that an unguarded compile would still only cost tens of MB. */
TEST(regex_compile_refuses_oversized_expansion) {
    cbm_regex_t re;
    /* 14^4 = 38,416 expanded atoms from 25 bytes: over the 32,768-unit budget. */
    const char *nested = "((((a){14}){14}){14}){14}";
    ASSERT_EQ(cbm_regcomp(&re, nested, CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_ETOOBIG);
    /* Every flag set the call sites use is refused the same way. */
    ASSERT_EQ(cbm_regcomp(&re, nested, CBM_REG_EXTENDED), CBM_REG_ETOOBIG);
    ASSERT_EQ(cbm_regcomp(&re, nested, CBM_REG_EXTENDED | CBM_REG_NOSUB | CBM_REG_ICASE),
              CBM_REG_ETOOBIG);
    /* A repeated alternation multiplies too: (16 + 16*16/8) x 255 x 3 = 36,720. */
    ASSERT_EQ(cbm_regcomp(&re, "((a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p){255}){3}",
                          CBM_REG_EXTENDED | CBM_REG_NOSUB),
              CBM_REG_ETOOBIG);
    /* A plain alternation just over the budget: 509 one-letter branches cost
     * 509 + 509*509/8 = 32,894 units; 508 cost 32,766 and stay accepted. */
    char alts[2 * 509 + 2];
    regex_one_letter_alternation(alts, 509);
    ASSERT_EQ(regex_units_ere(alts), 32894);
    ASSERT_EQ(cbm_regcomp(&re, alts, CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_ETOOBIG);
    regex_one_letter_alternation(alts, 508);
    ASSERT_EQ(regex_units_ere(alts), 32766);
    ASSERT_LTE(regex_units_ere(alts), (uint64_t)CBM_REGEX_COMPILE_BUDGET_UNITS);
    /* Bounds too large to represent saturate instead of wrapping around. */
    ASSERT_EQ(cbm_regcomp(&re, "(a{99999999999999999999}){99999999999999999999}",
                          CBM_REG_EXTENDED | CBM_REG_NOSUB),
              CBM_REG_ETOOBIG);
    /* Nesting deeper than the sizing pass tracks is refused, not recursed into. */
    char deep[2 * 70 + 2];
    memset(deep, '(', 70);
    deep[70] = 'a';
    memset(deep + 71, ')', 70);
    deep[141] = '\0';
    ASSERT_EQ(cbm_regcomp(&re, deep, CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_ETOOBIG);
    PASS();
}

/* The same nested shape one step under the budget (13^4 = 28,561 units)
 * compiles and matches, so the refusal starts only past the budget. */
TEST(regex_compile_accepts_expansion_under_budget) {
    cbm_regex_t re;
    ASSERT_EQ(regex_units_ere("((((a){13}){13}){13}){13}"), 28561);
    ASSERT_EQ(cbm_regcomp(&re, "((((a){13}){13}){13}){13}", CBM_REG_EXTENDED | CBM_REG_NOSUB),
              CBM_REG_OK);
    char *subject = (char *)malloc(28561 + 1);
    ASSERT_NOT_NULL(subject);
    memset(subject, 'a', 28561);
    subject[28561] = '\0';
    ASSERT_EQ(cbm_regexec(&re, subject, 0, NULL, 0), CBM_REG_OK);
    subject[28560] = '\0'; /* 28,560 a's: one short of 13^4 */
    ASSERT_EQ(cbm_regexec(&re, subject, 0, NULL, 0), CBM_REG_NOMATCH);
    free(subject);
    cbm_regfree(&re);
    PASS();
}

/* A pattern longer than CBM_REGEX_PATTERN_MAX_BYTES is refused outright; one
 * exactly at the cap passes the guard (a literal at the cap is exactly the
 * unit budget, which is accepted). */
TEST(regex_compile_refuses_overlong_pattern) {
    cbm_regex_t re;
    char *pattern = (char *)malloc(CBM_REGEX_PATTERN_MAX_BYTES + 2);
    ASSERT_NOT_NULL(pattern);
    memset(pattern, 'a', CBM_REGEX_PATTERN_MAX_BYTES + 1);
    pattern[CBM_REGEX_PATTERN_MAX_BYTES + 1] = '\0';
    ASSERT_EQ(cbm_regcomp(&re, pattern, CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_ETOOBIG);
    pattern[CBM_REGEX_PATTERN_MAX_BYTES] = '\0';
    /* At the cap the guard lets the pattern through. Whether the platform then
     * compiles it is the platform's own limit, which the guard does not
     * change: glibc does, macOS libc answers REG_ESPACE for a 32 KiB literal. */
    ASSERT_EQ(regex_units_ere(pattern), (uint64_t)CBM_REGEX_COMPILE_BUDGET_UNITS);
    int rc = cbm_regcomp(&re, pattern, CBM_REG_EXTENDED | CBM_REG_NOSUB);
    ASSERT_TRUE(rc != CBM_REG_ETOOBIG);
#ifdef __linux__
    ASSERT_EQ(rc, CBM_REG_OK);
#endif
    if (rc == CBM_REG_OK) {
        cbm_regfree(&re);
    }
    free(pattern);
    PASS();
}

/* Ordinary intervals, the pipeline's fixed patterns and patterns from the
 * tool-level tests stay well inside the budget. */
TEST(regex_compile_accepts_ordinary_intervals) {
    static const char *const patterns[] = {
        "[a-z]{1,64}",
        ".{0,255}",
        "([[:alnum:]_]{1,100}){1,20}",
        "(foo|bar|baz){2,5}",
        "^[A-Za-z_][A-Za-z0-9_]{0,255}$",
        "[0-9]{4}-[0-9]{2}-[0-9]{2}",
        /* pass_calls.cpp and pass_envscan.cpp */
        "Depends\\(([A-Za-z_][A-Za-z0-9_.]*)",
        "^(ENV|ARG)[[:space:]]+([A-Za-z0-9_]+)[= ](.*)",
        "(default|value)[[:space:]]*=[[:space:]]*\"(https?://[^\"]+)\"",
        "(export[[:space:]]+)?([A-Za-z0-9_]+)=[\"']?(https?://[^ \t\"']+)",
        /* test_mcp.cpp and test_cypher.cpp */
        ".*Order.*",
        "^src/",
        "^no_such_dir/",
        "/api/.*",
        "^design/",
    };
    cbm_regex_t re;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        int rc = cbm_regcomp(&re, patterns[i], CBM_REG_EXTENDED | CBM_REG_NOSUB);
        if (rc != CBM_REG_OK) {
            printf("  pattern %zu refused with rc=%d: %s\n", i, rc, patterns[i]);
        }
        ASSERT_EQ(rc, CBM_REG_OK);
        cbm_regfree(&re);
    }

    /* A long alternation of identifiers, as a search for "any of these names"
     * sends it: 400 names of up to nine characters is 3,761 bytes and about
     * 23,400 units (20,000 of them the alternation share). It stays under both
     * caps and compiles under the search_graph flag set. */
    static const char *const prefixes[] = {"parse", "emit", "visit", "bind", "scan"};
    char names[CBM_SZ_4K];
    char *w = names;
    *w++ = '(';
    for (int i = 0; i < 400; i++) {
        w += snprintf(w, (size_t)(names + sizeof(names) - w), "%s%s_%03d", i ? "|" : "",
                      prefixes[i % 5], i);
    }
    *w++ = ')';
    *w = '\0';
    ASSERT_LTE(strlen(names), (size_t)CBM_REGEX_PATTERN_MAX_BYTES);
    ASSERT_GTE(regex_units_ere(names), (uint64_t)20000);
    ASSERT_LTE(regex_units_ere(names), (uint64_t)CBM_REGEX_COMPILE_BUDGET_UNITS);
    ASSERT_EQ(cbm_regcomp(&re, names, CBM_REG_EXTENDED | CBM_REG_NOSUB | CBM_REG_ICASE),
              CBM_REG_OK);
    ASSERT_EQ(cbm_regexec(&re, "Bind_123", 0, NULL, 0), CBM_REG_OK);
    ASSERT_EQ(cbm_regexec(&re, "bind_400", 0, NULL, 0), CBM_REG_NOMATCH);
    cbm_regfree(&re);

    /* The same at realistic identifier length: 400 names of 20 characters is
     * 8,401 bytes and 28,000 units, inside the budget — the length cap never
     * decides a name list of this size, the unit budget does. */
    static const char *const long_prefixes[] = {"compute_checksum", "validate_request",
                                                "serialize_record", "normalize_header",
                                                "dispatch_message"};
    const size_t long_cap = 400 * 21 + 3;
    char *long_names = (char *)malloc(long_cap);
    ASSERT_NOT_NULL(long_names);
    w = long_names;
    *w++ = '(';
    for (int i = 0; i < 400; i++) {
        w += snprintf(w, long_cap - (size_t)(w - long_names), "%s%s_%03d", i ? "|" : "",
                      long_prefixes[i % 5], i);
    }
    *w++ = ')';
    *w = '\0';
    ASSERT_EQ(strlen(long_names), 8401);
    ASSERT_EQ(regex_units_ere(long_names), 28000);
    ASSERT_EQ(cbm_regcomp(&re, long_names, CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_OK);
    ASSERT_EQ(cbm_regexec(&re, "normalize_header_123", 0, NULL, 0), CBM_REG_OK);
    ASSERT_EQ(cbm_regexec(&re, "normalize_header_400", 0, NULL, 0), CBM_REG_NOMATCH);
    cbm_regfree(&re);
    free(long_names);

    /* `.{0,500}` is 500 units, inside the budget. Whether it compiles is the
     * platform's RE_DUP_MAX (255 on the TRE-derived libcs, 32767 on glibc),
     * which the guard does not change. */
    ASSERT_LTE(regex_units_ere(".{0,500}"), (uint64_t)CBM_REGEX_COMPILE_BUDGET_UNITS);
#ifdef __linux__
    ASSERT_EQ(cbm_regcomp(&re, ".{0,500}", CBM_REG_EXTENDED | CBM_REG_NOSUB), CBM_REG_OK);
    cbm_regfree(&re);
#endif
    PASS();
}

/* Sizing must read the syntax the way the compilers do: nothing inside a
 * bracket expression operates, a backslash escapes one character, braces that
 * do not form an interval are literals, and only an interval multiplies. */
TEST(regex_estimate_counts_brackets_and_escapes_literally) {
    ASSERT_EQ(regex_units_ere("[{]{2}"), 2);
    ASSERT_EQ(regex_units_ere("\\{"), 1);
    ASSERT_EQ(regex_units_ere("a\\{2\\}"), 4);
    ASSERT_EQ(regex_units_ere("[]a]{2}"), 2);
    ASSERT_EQ(regex_units_ere("[^]a]{3}"), 3);
    ASSERT_EQ(regex_units_ere("[[:alpha:]]{4}"), 4);
    ASSERT_EQ(regex_units_ere("[a-z{3}]"), 1);
    ASSERT_EQ(regex_units_ere("a{"), 2);
    ASSERT_EQ(regex_units_ere("a{2"), 3);
    ASSERT_EQ(regex_units_ere("a{x}"), 4);
    ASSERT_EQ(regex_units_ere("a{2}"), 2);
    ASSERT_EQ(regex_units_ere("a{2,}"), 2);
    ASSERT_EQ(regex_units_ere("a{,3}"), 3);
    ASSERT_EQ(regex_units_ere("a{2,5}"), 5);
    ASSERT_EQ(regex_units_ere("a*b+c?"), 3);
    ASSERT_EQ(regex_units_ere("a{3}{3}"), 9);
    ASSERT_EQ(regex_units_ere("(ab){3}"), 6);
    ASSERT_EQ(regex_units_ere("((ab){3}){4}"), 24);
    /* A K-way alternation is charged K*K/8 on top of its branches. */
    ASSERT_EQ(regex_units_ere("a|b|c|d"), 4 + 2);
    ASSERT_EQ(regex_units_ere("(a|b|c|d){10}"), 60);
    /* Basic syntax: bare braces and parens are literals; `\{ \}` and `\( \)`
     * operate. No call site uses it today, but the guard is shared. */
    ASSERT_EQ(regex_units_bre("a{2}"), 4);
    ASSERT_EQ(regex_units_bre("a\\{2\\}"), 2);
    ASSERT_EQ(regex_units_bre("\\(ab\\)\\{3\\}"), 6);
    ASSERT_EQ(regex_units_bre("(ab){3}"), 7);
    PASS();
}

/* A path past MAX_PATH (260) works through every file helper and through
 * SQLite. A branch slot named after a long branch is that long (about 340
 * characters under the cache), and on Windows the wide APIs refuse such a
 * path unless it is given in the extended-length form. Elsewhere the same
 * steps run as an ordinary check: each component stays under NAME_MAX. */
/* cbm_readdir calls an entry a directory only when it is one: a link to a
 * directory is a link, so a walker that deletes never descends through it.
 * (The DT_UNKNOWN and Windows-junction cases this guards cannot be staged on
 * a local POSIX filesystem; they share the code path checked here.) */
TEST(platform_readdir_reports_links_as_not_dirs) {
    char *made = th_mktempdir("cbm_readdir_kind");
    ASSERT_NOT_NULL(made);
    char dir[512];
    char file[512];
    snprintf(dir, sizeof(dir), "%s/real", made);
    snprintf(file, sizeof(file), "%s/plain", made);
    ASSERT_TRUE(cbm_mkdir_p(dir, 0755));
    FILE *f = cbm_fopen(file, "wb");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fclose(f), 0);
#ifndef _WIN32
    char lnk[512];
    snprintf(lnk, sizeof(lnk), "%s/linked", made);
    ASSERT_EQ(symlink(dir, lnk), 0);
    ASSERT_TRUE(cbm_is_dir(lnk)); /* follows: what the walkers must not trust */
#endif

    int seen_real = 0;
    int seen_plain = 0;
    int seen_linked = 0;
    cbm_dir_t *d = cbm_opendir(made);
    ASSERT_NOT_NULL(d);
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (strcmp(ent->name, "real") == 0) {
            seen_real++;
            ASSERT_TRUE(ent->is_dir);
        } else if (strcmp(ent->name, "plain") == 0) {
            seen_plain++;
            ASSERT_FALSE(ent->is_dir);
        } else if (strcmp(ent->name, "linked") == 0) {
            seen_linked++;
            ASSERT_FALSE(ent->is_dir);
        }
    }
    cbm_closedir(d);
    ASSERT_EQ(seen_real, 1);
    ASSERT_EQ(seen_plain, 1);
#ifndef _WIN32
    ASSERT_EQ(seen_linked, 1);
#endif
    th_rmtree(made);
    PASS();
}

TEST(platform_file_helpers_long_path) {
    char *made = th_mktempdir("cbm_longpath");
    ASSERT_NOT_NULL(made);
    char root[256];
    snprintf(root, sizeof(root), "%s", made);
    char d1[1024];
    char d2[1024];
    snprintf(d1, sizeof(d1), "%s/%0120d", root, 0);
    snprintf(d2, sizeof(d2), "%s/%0120d", d1, 1);
    ASSERT_GT(strlen(d2), 260);
    ASSERT_TRUE(cbm_mkdir_p(d2, 0755));
    ASSERT_TRUE(cbm_is_dir(d2));

    char file[1024];
    char moved[1024];
    snprintf(file, sizeof(file), "%s/%060d.bin", d2, 2);
    snprintf(moved, sizeof(moved), "%s/%060d.bin", d2, 3);
    FILE *f = cbm_fopen(file, "wb");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fwrite("hello", 1, 5, f), 5);
    ASSERT_EQ(fclose(f), 0);
    ASSERT_TRUE(cbm_file_exists(file));
    ASSERT_TRUE(cbm_is_regular_file(file));
    ASSERT_EQ(cbm_file_size(file), 5);
    cbm_stat_t st;
    ASSERT_EQ(cbm_stat(file, &st), 0);
    ASSERT_EQ((long long)st.st_size, 5);
    ASSERT_GT((long long)st.st_mtime, 0);
    ASSERT_EQ(cbm_rename_replace(file, moved), 0);
    ASSERT_FALSE(cbm_file_exists(file));
    ASSERT_TRUE(cbm_file_exists(moved));

    int seen = 0;
    cbm_dir_t *dir = cbm_opendir(d2);
    ASSERT_NOT_NULL(dir);
    for (cbm_dirent_t *e = cbm_readdir(dir); e; e = cbm_readdir(dir)) {
        seen += strstr(moved, e->name) != NULL;
    }
    cbm_closedir(dir);
    ASSERT_EQ(seen, 1);

    /* SQLite: open (create), dump through its .tmp and rename, reopen. */
    char db[1024];
    char dump[1024];
    snprintf(db, sizeof(db), "%s/%060d.db", d2, 4);
    snprintf(dump, sizeof(dump), "%s/%060d.db", d2, 5);
    cbm_store_t *s = cbm_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "longpath", "/x"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_dump_to_file(s, dump), CBM_STORE_OK);
    cbm_store_close(s);
    s = cbm_store_open_path_query(dump);
    ASSERT_NOT_NULL(s);
    cbm_store_close(s);

    const char *files[] = {moved, db, dump};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        (void)cbm_unlink(files[i]);
        cbm_remove_db_sidecars(files[i]);
        ASSERT_FALSE(cbm_file_exists(files[i]));
    }
    ASSERT_EQ(cbm_rmdir(d2), 0);
    ASSERT_FALSE(cbm_is_dir(d2));
    ASSERT_EQ(cbm_rmdir(d1), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    PASS();
}

/* Windows: two spellings of one directory that differ in a non-ASCII
 * letter's case are one checkout (the volume folds case by Unicode rules),
 * and a junction to it is the same checkout too (the final path resolves
 * it; the file identity backs that up). Same root, same slot key. */
TEST(platform_root_matches_windows_unicode_case_and_junction) {
#ifdef _WIN32
    char *base = th_mktempdir("cbm_plat_root");
    ASSERT_NOT_NULL(base);
    char upper[600];
    char lower[600];
    snprintf(upper, sizeof(upper), "%s/\xc3\x84-repo", base); /* Ä-repo */
    snprintf(lower, sizeof(lower), "%s/\xc3\xa4-repo", base); /* ä-repo */
    ASSERT_TRUE(cbm_mkdir_p(upper, 0755));
    ASSERT_TRUE(cbm_is_dir(lower)); /* the volume is case-insensitive */
    ASSERT_TRUE(cbm_root_matches(upper, lower));
    ASSERT_TRUE(cbm_root_matches(lower, upper));
    char ka[CBM_BRANCH_ROOT_KEY_LEN + 1];
    char kb[CBM_BRANCH_ROOT_KEY_LEN + 1];
    ASSERT_TRUE(cbm_branch_root_key(upper, ka, sizeof(ka)));
    ASSERT_TRUE(cbm_branch_root_key(lower, kb, sizeof(kb)));
    ASSERT_STR_EQ(ka, kb);
    /* A junction (no privilege needed, unlike a symlink) to an ASCII-named
     * directory: the mklink command line goes through system() in the ANSI
     * code page, which would garble a non-ASCII target. */
    char plain[600];
    snprintf(plain, sizeof(plain), "%s/plain-repo", base);
    ASSERT_TRUE(cbm_mkdir_p(plain, 0755));
    ASSERT_TRUE(cbm_branch_root_key(plain, ka, sizeof(ka)));
    char link[600];
    snprintf(link, sizeof(link), "%s/via-junction", base);
    char cmd[2048];
    char wl[600];
    char wu[600];
    snprintf(wl, sizeof(wl), "%s", link);
    snprintf(wu, sizeof(wu), "%s", plain);
    for (char *c = wl; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    for (char *c = wu; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", wl, wu);
    if (system(cmd) == 0 && cbm_is_link(link)) {
        ASSERT_TRUE(cbm_is_dir(link)); /* the target exists through it */
        ASSERT_TRUE(cbm_root_matches(plain, link));
        ASSERT_TRUE(cbm_root_matches(link, plain));
        ASSERT_FALSE(cbm_root_matches(upper, link));
        ASSERT_TRUE(cbm_branch_root_key(link, kb, sizeof(kb)));
        ASSERT_STR_EQ(ka, kb);
        (void)cbm_rmdir(link);
    }
    th_rmtree(base);
#endif
    PASS();
}

/* Windows: under a directory with NTFS per-directory case sensitivity,
 * "Repo" and "repo" are two directories, and so two checkouts: their file
 * identities decide, not the case-folded spelling. Skipped where the
 * attribute cannot be set (fsutil needs the feature, sometimes admin). */
TEST(platform_root_matches_windows_case_sensitive_dirs_differ) {
#ifdef _WIN32
    char *tmp = th_mktempdir("cbm_plat_cs");
    ASSERT_NOT_NULL(tmp);
    char base[300];
    snprintf(base, sizeof(base), "%s", tmp);
    char parent[400];
    snprintf(parent, sizeof(parent), "%s\\cs", base);
    ASSERT_TRUE(cbm_mkdir_p(parent, 0755));
    char wp[400];
    snprintf(wp, sizeof(wp), "%s", parent);
    for (char *c = wp; *c; c++) {
        if (*c == '/') {
            *c = '\\';
        }
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "fsutil file setCaseSensitiveInfo \"%s\" enable >nul 2>&1", wp);
    if (system(cmd) != 0) {
        th_rmtree(base);
        SKIP_PLATFORM("per-directory case sensitivity cannot be enabled here");
    }
    char upper[500];
    char lower[500];
    char marker[600];
    char other[600];
    snprintf(upper, sizeof(upper), "%s\\Repo", parent);
    snprintf(lower, sizeof(lower), "%s\\repo", parent);
    snprintf(marker, sizeof(marker), "%s\\marker", upper);
    snprintf(other, sizeof(other), "%s\\marker", lower);
    ASSERT_TRUE(cbm_mkdir_p(upper, 0755));
    ASSERT_TRUE(cbm_mkdir_p(lower, 0755));
    ASSERT_EQ(th_write_file(marker, "x"), 0);
    if (cbm_file_exists(other)) {
        th_rmtree(base);
        SKIP_PLATFORM("the directory still folds case");
    }
    ASSERT_FALSE(cbm_root_matches(upper, lower));
    ASSERT_FALSE(cbm_root_matches(lower, upper));
    ASSERT_TRUE(cbm_root_matches(upper, upper));
    th_rmtree(base);
#endif
    PASS();
}

/* A relative root keeps every unresolved "..": "../../repo" is not "repo"
 * (and not another checkout's identity), while a resolvable one still pops
 * and an absolute path's leading ".." still folds to the root. */
TEST(platform_root_normalize_keeps_unresolved_dotdot) {
    char out[256];
    /* Too small for a prefix and its terminator: refused, nothing written
     * past the buffer (a UNC prefix used to write its NUL at out[2]). */
    char tiny[4] = {'x', 'x', 'x', 'x'};
    ASSERT_FALSE(cbm_root_normalize("//server/share", false, tiny, 2));
    ASSERT_FALSE(cbm_root_normalize("//server/share", false, tiny, 3));
    ASSERT_EQ(tiny[2], 'x');
    ASSERT_EQ(tiny[3], 'x');
    ASSERT_TRUE(cbm_root_normalize("../../repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "../../repo");
    ASSERT_TRUE(cbm_root_normalize("a/../../repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "../repo");
    ASSERT_TRUE(cbm_root_normalize("../a/../repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "../repo");
    ASSERT_TRUE(cbm_root_normalize("/../../repo", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/repo");
    ASSERT_TRUE(cbm_root_normalize("/a/b/../../c", false, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/c");
    PASS();
}

/* A relative root that folds to nothing is the current directory, with a
 * real identity -- never the empty string every such checkout would share. */
TEST(platform_root_canonical_dot_is_cwd) {
    char out[CBM_ROOT_MAX];
    char cwd_out[CBM_ROOT_MAX];
    char cwd[CBM_ROOT_MAX];
    ASSERT_NOT_NULL(getcwd(cwd, sizeof(cwd)));
    ASSERT_TRUE(cbm_root_canonical(cwd, cwd_out, sizeof(cwd_out)));
    ASSERT_TRUE(cwd_out[0] != '\0');
    ASSERT_TRUE(cbm_root_canonical(".", out, sizeof(out)));
    ASSERT_STR_EQ(out, cwd_out);
    ASSERT_TRUE(cbm_root_canonical("sub/..", out, sizeof(out)));
    ASSERT_STR_EQ(out, cwd_out);
    char ka[CBM_BRANCH_ROOT_KEY_LEN + 1];
    char kb[CBM_BRANCH_ROOT_KEY_LEN + 1];
    ASSERT_TRUE(cbm_branch_root_key(".", ka, sizeof(ka)));
    ASSERT_TRUE(cbm_branch_root_key(cwd, kb, sizeof(kb)));
    ASSERT_STR_EQ(ka, kb);
    PASS();
}

/* A checkout's slot key is the same after the checkout is gone as while it
 * existed, so cleanup addresses its slots: through a symlinked ancestor (the
 * macOS /tmp case) and for a relative root. */
TEST(platform_root_key_survives_removed_checkout) {
#ifndef _WIN32
    char *base = th_mktempdir("cbm_plat_rkey");
    ASSERT_NOT_NULL(base);
    char real_dir[512];
    char link_dir[512];
    snprintf(real_dir, sizeof(real_dir), "%s/real", base);
    snprintf(link_dir, sizeof(link_dir), "%s/link", base);
    ASSERT_TRUE(cbm_mkdir_p(real_dir, 0755));
    ASSERT_EQ(symlink(real_dir, link_dir), 0);
    char root[600];
    snprintf(root, sizeof(root), "%s/repo", link_dir);
    ASSERT_TRUE(cbm_mkdir_p(root, 0755));
    char live[CBM_BRANCH_ROOT_KEY_LEN + 1];
    char gone[CBM_BRANCH_ROOT_KEY_LEN + 1];
    ASSERT_TRUE(cbm_branch_root_key(root, live, sizeof(live)));
    ASSERT_EQ(rmdir(root), 0);
    ASSERT_TRUE(cbm_branch_root_key(root, gone, sizeof(gone)));
    ASSERT_STR_EQ(live, gone);

    char old_cwd[1024];
    ASSERT_NOT_NULL(getcwd(old_cwd, sizeof(old_cwd)));
    ASSERT_EQ(chdir(link_dir), 0);
    ASSERT_TRUE(cbm_mkdir_p("rel-repo", 0755));
    ASSERT_TRUE(cbm_branch_root_key("rel-repo", live, sizeof(live)));
    ASSERT_EQ(rmdir("rel-repo"), 0);
    ASSERT_TRUE(cbm_branch_root_key("rel-repo", gone, sizeof(gone)));
    ASSERT_EQ(chdir(old_cwd), 0);
    ASSERT_STR_EQ(live, gone);

    /* ".." after a symlink is applied after following it, as the
     * filesystem does: base/real/hop -> base/other/sub, so
     * base/real/hop/../repo is base/other/repo, not base/real/repo. */
    char other_sub[600];
    char other_repo[600];
    char real_repo[600];
    char hop[600];
    snprintf(other_sub, sizeof(other_sub), "%s/other/sub", base);
    snprintf(other_repo, sizeof(other_repo), "%s/other/repo", base);
    snprintf(real_repo, sizeof(real_repo), "%s/repo", real_dir);
    snprintf(hop, sizeof(hop), "%s/hop", real_dir);
    ASSERT_TRUE(cbm_mkdir_p(other_sub, 0755));
    ASSERT_TRUE(cbm_mkdir_p(other_repo, 0755));
    ASSERT_TRUE(cbm_mkdir_p(real_repo, 0755));
    ASSERT_EQ(symlink(other_sub, hop), 0);
    char via[700];
    snprintf(via, sizeof(via), "%s/../repo", hop);
    ASSERT_TRUE(cbm_root_matches(via, other_repo));
    ASSERT_FALSE(cbm_root_matches(via, real_repo));
    /* The target gone, the other checkout still there: the vanished root
     * keeps ITS key, never real/repo's. */
    char k_via[CBM_BRANCH_ROOT_KEY_LEN + 1];
    char k_real[CBM_BRANCH_ROOT_KEY_LEN + 1];
    char k_other[CBM_BRANCH_ROOT_KEY_LEN + 1];
    ASSERT_TRUE(cbm_branch_root_key(other_repo, k_other, sizeof(k_other)));
    ASSERT_EQ(rmdir(other_repo), 0);
    ASSERT_TRUE(cbm_branch_root_key(via, k_via, sizeof(k_via)));
    ASSERT_TRUE(cbm_branch_root_key(real_repo, k_real, sizeof(k_real)));
    ASSERT_STR_EQ(k_via, k_other);
    ASSERT_TRUE(strcmp(k_via, k_real) != 0);
    ASSERT_FALSE(cbm_root_matches(via, real_repo));
    (void)unlink(hop);
    (void)unlink(link_dir);
    th_rmtree(base);
#endif
    PASS();
}

/* An index worker lowers its own priority (upstream #2834). A non-root process
 * cannot raise it back, so the helper runs in a forked child that reports its
 * nice. */
TEST(platform_lower_cpu_priority_nices_the_process) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX nice contract");
#else
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    pid_t child = fork();
    if (child == 0) {
        (void)close(fds[0]);
        /* report: lowered ok, nice after; then, from a nicer start, ok, target, nice after */
        int report[5] = {cbm_lower_cpu_priority() ? 1 : 0, 0, 0, 0, 0};
        errno = 0;
        report[1] = getpriority(PRIO_PROCESS, 0);
        if (errno != 0) {
            report[0] = 0;
        }
        /* Never-raise leg: start nicer than the helper's level; it must leave it. */
        report[3] = report[1] >= 15 ? 19 : 15;
        if (setpriority(PRIO_PROCESS, 0, report[3]) == 0) {
            report[2] = cbm_lower_cpu_priority() ? 1 : 0;
            errno = 0;
            report[4] = getpriority(PRIO_PROCESS, 0);
        }
        _exit(write(fds[1], report, sizeof(report)) == (ssize_t)sizeof(report) ? 0 : 1);
    }
    ASSERT_TRUE(child > 0);
    (void)close(fds[1]);
    int report[5] = {0, 0, 0, 0, 0};
    ssize_t got = read(fds[0], report, sizeof(report));
    (void)close(fds[0]);
    int status = 0;
    (void)waitpid(child, &status, 0);
    ASSERT_EQ(got, (ssize_t)sizeof(report));
    ASSERT_EQ(report[0], 1);
    ASSERT_TRUE(report[1] >= CBM_LOW_CPU_PRIORITY_NICE);
    ASSERT_EQ(report[2], 1);
    ASSERT_EQ(report[4], report[3]);
    PASS();
#endif
}

#ifdef _WIN32
/* cbm_safe_getenv reads Windows' wide environment as UTF-8. Its matching
 * setter must update that same wide environment; _putenv_s alone interprets
 * UTF-8 path bytes through the active ANSI code page (upstream 25546312). */
TEST(platform_setenv_preserves_utf8_in_wide_environment) {
    static const char utf8[] = "C:/cbm-cache-\xce\x94-\xe6\x97\xa5\xe6\x9c\xac-\xe4\xb8\x81";
    static const wchar_t wide[] = L"C:/cbm-cache-\u0394-\u65e5\u672c-\u4e01";
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", utf8, 1), 0);
    wchar_t observed_wide[128];
    ASSERT_EQ(GetEnvironmentVariableW(L"CBM_CACHE_DIR", observed_wide, 128), (DWORD)(wcslen(wide)));
    ASSERT_EQ(wcscmp(observed_wide, wide), 0);
    char observed_utf8[128];
    ASSERT_NOT_NULL(cbm_safe_getenv("CBM_CACHE_DIR", observed_utf8, sizeof(observed_utf8), NULL));
    ASSERT_STR_EQ(observed_utf8, utf8);
    /* Narrow getenv must agree with the CRT's active ANSI code page. */
    char expected_narrow[128];
    ASSERT_GT(WideCharToMultiByte(CP_ACP, 0, wide, -1, expected_narrow,
                                  (int)sizeof(expected_narrow), NULL, NULL),
              0);
    const char *observed_narrow = getenv("CBM_CACHE_DIR");
    ASSERT_NOT_NULL(observed_narrow);
    ASSERT_STR_EQ(observed_narrow, expected_narrow);
    (void)cbm_unsetenv("CBM_CACHE_DIR");
    PASS();
}

/* Empty and absent variables have different fallback semantics: an explicitly
 * empty CBM_CACHE_DIR means "use the default" and must not read as a failed
 * wide-environment read. Unset stays idempotent for cleanup paths. */
TEST(platform_windows_empty_environment_is_read_and_unset_idempotently) {
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", "", 1), 0);
    char observed[9] = "sentinel";
    ASSERT_NOT_NULL(cbm_safe_getenv("CBM_CACHE_DIR", observed, sizeof(observed), "fallback"));
    ASSERT_STR_EQ(observed, "");
    ASSERT_EQ(cbm_unsetenv("CBM_CACHE_DIR"), 0);
    ASSERT_EQ(cbm_unsetenv("CBM_CACHE_DIR"), 0);
    PASS();
}
#endif

SUITE(platform) {
#ifdef _WIN32
    RUN_TEST(platform_setenv_preserves_utf8_in_wide_environment);
    RUN_TEST(platform_windows_empty_environment_is_read_and_unset_idempotently);
#endif
    RUN_TEST(platform_lower_cpu_priority_nices_the_process);
    RUN_TEST(regex_compile_refuses_oversized_expansion);
    RUN_TEST(regex_compile_accepts_expansion_under_budget);
    RUN_TEST(regex_compile_refuses_overlong_pattern);
    RUN_TEST(regex_compile_accepts_ordinary_intervals);
    RUN_TEST(regex_estimate_counts_brackets_and_escapes_literally);
    RUN_TEST(platform_env_long_refuses_unreadable_values);
    RUN_TEST(platform_dump_verify_sparse_zero_persisted);
    RUN_TEST(platform_now_ns);
    RUN_TEST(platform_resolve_self_exe_survives_deleted_image_issue1204);
    RUN_TEST(platform_now_ms);
    RUN_TEST(platform_nprocs);
    RUN_TEST(platform_file_exists);
    RUN_TEST(platform_is_dir);
    RUN_TEST(platform_file_size);
    RUN_TEST(platform_mmap);
    RUN_TEST(platform_mmap_nonexistent);
    RUN_TEST(platform_readdir_reports_links_as_not_dirs);
    RUN_TEST(platform_root_matches_windows_unicode_case_and_junction);
    RUN_TEST(platform_root_matches_windows_case_sensitive_dirs_differ);
    RUN_TEST(platform_root_normalize_keeps_unresolved_dotdot);
    RUN_TEST(platform_root_canonical_dot_is_cwd);
    RUN_TEST(platform_root_key_survives_removed_checkout);
    RUN_TEST(platform_file_helpers_long_path);
    RUN_TEST(platform_default_workers_env_override);
    RUN_TEST(platform_default_workers_env_invalid);
    RUN_TEST(platform_default_workers_env_unset);
#ifdef __linux__
    RUN_TEST(cgroup_v2_cpu_quota);
    RUN_TEST(cgroup_v2_cpu_quota_rounds_up);
    RUN_TEST(cgroup_v2_cpu_unlimited);
    RUN_TEST(cgroup_v1_cpu_quota);
    RUN_TEST(cgroup_v1_cpu_unlimited);
    RUN_TEST(cgroup_no_cpu_files);
    RUN_TEST(cgroup_v2_mem);
    RUN_TEST(cgroup_v2_mem_unlimited);
    RUN_TEST(cgroup_v1_mem);
    RUN_TEST(cgroup_v1_mem_unlimited_sentinel);
    RUN_TEST(cgroup_no_mem_files);
#endif
}
