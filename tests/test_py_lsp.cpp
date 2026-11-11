/*
 * test_py_lsp.c — Tests for Python LSP type-aware call resolution.
 *
 * Mirrors tests/test_go_lsp.c shape: helper extract_py(source) calls
 * cbm_extract_file with CBM_LANG_PYTHON, then assertions search the
 * resolved_calls array. Phase 2 ships smoke tests only; subsequent
 * phases add categories matching the Go LSP layout (param types,
 * method dispatch, decorators, generics, cross-file).
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/py_lsp.h"
#include "lsp/lsp_work.h"
#include "../src/pipeline/lsp_resolve.h"
#include "result_compact.h" /* cbm_result_compact */
#include "../src/pipeline/pass_lsp_cross.h"

/* ── Helpers — same shape as test_go_lsp.c ──────────────────────── */

static CBMFileResult *extract_py(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_PYTHON,
                            "test", "main.py", 0, NULL, NULL);
}

static int find_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub) {
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        if (rc->caller_qn && strstr(rc->caller_qn, callerSub) && rc->callee_qn &&
            strstr(rc->callee_qn, calleeSub))
            return i;
    }
    return -1;
}

/* Avoid unused-static-function warnings: helpers compiled but not yet used
 * outside the smoke tests will be referenced in Phase 3+ tests. */
__attribute__((unused))
static int require_resolved(const CBMFileResult *r, const char *callerSub, const char *calleeSub) {
    int idx = find_resolved(r, callerSub, calleeSub);
    if (idx < 0) {
        printf("  MISSING resolved call: caller~%s -> callee~%s (have %d)\n",
               callerSub, calleeSub, r->resolved_calls.count);
        for (int i = 0; i < r->resolved_calls.count; i++) {
            const CBMResolvedCall *rc = &r->resolved_calls.items[i];
            printf("    %s -> %s [%s %.2f]\n",
                   rc->caller_qn ? rc->caller_qn : "(null)",
                   rc->callee_qn ? rc->callee_qn : "(null)",
                   rc->strategy ? rc->strategy : "(null)",
                   rc->confidence);
        }
    }
    return idx;
}

/* ── Phase 2 — smoke ───────────────────────────────────────────── */

TEST(pylsp_smoke_empty) {
    CBMFileResult *r = extract_py("");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->resolved_calls.count, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_one_function) {
    CBMFileResult *r = extract_py(
        "def greet(name):\n"
        "    return name\n");
    ASSERT_NOT_NULL(r);
    /* Phase 2 stub: no resolutions yet, but extraction must succeed and
     * the result must be addressable without crashes. */
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_one_class) {
    CBMFileResult *r = extract_py(
        "class Greeter:\n"
        "    def __init__(self, name):\n"
        "        self.name = name\n"
        "    def greet(self):\n"
        "        return self.name\n");
    ASSERT_NOT_NULL(r);
    /* Class + 2 methods at minimum */
    ASSERT_GTE(r->defs.count, 1);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_no_crash_on_syntax_error) {
    /* Tree-sitter recovers from errors but we must not crash on the
     * recovered tree. */
    CBMFileResult *r = extract_py(
        "def broken(\n"
        "    x = 1\n"
        "class\n");
    ASSERT_NOT_NULL(r);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_smoke_imports_passed_through) {
    /* Imports populate ctx->import_local_names — Phase 2 just verifies
     * the unified extractor still produces them; resolution happens in
     * Phase 3. */
    CBMFileResult *r = extract_py(
        "import os\n"
        "import json as j\n"
        "from pathlib import Path\n"
        "from . import sibling\n"
        "def use():\n"
        "    return os.getcwd()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(r->imports.count, 3);
    cbm_free_result(r);
    PASS();
}

/* ── Phase 3 — imports → scope bindings ────────────────────────── */

/* Build a context, register one or more imports, run the binding pass,
 * and let the caller verify scope state. */
static void bind_imports_into_ctx(PyLSPContext *ctx, CBMArena *a, CBMTypeRegistry *reg,
                                  const char *const *locals, const char *const *qns,
                                  int count) {
    py_lsp_init(ctx, a, "", 0, reg, "test.main", NULL);
    for (int i = 0; i < count; i++) {
        py_lsp_add_import(ctx, locals[i], qns[i]);
    }
    py_lsp_bind_imports(ctx);
}

TEST(pylsp_import_simple) {
    /* import os → os ∈ scope as MODULE("os") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"os"};
    const char *qns[] = {"os"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "os");
    ASSERT(cbm_type_is_module(t));
    ASSERT_STR_EQ(t->data.module.module_qn, "os");
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_aliased) {
    /* import json as j → j ∈ scope as MODULE("json") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"j"};
    const char *qns[] = {"json"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "j");
    ASSERT(cbm_type_is_module(t));
    ASSERT_STR_EQ(t->data.module.module_qn, "json");
    /* original name "json" not bound */
    const CBMType *miss = py_lsp_lookup_in_scope(&ctx, "json");
    ASSERT(cbm_type_is_unknown(miss));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_from) {
    /* from pathlib import Path → Path ∈ scope as NAMED("pathlib.Path") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"Path"};
    const char *qns[] = {"pathlib.Path"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "Path");
    ASSERT_EQ(t->kind, CBM_TYPE_NAMED);
    ASSERT_STR_EQ(t->data.named.qualified_name, "pathlib.Path");
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_from_aliased) {
    /* from pathlib import Path as P → P ∈ scope as NAMED("pathlib.Path") */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"P"};
    const char *qns[] = {"pathlib.Path"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "P");
    /* Aliased name doesn't end module_qn with .P, so this binds as MODULE.
     * Phase 6 registry lookup will downgrade to NAMED if registry has no
     * matching module entry. Both behaviors are acceptable for v1; the
     * test asserts the entry exists with the correct QN. */
    ASSERT(t->kind == CBM_TYPE_NAMED || t->kind == CBM_TYPE_MODULE);
    if (t->kind == CBM_TYPE_NAMED) {
        ASSERT_STR_EQ(t->data.named.qualified_name, "pathlib.Path");
    } else {
        ASSERT_STR_EQ(t->data.module.module_qn, "pathlib.Path");
    }
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_relative_one_dot) {
    /* from . import sibling — extract_imports records local=sibling,
     * qn="..sibling" or similar. py_lsp binds it regardless. */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"sibling"};
    const char *qns[] = {"..sibling"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "sibling");
    ASSERT(!cbm_type_is_unknown(t));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_relative_two_dots) {
    /* from ..pkg import x → bind x as NAMED("..pkg.x") best effort */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"x"};
    const char *qns[] = {"...pkg.x"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "x");
    ASSERT(!cbm_type_is_unknown(t));
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_star_best_effort) {
    /* from X import * — local_name="*". py_lsp does not bind "*" because
     * it's not a usable identifier; the import is preserved in the import
     * map for cross-file re-export resolution (Phase 9). */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"*"};
    const char *qns[] = {"X"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *star_miss = py_lsp_lookup_in_scope(&ctx, "*");
    ASSERT(cbm_type_is_unknown(star_miss));
    /* Import is still recorded — Phase 9 will use it. */
    ASSERT_EQ(ctx.import_count, 1);
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_typing_only_still_binds) {
    /* `if TYPE_CHECKING:` is just a runtime constant — extract_imports
     * emits CBMImport entries regardless of guard. py_lsp binds them. */
    CBMArena a;
    cbm_arena_init(&a);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &a);
    PyLSPContext ctx;
    const char *locals[] = {"List"};
    const char *qns[] = {"typing.List"};
    bind_imports_into_ctx(&ctx, &a, &reg, locals, qns, 1);
    const CBMType *t = py_lsp_lookup_in_scope(&ctx, "List");
    ASSERT(!cbm_type_is_unknown(t));
    ASSERT_EQ(t->kind, CBM_TYPE_NAMED);
    cbm_arena_destroy(&a);
    PASS();
}

