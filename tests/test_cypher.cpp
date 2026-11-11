/*
 * test_cypher.c — Tests for the Cypher query engine.
 *
 * Ported from internal/cypher/cypher_test.go (1016 LOC).
 * Covers lexer, parser, and end-to-end execution.
 */
#include "test_framework.h"
#include <cypher/cypher.h>
#include <store/store.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <foundation/compat_thread.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ══════════════════════════════════════════════════════════════════
 *  LEXER TESTS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_lex_simple_match) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("MATCH (n:Function)", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);

    /* MATCH ( n : Function ) EOF */
    ASSERT_GTE(r.count, 6);
    ASSERT_EQ(r.tokens[0].type, TOK_MATCH);
    ASSERT_EQ(r.tokens[1].type, TOK_LPAREN);
    ASSERT_EQ(r.tokens[2].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[2].text, "n");
    ASSERT_EQ(r.tokens[3].type, TOK_COLON);
    ASSERT_EQ(r.tokens[4].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[4].text, "Function");
    ASSERT_EQ(r.tokens[5].type, TOK_RPAREN);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_relationship) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("-[:CALLS]->", &r);
    ASSERT_EQ(rc, 0);

    /* - [ : CALLS ] - > EOF */
    ASSERT_GTE(r.count, 7);
    ASSERT_EQ(r.tokens[0].type, TOK_DASH);
    ASSERT_EQ(r.tokens[1].type, TOK_LBRACKET);
    ASSERT_EQ(r.tokens[2].type, TOK_COLON);
    ASSERT_EQ(r.tokens[3].type, TOK_IDENT);
    ASSERT_STR_EQ(r.tokens[3].text, "CALLS");
    ASSERT_EQ(r.tokens[4].type, TOK_RBRACKET);
    ASSERT_EQ(r.tokens[5].type, TOK_DASH);
    ASSERT_EQ(r.tokens[6].type, TOK_GT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_string_literal) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("\"hello world\"", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 1);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    ASSERT_STR_EQ(r.tokens[0].text, "hello world");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_single_quote_string) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("'hello'", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    ASSERT_STR_EQ(r.tokens[0].text, "hello");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_string_overflow) {
    /* Build a string literal longer than 4096 bytes to verify we don't
     * overflow the stack buffer in lex_string_literal. */
    const int big = 5000;
    /* query: "AAAA...A"  (quotes included) */
    char *query = (char *)malloc(big + 3); /* quote + big chars + quote + NUL */
    ASSERT_NOT_NULL(query);
    query[0] = '"';
    memset(query + 1, 'A', big);
    query[big + 1] = '"';
    query[big + 2] = '\0';

    cbm_lex_result_t r = {0};
    int rc = cbm_lex(query, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_GTE(r.count, 1);
    ASSERT_EQ(r.tokens[0].type, TOK_STRING);
    /* The string should be truncated to CBM_SZ_4K - 1 (4095) characters. */
    ASSERT_EQ((int)strlen(r.tokens[0].text), 4095);

    cbm_lex_free(&r);
    free(query);
    PASS();
}

TEST(cypher_lex_number) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("42 3.14", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[0].text, "42");
    ASSERT_EQ(r.tokens[1].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[1].text, "3.14");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_operators) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("= =~ >= <= ..", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 5);
    ASSERT_EQ(r.tokens[0].type, TOK_EQ);
    ASSERT_EQ(r.tokens[1].type, TOK_EQTILDE);
    ASSERT_EQ(r.tokens[2].type, TOK_GTE);
    ASSERT_EQ(r.tokens[3].type, TOK_LTE);
    ASSERT_EQ(r.tokens[4].type, TOK_DOTDOT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_keywords_case_insensitive) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("match WHERE Return limit", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.tokens[0].type, TOK_MATCH);
    ASSERT_EQ(r.tokens[1].type, TOK_WHERE);
    ASSERT_EQ(r.tokens[2].type, TOK_RETURN);
    ASSERT_EQ(r.tokens[3].type, TOK_LIMIT);

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_pipe_and_star) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("[:TYPE1|TYPE2*1..3]", &r);
    ASSERT_EQ(rc, 0);

    /* [ : TYPE1 | TYPE2 * 1 .. 3 ] */
    ASSERT_GTE(r.count, 9);
    ASSERT_EQ(r.tokens[3].type, TOK_PIPE);
    ASSERT_EQ(r.tokens[5].type, TOK_STAR);
    ASSERT_EQ(r.tokens[6].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[6].text, "1");
    ASSERT_EQ(r.tokens[7].type, TOK_DOTDOT);
    ASSERT_EQ(r.tokens[8].type, TOK_NUMBER);
    ASSERT_STR_EQ(r.tokens[8].text, "3");

    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_full_query) {
    const char *q = "MATCH (f:Function)-[:CALLS]->(g:Function) "
                    "WHERE f.name =~ \".*Order.*\" "
                    "RETURN f.name, g.name LIMIT 10";
    cbm_lex_result_t r = {0};
    int rc = cbm_lex(q, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    /* Should have many tokens; just check it doesn't crash */
    ASSERT_GT(r.count, 20);

    cbm_lex_free(&r);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PARSER TESTS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_parse_simple_node) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(err);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).node_count, 1);
    ASSERT_EQ(cbm_query_pattern(q).rel_count, 0);
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].variable, "f");
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].label, "Function");

    cbm_query_free(q);
    PASS();
}

/* Upstream 25a7aacc: trailing input must be an error, never a silent drop. The
 * parser used to stop at the first thing it did not understand and report
 * success, so the engine answered from the fragment it had parsed. */
TEST(cypher_parse_rejects_trailing_tokens) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name AS n BANANA SPLIT 99", &q, &err);
    ASSERT_NEQ(rc, 0);
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(q);
    /* Names what stopped the parse, and does not blame WITH (there is none). */
    ASSERT(strstr(err, "BANANA") != NULL);
    ASSERT(strstr(err, "WITH") == NULL);
    free(err);
    PASS();
}

/* Only one WITH is supported. A second one used to take the filter and the
 * RETURN with it, so every row came back unfiltered. */
TEST(cypher_parse_rejects_second_with_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) "
                              "OPTIONAL MATCH (a)-[:CALLS]->(f) "
                              "WITH f, count(a) AS calls "
                              "OPTIONAL MATCH (b)-[:USAGE]->(f) "
                              "WITH f, calls, count(b) AS usages "
                              "WHERE calls = 0 AND usages = 0 "
                              "RETURN f.name AS n",
                              &q, &err);
    ASSERT_NEQ(rc, 0);
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(q);
    ASSERT(strstr(err, "only one WITH clause is supported") != NULL);
    free(err);
    PASS();
}

/* The guard must not reject a finished query, nor a UNION whose branch was
 * parsed by a separate sub-parser. */
TEST(cypher_parse_accepts_single_with_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) "
                              "WITH f, f.name AS n "
                              "WHERE n = 'buildTree' "
                              "RETURN n",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(err);
    ASSERT_NOT_NULL(q);
    cbm_query_free(q);

    rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name UNION MATCH (m:Method) RETURN m.name",
                          &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(err);
    ASSERT_NOT_NULL(q);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_outbound) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS]->(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).node_count, 2);
    ASSERT_EQ(cbm_query_pattern(q).rel_count, 1);
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[0], "CALLS");
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "outbound");
    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 1);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_inbound) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)<-[:CALLS]-(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "inbound");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_relationship_any) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS]-(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].direction, "any");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_variable_length) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function)-[:CALLS*1..3]->(g:Function)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 3);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_variable_length_unbounded) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS*]->(g)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].min_hops, 1);
    ASSERT_EQ(cbm_query_pattern(q).rels[0].max_hops, 0); /* 0 = unbounded */

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_multiple_edge_types) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS|HTTP_CALLS]->(g)", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);

    ASSERT_EQ(cbm_query_pattern(q).rels[0].type_count, 2);
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[0], "CALLS");
    ASSERT_STR_EQ(cbm_query_pattern(q).rels[0].types[1], "HTTP_CALLS");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_clause) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name = \"Foo\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.variable, "f");
    ASSERT_STR_EQ(q->where->root->cond.property, "name");
    ASSERT_STR_EQ(q->where->root->cond.op, "=");
    ASSERT_STR_EQ(q->where->root->cond.value, "Foo");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_regex) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name =~ \".*Order.*\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "=~");
    ASSERT_STR_EQ(q->where->root->cond.value, ".*Order.*");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_where_and) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name = \"A\" AND f.label = \"Function\"",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_AND);
    ASSERT_NOT_NULL(q->where->root->left);
    ASSERT_NOT_NULL(q->where->root->right);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_simple) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name, f.qualified_name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[0].variable, "f");
    ASSERT_STR_EQ(q->ret->items[0].property, "name");
    ASSERT_STR_EQ(q->ret->items[1].property, "qualified_name");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_count) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) RETURN f.name, COUNT(g) AS cnt", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_NOT_NULL(q->ret->items[1].func);
    ASSERT_STR_EQ(q->ret->items[1].func, "COUNT");
    ASSERT_STR_EQ(q->ret->items[1].alias, "cnt");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_order_limit) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) RETURN f.name ORDER BY f.name DESC LIMIT 5", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->order_key_count, 1);
    ASSERT_STR_EQ(q->ret->order_keys[0], "f.name");
    ASSERT(q->ret->order_descs[0]);
    ASSERT_EQ(q->ret->limit, 5);

    cbm_query_free(q);
    PASS();
}

/* #1334: every ORDER BY key is parsed (per-key direction) and the LIMIT that
 * follows the key list is consumed instead of silently dropped. */
TEST(cypher_parse_multikey_order_by_issue1334) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f:Function) RETURN f.name ORDER BY f.complexity DESC, f.name ASC LIMIT 5", &q,
        &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->order_key_count, 2);
    ASSERT_STR_EQ(q->ret->order_keys[0], "f.complexity");
    ASSERT(q->ret->order_descs[0]);
    ASSERT(!q->ret->order_descs[1]);
    ASSERT_STR_EQ(q->ret->order_keys[1], "f.name");
    ASSERT_EQ(q->ret->limit, 5);

    cbm_query_free(q);
    PASS();
}

/* #1334: more keys than the modeled maximum is a loud parse error - the old
 * failure mode (ignore the remainder, drop the LIMIT) must never come back. */
TEST(cypher_parse_order_by_over_cap_rejected_issue1334) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name ORDER BY "
                              "f.a, f.b, f.c, f.d, f.e, f.f, f.g, f.h, f.i LIMIT 5",
                              &q, &err);
    ASSERT(rc != 0);
    free(err);
    PASS();
}

/* #1994: a non-numeric SKIP/LIMIT operand must be a loud parse error too. The old
 * failure mode was the same one #1334 banned by a different route: expect()
 * returned NULL, the clause was dropped, and the query still reported success
 * with limit left at its -1 "no LIMIT" sentinel - so query_graph answered a
 * bounded query with the entire result set. Cypher parameters ($limit) are the
 * everyday trigger: they are what a Neo4j-shaped client writes by default. */
TEST(cypher_parse_nonnumeric_limit_rejected_issue1994) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name LIMIT $limit", &q, &err);
    ASSERT(rc != 0);
    free(err);
    PASS();
}

TEST(cypher_parse_nonnumeric_skip_rejected_issue1994) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    /* The orphaned operand also swallowed the LIMIT that followed it. */
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name SKIP $offset LIMIT 10", &q, &err);
    ASSERT(rc != 0);
    free(err);
    PASS();
}

TEST(cypher_parse_word_limit_operand_rejected_issue1994) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name LIMIT abc", &q, &err);
    ASSERT(rc != 0);
    free(err);
    PASS();
}

/* Control: a well-formed SKIP/LIMIT still parses and still carries its values. */
TEST(cypher_parse_numeric_skip_limit_still_accepted) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN f.name SKIP 2 LIMIT 10", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->skip, 2);
    ASSERT_EQ(q->ret->limit, 10);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_return_distinct) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) RETURN DISTINCT f.label", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT(q->ret->distinct);

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_inline_props) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function {name: \"Foo\"})", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cbm_query_pattern(q).nodes[0].prop_count, 1);
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].props[0].key, "name");
    ASSERT_STR_EQ(cbm_query_pattern(q).nodes[0].props[0].value, "Foo");

    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_error) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("INVALID QUERY", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    free(err);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  EXECUTION TESTS (end-to-end against store)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: set up the standard test graph.
 * Nodes: HandleOrder, ValidateOrder, SubmitOrder (Function), main (Module), LogError (Function)
 * Edges: HandleOrder→ValidateOrder (CALLS), ValidateOrder→SubmitOrder (CALLS),
 *        HandleOrder→LogError (CALLS), main→HandleOrder (DEFINES)
 */
static cbm_store_t *setup_cypher_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "test.HandleOrder",
                     .file_path = "handler.go",
                     .start_line = 10,
                     .end_line = 30};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "ValidateOrder",
                     .qualified_name = "test.ValidateOrder",
                     .file_path = "validate.go",
                     .start_line = 5,
                     .end_line = 15};
    cbm_node_t n3 = {.project = "test",
                     .label = "Function",
                     .name = "SubmitOrder",
                     .qualified_name = "test.SubmitOrder",
                     .file_path = "submit.go"};
    cbm_node_t n4 = {
        .project = "test", .label = "Module", .name = "main", .qualified_name = "test.main"};
    cbm_node_t n5 = {.project = "test",
                     .label = "Function",
                     .name = "LogError",
                     .qualified_name = "test.LogError",
                     .file_path = "log.go"};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    int64_t id2 = cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);
    int64_t id4 = cbm_store_upsert_node(s, &n4);
    int64_t id5 = cbm_store_upsert_node(s, &n5);

    cbm_edge_t e1 = {.project = "test", .source_id = id1, .target_id = id2, .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = id2, .target_id = id3, .type = "CALLS"};
    cbm_edge_t e3 = {.project = "test", .source_id = id1, .target_id = id5, .type = "CALLS"};
    cbm_edge_t e4 = {.project = "test", .source_id = id4, .target_id = id1, .type = "DEFINES"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);
    cbm_store_insert_edge(s, &e4);

    return s;
}

TEST(cypher_exec_match_all_functions) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* HandleOrder, ValidateOrder, SubmitOrder, LogError */
    ASSERT_GT(r.col_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_eq) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #797: variable-length / repeated-variable path semantics. Fixture:
 * loopy has a SELF-LOOP as one of its outbound CALLS edges plus a real
 * 2-chain loopy->mid->leaf. Correct openCypher semantics:
 *  - a repeated node variable must unify: (a)-[:CALLS]->(a) matches ONLY
 *    the self-loop, not every edge;
 *  - relationship uniqueness within a path: the self-loop cannot be
 *    traversed repeatedly, so no *k..k path exists beyond the real chain;
 *  - the engine hop cap must not fabricate or silently truncate results. */
TEST(cypher_exec_varlength_path_semantics_issue797) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t loopy = {.project = "test",
                        .label = "Function",
                        .name = "loopy",
                        .qualified_name = "test.mod.loopy",
                        .file_path = "mod.go",
                        .start_line = 1,
                        .end_line = 2};
    cbm_node_t mid = {.project = "test",
                      .label = "Function",
                      .name = "mid",
                      .qualified_name = "test.mod.mid",
                      .file_path = "mod.go",
                      .start_line = 3,
                      .end_line = 4};
    cbm_node_t leaf = {.project = "test",
                       .label = "Function",
                       .name = "leaf",
                       .qualified_name = "test.mod.leaf",
                       .file_path = "mod.go",
                       .start_line = 5,
                       .end_line = 6};
    int64_t id_loopy = cbm_store_upsert_node(s, &loopy);
    int64_t id_mid = cbm_store_upsert_node(s, &mid);
    int64_t id_leaf = cbm_store_upsert_node(s, &leaf);
    ASSERT_GT(id_loopy, 0);
    cbm_edge_t self_loop = {
        .project = "test", .source_id = id_loopy, .target_id = id_loopy, .type = "CALLS"};
    cbm_edge_t e1 = {
        .project = "test", .source_id = id_loopy, .target_id = id_mid, .type = "CALLS"};
    cbm_edge_t e2 = {.project = "test", .source_id = id_mid, .target_id = id_leaf, .type = "CALLS"};
    cbm_store_insert_edge(s, &self_loop);
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    /* Bug 1: repeated variable must unify — only the self-loop matches. */
    cbm_cypher_result_t r1 = {0};
    ASSERT_EQ(cbm_cypher_execute(s, "MATCH (a)-[:CALLS]->(a) RETURN a.name", "test", 0, &r1), 0);
    ASSERT_EQ(r1.row_count, 1);
    cbm_cypher_result_free(&r1);

    /* Bug 2: *2..2 from loopy has two relationship-unique trails: the
     * self-loop followed by e1 reaches mid, and e1 followed by e2 reaches leaf.
     * Reusing the self-loop within one trail remains forbidden. */
    cbm_cypher_result_t r2 = {0};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a {name: \"loopy\"})-[:CALLS*2..2]->(b) "
                                 "RETURN DISTINCT b.name",
                                 "test", 0, &r2),
              0);
    ASSERT_EQ(r2.row_count, 2);
    bool saw_mid = false;
    bool saw_leaf = false;
    for (int i = 0; i < r2.row_count; i++) {
        saw_mid |= strcmp(r2.rows[i][0], "mid") == 0;
        saw_leaf |= strcmp(r2.rows[i][0], "leaf") == 0;
    }
    ASSERT_TRUE(saw_mid);
    ASSERT_TRUE(saw_leaf);
    cbm_cypher_result_free(&r2);

    /* Bug 2 amplifier: no directed path of length 5 exists at all. */
    cbm_cypher_result_t r3 = {0};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a {name: \"loopy\"})-[:CALLS*5..5]->(b) "
                                 "RETURN b.name",
                                 "test", 0, &r3),
              0);
    ASSERT_EQ(r3.row_count, 0);
    cbm_cypher_result_free(&r3);

    /* Bug 3: a hop range beyond the engine ceiling must be an ADVERTISED
     * clamp, not silently indistinguishable from "no such path". */
    cbm_cypher_result_t r4 = {0};
    ASSERT_EQ(
        cbm_cypher_execute(s, "MATCH (a)-[:CALLS*150..150]->(b) RETURN b.name", "test", 0, &r4), 0);
    ASSERT_EQ(r4.row_count, 0);
    ASSERT_NOT_NULL(r4.warning);
    ASSERT_NOT_NULL(strstr(r4.warning, "clamped"));
    cbm_cypher_result_free(&r4);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_regex) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name =~ \".*Order.*\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3); /* HandleOrder, ValidateOrder, SubmitOrder */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* An `=~` pattern over the regex wrapper's compile-size budget is refused
 * before the platform compiler expands it; the comparison then matches nothing,
 * as for any pattern that cannot be compiled, the result carries a warning
 * naming the reason, and the engine keeps answering. The pattern is a 509-way
 * alternation whose first branch is `.`: once compiled it matches every name,
 * and it costs 509 + 509*509/8 = 32,894 units, just over the budget. */
