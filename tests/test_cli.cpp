/*
 * test_cli.c — Tests for CLI subcommands: install, uninstall, update, version.
 *
 * Port of Go test files:
 *   - cmd/code-cortex-mcp/cli_test.go (11 tests)
 *   - cmd/code-cortex-mcp/install_test.go (24 tests)
 *   - cmd/code-cortex-mcp/update_test.go (5 tests)
 *   - internal/selfupdate/selfupdate_test.go (7 tests)
 *
 * Total: 47 Go tests → 47 C tests
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <foundation/yaml.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <zlib.h>

/* Helper: create a file with content */
static int write_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

/* Helper: read a file into static buffer */
static const char *read_test_file(const char *path) {
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Helper: mkdirp */
static int test_mkdirp(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            cbm_mkdir(tmp);
            *p = '/';
        }
    }
    return cbm_mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
}

/* Helper: recursive remove */
static void test_rmdir_r(const char *path) {
    th_rmtree(path);
}

/* Helper: create tar.gz with a single file */
static unsigned char *create_test_targz(const char *filename, const unsigned char *content,
                                        int content_len, int *out_len) {
    /* Build tar data: 512-byte header + content padded to 512-byte boundary + 2x512 zero blocks */
    int data_blocks = (content_len + 511) / 512;
    int tar_size = 512 + data_blocks * 512 + 1024; /* header + data + end-of-archive */
    unsigned char *tar = (unsigned char *)calloc(1, (size_t)tar_size);
    if (!tar)
        return NULL;

    /* Filename (bytes 0-99) */
    strncpy((char *)tar, filename, 99);

    /* Mode (bytes 100-107): octal 0700 */
    memcpy(tar + 100, "0000700\0", 8);

    /* UID/GID (bytes 108-123): 0 */
    memcpy(tar + 108, "0000000\0", 8);
    memcpy(tar + 116, "0000000\0", 8);

    /* Size (bytes 124-135): octal */
    char size_str[12];
    snprintf(size_str, sizeof(size_str), "%011o", content_len);
    memcpy(tar + 124, size_str, 11);

    /* Mtime (bytes 136-147): 0 */
    memcpy(tar + 136, "00000000000\0", 12);

    /* Type flag (byte 156): '0' = regular file */
    tar[156] = '0';

    /* Checksum (bytes 148-155): compute over header with checksum field as spaces */
    memset(tar + 148, ' ', 8);
    unsigned int checksum = 0;
    for (int i = 0; i < 512; i++)
        checksum += tar[i];
    snprintf((char *)tar + 148, 7, "%06o", checksum);
    tar[154] = '\0';

    /* File content */
    memcpy(tar + 512, content, (size_t)content_len);

    /* Compress with gzip */
    z_stream strm = {0};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        free(tar);
        return NULL;
    }

    size_t gz_cap = (size_t)tar_size + 256;
    unsigned char *gz = (unsigned char *)malloc(gz_cap);
    if (!gz) {
        deflateEnd(&strm);
        free(tar);
        return NULL;
    }

    strm.next_in = tar;
    strm.avail_in = (unsigned int)tar_size;
    strm.next_out = gz;
    strm.avail_out = (unsigned int)gz_cap;

    deflate(&strm, Z_FINISH);
    *out_len = (int)(gz_cap - strm.avail_out);

    deflateEnd(&strm);
    free(tar);
    return gz;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Version comparison tests (port of selfupdate_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_compare_versions) {
    /* Port of TestCompareVersions — 13 cases */
    ASSERT(cbm_compare_versions("0.2.1", "0.2.0") > 0);
    ASSERT_EQ(cbm_compare_versions("0.2.0", "0.2.0"), 0);
    ASSERT(cbm_compare_versions("0.1.9", "0.2.0") < 0);
    ASSERT(cbm_compare_versions("0.10.0", "0.2.0") > 0);
    ASSERT(cbm_compare_versions("1.0.0", "0.99.99") > 0);
    ASSERT(cbm_compare_versions("0.0.1", "0.0.2") < 0);
    ASSERT_EQ(cbm_compare_versions("v0.2.1", "0.2.1"), 0);
    ASSERT_EQ(cbm_compare_versions("0.2.1", "v0.2.1"), 0);
    ASSERT(cbm_compare_versions("0.2.1-dev", "0.2.1") < 0);
    ASSERT(cbm_compare_versions("0.2.1", "0.2.1-dev") > 0);
    ASSERT_EQ(cbm_compare_versions("0.2.1-dev", "0.2.1-dev"), 0);
    ASSERT(cbm_compare_versions("0.3.0", "0.2.1-dev") > 0);
    ASSERT(cbm_compare_versions("0.2.0", "0.2.1-dev") < 0);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Version get/set (port of TestCLI_Version)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_version_get_set) {
    cbm_cli_set_version("1.2.3");
    ASSERT_STR_EQ(cbm_cli_get_version(), "1.2.3");
    cbm_cli_set_version("dev");
    ASSERT_STR_EQ(cbm_cli_get_version(), "dev");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shell RC detection (port of TestDetectShellRC + BashWithBashrc)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_detect_shell_rc_zsh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Save and override SHELL — must strdup because setenv may realloc env block */
    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/zsh", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_NOT_NULL(rc);
    ASSERT(strstr(rc, ".zshrc") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_bash) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/bash", 1);

    /* No .bashrc → falls back to .bash_profile */
    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_NOT_NULL(rc);
    ASSERT(strstr(rc, ".bash_profile") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_bash_with_bashrc) {
    /* Port of TestDetectShellRC_BashWithBashrc */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/bash", 1);

    /* Create .bashrc */
    char bashrc[512];
    snprintf(bashrc, sizeof(bashrc), "%s/.bashrc", tmpdir);
    write_test_file(bashrc, "# test\n");

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT_STR_EQ(rc, bashrc);

    unlink(bashrc);
    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_fish) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/usr/bin/fish", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT(strstr(rc, ".config/fish/config.fish") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

TEST(cli_detect_shell_rc_default) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-rc-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("SHELL");
    char *old_shell = raw ? strdup(raw) : NULL;
    cbm_setenv("SHELL", "/bin/sh", 1);

    const char *rc = cbm_detect_shell_rc(tmpdir);
    ASSERT(strstr(rc, ".profile") != NULL);

    if (old_shell) {
        cbm_setenv("SHELL", old_shell, 1);
        free(old_shell);
    } else
        cbm_unsetenv("SHELL");
    rmdir(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  CLI binary detection (port of TestFindCLI_*)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_find_cli_not_found) {
    /* Port of TestFindCLI_NotFound */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", tmpdir, 1);

    const char *result = cbm_find_cli("nonexistent-binary-xyz", tmpdir);
    ASSERT_STR_EQ(result, "");

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    rmdir(tmpdir);
    PASS();
}

TEST(cli_find_cli_on_path) {
    /* Port of TestFindCLI_FoundOnPATH */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char fakecli[512];
    snprintf(fakecli, sizeof(fakecli), "%s/fakecli", tmpdir);
    write_test_file(fakecli, "#!/bin/sh\n");
    th_make_executable(fakecli);

#ifdef _WIN32
    rmdir(tmpdir);
    SKIP_PLATFORM("Windows: PATH-based CLI lookup uses POSIX semantics");
#endif
    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", tmpdir, 1);

    const char *result = cbm_find_cli("fakecli", tmpdir);
    ASSERT(result[0] != '\0');
    ASSERT(strstr(result, "fakecli") != NULL);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    unlink(fakecli);
    rmdir(tmpdir);
    PASS();
}

TEST(cli_find_cli_fallback_paths) {
    /* Port of TestFindCLI_FallbackPaths */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-find-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

#ifdef _WIN32
    rmdir(tmpdir);
    SKIP_PLATFORM("Windows: fallback path lookup uses POSIX semantics");
#endif
    char localbin[512];
    snprintf(localbin, sizeof(localbin), "%s/.local/bin", tmpdir);
    test_mkdirp(localbin);

    char fakecli[512];
    snprintf(fakecli, sizeof(fakecli), "%s/testcli", localbin);
    write_test_file(fakecli, "#!/bin/sh\n");
    th_make_executable(fakecli);

    const char *raw = getenv("PATH");
    char *old_path = raw ? strdup(raw) : NULL;
    cbm_setenv("PATH", "/nonexistent", 1);

    const char *result = cbm_find_cli("testcli", tmpdir);
    ASSERT_STR_EQ(result, fakecli);

    if (old_path) {
        cbm_setenv("PATH", old_path, 1);
        free(old_path);
    }
    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dry-run flag parsing (port of TestDryRun)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_dry_run_flags) {
    /* Port of TestDryRun — just verifies the pattern */
    bool dry_run = false, force = false;
    const char *args[] = {"--dry-run", "--force"};
    for (int i = 0; i < 2; i++) {
        if (strcmp(args[i], "--dry-run") == 0)
            dry_run = true;
        if (strcmp(args[i], "--force") == 0)
            force = true;
    }
    ASSERT_TRUE(dry_run);
    ASSERT_TRUE(force);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Skill file management tests (port of install_test.go skill tests)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_skill_creation) {
    /* Port of TestInstallSkillCreation */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    int written = cbm_install_skills(skills_dir, false, false);
    ASSERT_EQ(written, CBM_SKILL_COUNT);

    /* Verify all 4 skills exist and have content */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        const char *data = read_test_file(path);
        ASSERT_NOT_NULL(data);
        ASSERT(strlen(data) > 0);
        /* Check YAML frontmatter */
        ASSERT(strncmp(data, "---\n", 4) == 0);
        /* Check name field */
        ASSERT(strstr(data, sk[i].name) != NULL);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_idempotent) {
    /* Port of TestInstallIdempotent */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Install twice */
    cbm_install_skills(skills_dir, false, false);
    int second = cbm_install_skills(skills_dir, false, false);

    /* Second install should write 0 (skills exist, no force) */
    ASSERT_EQ(second, 0);

    /* All skills should still exist */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_force_overwrite) {
    /* Port of TestCLI_InstallForceOverwrites */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int force_count = cbm_install_skills(skills_dir, true, false);

    /* Force should overwrite all */
    ASSERT_EQ(force_count, CBM_SKILL_COUNT);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_uninstall_removes_skills) {
    /* Port of TestUninstallRemovesSkills */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int removed = cbm_remove_skills(skills_dir, false);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Verify all removed */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_old_monolithic_skill) {
    /* Port of TestRemoveOldMonolithicSkill */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-skill-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Create old monolithic skill */
    char old_dir[1024];
    snprintf(old_dir, sizeof(old_dir), "%s/code-cortex-mcp", skills_dir);
    test_mkdirp(old_dir);
    char old_file[1024];
    snprintf(old_file, sizeof(old_file), "%s/SKILL.md", old_dir);
    write_test_file(old_file, "old skill");

    bool removed = cbm_remove_old_monolithic_skill(skills_dir, false);
    ASSERT_TRUE(removed);

    struct stat st;
    ASSERT(stat(old_dir, &st) != 0);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_skill_files_content) {
    /* Consolidated skill: all 4 former skills merged into one. */
    const cbm_skill_t *sk = cbm_get_skills();
    ASSERT_EQ(CBM_SKILL_COUNT, 1);
    ASSERT(strcmp(sk[0].name, "code-cortex") == 0);

    /* Exploring capabilities */
    ASSERT(strstr(sk[0].content, "search_graph") != NULL);
    ASSERT(strstr(sk[0].content, "get_graph_schema") != NULL);

    /* Tracing capabilities */
    ASSERT(strstr(sk[0].content, "trace_path") != NULL);
    ASSERT(strstr(sk[0].content, "direction") != NULL);
    ASSERT(strstr(sk[0].content, "detect_changes") != NULL);

    /* Quality capabilities */
    ASSERT(strstr(sk[0].content, "max_degree=0") != NULL);
    ASSERT(strstr(sk[0].content, "exclude_entry_points") != NULL);

    /* Reference capabilities */
    ASSERT(strstr(sk[0].content, "query_graph") != NULL);
    ASSERT(strstr(sk[0].content, "Cypher") != NULL);
    ASSERT(strstr(sk[0].content, "14 MCP Tools") != NULL);

    /* Gotchas section */
    ASSERT(strstr(sk[0].content, "Gotchas") != NULL);

    PASS();
}

TEST(cli_codex_instructions) {
    /* Port of TestCodexInstructionsCreation */
    const char *instr = cbm_get_codex_instructions();
    ASSERT_NOT_NULL(instr);
    ASSERT(strstr(instr, "Codebase Knowledge Graph") != NULL);
    ASSERT(strstr(instr, "trace_path") != NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Editor MCP config tests (Cursor/Windsurf/Gemini)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_editor_mcp_install) {
    /* Port of TestEditorMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    int rc = cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "mcpServers") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_idempotent) {
    /* Port of TestEditorMCPInstallIdempotent */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    int rc = cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    /* Should still parse as valid JSON with only 1 server */
    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Count occurrences of "code-cortex-mcp" (should be exactly 1 in mcpServers) */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "\"code-cortex-mcp\"")) != NULL) {
        count++;
        p += 20;
    }
    /* The key appears once as key name */
    ASSERT_EQ(count, 1);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_preserves_others) {
    /* Port of TestEditorMCPPreservesOtherServers */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);
    test_mkdirp(tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);

    /* Write config with existing server */
    write_test_file(configpath,
                    "{\"mcpServers\": {\"other-server\": {\"command\": \"/usr/bin/other\"}}}");

    cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "other-server") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_uninstall) {
    /* Port of TestEditorMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.cursor/mcp.json", tmpdir);

    cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    int rc = cbm_remove_editor_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* code-cortex-mcp should be removed */
    ASSERT(strstr(data, "\"code-cortex-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_migrates_legacy_key) {
    /* Install over a config carrying the pre-rename server key must drop the
     * legacy entry and write the new one (config migration on upsert). */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp.json", tmpdir);
    write_test_file(configpath, "{\"mcpServers\": {"
                                "\"codebase-memory-mcp\": {\"command\": \"/old/path\"},"
                                "\"other-server\": {\"command\": \"/usr/bin/other\"}}}");

    cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);
    ASSERT(strstr(data, "\"code-cortex-mcp\"") != NULL);
    ASSERT(strstr(data, "other-server") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_editor_mcp_uninstall_removes_legacy_key) {
    /* Uninstall must clean both the current and the pre-rename server key. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp.json", tmpdir);
    write_test_file(configpath, "{\"mcpServers\": {"
                                "\"codebase-memory-mcp\": {\"command\": \"/old/path\"},"
                                "\"other-server\": {\"command\": \"/usr/bin/other\"}}}");

    int rc = cbm_remove_editor_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"codebase-memory-mcp\"") == NULL);
    ASSERT(strstr(data, "other-server") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_mcp_migrates_legacy_section) {
    /* Codex TOML upsert must drop the pre-rename [mcp_servers.*] section. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "[mcp_servers.codebase-memory-mcp]\n"
                                "command = \"/old/path\"\n"
                                "\n"
                                "[other]\n"
                                "key = \"value\"\n");

    cbm_upsert_codex_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "[mcp_servers.codebase-memory-mcp]") == NULL);
    ASSERT(strstr(data, "[mcp_servers.code-cortex-mcp]") != NULL);
    ASSERT(strstr(data, "[other]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_mcp_migrates_legacy_subtables) {
    /* Codex writes [mcp_servers.<name>.tools.<tool>] subtables of its own once
     * a tool gets an approval mode. Leaving one behind after the rename
     * re-declares the old server with no command — Codex then refuses to start
     * with "invalid transport". The whole family has to go. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "[mcp_servers.codebase-memory-mcp]\n"
                                "command = \"/old/path\"\n"
                                "\n"
                                "[mcp_servers.codebase-memory-mcp.tools.list_projects]\n"
                                "approval_mode = \"approve\"\n"
                                "\n"
                                "[mcp_servers.codebase-memory-mcp.tools.search_graph]\n"
                                "approval_mode = \"approve\"\n"
                                "\n"
                                "[other]\n"
                                "key = \"value\"\n");

    cbm_upsert_codex_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT_NULL(strstr(data, "codebase-memory-mcp")); /* no orphan subtable */
    ASSERT(strstr(data, "[mcp_servers.code-cortex-mcp]") != NULL);
    ASSERT(strstr(data, "[other]") != NULL);
    ASSERT(strstr(data, "key = \"value\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_mcp_upsert_keeps_tool_subtables) {
    /* Our own per-tool approval modes are the user's settings — a reinstall
     * updates the command and leaves them alone. The comment introducing the
     * next section must survive too: it is the SessionStart sentinel. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "[mcp_servers.code-cortex-mcp]\n"
                                "command = \"/old/path\"\n"
                                "\n"
                                "# >>> code-cortex-mcp SessionStart >>>\n"
                                "[[hooks.SessionStart]]\n"
                                "matcher = \"startup\"\n"
                                "# <<< code-cortex-mcp SessionStart <<<\n"
                                "\n"
                                "[mcp_servers.code-cortex-mcp.tools.search_graph]\n"
                                "approval_mode = \"approve\"\n");

    ASSERT_EQ(cbm_upsert_codex_mcp("/new/path/code-cortex-mcp", configpath), 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT_NULL(strstr(data, "/old/path"));
    ASSERT(strstr(data, "/new/path/code-cortex-mcp") != NULL);
    /* User's approval setting kept */
    ASSERT(strstr(data, "[mcp_servers.code-cortex-mcp.tools.search_graph]") != NULL);
    ASSERT(strstr(data, "approval_mode = \"approve\"") != NULL);
    /* Sentinel comment not swallowed by the replaced table */
    ASSERT(strstr(data, "# >>> code-cortex-mcp SessionStart >>>") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_mcp_uninstall_removes_subtables) {
    /* Same family rule on the way out: uninstall must not strand a subtable. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "model = \"gpt-5\"\n"
                                "\n"
                                "[mcp_servers.code-cortex-mcp]\n"
                                "command = \"/usr/local/bin/code-cortex-mcp\"\n"
                                "\n"
                                "[mcp_servers.code-cortex-mcp.tools.search_graph]\n"
                                "approval_mode = \"approve\"\n"
                                "\n"
                                "[mcp_servers.other]\n"
                                "command = \"x\"\n");

    ASSERT_EQ(cbm_remove_codex_mcp(configpath), 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT_NULL(strstr(data, "code-cortex-mcp"));
    ASSERT(strstr(data, "model = \"gpt-5\"") != NULL);
    ASSERT(strstr(data, "[mcp_servers.other]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_migrate_legacy_install) {
    /* Filesystem migration: legacy binary, cache dir, and skills removed. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mig-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char path[512];
    /* The migration looks for the platform-appropriate legacy binary name. */
    char old_bin[512];
#ifdef _WIN32
    snprintf(old_bin, sizeof(old_bin), "%s/.local/bin/codebase-memory-mcp.exe", tmpdir);
#else
    snprintf(old_bin, sizeof(old_bin), "%s/.local/bin/codebase-memory-mcp", tmpdir);
#endif
    snprintf(path, sizeof(path), "%s/.local/bin", tmpdir);
    test_mkdirp(path);
    write_test_file(old_bin, "old binary");
    snprintf(path, sizeof(path), "%s/.cache/codebase-memory-mcp", tmpdir);
    test_mkdirp(path);
    snprintf(path, sizeof(path), "%s/.cache/codebase-memory-mcp/proj.db", tmpdir);
    write_test_file(path, "old index");
    snprintf(path, sizeof(path), "%s/.claude/skills/codebase-memory", tmpdir);
    test_mkdirp(path);
    snprintf(path, sizeof(path), "%s/.claude/skills/codebase-memory/SKILL.md", tmpdir);
    write_test_file(path, "old skill");

    /* Dry-run reports but removes nothing */
    int would = cbm_migrate_legacy_install(tmpdir, true);
    ASSERT_EQ(would, 3);
    struct stat st;
    ASSERT(stat(old_bin, &st) == 0);

    /* Real run removes all three */
    int migrated = cbm_migrate_legacy_install(tmpdir, false);
    ASSERT_EQ(migrated, 3);
    ASSERT(stat(old_bin, &st) != 0);
    snprintf(path, sizeof(path), "%s/.cache/codebase-memory-mcp", tmpdir);
    ASSERT(stat(path, &st) != 0);
    snprintf(path, sizeof(path), "%s/.claude/skills/codebase-memory", tmpdir);
    ASSERT(stat(path, &st) != 0);

    /* Second run is a no-op */
    ASSERT_EQ(cbm_migrate_legacy_install(tmpdir, false), 0);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_gemini_mcp_install) {
    /* Port of TestGeminiMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.gemini/settings.json", tmpdir);

    /* Gemini uses same mcpServers format as Cursor */
    int rc = cbm_install_editor_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "mcpServers") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  VS Code MCP config tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_vscode_mcp_install) {
    /* Port of TestVSCodeMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/Code/User/mcp.json", tmpdir);

    int rc = cbm_install_vscode_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"servers\"") != NULL);
    ASSERT(strstr(data, "\"type\"") != NULL);
    ASSERT(strstr(data, "\"stdio\"") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_vscode_mcp_uninstall) {
    /* Port of TestVSCodeMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/Code/User/mcp.json", tmpdir);

    cbm_install_vscode_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    int rc = cbm_remove_vscode_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"code-cortex-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Zed MCP config tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_zed_mcp_install) {
    /* Port of TestZedMCPInstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);

    int rc = cbm_install_zed_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"context_servers\"") != NULL);
    ASSERT(strstr(data, "\"command\"") != NULL);
    ASSERT(strstr(data, "\"args\"") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_preserves_settings) {
    /* Port of TestZedMCPPreservesSettings */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
    test_mkdirp(dir);

    /* Pre-existing Zed settings */
    write_test_file(configpath, "{\"theme\": \"One Dark\", \"vim_mode\": true}");

    cbm_install_zed_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Original settings preserved */
    ASSERT(strstr(data, "One Dark") != NULL);
    ASSERT(strstr(data, "vim_mode") != NULL);
    /* MCP server added */
    ASSERT(strstr(data, "context_servers") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_uninstall) {
    /* Port of TestZedMCPUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);

    cbm_install_zed_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    int rc = cbm_remove_zed_mcp(configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"code-cortex-mcp\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_zed_mcp_jsonc_comments) {
    /* Issue #24: Zed settings.json uses JSONC (comments + trailing commas) */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-mcp-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/.config/zed/settings.json", tmpdir);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
    test_mkdirp(dir);

    /* JSONC with comments and trailing commas — must not fail */
    write_test_file(configpath, "// Zed settings\n"
                                "{\n"
                                "  \"theme\": \"One Dark\",\n"
                                "  /* multi-line\n"
                                "     comment */\n"
                                "  \"vim_mode\": true,\n" /* trailing comma */
                                "}\n");

    int rc = cbm_install_zed_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Original settings preserved */
    ASSERT(strstr(data, "One Dark") != NULL);
    ASSERT(strstr(data, "vim_mode") != NULL);
    /* MCP server added */
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "context_servers") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  PATH management tests (port of TestCLI_InstallPATHAppend)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_ensure_path_append) {
    /* Port of TestCLI_InstallPATHAppend */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "# existing content\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(rcfile);
    ASSERT(strstr(data, "export PATH=\"/usr/local/bin:$PATH\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_ensure_path_already_present) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "export PATH=\"/usr/local/bin:$PATH\"\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 1); /* 1 = already present */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_ensure_path_dry_run) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/.zshrc", tmpdir);
    write_test_file(rcfile, "# clean\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, true);
    ASSERT_EQ(rc, 0);

    /* File should NOT be modified */
    const char *data = read_test_file(rcfile);
    ASSERT(strstr(data, "export PATH") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #319: a fish config must get fish-native syntax, never `export PATH=`
 * (which is a syntax error in fish and breaks config.fish). */
TEST(cli_ensure_path_fish_syntax_issue319) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-path-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char rcfile[512];
    snprintf(rcfile, sizeof(rcfile), "%s/config.fish", tmpdir);
    write_test_file(rcfile, "# existing fish config\n");

    int rc = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(rcfile);
    ASSERT_NOT_NULL(data);
    /* fish-native form, and NO sh-style export. */
    ASSERT(strstr(data, "fish_add_path /usr/local/bin") != NULL);
    ASSERT(strstr(data, "export PATH") == NULL);

    /* Idempotent: a second call detects the existing fish line. */
    int rc2 = cbm_ensure_path("/usr/local/bin", rcfile, false);
    ASSERT_EQ(rc2, 1);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  File copy tests (port of update_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_copy_file) {
    /* Port of TestCopyFile */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-copy-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/source", tmpdir);
    snprintf(dst, sizeof(dst), "%s/dest", tmpdir);

    write_test_file(src, "test content for copy");

    int rc = cbm_copy_file(src, dst);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(dst);
    ASSERT_STR_EQ(data, "test content for copy");

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_copy_file_source_not_found) {
    /* Port of TestCopyFile_SourceNotFound */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-copy-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/nonexistent", tmpdir);
    snprintf(dst, sizeof(dst), "%s/dest", tmpdir);

    int rc = cbm_copy_file(src, dst);
    ASSERT(rc != 0);

    rmdir(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Tar.gz extraction tests (port of update_test.go)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_extract_binary_from_targz) {
    /* Port of TestExtractBinaryFromTarGz */
    const char *content = "fake binary content";
    int gz_len;
    unsigned char *gz =
        create_test_targz("code-cortex-mcp-linux-amd64", (const unsigned char *)content,
                          (int)strlen(content), &gz_len);
    ASSERT_NOT_NULL(gz);

    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(gz, gz_len, &out_len);
    ASSERT_NOT_NULL(extracted);
    ASSERT_EQ(out_len, (int)strlen(content));
    ASSERT_MEM_EQ(extracted, content, out_len);

    free(extracted);
    free(gz);
    PASS();
}

TEST(cli_extract_binary_from_targz_not_found) {
    /* Port of TestExtractBinaryFromTarGz_NotFound */
    const char *content = "hello";
    int gz_len;
    unsigned char *gz = create_test_targz("some-other-file", (const unsigned char *)content,
                                          (int)strlen(content), &gz_len);
    ASSERT_NOT_NULL(gz);

    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(gz, gz_len, &out_len);
    ASSERT_NULL(extracted);

    free(gz);
    PASS();
}

TEST(cli_extract_binary_from_targz_invalid_data) {
    /* Port of TestExtractBinaryFromTarGz_InvalidData */
    const unsigned char bad_data[] = "not a valid tar.gz";
    int out_len;
    unsigned char *extracted = cbm_extract_binary_from_targz(bad_data, sizeof(bad_data), &out_len);
    ASSERT_NULL(extracted);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Zip extraction tests
 * ═══════════════════════════════════════════════════════════════════ */

/* Build a minimal zip file with one stored (uncompressed) entry. */
static unsigned char *create_test_zip_stored(const char *filename, const unsigned char *content,
                                             int content_len, int *out_len) {
    /* Local file header (30 bytes) + filename + content + central dir + EOCD */
    int name_len = (int)strlen(filename);
    int local_hdr_sz = 30 + name_len;
    int cd_hdr_sz = 46 + name_len;
    int eocd_sz = 22;
    int total = local_hdr_sz + content_len + cd_hdr_sz + eocd_sz;
    unsigned char *zip = (unsigned char *)calloc(1, (size_t)total);
    if (!zip)
        return NULL;
    int pos = 0;

    /* Local file header */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x03;
    zip[pos + 3] = 0x04; /* signature */
    zip[pos + 4] = 20;
    zip[pos + 5] = 0; /* version needed = 2.0 */
    zip[pos + 8] = 0;
    zip[pos + 9] = 0; /* compression = stored */
    zip[pos + 18] = (unsigned char)(content_len & 0xFF);
    zip[pos + 19] = (unsigned char)((content_len >> 8) & 0xFF);
    zip[pos + 20] = (unsigned char)((content_len >> 16) & 0xFF);
    zip[pos + 21] = (unsigned char)((content_len >> 24) & 0xFF);
    zip[pos + 22] = zip[pos + 18];
    zip[pos + 23] = zip[pos + 19];
    zip[pos + 24] = zip[pos + 20];
    zip[pos + 25] = zip[pos + 21];
    zip[pos + 26] = (unsigned char)(name_len & 0xFF);
    zip[pos + 27] = (unsigned char)((name_len >> 8) & 0xFF);
    memcpy(zip + pos + 30, filename, (size_t)name_len);
    pos += 30 + name_len;
    memcpy(zip + pos, content, (size_t)content_len);
    pos += content_len;

    int cd_start = pos;
    /* Central directory header */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x01;
    zip[pos + 3] = 0x02;
    zip[pos + 10] = 0;
    zip[pos + 11] = 0; /* compression = stored */
    zip[pos + 20] = (unsigned char)(content_len & 0xFF);
    zip[pos + 21] = (unsigned char)((content_len >> 8) & 0xFF);
    zip[pos + 22] = (unsigned char)((content_len >> 16) & 0xFF);
    zip[pos + 23] = (unsigned char)((content_len >> 24) & 0xFF);
    zip[pos + 24] = zip[pos + 20];
    zip[pos + 25] = zip[pos + 21];
    zip[pos + 26] = zip[pos + 22];
    zip[pos + 27] = zip[pos + 23];
    zip[pos + 28] = (unsigned char)(name_len & 0xFF);
    zip[pos + 29] = (unsigned char)((name_len >> 8) & 0xFF);
    pos += 46 + name_len;

    /* EOCD */
    zip[pos] = 0x50;
    zip[pos + 1] = 0x4B;
    zip[pos + 2] = 0x05;
    zip[pos + 3] = 0x06;
    zip[pos + 8] = 1;  /* num entries this disk */
    zip[pos + 10] = 1; /* total entries */
    int cd_size = pos - cd_start;
    zip[pos + 12] = (unsigned char)(cd_size & 0xFF);
    zip[pos + 13] = (unsigned char)((cd_size >> 8) & 0xFF);
    zip[pos + 16] = (unsigned char)(cd_start & 0xFF);
    zip[pos + 17] = (unsigned char)((cd_start >> 8) & 0xFF);

    *out_len = total;
    return zip;
}

TEST(cli_extract_binary_from_zip) {
    const char *content = "#!/bin/sh\necho test\n";
    int zip_len = 0;
    unsigned char *zip = create_test_zip_stored("code-cortex-mcp", (const unsigned char *)content,
                                                (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NOT_NULL(extracted);
    ASSERT_EQ(out_len, (int)strlen(content));
    ASSERT_MEM_EQ(extracted, content, (size_t)out_len);
    free(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_not_found) {
    const char *content = "data";
    int zip_len = 0;
    unsigned char *zip = create_test_zip_stored("other-file.txt", (const unsigned char *)content,
                                                (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NULL(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_path_traversal) {
    const char *content = "malicious";
    int zip_len = 0;
    unsigned char *zip =
        create_test_zip_stored("../../etc/code-cortex-mcp", (const unsigned char *)content,
                               (int)strlen(content), &zip_len);
    ASSERT_NOT_NULL(zip);

    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(zip, zip_len, &out_len);
    ASSERT_NULL(extracted);
    free(zip);
    PASS();
}

TEST(cli_extract_binary_from_zip_invalid) {
    const unsigned char bad_data[] = "not a zip file";
    int out_len = 0;
    unsigned char *extracted = cbm_extract_binary_from_zip(bad_data, sizeof(bad_data), &out_len);
    ASSERT_NULL(extracted);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Skill dry-run tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_install_dry_run) {
    /* Port of TestCLI_InstallDryRun */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-dry-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    int count = cbm_install_skills(skills_dir, false, true);
    ASSERT_EQ(count, CBM_SKILL_COUNT);

    /* Skills should NOT be created */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    rmdir(tmpdir);
    PASS();
}

TEST(cli_uninstall_dry_run) {
    /* Port of TestCLI_UninstallDryRun */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-dry-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    cbm_install_skills(skills_dir, false, false);
    int removed = cbm_remove_skills(skills_dir, true);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Skills should still exist */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Full install + uninstall lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_install_and_uninstall) {
    /* Port of TestCLI_InstallAndUninstall */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-full-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/.claude/skills", tmpdir);

    /* Install */
    int written = cbm_install_skills(skills_dir, false, false);
    ASSERT_EQ(written, CBM_SKILL_COUNT);

    /* Verify */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, sk[i].name);
        struct stat st;
        ASSERT_EQ(stat(path, &st), 0);
    }

    /* Uninstall */
    int removed = cbm_remove_skills(skills_dir, false);
    ASSERT_EQ(removed, CBM_SKILL_COUNT);

    /* Verify removed */
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", skills_dir, sk[i].name);
        struct stat st;
        ASSERT(stat(path, &st) != 0);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  YAML parser unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_yaml_parse_simple) {
    /* Basic key-value parsing */
    const char *yaml = "name: test\nversion: 1.0\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "name"), "test");
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "version"), "1.0");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_nested) {
    /* Nested map */
    const char *yaml = "parent:\n"
                       "  child: value\n"
                       "  number: 42\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "parent.child"), "value");
    ASSERT_FLOAT_EQ(cbm_yaml_get_float(root, "parent.number", 0), 42.0, 0.001);
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_list) {
    /* String list */
    const char *yaml = "items:\n"
                       "  - alpha\n"
                       "  - beta\n"
                       "  - gamma\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    const char *items[8];
    int count = cbm_yaml_get_str_list(root, "items", items, 8);
    ASSERT_EQ(count, 3);
    ASSERT_STR_EQ(items[0], "alpha");
    ASSERT_STR_EQ(items[1], "beta");
    ASSERT_STR_EQ(items[2], "gamma");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_bool) {
    const char *yaml = "enabled: true\n"
                       "disabled: false\n"
                       "on_flag: yes\n"
                       "off_flag: no\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(cbm_yaml_get_bool(root, "enabled", false));
    ASSERT_FALSE(cbm_yaml_get_bool(root, "disabled", true));
    ASSERT_TRUE(cbm_yaml_get_bool(root, "on_flag", false));
    ASSERT_FALSE(cbm_yaml_get_bool(root, "off_flag", true));
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_comments) {
    const char *yaml = "# This is a comment\n"
                       "key: value # inline comment\n"
                       "\n"
                       "# Another comment\n"
                       "other: data\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "key"), "value");
    ASSERT_STR_EQ(cbm_yaml_get_str(root, "other"), "data");
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_parse_empty) {
    cbm_yaml_node_t *root = cbm_yaml_parse("", 0);
    ASSERT_NOT_NULL(root);
    ASSERT_NULL(cbm_yaml_get_str(root, "anything"));
    cbm_yaml_free(root);
    PASS();
}

TEST(cli_yaml_has) {
    const char *yaml = "a:\n  b: c\n";
    cbm_yaml_node_t *root = cbm_yaml_parse(yaml, (int)strlen(yaml));
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(cbm_yaml_has(root, "a"));
    ASSERT_TRUE(cbm_yaml_has(root, "a.b"));
    ASSERT_FALSE(cbm_yaml_has(root, "a.c"));
    ASSERT_FALSE(cbm_yaml_has(root, "x"));
    cbm_yaml_free(root);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group A: Agent Detection
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_detect_agents_finds_claude) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.claude", tmpdir);
    test_mkdirp(dir);

    /* Unset CLAUDE_CONFIG_DIR so detection is exercised against home_dir/.claude
     * and the runner's real env (which may set it) does not leak in. */
    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_unsetenv("CLAUDE_CONFIG_DIR");

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.claude_code);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_claude_via_env) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Config dir lives OUTSIDE home_dir/.claude, pointed at by CLAUDE_CONFIG_DIR. */
    char ccd[512];
    snprintf(ccd, sizeof(ccd), "%s/custom-claude", tmpdir);
    test_mkdirp(ccd);

    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_setenv("CLAUDE_CONFIG_DIR", ccd, 1);

    /* home_dir has no .claude, but detection must still find Claude via the env var. */
    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.claude_code);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    } else {
        cbm_unsetenv("CLAUDE_CONFIG_DIR");
    }

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_codex) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.codex", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.codex);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #222: Cursor (~/.cursor/) must be detected so install/update registers
 * the MCP server in ~/.cursor/mcp.json — previously it was never discovered. */
TEST(cli_detect_agents_finds_cursor_issue222) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.cursor);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #388: `install --plan` must emit a machine-readable receipt of planned
 * writes WITHOUT mutating any config (the pre-mutation trust primitive). */
TEST(cli_install_plan_receipt_no_mutation_issue388) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-plan-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Make Cursor + Codex "detected". */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.cursor", tmpdir);
    test_mkdirp(dir);
    snprintf(dir, sizeof(dir), "%s/.codex", tmpdir);
    test_mkdirp(dir);

    char *json = cbm_build_install_plan_json(tmpdir, "/usr/local/bin/code-cortex-mcp");
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "agent.install.plan.v1") != NULL);
    ASSERT(strstr(json, "writes_started") != NULL);
    ASSERT(strstr(json, "next_safe_command") != NULL);
    ASSERT(strstr(json, "cursor") != NULL);
    ASSERT(strstr(json, ".cursor/mcp.json") != NULL);
    ASSERT(strstr(json, ".codex/config.toml") != NULL);
    free(json);

    /* Critical: building the plan must NOT have created any config file. */
    char cfg[512];
    struct stat st;
    snprintf(cfg, sizeof(cfg), "%s/.cursor/mcp.json", tmpdir);
    ASSERT(stat(cfg, &st) != 0); /* must not exist */
    snprintf(cfg, sizeof(cfg), "%s/.codex/config.toml", tmpdir);
    ASSERT(stat(cfg, &st) != 0); /* must not exist */

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #330: Codex SessionStart reminder hook in config.toml — installed,
 * idempotent, preserves other content, and cleanly removed. */
TEST(cli_codex_session_hook_issue330) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codexhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config.toml", tmpdir);
    write_test_file(cfg, "[mcp_servers.other]\ncommand = \"x\"\n");

    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "[[hooks.SessionStart]]") != NULL);
    ASSERT(strstr(d, "[[hooks.SessionStart.hooks]]") != NULL);
    ASSERT(strstr(d, "search_graph") != NULL);
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL); /* pre-existing content preserved */
    /* Idempotent: a second upsert leaves exactly ONE hook block. */
    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    const char *first = strstr(d, "[[hooks.SessionStart]]");
    ASSERT_NOT_NULL(first);
    ASSERT_NULL(strstr(first + 1, "[[hooks.SessionStart]]"));

    ASSERT_EQ(cbm_remove_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NULL(strstr(d, "hooks.SessionStart"));
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL); /* still preserved after removal */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_hook_migrates_legacy_block) {
    /* The pre-rename sentinel block has its own markers — without a migration
     * it accumulates, leaving a second hook that advertises a dead server. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codexhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config.toml", tmpdir);
    write_test_file(cfg, "[mcp_servers.other]\n"
                         "command = \"x\"\n"
                         "\n"
                         "# >>> codebase-memory-mcp SessionStart >>>\n"
                         "[[hooks.SessionStart]]\n"
                         "matcher = \"startup|resume|clear|compact\"\n"
                         "\n"
                         "[[hooks.SessionStart.hooks]]\n"
                         "type = \"command\"\n"
                         "command = 'echo \"prefer codebase-memory-mcp\"'\n"
                         "# <<< codebase-memory-mcp SessionStart <<<\n");

    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);

    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT_NULL(strstr(d, "codebase-memory-mcp")); /* legacy block gone */
    ASSERT(strstr(d, "# >>> code-cortex-mcp SessionStart >>>") != NULL);
    /* Exactly one hook block remains. */
    const char *first = strstr(d, "[[hooks.SessionStart]]");
    ASSERT_NOT_NULL(first);
    ASSERT_NULL(strstr(first + 1, "[[hooks.SessionStart]]"));
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_codex_hook_preserves_foreign_tables) {
    /* Codex appends its own tables ahead of the file's trailing comment, so
     * [hooks.state] — hook-trust hashes for every project on the machine —
     * drifts inside our sentinels. Removing the block must not take them. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codexhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/config.toml", tmpdir);
    write_test_file(cfg, "[mcp_servers.other]\n"
                         "command = \"x\"\n"
                         "\n"
                         "# >>> code-cortex-mcp SessionStart >>>\n"
                         "[[hooks.SessionStart]]\n"
                         "matcher = \"startup|resume|clear|compact\"\n"
                         "\n"
                         "[[hooks.SessionStart.hooks]]\n"
                         "type = \"command\"\n"
                         "command = 'echo \"hi\"'\n"
                         "\n"
                         "[hooks.state]\n"
                         "\n"
                         "[hooks.state.\"/other/project:pre_tool_use:0:0\"]\n"
                         "trusted_hash = \"sha256:deadbeef\"\n"
                         "# <<< code-cortex-mcp SessionStart <<<\n");

    ASSERT_EQ(cbm_remove_codex_hooks(cfg), 0);

    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT_NULL(strstr(d, "hooks.SessionStart"));           /* ours removed */
    ASSERT_NULL(strstr(d, "code-cortex-mcp SessionStart")); /* sentinels too */
    /* Codex's bookkeeping survives */
    ASSERT(strstr(d, "[hooks.state]") != NULL);
    ASSERT(strstr(d, "trusted_hash = \"sha256:deadbeef\"") != NULL);
    ASSERT(strstr(d, "[mcp_servers.other]") != NULL);

    /* Re-upserting after the salvage still yields exactly one block. */
    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    ASSERT_EQ(cbm_upsert_codex_hooks(cfg), 0);
    d = read_test_file(cfg);
    const char *first = strstr(d, "[[hooks.SessionStart]]");
    ASSERT_NOT_NULL(first);
    ASSERT_NULL(strstr(first + 1, "[[hooks.SessionStart]]"));
    ASSERT(strstr(d, "trusted_hash = \"sha256:deadbeef\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* Gemini/Antigravity SessionStart reminder parity (settings.json JSON path). */
TEST(cli_gemini_session_hook_parity) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-gemhook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char cfg[512];
    snprintf(cfg, sizeof(cfg), "%s/settings.json", tmpdir);

    ASSERT_EQ(cbm_upsert_gemini_session_hooks(cfg), 0);
    const char *d = read_test_file(cfg);
    ASSERT_NOT_NULL(d);
    ASSERT(strstr(d, "SessionStart") != NULL);
    ASSERT(strstr(d, "search_graph") != NULL);

    ASSERT_EQ(cbm_remove_gemini_session_hooks(cfg), 0);
    d = read_test_file(cfg);
    ASSERT_NULL(strstr(d, "SessionStart"));

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_gemini) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.gemini", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.gemini);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_zed) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
#ifdef __APPLE__
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/Zed", tmpdir);
#elif defined(_WIN32)
    snprintf(dir, sizeof(dir), "%s/AppData/Local/Zed", tmpdir);
#else
    snprintf(dir, sizeof(dir), "%s/.config/zed", tmpdir);
#endif
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.zed);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_antigravity) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    /* Antigravity CLI installs under ~/.gemini/antigravity-cli/ (2026). */
    snprintf(dir, sizeof(dir), "%s/.gemini/antigravity-cli", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.antigravity);
    ASSERT_TRUE(agents.gemini); /* parent ~/.gemini implies gemini too */

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_kilocode) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
#ifdef __APPLE__
    snprintf(dir, sizeof(dir),
             "%s/Library/Application Support/Code/User/globalStorage/kilocode.kilo-code", tmpdir);
