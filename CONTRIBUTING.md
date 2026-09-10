# Contributing to code-cortex-mcp

Contributions are welcome. This guide covers setup, testing, and pull request guidelines.

First-party source and tests use C++23. Vendored tree-sitter parsers and third-party libraries
retain their upstream C sources.

## Build from Source

**Prerequisites**: CMake, a C++23 compiler, make, zlib, and Git.

```bash
git clone https://github.com/tigercosmos/code-cortex-mcp.git
cd code-cortex-mcp
git config core.hooksPath scripts/hooks  # activates pre-commit security checks
scripts/build.sh
```

macOS: `xcode-select --install` provides Apple Clang.
Linux: install `build-essential cmake zlib1g-dev` on Debian or Ubuntu. Install
`gcc-c++ cmake zlib-devel` on Fedora.

The binary is output to `build/c/code-cortex-mcp`.

## Run Tests

```bash
scripts/test.sh
```

This builds with AddressSanitizer and UndefinedBehaviorSanitizer. It then runs the full test
suite. Key test files:

- `tests/test_pipeline.cpp` — pipeline integration tests
- `tests/test_integration.cpp` — end-to-end indexing and MCP tests
- `tests/test_mcp.cpp` — MCP protocol and tool handler tests
- `tests/test_store_*.cpp` — SQLite graph store tests

## Run Linter

```bash
scripts/lint.sh
```

Runs clang-tidy, cppcheck, and clang-format. All must pass before committing (also enforced by pre-commit hook).

## Run Security Audit

```bash
scripts/security.sh
```

Runs the security layers: static allow-list audit, binary string scan, install audit, network egress test, MCP robustness (fuzz), and vendored dependency integrity.

## Project Structure

```
src/
  foundation/       Arena allocator, hash table, string utils, platform compat
  store/            SQLite graph storage (WAL mode, FTS5)
  cypher/           Cypher query → SQL translation
  mcp/              MCP server (JSON-RPC 2.0 over stdio, 15 tools)
  pipeline/         Multi-pass indexing pipeline
    pass_*.cpp      Individual pipeline passes (definitions, calls, usages, routes)
    result_store.*  Optional temporary storage for full extraction results
  discover/         File discovery with gitignore support
  watcher/          Git-based background auto-sync
  cli/              CLI subcommands (install, update, uninstall, config)
internal/cbm/       Tree-sitter AST extraction (155 languages, vendored C grammars)
vendored/           sqlite3, yyjson, mimalloc, xxhash
scripts/            Build, test, lint, security audit scripts
tests/              C++ unit, integration, and regression tests
```

## Adding or Fixing Language Support

Language support is split between two layers:

1. **Tree-sitter extraction** (`internal/cbm/`): grammar loading, syntax-node configuration
   in `lang_specs.cpp`, and extraction in `extract_*.cpp`.
2. **Pipeline passes** (`src/pipeline/`): Call resolution, usage tracking, HTTP route linking

**Workflow for language fixes:**

1. Check the language spec in `internal/cbm/lang_specs.cpp`.
2. Add an extraction regression test in `tests/test_extraction.cpp`.
3. Add an integration test in `tests/test_pipeline.cpp` when the change affects graph edges.
4. Test the change on a real open-source repository.

### Infrastructure Languages (Infra-Pass Pattern)

Languages like **Dockerfile**, **docker-compose**, **Kubernetes manifests**, and **Kustomize** do not require a new tree-sitter grammar. Instead they follow an *infra-pass* pattern, reusing the existing tree-sitter YAML grammar where applicable:

1. **Detection helpers** in `src/pipeline/pass_infrascan.cpp` identify files by name and
   content. Examples include Dockerfiles, Kubernetes manifests, and Kustomize files.
2. **Custom extractors** in `internal/cbm/extract_k8s.cpp` walk the YAML syntax tree. They
   populate `CBMFileResult` with imports and definitions.
3. **Pipeline passes** in `pass_k8s.cpp` and `pass_infrascan.cpp` emit graph nodes and edges.
   Kubernetes manifests emit `Resource` nodes. Kustomize files emit `Module` nodes.

**When adding a new infrastructure language:**
- Add a detection helper in `pass_infrascan.cpp` or a new `pass_<lang>.cpp` file.
- Add the `CBM_LANG_<LANG>` value in `internal/cbm/cbm.h` and a row in `lang_specs.cpp`.
- Write a custom extractor that returns `CBMFileResult*` — do not add a tree-sitter grammar.
- Register the pass in `pipeline.cpp`.
- Add tests in `tests/test_pipeline.cpp` following the `TEST(infra_is_dockerfile)` and `TEST(k8s_extract_manifest)` patterns.

## Commit Format

Use conventional commits: `type(scope): description`

| Type | When to use |
|------|-------------|
| `feat` | New feature or capability |
| `fix` | Bug fix |
| `test` | Adding or updating tests |
| `refactor` | Code change that neither fixes a bug nor adds a feature |
| `perf` | Performance improvement |
| `docs` | Documentation only |
| `chore` | Build scripts, CI, dependency updates |

Examples: `fix(store): set busy_timeout before WAL`, `feat(cli): add --progress flag`

## Pull Request Guidelines

### Before You Write Code

- **Open an issue first — always.** Every PR must reference a tracking issue (`Fixes #N` or `Closes #N`). Describe what you want to change and why. Wait for maintainer feedback before implementing. PRs without a prior issue discussion will be closed.
- **Bug fixes and test additions** are the exception — these are welcome without prior discussion, as long as they're focused.

### What Requires Explicit Maintainer Approval

The following changes will not be merged without prior design discussion in an issue:

- **API surface changes** — adding, removing, renaming, or changing defaults of MCP tools
- **New pipeline passes or indexing algorithms** — anything that changes what gets extracted or how
- **Build system / Makefile changes** — beyond trivial fixes
- **Project configuration** — CLAUDE.md, skill files, .mcp.json, CI workflows
- **New dependencies** — vendored or otherwise
- **Breaking changes** of any kind

If in doubt, open an issue and ask.

### PR Scope and Size

- **One issue per PR.** Each PR must address exactly one bug, one feature, or one refactor. Do not bundle multiple fixes or feature additions into a single PR. Kitchen-sink PRs will be closed with a request to split.
- **Keep PRs small.** A good PR is under 500 lines. If your change is larger, split it into reviewable increments that each stand on their own.
- **Don't mix features with fixes.** If you find a bug while implementing a feature, submit the bug fix as a separate PR.

### Code Requirements

- Use C++23 for first-party source and tests.
- Include tests for new functionality
- Run `scripts/test.sh` and `scripts/lint.sh` before submitting
- Keep PRs focused — avoid unrelated reformatting or refactoring

## Security

We take security seriously. All PRs go through:
- Manual security review (dangerous calls, network access, file writes, prompt injection)
- Automated 8-layer security audit in CI
- Vendored dependency integrity checks

If you add a new `system()`, `popen()`, `fork()`, or network call, it must be justified and added to `scripts/security-allowlist.txt`.

## Good First Issues

Check [issues labeled `good first issue`](https://github.com/tigercosmos/code-cortex-mcp/labels/good%20first%20issue) for beginner-friendly tasks with clear scope and guidance.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