TEST(cypher_exec_where_regex_oversized_pattern_matches_nothing) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    char query[1200];
    char *w = query;
    w += snprintf(w, sizeof(query), "MATCH (f:Function) WHERE f.name =~ \"(.");
    for (int i = 0; i < 508; i++) {
        *w++ = '|';
        *w++ = (char)('a' + i % 26);
    }
    snprintf(w, (size_t)(query + sizeof(query) - w), ")\"");

    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);
    ASSERT_NOT_NULL(r.warning);
    ASSERT_NOT_NULL(strstr(r.warning, "too large to compile"));
    cbm_cypher_result_free(&r);

    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name =~ \".*Order.*\"", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 3);
    ASSERT_NULL(r2.warning);
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_contains) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name CONTAINS \"Order\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_starts_with) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name STARTS WITH \"Handle\"", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_return_properties) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, f.qualified_name, f.file_path",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.col_count, 3);
    /* Columns should be f.name, f.qualified_name, f.file_path */
    ASSERT_STR_EQ(r.columns[0], "f.name");
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[0][1], "test.HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* ── Scalar / introspection functions (full-suite Tier 1) ──────── */

TEST(cypher_func_labels) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN labels(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "[\"Function\"]");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_type) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function)-[r:CALLS]->(g:Function) RETURN type(r) LIMIT 1", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_id) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN id(f)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    /* id is a non-empty numeric string */
    ASSERT_TRUE(r.rows[0][0][0] >= '0' && r.rows[0][0][0] <= '9');
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_keys) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN keys(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_TRUE(strstr(r.rows[0][0], "\"name\"") != NULL);
    ASSERT_TRUE(strstr(r.rows[0][0], "\"qualified_name\"") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_properties) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN properties(f)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.rows[0][0][0], '{'); /* a JSON object */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_tointeger_tofloat) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN toInteger(f.start_line), toFloat(f.start_line)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10"); /* start_line = 10 */
    ASSERT_STR_EQ(r.rows[0][1], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_size_reverse) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "RETURN size(f.name), length(f.name), reverse(f.name)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "8"); /* "LogError" has 8 chars */
    ASSERT_STR_EQ(r.rows[0][1], "8");
    ASSERT_STR_EQ(r.rows[0][2], "rorrEgoL");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_func_multiarg) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "RETURN substring(f.name, 0, 6), left(f.name, 6), "
                                "right(f.name, 5), replace(f.name, \"Order\", \"Req\"), "
                                "coalesce(f.missing, \"fallback\")",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "Handle");    /* substring("HandleOrder",0,6) */
    ASSERT_STR_EQ(r.rows[0][1], "Handle");    /* left(...,6) */
    ASSERT_STR_EQ(r.rows[0][2], "Order");     /* right("HandleOrder",5) */
    ASSERT_STR_EQ(r.rows[0][3], "HandleReq"); /* replace Order->Req */
    ASSERT_STR_EQ(r.rows[0][4], "fallback");  /* coalesce: f.missing empty -> literal */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exists_no_callers) {
    /* NOT EXISTS { (f)<-[:CALLS]-() } → functions with no CALLS caller.
     * HandleOrder has only an incoming DEFINES edge (not CALLS), so it is the
     * sole match — proving EXISTS is edge-type-specific (in_degree=1 here). */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT EXISTS { (f)<-[:CALLS]-() } RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exists_has_outgoing_calls) {
    /* EXISTS { (f)-[:CALLS]->() } → functions that call something.
     * HandleOrder (→ValidateOrder, →LogError) and ValidateOrder (→SubmitOrder). */
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE EXISTS { (f)-[:CALLS]->() } RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s,
                            "MATCH (a)-[:CALLS]->(b) WHERE b.name <> \"SubmitOrder\" "
                            "RETURN a.name, b.name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s,
                            "MATCH (a)-[:CALLS]->(b) WHERE b.name = \"SubmitOrder\" OR "
                            "NOT b.name = \"SubmitOrder\" RETURN count(*)",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "3");
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s,
                            "MATCH (a)-[:CALLS]->(b) WITH a, b WHERE NOT b.name = "
                            "\"SubmitOrder\" RETURN a.name, b.name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_calls_relationship) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder→ValidateOrder, HandleOrder→LogError, ValidateOrder→SubmitOrder */
    ASSERT_EQ(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_calls_with_where) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* →ValidateOrder, →LogError */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_inbound) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)<-[:CALLS]-(g:Function) "
                                "WHERE f.name = \"ValidateOrder\" "
                                "RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* HandleOrder calls ValidateOrder */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_count) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "RETURN f.name, COUNT(g) AS cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder→2, ValidateOrder→1 */
    ASSERT_EQ(r.row_count, 2);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_bound_terminal_count_ignores_unbound) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) OPTIONAL MATCH (t)-[:CALLS]->(f) "
                                "RETURN f.name, COUNT(t) AS cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);

    for (int i = 0; i < r.row_count; i++) {
        const char *name = r.rows[i][0];
        const char *count = r.rows[i][1];
        if (strcmp(name, "HandleOrder") == 0) {
            ASSERT_STR_EQ(count, "0");
        } else if (strcmp(name, "ValidateOrder") == 0 || strcmp(name, "SubmitOrder") == 0 ||
                   strcmp(name, "LogError") == 0) {
            ASSERT_STR_EQ(count, "1");
        } else {
            ASSERT_TRUE(false);
        }
    }

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* A label or EXISTS predicate on the variable the seed step has not bound
 * yet must not discard the seeds: `a:Function` with only `b` bound used to
 * be false, so both spellings below answered nothing (the second one before
 * this branch, the first once the planner started seeding from `b`). */
static int cypher_rows_for(const char *query, char out[][64], int cap) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    int n = rc == 0 ? r.row_count : -1;
    for (int i = 0; i < n && i < cap; i++) {
        snprintf(out[i], 64, "%s", r.rows[i][0]);
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return n;
}

TEST(cypher_exec_label_on_unbound_side_keeps_seeds) {
    char rows[8][64];
    int n = cypher_rows_for(
        "MATCH (a)-[:CALLS]->(b) WHERE a:Function AND b.name = 'ValidateOrder' RETURN a.name",
        rows, 8);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(rows[0], "HandleOrder");

    n = cypher_rows_for("MATCH (a)-[:CALLS]->(b) WHERE b:Function AND a.name = 'HandleOrder' "
                        "RETURN b.name ORDER BY b.name",
                        rows, 8);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(rows[0], "LogError");
    ASSERT_STR_EQ(rows[1], "ValidateOrder");

    n = cypher_rows_for(
        "MATCH (a)-[:CALLS]->(b) WHERE NOT a:Module AND b.name = 'SubmitOrder' RETURN a.name",
        rows, 8);
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(rows[0], "ValidateOrder");

    /* Control: the deferred label still filters once both ends are bound. */
    n = cypher_rows_for(
        "MATCH (a)-[:CALLS]->(b) WHERE a:Module AND b.name = 'ValidateOrder' RETURN a.name", rows,
        8);
    ASSERT_EQ(n, 0);
    PASS();
}

TEST(cypher_exec_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_order_by) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    /* Alphabetical: HandleOrder, LogError, SubmitOrder, ValidateOrder */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "LogError");
    ASSERT_STR_EQ(r.rows[2][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[3][0], "ValidateOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_variable_length) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    /* HandleOrder →CALLS→ ValidateOrder →CALLS→ SubmitOrder (2 hops) */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS*1..3]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* Should find: ValidateOrder (1 hop), SubmitOrder (2 hops), LogError (1 hop) */
    ASSERT_GTE(r.row_count, 3);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Pin the relationship-trail contract: a self-loop edge cannot be reused
 * within one variable-length trail, so *2..2 yields no fabricated match. */
TEST(cypher_exec_var_length_no_reuse_self_loop) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "Recursive",
                    .qualified_name = "test.Recursive",
                    .file_path = "recursive.go"};
    int64_t id = cbm_store_upsert_node(s, &n);
    cbm_edge_t e = {.project = "test", .source_id = id, .target_id = id, .type = "CALLS"};
    cbm_store_insert_edge(s, &e);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: \"Recursive\"})-[:CALLS*2..2]"
                                "->(g:Function) RETURN g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_var_length_truncation_surfaces_warning) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    int64_t ids[18];
    for (int i = 0; i < 18; i++) {
        char name[16];
        snprintf(name, sizeof(name), "node-%d", i);
        cbm_node_t node = {.project = "test",
                           .label = "Function",
                           .name = name,
                           .qualified_name = name,
                           .file_path = "graph.c"};
        ids[i] = cbm_store_upsert_node(s, &node);
    }
    for (int source = 0; source < 17; source++) {
        for (int target = source + 1; target < 18; target++) {
            cbm_edge_t edge = {.project = "test",
                               .source_id = ids[source],
                               .target_id = ids[target],
                               .type = "CALLS"};
            cbm_store_insert_edge(s, &edge);
        }
    }

    cbm_cypher_result_t result = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function {name: \"node-0\"})-[:CALLS*10..10]->"
                                "(b:Function) RETURN b.name",
                                "test", 0, &result);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(result.warning);
    ASSERT_TRUE(strstr(result.warning, "traversal budget") != NULL);
    ASSERT_TRUE(result.row_count > 0);

    cbm_cypher_result_free(&result);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_defines_edge) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (m:Module)-[:DEFINES]->(f:Function) "
                                "RETURN m.name, f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "main");
    ASSERT_STR_EQ(r.rows[0][1], "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_no_results) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.name = \"NonExistent\"", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_numeric) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.start_line > \"8\" "
                                "RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder starts at 10 */
    ASSERT_GTE(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestExecuteDistinct --- */
TEST(cypher_exec_distinct) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN DISTINCT f.label", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* All 4 Function nodes share label "Function" → 1 distinct row */
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* issue #238: WITH DISTINCT must deduplicate projected rows (previously the
 * DISTINCT keyword on WITH was parsed but silently ignored). */
TEST(cypher_exec_with_distinct_issue238) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    /* 4 Function nodes all share label "Function" → WITH DISTINCT collapses to
     * one row; without dedup this returned 4. */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WITH DISTINCT f.label AS lbl RETURN lbl",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);

    /* Control: without DISTINCT, all 4 rows flow through. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) WITH f.label AS lbl RETURN lbl", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 4);
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* issue #241: label tests in WHERE clauses (openCypher `WHERE n:Label`) —
 * previously a parse error. */
TEST(cypher_exec_where_label_test_issue241) {
    cbm_store_t *s = setup_cypher_store();

    /* f:Function is true for all 4 Function nodes. */
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE f:Function RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    cbm_cypher_result_free(&r);

    /* f:Class matches none of the functions. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f:Class RETURN f.name", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 0);
    cbm_cypher_result_free(&r2);

    /* Negated label test: NOT f:Class is always true. */
    cbm_cypher_result_t r3 = {0};
    rc =
        cbm_cypher_execute(s, "MATCH (f:Function) WHERE NOT f:Class RETURN f.name", "test", 0, &r3);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r3.row_count, 4);
    cbm_cypher_result_free(&r3);

    cbm_store_close(s);
    PASS();
}

/* issue #239: COUNT(DISTINCT x) — previously a parse error. */
TEST(cypher_exec_count_distinct_issue239) {
    cbm_store_t *s = setup_cypher_store();

    /* 4 functions all share label "Function" → COUNT(DISTINCT f.label) = 1. */
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(DISTINCT f.label)", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "1");
    cbm_cypher_result_free(&r);

    /* Non-distinct COUNT counts all 4 occurrences. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(f.label)", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(r2.rows[0][0], "4");
    cbm_cypher_result_free(&r2);

    /* DISTINCT over the 4 unique function names = 4. */
    cbm_cypher_result_t r3 = {0};
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN count(DISTINCT f.name)", "test", 0, &r3);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(r3.rows[0][0], "4");
    cbm_cypher_result_free(&r3);

    cbm_store_close(s);
    PASS();
}

/* Upstream #1919 (afc948ba): a variable the WITH dropped must be REFUSED, not
 * projected as an empty column that reads as "the graph holds no such data". */
TEST(cypher_rejects_projection_of_dropped_with_var_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function)-[:CALLS]->(g) WITH f.name AS caller RETURN caller, g.name", "test",
        0, &r);
    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "'g'") != NULL);
    ASSERT_TRUE(strstr(r.error, "scope") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* An OPTIONAL MATCH target that did not match is still DECLARED, so it stays
 * legal and projects "" — the line the check must not cross. */
TEST(cypher_optional_match_target_still_allowed_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN f.name, g.name", "test", 0,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_TRUE(r.row_count > 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* An alias the WITH created, and a name carried through whole, stay in scope. */
TEST(cypher_with_alias_stays_in_scope_issue1919) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g) WITH f.name AS caller, g AS "
                                "callee RETURN caller, callee.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_TRUE(r.row_count > 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* issue #373: an unsupported computed expression in WITH/RETURN (an unknown
 * function like split(...) or list indexing [..]) must FAIL LOUDLY with a clear
 * "unsupported function" error rather than silently projecting an empty column
 * (which looks like a valid-but-blank result and hides the real problem). */
TEST(cypher_exec_unsupported_func_errors_issue373) {
    cbm_store_t *s = setup_cypher_store();

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WITH split(f.name)[0] AS top, count(*) AS c RETURN top, c", "test",
        0, &r);
    ASSERT_TRUE(rc != 0); /* unsupported function now fails loudly */
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "unsupported") != NULL);
    ASSERT_TRUE(strstr(r.error, "split") != NULL);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* A recognised function still works, and an unknown one in plain RETURN errors. */
TEST(cypher_exec_unknown_func_return_errors) {
    cbm_store_t *s = setup_cypher_store();

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN nosuchfunc(f.name)", "test", 0, &r);
    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "unsupported function") != NULL);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* issue #242: openCypher label alternation in MATCH — (n:A|B). */
TEST(cypher_exec_label_alternation_issue242) {
    cbm_store_t *s = setup_cypher_store();

    /* Store has 4 Function + 1 Module node → alternation seeds all 5. */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n:Function|Module) RETURN n.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    cbm_cypher_result_free(&r);

    /* Alternation with a non-existent label still returns the existing one. */
    cbm_cypher_result_t r2 = {0};
    rc = cbm_cypher_execute(s, "MATCH (n:Function|Class) RETURN n.name", "test", 0, &r2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r2.row_count, 4);
    cbm_cypher_result_free(&r2);

    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestExecuteInlinePropertyFilter --- */
TEST(cypher_exec_inline_props) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: \"SubmitOrder\"}) "
                                "RETURN f.name, f.qualified_name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereStartsWith --- */
TEST(cypher_parse_where_starts_with) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) WHERE f.name STARTS WITH \"Send\" RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "STARTS WITH");
    ASSERT_STR_EQ(q->where->root->cond.value, "Send");
    cbm_query_free(q);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereContains --- */
TEST(cypher_parse_where_contains) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f:Function) WHERE f.name CONTAINS \"Handler\" RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "CONTAINS");
    ASSERT_STR_EQ(q->where->root->cond.value, "Handler");
    cbm_query_free(q);
    PASS();
}

/* --- Ported from cypher_test.go: TestParseWhereNumericComparison --- */
TEST(cypher_parse_where_numeric) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.start_line > 10 RETURN f", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, ">");
    ASSERT_STR_EQ(q->where->root->cond.value, "10");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  EDGE PROPERTY TESTS (ported from cypher_test.go Feature 2)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: set up store with HTTP_CALLS edge having properties.
 * Creates same graph as setup_cypher_store + one HTTP_CALLS edge. */
static cbm_store_t *setup_cypher_http_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "test.main.HandleOrder",
                     .file_path = "main.go",
                     .start_line = 10,
                     .end_line = 30};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "ValidateOrder",
                     .qualified_name = "test.service.ValidateOrder",
                     .file_path = "service.go",
                     .start_line = 5,
                     .end_line = 20};
    cbm_node_t n3 = {.project = "test",
                     .label = "Function",
                     .name = "SubmitOrder",
                     .qualified_name = "test.service.SubmitOrder",
                     .file_path = "service.go",
                     .start_line = 25,
                     .end_line = 50};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);

    cbm_edge_t http = {
        .project = "test",
        .source_id = id1,
        .target_id = id3,
        .type = "HTTP_CALLS",
        .properties_json =
            "{\"url_path\":\"/api/orders\",\"confidence\":0.85,\"method\":\"POST\"}"};
    cbm_store_insert_edge(s, &http);

    return s;
}

/* Helper: set up store with TWO HTTP_CALLS edges for filtering tests. */
static cbm_store_t *setup_cypher_multi_edge_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "testproj", "/tmp/test");

    cbm_node_t n1 = {.project = "testproj",
                     .label = "Function",
                     .name = "SendOrder",
                     .qualified_name = "testproj.caller.SendOrder",
                     .file_path = "caller/client.go"};
    cbm_node_t n2 = {.project = "testproj",
                     .label = "Function",
                     .name = "HandleOrder",
                     .qualified_name = "testproj.handler.HandleOrder",
                     .file_path = "handler/routes.go"};
    cbm_node_t n3 = {.project = "testproj",
                     .label = "Function",
                     .name = "HandleHealth",
                     .qualified_name = "testproj.handler.HandleHealth",
                     .file_path = "handler/health.go"};

    int64_t id1 = cbm_store_upsert_node(s, &n1);
    int64_t id2 = cbm_store_upsert_node(s, &n2);
    int64_t id3 = cbm_store_upsert_node(s, &n3);

    cbm_edge_t e1 = {.project = "testproj",
                     .source_id = id1,
                     .target_id = id2,
                     .type = "HTTP_CALLS",
                     .properties_json =
                         "{\"url_path\":\"/api/orders\",\"confidence\":0.85,\"method\":\"POST\"}"};
    cbm_edge_t e2 = {.project = "testproj",
                     .source_id = id1,
                     .target_id = id3,
                     .type = "HTTP_CALLS",
                     .properties_json = "{\"url_path\":\"/health\",\"confidence\":0.45}"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);

    return s;
}

/* Helper: find a column value in a cypher result row */
static const char *cypher_get_col(const cbm_cypher_result_t *r, int row, const char *col) {
    for (int c = 0; c < r->col_count; c++) {
        if (strcmp(r->columns[c], col) == 0)
            return r->rows[row][c];
    }
    return NULL;
}

