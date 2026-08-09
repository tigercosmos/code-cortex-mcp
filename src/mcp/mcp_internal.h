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

#ifdef __cplusplus
}
#endif

#endif /* CBM_MCP_INTERNAL_H */