#elif defined(_WIN32)
    snprintf(dir, sizeof(dir), "%s/AppData/Roaming/Code/User/globalStorage/kilocode.kilo-code",
             tmpdir);
#else
    snprintf(dir, sizeof(dir), "%s/.config/Code/User/globalStorage/kilocode.kilo-code", tmpdir);
#endif
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.kilocode);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_finds_kiro) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.kiro", tmpdir);
    test_mkdirp(dir);

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_TRUE(agents.kiro);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_detect_agents_none_found) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-detect-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    /* Empty home dir → no config dirs → no directory-based agents detected.
     * Note: opencode/aider may still be detected via system fallback paths
     * (e.g. /usr/local/bin) so we only assert on directory-based agents.
     * Unset CLAUDE_CONFIG_DIR so the runner's real env does not leak in. */
    const char *saved_ccd = getenv("CLAUDE_CONFIG_DIR");
    char *saved_ccd_copy = saved_ccd ? strdup(saved_ccd) : NULL;
    cbm_unsetenv("CLAUDE_CONFIG_DIR");

    cbm_detected_agents_t agents = cbm_detect_agents(tmpdir);
    ASSERT_FALSE(agents.claude_code);
    ASSERT_FALSE(agents.codex);
    ASSERT_FALSE(agents.gemini);
    ASSERT_FALSE(agents.zed);
    ASSERT_FALSE(agents.antigravity);
    ASSERT_FALSE(agents.kilocode);
    ASSERT_FALSE(agents.kiro);

    if (saved_ccd_copy) {
        cbm_setenv("CLAUDE_CONFIG_DIR", saved_ccd_copy, 1);
        free(saved_ccd_copy);
    }

    rmdir(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Codex TOML
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_codex_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);

    int rc = cbm_upsert_codex_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "[mcp_servers.code-cortex-mcp]") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "model = \"gpt-4\"\n\n[other_setting]\nfoo = \"bar\"\n");

    int rc = cbm_upsert_codex_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Existing settings preserved */
    ASSERT(strstr(data, "model = \"gpt-4\"") != NULL);
    ASSERT(strstr(data, "[other_setting]") != NULL);
    /* Our entry added */
    ASSERT(strstr(data, "[mcp_servers.code-cortex-mcp]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_codex_mcp_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-codex-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/config.toml", tmpdir);
    write_test_file(configpath, "[mcp_servers.code-cortex-mcp]\n"
                                "command = \"/old/path/code-cortex-mcp\"\n"
                                "\n"
                                "[other_setting]\nfoo = \"bar\"\n");

    int rc = cbm_upsert_codex_mcp("/new/path/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    /* Old path replaced */
    ASSERT(strstr(data, "/old/path") == NULL);
    ASSERT(strstr(data, "/new/path/code-cortex-mcp") != NULL);
    /* Other settings preserved */
    ASSERT(strstr(data, "[other_setting]") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Zed (corrected format)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_zed_mcp_uses_args_format) {
    /* Verify Zed uses args:[""] NOT source:"custom" */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-zed-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/settings.json", tmpdir);

    cbm_install_zed_mcp("/usr/local/bin/code-cortex-mcp", configpath);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "\"args\"") != NULL);
    /* Must NOT have source:"custom" */
    ASSERT(strstr(data, "\"source\"") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — OpenCode
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_opencode_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ocode-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/opencode.json", tmpdir);

    int rc = cbm_upsert_opencode_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "/usr/local/bin/code-cortex-mcp") != NULL);
    ASSERT(strstr(data, "\"enabled\":true") != NULL || strstr(data, "\"enabled\": true") != NULL);
    /* command must be emitted as an array, not a string */
    ASSERT(strstr(data, "\"command\":[") != NULL || strstr(data, "\"command\": [") != NULL);
    /* type must be explicitly set to \"local\" */
    ASSERT(strstr(data, "\"type\":\"local\"") != NULL ||
           strstr(data, "\"type\": \"local\"") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_opencode_mcp_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ocode-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/opencode.json", tmpdir);
    write_test_file(configpath, "{\"mcp\":{\"other-server\":{\"command\":\"/usr/bin/other\"}}}");

    int rc = cbm_upsert_opencode_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "other-server") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group B: MCP Config Upsert — Antigravity
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_antigravity_mcp_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-anti-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp_config.json", tmpdir);

    int rc = cbm_upsert_antigravity_mcp("/usr/local/bin/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_antigravity_mcp_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-anti-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char configpath[512];
    snprintf(configpath, sizeof(configpath), "%s/mcp_config.json", tmpdir);
    write_test_file(configpath,
                    "{\"mcpServers\":{\"code-cortex-mcp\":{\"command\":\"/old/path\"}}}");

    int rc = cbm_upsert_antigravity_mcp("/new/path/code-cortex-mcp", configpath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(configpath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "/old/path") == NULL);
    ASSERT(strstr(data, "/new/path/code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group C: Instructions File Upsert
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_instructions_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);

    int rc = cbm_upsert_instructions(filepath, "# Test content\nHello world\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "<!-- code-cortex-mcp:start -->") != NULL);
    ASSERT(strstr(data, "<!-- code-cortex-mcp:end -->") != NULL);
    ASSERT(strstr(data, "Hello world") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# My Project Rules\n\nDo the thing.\n");

    int rc = cbm_upsert_instructions(filepath, "# CMM\nUse search_graph\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Original content preserved */
    ASSERT(strstr(data, "My Project Rules") != NULL);
    ASSERT(strstr(data, "Do the thing") != NULL);
    /* CMM section appended */
    ASSERT(strstr(data, "code-cortex-mcp:start") != NULL);
    ASSERT(strstr(data, "search_graph") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# Rules\n"
                              "<!-- code-cortex-mcp:start -->\n"
                              "OLD CONTENT\n"
                              "<!-- code-cortex-mcp:end -->\n"
                              "# Other stuff\n");

    int rc = cbm_upsert_instructions(filepath, "NEW CONTENT\n");
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Old content replaced */
    ASSERT(strstr(data, "OLD CONTENT") == NULL);
    ASSERT(strstr(data, "NEW CONTENT") != NULL);
    /* Surrounding content preserved */
    ASSERT(strstr(data, "# Rules") != NULL);
    ASSERT(strstr(data, "# Other stuff") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_instructions_no_duplicate) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);

    /* Install twice */
    cbm_upsert_instructions(filepath, "Content v1\n");
    cbm_upsert_instructions(filepath, "Content v2\n");

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    /* Only one start marker */
    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "code-cortex-mcp:start")) != NULL) {
        count++;
        p += 25;
    }
    ASSERT_EQ(count, 1);
    /* Latest content */
    ASSERT(strstr(data, "Content v2") != NULL);
    ASSERT(strstr(data, "Content v1") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_instructions) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-instr-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/AGENTS.md", tmpdir);
    write_test_file(filepath, "# Rules\n"
                              "<!-- code-cortex-mcp:start -->\n"
                              "CMM Content\n"
                              "<!-- code-cortex-mcp:end -->\n"
                              "# Other\n");

    int rc = cbm_remove_instructions(filepath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(filepath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "CMM Content") == NULL);
    ASSERT(strstr(data, "code-cortex-mcp") == NULL);
    ASSERT(strstr(data, "# Rules") != NULL);
    ASSERT(strstr(data, "# Other") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_agent_instructions_content) {
    const char *instr = cbm_get_agent_instructions();
    ASSERT_NOT_NULL(instr);
    ASSERT(strstr(instr, "search_graph") != NULL);
    ASSERT(strstr(instr, "trace_path") != NULL);
    ASSERT(strstr(instr, "get_code_snippet") != NULL);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group D: Pre-Tool Hook Upsert — Claude Code
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_claude_hook_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "PreToolUse") != NULL);
    /* Matcher includes Read for the coverage note (#963). Safe against the
     * issue-#362 gate hazard: the augmenter is structurally non-blocking
     * (always exit 0, additionalContext only). */
    ASSERT(strstr(data, "\"Grep|Glob|Bash|Read\"") != NULL);
    ASSERT(strstr(data, "cbm-code-discovery-gate") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* issue #384: the PreToolUse gate shim must never use a predictable /tmp
 * filename (the old `/tmp/cbm-code-discovery-gate-$PPID` was a symlink-attack
 * vector). The shim is now a stateless wrapper around the compiled augmenter. */
TEST(cli_hook_gate_script_no_predictable_tmp_issue384) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-gate-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_install_hook_gate_script(tmpdir, "/usr/local/bin/code-cortex-mcp");

    char script_path[512];
    snprintf(script_path, sizeof(script_path), "%s/.claude/hooks/cbm-code-discovery-gate", tmpdir);
    const char *data = read_test_file(script_path);
    ASSERT_NOT_NULL(data);
    /* No predictable temp/state file and no PPID-derived path. */
    ASSERT(strstr(data, "/tmp") == NULL);
    ASSERT(strstr(data, "PPID") == NULL);
    /* It delegates to the stateless compiled augmenter (stdout only). */
    ASSERT(strstr(data, "hook-augment") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_claude_hook_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    /* Pre-existing settings with other hooks */
    write_test_file(settingspath,
                    "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo firewall\"}]}]}}");

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Our hook added with the current matcher (Read included for #963). */
    ASSERT(strstr(data, "\"Grep|Glob|Bash|Read\"") != NULL);
    /* Existing hook preserved */
    ASSERT(strstr(data, "Bash") != NULL);
    ASSERT(strstr(data, "firewall") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_claude_hook_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    /* Pre-existing CMM hook with old message */
    write_test_file(settingspath,
                    "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Grep|Glob|Read\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo old-cmm-message\"}]}]}}");

    int rc = cbm_upsert_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Old message gone, new hook script path present */
    ASSERT(strstr(data, "old-cmm-message") == NULL);
    ASSERT(strstr(data, "cbm-code-discovery-gate") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_claude_hook_preserves_others) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(settingspath,
                    "{\"apiKey\":\"sk-123\","
                    "\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo guard\"}]}]}}");

    cbm_upsert_claude_hooks(settingspath);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Non-hook settings preserved */
    ASSERT(strstr(data, "apiKey") != NULL);
    ASSERT(strstr(data, "sk-123") != NULL);
    /* Bash hook preserved */
    ASSERT(strstr(data, "Bash") != NULL);
    ASSERT(strstr(data, "guard") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* #1082: agents run far more searches through `rg`/`grep`/`git grep` than
 * through the Grep tool, so the PreToolUse matcher covers Bash too. The
 * extractor must read one unambiguous pattern out of the command line and
 * FAIL CLOSED on anything else — a wrong pattern would augment with symbols
 * the agent never asked about. It never executes or rewrites the command. */
TEST(cli_hook_augment_bash_pattern_extractor) {
    char out[256];

    /* common forms */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("rg -n CreateStripeCheckout .", out,
                                                                sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("grep -rn CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("grep -e CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("ag CreateStripeCheckout src/", out,
                                                                sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("git grep CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* value-taking flags are skipped correctly */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("grep -A 5 CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("rg -t py CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* env-var prefix and wrappers */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "FOO=bar rg CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rtk grep -n CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "tokf run rg CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "env FOO=bar rg CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* rtk -l <N> shadows grep's valueless -l with a value-taking form, so the
     * pattern position is ambiguous — bail out rather than guess. */
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rtk grep -l 80 CreateStripeCheckout .", out, sizeof(out)));

    /* bail-out cases */
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing("grep -f /path/patterns .", out,
                                                                 sizeof(out)));
    ASSERT_FALSE(
        cbm_hook_augment_parse_bash_pattern_for_testing("grep -e FOO -e BAR .", out, sizeof(out)));
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing("ls -la", out, sizeof(out)));
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing("", out, sizeof(out)));
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing(NULL, out, sizeof(out)));

    /* -- end-of-flags separator */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing("grep -- CreateStripeCheckout .",
                                                                out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* A long option whose value is a SEPARATE token must consume that value.
     * Skipping the option alone handed the value to the pattern slot, so
     * `rg --glob '*.cpp' Symbol .` searched the graph for "*.cpp" and injected
     * context for a symbol the agent never asked about. */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rg --glob *.cpp CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rg --type py CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "grep --include *.c -rn CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* --opt=value carries its own value, so the next token is still the pattern. */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rg --glob=*.cpp CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* Valueless long options stay transparent. */
    ASSERT_TRUE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rg --ignore-case --line-number CreateStripeCheckout .", out, sizeof(out)));
    ASSERT_STR_EQ(out, "CreateStripeCheckout");

    /* An unrecognised long option could take a value or not, so the pattern
     * position is undecidable — decline rather than guess. */
    ASSERT_FALSE(cbm_hook_augment_parse_bash_pattern_for_testing(
        "rg --some-future-flag CreateStripeCheckout .", out, sizeof(out)));

    /* A value-taking long option with nothing after it is equally undecidable. */
    ASSERT_FALSE(
        cbm_hook_augment_parse_bash_pattern_for_testing("rg --glob", out, sizeof(out)));

    PASS();
}

TEST(cli_remove_claude_hooks) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-hook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    /* Install then remove */
    cbm_upsert_claude_hooks(settingspath);
    int rc = cbm_remove_claude_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "Grep|Glob|Bash|Read") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group D: Pre-Tool Hook Upsert — Gemini CLI / Antigravity
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_upsert_gemini_hook_fresh) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "BeforeTool") != NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_gemini_hook_existing) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(settingspath,
                    "{\"hooks\":{\"BeforeTool\":[{\"matcher\":\"shell\","
                    "\"hooks\":[{\"type\":\"command\",\"command\":\"echo guard\"}]}]}}");

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    /* Our hook added */
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);
    /* Existing hook preserved */
    ASSERT(strstr(data, "shell") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_upsert_gemini_hook_replace) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);
    write_test_file(
        settingspath,
        "{\"hooks\":{\"BeforeTool\":[{\"matcher\":\"google_search|read_file|grep_search\","
        "\"hooks\":[{\"type\":\"command\",\"command\":\"echo old-cmm\"}]}]}}");

    int rc = cbm_upsert_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "old-cmm") == NULL);
    ASSERT(strstr(data, "code-cortex-mcp") != NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_remove_gemini_hooks) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-ghook-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    char settingspath[512];
    snprintf(settingspath, sizeof(settingspath), "%s/settings.json", tmpdir);

    cbm_upsert_gemini_hooks(settingspath);
    int rc = cbm_remove_gemini_hooks(settingspath);
    ASSERT_EQ(rc, 0);

    const char *data = read_test_file(settingspath);
    ASSERT_NOT_NULL(data);
    ASSERT(strstr(data, "code-cortex-mcp") == NULL);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group E: Skill descriptions use directive pattern
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_skill_descriptions_directive) {
    /* Verify skill description has trigger phrases for agent matching */
    const cbm_skill_t *sk = cbm_get_skills();
    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        ASSERT(strstr(sk[i].content, "Triggers on:") != NULL);
        ASSERT(strstr(sk[i].content, "search_graph") != NULL);
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group F: Config store (persistent key-value)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(cli_config_open_close) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    cbm_config_close(cfg);

    /* DB file should exist */
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/_config.db", tmpdir);
    struct stat st;
    ASSERT_EQ(stat(dbpath, &st), 0);

    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_set) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    /* Default when key doesn't exist */
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "default");

    /* Set and get */
    ASSERT_EQ(cbm_config_set(cfg, "foo", "bar"), 0);
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "bar");

    /* Overwrite */
    ASSERT_EQ(cbm_config_set(cfg, "foo", "baz"), 0);
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "default"), "baz");

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_bool) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    /* Default */
    ASSERT_FALSE(cbm_config_get_bool(cfg, "auto_index", false));
    ASSERT_TRUE(cbm_config_get_bool(cfg, "auto_index", true));

    /* true variants */
    cbm_config_set(cfg, "k1", "true");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k1", false));
    cbm_config_set(cfg, "k2", "1");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k2", false));
    cbm_config_set(cfg, "k3", "on");
    ASSERT_TRUE(cbm_config_get_bool(cfg, "k3", false));

    /* false variants */
    cbm_config_set(cfg, "k4", "false");
    ASSERT_FALSE(cbm_config_get_bool(cfg, "k4", true));
    cbm_config_set(cfg, "k5", "0");
    ASSERT_FALSE(cbm_config_get_bool(cfg, "k5", true));

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_get_int) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 50000);

    cbm_config_set(cfg, "limit", "20000");
    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 20000);

    /* Non-numeric → default */
    cbm_config_set(cfg, "limit", "abc");
    ASSERT_EQ(cbm_config_get_int(cfg, "limit", 50000), 50000);

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_delete) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);

    cbm_config_set(cfg, "foo", "bar");
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", ""), "bar");

    cbm_config_delete(cfg, "foo");
    ASSERT_STR_EQ(cbm_config_get(cfg, "foo", "gone"), "gone");

    cbm_config_close(cfg);
    test_rmdir_r(tmpdir);
    PASS();
}