/* Helper: check if any row has a column matching a value */
static bool cypher_has_row_with(const cbm_cypher_result_t *r, const char *col, const char *val) {
    int ci = -1;
    for (int c = 0; c < r->col_count; c++) {
        if (strcmp(r->columns[c], col) == 0) {
            ci = c;
            break;
        }
    }
    if (ci < 0)
        return false;
    for (int row = 0; row < r->row_count; row++) {
        if (strcmp(r->rows[row][ci], val) == 0)
            return true;
    }
    return false;
}

TEST(cypher_edge_prop_access) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function)-[r:HTTP_CALLS]->(b:Function) "
                                "RETURN a.name, b.name, r.url_path, r.confidence",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "a.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "SubmitOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.confidence"), "0.85");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_prop_in_where) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    /* confidence > 0.8 → should match */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence > 0.8 "
                                "RETURN a.name, b.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);

    /* confidence > 0.9 → should NOT match */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence > 0.9 "
                            "RETURN a.name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_type_prop) {
    cbm_store_t *s = setup_cypher_http_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (a)-[r:HTTP_CALLS]->(b) RETURN r.type", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.type"), "HTTP_CALLS");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_contains) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path CONTAINS 'orders' "
                                "RETURN a.name, b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "a.name"), "SendOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_numeric_gte) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence >= 0.6 "
                                "RETURN a.name, b.name, r.confidence LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_bare_edge_return_exposes_properties_json) {
    /* `RETURN r` on an edge variable, with no property accessor, should
     * surface the edge's full properties JSON (or "{}"). Before the fix,
     * binding_get_virtual returned an empty string, which made bare edge
     * returns useless for callers that wanted to inspect timestamps,
     * weights, etc. without naming each property up front. */
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'POST' RETURN r",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    const char *r_val = cypher_get_col(&r, 0, "r");
    ASSERT_NOT_NULL(r_val);
    /* Expect JSON object content rather than the previous empty string. */
    ASSERT_NOT_NULL(strstr(r_val, "url_path"));
    ASSERT_NOT_NULL(strstr(r_val, "/api/orders"));

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_return_without_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) "
                                "RETURN a.name, b.name, r.url_path, r.confidence LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.row_count, 2);
    ASSERT(cypher_has_row_with(&r, "r.url_path", "/api/orders"));
    ASSERT(cypher_has_row_with(&r, "r.url_path", "/health"));

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_equals) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'POST' "
                                "RETURN a.name, b.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_starts_with) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path STARTS WITH '/api' "
                                "RETURN a.name, b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_combined_node_and_edge_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function)-[r:HTTP_CALLS]->(b:Function) "
                                "WHERE a.name = 'SendOrder' AND r.confidence >= 0.6 "
                                "RETURN b.name, r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "r.url_path"), "/api/orders");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_no_match) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* No edge has method = 'DELETE' */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.method = 'DELETE' "
                                "RETURN a.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_numeric_lt) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* Only health edge (0.45) should match confidence < 0.5 */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.confidence < 0.5 "
                                "RETURN b.name, r.confidence",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleHealth");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_filter_regex) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE r.url_path =~ \"/api/.*\" "
                                "RETURN b.name",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(cypher_get_col(&r, 0, "b.name"), "HandleOrder");

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_edge_builtin_type_filter) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};

    /* Untyped rel [r] — filter on r.type in WHERE */
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r]->(b) WHERE r.type = 'HTTP_CALLS' "
                                "RETURN a.name, b.name LIMIT 20",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* Both HTTP_CALLS edges */

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Ported from cypher_test.go: TestApplyLimitRespectsExplicit */
TEST(cypher_apply_limit) {
    /* Create store with many nodes */
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "lim", "/tmp/lim");

    for (int i = 0; i < 50; i++) {
        char name[32], qn[64];
        snprintf(name, sizeof(name), "func%d", i);
        snprintf(qn, sizeof(qn), "lim.func%d", i);
        cbm_node_t n = {.project = "lim",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "test.go"};
        cbm_store_upsert_node(s, &n);
    }

    /* LIMIT 5 → 5 rows */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 5", "lim", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    cbm_cypher_result_free(&r);

    /* No LIMIT, max_rows=10 → capped at 10 */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "lim", 10, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 10);
    cbm_cypher_result_free(&r);

    /* LIMIT above max_rows → explicit limit wins */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name LIMIT 30", "lim", 10, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 30);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 1: SIMPLE OPERATORS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_lex_neq_operators) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("<> !=", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_NEQ);
    ASSERT_EQ(r.tokens[1].type, TOK_NEQ);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_ends_keyword) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("ENDS WITH", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 2);
    ASSERT_EQ(r.tokens[0].type, TOK_ENDS);
    ASSERT_EQ(r.tokens[1].type, TOK_WITH);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_lex_in_is_null) {
    cbm_lex_result_t r = {0};
    int rc = cbm_lex("IN IS NULL", &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.count, 3);
    ASSERT_EQ(r.tokens[0].type, TOK_IN);
    ASSERT_EQ(r.tokens[1].type, TOK_IS);
    ASSERT_EQ(r.tokens[2].type, TOK_NULL_KW);
    cbm_lex_free(&r);
    PASS();
}

TEST(cypher_exec_where_neq) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name <> \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3); /* ValidateOrder, SubmitOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_neq_bang) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name != \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_ends_with) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name ENDS WITH \"Order\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* HandleOrder, ValidateOrder, SubmitOrder */
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT f.name = \"HandleOrder\" RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* The early WHERE pass runs on seed rows where only the first alias is
 * bound; a NOT on the target node or the relationship must not prune every
 * row (upstream #2250). */
TEST(cypher_exec_where_not_target_node) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[:CALLS]->(b) WHERE NOT b.name = \"SubmitOrder\" "
                                "RETURN a.name, b.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_target_relationship) {
    cbm_store_t *s = setup_cypher_multi_edge_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[r:HTTP_CALLS]->(b) WHERE NOT r.url_path = "
                                "\"/health\" RETURN r.url_path",
                                "testproj", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "/api/orders");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_source_node) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[:CALLS]->(b) WHERE NOT a.name = "
                                "\"HandleOrder\" RETURN a.name, b.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_target_node_count) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a)-[:CALLS]->(b) WHERE NOT b.name = "
                                "\"SubmitOrder\" RETURN count(*)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "2");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_in) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f) WHERE f.label IN [\"Function\", \"Module\"] RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5); /* 4 Functions + 1 Module */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_in) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f) WHERE NOT f.label IN [\"Module\"] RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* 4 Functions only */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_is_null) {
    /* SubmitOrder has no start_line (defaults to 0, so start_line prop = "0") */
    /* But file_path is set for all. Use a node with missing data. */
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "WithFile",
                     .qualified_name = "test.WithFile",
                     .file_path = "a.go"};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "NoFile",
                     .qualified_name = "test.NoFile",
                     .file_path = NULL};
    cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.file_path IS NULL RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* NoFile has NULL file_path */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_is_not_null) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "WithFile",
                     .qualified_name = "test.WithFile",
                     .file_path = "a.go"};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "NoFile",
                     .qualified_name = "test.NoFile",
                     .file_path = NULL};
    cbm_store_upsert_node(s, &n1);
    cbm_store_upsert_node(s, &n2);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) WHERE f.file_path IS NOT NULL RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* WithFile */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_return_star) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN * LIMIT 3", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    /* Should have columns: f.name, f.qualified_name, f.label, f.file_path */
    ASSERT_EQ(r.col_count, 4);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 63b99976: f is named in the MATCH and again in the OPTIONAL MATCH,
 * so eight columns is right and twelve is the fault. */
TEST(cypher_return_star_dedups_repeated_pattern_var) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN *",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 8);
    ASSERT_STR_EQ(r.columns[0], "f.name");
    ASSERT_STR_EQ(r.columns[4], "g.name");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* After a WITH the live scope is the aliases it made; RETURN * used to read the
 * pattern, find neither f nor g, and answer every value empty. */
TEST(cypher_return_star_after_with_names_aliases) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g) "
                                "WITH f.name AS caller, g.name AS callee RETURN *",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 2);
    ASSERT_STR_EQ(r.columns[0], "caller");
    ASSERT_STR_EQ(r.columns[1], "callee");
    ASSERT_TRUE(r.row_count > 0);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_TRUE(r.rows[i][0][0] != '\0');
        ASSERT_TRUE(r.rows[i][1][0] != '\0');
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Upstream 426e415f: a binding carries CYP_MAX_VARS (16) WITH items; a 20-alias
 * WITH used to answer 16 columns with no error. Refuse it; 16 still works. */
TEST(cypher_wide_with_refused_not_truncated) {
    char query[1024];
    int off = snprintf(query, sizeof(query), "MATCH (f:Function) WITH ");
    for (int i = 0; i < 20; i++) {
        off +=
            snprintf(query + off, sizeof(query) - (size_t)off, "%sf.name AS c%d", i ? ", " : "", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN *");
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &r) != 0);
    cbm_cypher_result_free(&r);

    off = snprintf(query, sizeof(query), "MATCH (f:Function) WITH ");
    for (int i = 0; i < 16; i++) {
        off +=
            snprintf(query + off, sizeof(query) - (size_t)off, "%sf.name AS c%d", i ? ", " : "", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN *");
    cbm_cypher_result_t r16 = {0};
    ASSERT_EQ(cbm_cypher_execute(s, query, "test", 0, &r16), 0);
    ASSERT_EQ(r16.col_count, 16);
    cbm_cypher_result_free(&r16);
    cbm_store_close(s);
    PASS();
}

/* "MATCH (a0:NoSuchLabelXYZ)-[:CALLS]->(a1)-…->(aN-1)": matches nothing, so any
 * query built on it is instant. */
static void build_node_chain(char *buf, size_t buf_sz, int nodes) {
    int off = snprintf(buf, buf_sz, "MATCH (a0:NoSuchLabelXYZ)");
    for (int i = 1; i < nodes; i++) {
        off += snprintf(buf + off, buf_sz - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
}

/* Upstream 04ba2fa3 (#1995): binding_set drops the 17th node variable without a
 * word. Refuse such a query, naming the limit; 16 still runs. */
TEST(cypher_wide_pattern_refused) {
    cbm_store_t *s = setup_cypher_store();
    char query[2048];
    build_node_chain(query, sizeof(query), 20);
    strncat(query, " RETURN a0.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "node") != NULL);
    cbm_cypher_result_free(&wide);

    build_node_chain(query, sizeof(query), 16);
    strncat(query, " RETURN a0.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t ok = {0};
    ASSERT_EQ(cbm_cypher_execute(s, query, "test", 0, &ok), 0);
    cbm_cypher_result_free(&ok);
    cbm_store_close(s);
    PASS();
}

/* Same on the edge table, where binding_set_edge stops at CYP_MAX_EDGE_VARS. */
TEST(cypher_wide_edge_pattern_refused) {
    cbm_store_t *s = setup_cypher_store();
    char query[2048];
    int off = snprintf(query, sizeof(query), "MATCH (a0:NoSuchLabelXYZ)");
    for (int i = 1; i <= 9; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[r%d:CALLS]->(a%d)", i, i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "edge") != NULL);
    cbm_cypher_result_free(&wide);
    cbm_store_close(s);
    PASS();
}

/* Upstream 98899ba5: the unnamed head is bound as "_n0" and takes a slot, so an
 * unnamed head plus 16 named nodes needs 17. */
TEST(cypher_unnamed_head_takes_a_slot) {
    cbm_store_t *s = setup_cypher_store();
    char query[2048];
    int off = snprintf(query, sizeof(query), "MATCH (:NoSuchLabelXYZ)");
    for (int i = 0; i < 16; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    ASSERT_TRUE(strstr(wide.error, "node") != NULL);
    cbm_cypher_result_free(&wide);

    off = snprintf(query, sizeof(query), "MATCH (:NoSuchLabelXYZ)");
    for (int i = 0; i < 15; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "-[:CALLS]->(a%d)", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, " RETURN a0.name");
    cbm_cypher_result_t ok = {0};
    ASSERT_EQ(cbm_cypher_execute(s, query, "test", 0, &ok), 0);
    cbm_cypher_result_free(&ok);
    cbm_store_close(s);
    PASS();
}

/* #1995: the out-of-scope refusal used to switch itself off past 32 declared
 * names. Both the narrow and the wide query must be refused. */
TEST(cypher_scope_check_survives_wide_pattern) {
    cbm_store_t *s = setup_cypher_store();
    char query[4096];
    build_node_chain(query, sizeof(query), 10);
    strncat(query, " RETURN zzz.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t narrow = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &narrow) != 0);
    ASSERT_NOT_NULL(narrow.error);
    ASSERT_TRUE(strstr(narrow.error, "zzz") != NULL);
    cbm_cypher_result_free(&narrow);

    build_node_chain(query, sizeof(query), 35);
    strncat(query, " RETURN zzz.name", sizeof(query) - strlen(query) - 1);
    cbm_cypher_result_t wide = {0};
    ASSERT_TRUE(cbm_cypher_execute(s, query, "test", 0, &wide) != 0);
    ASSERT_NOT_NULL(wide.error);
    cbm_cypher_result_free(&wide);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_neq) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f:Function) WHERE f.name <> \"X\"", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "<>");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_in) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) WHERE f.label IN [\"Function\", \"Module\"]", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_CONDITION);
    ASSERT_STR_EQ(q->where->root->cond.op, "IN");
    ASSERT_EQ(q->where->root->cond.in_value_count, 2);
    ASSERT_STR_EQ(q->where->root->cond.in_values[0], "Function");
    ASSERT_STR_EQ(q->where->root->cond.in_values[1], "Module");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_is_null) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) WHERE f.file_path IS NULL", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_STR_EQ(q->where->root->cond.op, "IS NULL");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 2: EXPRESSION TREE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_where_or) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) WHERE f.name = \"HandleOrder\" OR f.name = \"LogError\" RETURN f.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_complex_bool) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* (name CONTAINS "Order" OR name = "LogError") AND label = "Function" */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f) WHERE (f.name CONTAINS \"Order\" OR f.name = "
                                "\"LogError\") AND f.label = \"Function\" "
                                "RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4); /* HandleOrder, ValidateOrder, SubmitOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_xor) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* name CONTAINS "Handle" XOR name CONTAINS "Order" → XOR = true when exactly one is true
     * HandleOrder: both true → false
     * ValidateOrder: false, true → true
     * SubmitOrder: false, true → true
     * LogError: false, false → false */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name CONTAINS \"Handle\" XOR f.name "
                                "CONTAINS \"Order\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* ValidateOrder, SubmitOrder */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_not_prefix) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE NOT (f.name CONTAINS \"Order\") RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_expr_tree_and_or) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) WHERE f.a = \"1\" AND f.b = \"2\" OR f.c = \"3\"", &q, &err);
    ASSERT_EQ(rc, 0);
    /* Precedence: AND binds tighter than OR → root is OR */
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_OR);
    ASSERT_EQ(q->where->root->left->type, EXPR_AND);
    ASSERT_EQ(q->where->root->right->type, EXPR_CONDITION);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_expr_tree_nested) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) WHERE (f.a = \"1\" OR f.b = \"2\") AND f.c = \"3\"", &q, &err);
    ASSERT_EQ(rc, 0);
    /* Parens override precedence: root is AND, left is OR */
    ASSERT_NOT_NULL(q->where->root);
    ASSERT_EQ(q->where->root->type, EXPR_AND);
    ASSERT_EQ(q->where->root->left->type, EXPR_OR);
    ASSERT_EQ(q->where->root->right->type, EXPR_CONDITION);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 3: UNSUPPORTED KEYWORD ERRORS
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_error_create) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("CREATE (n:Node {name: \"X\"})", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "CREATE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_delete) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("DELETE n", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "DELETE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_set) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("SET n.name = \"X\"", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "SET") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_merge) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MERGE (n:Node)", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "MERGE") != NULL);
    free(err);
    PASS();
}

TEST(cypher_error_call) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("CALL db.labels()", &q, &err);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT(strstr(err, "CALL") != NULL);
    free(err);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 4: SKIP + GENERALIZED AGGREGATION
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_skip) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC SKIP 2",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* 4 functions ordered: HandleOrder, LogError, SubmitOrder, ValidateOrder → skip 2 = 2 */
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_skip_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) RETURN f.name ORDER BY f.name ASC SKIP 1 LIMIT 2", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    ASSERT_STR_EQ(r.rows[1][0], "SubmitOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Regression for #1334: a LIMIT must survive a multi-key ORDER BY. The parser
 * used to stop at the first sort key, leaving ", key2 ... LIMIT n" unconsumed —
 * the whole result set came back (6326 rows instead of 5 on the reporter's
 * graph: a token-flood into agent context). */
TEST(cypher_exec_multikey_order_by_keeps_limit_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: HandleOrder=10, ValidateOrder=5, SubmitOrder=0, LogError=0 */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) RETURN f.name, f.start_line "
                                "ORDER BY f.start_line DESC, f.name ASC LIMIT 2",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1334: the secondary key must actually break ties, per-key direction. */
TEST(cypher_exec_multikey_order_by_tiebreak_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_line ASC puts the two 0-line functions first; name DESC breaks the
     * tie: SubmitOrder before LogError. */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) RETURN f.name, f.start_line "
                                "ORDER BY f.start_line ASC, f.name DESC LIMIT 2",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "SubmitOrder");
    ASSERT_STR_EQ(r.rows[1][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1334: the WITH-clause pipeline has the same multi-key ORDER BY contract. */
TEST(cypher_exec_with_multikey_order_by_keeps_limit_issue1334) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WITH f.name AS n, f.start_line AS sl "
                                "ORDER BY sl DESC, n ASC LIMIT 2 RETURN n",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_sum) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: HandleOrder=10, ValidateOrder=5, SubmitOrder=0, LogError=0 → sum=15 */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN SUM(f.start_line) AS total", "test",
                                0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "15");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_avg) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* start_lines: 10, 5, 0, 0 → avg = 3.75 */
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN AVG(f.start_line) AS avg_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "3.75");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_min) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* Among functions with nonzero: HandleOrder=10, ValidateOrder=5 → but MIN is 0 from others */
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN MIN(f.start_line) AS mn", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "0");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_max) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s, "MATCH (f:Function) RETURN MAX(f.start_line) AS mx", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_collect) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WHERE f.name = \"HandleOrder\" "
                                "RETURN f.name, COLLECT(g.name) AS callees",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    /* Should be a JSON array like ["ValidateOrder","LogError"] */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT(strstr(r.rows[0][1], "ValidateOrder") != NULL);
    ASSERT(strstr(r.rows[0][1], "LogError") != NULL);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_count_star) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN COUNT(*) AS n", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "4");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1111: type(r) grouped with count(*) must return the actual relationship type,
 * not the row count. ret_agg_build_key/ret_agg_emit_row classified aggregate vs.
 * scalar columns with a bare `item->func` truthy check, so type(r) (a non-aggregate
 * function, func != NULL) was misrouted into the aggregate-value branch and
 * formatted via format_agg_value's default case, silently substituting the row
 * count for the relationship type. */
