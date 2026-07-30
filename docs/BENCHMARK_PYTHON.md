# Python LSP — Phase 11 benchmark methodology

This document describes how to measure the Python LSP type resolver
(`internal/cbm/lsp/py_lsp.c`, Phases 0–10) against real Python code.
Numbers from a specific corpus run can be appended in a "Results"
section as new measurements land.

## Targets (locked in PYTHON_LSP_PLAN.md)

| Metric | Target |
|---|---|
| Per-file LSP overhead (absolute) | < 10 ms for a 500-line file |
| `resolved_calls.count / calls.count` (application code) | ≥ 40% |
| `resolved_calls.count / calls.count` (with stdlib resolution) | ≥ 70% |
| Manual spot-check false-positive rate (50 sampled resolutions) | < 5% |

## Accepted failure classes (declared ceiling)

The following resolution failures are considered correct UNKNOWN, not
benchmark regressions. Surface area outside this list is the measurable
gap to investigate:

1. Metaclass `__call__` — class instantiation through a custom metaclass.
2. `__getattr__` / `__getattribute__` — dynamic attribute resolution at
   runtime; py_lsp bails to UNKNOWN as documented in Phase 6.
3. `setattr(obj, name, value)` where `name` is not a literal string.
4. `importlib.import_module(<dynamic_arg>)` — import path computed at
   runtime.
5. Runtime monkey-patching of methods or classes after definition.
6. `eval` / `exec` of source strings.
7. Descriptors (`__get__`, `__set__`, `__delete__`) beyond `@property`,
   `@classmethod`, `@staticmethod`.
8. `__call__` on instances of classes whose `__call__` is dynamically
   bound.
9. `__init_subclass__` hooks that mutate the subclass after creation.
10. Dynamically generated classes via `type(name, bases, dict)`.

Anything outside this list that produces UNKNOWN is a known gap
candidate for Phase 11.x extensions or v1.1.

## How to measure

### Fast: in-process integration test

`tests/test_py_lsp_bench.cpp` runs `cbm_extract_file` on a representative
Python source string and reports `calls.count` /
`resolved_calls.count` plus extraction time. Run with:

```bash
scripts/test.sh
# Look for the "py_lsp_bench" suite output
```

### Full: against a Python-heavy repository

```bash
# 1. Build a fresh production binary from this worktree.
scripts/build.sh

# 2. Run benchmark-index against a Python-heavy repo. Suggested
#    candidates (Python file counts as of 2026-05-09):
#      ~/project_dir/datadice/falkemedia (232 .py files)
#      ~/project_dir/datadice/api-hub    (188 .py files)
mkdir -p /tmp/py-bench-results
scripts/benchmark-index.sh \
  ./build/c/code-cortex-mcp \
  python \
  ~/project_dir/datadice/falkemedia \
  /tmp/py-bench-results

# 3. Compute the resolution ratio. The benchmark dumps node/edge counts
#    plus per-file timings; cross-reference with calls / resolved_calls
#    via the MCP graph queries:
code-cortex-mcp query \
  "MATCH ()-[r:CALLS]->() WHERE r.strategy STARTS WITH 'lsp_' \
   RETURN count(r) AS lsp_resolved"

code-cortex-mcp query \
  "MATCH ()-[r:CALLS]->() RETURN count(r) AS total_resolved"
```

### Manual spot-check

Sample 50 resolved-call edges (random offset in the CALLS edge set)
and verify by hand:

- Open the source file at `caller_qn`'s line.
- Locate the call expression that produced the edge.
- Confirm the `callee_qn` matches what a human reader would identify.

A false positive count of 0–2 out of 50 (0–4%) is within the < 5%
target. 3+ false positives on the sample → triage and document
contributing patterns; usually they cluster on a specific dynamic
construct (e.g. `getattr`-style dispatch).

## Results

### In-process benchmark (test_py_lsp_bench.c)