TEST(cli_config_persists) {
    /* Values survive close + reopen */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-cfg-XXXXXX");
    if (!cbm_mkdtemp(tmpdir))
        FAIL("cbm_mkdtemp failed");

    cbm_config_t *cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    cbm_config_set(cfg, "auto_index", "true");
    cbm_config_close(cfg);

    /* Reopen */
    cfg = cbm_config_open(tmpdir);
    ASSERT_NOT_NULL(cfg);
    ASSERT_TRUE(cbm_config_get_bool(cfg, "auto_index", false));
    cbm_config_close(cfg);

    test_rmdir_r(tmpdir);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Group H: cbm_replace_binary (update command helper)
 * ═══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32

TEST(replace_binary_overwrites_readonly) {
    /* Simulate #114: existing binary has mode 0500 (no write permission).
     * cbm_replace_binary must unlink first, then create with 0755. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-replace-XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/test-binary", tmpdir);

    /* Create a read-only file (simulating an installed binary with 0500) */
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs("old-content", f);
    fclose(f);
    th_make_executable(path); /* r-x------ */

    /* Replace it with new content */
    const unsigned char new_data[] = "new-content-replaced";
    int rc = cbm_replace_binary(path, new_data, (int)sizeof(new_data) - 1, 0755);
    ASSERT_EQ(rc, 0);

    /* Verify new content was written */
    FILE *check = fopen(path, "r");
    ASSERT_NOT_NULL(check);
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);
    ASSERT_STR_EQ(buf, "new-content-replaced");

    /* Verify permissions are 0755 */
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0755);

    remove(path);
    rmdir(tmpdir);
    PASS();
}