TEST(cypher_issue1111_return_type_count_group) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (a)-[r]->(b) RETURN type(r) AS t, count(*) AS n ORDER BY n DESC", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    ASSERT_STR_EQ(r.rows[0][1], "3");
    ASSERT_STR_EQ(r.rows[1][0], "DEFINES");
    ASSERT_STR_EQ(r.rows[1][1], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_skip) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN f.name SKIP 5 LIMIT 10", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret);
    ASSERT_EQ(q->ret->skip, 5);
    ASSERT_EQ(q->ret->limit, 10);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_sum_avg) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN SUM(f.x) AS s, AVG(f.y) AS a", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[0].func, "SUM");
    ASSERT_STR_EQ(q->ret->items[0].alias, "s");
    ASSERT_STR_EQ(q->ret->items[1].func, "AVG");
    ASSERT_STR_EQ(q->ret->items[1].alias, "a");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_collect) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) RETURN f.name, COLLECT(g.name) AS names", &q,
                              &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->ret->count, 2);
    ASSERT_STR_EQ(q->ret->items[1].func, "COLLECT");
    ASSERT_STR_EQ(q->ret->items[1].alias, "names");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 5: STRING FUNCTIONS + CASE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_tolower) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toLower(f.name) AS lower_name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "handleorder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_toupper) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toUpper(f.name) AS upper_name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HANDLEORDER");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_tostring) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN toString(f.start_line) AS sl",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "10");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_case) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
        "RETURN CASE WHEN f.start_line > \"5\" THEN \"high\" ELSE \"low\" END AS pos",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "high");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_tolower) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f) RETURN toLower(f.name) AS n", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(q->ret->items[0].func, "toLower");
    ASSERT_STR_EQ(q->ret->items[0].alias, "n");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_case) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f) RETURN CASE WHEN f.x = \"1\" THEN \"a\" ELSE \"b\" END AS val", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->ret->items[0].kase);
    ASSERT_EQ(q->ret->items[0].kase->branch_count, 1);
    ASSERT_STR_EQ(q->ret->items[0].kase->branches[0].then_val, "a");
    ASSERT_STR_EQ(q->ret->items[0].kase->else_val, "b");
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 6: WITH CLAUSE
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_with_rename) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "WITH f.name AS fname RETURN fname",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_count) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "RETURN caller, cnt ORDER BY cnt DESC",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_GTE(r.row_count, 1);
    /* HandleOrder calls 2 (ValidateOrder, LogError), ValidateOrder calls 1 (SubmitOrder) */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    ASSERT_STR_EQ(r.rows[0][1], "2");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1111, WITH variant: the same misrouting in with_agg_build_key/with_agg_accumulate/
 * execute_with_aggregate's per-column func check. */
TEST(cypher_issue1111_with_type_count_group) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (a)-[r]->(b) WITH type(r) AS t, count(*) AS n RETURN t, n ORDER BY n DESC",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "CALLS");
    ASSERT_STR_EQ(r.rows[0][1], "3");
    ASSERT_STR_EQ(r.rows[1][0], "DEFINES");
    ASSERT_STR_EQ(r.rows[1][1], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #1111 follow-up (review from DeusData on #1221): with_agg_find_or_create's
 * bare-node-carry check only tested `!property && variable`, so an entity-
 * introspection alias like `labels(f) AS l` (variable set, property NULL, func
 * set) was ALSO tagged with the source node. A later `l.file_path` then read
 * the carried node and silently returned HandleOrder's real file_path instead
 * of "" for the non-node alias `l`. */
TEST(cypher_issue1111_with_scalar_func_alias_no_node_leak) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "WITH labels(f) AS l, COUNT(*) AS c "
                                "RETURN l, l.file_path, c",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "[\"Function\"]");
    ASSERT_STR_EQ(r.rows[0][1], ""); /* was "handler.go" before the fix */
    ASSERT_STR_EQ(r.rows[0][2], "1");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_where) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "WHERE cnt > \"1\" "
                                "RETURN caller, cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* Only HandleOrder has cnt > 1 (cnt=2) */
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_with_orderby_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function)-[:CALLS]->(g:Function) "
                                "WITH f.name AS caller, COUNT(g) AS cnt "
                                "ORDER BY cnt DESC LIMIT 1 "
                                "RETURN caller, cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_with) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f)-[:CALLS]->(g) WITH f.name AS caller, COUNT(g) AS cnt RETURN caller, cnt", &q,
        &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->with_clause);
    ASSERT_EQ(q->with_clause->count, 2);
    ASSERT_STR_EQ(q->with_clause->items[0].alias, "caller");
    ASSERT_STR_EQ(q->with_clause->items[1].func, "COUNT");
    ASSERT_NOT_NULL(q->ret);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_with_where) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("MATCH (f)-[:CALLS]->(g) WITH f.name AS caller, COUNT(g) AS cnt "
                              "WHERE cnt > \"1\" RETURN caller",
                              &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->with_clause);
    ASSERT_NOT_NULL(q->post_with_where);
    ASSERT_NOT_NULL(q->post_with_where->root);
    ASSERT_NOT_NULL(q->ret);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 7: OPTIONAL MATCH + MULTIPLE MATCH
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_optional_match_no_result) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* LogError has no CALLS outbound edges → OPTIONAL MATCH keeps binding with empty target */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    /* g.name should be empty since OPTIONAL MATCH found nothing */
    ASSERT_STR_EQ(r.rows[0][1], "");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_match_has_result) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "RETURN f.name, g.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2); /* ValidateOrder, LogError */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_multi_match) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* Two MATCH clauses: first finds a module, second finds functions */
    int rc =
        cbm_cypher_execute(s,
                           "MATCH (m:Module) MATCH (f:Function) WHERE f.name CONTAINS \"Order\" "
                           "RETURN m.name, f.name",
                           "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* 1 module × 3 *Order functions = 3 */
    ASSERT_EQ(r.row_count, 3);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_second_match_joins_bound_var) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* `f` is bound by the first MATCH; the second MATCH must treat it as a
     * join constraint, not rebind it (regression: this used to cross-join
     * every scanned `c` and overwrite `f`, returning bogus rows). */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "MATCH (c)-[:CALLS]->(f) "
                                "RETURN c.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1); /* only HandleOrder calls LogError */
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_match_count_skips_null) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* LogError calls nothing → g is unbound on its row; COUNT(g) must skip
     * nulls and yield 0, not count the row itself. */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "WITH f, COUNT(g) AS cnt "
                                "RETURN f.name, cnt",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    ASSERT_STR_EQ(r.rows[0][1], "0");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_match_zero_callers) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* The canonical dead-code query: only HandleOrder has no inbound CALLS.
     * Regression: this shape used to explode into a cross product (OOM on
     * real graphs) and never produced callers = 0 rows. */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) "
                                "OPTIONAL MATCH (c)-[:CALLS]->(f) "
                                "WITH f, COUNT(c) AS callers "
                                "WHERE callers = 0 "
                                "RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_optional_match_empty_min_max) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* LogError calls nothing → g is unbound on every row. MIN/MAX over an
     * all-null aggregate must be null (empty), not the DBL_MAX/-DBL_MAX
     * init sentinels; COUNT stays 0. */
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"LogError\" "
                                "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                                "RETURN COUNT(g), MIN(g.start_line), MAX(g.start_line)",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "0");
    ASSERT_STR_EQ(r.rows[0][1], "");
    ASSERT_STR_EQ(r.rows[0][2], "");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_optional_match) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(
        "MATCH (f:Function) OPTIONAL MATCH (f)-[:CALLS]->(g) RETURN f.name, g.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->pattern_count, 2);
    ASSERT(!q->pattern_optional[0]);
    ASSERT(q->pattern_optional[1]);
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_multi_match) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (a:Module) MATCH (b:Function) RETURN a.name, b.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(q->pattern_count, 2);
    ASSERT(!q->pattern_optional[0]);
    ASSERT(!q->pattern_optional[1]);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 8: UNION / UNION ALL
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_exec_union) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name "
                                "UNION "
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* UNION deduplicates → 1 row */
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_union_all) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name "
                                "UNION ALL "
                                "MATCH (f:Function) WHERE f.name = \"HandleOrder\" RETURN f.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    /* UNION ALL keeps duplicates → 2 rows */
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_parse_union) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("MATCH (f) RETURN f.name UNION ALL MATCH (g) RETURN g.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT(q->union_all);
    ASSERT_NOT_NULL(q->union_next);
    cbm_query_free(q);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PHASE 9: UNWIND
 * ══════════════════════════════════════════════════════════════════ */

TEST(cypher_parse_unwind) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc =
        cbm_cypher_parse("UNWIND [\"a\", \"b\", \"c\"] AS x MATCH (f) RETURN f.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    ASSERT_STR_EQ(q->unwind_alias, "x");
    cbm_query_free(q);
    PASS();
}

TEST(cypher_parse_unwind_var) {
    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse("UNWIND items AS item MATCH (f) RETURN f.name", &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(q->unwind_expr, "items");
    ASSERT_STR_EQ(q->unwind_alias, "item");
    cbm_query_free(q);
    PASS();
}

/* Regression: an UNWIND literal list whose element is longer than the 2KB
 * assembly buffer used to overflow the stack. snprintf reports the length it
 * WOULD have written, so blen ran past sizeof(buf) and the trailing
 * buf[blen++]=']' / buf[blen]='\0' wrote out of bounds (ASan: stack-buffer-
 * overflow). The query text is agent-controlled via the MCP query tool. */
TEST(cypher_parse_unwind_oversized_literal_no_overflow) {
    char query[4096];
    char big[3000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    snprintf(query, sizeof(query), "UNWIND [\"%s\"] AS x MATCH (f) RETURN f.name", big);

    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(query, &q, &err);
    /* Must not crash and must produce a NUL-terminated, in-bounds expression. */
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    ASSERT_STR_EQ(q->unwind_alias, "x");
    cbm_query_free(q);
    PASS();
}

/* Regression: many oversized elements accumulate blen well past the buffer,
 * which also underflowed the (size_t)(cap - blen) length passed to snprintf. */
TEST(cypher_parse_unwind_many_elements_no_overflow) {
    /* 200 elements (~20 chars each) accumulate well past the 2KB assembly
     * buffer, which also underflowed the (size_t)(cap - blen) length. */
    char query[8192];
    int off = snprintf(query, sizeof(query), "UNWIND [");
    for (int i = 0; i < 200; i++) {
        off += snprintf(query + off, sizeof(query) - (size_t)off, "%s\"element_value_%d\"",
                        i ? "," : "", i);
    }
    snprintf(query + off, sizeof(query) - (size_t)off, "] AS x MATCH (f) RETURN f.name");

    cbm_query_t *q = NULL;
    char *err = NULL;
    int rc = cbm_cypher_parse(query, &q, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NOT_NULL(q->unwind_expr);
    cbm_query_free(q);
    PASS();
}

/* ── Issue #389 group: Cypher feature reproductions ─────────────────
 * Each asserts the CORRECT behavior; a failure reproduces the bug. */

/* #240: labels() function */
TEST(cypher_issue240_labels_function) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n:Module) RETURN labels(n) AS lbl", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #237: DISTINCT applied before ORDER BY + LIMIT */
TEST(cypher_issue237_distinct_order_limit) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s, "MATCH (f:Function) RETURN DISTINCT f.label AS l ORDER BY l LIMIT 10", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #252: toInteger() */
TEST(cypher_issue252_tointeger) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN toInteger(f.start_line) AS ln",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #305: count(*) + AS alias */
TEST(cypher_issue305_count_star_alias) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n) RETURN count(*) AS total", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Runs one query and asserts it answers exactly one row with `want` in the
 * first column. */
static int expect_one_row(cbm_store_t *s, const char *project, const char *query,
                          const char *want) {
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, project, 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], want);
    cbm_cypher_result_free(&r);
    return 0;
}

/* #2742: an ungrouped aggregation is one row even when MATCH/WHERE produced no
 * input rows. count(*) over nothing is 0, not an empty result: a caller asking
 * "how many?" must not have to read "no rows" as "zero". */
TEST(cypher_count_star_empty_input_returns_zero) {
    /* Empty graph: the project exists but holds no nodes. */
    cbm_store_t *empty = cbm_store_open_memory();
    cbm_store_upsert_project(empty, "empty", "/tmp/empty");
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(empty, "MATCH (n) RETURN count(*) AS total", "empty", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.col_count, 1);
    ASSERT_STR_EQ(r.columns[0], "total");
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "0");
    cbm_cypher_result_free(&r);
    cbm_store_close(empty);

    /* Nonempty graph, every row filtered out. */
    cbm_store_t *s = setup_cypher_store();
    ASSERT_EQ(expect_one_row(s, "test",
                             "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                             "RETURN count(*) AS total",
                             "0"),
              0);
    ASSERT_EQ(expect_one_row(s, "test",
                             "MATCH (a)-[:CALLS]->(b) WHERE b.name = \"NoSuchFunction\" "
                             "RETURN count(*)",
                             "0"),
              0);
    ASSERT_EQ(expect_one_row(s, "test", "MATCH (n:NoSuchLabel) RETURN count(*)", "0"), 0);
    ASSERT_EQ(expect_one_row(s, "test",
                             "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                             "RETURN count(f) AS n",
                             "0"),
              0);
    ASSERT_EQ(expect_one_row(s, "test",
                             "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                             "RETURN count(DISTINCT f.name) AS n",
                             "0"),
              0);

    /* The other aggregates in the same row take their empty-input values:
     * sum 0, collect [], and avg/min/max null (rendered as ""). */
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                            "RETURN count(*) AS c, sum(f.start_line) AS s, "
                            "avg(f.start_line) AS a, min(f.start_line) AS lo, "
                            "max(f.start_line) AS hi, collect(f.name) AS names",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "0");
    ASSERT_STR_EQ(r.rows[0][1], "0");
    ASSERT_STR_EQ(r.rows[0][2], "");
    ASSERT_STR_EQ(r.rows[0][3], "");
    ASSERT_STR_EQ(r.rows[0][4], "");
    ASSERT_STR_EQ(r.rows[0][5], "[]");
    cbm_cypher_result_free(&r);

    /* A grouped aggregation has no group to report: still no rows. */
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                            "RETURN f.label, count(*)",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 0);
    cbm_cypher_result_free(&r);

    /* SKIP still applies to the single aggregate row. */
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = \"NoSuchFunction\" "
                            "RETURN count(*) SKIP 1",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(r.error);
    ASSERT_EQ(r.row_count, 0);
    cbm_cypher_result_free(&r);

    /* Nonempty input is unchanged. */
    ASSERT_EQ(expect_one_row(s, "test", "MATCH (f:Function) RETURN count(*) AS total", "4"), 0);
    cbm_store_close(s);
    PASS();
}

/* Regression: projecting several computed/JSON properties in one row must yield
 * DISTINCT values. node_prop previously returned a single shared static buffer,
 * so every such column aliased the last property read — and because the search
 * key is matched in the JSON, `loop_depth` must not be confused with its suffix
 * `transitive_loop_depth`. Exercises the bottleneck metrics end-to-end. */
TEST(cypher_multi_prop_projection_no_alias) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "Hot",
                    .qualified_name = "test.Hot",
                    .file_path = "hot.go",
                    .start_line = 10,
                    .end_line = 42,
                    .properties_json = "{\"complexity\":3,\"cognitive\":7,\"loop_count\":2,"
                                       "\"loop_depth\":1,\"self_recursive\":false,"
                                       "\"transitive_loop_depth\":5,\"recursive\":true}"};
    cbm_store_upsert_node(s, &n);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) RETURN f.loop_depth, f.transitive_loop_depth, "
                                "f.cognitive, f.complexity, f.start_line, f.end_line",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_EQ(r.col_count, 6);
    ASSERT_STR_EQ(r.rows[0][0], "1"); /* loop_depth — NOT the suffix transitive_loop_depth */
    ASSERT_STR_EQ(r.rows[0][1], "5"); /* transitive_loop_depth */
    ASSERT_STR_EQ(r.rows[0][2], "7"); /* cognitive */
    ASSERT_STR_EQ(r.rows[0][3], "3"); /* complexity */
    ASSERT_STR_EQ(r.rows[0][4], "10"); /* start_line (computed) */
    ASSERT_STR_EQ(r.rows[0][5], "42"); /* end_line (computed) — distinct from start_line */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #601: an unbounded whole-graph OPTIONAL MATCH / GROUP BY does
 * O(bindings x groups) work and can run for minutes with no wall-clock guard —
 * the 100k row ceiling never fires because no rows are produced, so query_graph
 * just hangs. With the execution deadline armed to trip immediately (budget 0),
 * the runaway query must abort with a clear error instead of returning a
 * (misleading, possibly partial) result.
 *
 * RED on unfixed code: no deadline exists, so the query completes and returns
 * rc==0 with rows and no error — the assertions below fail. */