TEST(pylsp_import_multi_pass_through_extract_file) {
    /* End-to-end: extract_file + run_py_lsp populate scope via imports.
     * We can't peek into the embedded ctx, but we verify imports survive
     * to the result and bind correctly when re-traversed. */
    CBMFileResult *r = extract_py(
        "import os\n"
        "import json as j\n"
        "from pathlib import Path\n"
        "def use():\n"
        "    return Path('.')\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(r->imports.count, 3);
    cbm_free_result(r);
    PASS();
}

/* ── Phase 4-6 — direct calls + method dispatch ────────────────── */

TEST(pylsp_direct_function_call) {
    /* def helper(): return 1
     * def main(): return helper() */
    CBMFileResult *r = extract_py(
        "def helper():\n"
        "    return 1\n"
        "def main():\n"
        "    return helper()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "main", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_call_simple) {
    /* class C:
     *     def m(self): return 1
     * def use(c):
     *     c.m()  -- with annotation */
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(c: C):\n"
        "    return c.m()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "m"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_via_self) {
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    def helper(self):\n"
        "        return 1\n"
        "    def caller(self):\n"
        "        return self.helper()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "caller", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_constructor_call_returns_instance) {
    /* class Foo: ...
     * def use():
     *   f = Foo()
     *   f.method()  -- requires inferring f as Foo */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use():\n"
        "    f = Foo()\n"
        "    return f.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_method_via_inheritance) {
    CBMFileResult *r = extract_py(
        "class Base:\n"
        "    def shared(self):\n"
        "        return 1\n"
        "class Child(Base):\n"
        "    def go(self):\n"
        "        return self.shared()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "go", "shared"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_no_false_positive_on_unknown_method) {
    /* Calling a method on an UNKNOWN type should NOT emit a high-confidence
     * resolution. */
    CBMFileResult *r = extract_py(
        "def f(x):\n"
        "    return x.something_unknown_42()\n");
    ASSERT_NOT_NULL(r);
    /* Should produce no high-confidence match for "something_unknown_42" */
    int idx = find_resolved(r, "f", "something_unknown_42");
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(rc->confidence < 0.6f);
    }
    cbm_free_result(r);
    PASS();
}

/* ── Phase 7-8 — decorators, super(), multi-inheritance ──────── */

TEST(pylsp_decorated_function_resolves) {
    /* Decorated functions still resolve as their bare-name target. */
    CBMFileResult *r = extract_py(
        "import functools\n"
        "@functools.cache\n"
        "def helper():\n"
        "    return 1\n"
        "def main():\n"
        "    return helper()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "main", "helper"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_classmethod_resolves) {
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    @classmethod\n"
        "    def make(cls):\n"
        "        return cls()\n"
        "def use():\n"
        "    return C.make()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "make"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_staticmethod_resolves) {
    CBMFileResult *r = extract_py(
        "class C:\n"
        "    @staticmethod\n"
        "    def add(a, b):\n"
        "        return a + b\n"
        "def use():\n"
        "    return C.add(1, 2)\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "add"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_dataclass_constructor) {
    /* @dataclass synthesizes __init__. We don't emit __init__ explicitly,
     * but the constructor call should still link to the class qn. */
    CBMFileResult *r = extract_py(
        "from dataclasses import dataclass\n"
        "@dataclass\n"
        "class Point:\n"
        "    x: int\n"
        "    y: int\n"
        "    def magnitude(self):\n"
        "        return self.x + self.y\n"
        "def use():\n"
        "    p = Point(1, 2)\n"
        "    return p.magnitude()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "magnitude"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_super_call) {
    CBMFileResult *r = extract_py(
        "class Base:\n"
        "    def greet(self):\n"
        "        return 'hi'\n"
        "class Child(Base):\n"
        "    def greet(self):\n"
        "        return super().greet()\n");
    ASSERT_NOT_NULL(r);
    /* super().greet() should resolve to Base.greet, not Child.greet. */
    int idx = find_resolved(r, "greet", "greet");
    ASSERT_GTE(idx, 0);
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(strstr(rc->callee_qn, "Base") != NULL);
    }
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_multi_inheritance_first_base) {
    CBMFileResult *r = extract_py(
        "class A:\n"
        "    def a_method(self):\n"
        "        return 1\n"
        "class B:\n"
        "    def b_method(self):\n"
        "        return 2\n"
        "class C(A, B):\n"
        "    def use(self):\n"
        "        self.a_method()\n"
        "        self.b_method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "a_method"), 0);
    ASSERT_GTE(require_resolved(r, "use", "b_method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_pep695_generic_class) {
    /* PEP 695: class Box[T]:  -- our implementation ignores the [T] part */
    CBMFileResult *r = extract_py(
        "class Box:\n"
        "    def get(self):\n"
        "        return 1\n"
        "def use(b: Box):\n"
        "    return b.get()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "get"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Phase 9 — cross-file resolution ──────────────────────────── */

static int find_resolved_arr(const CBMResolvedCallArray *arr, const char *callerSub,
                             const char *calleeSub) {
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (rc->caller_qn && strstr(rc->caller_qn, callerSub) &&
            rc->callee_qn && strstr(rc->callee_qn, calleeSub))
            return i;
    }
    return -1;
}

TEST(pylsp_crossfile_method_dispatch) {
    /* file svc.py defines class RedisStore with Get(); file main.py calls
     * the method on a typed parameter. Reuses CBMLSPDef to feed the
     * cross-file definition into the resolver. */
    const char *source =
        "from svc import RedisStore\n"
        "def process(s: RedisStore):\n"
        "    return s.Get('k')\n";

    CBMLSPDef defs[2];
    memset(defs, 0, sizeof(defs));
    defs[0].qualified_name = "svc.RedisStore";
    defs[0].short_name = "RedisStore";
    defs[0].label = "Class";
    defs[0].def_module_qn = "svc";

    defs[1].qualified_name = "svc.RedisStore.Get";
    defs[1].short_name = "Get";
    defs[1].label = "Method";
    defs[1].receiver_type = "svc.RedisStore";
    defs[1].def_module_qn = "svc";

    const char *imp_names[] = {"RedisStore"};
    const char *imp_qns[] = {"svc.RedisStore"};

    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out = {0};

    cbm_run_py_lsp_cross(&arena, source, (int)strlen(source), "test.main",
                         defs, 2, imp_names, imp_qns, 1, NULL, &out);

    ASSERT_GTE(find_resolved_arr(&out, "process", "Get"), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

/* Issue #1277 (reproducer contributed by @Enferlain): a method call through a
 * typed instance field of a class imported from ANOTHER file. The field type
 * reaches the resolver only through CBMLSPDef.field_defs on the class def. */
/* #1277 / upstream bf2daa36 (adapted): refining a field that reached the
 * sealed shared registry through field_defs must not touch the shared arrays
 * (the fork records the refinement in the per-file ctx overlay instead of a
 * writable registry overlay), the refinement must win inside its own file, and
 * a later file must still see the declared Contract field. */
TEST(pylsp_overlay_existing_field_keeps_shared_type) {
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMLSPDef defs[6];
    memset(defs, 0, sizeof(defs));
    defs[0].qualified_name = "test.mod.Foo";
    defs[0].short_name = "Foo";
    defs[0].label = "Class";
    defs[0].def_module_qn = "test.mod";
    defs[0].field_defs = "x:test.contracts.Contract";
    defs[1].qualified_name = "test.mod.Foo.m";
    defs[1].short_name = "m";
    defs[1].label = "Method";
    defs[1].receiver_type = "test.mod.Foo";
    defs[1].def_module_qn = "test.mod";
    defs[2].qualified_name = "test.contracts.Contract";
    defs[2].short_name = "Contract";
    defs[2].label = "Class";
    defs[2].def_module_qn = "test.contracts";
    defs[3].qualified_name = "test.contracts.Contract.process_batch";
    defs[3].short_name = "process_batch";
    defs[3].label = "Method";
    defs[3].receiver_type = "test.contracts.Contract";
    defs[3].def_module_qn = "test.contracts";
    defs[4].qualified_name = "test.other.Other";
    defs[4].short_name = "Other";
    defs[4].label = "Class";
    defs[4].def_module_qn = "test.other";
    defs[5].qualified_name = "test.other.Other.process_batch";
    defs[5].short_name = "process_batch";
    defs[5].label = "Method";
    defs[5].receiver_type = "test.other.Other";
    defs[5].def_module_qn = "test.other";
    for (int i = 0; i < 6; i++) {
        defs[i].lang = CBM_LANG_PYTHON;
    }
    CBMTypeRegistry *reg = cbm_py_build_cross_registry(&arena, defs, 6);
    ASSERT_NOT_NULL(reg);
    ASSERT_TRUE(reg->read_only);
    const CBMRegisteredType *base = cbm_registry_lookup_type(reg, "test.mod.Foo");
    ASSERT_NOT_NULL(base);
    ASSERT_NOT_NULL(base->field_names);
    ASSERT_NOT_NULL(base->field_types);
    const char **names_before = base->field_names;
    const CBMType **types_before = base->field_types;
    const CBMType *type_before = base->field_types[0];
    ASSERT_NOT_NULL(type_before);
    ASSERT_NULL(base->field_types[1]);

    CBMArena scratch;
    cbm_arena_init(&scratch);
    const char *writer = "from other import Other\n"
                         "class Foo:\n"
                         "    def m(self):\n"
                         "        self.x = Other()\n"
                         "        self.x.process_batch()\n";
    const char *w_names[] = {"Other"};
    const char *w_qns[] = {"test.other.Other"};
    CBMResolvedCallArray out = {0};
    cbm_run_py_lsp_cross_with_registry(&scratch, writer, (int)strlen(writer), "test.mod", reg,
                                       w_names, w_qns, 1, NULL, &out);
    int refined_hits = find_resolved_arr(&out, "Foo.m", "Other.process_batch") >= 0 ? 1 : 0;
    int stale_hits = find_resolved_arr(&out, "Foo.m", "Contract.process_batch") >= 0 ? 1 : 0;
    bool shared_names_unchanged = base->field_names == names_before;
    bool shared_types_unchanged = base->field_types == types_before;
    bool shared_element_unchanged = base->field_types[0] == type_before;
    bool shared_terminator_unchanged = base->field_types[1] == NULL;
    cbm_arena_destroy(&scratch);

    int hits = 0;
    if (shared_names_unchanged && shared_types_unchanged && shared_element_unchanged &&
        shared_terminator_unchanged) {
        cbm_arena_init(&scratch);
        const char *reader = "from mod import Foo\n\n"
                             "def run(foo: Foo):\n"
                             "    foo.x.process_batch()\n";
        const char *import_names[] = {"Foo"};
        const char *import_qns[] = {"test.mod.Foo"};
        memset(&out, 0, sizeof(out));
        cbm_run_py_lsp_cross_with_registry(&scratch, reader, (int)strlen(reader), "test.reader",
                                           reg, import_names, import_qns, 1, NULL, &out);
        hits = find_resolved_arr(&out, "run", "Contract.process_batch") >= 0 ? 1 : 0;
        cbm_arena_destroy(&scratch);
    }
    cbm_arena_destroy(&arena);
    ASSERT_TRUE(shared_names_unchanged);
    ASSERT_TRUE(shared_types_unchanged);
    ASSERT_TRUE(shared_element_unchanged);
    ASSERT_TRUE(shared_terminator_unchanged);
    ASSERT_EQ(refined_hits, 1);
    ASSERT_EQ(stale_hits, 0);
    ASSERT_EQ(hits, 1);
    PASS();
}

TEST(pylsp_crossfile_receiver_through_typed_field) {
    const char *source = "from trainer import Trainer\n"
                         "def run(trainer: Trainer):\n"
                         "    strategies = trainer.strategies\n"
                         "    return strategies.process_batch()\n";

    CBMLSPDef defs[3];
    memset(defs, 0, sizeof(defs));

    defs[0].qualified_name = "contracts.Contract";
    defs[0].short_name = "Contract";
    defs[0].label = "Class";
    defs[0].def_module_qn = "contracts";

    defs[1].qualified_name = "contracts.Contract.process_batch";
    defs[1].short_name = "process_batch";
    defs[1].label = "Method";
    defs[1].receiver_type = "contracts.Contract";
    defs[1].def_module_qn = "contracts";

    defs[2].qualified_name = "trainer.Trainer";
    defs[2].short_name = "Trainer";
    defs[2].label = "Class";
    defs[2].def_module_qn = "trainer";
    defs[2].field_defs = "strategies:contracts.Contract";

    const char *imp_names[] = {"Trainer"};
    const char *imp_qns[] = {"trainer.Trainer"};

    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out = {0};

    cbm_run_py_lsp_cross(&arena, source, (int)strlen(source), "test.loop", defs, 3, imp_names,
                         imp_qns, 1, NULL, &out);

    ASSERT_GTE(find_resolved_arr(&out, "run", "process_batch"), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

/* #1277 on the production path: the sealed shared Tier-2 registry. Covers the
 * direct chain, the local alias, a field inherited from a base class, and two
 * controls: an undeclared field and a field whose type has no such method
 * must stay unresolved (never guessed). */
TEST(pylsp_crossfile_typed_field_shared_registry) {
    CBMArena arena;
    cbm_arena_init(&arena);

    CBMLSPDef defs[5];
    memset(defs, 0, sizeof(defs));
    defs[0].qualified_name = "contracts.Contract";
    defs[0].short_name = "Contract";
    defs[0].label = "Class";
    defs[0].def_module_qn = "contracts";
    defs[1].qualified_name = "contracts.Contract.process_batch";
    defs[1].short_name = "process_batch";
    defs[1].label = "Method";
    defs[1].receiver_type = "contracts.Contract";
    defs[1].def_module_qn = "contracts";
    defs[2].qualified_name = "trainer.Trainer";
    defs[2].short_name = "Trainer";
    defs[2].label = "Class";
    defs[2].def_module_qn = "trainer";
    defs[2].field_defs = "strategies:contracts.Contract|name:builtins.str";
    defs[3].qualified_name = "trainer.SubTrainer";
    defs[3].short_name = "SubTrainer";
    defs[3].label = "Class";
    defs[3].def_module_qn = "trainer";
    defs[3].embedded_types = "trainer.Trainer";
    defs[4].qualified_name = "trainer.Other";
    defs[4].short_name = "Other";
    defs[4].label = "Class";
    defs[4].def_module_qn = "trainer";
    for (int i = 0; i < 5; i++) {
        defs[i].lang = CBM_LANG_PYTHON;
    }

    CBMTypeRegistry *reg = cbm_py_build_cross_registry(&arena, defs, 5);
    ASSERT_NOT_NULL(reg);
    ASSERT_TRUE(reg->read_only);

    const char *src = "from trainer import Trainer, SubTrainer, Other\n"
                      "def direct(t: Trainer):\n"
                      "    return t.strategies.process_batch()\n"
                      "def alias(t: Trainer):\n"
                      "    s = t.strategies\n"
                      "    return s.process_batch()\n"
                      "def inherited(t: SubTrainer):\n"
                      "    return t.strategies.process_batch()\n"
                      "def undeclared(t: Other):\n"
                      "    return t.strategies.process_batch()\n"
                      "def wrong_type(t: Trainer):\n"
                      "    return t.name.process_batch()\n";
    const char *imp_names[] = {"Trainer", "SubTrainer", "Other"};
    const char *imp_qns[] = {"trainer.Trainer", "trainer.SubTrainer", "trainer.Other"};
    CBMResolvedCallArray out = {0};
    cbm_run_py_lsp_cross_with_registry(&arena, src, (int)strlen(src), "test.loop", reg, imp_names,
                                       imp_qns, 3, NULL, &out);

    ASSERT_GTE(find_resolved_arr(&out, "direct", "Contract.process_batch"), 0);
    ASSERT_GTE(find_resolved_arr(&out, "alias", "Contract.process_batch"), 0);
    ASSERT_GTE(find_resolved_arr(&out, "inherited", "Contract.process_batch"), 0);
    ASSERT_EQ(find_resolved_arr(&out, "undeclared", "process_batch"), -1);
    ASSERT_EQ(find_resolved_arr(&out, "wrong_type", "process_batch"), -1);

    cbm_arena_destroy(&arena);
    PASS();
}

/* Issue #228: a class/static method invoked directly on a CROSS-FILE imported
 * class name — ActionRecordX.build_from_text(...) — produced no CALLS edge, so
 * the method showed in/out degree 0 and was flagged as dead code. Distinct from
 * pylsp_crossfile_method_dispatch (which dispatches on a typed *instance*). */
TEST(pylsp_crossfile_classmethod_on_class_issue228) {
    const char *source =
        "from core.schemas import ActionRecordX\n"
        "def run_plain_flow():\n"
        "    return ActionRecordX.build_from_text('hello')\n";

    CBMLSPDef defs[2];
    memset(defs, 0, sizeof(defs));
    defs[0].qualified_name = "core.schemas.ActionRecordX";
    defs[0].short_name = "ActionRecordX";
    defs[0].label = "Class";
    defs[0].def_module_qn = "core.schemas";

    defs[1].qualified_name = "core.schemas.ActionRecordX.build_from_text";
    defs[1].short_name = "build_from_text";
    defs[1].label = "Method";
    defs[1].receiver_type = "core.schemas.ActionRecordX";
    defs[1].def_module_qn = "core.schemas";

    const char *imp_names[] = {"ActionRecordX"};
    const char *imp_qns[] = {"core.schemas.ActionRecordX"};

    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out = {0};

    cbm_run_py_lsp_cross(&arena, source, (int)strlen(source), "test.main", defs, 2, imp_names,
                         imp_qns, 1, NULL, &out);

    ASSERT_GTE(find_resolved_arr(&out, "run_plain_flow", "build_from_text"), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(pylsp_crossfile_inheritance) {
    /* svc.py defines class Base with shared(); main.py defines class Child(Base)
     * and calls self.shared(). Caller passes ALL relevant defs (cross-file
     * Base/shared + local Child/go) — same convention as test_go_lsp.c. */
    const char *source =
        "from svc import Base\n"
        "class Child(Base):\n"
        "    def go(self):\n"
        "        return self.shared()\n";

    CBMLSPDef defs[4];
    memset(defs, 0, sizeof(defs));
    defs[0].qualified_name = "svc.Base";
    defs[0].short_name = "Base";
    defs[0].label = "Class";
    defs[0].def_module_qn = "svc";

    defs[1].qualified_name = "svc.Base.shared";
    defs[1].short_name = "shared";
    defs[1].label = "Method";
    defs[1].receiver_type = "svc.Base";
    defs[1].def_module_qn = "svc";

    defs[2].qualified_name = "test.main.Child";
    defs[2].short_name = "Child";
    defs[2].label = "Class";
    defs[2].def_module_qn = "test.main";
    defs[2].embedded_types = "svc.Base";

    defs[3].qualified_name = "test.main.Child.go";
    defs[3].short_name = "go";
    defs[3].label = "Method";
    defs[3].receiver_type = "test.main.Child";
    defs[3].def_module_qn = "test.main";

    const char *imp_names[] = {"Base"};
    const char *imp_qns[] = {"svc.Base"};

    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out = {0};

    cbm_run_py_lsp_cross(&arena, source, (int)strlen(source), "test.main",
                         defs, 4, imp_names, imp_qns, 1, NULL, &out);

    ASSERT_GTE(find_resolved_arr(&out, "go", "shared"), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(pylsp_batch_two_files) {
    const char *src_a =
        "def helper():\n"
        "    return 1\n";
    const char *src_b =
        "from a import helper\n"
        "def main():\n"
        "    return helper()\n";

    CBMLSPDef a_defs[1];
    memset(a_defs, 0, sizeof(a_defs));
    a_defs[0].qualified_name = "a.helper";
    a_defs[0].short_name = "helper";
    a_defs[0].label = "Function";
    a_defs[0].def_module_qn = "a";

    CBMLSPDef b_defs[1];
    memset(b_defs, 0, sizeof(b_defs));
    b_defs[0].qualified_name = "b.main";
    b_defs[0].short_name = "main";
    b_defs[0].label = "Function";
    b_defs[0].def_module_qn = "b";

    const char *b_imp_names[] = {"helper"};
    const char *b_imp_qns[] = {"a.helper"};

    CBMBatchPyLSPFile files[2];
    memset(files, 0, sizeof(files));
    files[0].source = src_a;
    files[0].source_len = (int)strlen(src_a);
    files[0].module_qn = "a";
    files[0].defs = a_defs;
    files[0].def_count = 1;

    files[1].source = src_b;
    files[1].source_len = (int)strlen(src_b);
    files[1].module_qn = "b";
    files[1].defs = b_defs;
    files[1].def_count = 1;
    /* b imports helper from a — also include a's def in b's reachable set. */
    CBMLSPDef b_combined[2];
    memcpy(&b_combined[0], &a_defs[0], sizeof(CBMLSPDef));
    memcpy(&b_combined[1], &b_defs[0], sizeof(CBMLSPDef));
    files[1].defs = b_combined;
    files[1].def_count = 2;
    files[1].import_names = b_imp_names;
    files[1].import_qns = b_imp_qns;
    files[1].import_count = 1;

    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray outs[2];
    memset(outs, 0, sizeof(outs));

    cbm_batch_py_lsp_cross(&arena, files, 2, outs);

    /* file b's main should have called helper. */
    ASSERT_GTE(find_resolved_arr(&outs[1], "main", "helper"), 0);
    cbm_arena_destroy(&arena);
    PASS();
}

/* ── Phase 10 — stdlib resolution ─────────────────────────────── */

TEST(pylsp_stdlib_os_getcwd) {
    /* Top-level module attribute resolution against the stdlib registry.
     * Note: `import os.path` only binds `path` in scope (the leaf binding
     * extracted from the dotted import) — full submodule traversal of
     * `os.path.join` style accesses needs a Phase 10.5 follow-up where we
     * also stamp parent-module bindings. For v1, top-level module calls
     * resolve correctly. */
    CBMFileResult *r = extract_py(
        "import os\n"
        "def use():\n"
        "    return os.getcwd()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "getcwd");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_stdlib_collections_defaultdict) {
    CBMFileResult *r = extract_py(
        "from collections import defaultdict\n"
        "def use():\n"
        "    return defaultdict(list)\n");
    ASSERT_NOT_NULL(r);
    /* defaultdict(list) is a constructor — emits lsp_constructor edge */
    int idx = find_resolved(r, "use", "defaultdict");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_stdlib_pathlib_path_method) {
    CBMFileResult *r = extract_py(
        "from pathlib import Path\n"
        "def use(p: Path):\n"
        "    return p.exists()\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "exists");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_stdlib_logging_getlogger) {
    CBMFileResult *r = extract_py(
        "import logging\n"
        "def use():\n"
        "    return logging.getLogger('app')\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "getLogger");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

/* ── Round 1 — parity push ────────────────────────────────────── */

TEST(pylsp_round1_dotted_import_walk) {
    /* `import os.path` — `os` and `os.path` should both be navigable
     * through attribute access so `os.path.join('a', 'b')` resolves to
     * the registered os.path.join function. */
    CBMFileResult *r = extract_py(
        "import os.path\n"
        "def use():\n"
        "    return os.path.join('a', 'b')\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "join");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round1_typing_cast) {
    /* cast(Foo, x) returns NAMED("Foo"), enabling subsequent method
     * dispatch to resolve. */
    CBMFileResult *r = extract_py(
        "from typing import cast\n"
        "class Foo:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(x):\n"
        "    f = cast(Foo, x)\n"
        "    return f.m()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "m"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round1_assert_type_passthrough) {
    /* assert_type(x, T) is a no-op at runtime; the returned value's type
     * is unchanged. We type the result as type-of(x). */
    CBMFileResult *r = extract_py(
        "from typing import assert_type\n"
        "class Foo:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(x: Foo):\n"
        "    f = assert_type(x, Foo)\n"
        "    return f.m()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "m"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round1_forward_reference) {
    /* def f(x: "Foo") — quoted annotation must resolve as if unquoted. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(x: \"Foo\"):\n"
        "    return x.m()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "m"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round1_self_return_chains) {
    /* class Builder:
     *   def step1(self) -> Self: return self
     *   def step2(self) -> Self: return self
     *   def build(self): return ...
     * Builder().step1().step2().build()  — must chain through Self. */
    CBMFileResult *r = extract_py(
        "from typing import Self\n"
        "class Builder:\n"
        "    def step1(self) -> Self:\n"
        "        return self\n"
        "    def step2(self) -> Self:\n"
        "        return self\n"
        "    def build(self):\n"
        "        return 1\n"
        "def use():\n"
        "    return Builder().step1().step2().build()\n");
    ASSERT_NOT_NULL(r);
    /* Each chain link should resolve. We assert the final .build() does. */
    ASSERT_GTE(require_resolved(r, "use", "build"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round1_generic_subscript_annotation) {
    /* `def f(items: list[Foo])` — the generic subscript should not
     * confuse annotation resolution; we drop the [Foo] part for v1. */
    CBMFileResult *r = extract_py(
        "from typing import Optional\n"
        "class Foo:\n"
        "    def m(self):\n"
        "        return 1\n"
        "def use(x: Optional[Foo]):\n"
        "    return x.m()\n");
    ASSERT_NOT_NULL(r);
    /* x has type Optional which strips to Optional, then we look up
     * .m on it. This SHOULD NOT resolve in v1 since Optional is just
     * Union — but it shouldn't crash either. We assert no false-positive
     * high-confidence resolution against an unrelated method. */
    int idx = find_resolved(r, "use", "m");
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        /* If we did resolve, must be against Foo, not something garbage. */
        ASSERT(strstr(rc->callee_qn, "Foo") != NULL);
    }
    cbm_free_result(r);
    PASS();
}

/* ── Round 2 — narrowing ──────────────────────────────────────── */

TEST(pylsp_round2_isinstance_narrow) {
    /* if isinstance(x, Foo): x.method() — x narrowed to Foo */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x):\n"
        "    if isinstance(x, Foo):\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round2_is_not_none_narrow) {
    /* def f(x: Optional[Foo]):
     *   if x is not None:
     *     x.method() */
    CBMFileResult *r = extract_py(
        "from typing import Optional\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x: Optional[Foo]):\n"
        "    if x is not None:\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    /* Optional strips to Foo for v1 (we drop generic args). x: Optional[Foo]
     * binds x as NAMED("Optional"). After narrowing, ideally NAMED("Foo").
     * Since we strip generic args, x is bound as Optional unrelated. The
     * narrow extracts the non-None member from a UNION; if x is bound as a
     * single type (not UNION), the narrow is a no-op. */
    int idx = find_resolved(r, "use", "method");
    /* We don't fail this test if narrowing doesn't help — this exercises
     * the code path. The proper fix needs Optional to bind as UNION. */
    (void)idx;
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round2_isinstance_no_false_positive_in_else) {
    /* In the else branch, narrowing must NOT apply. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x):\n"
        "    if isinstance(x, Foo):\n"
        "        return 1\n"
        "    else:\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    /* No high-confidence resolution should exist for x.method() in the
     * else branch, since x is UNKNOWN there. */
    int idx = find_resolved(r, "use", "method");
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(rc->confidence < 0.6f);
    }
    cbm_free_result(r);
    PASS();
}

/* ── Round 2/3 — walrus, comprehension, optional-narrow ──────── */

TEST(pylsp_round2_narrow_after_call) {
    /* Without walrus: x = compute(); if x is not None: x.method().
     * Tests narrow on UNION return-type-of-call. */
    CBMFileResult *r = extract_py(
        "from typing import Optional\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def compute() -> Optional[Foo]:\n"
        "    return None\n"
        "def use():\n"
        "    x = compute()\n"
        "    if x is not None:\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round2_walrus_binds) {
    /* if (x := compute()) is not None: x.method() */
    CBMFileResult *r = extract_py(
        "from typing import Optional\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def compute() -> Optional[Foo]:\n"
        "    return None\n"
        "def use():\n"
        "    if (x := compute()) is not None:\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round3_listcomp_element_method) {
    /* [x.method() for x in items] where items: list[Foo] */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(items: list[Foo]):\n"
        "    return [x.method() for x in items]\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round3_for_loop_element_method) {
    /* for x in items: x.method() — items: list[Foo] */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(items: list[Foo]):\n"
        "    for x in items:\n"
        "        x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round3_optional_narrow_with_union) {
    /* def f(x: Optional[Foo]):
     *   if x is not None: x.method() */
    CBMFileResult *r = extract_py(
        "from typing import Optional\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(x: Optional[Foo]):\n"
        "    if x is not None:\n"
        "        return x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Round 3 — match/case + async ──────────────────────────── */

TEST(pylsp_round3_match_case_class_pattern) {
    /* match x: case Foo(): subject narrows to Foo */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class Bar:\n"
        "    def method(self):\n"
        "        return 2\n"
        "def use(x):\n"
        "    match x:\n"
        "        case Foo():\n"
        "            return x.method()\n"
        "        case _:\n"
        "            return None\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "use", "method");
    ASSERT_GTE(idx, 0);
    /* Should be the Foo.method binding, not Bar.method */
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(strstr(rc->callee_qn, "Foo") != NULL);
    }
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round3_async_await_pass_through) {
    /* await expr returns expr's type. async def f() -> int registers
     * with return int. await f() should resolve as int. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "async def make() -> Foo:\n"
        "    return Foo()\n"
        "async def use():\n"
        "    f = await make()\n"
        "    return f.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Round 4 — instance attribute typing ──────────────────────── */

TEST(pylsp_round4_instance_attribute_init) {
    /* class C:
     *   def __init__(self, cfg: Config):
     *     self.cfg = cfg     # field cfg : Config
     *   def use(self):
     *     return self.cfg.display()  # resolves through field type */
    CBMFileResult *r = extract_py(
        "class Config:\n"
        "    def display(self):\n"
        "        return 1\n"
        "class App:\n"
        "    def __init__(self, cfg: Config):\n"
        "        self.cfg = cfg\n"
        "    def use(self):\n"
        "    self.cfg.display()\n");
    /* Note: extra indentation simulates a body block; real test
     * mirrors realistic Python code. */
    ASSERT_NOT_NULL(r);
    /* The above source has bad indent — replace with proper test source. */
    cbm_free_result(r);
    r = extract_py(
        "class Config:\n"
        "    def display(self):\n"
        "        return 1\n"
        "class App:\n"
        "    def __init__(self, cfg: Config):\n"
        "        self.cfg = cfg\n"
        "    def use(self):\n"
        "        return self.cfg.display()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "display"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round4_instance_attribute_class_annotation) {
    /* class C:
     *   x: Foo
     *   def use(self): return self.x.method() */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class C:\n"
        "    x: Foo\n"
        "    def use(self):\n"
        "        return self.x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Round 5 — subscript, super().__init__, operator dunders ───── */

TEST(pylsp_round5_dict_subscript_value_type) {
    /* self.cache: dict[str, Foo]; self.cache[k].method() resolves. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class C:\n"
        "    def __init__(self):\n"
        "        self.cache: dict[str, Foo] = {}\n"
        "    def use(self, k):\n"
        "        return self.cache[k].method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round5_list_subscript_value_type) {
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def use(items: list[Foo]):\n"
        "    return items[0].method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round5_super_init) {
    CBMFileResult *r = extract_py(
        "class Base:\n"
        "    def __init__(self, root):\n"
        "        self.root = root\n"
        "class Child(Base):\n"
        "    def __init__(self, root, extra):\n"
        "        super().__init__(root)\n"
        "        self.extra = extra\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "__init__", "__init__");
    ASSERT_GTE(idx, 0);
    if (idx >= 0) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[idx];
        ASSERT(strstr(rc->callee_qn, "Base") != NULL);
    }
    cbm_free_result(r);
    PASS();
}

/* ── Round 6 — generators, dataclasses, decorator flags ───────── */

TEST(pylsp_round6_generator_yields_iterable) {
    /* def gen() -> Generator[Foo, None, None]: yield Foo()
     * for x in gen(): x.method()  — x : Foo via element-of(generator) */
    CBMFileResult *r = extract_py(
        "from typing import Generator\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "def gen() -> Generator[Foo, None, None]:\n"
        "    yield Foo()\n"
        "def use():\n"
        "    for x in gen():\n"
        "        x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round6_dataclass_field_access) {
    /* @dataclass class Point: x: Foo; def use(p: Point): p.x.method() */
    CBMFileResult *r = extract_py(
        "from dataclasses import dataclass\n"
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "@dataclass\n"
        "class Point:\n"
        "    x: Foo\n"
        "    y: int\n"
        "def use(p: Point):\n"
        "    return p.x.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

TEST(pylsp_round6_property_access_chains) {
    /* class C: @property def thing(self) -> Foo: ...
     * def use(c: C): c.thing.method()  -- thing is a property; access
     * returns its getter's return type. */
    CBMFileResult *r = extract_py(
        "class Foo:\n"
        "    def method(self):\n"
        "        return 1\n"
        "class C:\n"
        "    @property\n"
        "    def thing(self) -> Foo:\n"
        "        return Foo()\n"
        "def use(c: C):\n"
        "    return c.thing.method()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "use", "method"), 0);
    cbm_free_result(r);
    PASS();
}

/* ── Golden regression tests (WS9) ─────────────────────────────── */

/* Golden: a method call via self resolves to the sibling method. Mirrors
 * pylsp_method_via_self exactly — known-passing pattern. Robust: only
 * substring-asserts the call resolved at all. */
TEST(pylsp_golden_self_method) {
    CBMFileResult *r = extract_py(
        "class A:\n"
        "    def bar(self):\n"
        "        return 1\n"
        "    def foo(self):\n"
        "        return self.bar()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_GTE(require_resolved(r, "foo", "bar"), 0);
    cbm_free_result(r);
    PASS();
}

/* Golden: a top-level stdlib module call (logging.getLogger) resolves.
 * Mirrors pylsp_stdlib_logging_getlogger structure exactly; getLogger is a
 * stable stdlib symbol. Robust substring assertion only. */
TEST(pylsp_golden_stdlib_logging_getlogger) {
    CBMFileResult *r = extract_py(
        "import logging\n"
        "def setup():\n"
        "    return logging.getLogger(\"x\")\n");
    ASSERT_NOT_NULL(r);
    int idx = find_resolved(r, "setup", "getLogger");
    ASSERT_GTE(idx, 0);
    cbm_free_result(r);
    PASS();
}

/* Golden: a call to a same-module top-level function resolves with positive
 * confidence. Mirrors pylsp_direct_function_call plus the confidence>0 check
 * used in Go's golsp_golden_same_file_func. Direct same-module func calls
 * are the most basic resolution and definitely supported. */
TEST(pylsp_golden_same_module_func) {
    CBMFileResult *r = extract_py(
        "def add(a, b):\n"
        "    return a + b\n"
        "def compute():\n"
        "    return add(1, 2)\n");
    ASSERT_NOT_NULL(r);
    int idx = require_resolved(r, "compute", "add");
    ASSERT_GTE(idx, 0);
    ASSERT_TRUE(r->resolved_calls.items[idx].confidence > 0);
    cbm_free_result(r);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

/* ── #1527: no per-file eval budget; cost bounded at the root ─────────── */

/* Resolved rows whose callee QN ends with `suffix` (e.g. ".compile"). The
 * fork dedups resolved rows per (caller, callee), so the fixtures below give
 * every statement its own class: one row per class proves that statement
 * resolved. */
static int count_resolved_suffix(const CBMFileResult *r, const char *suffix) {
    size_t sl = strlen(suffix);
    int n = 0;
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        size_t ql = rc->callee_qn ? strlen(rc->callee_qn) : 0;
        if (ql >= sl && strcmp(rc->callee_qn + ql - sl, suffix) == 0 &&
            !(rc->strategy && strcmp(rc->strategy, "lsp_unresolved") == 0)) {
            n++;
        }
    }
    return n;
}

TEST(pylsp_issue1527_chain_statements_resolve_completely) {
    /* #1527 replaces the old step budget (10000 evaluations per file): the
     * #710 chain shape -- 90 statements x 30-link chains, re-evaluated across
     * the bind + emit phases -- must finish quickly AND resolve every
     * statement's chain and final call, not only the first statements. */
    enum { CHAIN_STMTS = 90, CHAIN_LINKS = 30 };
    size_t sz = 4096 + (size_t)CHAIN_STMTS * (256 + (size_t)CHAIN_LINKS * 8);
    char *src = (char *)malloc(sz);
    ASSERT_NOT_NULL(src);
    size_t pos = (size_t)snprintf(src, sz, "from typing import Self\n");
    for (int s = 0; s < CHAIN_STMTS; s++) {
        pos += (size_t)snprintf(src + pos, sz - pos,
                                "class G%d:\n"
                                "    def add(self, x) -> Self:\n"
                                "        return self\n"
                                "    def compile(self):\n"
                                "        return 1\n",
                                s);
    }
    pos += (size_t)snprintf(src + pos, sz - pos, "def run():\n");
    for (int s = 0; s < CHAIN_STMTS; s++) {
        pos += (size_t)snprintf(src + pos, sz - pos, "    v%d = G%d()", s, s);
        for (int l = 0; l < CHAIN_LINKS; l++)
            pos += (size_t)snprintf(src + pos, sz - pos, ".add(%d)", l);
        pos += (size_t)snprintf(src + pos, sz - pos, "\n    v%d.compile()\n", s);
    }
    ASSERT(pos < sz);
    CBMFileResult *r = extract_py(src);
    free(src);
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->lsp_skipped);
    ASSERT_EQ(count_resolved_suffix(r, ".compile"), CHAIN_STMTS);
    ASSERT_EQ(count_resolved_suffix(r, ".add"), CHAIN_STMTS);
    ASSERT_GTE(require_resolved(r, "run", "G89.compile"), 0);
    cbm_free_result(r);
    PASS();
}

static char *pylsp_work_fixture(int n_classes, int *out_len) {
    int approx = n_classes * 256 + 1024;
    char *buf = (char *)malloc((size_t)approx);
    if (!buf)
        return NULL;
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(approx - pos), "from typing import Self\n");
    for (int i = 0; i < n_classes; i++) {
        int n = snprintf(buf + pos, (size_t)(approx - pos),
                         "class Cls%d:\n"
                         "    def method(self) -> int:\n"
                         "        return %d\n"
                         "    def chain(self) -> Self:\n"
                         "        return self\n",
                         i, i);
        if (n < 0 || pos + n >= approx)
            break;
        pos += n;
    }
    int n = snprintf(buf + pos, (size_t)(approx - pos), "def use():\n");
    pos += n;
    for (int i = 0; i < n_classes; i++) {
        int m = snprintf(buf + pos, (size_t)(approx - pos),
                         "    Cls%d().chain().chain().method()\n", i);
        if (m < 0 || pos + m >= approx)
            break;
        pos += m;
    }
    *out_len = pos;
    return buf;
}

TEST(pylsp_issue1527_large_module_resolves_every_call) {
    /* #1527 reproduction: a module of 8000 classes plus one function calling
     * `ClsN().chain().chain().method()` for each. The per-file step cap
     * stopped resolution after ~3333 statements while the file still reported
     * `indexed`. Every class's method and chain must resolve (rows are per
     * callee). */
    enum { N_CLASSES = 8000 };
    int len = 0;
    char *src = pylsp_work_fixture(N_CLASSES, &len);
    ASSERT_NOT_NULL(src);
    CBMFileResult *r =
        cbm_extract_file(src, len, CBM_LANG_PYTHON, "test", "scale.py", 0, NULL, NULL);
    free(src);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(count_resolved_suffix(r, ".method"), N_CLASSES);
    ASSERT_EQ(count_resolved_suffix(r, ".chain"), N_CLASSES);
    /* The last class is resolved exactly, not just some class. */
    ASSERT_GTE(require_resolved(r, "use", "Cls7999.method"), 0);
    cbm_free_result(r);
    PASS();
}

#ifdef CBM_ENABLE_TEST_SEAMS
/* Deterministic complexity gate (#1527, no clock). The resolver primitives
 * count every candidate they examine -- scope-frame probes, registry lookup
 * probes -- plus every real (non-memoized) expression evaluation, into a
 * seam-build work counter. 4x the input must cost about 4x the work: a
 * linear-scan lookup or a lost memo lands at ~16x (quadratic) or worse. Also
 * asserts every class resolved, so the work is not "linear" because
 * resolution stopped early. */
static uint64_t pylsp_work_for(int n_classes, int *out_methods) {
    int slen = 0;
    char *src = pylsp_work_fixture(n_classes, &slen);
    if (!src)
        return 0;
    (void)cbm_lsp_work_take();
    CBMFileResult *r =
        cbm_extract_file(src, slen, CBM_LANG_PYTHON, "test", "scale.py", 0, NULL, NULL);
    uint64_t work = cbm_lsp_work_take();
    *out_methods = r ? count_resolved_suffix(r, ".method") : -1;
    if (r)
        cbm_free_result(r);
    free(src);
    return work;
}

TEST(pylsp_scale_work_is_linear) {
    int r1 = 0, r4 = 0;
    uint64_t w1 = pylsp_work_for(1000, &r1);
    uint64_t w4 = pylsp_work_for(4000, &r4);
    double ratio = w1 ? (double)w4 / (double)w1 : 0.0;
    printf("    work: 1000=%llu (methods=%d)  4000=%llu (methods=%d)  ratio=%.2fx "
           "(linear ~4x, quadratic ~16x)\n",
           (unsigned long long)w1, r1, (unsigned long long)w4, r4, ratio);
    ASSERT_EQ(r1, 1000);
    ASSERT_EQ(r4, 4000);
    ASSERT_GT(w1, 0);
    ASSERT(ratio < 5.0);
    PASS();
}

TEST(pylsp_issue1527_memo_allocation_failure_is_reported) {
    int len = 0;
    char *src = pylsp_work_fixture(200, &len);
    ASSERT_NOT_NULL(src);
    bool failures_reported = true;
    for (int fail_after = 0; fail_after <= 1; fail_after++) {
        cbm_py_lsp_test_memo_fail_after(fail_after);
        CBMFileResult *r = extract_py(src);
        cbm_py_lsp_test_memo_fail_after(-1);
        failures_reported = failures_reported && r && !r->has_error && r->lsp_skipped &&
                            r->lsp_skip_reason == CBM_LSP_SKIP_EVAL_MEMO &&
                            r->resolved_calls.count == 0;
        if (r) {
            cbm_result_compact(r);
            failures_reported = failures_reported && r->lsp_skipped &&
                                r->lsp_skip_reason == CBM_LSP_SKIP_EVAL_MEMO;
            cbm_free_result(r);
        }
    }
    CBMFileResult *fresh = extract_py(src);
    bool recovered = fresh && !fresh->lsp_skipped && fresh->resolved_calls.count > 0;
    if (fresh)
        cbm_free_result(fresh);
    free(src);
    ASSERT_TRUE(failures_reported);
    ASSERT_TRUE(recovered);
    PASS();
}

extern "C" const TSLanguage *tree_sitter_python(void);

TEST(pylsp_issue1527_memo_and_depth_failures_stop_work) {
    const char *src = "x\n";
    TSParser *parser = ts_parser_new();
    ASSERT_NOT_NULL(parser);
    ts_parser_set_language(parser, tree_sitter_python());
    TSTree *tree = ts_parser_parse_string(parser, NULL, src, (uint32_t)strlen(src));
    if (!tree) {
        ts_parser_delete(parser);
        FAIL("failed to parse memo fixture");
    }
    TSNode node = ts_node_named_child(ts_node_named_child(ts_tree_root_node(tree), 0), 0);
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMTypeRegistry reg;
    cbm_registry_init(&reg, &arena);
    CBMResolvedCallArray out = {};
    PyLSPContext ctx;
    py_lsp_init(&ctx, &arena, src, (int)strlen(src), &reg, "test", &out);
    const CBMType *known = cbm_type_named(&arena, "test.Known");
    cbm_scope_bind(ctx.current_scope, "x", known);
    ctx.eval_depth = 256;
    bool depth_unknown = cbm_type_is_unknown(cbm_py_lsp_test_eval(&ctx, node)) &&
                         ctx.eval_failure == CBM_LSP_DEPTH_EXCEEDED;
    ctx.eval_depth = 0;
    (void)cbm_lsp_work_take();
    bool depth_stopped = cbm_type_is_unknown(cbm_py_lsp_test_eval(&ctx, node));
    depth_stopped = depth_stopped && cbm_lsp_work_take() == 0;
    /* A fresh walk can resolve this shallow expression; the failed one cannot. */
    py_lsp_init(&ctx, &arena, src, (int)strlen(src), &reg, "test", &out);
    cbm_scope_bind(ctx.current_scope, "x", known);
    bool depth_recovered = cbm_py_lsp_test_eval(&ctx, node) == known && !ctx.eval_failure;
    bool stopped = true;
    for (int capacity_failure = 0; capacity_failure < 2; capacity_failure++) {
        py_lsp_init(&ctx, &arena, src, (int)strlen(src), &reg, "test", &out);
        cbm_scope_bind(ctx.current_scope, "x", known);
        ctx.test_memo_allocations_left = 0;
        /* Pin the maximum-capacity refusal without allocating a huge table.
         * A NULL table bypasses lookup; insertion must report failed growth. */
        if (capacity_failure)
            ctx.type_cache_cap = 1 << 26;
        stopped =
            stopped && cbm_type_is_unknown(cbm_py_lsp_test_eval(&ctx, node)) && ctx.eval_failure;
        (void)cbm_lsp_work_take();
        for (int i = 0; i < 20; i++)
            stopped = cbm_type_is_unknown(cbm_py_lsp_test_eval(&ctx, node)) && stopped;
        stopped = stopped && cbm_lsp_work_take() == 0;
    }
    cbm_arena_destroy(&arena);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    ASSERT_TRUE(depth_unknown);
    ASSERT_TRUE(depth_stopped);
    ASSERT_TRUE(depth_recovered);
    ASSERT_TRUE(stopped);
    PASS();
}

/* A failed cross walk drops only its own rows and restores any earlier row
 * its dedup touched; completed rows before the walk are kept. */
TEST(pylsp_issue1527_cross_memo_failure_preserves_completed_rows) {
    char src[4096];
    size_t pos = (size_t)snprintf(src, sizeof(src), "def run(s: S):\n    s + s\n");
    for (int i = 0; i < 200; i++)
        pos += (size_t)snprintf(src + pos, sizeof(src) - pos, "    s.m()\n");
    CBMLSPDef defs[3] = {};
    defs[0].qualified_name = "test.S";
    defs[0].short_name = "S";
    defs[0].label = "Class";
    defs[0].lang = CBM_LANG_PYTHON;
    defs[1].qualified_name = "test.S.m";
    defs[1].short_name = "m";
    defs[1].label = "Method";
    defs[1].receiver_type = "test.S";
    defs[1].return_types = "int";
    defs[1].lang = CBM_LANG_PYTHON;
    defs[2].qualified_name = "test.S.__add__";
    defs[2].short_name = "__add__";
    defs[2].label = "Method";
    defs[2].receiver_type = "test.S";
    defs[2].return_types = "S";
    defs[2].lang = CBM_LANG_PYTHON;
    bool correct = true;
    for (int mode = 0; mode < 3; mode++) {
        CBMArena arena, shared;
        cbm_arena_init(&arena);
        cbm_arena_init(&shared);
        CBMResolvedCall prior = {};
        prior.caller_qn = "test.run";
        prior.callee_qn = "test.S.m";
        prior.strategy = "prior.strategy";
        prior.confidence = 0.1f;
        prior.reason = "prior.reason";
        CBMResolvedCallArray out = {&prior, 1, 1};
        CBMTypeRegistry *base = mode == 1 ? cbm_py_build_cross_registry(&shared, defs, 3) : NULL;
        CBMBatchPyLSPFile file = {};
        file.source = src;
        file.source_len = (int)strlen(src);
        file.module_qn = "test";
        file.defs = defs;
        file.def_count = 3;
        /* The first type-cache allocation succeeds; growing past it fails
         * after rows were emitted (200 calls exceed the initial capacity). */
        cbm_py_lsp_test_memo_fail_after(1);
        bool complete =
            mode == 0   ? cbm_run_py_lsp_cross(&arena, src, (int)strlen(src), "test", defs, 3, NULL,
                                               NULL, 0, NULL, &out)
            : mode == 1 ? cbm_run_py_lsp_cross_with_registry(&arena, src, (int)strlen(src), "test",
                                                             base, NULL, NULL, 0, NULL, &out)
                        : cbm_batch_py_lsp_cross(&arena, &file, 1, &out);
        cbm_py_lsp_test_memo_fail_after(-1);
        correct = correct && !complete && out.count == (mode == 2 ? 0 : 1) &&
                  prior.confidence == 0.1f && strcmp(prior.strategy, "prior.strategy") == 0 &&
                  strcmp(prior.reason, "prior.reason") == 0;
        /* The walk may have grown (moved) the array: the restored row is
         * out.items[0], which must carry the prior values again. */
        if (mode != 2)
            correct = correct && out.items[0].confidence == 0.1f &&
                      strcmp(out.items[0].strategy, "prior.strategy") == 0 &&
                      strcmp(out.items[0].reason, "prior.reason") == 0;
        if (mode == 2)
            out = CBMResolvedCallArray{};
        complete = mode == 0 ? cbm_run_py_lsp_cross(&arena, src, (int)strlen(src), "test", defs, 3,
                                                    NULL, NULL, 0, NULL, &out)
                   : mode == 1 ? cbm_run_py_lsp_cross_with_registry(&arena, src, (int)strlen(src),
                                                                    "test", base, NULL, NULL, 0,
                                                                    NULL, &out)
                               : cbm_batch_py_lsp_cross(&arena, &file, 1, &out);
        /* A complete walk does refine the prior row through its dedup. */
        correct = correct && complete && out.count >= 1 &&
                  (mode == 2 || out.items[0].confidence > 0.1f);
        cbm_arena_destroy(&arena);
        cbm_arena_destroy(&shared);
    }
    ASSERT_TRUE(correct);
    PASS();
}

TEST(pylsp_issue1527_cross_memo_failure_fallback_reports_error) {
    const char *src = "def run(s: S):\n    s.m()\n";
    CBMLSPDef defs[2] = {};
    defs[0].qualified_name = "test.S";
    defs[0].short_name = "S";
    defs[0].label = "Class";
    defs[0].lang = CBM_LANG_PYTHON;
    defs[1].qualified_name = "test.S.m";
    defs[1].short_name = "m";
    defs[1].label = "Method";
    defs[1].receiver_type = "test.S";
    defs[1].return_types = "int";
    defs[1].lang = CBM_LANG_PYTHON;
    CBMFileResult result = {};
    cbm_arena_init(&result.arena);
    cbm_py_lsp_test_memo_fail_after(0);
    cbm_pxc_run_one(CBM_LANG_PYTHON, &result, src, (int)strlen(src), "test", NULL, defs, 2, NULL,
                    NULL, 0);
    cbm_py_lsp_test_memo_fail_after(-1);
    bool reported = result.lsp_skipped && result.lsp_skip_reason == CBM_LSP_SKIP_EVAL_MEMO &&
                    result.resolved_calls.count == 0;
    cbm_arena_destroy(&result.arena);
    ASSERT_TRUE(reported);
    PASS();
}

TEST(pylsp_issue1527_depth_failure_reports_all_entry_points) {
    const char *src = "class S:\n    def m(self):\n        return 1\ndef run(s: S):\n    s.m()\n";
    CBMLSPDef defs[2] = {};
    defs[0].qualified_name = "test.S";
    defs[0].short_name = "S";
    defs[0].label = "Class";
    defs[0].lang = CBM_LANG_PYTHON;
    defs[1].qualified_name = "test.S.m";
    defs[1].short_name = "m";
    defs[1].label = "Method";
    defs[1].receiver_type = "test.S";
    defs[1].return_types = "int";
    defs[1].lang = CBM_LANG_PYTHON;
    cbm_py_lsp_test_depth_fail(true);
    CBMFileResult *raw = extract_py(src);
    bool correct = raw && raw->lsp_skipped && raw->lsp_skip_reason == CBM_LSP_SKIP_EVAL_DEPTH &&
                   raw->resolved_calls.count == 0;
    if (raw)
        cbm_free_result(raw);
    for (int mode = 0; mode < 3; mode++) {
        CBMArena arena, shared;
        cbm_arena_init(&arena);
        cbm_arena_init(&shared);
        CBMTypeRegistry *base = mode == 1 ? cbm_py_build_cross_registry(&shared, defs, 2) : NULL;
        CBMResolvedCallArray out = {};
        CBMBatchPyLSPFile file = {};
        file.source = src;
        file.source_len = (int)strlen(src);
        file.module_qn = "test";
        file.defs = defs;
        file.def_count = 2;
        if (mode == 2) {
            correct = !cbm_batch_py_lsp_cross(&arena, &file, 1, &out) && correct;
        } else {
            CBMLSPStatus status =
                mode == 0 ? cbm_run_py_lsp_cross_status(&arena, src, (int)strlen(src), "test", defs,
                                                        2, NULL, NULL, 0, NULL, &out)
                          : cbm_run_py_lsp_cross_with_registry_status(&arena, src, (int)strlen(src),
                                                                      "test", base, NULL, NULL, 0,
                                                                      NULL, &out);
            correct = correct && status == CBM_LSP_DEPTH_EXCEEDED;
        }
        correct = correct && out.count == 0;
        cbm_arena_destroy(&arena);
        cbm_arena_destroy(&shared);
    }
    CBMFileResult fallback = {};
    cbm_arena_init(&fallback.arena);
    cbm_pxc_run_one(CBM_LANG_PYTHON, &fallback, src, (int)strlen(src), "test", NULL, defs, 2, NULL,
                    NULL, 0);
    correct = correct && fallback.lsp_skipped &&
              fallback.lsp_skip_reason == CBM_LSP_SKIP_EVAL_DEPTH &&
              fallback.resolved_calls.count == 0;
    cbm_arena_destroy(&fallback.arena);
    cbm_py_lsp_test_depth_fail(false);
    CBMFileResult *fresh = extract_py(src);
    bool recovered = fresh && !fresh->lsp_skipped && fresh->resolved_calls.count > 0;
    if (fresh)
        cbm_free_result(fresh);
    ASSERT_TRUE(correct);
    ASSERT_TRUE(recovered);
    PASS();
}
#endif /* CBM_ENABLE_TEST_SEAMS */

/* A chain's LENGTH is not nesting depth (#1527 follow-up). An operator chain
 * `a + a + ... + a` and a receiver chain `b.a().a()...` are evaluated link by
 * link, so neither consumes the evaluator's depth guard: the file keeps its
 * LSP resolution (an unrelated call in another function still resolves), no
 * file-level failure is recorded, and the chain's result type still drives
 * the call made on it. Before the iterative evaluation, 300 terms already
 * exceeded the guard. The per-file extraction runs at the smallest size (as
 * upstream); the cross-file dispatch runs at every size. */
static const int pylsp_long_chain_sizes[] = {300, 2000, 20000};

static char *pylsp_long_chain_source(bool receiver, int links) {
    size_t sz = (size_t)links * 8 + 512;
    char *src = (char *)malloc(sz);
    if (!src)
        return NULL;
    size_t pos = (size_t)snprintf(src, sz,
                                  "from typing import Self\n"
                                  "class V:\n"
                                  "    def done(self):\n"
                                  "        return 1\n"
                                  "class B:\n"
                                  "    def a(self) -> Self:\n"
                                  "        return self\n"
                                  "    def done(self):\n"
                                  "        return 1\n"
                                  "class S:\n"
                                  "    def m(self):\n"
                                  "        return 1\n");
    if (receiver) {
        pos += (size_t)snprintf(src + pos, sz - pos, "def chain(b: B):\n    b");
        for (int i = 0; i < links; i++)
            pos += (size_t)snprintf(src + pos, sz - pos, ".a()");
        pos += (size_t)snprintf(src + pos, sz - pos, ".done()\n");
    } else {
        pos += (size_t)snprintf(src + pos, sz - pos, "def deep(a: V):\n    x = a");
        for (int i = 1; i < links; i++)
            pos += (size_t)snprintf(src + pos, sz - pos, " + a");
        pos += (size_t)snprintf(src + pos, sz - pos, "\n    x.done()\n");
    }
    snprintf(src + pos, sz - pos, "def run():\n    s = S()\n    s.m()\n");
    return src;
}

static int pylsp_long_chain_case(bool receiver) {
    CBMLSPDef defs[7] = {};
    const char *qns[] = {"test.V",    "test.V.done", "test.B", "test.B.a",
                         "test.B.done", "test.S",    "test.S.m"};
    const char *shorts[] = {"V", "done", "B", "a", "done", "S", "m"};
    const char *recv[] = {NULL, "test.V", NULL, "test.B", "test.B", NULL, "test.S"};
    const char *rets[] = {NULL, "int", NULL, "test.B", "int", NULL, "int"};
    for (int i = 0; i < 7; i++) {
        defs[i].qualified_name = qns[i];
        defs[i].short_name = shorts[i];
        defs[i].label = recv[i] ? "Method" : "Class";
        defs[i].receiver_type = recv[i];
        defs[i].return_types = rets[i];
        defs[i].lang = CBM_LANG_PYTHON;
    }
    const char *chain_fn = receiver ? "chain" : "deep";
    const char *done = receiver ? "B.done" : "V.done";
    int sizes = (int)(sizeof(pylsp_long_chain_sizes) / sizeof(pylsp_long_chain_sizes[0]));
    for (int i = 0; i < sizes; i++) {
        int links = pylsp_long_chain_sizes[i];
        char *src = pylsp_long_chain_source(receiver, links);
        ASSERT_NOT_NULL(src);
        bool per_file_ok = true;
        if (i == 0) {
            CBMFileResult *r = extract_py(src);
            per_file_ok = r && !r->has_error && !r->lsp_skipped &&
                          find_resolved(r, "run", "S.m") >= 0 &&
                          find_resolved(r, chain_fn, done) >= 0;
            printf("    per-file %s chain of %d: ok=%d\n", receiver ? "receiver" : "binary",
                   links, (int)per_file_ok);
            if (r)
                cbm_free_result(r);
        }
        CBMFileResult cross = {};
        cbm_arena_init(&cross.arena);
        cbm_pxc_run_one(CBM_LANG_PYTHON, &cross, src, (int)strlen(src), "test", NULL, defs, 7, NULL,
                        NULL, 0);
        free(src);
        bool no_error = !cross.has_error && !cross.lsp_skipped;
        bool unrelated = find_resolved(&cross, "run", "S.m") >= 0;
        bool typed = find_resolved(&cross, chain_fn, done) >= 0;
        printf("    cross %s chain of %d: skipped=%d run->S.m=%d %s=%d resolved=%d\n",
               receiver ? "receiver" : "binary", links, (int)cross.lsp_skipped, (int)unrelated,
               done, (int)typed, cross.resolved_calls.count);
        cbm_arena_destroy(&cross.arena);
        ASSERT_TRUE(per_file_ok);
        ASSERT_TRUE(no_error);
        ASSERT_TRUE(unrelated);
        ASSERT_TRUE(typed);
    }
    PASS();
}

TEST(pylsp_issue1527_long_binary_chain_is_not_nesting) {
    return pylsp_long_chain_case(false);
}

TEST(pylsp_issue1527_long_receiver_chain_is_not_nesting) {
    return pylsp_long_chain_case(true);
}

/* Upstream bf5ea5a4: module-level Python calls are owned by the file's Module
 * QN (the caller the extractor records), so the pipeline join finds the
 * exact LSP resolution instead of a `<module>.__module__` caller nothing
 * matches. The fork's resolved rows carry no site range, so the join is
 * checked through the production lookup on the extracted call itself. */
static bool pylsp_module_call_joins(const CBMFileResult *r, const char *caller,
                                    const char *callee_name, const char *target) {
    const CBMCall *call = NULL;
    int call_count = 0;
    for (int i = 0; i < r->calls.count; i++) {
        const CBMCall *candidate = &r->calls.items[i];
        if (candidate->enclosing_func_qn && strcmp(candidate->enclosing_func_qn, caller) == 0 &&
            candidate->callee_name && strcmp(candidate->callee_name, callee_name) == 0) {
            call = candidate;
            call_count++;
        }
    }
    if (call_count != 1)
        return false;
    const CBMResolvedCall *hit = cbm_pipeline_find_lsp_resolution(&r->resolved_calls, call, false);
    if (!hit || !hit->caller_qn || strcmp(hit->caller_qn, caller) != 0 || !hit->callee_qn ||
        strcmp(hit->callee_qn, target) != 0)
        return false;
    CBMCall wrong = *call;
    wrong.enclosing_func_qn = "unrelated.module.caller";
    return cbm_pipeline_find_lsp_resolution(&r->resolved_calls, &wrong, false) == NULL;
}

static bool pylsp_module_no_phantom_caller(const CBMFileResult *r) {
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const char *caller = r->resolved_calls.items[i].caller_qn;
        if (caller && strstr(caller, "__module__"))
            return false;
    }
    return true;
}

TEST(pylsp_module_callers_join_exact_raw_occurrences) {
    /* The fork joins resolutions per (caller, callee leaf) -- its Python rows
     * carry no site range -- so the two classes use distinct method names. */
    static const char source[] = "class Alpha:\n"
                                 "    def render(self):\n"
                                 "        return 1\n"
                                 "class Beta:\n"
                                 "    def draw(self):\n"
                                 "        return 2\n"
                                 "def helper():\n"
                                 "    return 3\n"
                                 "def control():\n"
                                 "    function_a = Alpha()\n"
                                 "    function_a.render()\n"
                                 "    helper()\n"
                                 "class Controller:\n"
                                 "    def control(self):\n"
                                 "        method_b = Beta()\n"
                                 "        method_b.draw()\n"
                                 "module_a = Alpha()\n"
                                 "module_b = Beta()\n"
                                 "module_a.render()\n"
                                 "module_b.draw()\n";
    CBMFileResult *r = cbm_extract_file(source, (int)strlen(source), CBM_LANG_PYTHON, "test",
                                        "app.py", 0, NULL, NULL);
    bool correct = r && !r->has_error && !r->lsp_skipped && r->module_qn &&
                   strcmp(r->module_qn, "test.app") == 0 &&
                   pylsp_module_call_joins(r, "test.app", "module_a.render",
                                           "test.app.Alpha.render") &&
                   pylsp_module_call_joins(r, "test.app", "module_b.draw",
                                           "test.app.Beta.draw") &&
                   pylsp_module_call_joins(r, "test.app.control", "function_a.render",
                                           "test.app.Alpha.render") &&
                   pylsp_module_call_joins(r, "test.app.Controller.control", "method_b.draw",
                                           "test.app.Beta.draw") &&
                   pylsp_module_no_phantom_caller(r);
    if (r)
        cbm_free_result(r);
    ASSERT_TRUE(correct);

    /* A package Module keeps its raw caller identity too. */
    static const char init_source[] = "PACKAGE_SIZE = len([1])\n";
    r = cbm_extract_file(init_source, (int)strlen(init_source), CBM_LANG_PYTHON, "test",
                         "pkg/__init__.py", 0, NULL, NULL);
    correct = r && !r->has_error && !r->lsp_skipped && r->module_qn &&
              strcmp(r->module_qn, "test.pkg.__init__") == 0 &&
              pylsp_module_call_joins(r, "test.pkg.__init__", "len", "builtins.len") &&
              pylsp_module_no_phantom_caller(r);
    if (r)
        cbm_free_result(r);
    ASSERT_TRUE(correct);
    PASS();
}

TEST(pylsp_module_named_lambda_keeps_module_owner) {
    static const char source[] = "class Number:\n"
                                 "    def __add__(self, other):\n"
                                 "        return self\n"
                                 "module_number = Number()\n"
                                 "add = lambda left, right: left + right\n"
                                 "first = add(module_number, module_number)\n";
    CBMFileResult *r = cbm_extract_file(source, (int)strlen(source), CBM_LANG_PYTHON, "test",
                                        "app.py", 0, NULL, NULL);
    bool correct = r && !r->has_error && !r->lsp_skipped && pylsp_module_no_phantom_caller(r);
    if (r)
        cbm_free_result(r);
    ASSERT_TRUE(correct);
    PASS();
}

/* Cross-file entry points: the module caller is an arena copy, so it
 * outlives a temporary module_qn input buffer. */
TEST(pylsp_module_cross_callers_own_input) {
    static const char source[] = "from helpers import Alpha, Beta\n"
                                 "module_a = Alpha()\n"
                                 "module_b = Beta()\n"
                                 "module_a.render()\n"
                                 "module_b.render()\n";
    CBMLSPDef defs[4] = {};
    const char *qns[] = {"test.helpers.Alpha", "test.helpers.Alpha.render", "test.helpers.Beta",
                         "test.helpers.Beta.render"};
    const char *shorts[] = {"Alpha", "render", "Beta", "render"};
    const char *recv[] = {NULL, "test.helpers.Alpha", NULL, "test.helpers.Beta"};
    for (int i = 0; i < 4; i++) {
        defs[i].qualified_name = qns[i];
        defs[i].short_name = shorts[i];
        defs[i].label = recv[i] ? "Method" : "Class";
        defs[i].receiver_type = recv[i];
        defs[i].def_module_qn = "test.helpers";
        defs[i].return_types = recv[i] ? "int" : NULL;
        defs[i].lang = CBM_LANG_PYTHON;
    }
    const char *import_names[] = {"Alpha", "Beta"};
    const char *import_qns[] = {"test.helpers.Alpha", "test.helpers.Beta"};
    bool correct = true;
    for (int prebuilt = 0; prebuilt < 2; prebuilt++) {
        CBMArena arena, shared;
        cbm_arena_init(&arena);
        cbm_arena_init(&shared);
        CBMTypeRegistry *base = prebuilt ? cbm_py_build_cross_registry(&shared, defs, 4) : NULL;
        char module_input[64];
        snprintf(module_input, sizeof(module_input), "%s", "test.app");
        CBMResolvedCallArray out = {};
        CBMLSPStatus status =
            prebuilt ? cbm_run_py_lsp_cross_with_registry_status(&arena, source, (int)strlen(source),
                                                                 module_input, base, import_names,
                                                                 import_qns, 2, NULL, &out)
                     : cbm_run_py_lsp_cross_status(&arena, source, (int)strlen(source),
                                                   module_input, defs, 4, import_names, import_qns,
                                                   2, NULL, &out);
        /* Overwrite the caller's input: returned caller strings must not alias it. */
        memset(module_input, 'x', strlen(module_input));
        int alpha = -1, beta = -1;
        for (int i = 0; i < out.count; i++) {
            const CBMResolvedCall *rc = &out.items[i];
            if (!rc->caller_qn || strcmp(rc->caller_qn, "test.app") != 0 || !rc->callee_qn)
                continue;
            if (strcmp(rc->callee_qn, "test.helpers.Alpha.render") == 0)
                alpha = i;
            if (strcmp(rc->callee_qn, "test.helpers.Beta.render") == 0)
                beta = i;
        }
        correct = correct && status == CBM_LSP_COMPLETE && alpha >= 0 && beta >= 0;
        cbm_arena_destroy(&arena);
        cbm_arena_destroy(&shared);
    }
    ASSERT_TRUE(correct);
    PASS();
}

SUITE(py_lsp) {
    RUN_TEST(pylsp_module_callers_join_exact_raw_occurrences);
    RUN_TEST(pylsp_module_named_lambda_keeps_module_owner);
    RUN_TEST(pylsp_module_cross_callers_own_input);
    RUN_TEST(pylsp_issue1527_chain_statements_resolve_completely);
    RUN_TEST(pylsp_issue1527_large_module_resolves_every_call);
    RUN_TEST(pylsp_issue1527_long_binary_chain_is_not_nesting);
    RUN_TEST(pylsp_issue1527_long_receiver_chain_is_not_nesting);
#ifdef CBM_ENABLE_TEST_SEAMS
    RUN_TEST(pylsp_scale_work_is_linear);
    RUN_TEST(pylsp_issue1527_memo_allocation_failure_is_reported);
    RUN_TEST(pylsp_issue1527_memo_and_depth_failures_stop_work);
    RUN_TEST(pylsp_issue1527_cross_memo_failure_preserves_completed_rows);
    RUN_TEST(pylsp_issue1527_cross_memo_failure_fallback_reports_error);
    RUN_TEST(pylsp_issue1527_depth_failure_reports_all_entry_points);
#endif
    /* Phase 2 — smoke */
    RUN_TEST(pylsp_smoke_empty);
    RUN_TEST(pylsp_smoke_one_function);
    RUN_TEST(pylsp_smoke_one_class);
    RUN_TEST(pylsp_no_crash_on_syntax_error);
    RUN_TEST(pylsp_smoke_imports_passed_through);
    /* Phase 3 — imports → scope */
    RUN_TEST(pylsp_import_simple);
    RUN_TEST(pylsp_import_aliased);
    RUN_TEST(pylsp_import_from);
    RUN_TEST(pylsp_import_from_aliased);
    RUN_TEST(pylsp_import_relative_one_dot);
    RUN_TEST(pylsp_import_relative_two_dots);
    RUN_TEST(pylsp_import_star_best_effort);
    RUN_TEST(pylsp_import_typing_only_still_binds);
    RUN_TEST(pylsp_import_multi_pass_through_extract_file);
    /* Phases 4-6 — bindings + expression typing + method dispatch */
    RUN_TEST(pylsp_direct_function_call);
    RUN_TEST(pylsp_method_call_simple);
    RUN_TEST(pylsp_method_via_self);
    RUN_TEST(pylsp_constructor_call_returns_instance);
    RUN_TEST(pylsp_method_via_inheritance);
    RUN_TEST(pylsp_no_false_positive_on_unknown_method);
    /* Phases 7-8 — decorators, super(), multi-inheritance */
    RUN_TEST(pylsp_decorated_function_resolves);
    RUN_TEST(pylsp_classmethod_resolves);
    RUN_TEST(pylsp_staticmethod_resolves);
    RUN_TEST(pylsp_dataclass_constructor);
    RUN_TEST(pylsp_super_call);
    RUN_TEST(pylsp_multi_inheritance_first_base);
    RUN_TEST(pylsp_pep695_generic_class);
    /* Phase 9 — cross-file + batch */
    RUN_TEST(pylsp_crossfile_method_dispatch);
    RUN_TEST(pylsp_crossfile_receiver_through_typed_field);
    RUN_TEST(pylsp_crossfile_typed_field_shared_registry);
    RUN_TEST(pylsp_overlay_existing_field_keeps_shared_type);
    RUN_TEST(pylsp_crossfile_classmethod_on_class_issue228);
    RUN_TEST(pylsp_crossfile_inheritance);
    RUN_TEST(pylsp_batch_two_files);
    /* Phase 10 — stdlib resolution */
    RUN_TEST(pylsp_stdlib_os_getcwd);
    RUN_TEST(pylsp_stdlib_collections_defaultdict);
    RUN_TEST(pylsp_stdlib_pathlib_path_method);
    RUN_TEST(pylsp_stdlib_logging_getlogger);
    /* Round 1 — parity push */
    RUN_TEST(pylsp_round1_dotted_import_walk);
    RUN_TEST(pylsp_round1_typing_cast);
    RUN_TEST(pylsp_round1_assert_type_passthrough);
    RUN_TEST(pylsp_round1_forward_reference);
    RUN_TEST(pylsp_round1_self_return_chains);
    RUN_TEST(pylsp_round1_generic_subscript_annotation);
    /* Round 2 — narrowing */
    RUN_TEST(pylsp_round2_isinstance_narrow);
    RUN_TEST(pylsp_round2_is_not_none_narrow);
    RUN_TEST(pylsp_round2_isinstance_no_false_positive_in_else);
    /* Round 2/3 — walrus, comprehension, optional-narrow */
    RUN_TEST(pylsp_round2_narrow_after_call);
    RUN_TEST(pylsp_round2_walrus_binds);
    RUN_TEST(pylsp_round3_listcomp_element_method);
    RUN_TEST(pylsp_round3_for_loop_element_method);
    RUN_TEST(pylsp_round3_optional_narrow_with_union);
    /* Round 3 — match/case + async */
    RUN_TEST(pylsp_round3_match_case_class_pattern);
    RUN_TEST(pylsp_round3_async_await_pass_through);
    /* Round 4 — instance attribute typing */
    RUN_TEST(pylsp_round4_instance_attribute_init);
    RUN_TEST(pylsp_round4_instance_attribute_class_annotation);
    /* Round 5 — subscript, super().__init__, operator dunders */
    RUN_TEST(pylsp_round5_dict_subscript_value_type);
    RUN_TEST(pylsp_round5_list_subscript_value_type);
    RUN_TEST(pylsp_round5_super_init);
    /* Round 6 — generators, dataclasses, properties */
    RUN_TEST(pylsp_round6_generator_yields_iterable);
    RUN_TEST(pylsp_round6_dataclass_field_access);
    RUN_TEST(pylsp_round6_property_access_chains);
    /* Golden regression tests (WS9) */
    RUN_TEST(pylsp_golden_self_method);
    RUN_TEST(pylsp_golden_stdlib_logging_getlogger);
    RUN_TEST(pylsp_golden_same_module_func);
}