| Date | Commit | Fixture LOC | Calls | Resolved | Ratio | Time (ASan+UBSan) |
|---|---|---:|---:|---:|---:|---:|
| 2026-05-08 | a8c9ace (Phase 11 baseline) | 65 | 16 | 13 | 81% | 2.45 ms |
| 2026-05-09 | 2e042da (Round 1: dotted imports, cast, Self, fwd-refs) | 65 | 16 | 13 | 81% | 3.11 ms |
| 2026-05-09 | fa45e7d (Round 2: narrowing, walrus, comprehension typing) | 65 | 16 | 13 | 81% | 2.74 ms |
| 2026-05-09 | e7dcadc (Round 3: match/case, await, expanded fixture to 136 LOC) | 136 | 38 | 30 | 79% | 7.50 ms |
| 2026-05-09 | 4b99389 (Round 4: instance attribute typing) | 136 | 38 | 36 | 95% | 8.06 ms |
| 2026-05-09 | ce54e81 (Round 5: subscript, super().__init__, operators) | 136 | 38 | 36 | 95% | — |
| 2026-05-09 | fd81dff (Round 6: decorator flags, generator/dataclass/property tests) | 136 | 38 | 36 | 95% | — |
| 2026-05-09 | dd3a816 (Round 7: builtin/template attr dispatch, wrappers, dedup) | 178 | 52 | 51 | 98% | 11.79 ms |
| 2026-05-09 | f35745c (Round 8: with-as, except-as, tuple unpack, dict.items, slice) | 178 | 52 | 52 | **100%** | 11.59 ms |
| 2026-05-09 | 4ccd08c (Round 9: TypedDict, early-return narrowing, match seq pattern) | 178 | 52 | 52 | **100%** | 11.02 ms |
| 2026-05-09 | 39f41c3 (Round 10: lambda, dict-dispatch, async, callable, next, kwargs + 21 new stress tests) | 178 | 52 | 52 | **100%** | 11.25 ms |

The 65→136 fixture jump is more honest: the small fixture was
hand-shaped to match what the resolver did well at the time. The
136-line fixture covers the full parity surface — narrowing
(isinstance / `is not None` / walrus), generic containers and
comprehensions, fluent `Self` chains, `super()`, async/await,
match/case dispatch, classmethod / staticmethod, common stdlib calls,
and instance attributes via both `self.x = expr` in `__init__` and
class-body `x: T` annotations.

The remaining 5% gap (2/38 unresolved calls) hits cases that genuinely
need a constraint solver — see "Stopping Point" below.

## Stopping Point — what we deliberately did not build

Per the user directive "stop if any addition would realistically mean
that we need to kinda rebuild a compiler or whatever, but otherwise
pursue parity," the following items are out of scope for v1 and would
require building infrastructure equivalent to a full Python type
checker:

1. **Constraint solver / bidirectional inference.** Pyright's
   `constraintSolver.ts` is ~3,000 lines of bidirectional type
   inference. Required for: full overload resolution by argument type,
   inferring callable types from context, ParamSpec resolution.
2. **Code-flow type narrowing across basic blocks.** Pyright's
   `codeFlowEngine.ts` tracks types through every basic block,
   handling early returns, exception flow, generator flow.
   Equivalent to building an SSA-style analysis for Python.
3. **`ParamSpec` / `TypeVarTuple` / `Concatenate` resolution.** These
   PEP 612 / 646 / 696 features are how decorator-rewrapping and
   variadic generics work properly. Without them, `@functools.wraps`
   collapses to identity — fine for our call resolution but not
   parity with type checkers.
4. **Custom metaclasses' `__call__`.** A class with a custom
   metaclass can short-circuit `Class()` to call something else
   entirely. Pyright handles this by recognizing common metaclass
   patterns; full support means tracking metaclass MRO.
5. **Descriptor protocol beyond `@property` / `@classmethod` /
   `@staticmethod`.** Custom descriptors via `__get__` / `__set__` /
   `__delete__` change attribute semantics arbitrarily. Common in
   ORMs (SQLAlchemy column objects), Pydantic, attrs.
6. **Plugin protocols.** SQLAlchemy declarative classes, Pydantic
   models, attrs / dataclasses-style libraries — Pyright ships
   special-case "plugins" for each. Each is hundreds of lines.