TEST(cypher_exec_deadline_aborts_runaway_query_issue601) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    cbm_cypher_test_set_deadline_ms(0); /* trip on the first hot-loop check */
    int rc = cbm_cypher_execute(
        s, "MATCH (a) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.qualified_name, count(b)", "test",
        0, &r);
    cbm_cypher_test_set_deadline_ms(-1); /* restore default before asserting (thread-local) */

    ASSERT_TRUE(rc != 0); /* CBM_NOT_FOUND (-1) — query aborted, not success */
    ASSERT_NOT_NULL(r.error);
    ASSERT_TRUE(strstr(r.error, "time limit") != NULL);
    ASSERT_EQ(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* #601 companion: the default (ample) budget must NOT false-positive on a
 * normal small query — it still returns its rows. */
TEST(cypher_exec_deadline_allows_normal_query_issue601) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(
        s, "MATCH (a) OPTIONAL MATCH (a)-[:CALLS]->(b) RETURN a.qualified_name, count(b)", "test",
        0, &r);

    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(r.error == NULL);
    ASSERT_GT(r.row_count, 0);

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════ */

/* #1196: max_rows limits PROJECTED RESULTS, not the source-node candidates
 * considered before WHERE. The unlabeled scan searched only 10 * max_rows nodes,
 * so a valid match later in search order simply disappeared — and because the
 * cap sat before WHERE and aggregation, count() reported the scanned prefix as
 * if it were a fact. Adding a LABEL to the source was the only workaround.
 *
 * max_rows = 1 with 11 distractors ahead of the match: the old 10-node scan
 * could not reach it. */
/* Planner: a single-hop pattern whose FAR node carries the selective filter is
 * walked from that end. `MATCH (a)-[:CALLS]->(b) WHERE b.name = 'X'` used to
 * scan every node before the filter on b could act — on upstream's 8.5 M node
 * kernel graph it hit the execution-time limit. The rows must be the same in
 * every spelling, including the inline-property and inbound-direction forms,
 * and the caller-side variable must still bind the right nodes. */
TEST(cypher_single_hop_seeds_from_selective_far_node) {
    cbm_store_t *s = setup_cypher_store();

    cbm_cypher_result_t where_form = {};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a)-[:CALLS]->(b) WHERE b.name = 'ValidateOrder' "
                                 "RETURN a.name ORDER BY a.name",
                                 "test", 0, &where_form),
              0);
    ASSERT_EQ(where_form.row_count, 1);
    ASSERT_STR_EQ(where_form.rows[0][0], "HandleOrder");
    cbm_cypher_result_free(&where_form);

    cbm_cypher_result_t inline_form = {};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a)-[:CALLS]->(b {name: 'SubmitOrder'}) "
                                 "RETURN a.name",
                                 "test", 0, &inline_form),
              0);
    ASSERT_EQ(inline_form.row_count, 1);
    ASSERT_STR_EQ(inline_form.rows[0][0], "ValidateOrder");
    cbm_cypher_result_free(&inline_form);

    /* Inbound spelling: the far node is now the CALLER; direction inverts back. */
    cbm_cypher_result_t inbound_form = {};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (callee)<-[:CALLS]-(caller) WHERE caller.name = "
                                 "'HandleOrder' RETURN callee.name ORDER BY callee.name",
                                 "test", 0, &inbound_form),
              0);
    ASSERT_EQ(inbound_form.row_count, 2);
    ASSERT_STR_EQ(inbound_form.rows[0][0], "LogError");
    ASSERT_STR_EQ(inbound_form.rows[1][0], "ValidateOrder");
    cbm_cypher_result_free(&inbound_form);

    /* Count aggregation through the swapped seed. */
    cbm_cypher_result_t count_form = {};
    ASSERT_EQ(cbm_cypher_execute(s,
                                 "MATCH (a)-[:CALLS]->(b) WHERE b.name = 'LogError' "
                                 "RETURN count(a) AS callers",
                                 "test", 0, &count_form),
              0);
    ASSERT_EQ(count_form.row_count, 1);
    ASSERT_STR_EQ(count_form.rows[0][0], "1");
    cbm_cypher_result_free(&count_form);

    /* RETURN * keeps pattern order, so it is never swapped: column 0 is `a`. */
    cbm_cypher_result_t star_form = {};
    ASSERT_EQ(cbm_cypher_execute(s, "MATCH (a)-[:CALLS]->(b) WHERE b.name = 'LogError' RETURN *",
                                 "test", 0, &star_form),
              0);
    ASSERT_EQ(star_form.row_count, 1);
    ASSERT_TRUE(star_form.col_count >= 2);
    ASSERT_NOT_NULL(strstr(star_form.columns[0], "a"));
    cbm_cypher_result_free(&star_form);

    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_unlabeled_where_beyond_result_limit_issue1196) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "test", "/tmp/test"), CBM_STORE_OK);

    for (int i = 0; i < 11; i++) {
        char name[32];
        char qn[64];
        snprintf(name, sizeof(name), "early_%02d", i);
        snprintf(qn, sizeof(qn), "test.%s", name);
        cbm_node_t distractor = {.project = "test",
                                 .label = "Function",
                                 .name = name,
                                 .qualified_name = qn,
                                 .file_path = "early.py"};
        ASSERT_GT(cbm_store_upsert_node(s, &distractor), 0);
    }

    cbm_node_t late = {.project = "test",
                       .label = "Function",
                       .name = "zz_late_match",
                       .qualified_name = "test.zz_late_match",
                       .file_path = "late.py"};
    ASSERT_GT(cbm_store_upsert_node(s, &late), 0);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH (n) WHERE n.name = \"zz_late_match\" RETURN n.name",
                                "test", 1, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "zz_late_match");
    cbm_cypher_result_free(&r);

    /* The aggregate half: count() over an unlabeled source must see EVERY node,
     * not the scanned prefix. 12 nodes exist; max_rows = 1 must not make it 10. */
    cbm_cypher_result_t c = {0};
    rc = cbm_cypher_execute(s, "MATCH (n) RETURN count(n)", "test", 1, &c);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(c.row_count, 1);
    ASSERT_STR_EQ(c.rows[0][0], "12");
    cbm_cypher_result_free(&c);

    cbm_store_close(s);
    PASS();
}

/* #2208: a node carried IN SCOPE through WITH must project every property
 * exactly as the same node does without the WITH. The carried var used to be a
 * name-only stub whose qualified_name slot held the variable name and whose
 * start_line was 0, so f.qualified_name returned "f" and f.start_line "0". */
static int64_t issue2208_id_of(cbm_store_t *s, const char *qn) {
    cbm_node_t n = {0};
    int64_t id = 0;
    if (cbm_store_find_node_by_qn(s, "test", qn, &n) == CBM_STORE_OK) {
        id = n.id;
        cbm_node_free_fields(&n);
    }
    return id;
}

static cbm_store_t *setup_issue2208_store(void) {
    cbm_store_t *s = setup_cypher_store();
    cbm_node_t twins[] = {
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionA",
         .file_path = "projection_a.go",
         .start_line = 30,
         .end_line = 40,
         .properties_json = "{\"marker\":\"alpha\"}"},
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionB",
         .file_path = "projection_b.go",
         .start_line = 10,
         .end_line = 20,
         .properties_json = "{\"marker\":\"beta\"}"},
        {.project = "test",
         .label = "Function",
         .name = "ProjectionTwin",
         .qualified_name = "test.ProjectionC",
         .file_path = "projection_c.go",
         .start_line = 20,
         .end_line = 30,
         .properties_json = "{\"marker\":\"gamma\"}"},
    };
    int64_t ids[3];
    for (int i = 0; i < 3; i++) {
        ids[i] = cbm_store_upsert_node(s, &twins[i]);
    }
    /* Callers: A has 2 (HandleOrder, ValidateOrder), B has 1 (HandleOrder), C none. */
    int64_t handle = issue2208_id_of(s, "test.HandleOrder");
    int64_t validate = issue2208_id_of(s, "test.ValidateOrder");
    cbm_edge_t e1 = {.project = "test", .source_id = handle, .target_id = ids[0], .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "test", .source_id = validate, .target_id = ids[0], .type = "CALLS"};
    cbm_edge_t e3 = {.project = "test", .source_id = handle, .target_id = ids[1], .type = "CALLS"};
    cbm_store_insert_edge(s, &e1);
    cbm_store_insert_edge(s, &e2);
    cbm_store_insert_edge(s, &e3);
    return s;
}

/* The reporter's exact shape: aggregate WITH carrying the node beside count(). */
TEST(cypher_issue2208_with_agg_carried_node_props) {
    cbm_store_t *s = setup_issue2208_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {qualified_name: 'test.ProjectionA'}) "
                                "OPTIONAL MATCH (caller)-[r:CALLS]->(f) "
                                "WITH f, count(r) AS refs "
                                "RETURN f.qualified_name, f.file_path, f.start_line, "
                                "f.end_line, f.label, f.marker, f.name, refs",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][2], "30");
    ASSERT_STR_EQ(r.rows[0][3], "40");
    ASSERT_STR_EQ(r.rows[0][4], "Function");
    ASSERT_STR_EQ(r.rows[0][5], "alpha");
    ASSERT_STR_EQ(r.rows[0][6], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][7], "2");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Aggregate WITH groups by node IDENTITY, not display name: three same-named
 * functions stay three rows, each with its own properties and caller count
 * (they used to collapse into one row with one node's props and summed refs). */
TEST(cypher_issue2208_with_agg_groups_by_node_identity) {
    cbm_store_t *s = setup_issue2208_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function {name: 'ProjectionTwin'}) "
                                "OPTIONAL MATCH (caller)-[r:CALLS]->(f) "
                                "WITH f, count(r) AS refs "
                                "RETURN f.qualified_name, f.start_line, refs "
                                "ORDER BY f.qualified_name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 3);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "30");
    ASSERT_STR_EQ(r.rows[0][2], "2");
    ASSERT_STR_EQ(r.rows[1][0], "test.ProjectionB");
    ASSERT_STR_EQ(r.rows[1][1], "10");
    ASSERT_STR_EQ(r.rows[1][2], "1");
    ASSERT_STR_EQ(r.rows[2][0], "test.ProjectionC");
    ASSERT_STR_EQ(r.rows[2][1], "20");
    /* rows[2][2] (count(r) for the caller-less C) is not asserted here: count()
     * of an unbound OPTIONAL var is a separate, pre-existing defect. */
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* Simple (non-aggregate) WITH: a bare node stays a node (renamed or not);
 * scalar items stay scalars; OPTIONAL null stays null; post-WITH WHERE and
 * WITH ... ORDER BY see the real node properties. */
TEST(cypher_issue2208_with_simple_carried_node_props) {
    cbm_store_t *s = setup_issue2208_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionA' "
                                "WITH f "
                                "RETURN f.name, f.qualified_name, f.file_path, f.label, "
                                "f.start_line, f.marker",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][1], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][2], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][3], "Function");
    ASSERT_STR_EQ(r.rows[0][4], "30");
    ASSERT_STR_EQ(r.rows[0][5], "alpha");
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.qualified_name = 'test.ProjectionA' "
                            "WITH f AS g, f.name AS n, labels(f) AS ls "
                            "RETURN g.qualified_name, g.file_path, n, ls, n.file_path, "
                            "ls.file_path",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionA");
    ASSERT_STR_EQ(r.rows[0][1], "projection_a.go");
    ASSERT_STR_EQ(r.rows[0][2], "ProjectionTwin");
    ASSERT_STR_EQ(r.rows[0][3], "[\"Function\"]");
    ASSERT_STR_EQ(r.rows[0][4], "");
    ASSERT_STR_EQ(r.rows[0][5], "");
    cbm_cypher_result_free(&r);

    /* OPTIONAL null carried through WITH stays null (no fabricated node). */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = 'LogError' "
                            "OPTIONAL MATCH (f)-[:CALLS]->(g:Function) "
                            "WITH g AS projected "
                            "RETURN projected, projected.file_path, projected.qualified_name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "");
    ASSERT_STR_EQ(r.rows[0][1], "");
    ASSERT_STR_EQ(r.rows[0][2], "");
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WITH f AS g "
                            "WHERE g.file_path = 'projection_b.go' "
                            "RETURN g.qualified_name",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionB");
    cbm_cypher_result_free(&r);

    /* A(30), B(10), C(20): ascending then SKIP 1 LIMIT 1 selects C. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s,
                            "MATCH (f:Function) WHERE f.name = 'ProjectionTwin' "
                            "WITH f AS g ORDER BY g.start_line ASC SKIP 1 LIMIT 1 "
                            "RETURN g.qualified_name, g.start_line",
                            "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "test.ProjectionC");
    ASSERT_STR_EQ(r.rows[0][1], "20");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}


/* #1364: RETURN DISTINCT de-duplicated only AFTER the engine row cap, so a
 * value that first appears past the cap was never seen: on a 150k-edge index
 * `RETURN DISTINCT type(r)` returned 21 of 22 types (HAS_BRANCH, one edge,
 * missing). The cap must count DISTINCT rows. Fixture: 20 CALLS edges, then
 * one HAS_BRANCH edge from the last-inserted node; max_rows=5 puts the rare
 * type far past the raw-row cap. */
TEST(cypher_return_distinct_counts_distinct_rows_against_cap_issue1364) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "test", "/tmp/test"), CBM_STORE_OK);

    int64_t ids[22];
    for (int i = 0; i < 22; i++) {
        char name[32];
        char qn[64];
        snprintf(name, sizeof(name), "fn_%02d", i);
        snprintf(qn, sizeof(qn), "test.%s", name);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "a.py"};
        ids[i] = cbm_store_upsert_node(s, &n);
        ASSERT_GT(ids[i], 0);
    }
    for (int i = 0; i < 20; i++) {
        cbm_edge_t e = {
            .project = "test", .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
        cbm_store_insert_edge(s, &e);
    }
    cbm_edge_t rare = {
        .project = "test", .source_id = ids[21], .target_id = ids[0], .type = "HAS_BRANCH"};
    cbm_store_insert_edge(s, &rare);

    /* Plain DISTINCT: both types, complete (2 distinct rows << cap of 5). */
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t", "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    bool saw_calls = false;
    bool saw_branch = false;
    for (int i = 0; i < r.row_count; i++) {
        saw_calls = saw_calls || strcmp(r.rows[i][0], "CALLS") == 0;
        saw_branch = saw_branch || strcmp(r.rows[i][0], "HAS_BRANCH") == 0;
    }
    ASSERT_TRUE(saw_calls);
    ASSERT_TRUE(saw_branch);
    cbm_cypher_result_free(&r);

    /* DISTINCT + ORDER BY shares the cap. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t ORDER BY t DESC",
                            "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    ASSERT_STR_EQ(r.rows[0][0], "HAS_BRANCH");
    ASSERT_STR_EQ(r.rows[1][0], "CALLS");
    cbm_cypher_result_free(&r);

    /* DISTINCT + ORDER BY + SKIP/LIMIT: SKIP counts distinct rows. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(
        s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t ORDER BY t SKIP 1 LIMIT 1", "test", 5,
        &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "HAS_BRANCH");
    cbm_cypher_result_free(&r);

    /* An explicit LIMIT on DISTINCT counts distinct rows too. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH ()-[r]->() RETURN DISTINCT type(r) AS t LIMIT 2", "test", 5,
                            &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    /* More distinct rows than the cap: still bounded and reported truncated. */
    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (a)-[r]->(b) RETURN DISTINCT b.name", "test", 5, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 5);
    /* (the fork has no result.truncated flag; the row cap is what is checked) */
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}


/* Regression (distilled from PR #1245, Andrew Hundt): the early WHERE pass in
 * execute_single runs on seed rows with only the first alias bound. A
 * comparison on an unbound alias used to evaluate to true, which NOT / XOR
 * then inverted -- so every seed was pruned before CALLS expansion and the
 * query returned zero rows. Unbound leaves must stay UNKNOWN there. */
TEST(cypher_exec_where_not_on_relationship_target) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function)-[:CALLS]->(b:Function) "
                                "WHERE NOT b.name CONTAINS \"Order\" RETURN b.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_mixed_alias_and) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (a:Function)-[:CALLS]->(b:Function) "
        "WHERE a.name = \"HandleOrder\" AND NOT b.name CONTAINS \"Order\" RETURN b.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_mixed_alias_or) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(
        s,
        "MATCH (a:Function)-[:CALLS]->(b:Function) "
        "WHERE a.name = \"NoSuchFunction\" OR NOT b.name CONTAINS \"Order\" RETURN b.name",
        "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "LogError");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

TEST(cypher_exec_where_mixed_alias_xor) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc =
        cbm_cypher_execute(s,
                           "MATCH (a:Function)-[:CALLS]->(b:Function) "
                           "WHERE a.name = \"HandleOrder\" XOR b.name = \"LogError\" RETURN b.name",
                           "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 1);
    ASSERT_STR_EQ(r.rows[0][0], "ValidateOrder");
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}


/* Regression: the relationship variant of the test above. An OPTIONAL MATCH
 * with a relationship whose start AND terminal are both unbound, and whose
 * start matches zero nodes, drove cross_join_with_rels with extra_count == 0.
 * That sized its output for a single binding, and the OPTIONAL fallback then
 * wrote one row per existing binding without growing the buffer: a heap
 * buffer overflow once the first MATCH bound more than one node (ASan:
 * heap-buffer-overflow). The query text reaches this from the MCP query tool. */
TEST(cypher_exec_optional_empty_label_rel_no_overflow) {
    cbm_store_t *s = setup_cypher_store(); /* 4 Function nodes */
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s,
                                "MATCH (a:Function) OPTIONAL MATCH (x:NoSuchLabel)-[:CALLS]->(y) "
                                "RETURN a.name, x.name, y.name",
                                "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.col_count, 3);
    /* One row per Function, each with x and y left unbound (""). */
    ASSERT_EQ(r.row_count, 4);
    const char *want[] = {"HandleOrder", "ValidateOrder", "SubmitOrder", "LogError"};
    for (int w = 0; w < 4; w++) {
        int seen = 0;
        for (int i = 0; i < r.row_count; i++) {
            if (strcmp(r.rows[i][0], want[w]) == 0) {
                seen++;
            }
        }
        ASSERT_EQ(seen, 1);
    }
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][1], "");
        ASSERT_STR_EQ(r.rows[i][2], "");
    }

    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}


/* A pattern that fails partway frees what it already parsed. Each query
 * fails at a different point; under LeakSanitizer the first four leaked
 * before the fix (the first is the fuzz seed that found it), and the fifth
 * reaches the same property-list failure through a second MATCH. */
