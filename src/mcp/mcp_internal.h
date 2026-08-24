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

/* Internal command builder, exposed so tests can pin the PowerShell pipeline
 * ORDERING without starting an external shell — the Windows branch cannot be
 * exercised end-to-end from a POSIX CI host. */
void cbm_search_code_build_grep_cmd(char *cmd, size_t cmd_sz, bool use_regex, bool scoped,
                                    const char *file_pattern, const char *tmpfile,
                                    const char *filelist, const char *root_path);

#ifdef __cplusplus
}
#endif

#endif /* CBM_MCP_INTERNAL_H */