7. **Recursive types and self-referential generics.** `tree:
   Node[Node[Node[...]]]` resolution requires lazy type evaluation.
8. **Runtime monkey-patching, `eval`/`exec`, `setattr` with
   non-literal name.** No static analyzer handles these well; they're
   fundamentally dynamic.
9. **Per-symbol typeshed version guards in the stdlib generator.**
   The plan called for `min_python_version` / `max_python_version`
   tracking. v1 flattens version branches (union of all). Adding
   per-version filtering requires propagating the project's target
   Python version through the resolver — non-trivial threading
   change.

Items 1, 2, 3, 5, 7 are direct compiler-rebuild territory. Items 4,
6 need significant per-pattern engineering. Item 8 is impossible
statically. Item 9 is achievable but a separate v1.1 task.

## Stress-test surface (test_py_lsp_stress.c)

**43 advanced patterns probed individually. All hard-asserted as PASS. Zero remaining KNOWN GAPs.**

**Hard-asserted (PASS clean)**:
- NamedTuple class form
- TypedDict literal-key subscript
- Protocol structural method dispatch
- ABC + @abstractmethod
- Context manager `with X() as f:` via `__enter__`
- Exception handler `except E as e:`
- Post-early-return narrowing (`if not isinstance: return`)
- Tuple unpacking from function return
- Comprehension over `dict.items()` with k, v unpacking
- List slice returns list of same element type
- Decorator factory with closures
- Property setter
- Self-type in inheritance chain (Builder pattern)
- Diamond inheritance (C3 MRO)
- Recursive self-referencing types
- Nested closure capturing outer variable
- Long method chain via Self
- Generator delegation (`yield from`)
- Async-generator iteration

**Round 10 additions (21 new patterns, all PASS)**:
- SQLAlchemy 2.0-style Mapped[T] field
- Pydantic-style BaseModel field
- chained filter / map / comprehension
- nested function call with annotated returns
- Optional chain with walrus + None narrow
- classmethod chained to instance method via Self
- multi-assignment from explicit tuple
- dict.get with default
- @property attribute chain to method
- async with + async for via __aenter__ / __anext__
- recursive self-referencing method
- Callable[[], R] return-value evaluation
- TypedDict total=False
- nested match patterns with multiple class arms
- Protocol via NAMED class
- next(iter) returning element type
- **kwargs: T -> dict[str, T] annotation
- yield from in chained generator
- isinstance | branches
- module-level constants
- dataclass with default_factory

**Round 10 gap closures**: lambda call-site inference, function-as-
dict-value dispatch, match sequence pattern element typing — all now
hard-asserted PASS.

## Achievable next steps (not undertaken in this iteration)

These would push past 95% on the bench fixture but stay below the
"compiler rebuild" line:

- **Decorator return-type substitution for the simple case.** When a
  decorator `D` is registered with return annotation `Callable[..., R]`
  or just a function-typed return, the decorated function's effective
  return is the decorator's return. Doesn't need ParamSpec for the
  identity / wrapper pattern.
- **Overload selection by literal argument.** Many stdlib functions
  have overloads keyed on literal flags (`subprocess.run(..., check=True)`,
  `re.compile(..., flags=...)`, `json.loads`). The current generator
  collapses overloads; selection at call site by matching literal arg
  values would resolve the right overload.
- **`super().__init__(...)` constructor resolution.** Currently
  `super()` in __init__ may not resolve to the parent's __init__
  because __init__ isn't typically registered with a return type
  worth chaining. A small special-case would emit the constructor
  edge.
- **Dict subscript value typing.** `self.cache[key]` should type as
  the dict's value type. Currently UNKNOWN. Easy to add given
  TEMPLATE("dict", [K, V]) is already extracted from annotations.
- **`@property` getter return type.** Currently we return the method's
  return type when accessing a method as an attribute (which happens
  to be right for properties and rarely wrong for regular methods,
  per the discussion in the Round 1 commit). A real fix tracks
  decorator info on registered methods.

These are within reach without compiler-level infrastructure and
would push the bench from 95% toward ~98%.