TEST(cypher_parse_error_frees_partial_pattern) {
    static const char *const queries[] = {
        /* named path: unsupported, fails at the first node */
        "MATCH p = (a:Class)-[:INHERITS*1..3]->(b:Class) RETURN a.name, length(p)",
        /* property list fails after one parsed entry */
        "MATCH (a:Function {name: \"x\", 7}) RETURN a",
        /* relationship cut off after its variable */
        "MATCH (a:Function)-[r:CALLS",
        /* second node's label missing, after a parsed relationship */
        "MATCH (a:Function)-[:CALLS]->(b:",
        /* a second MATCH clause's pattern fails inside its properties */
        "MATCH (a:Function) MATCH (b:Function {name: \"y\" 5}) RETURN a",
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_query_t *q = NULL;
        char *err = NULL;
        int rc = cbm_cypher_parse(queries[i], &q, &err);
        ASSERT_EQ(rc, -1);
        ASSERT_NOT_NULL(err);
        free(err);
        cbm_query_free(q);
    }
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
static uint64_t cypher_frozen_deadline_clock(void) {
    return 1000;
}

static thread_local unsigned cypher_deadline_clock_reads = 0;
#endif

/* ══════════════════════════════════════════════════════════════════
 *  LONG OPERATOR CHAINS AND MANY UNION BRANCHES
 *
 * A WHERE chain `a OR b OR c ...` and a UNION chain are bounded only by the
 * message size, and an agent that lists a few thousand names in one query is
 * ordinary input. The parser used to fold a chain into a tree as deep as the
 * chain is long and to parse every UNION branch through a nested call, so each
 * walker (evaluation, seed selectivity, parsing) recursed once per operand or
 * branch. The daemon runs a query on a worker thread with a 256 KiB stack
 * (daemon/runtime.c RUNTIME_WORKER_STACK_SIZE), where a few thousand frames
 * already overflow. These tests run their query on a thread of that size,
 * inside a forked child on POSIX, so an overflow is reported as the signal
 * that killed the child instead of ending the test runner.
 * ══════════════════════════════════════════════════════════════════ */

enum { CYPHER_WORKER_STACK_BYTES = 256 * 1024 }; /* = RUNTIME_WORKER_STACK_SIZE */

typedef struct {
    int (*check)(void *);
    void *arg;
    int result;
} worker_stack_run_t;

static void *worker_stack_thread(void *opaque) {
    worker_stack_run_t *run = (worker_stack_run_t *)opaque;
    run->result = run->check(run->arg);
    return NULL;
}

/* Runs check(arg) on a thread with the worker stack size; returns its result. */
static int run_on_worker_stack_thread(worker_stack_run_t *run) {
    cbm_thread_t t;
    if (cbm_thread_create(&t, CYPHER_WORKER_STACK_BYTES, worker_stack_thread, run) != 0) {
        FAIL("could not create the worker-stack thread");
    }
    (void)cbm_thread_join(&t);
    return run->result;
}

/* Runs check(arg) the way the daemon runs a query. POSIX: on the worker-sized
 * thread of a forked child, whose exit status is the check's result; a killing
 * signal is reported as such. Windows has no fork, and CreateThread's size
 * argument sets the committed stack while the reserve stays the image default,
 * so the thread runs in-process there. */
static int run_on_worker_stack(int (*check)(void *), void *arg) {
    worker_stack_run_t run = {.check = check, .arg = arg, .result = 1};
#ifdef _WIN32
    return run_on_worker_stack_thread(&run);
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int code = run_on_worker_stack_thread(&run);
        fflush(NULL);
        _exit(code == 0 ? 0 : 1);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        char m[96];
        snprintf(m, sizeof(m), "query thread killed by signal %d on a %d KiB stack",
                 WTERMSIG(status), CYPHER_WORKER_STACK_BYTES / 1024);
        FAIL(m);
    }
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
#endif
}

/* The fixture graph of setup_cypher_store, restated here so the expected rows
 * are computed by the test and not read back from the engine. */
static const char *const cypher_fixture_functions[] = {"HandleOrder", "ValidateOrder",
                                                       "SubmitOrder", "LogError"};
enum { CYPHER_FIXTURE_FUNCTION_COUNT = 4 };
static const char *const cypher_fixture_calls[][2] = {{"HandleOrder", "ValidateOrder"},
                                                      {"ValidateOrder", "SubmitOrder"},
                                                      {"HandleOrder", "LogError"}};
enum { CYPHER_FIXTURE_CALL_COUNT = 3 };
static const char *const cypher_fixture_nodes[] = {"HandleOrder", "ValidateOrder", "SubmitOrder",
                                                   "main", "LogError"};
enum { CYPHER_FIXTURE_NODE_COUNT = 5 };

enum { CYPHER_CHAIN_OPERANDS = 50000 };

/* Operand i of a generated chain compares f.name with this value. Two fixture
 * names sit deep in the chain (the middle and the very last operand); every
 * other operand names a function that does not exist. */
static const char *chain_operand_value(int i, char *buf, size_t n) {
    if (i == CYPHER_CHAIN_OPERANDS / 2) {
        return "ValidateOrder";
    }
    if (i == CYPHER_CHAIN_OPERANDS - 1) {
        return "LogError";
    }
    snprintf(buf, n, "no_such_function_%d", i);
    return buf;
}

/* The test's own reading of the chain: does any operand name `name`? */
static bool chain_names(const char *name) {
    for (int i = 0; i < CYPHER_CHAIN_OPERANDS; i++) {
        char buf[48];
        if (strcmp(chain_operand_value(i, buf, sizeof(buf)), name) == 0) {
            return true;
        }
    }
    return false;
}

/* head + "f.name <op> \"v0\"" + join + "f.name <op> \"v1\"" + ... + tail */
static char *chain_query(const char *head, const char *op, const char *join, const char *tail) {
    size_t cap = 256 + (size_t)CYPHER_CHAIN_OPERANDS * 64;
    char *q = (char *)malloc(cap);
    if (!q) {
        return NULL;
    }
    size_t len = (size_t)snprintf(q, cap, "%s", head);
    for (int i = 0; i < CYPHER_CHAIN_OPERANDS && len < cap; i++) {
        char buf[48];
        const char *v = chain_operand_value(i, buf, sizeof(buf));
        len += (size_t)snprintf(q + len, cap - len, "%sf.name %s \"%s\"", i ? join : "", op, v);
    }
    if (len >= cap) {
        free(q);
        return NULL;
    }
    snprintf(q + len, cap - len, "%s", tail);
    return q;
}
typedef struct {
    const char *head;
    const char *op;
    const char *join;
    const char *tail;
    bool admits_named; /* true: a row passes when the chain names f; false: when it does not */
    int expected_rows;
} chain_case_t;

/* Runs the chain query and checks every row against the test's own reading of
 * the chain, then checks that the engine still answers afterwards. */
static int chain_check(void *arg) {
    const chain_case_t *c = (const chain_case_t *)arg;
    char *query = chain_query(c->head, c->op, c->join, c->tail);
    ASSERT_NOT_NULL(query);

    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    free(query);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, c->expected_rows);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_EQ(chain_names(r.rows[i][0]), c->admits_named);
    }
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_FIXTURE_FUNCTION_COUNT);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return 0;
}

/* 50,000 operands joined by OR over a single node pattern: the rows are the
 * Functions the chain names. The expected count comes from the fixture list
 * and chain_names, not from the engine. */
TEST(cypher_long_or_chain_evaluates) {
    chain_case_t c = {.head = "MATCH (f:Function) WHERE ",
                      .op = "=",
                      .join = " OR ",
                      .tail = " RETURN f.name",
                      .admits_named = true,
                      .expected_rows = 0};
    for (int i = 0; i < CYPHER_FIXTURE_FUNCTION_COUNT; i++) {
        c.expected_rows += chain_names(cypher_fixture_functions[i]) ? 1 : 0;
    }
    ASSERT_EQ(c.expected_rows, 2); /* the generator plants ValidateOrder and LogError */
    int rc = run_on_worker_stack(chain_check, &c);
    if (rc != 0) {
        return rc;
    }
    PASS();
}

/* 50,000 operands joined by AND over a single-hop pattern, so the planner's
 * seed-selectivity walk sees the chain as well as the evaluator: the rows are
 * the CALLS edges whose caller the chain does not name. */
TEST(cypher_long_and_chain_evaluates) {
    chain_case_t c = {.head = "MATCH (f:Function)-[:CALLS]->(g:Function) WHERE ",
                      .op = "<>",
                      .join = " AND ",
                      .tail = " RETURN f.name, g.name",
                      .admits_named = false,
                      .expected_rows = 0};
    for (int i = 0; i < CYPHER_FIXTURE_CALL_COUNT; i++) {
        c.expected_rows += chain_names(cypher_fixture_calls[i][0]) ? 0 : 1;
    }
    ASSERT_EQ(c.expected_rows, 2); /* HandleOrder's two calls; ValidateOrder's is named */
    int rc = run_on_worker_stack(chain_check, &c);
    if (rc != 0) {
        return rc;
    }
    PASS();
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Runs `query` and checks that its first column holds exactly the names in
 * `expected`, given in strcmp order; the rows are sorted the same way so the
 * engine's row order does not matter. */
static int assert_name_set(cbm_store_t *s, const char *query, const char *const *expected, int n) {
    enum { NAME_SET_MAX = 8 };
    ASSERT_TRUE(n <= NAME_SET_MAX);
    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, query, "test", 0, &r);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, n);
    const char *names[NAME_SET_MAX];
    for (int i = 0; i < n; i++) {
        names[i] = r.rows[i][0];
    }
    qsort(names, (size_t)n, sizeof(names[0]), cmp_cstr);
    for (int i = 0; i < n; i++) {
        ASSERT_STR_EQ(names[i], expected[i]);
    }
    cbm_cypher_result_free(&r);
    return 0;
}

/* Every spelling in `forms` is one WHERE expression; all must yield `expected`. */
static int where_forms_agree(cbm_store_t *s, const char *const *forms, int form_count,
                             const char *const *expected, int n) {
    for (int f = 0; f < form_count; f++) {
        char query[512];
        snprintf(query, sizeof(query), "MATCH (n) WHERE %s RETURN n.name", forms[f]);
        int rc = assert_name_set(s, query, expected, n);
        if (rc != 0) {
            printf("  form: %s\n", forms[f]);
            return rc;
        }
    }
    return 0;
}

/* A chain and the same expression written out with parentheses, in both
 * associations, must agree row for row. The operands include a property the
 * `main` node does not have: file_path reads as the empty string there, so
 * `IS NULL` holds and `<>` holds, and a missing value takes part in AND, OR and
 * XOR like any other operand instead of voiding the chain. */
TEST(cypher_mixed_chain_matches_nested_form) {
    cbm_store_t *s = setup_cypher_store();

    static const char *const or_forms[] = {
        "n.file_path = \"handler.go\" OR n.file_path IS NULL OR n.name = \"LogError\"",
        "(n.file_path = \"handler.go\" OR n.file_path IS NULL) OR n.name = \"LogError\"",
        "n.file_path = \"handler.go\" OR (n.file_path IS NULL OR n.name = \"LogError\")"};
    static const char *const or_rows[] = {"HandleOrder", "LogError", "main"};
    ASSERT_EQ(where_forms_agree(s, or_forms, 3, or_rows, 3), 0);

    static const char *const and_forms[] = {
        "n.file_path <> \"handler.go\" AND n.file_path <> \"log.go\" AND n.name <> \"SubmitOrder\"",
        "(n.file_path <> \"handler.go\" AND n.file_path <> \"log.go\") AND n.name <> "
        "\"SubmitOrder\"",
        "n.file_path <> \"handler.go\" AND (n.file_path <> \"log.go\" AND n.name <> "
        "\"SubmitOrder\")"};
    static const char *const and_rows[] = {"ValidateOrder", "main"};
    ASSERT_EQ(where_forms_agree(s, and_forms, 3, and_rows, 2), 0);

    /* Parity: main is T^T^F^F, LogError F^F^T^F, HandleOrder F^F^F^T. */
    static const char *const xor_forms[] = {
        "n.name = \"main\" XOR n.file_path IS NULL XOR n.name = \"LogError\" XOR "
        "n.file_path = \"handler.go\"",
        "((n.name = \"main\" XOR n.file_path IS NULL) XOR n.name = \"LogError\") XOR "
        "n.file_path = \"handler.go\"",
        "n.name = \"main\" XOR (n.file_path IS NULL XOR (n.name = \"LogError\" XOR "
        "n.file_path = \"handler.go\"))"};
    static const char *const xor_rows[] = {"HandleOrder", "LogError"};
    ASSERT_EQ(where_forms_agree(s, xor_forms, 3, xor_rows, 2), 0);

    /* Mixed precedence (NOT > AND > XOR > OR) with a NOT over a missing value:
     * main by name, LogError by (F AND T) XOR T, SubmitOrder by NOT (F). */
    static const char *const mixed_forms[] = {
        "n.name = \"main\" OR n.file_path IS NULL AND n.name = \"LogError\" XOR "
        "n.file_path = \"log.go\" OR NOT n.file_path <> \"submit.go\"",
        "n.name = \"main\" OR ((n.file_path IS NULL AND n.name = \"LogError\") XOR "
        "n.file_path = \"log.go\") OR (NOT (n.file_path <> \"submit.go\"))",
        "(n.name = \"main\" OR ((n.file_path IS NULL AND n.name = \"LogError\") XOR "
        "n.file_path = \"log.go\")) OR (NOT (n.file_path <> \"submit.go\"))"};
    static const char *const mixed_rows[] = {"LogError", "SubmitOrder", "main"};
    ASSERT_EQ(where_forms_agree(s, mixed_forms, 3, mixed_rows, 3), 0);

    cbm_store_close(s);
    PASS();
}

/* Branch i selects fixture node i % 5 by name; `join` is " UNION " or
 * " UNION ALL ". */
static char *union_query(int branches, const char *join) {
    size_t cap = 64 + (size_t)branches * 80;
    char *q = (char *)malloc(cap);
    if (!q) {
        return NULL;
    }
    size_t len = 0;
    for (int i = 0; i < branches && len < cap; i++) {
        len +=
            (size_t)snprintf(q + len, cap - len, "%sMATCH (n) WHERE n.name = \"%s\" RETURN n.name",
                             i ? join : "", cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    if (len >= cap) {
        free(q);
        return NULL;
    }
    return q;
}

#ifdef CBM_ENABLE_TEST_SEAMS
enum { CYPHER_UNION_MANY_BRANCHES = 20000 };

/* UNION ALL keeps every branch's row in branch order. */
static int union_many_check(void *arg) {
    (void)arg;
    char *query = union_query(CYPHER_UNION_MANY_BRANCHES, " UNION ALL ");
    ASSERT_NOT_NULL(query);

    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    /* This exercises stack depth and every result, independently of how
     * slowly instrumentation runs. Dedicated deadline tests retain expiry. */
    cbm_cypher_test_set_deadline_clock(cypher_frozen_deadline_clock);
    /* Request exactly the output this fixture needs: the default 100k ceiling
     * also reserves 100k binding slots separately for every UNION branch. */
    int rc = cbm_cypher_execute(s, query, "test", CYPHER_UNION_MANY_BRANCHES, &r);
    cbm_cypher_test_set_deadline_clock(NULL);
    free(query);
    if (rc != 0) {
        printf("  query error: %s\n", r.error ? r.error : "(none)");
    }
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_UNION_MANY_BRANCHES);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    return 0;
}

/* 20,000 UNION ALL branches parse, run and free on the worker stack, and
 * every branch contributes its row. */
TEST(cypher_many_union_branches) {
    int rc = run_on_worker_stack(union_many_check, NULL);
    if (rc != 0) {
        return rc;
    }
    PASS();
}
#endif

/* 50 branches: UNION ALL yields one row per branch in branch order, UNION
 * keeps the first row of each name, and a UNION with nothing after it is still
 * a clean parse error. */
TEST(cypher_fifty_union_branches) {
    enum { BRANCHES = 50 };
    cbm_store_t *s = setup_cypher_store();
    char *all = union_query(BRANCHES, " UNION ALL ");
    char *distinct = union_query(BRANCHES, " UNION ");
    ASSERT_NOT_NULL(all);
    ASSERT_NOT_NULL(distinct);

    cbm_cypher_result_t r = {0};
    int rc = cbm_cypher_execute(s, all, "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, BRANCHES);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i % CYPHER_FIXTURE_NODE_COUNT]);
    }
    cbm_cypher_result_free(&r);

    rc = cbm_cypher_execute(s, distinct, "test", 0, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, CYPHER_FIXTURE_NODE_COUNT);
    for (int i = 0; i < r.row_count; i++) {
        ASSERT_STR_EQ(r.rows[i][0], cypher_fixture_nodes[i]);
    }
    cbm_cypher_result_free(&r);
    free(all);
    free(distinct);

    rc = cbm_cypher_execute(s, "MATCH (n) RETURN n.name UNION", "test", 0, &r);
    ASSERT_EQ(rc, -1);
    ASSERT_NOT_NULL(r.error);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* The early WHERE pass (execute_single) binds only the first MATCH alias, so
 * an operand on the second alias is UNKNOWN there and must neither decide nor
 * be dropped from a chain: three and more operands put it on a chain link and
 * not only on the chain's head. The rows are the full evaluation's; an early
 * pass that read UNKNOWN as false, or let a later operand overwrite it, would
 * prune the HandleOrder seed and lose them. */
TEST(cypher_chain_with_unbound_alias_keeps_seed) {
    cbm_store_t *s = setup_cypher_store();

    /* OR, UNKNOWN first: only HandleOrder -> LogError has a target without "Order". */
    static const char *const log_error[] = {"LogError"};
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE NOT b.name CONTAINS "
                              "\"Order\" OR a.name = \"NoSuchA\" OR a.name = \"NoSuchB\" "
                              "RETURN b.name",
                              log_error, 1),
              0);

    /* XOR, UNKNOWN in the middle: F ^ T ^ F holds for HandleOrder -> LogError only. */
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE a.name = "
                              "\"NoSuchA\" XOR b.name = \"LogError\" XOR a.name = \"NoSuchB\" "
                              "RETURN b.name",
                              log_error, 1),
              0);

    /* AND, UNKNOWN first and a definite operand last: HandleOrder's two calls. */
    static const char *const handle_order_calls[] = {"LogError", "ValidateOrder"};
    ASSERT_EQ(assert_name_set(s,
                              "MATCH (a:Function)-[:CALLS]->(b:Function) WHERE b.name <> "
                              "\"SubmitOrder\" AND a.name <> \"NoSuchA\" AND a.name = "
                              "\"HandleOrder\" RETURN b.name",
                              handle_order_calls, 2),
              0);

    cbm_store_close(s);
    PASS();
}



#ifdef CBM_ENABLE_TEST_SEAMS
static uint64_t cypher_expiring_deadline_clock(void) {
    /* Default budget is 30 seconds; expire exactly at the first checkpoint. */
    return cypher_deadline_clock_reads++ == 0 ? 1000 : 31000;
}

TEST(cypher_exec_deadline_expires_at_positive_budget_boundary) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};
    cypher_deadline_clock_reads = 0;
    cbm_cypher_test_set_deadline_clock(cypher_expiring_deadline_clock);
    int rc = cbm_cypher_execute(s, "MATCH (n) RETURN n.name", "test", 0, &r);
    cbm_cypher_test_set_deadline_clock(NULL);

    ASSERT_TRUE(cypher_deadline_clock_reads >= 2);
    ASSERT_TRUE(rc != 0);
    ASSERT_NOT_NULL(r.error);
    ASSERT_NOT_NULL(strstr(r.error, "time limit"));
    ASSERT_EQ(r.row_count, 0);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}
#endif

/* ── ORDER BY, DISTINCT and grouping against a reference ───────────
 * These were bubble sorts and linear duplicate/group scans — seconds to
 * minutes on a 30k-row result. The results must stay exactly the old ones: a
 * stable sort of the unsorted rows, first-occurrence DISTINCT, first-seen
 * group order. Each case derives that reference from the engine's own
 * unsorted rows, on a graph built for ties, duplicates and large groups. */

