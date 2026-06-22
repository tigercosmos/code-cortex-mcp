/*
 * git_context.h — Resolve git/worktree metadata for an indexed repo root.
 *
 * Captures whether a path is a git repo, whether it is a linked worktree,
 * the canonical (shared) repo root, the current branch (and a filesystem-safe
 * slug), HEAD/base SHAs, and a stable "branch root" qualified name used to
 * hang the structure graph off a Branch node.
 *
 * All git lookups go through cbm_popen with the repo path validated by
 * cbm_validate_shell_arg() before interpolation.
 *
 * Depends on: foundation (compat_fs, str_util, constants)
 */
#ifndef CBM_GIT_CONTEXT_H
#define CBM_GIT_CONTEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_git;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
} cbm_git_context_t;

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out);
void cbm_git_context_free(cbm_git_context_t *ctx);
char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx);
int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif /* CBM_GIT_CONTEXT_H */