TEST(replace_binary_creates_new_file) {
    /* If no existing file, cbm_replace_binary should create it. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cli-replace2-XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/new-binary", tmpdir);

    const unsigned char data[] = "brand-new";
    int rc = cbm_replace_binary(path, data, (int)sizeof(data) - 1, 0755);
    ASSERT_EQ(rc, 0);

    FILE *check = fopen(path, "r");
    ASSERT_NOT_NULL(check);
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, check);
    fclose(check);
    ASSERT_STR_EQ(buf, "brand-new");

    remove(path);
    rmdir(tmpdir);
    PASS();
}

#endif /* _WIN32 */

/* ═══════════════════════════════════════════════════════════════════
 *  Suite definition
 * ═══════════════════════════════════════════════════════════════════ */

SUITE(cli) {
    /* Version (2 tests — selfupdate_test.go) */
    RUN_TEST(cli_compare_versions);
    RUN_TEST(cli_version_get_set);

    /* Shell RC detection (5 tests — install_test.go) */
    RUN_TEST(cli_detect_shell_rc_zsh);
    RUN_TEST(cli_detect_shell_rc_bash);
    RUN_TEST(cli_detect_shell_rc_bash_with_bashrc);
    RUN_TEST(cli_detect_shell_rc_fish);
    RUN_TEST(cli_detect_shell_rc_default);

    /* CLI binary detection (3 tests — install_test.go) */
    RUN_TEST(cli_find_cli_not_found);
    RUN_TEST(cli_find_cli_on_path);
    RUN_TEST(cli_find_cli_fallback_paths);

    /* Dry-run flag parsing (1 test — install_test.go) */
    RUN_TEST(cli_dry_run_flags);

    /* Skill management (7 tests — install_test.go) */
    RUN_TEST(cli_skill_creation);
    RUN_TEST(cli_skill_idempotent);
    RUN_TEST(cli_skill_force_overwrite);
    RUN_TEST(cli_uninstall_removes_skills);
    RUN_TEST(cli_remove_old_monolithic_skill);
    RUN_TEST(cli_skill_files_content);
    RUN_TEST(cli_codex_instructions);

    /* Editor MCP: Cursor/Windsurf/Gemini (5 tests — install_test.go) */
    RUN_TEST(cli_editor_mcp_install);
    RUN_TEST(cli_editor_mcp_idempotent);
    RUN_TEST(cli_editor_mcp_preserves_others);
    RUN_TEST(cli_editor_mcp_uninstall);
    RUN_TEST(cli_editor_mcp_migrates_legacy_key);
    RUN_TEST(cli_editor_mcp_uninstall_removes_legacy_key);
    RUN_TEST(cli_codex_mcp_migrates_legacy_section);
    RUN_TEST(cli_codex_mcp_migrates_legacy_subtables);
    RUN_TEST(cli_codex_mcp_upsert_keeps_tool_subtables);
    RUN_TEST(cli_codex_mcp_uninstall_removes_subtables);
    RUN_TEST(cli_migrate_legacy_install);
    RUN_TEST(cli_gemini_mcp_install);

    /* VS Code MCP (2 tests — install_test.go) */
    RUN_TEST(cli_vscode_mcp_install);
    RUN_TEST(cli_vscode_mcp_uninstall);

    /* Zed MCP (3 tests — install_test.go) */
    RUN_TEST(cli_zed_mcp_install);
    RUN_TEST(cli_zed_mcp_preserves_settings);
    RUN_TEST(cli_zed_mcp_uninstall);
    RUN_TEST(cli_zed_mcp_jsonc_comments);

    /* PATH management (3 tests) */
    RUN_TEST(cli_ensure_path_append);
    RUN_TEST(cli_ensure_path_already_present);
    RUN_TEST(cli_ensure_path_dry_run);
    RUN_TEST(cli_ensure_path_fish_syntax_issue319);

    /* File copy (2 tests — update_test.go) */
    RUN_TEST(cli_copy_file);
    RUN_TEST(cli_copy_file_source_not_found);

    /* Tar.gz extraction (3 tests — update_test.go) */
    RUN_TEST(cli_extract_binary_from_targz);
    RUN_TEST(cli_extract_binary_from_targz_not_found);
    RUN_TEST(cli_extract_binary_from_targz_invalid_data);
    RUN_TEST(cli_extract_binary_from_zip);
    RUN_TEST(cli_extract_binary_from_zip_not_found);
    RUN_TEST(cli_extract_binary_from_zip_path_traversal);
    RUN_TEST(cli_extract_binary_from_zip_invalid);

    /* Dry-run lifecycle (2 tests) */
    RUN_TEST(cli_install_dry_run);
    RUN_TEST(cli_uninstall_dry_run);

    /* Full lifecycle (1 test — cli_test.go) */
    RUN_TEST(cli_install_and_uninstall);

    /* YAML parser (7 unit tests) */
    RUN_TEST(cli_yaml_parse_simple);
    RUN_TEST(cli_yaml_parse_nested);
    RUN_TEST(cli_yaml_parse_list);
    RUN_TEST(cli_yaml_parse_bool);
    RUN_TEST(cli_yaml_parse_comments);
    RUN_TEST(cli_yaml_parse_empty);
    RUN_TEST(cli_yaml_has);

    /* Agent detection (6 tests — group A) */
    RUN_TEST(cli_detect_agents_finds_claude);
    RUN_TEST(cli_detect_agents_finds_claude_via_env);
    RUN_TEST(cli_detect_agents_finds_codex);
    RUN_TEST(cli_detect_agents_finds_cursor_issue222);
    RUN_TEST(cli_install_plan_receipt_no_mutation_issue388);
    RUN_TEST(cli_codex_session_hook_issue330);
    RUN_TEST(cli_codex_hook_migrates_legacy_block);
    RUN_TEST(cli_codex_hook_preserves_foreign_tables);
    RUN_TEST(cli_gemini_session_hook_parity);
    RUN_TEST(cli_detect_agents_finds_gemini);
    RUN_TEST(cli_detect_agents_finds_zed);
    RUN_TEST(cli_detect_agents_finds_antigravity);
    RUN_TEST(cli_detect_agents_finds_kilocode);
    RUN_TEST(cli_detect_agents_finds_kiro);
    RUN_TEST(cli_detect_agents_none_found);

    /* Codex MCP config upsert (3 tests — group B) */
    RUN_TEST(cli_upsert_codex_mcp_fresh);
    RUN_TEST(cli_upsert_codex_mcp_existing);
    RUN_TEST(cli_upsert_codex_mcp_replace);

    /* Zed MCP format fix (1 test — group B) */
    RUN_TEST(cli_zed_mcp_uses_args_format);

    /* OpenCode MCP config upsert (2 tests — group B) */
    RUN_TEST(cli_upsert_opencode_mcp_fresh);
    RUN_TEST(cli_upsert_opencode_mcp_existing);

    /* Antigravity MCP config upsert (2 tests — group B) */
    RUN_TEST(cli_upsert_antigravity_mcp_fresh);
    RUN_TEST(cli_upsert_antigravity_mcp_replace);

    /* Instructions file upsert (6 tests — group C) */
    RUN_TEST(cli_upsert_instructions_fresh);
    RUN_TEST(cli_upsert_instructions_existing);
    RUN_TEST(cli_upsert_instructions_replace);
    RUN_TEST(cli_upsert_instructions_no_duplicate);
    RUN_TEST(cli_remove_instructions);
    RUN_TEST(cli_agent_instructions_content);

    /* Claude Code hooks (5 tests — group D) */
    RUN_TEST(cli_hook_gate_script_no_predictable_tmp_issue384);
    RUN_TEST(cli_upsert_claude_hook_fresh);
    RUN_TEST(cli_upsert_claude_hook_existing);
    RUN_TEST(cli_upsert_claude_hook_replace);
    RUN_TEST(cli_upsert_claude_hook_preserves_others);
    RUN_TEST(cli_hook_augment_bash_pattern_extractor);
    RUN_TEST(cli_remove_claude_hooks);

    /* Gemini CLI hooks (4 tests — group D) */
    RUN_TEST(cli_upsert_gemini_hook_fresh);
    RUN_TEST(cli_upsert_gemini_hook_existing);
    RUN_TEST(cli_upsert_gemini_hook_replace);
    RUN_TEST(cli_remove_gemini_hooks);

    /* Skill directive descriptions (1 test — group E) */
    RUN_TEST(cli_skill_descriptions_directive);

    /* Config store (6 tests — group F) */
    RUN_TEST(cli_config_open_close);
    RUN_TEST(cli_config_get_set);
    RUN_TEST(cli_config_get_bool);
    RUN_TEST(cli_config_get_int);
    RUN_TEST(cli_config_delete);
    RUN_TEST(cli_config_persists);

    /* Replace binary (update command helper — group H) */
#ifndef _WIN32
    RUN_TEST(replace_binary_overwrites_readonly);
    RUN_TEST(replace_binary_creates_new_file);
#endif
}