enum { REF_NODES = 400, REF_NAMES = 97 };

static cbm_store_t *setup_reference_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    int64_t ids[REF_NODES];
    for (int i = 0; i < REF_NODES; i++) {
        char name[32];
        char qn[32];
        char file[32];
        snprintf(name, sizeof(name), "fn_%02d", (i * 7) % REF_NAMES); /* repeated names */
        snprintf(qn, sizeof(qn), "test.n%d", i);
        snprintf(file, sizeof(file), "f%02d.c", i % 13);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = file,
                        .start_line = (i * 31) % 50, /* tied lines */
                        .end_line = 60};
        ids[i] = cbm_store_upsert_node(s, &n);
    }
    /* Every node calls both hubs (n0, n1: ~400 callers, 97 distinct names,
     * past the hashed COUNT(DISTINCT) threshold) and one more node. */
    for (int i = 0; i < REF_NODES; i++) {
        int targets[3] = {0, 1, (i * i + 3) % REF_NODES};
        for (int t = 0; t < 3; t++) {
            if (targets[t] != i) {
                cbm_edge_t e = {.project = "test",
                                .source_id = ids[i],
                                .target_id = ids[targets[t]],
                                .type = "CALLS"};
                cbm_store_insert_edge(s, &e);
            }
        }
    }
    return s;
}

typedef int (*ref_cmp_fn)(const cbm_cypher_result_t *r, int a, int b);

/* The reference order: an insertion sort that moves a row before an earlier
 * one only when the earlier one compares greater — stable, like the old
 * bubble sorts. */
static void ref_stable_order(const cbm_cypher_result_t *r, int *order, ref_cmp_fn cmp) {
    for (int i = 0; i < r->row_count; i++) {
        order[i] = i;
    }
    for (int i = 1; i < r->row_count; i++) {
        for (int j = i; j > 0 && cmp(r, order[j - 1], order[j]) > 0; j--) {
            int swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
}

/* Columns 0 (name) and 1 (line): line descending, then name ascending. */
static int ref_line_desc_name(const cbm_cypher_result_t *r, int a, int b) {
    long la = strtol(r->rows[a][1], NULL, 10);
    long lb = strtol(r->rows[b][1], NULL, 10);
    int cmp = (lb > la) - (lb < la);
    return cmp != 0 ? cmp : strcmp(r->rows[a][0], r->rows[b][0]);
}

static int ref_all_equal(const cbm_cypher_result_t *r, int a, int b) {
    (void)r;
    (void)a;
    (void)b;
    return 0;
}

/* `sorted` must hold the rows of `unsorted` in the reference order. */
static bool ref_rows_in_order(const cbm_cypher_result_t *sorted,
                              const cbm_cypher_result_t *unsorted, ref_cmp_fn cmp) {
    if (sorted->row_count != unsorted->row_count || sorted->col_count != unsorted->col_count ||
        unsorted->row_count < REF_NODES) {
        return false;
    }
    int order[REF_NODES * 4];
    ref_stable_order(unsorted, order, cmp);
    for (int i = 0; i < sorted->row_count; i++) {
        for (int c = 0; c < sorted->col_count; c++) {
            if (strcmp(sorted->rows[i][c], unsorted->rows[order[i]][c]) != 0) {
                return false;
            }
        }
    }
    return true;
}

static bool ref_query(cbm_store_t *s, const char *query, cbm_cypher_result_t *out) {
    memset(out, 0, sizeof(*out));
    return cbm_cypher_execute(s, query, "test", 0, out) == 0;
}

TEST(cypher_order_by_is_the_stable_reference_order) {
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t plain;
    cbm_cypher_result_t sorted;
    ASSERT_TRUE(ref_query(s, "MATCH (f:Function) RETURN f.name, f.start_line", &plain));

    /* RETURN: numeric key descending, ties broken by the next key. */
    ASSERT_TRUE(ref_query(
        s, "MATCH (f:Function) RETURN f.name, f.start_line ORDER BY f.start_line DESC, f.name",
        &sorted));
    ASSERT_TRUE(ref_rows_in_order(&sorted, &plain, ref_line_desc_name));
    cbm_cypher_result_free(&sorted);

    /* WITH: the same keys through a projection. */
    ASSERT_TRUE(ref_query(s,
                          "MATCH (f:Function) WITH f.name AS n, f.start_line AS l "
                          "ORDER BY l DESC, n RETURN n, l",
                          &sorted));
    ASSERT_TRUE(ref_rows_in_order(&sorted, &plain, ref_line_desc_name));
    cbm_cypher_result_free(&sorted);
    cbm_cypher_result_free(&plain);

    /* All keys tied: both sorts keep the unsorted order. */
    ASSERT_TRUE(ref_query(s, "MATCH (f:Function) RETURN f.name, f.label", &plain));
    ASSERT_TRUE(
        ref_query(s, "MATCH (f:Function) RETURN f.name, f.label ORDER BY f.label", &sorted));
    ASSERT_TRUE(ref_rows_in_order(&sorted, &plain, ref_all_equal));
    cbm_cypher_result_free(&sorted);
    ASSERT_TRUE(ref_query(
        s, "MATCH (f:Function) WITH f.name AS n, f.label AS l ORDER BY l RETURN n, l", &sorted));
    ASSERT_TRUE(ref_rows_in_order(&sorted, &plain, ref_all_equal));
    cbm_cypher_result_free(&sorted);
    cbm_cypher_result_free(&plain);
    cbm_store_close(s);
    PASS();
}

/* `got` must be the first occurrence of every value in column 0 of `all`. */
static bool ref_first_occurrences(const cbm_cypher_result_t *got, const cbm_cypher_result_t *all) {
    int kept = 0;
    for (int i = 0; i < all->row_count; i++) {
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(all->rows[i][0], all->rows[j][0]) == 0;
        }
        if (seen) {
            continue;
        }
        if (kept >= got->row_count || strcmp(got->rows[kept][0], all->rows[i][0]) != 0) {
            return false;
        }
        kept++;
    }
    return kept == got->row_count;
}

TEST(cypher_distinct_keeps_first_occurrences) {
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t all;
    cbm_cypher_result_t got;
    ASSERT_TRUE(ref_query(s, "MATCH (f:Function) RETURN f.name", &all));
    ASSERT_TRUE(ref_query(s, "MATCH (f:Function) RETURN DISTINCT f.name", &got));
    ASSERT_EQ(got.row_count, REF_NAMES);
    ASSERT_TRUE(ref_first_occurrences(&got, &all));
    cbm_cypher_result_free(&got);
    ASSERT_TRUE(ref_query(s, "MATCH (f:Function) WITH DISTINCT f.name AS n RETURN n", &got));
    ASSERT_EQ(got.row_count, REF_NAMES);
    ASSERT_TRUE(ref_first_occurrences(&got, &all));
    cbm_cypher_result_free(&got);
    cbm_cypher_result_free(&all);
    cbm_store_close(s);
    PASS();
}

/* `got` (column 0 = callee, column 1 = count) must list the callees of
 * `pairs` (column 0 = callee, column 1 = caller name) in first-seen order,
 * each with its caller count — of distinct names when `distinct`. */
static bool ref_groups_match(const cbm_cypher_result_t *got, const cbm_cypher_result_t *pairs,
                             bool distinct) {
    enum { REF_PAIRS_MAX = REF_NODES * 4 };
    if (pairs->row_count > REF_PAIRS_MAX) {
        return false;
    }
    bool first_callee[REF_PAIRS_MAX];
    bool first_pair[REF_PAIRS_MAX];
    for (int k = 0; k < pairs->row_count; k++) {
        first_callee[k] = true;
        first_pair[k] = true;
        for (int m = 0; m < k; m++) {
            if (strcmp(pairs->rows[m][0], pairs->rows[k][0]) == 0) {
                first_callee[k] = false;
                first_pair[k] = first_pair[k] && strcmp(pairs->rows[m][1], pairs->rows[k][1]) != 0;
            }
        }
    }
    int groups = 0;
    for (int i = 0; i < pairs->row_count; i++) {
        if (!first_callee[i]) {
            continue;
        }
        int count = 0;
        for (int k = 0; k < pairs->row_count; k++) {
            if (strcmp(pairs->rows[k][0], pairs->rows[i][0]) == 0) {
                count += !distinct || first_pair[k] ? 1 : 0;
            }
        }
        char want[16];
        snprintf(want, sizeof(want), "%d", count);
        if (groups >= got->row_count || strcmp(got->rows[groups][0], pairs->rows[i][0]) != 0 ||
            strcmp(got->rows[groups][1], want) != 0) {
            return false;
        }
        groups++;
    }
    return groups == got->row_count;
}

TEST(cypher_grouping_keeps_first_seen_groups) {
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t pairs;
    cbm_cypher_result_t got;
    ASSERT_TRUE(ref_query(
        s, "MATCH (a)-[:CALLS]->(b:Function) RETURN b.qualified_name, a.name", &pairs));
    ASSERT_GT(pairs.row_count, REF_NODES * 2);

    ASSERT_TRUE(ref_query(
        s, "MATCH (a)-[:CALLS]->(b:Function) RETURN b.qualified_name, count(a) AS c", &got));
    ASSERT_TRUE(ref_groups_match(&got, &pairs, false));
    cbm_cypher_result_free(&got);

    ASSERT_TRUE(ref_query(s,
                          "MATCH (a)-[:CALLS]->(b:Function) "
                          "RETURN b.qualified_name, count(DISTINCT a.name) AS c",
                          &got));
    ASSERT_TRUE(ref_groups_match(&got, &pairs, true));
    cbm_cypher_result_free(&got);

    ASSERT_TRUE(ref_query(s,
                          "MATCH (a)-[:CALLS]->(b:Function) WITH b.qualified_name AS q, "
                          "count(DISTINCT a.name) AS c RETURN q, c",
                          &got));
    ASSERT_TRUE(ref_groups_match(&got, &pairs, true));
    cbm_cypher_result_free(&got);
    cbm_cypher_result_free(&pairs);
    cbm_store_close(s);
    PASS();
}

/* The caller-supplied output-row limit sizes the initial binding array; a
 * value above the engine ceiling is clamped to it before that happens, and a
 * value inside the ceiling still bounds the row count. */
TEST(cypher_exec_max_rows_above_ceiling_is_clamped) {
    cbm_store_t *s = setup_cypher_store();
    cbm_cypher_result_t r = {0};

    int rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "test", INT_MAX, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 4);
    cbm_cypher_result_free(&r);

    memset(&r, 0, sizeof(r));
    rc = cbm_cypher_execute(s, "MATCH (f:Function) RETURN f.name", "test", 2, &r);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(r.row_count, 2);
    cbm_cypher_result_free(&r);

    cbm_store_close(s);
    PASS();
}


#ifdef CBM_ENABLE_TEST_SEAMS
/* ── Streamed execution: chunks of start nodes give the single pass ──
 *
 * execute_single runs the start nodes through the pipeline in chunks rather
 * than materialising every binding first (kernel count(*): 18 GB RSS). The
 * chunked run must be indistinguishable from one pass over the whole start
 * set: same rows in the same order, first-seen group order, ORDER BY ties,
 * DISTINCT, LIMIT / max_rows truncation and OPTIONAL fallback rows. Each query
 * runs as one chunk (the reference) and in chunks of 1, 7, 64 and the
 * production default, so chunk boundaries fall inside groups and inside
 * every start node's expansion order. */

static int stream_run(cbm_store_t *s, const char *query, int max_rows, int chunk,
                      cbm_cypher_result_t *out) {
    memset(out, 0, sizeof(*out));
    cbm_cypher_test_set_stream_chunk(chunk);
    int rc = cbm_cypher_execute(s, query, "test", max_rows, out);
    cbm_cypher_test_set_stream_chunk(0);
    return rc;
}

static bool stream_same_str(const char *a, const char *b) {
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static bool stream_results_equal(const cbm_cypher_result_t *a, const cbm_cypher_result_t *b) {
    if (a->col_count != b->col_count || a->row_count != b->row_count ||
        !stream_same_str(a->warning, b->warning) || !stream_same_str(a->error, b->error)) {
        return false;
    }
    for (int c = 0; c < a->col_count; c++) {
        if (strcmp(a->columns[c], b->columns[c]) != 0) {
            return false;
        }
    }
    for (int r = 0; r < a->row_count; r++) {
        for (int c = 0; c < a->col_count; c++) {
            if (strcmp(a->rows[r][c], b->rows[r][c]) != 0) {
                return false;
            }
        }
    }
    return true;
}

/* True when every chunk size reproduces the single-pass result, which is
 * left in *ref for the caller's own checks (caller frees). */
static bool stream_matches_single_pass(cbm_store_t *s, const char *query, int max_rows,
                                       cbm_cypher_result_t *ref) {
    static const int chunks[] = {1, 7, 64, 0 /* production default */};
    int ref_rc = stream_run(s, query, max_rows, INT_MAX, ref);
    bool same = ref_rc == 0;
    for (size_t i = 0; same && i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        cbm_cypher_result_t got;
        int rc = stream_run(s, query, max_rows, chunks[i], &got);
        same = rc == ref_rc && stream_results_equal(ref, &got);
        if (!same) {
            printf("  chunk %d differs from the single pass: %s\n", chunks[i], query);
        }
        cbm_cypher_result_free(&got);
    }
    return same;
}

/* Every pipeline consumer across chunk boundaries: unsorted rows, global and
 * grouped aggregates (groups fed from many chunks), DISTINCT, late WHERE,
 * collect, WITH aggregation and WITH sorting, RETURN * with and without a
 * WITH, the default projection, edge variables and variable-length hops. */
TEST(cypher_stream_chunks_match_single_pass) {
    static const char *const queries[] = {
        "MATCH (a)-[:CALLS]->(b) RETURN a.name, b.qualified_name",
        "MATCH (a)-[:CALLS]->(b) RETURN count(*)",
        "MATCH (a)-[:CALLS]->(b:Function) RETURN b.qualified_name, count(a) AS c ORDER BY c DESC",
        "MATCH (a:Function)-[:CALLS]->(b) RETURN b.name, count(DISTINCT a.name) AS c",
        "MATCH (a)-[:CALLS]->(b) RETURN DISTINCT b.name",
        "MATCH (a)-[:CALLS]->(b) WHERE b.start_line > 20 RETURN a.name, b.name, b.start_line",
        "MATCH (a)-[:CALLS]->(b) RETURN b.name, collect(a.name) AS cs",
        ("MATCH (a:Function)-[:CALLS]->(b) WITH b, count(a) AS c WHERE c > 1 "
         "RETURN b.name, c ORDER BY c DESC LIMIT 20"),
        "MATCH (a)-[:CALLS]->(b) WITH a.name AS n, b.start_line AS l ORDER BY l DESC RETURN n, l",
        "MATCH (a)-[:CALLS]->(b) WITH DISTINCT b.name AS n RETURN n",
        "MATCH (a)-[:CALLS]->(b) RETURN *",
        "MATCH (a)-[:CALLS]->(b) WITH a.name AS n RETURN *",
        "MATCH (a)-[:CALLS]->(b)",
        "MATCH (a)-[r:CALLS]->(b) RETURN type(r), a.name ORDER BY a.name",
        "MATCH (a)-[:CALLS*1..2]->(b) RETURN a.name, b.name",
    };
    cbm_store_t *s = setup_reference_store();
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_cypher_result_t ref;
        ASSERT_TRUE(stream_matches_single_pass(s, queries[i], 0, &ref));
        ASSERT_GT(ref.row_count, 0);
        cbm_cypher_result_free(&ref);
    }
    cbm_store_close(s);
    PASS();
}

/* OPTIONAL fallback rows (a start node with no match keeps its row, target
 * unbound) sit between the expanded rows of their neighbours in every chunk
 * layout; the multi-pattern form keeps its single pass. */
TEST(cypher_stream_optional_across_chunks) {
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t ref;
    ASSERT_TRUE(stream_matches_single_pass(
        s, "OPTIONAL MATCH (a)<-[:CALLS]-(b) RETURN a.name, b.name", 0, &ref));
    int fallback = 0;
    for (int r = 0; r < ref.row_count; r++) {
        fallback += ref.rows[r][1][0] == '\0' ? 1 : 0;
    }
    ASSERT_GT(fallback, 0);             /* uncalled nodes keep a row */
    ASSERT_GT(ref.row_count, fallback); /* and called nodes expand */
    cbm_cypher_result_free(&ref);

    ASSERT_TRUE(stream_matches_single_pass(
        s, "OPTIONAL MATCH (a)<-[:CALLS]-(b) RETURN a.qualified_name, count(b) AS c", 0, &ref));
    ASSERT_EQ(ref.row_count, REF_NODES);
    cbm_cypher_result_free(&ref);

    ASSERT_TRUE(stream_matches_single_pass(
        s, "MATCH (a:Function) OPTIONAL MATCH (a)<-[:CALLS]-(b) RETURN a.name, count(b)", 0, &ref));
    ASSERT_GT(ref.row_count, 0);
    cbm_cypher_result_free(&ref);
    cbm_store_close(s);
    PASS();
}

/* A LIMIT or the max_rows budget fills up in one chunk while later chunks
 * still arrive: they must add nothing, and truncation is reported exactly
 * when the single pass reports it (engine budget yes, explicit LIMIT no). */
TEST(cypher_stream_limit_and_max_rows_across_chunks) {
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t ref;
    ASSERT_TRUE(stream_matches_single_pass(
        s, "MATCH (a)-[:CALLS]->(b) RETURN a.name, b.name LIMIT 25", 0, &ref));
    ASSERT_EQ(ref.row_count, 25);
    cbm_cypher_result_free(&ref);

    ASSERT_TRUE(stream_matches_single_pass(
        s, "MATCH (a)-[:CALLS]->(b) RETURN DISTINCT b.name LIMIT 30", 0, &ref));
    ASSERT_EQ(ref.row_count, 30);
    cbm_cypher_result_free(&ref);

    ASSERT_TRUE(stream_matches_single_pass(
        s, "MATCH (a)-[:CALLS]->(b) RETURN a.name, b.name SKIP 5 LIMIT 10", 0, &ref));
    ASSERT_EQ(ref.row_count, 10);
    cbm_cypher_result_free(&ref);

    static const char *const budgeted[] = {
        "MATCH (a)-[:CALLS]->(b) RETURN a.name, b.name",
        "MATCH (a)-[:CALLS]->(b) RETURN a.name, b.name ORDER BY b.name",
        "MATCH (a)-[:CALLS]->(b) RETURN *",
        "MATCH (a)-[:CALLS]->(b)",
    };
    for (size_t i = 0; i < sizeof(budgeted) / sizeof(budgeted[0]); i++) {
        ASSERT_TRUE(stream_matches_single_pass(s, budgeted[i], 30, &ref));
        ASSERT_EQ(ref.row_count, 30);
        cbm_cypher_result_free(&ref);
    }
    cbm_store_close(s);
    PASS();
}

