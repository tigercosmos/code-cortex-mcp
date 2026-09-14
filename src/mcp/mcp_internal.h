/*
 * mcp_internal.h — white-box hooks into src/mcp/mcp.cpp.
 *
 * This header is INTERNAL and is not part of the MCP API: it exists so tests
 * can exercise decision logic directly, without the git/subprocess/index
 * plumbing that surrounds it in the handlers. Nothing outside src/mcp and
 * tests/ should include it.
 */
#ifndef CBM_MCP_INTERNAL_H
#define CBM_MCP_INTERNAL_H

#include "mcp/mcp.h"
#include "pipeline/pipeline.h" /* cbm_changed_hunk_t */
#include "store/store.h"       /* cbm_node_t */

#ifdef __cplusplus
extern "C" {
#endif

/* detect_changes seed scoping (#1363): does `node`'s line range overlap any
 * recorded hunk for `file`? A one-line edit inside a single method used to
 * seed every other definition in the file, producing an impact report an
 * order of magnitude larger than the edit actually touched. Exposed here so
 * the overlap logic has a direct unit test. */
bool cbm_detect_node_in_hunks(const cbm_node_t *node, const cbm_changed_hunk_t *hunks,
                              int hunk_count, const char *file);

/* search_code Windows pre-scan optimization: only simple suffix globs can be
 * moved ahead of Select-String without changing the existing full-path
 * PowerShell -like contract. Exposed for direct boundary tests only. */
bool cbm_search_code_file_pattern_can_prefilter(const char *file_pattern);

/* Background release checks are enabled unless CBM_UPDATE_CHECK=0. Exposed
 * for a direct environment-contract test without starting a network thread. */
bool cbm_mcp_update_check_enabled(void);

/* Internal command builder, exposed so tests can pin the PowerShell pipeline
 * ORDERING without starting an external shell — the Windows branch cannot be
 * exercised end-to-end from a POSIX CI host. */
void cbm_search_code_build_grep_cmd(char *cmd, size_t cmd_sz, bool use_regex, bool scoped,
                                    const char *file_pattern, const char *tmpfile,
                                    const char *filelist, const char *root_path);

/* auto_index admission guard (#713): true when root_path holds at most
 * file_limit indexable files under the full discovery policy, git checkout or
 * not. *file_count_out receives the exact count, file_limit + 1 past the limit,
 * or -1 when the root could not be counted. */
bool cbm_mcp_auto_index_within_file_limit(const char *root_path, int file_limit,
                                          int *file_count_out);

/* CBM_INDEX_MAX_RESTARTS as the supervised index resolves it: 100 by default
 * and for an unreadable value, 0 means no restarts. Exposed for tests. */
int cbm_index_restart_cap_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif /* CBM_MCP_INTERNAL_H */