/* A start-node scan that fails part-way fails the whole query: the rows read
 * before the failure must not answer it as if they were all of them. Pages
 * hold 7 rows at chunk 7; the store seam fails a page after earlier pages
 * delivered rows (the all-nodes scan's first page is its empty NULL
 * file_path pass, hence one more page before the fault). */
TEST(cypher_stream_scan_failure_fails_query) {
    static const struct {
        const char *query;
        int pages_before_fault;
    } cases[] = {
        {"MATCH (a:Function)-[:CALLS]->(b) RETURN a.name, b.name", 1},
        {"MATCH (a:Function)-[:CALLS]->(b) RETURN count(*)", 1},
        {"MATCH (n) RETURN n.name", 2},
    };
    cbm_store_t *s = setup_reference_store();
    cbm_cypher_result_t r;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_EQ(stream_run(s, cases[i].query, 0, 7, &r), 0);
        ASSERT_GT(r.row_count, 0);
        cbm_cypher_result_free(&r);

        cbm_store_node_scan_test_fail_page_after(cases[i].pages_before_fault);
        int rc = stream_run(s, cases[i].query, 0, 7, &r);
        cbm_store_node_scan_test_fail_page_after(-1);
        ASSERT_TRUE(rc != 0);
        ASSERT_NOT_NULL(r.error);
        ASSERT_TRUE(strstr(r.error, "reading the graph failed") != NULL);
        ASSERT_TRUE(strstr(r.error, "injected page failure") != NULL);
        ASSERT_EQ(r.row_count, 0);
        cbm_cypher_result_free(&r);
    }
    /* The failure does not outlive its query. */
    ASSERT_EQ(stream_run(s, cases[0].query, 0, 7, &r), 0);
    ASSERT_GT(r.row_count, 0);
    ASSERT_NULL(r.error);
    cbm_cypher_result_free(&r);
    cbm_store_close(s);
    PASS();
}

/* The production chunk boundary itself: a start set of 2.5 chunks, a ring of
 * calls plus five hubs whose caller groups straddle both boundaries. */
TEST(cypher_stream_default_chunk_boundary) {
    enum { STREAM_NODES = 2600, STREAM_HUBS = 5 };
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "test", "/tmp/test");
    static int64_t ids[STREAM_NODES];
    for (int i = 0; i < STREAM_NODES; i++) {
        char name[32];
        char qn[32];
        snprintf(name, sizeof(name), "fn_%d", i % 211);
        snprintf(qn, sizeof(qn), "test.s%d", i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "ring.c",
                        .start_line = i % 37};
        ids[i] = cbm_store_upsert_node(s, &n);
    }
    for (int i = 0; i < STREAM_NODES; i++) {
        cbm_edge_t ring = {.project = "test",
                           .source_id = ids[i],
                           .target_id = ids[(i + 1) % STREAM_NODES],
                           .type = "CALLS"};
        cbm_edge_t hub = {.project = "test",
                          .source_id = ids[i],
                          .target_id = ids[i % STREAM_HUBS],
                          .type = "CALLS"};
        cbm_store_insert_edge(s, &ring);
        if (i >= STREAM_HUBS) { /* no hub self-loops */
            cbm_store_insert_edge(s, &hub);
        }
    }
    static const char *const queries[] = {
        "MATCH (a)-[:CALLS]->(b) RETURN count(*)",
        "MATCH (a)-[:CALLS]->(b) RETURN a.qualified_name, b.qualified_name",
        "MATCH (a:Function)-[:CALLS]->(b) RETURN b.qualified_name, count(a) AS c ORDER BY c DESC",
        "MATCH (a)-[:CALLS]->(b) WITH b.name AS n, count(DISTINCT a.name) AS c RETURN n, c",
        "MATCH (a)-[:CALLS]->(b) RETURN DISTINCT b.name LIMIT 300",
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        cbm_cypher_result_t ref;
        ASSERT_TRUE(stream_matches_single_pass(s, queries[i], 0, &ref));
        ASSERT_GT(ref.row_count, 0);
        cbm_cypher_result_free(&ref);
    }
    cbm_cypher_result_t ref;
    ASSERT_TRUE(stream_matches_single_pass(s, queries[0], 0, &ref));
    ASSERT_STR_EQ(ref.rows[0][0], "5195"); /* 2600 ring + 2595 hub edges */
    cbm_cypher_result_free(&ref);
    cbm_store_close(s);
    PASS();
}
#endif

    /* Streamed execution: chunked start set == single pass */

SUITE(cypher) {
    /* Lexer */
    RUN_TEST(cypher_lex_simple_match);
    RUN_TEST(cypher_lex_relationship);
    RUN_TEST(cypher_lex_string_literal);
    RUN_TEST(cypher_lex_single_quote_string);
    RUN_TEST(cypher_lex_string_overflow);
    RUN_TEST(cypher_lex_number);
    RUN_TEST(cypher_lex_operators);
    RUN_TEST(cypher_lex_keywords_case_insensitive);
    RUN_TEST(cypher_lex_pipe_and_star);
    RUN_TEST(cypher_lex_full_query);
    /* Parser */
    RUN_TEST(cypher_parse_simple_node);
    RUN_TEST(cypher_parse_rejects_trailing_tokens);
    RUN_TEST(cypher_parse_rejects_second_with_clause);
    RUN_TEST(cypher_parse_accepts_single_with_clause);
    RUN_TEST(cypher_parse_relationship_outbound);
    RUN_TEST(cypher_parse_relationship_inbound);
    RUN_TEST(cypher_parse_relationship_any);
    RUN_TEST(cypher_parse_variable_length);
    RUN_TEST(cypher_parse_variable_length_unbounded);
    RUN_TEST(cypher_parse_multiple_edge_types);
    RUN_TEST(cypher_parse_where_clause);
    RUN_TEST(cypher_parse_where_regex);
    RUN_TEST(cypher_parse_where_and);
    RUN_TEST(cypher_parse_return_simple);
    RUN_TEST(cypher_parse_return_count);
    RUN_TEST(cypher_parse_return_order_limit);
    RUN_TEST(cypher_parse_multikey_order_by_issue1334);
    RUN_TEST(cypher_parse_order_by_over_cap_rejected_issue1334);
    RUN_TEST(cypher_parse_nonnumeric_limit_rejected_issue1994);
    RUN_TEST(cypher_parse_nonnumeric_skip_rejected_issue1994);
    RUN_TEST(cypher_parse_word_limit_operand_rejected_issue1994);
    RUN_TEST(cypher_parse_numeric_skip_limit_still_accepted);
    RUN_TEST(cypher_parse_return_distinct);
    RUN_TEST(cypher_parse_inline_props);
    RUN_TEST(cypher_parse_error);
    /* Execution */
    RUN_TEST(cypher_exec_deadline_aborts_runaway_query_issue601);
    RUN_TEST(cypher_exec_deadline_allows_normal_query_issue601);
    RUN_TEST(cypher_exec_match_all_functions);
    RUN_TEST(cypher_issue240_labels_function);
    RUN_TEST(cypher_issue237_distinct_order_limit);
    RUN_TEST(cypher_issue252_tointeger);
    RUN_TEST(cypher_issue305_count_star_alias);
    RUN_TEST(cypher_count_star_empty_input_returns_zero);
    RUN_TEST(cypher_exec_where_eq);
    RUN_TEST(cypher_exec_varlength_path_semantics_issue797);
    RUN_TEST(cypher_exec_where_regex);
    RUN_TEST(cypher_exec_where_regex_oversized_pattern_matches_nothing);
    RUN_TEST(cypher_exec_where_contains);
    RUN_TEST(cypher_exec_where_starts_with);
    RUN_TEST(cypher_exec_return_properties);
    RUN_TEST(cypher_func_labels);
    RUN_TEST(cypher_func_type);
    RUN_TEST(cypher_func_id);
    RUN_TEST(cypher_func_keys);
    RUN_TEST(cypher_func_properties);
    RUN_TEST(cypher_func_tointeger_tofloat);
    RUN_TEST(cypher_func_size_reverse);
    RUN_TEST(cypher_func_multiarg);
    RUN_TEST(cypher_multi_prop_projection_no_alias);
    RUN_TEST(cypher_exists_no_callers);
    RUN_TEST(cypher_exists_has_outgoing_calls);
    RUN_TEST(cypher_exec_calls_relationship);
    RUN_TEST(cypher_exec_calls_with_where);
    RUN_TEST(cypher_exec_inbound);
    RUN_TEST(cypher_exec_count);
    RUN_TEST(cypher_exec_optional_bound_terminal_count_ignores_unbound);
    RUN_TEST(cypher_exec_label_on_unbound_side_keeps_seeds);
    RUN_TEST(cypher_exec_limit);
    RUN_TEST(cypher_exec_order_by);
    RUN_TEST(cypher_exec_variable_length);
    RUN_TEST(cypher_exec_var_length_no_reuse_self_loop);
    RUN_TEST(cypher_exec_var_length_truncation_surfaces_warning);
    RUN_TEST(cypher_exec_defines_edge);
    RUN_TEST(cypher_exec_no_results);
    RUN_TEST(cypher_exec_where_numeric);
    /* Go test ports */
    RUN_TEST(cypher_exec_distinct);
    RUN_TEST(cypher_exec_with_distinct_issue238);
    RUN_TEST(cypher_exec_where_label_test_issue241);
    RUN_TEST(cypher_exec_label_alternation_issue242);
    RUN_TEST(cypher_exec_count_distinct_issue239);
    RUN_TEST(cypher_exec_unsupported_func_errors_issue373);
    RUN_TEST(cypher_rejects_projection_of_dropped_with_var_issue1919);
    RUN_TEST(cypher_optional_match_target_still_allowed_issue1919);
    RUN_TEST(cypher_with_alias_stays_in_scope_issue1919);
    RUN_TEST(cypher_exec_unknown_func_return_errors);
    RUN_TEST(cypher_exec_inline_props);
    RUN_TEST(cypher_parse_where_starts_with);
    RUN_TEST(cypher_parse_where_contains);
    RUN_TEST(cypher_parse_where_numeric);
    /* Edge property tests (ported from cypher_test.go Feature 2) */
    RUN_TEST(cypher_edge_prop_access);
    RUN_TEST(cypher_edge_prop_in_where);
    RUN_TEST(cypher_edge_type_prop);
    RUN_TEST(cypher_edge_filter_contains);
    RUN_TEST(cypher_edge_filter_numeric_gte);
    RUN_TEST(cypher_bare_edge_return_exposes_properties_json);
    RUN_TEST(cypher_edge_return_without_filter);
    RUN_TEST(cypher_edge_filter_equals);
    RUN_TEST(cypher_edge_filter_starts_with);
    RUN_TEST(cypher_edge_combined_node_and_edge_filter);
    RUN_TEST(cypher_edge_filter_no_match);
    RUN_TEST(cypher_edge_filter_numeric_lt);
    RUN_TEST(cypher_edge_filter_regex);
    RUN_TEST(cypher_edge_builtin_type_filter);
    RUN_TEST(cypher_apply_limit);
    /* Phase 1: Simple operators */
    RUN_TEST(cypher_lex_neq_operators);
    RUN_TEST(cypher_lex_ends_keyword);
    RUN_TEST(cypher_lex_in_is_null);
    RUN_TEST(cypher_exec_where_neq);
    RUN_TEST(cypher_exec_where_neq_bang);
    RUN_TEST(cypher_exec_where_ends_with);
    RUN_TEST(cypher_exec_where_not);
    RUN_TEST(cypher_exec_where_not_target_node);
    RUN_TEST(cypher_exec_where_not_target_relationship);
    RUN_TEST(cypher_exec_where_not_source_node);
    RUN_TEST(cypher_exec_where_not_target_node_count);
    RUN_TEST(cypher_exec_where_in);
    RUN_TEST(cypher_exec_where_not_in);
    RUN_TEST(cypher_exec_where_is_null);
    RUN_TEST(cypher_exec_where_is_not_null);
    RUN_TEST(cypher_exec_return_star);
    RUN_TEST(cypher_return_star_dedups_repeated_pattern_var);
    RUN_TEST(cypher_return_star_after_with_names_aliases);
    RUN_TEST(cypher_wide_with_refused_not_truncated);
    RUN_TEST(cypher_wide_pattern_refused);
    RUN_TEST(cypher_wide_edge_pattern_refused);
    RUN_TEST(cypher_unnamed_head_takes_a_slot);
    RUN_TEST(cypher_scope_check_survives_wide_pattern);
    RUN_TEST(cypher_parse_neq);
    RUN_TEST(cypher_parse_in);
    RUN_TEST(cypher_parse_is_null);
    /* Phase 2: Expression tree */
    RUN_TEST(cypher_exec_where_or);
    RUN_TEST(cypher_exec_where_complex_bool);
    RUN_TEST(cypher_exec_where_xor);
    RUN_TEST(cypher_exec_where_not_prefix);
    RUN_TEST(cypher_parse_expr_tree_and_or);
    RUN_TEST(cypher_parse_expr_tree_nested);
    /* Phase 3: Unsupported keyword errors */
    RUN_TEST(cypher_error_create);
    RUN_TEST(cypher_error_delete);
    RUN_TEST(cypher_error_set);
    RUN_TEST(cypher_error_merge);
    RUN_TEST(cypher_error_call);
    /* Phase 4: SKIP + aggregation */
    RUN_TEST(cypher_exec_skip);
    RUN_TEST(cypher_exec_skip_limit);
    RUN_TEST(cypher_exec_multikey_order_by_keeps_limit_issue1334);
    RUN_TEST(cypher_exec_multikey_order_by_tiebreak_issue1334);
    RUN_TEST(cypher_exec_with_multikey_order_by_keeps_limit_issue1334);
    RUN_TEST(cypher_exec_sum);
    RUN_TEST(cypher_exec_avg);
    RUN_TEST(cypher_exec_min);
    RUN_TEST(cypher_exec_max);
    RUN_TEST(cypher_exec_collect);
    RUN_TEST(cypher_exec_count_star);
    RUN_TEST(cypher_issue1111_return_type_count_group);
    RUN_TEST(cypher_parse_skip);
    RUN_TEST(cypher_parse_sum_avg);
    RUN_TEST(cypher_parse_collect);
    /* Phase 5: String functions + CASE */
    RUN_TEST(cypher_exec_tolower);
    RUN_TEST(cypher_exec_toupper);
    RUN_TEST(cypher_exec_tostring);
    RUN_TEST(cypher_exec_case);
    RUN_TEST(cypher_parse_tolower);
    RUN_TEST(cypher_parse_case);
    /* Phase 6: WITH clause */
    RUN_TEST(cypher_exec_with_rename);
    RUN_TEST(cypher_exec_with_count);
    RUN_TEST(cypher_issue1111_with_type_count_group);
    RUN_TEST(cypher_issue1111_with_scalar_func_alias_no_node_leak);
    RUN_TEST(cypher_exec_with_where);
    RUN_TEST(cypher_exec_with_orderby_limit);
    RUN_TEST(cypher_parse_with);
    RUN_TEST(cypher_parse_with_where);
    /* Phase 7: OPTIONAL MATCH + multiple MATCH */
    RUN_TEST(cypher_exec_optional_match_no_result);
    RUN_TEST(cypher_exec_optional_match_has_result);
    RUN_TEST(cypher_exec_second_match_joins_bound_var);
    RUN_TEST(cypher_exec_optional_match_count_skips_null);
    RUN_TEST(cypher_exec_optional_match_zero_callers);
    RUN_TEST(cypher_exec_optional_match_empty_min_max);
    RUN_TEST(cypher_exec_multi_match);
    RUN_TEST(cypher_parse_optional_match);
    RUN_TEST(cypher_parse_multi_match);
    /* Phase 8: UNION */
    RUN_TEST(cypher_exec_union);
    RUN_TEST(cypher_exec_union_all);
    RUN_TEST(cypher_parse_union);
    /* Phase 9: UNWIND */
    RUN_TEST(cypher_parse_unwind);
    RUN_TEST(cypher_parse_unwind_var);
    RUN_TEST(cypher_parse_unwind_oversized_literal_no_overflow);
    RUN_TEST(cypher_parse_unwind_many_elements_no_overflow);
    RUN_TEST(cypher_exec_unlabeled_where_beyond_result_limit_issue1196);
    RUN_TEST(cypher_single_hop_seeds_from_selective_far_node);
    RUN_TEST(cypher_issue2208_with_agg_carried_node_props);
    RUN_TEST(cypher_issue2208_with_agg_groups_by_node_identity);
    RUN_TEST(cypher_issue2208_with_simple_carried_node_props);
    RUN_TEST(cypher_return_distinct_counts_distinct_rows_against_cap_issue1364);
    RUN_TEST(cypher_exec_where_not_on_relationship_target);
    RUN_TEST(cypher_exec_where_mixed_alias_and);
    RUN_TEST(cypher_exec_where_mixed_alias_or);
    RUN_TEST(cypher_exec_where_mixed_alias_xor);
    RUN_TEST(cypher_exec_optional_empty_label_rel_no_overflow);
    RUN_TEST(cypher_parse_error_frees_partial_pattern);
    RUN_TEST(cypher_long_or_chain_evaluates);
    RUN_TEST(cypher_long_and_chain_evaluates);
    RUN_TEST(cypher_mixed_chain_matches_nested_form);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(cypher_many_union_branches);
#endif
    RUN_TEST(cypher_fifty_union_branches);
    RUN_TEST(cypher_chain_with_unbound_alias_keeps_seed);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(cypher_exec_deadline_expires_at_positive_budget_boundary);
#endif
    RUN_TEST(cypher_order_by_is_the_stable_reference_order);
    RUN_TEST(cypher_distinct_keeps_first_occurrences);
    RUN_TEST(cypher_grouping_keeps_first_seen_groups);
    RUN_TEST(cypher_exec_max_rows_above_ceiling_is_clamped);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(cypher_stream_chunks_match_single_pass);
    RUN_TEST(cypher_stream_optional_across_chunks);
    RUN_TEST(cypher_stream_limit_and_max_rows_across_chunks);
    RUN_TEST(cypher_stream_default_chunk_boundary);
    RUN_TEST(cypher_stream_scan_failure_fails_query);
#endif
}
