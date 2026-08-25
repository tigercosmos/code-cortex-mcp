# Indexing benchmark, 2026-08-25

Full-index time of code-cortex-mcp against codebase-memory-mcp on 13 open-source
repositories, on one macOS machine and one Linux machine.

## Test conditions

| Item | macOS | Linux |
|---|---|---|
| Machine | Apple M3 Max, 14 cores, 36 GB | 2-socket x86-64, 32 cores, 62 GB, Ubuntu 24.04 |
| Compiler | Apple clang (`scripts/build.sh` default) | gcc 13.3 (`scripts/build.sh CC=gcc CXX=g++`) |
| Workers | 14 (auto) | 32 (auto) |

- **Versions** — code-cortex-mcp at `af4579de` plus the crash fix in this change
  (`internal/cbm/lsp/c_lsp.cpp`, `internal/cbm/preprocessor.cpp`);
  codebase-memory-mcp at `010569fa` (its `main` on the test date).
- **Build** — each project's own `scripts/build.sh`, release flags, no web UI.
- **Procedure** — `cli index_repository` in `full` mode from an empty `CBM_CACHE_DIR`,
  three runs per repository per engine, wall clock measured around the CLI process.
  The tables report the median. `scripts/benchmark-index-compare.sh` is the runner.
- **Repositories** — shallow clones at the commits in [`repos.csv`](repos.csv), identical
  on both machines. "Lines" counts every line of every tracked file.
- **Raw data** — [`index-macos.csv`](index-macos.csv), [`index-linux.csv`](index-linux.csv):
  one row per run with engine, repository, seconds, node count, edge count, and exit code.
  Rows tagged `ccm-fixed` come from the binary with the crash fix; the `ccm` rows for
  `llvm` are the crashing runs of the unfixed binary.

## Results, macOS

| Repository | Language | Files | Lines | code-cortex-mcp | codebase-memory-mcp | Ratio | code-cortex-mcp nodes / edges | codebase-memory-mcp nodes / edges |
|---|---|---:|---:|---:|---:|---:|---|---|
| Redis | C | 1,859 | 0.6M | **2.80 s** | 9.59 s | 3.4× | 38,438 / 146,017 | 38,658 / 135,843 |
| etcd | Go | 1,499 | 0.3M | **1.42 s** | 8.11 s | 5.7× | 14,836 / 94,801 | 15,475 / 108,174 |
| Django | Python | 7,085 | 1.1M | **4.79 s** | 13.30 s | 2.8× | 55,458 / 371,515 | 55,461 / 344,204 |
| RocksDB | C++ | 2,332 | 1.1M | **7.49 s** | 16.45 s | 2.2× | 67,710 / 349,392 | 68,005 / 333,179 |
| Neovim | C, Lua | 3,900 | 1.7M | **3.51 s** | 12.08 s | 3.4× | 39,057 / 178,207 | 39,345 / 204,296 |
| CPython | C, Python | 6,142 | 3.3M | **14.35 s** | 47.58 s | 3.3× | 136,575 / 1,002,469 | 137,282 / 956,504 |
| Rails | Ruby | 4,991 | 0.8M | **5.48 s** | 10.47 s | 1.9× | 100,649 / 364,854 | 64,354 / 302,076 |
| TypeScript | TypeScript | 65,905 | 6.4M | **19.06 s** | 42.43 s | 2.2× | 237,390 / 743,110 | 244,284 / 839,757 |
| Elasticsearch | Java | 47,191 | 8.8M | **94.0 s** | 112.8 s | 1.2× | 691,851 / 5,202,376 | 700,771 / 5,703,523 |
| Kubernetes | Go | 31,300 | 7.3M | **56.5 s** | 62.8 s | 1.1× | 287,974 / 3,311,322 | 293,008 / 2,002,558 |
| PyTorch | C++, Python | 21,653 | 5.1M | **31.9 s** | 77.8 s | 2.4× | 234,518 / 2,111,084 | 275,520 / 2,044,199 |
| llvm-project | C++ | 182,611 | 46.8M | **487 s** | crash | — | 2,209,544 / 7,811,919 | — |
| Linux kernel | C | 95,862 | 43.8M | **368 s** | stopped | — | 4,728,379 / 11,549,862 | — |

## Results, Linux

| Repository | Language | Files | Lines | code-cortex-mcp | codebase-memory-mcp | Ratio | code-cortex-mcp nodes / edges | codebase-memory-mcp nodes / edges |
|---|---|---:|---:|---:|---:|---:|---|---|
| Redis | C | 1,859 | 0.6M | **2.77 s** | 6.00 s | 2.2× | 38,438 / 146,023 | 38,658 / 135,901 |
| etcd | Go | 1,499 | 0.3M | **1.15 s** | 5.08 s | 4.4× | 14,831 / 94,438 | 15,470 / 107,049 |
| Django | Python | 7,085 | 1.1M | **3.76 s** | 8.95 s | 2.4× | 55,458 / 371,621 | 55,461 / 344,342 |
| RocksDB | C++ | 2,332 | 1.1M | **5.06 s** | 10.55 s | 2.1× | 67,710 / 349,213 | 68,005 / 333,105 |
| Neovim | C, Lua | 3,900 | 1.7M | **2.56 s** | 7.76 s | 3.0× | 39,057 / 178,259 | 39,348 / 204,575 |
| CPython | C, Python | 6,142 | 3.3M | **11.87 s** | 28.19 s | 2.4× | 136,575 / 1,001,782 | 137,283 / 942,694 |
| Rails | Ruby | 4,991 | 0.8M | **4.35 s** | 7.04 s | 1.6× | 100,649 / 365,268 | 64,315 / 302,318 |
| TypeScript | TypeScript | 65,905 | 6.4M | **13.16 s** | 43.31 s | 3.3× | 237,390 / 742,838 | 244,284 / 848,479 |
| Elasticsearch | Java | 47,191 | 8.8M | **63.1 s** | 116.2 s | 1.8× | 691,853 / 5,192,942 | 700,748 / 5,707,402 |
| Kubernetes | Go | 31,300 | 7.3M | **57.1 s** | 65.3 s | 1.1× | 287,951 / 3,322,692 | 293,011 / 2,013,825 |
| PyTorch | C++, Python | 21,653 | 5.1M | **25.8 s** | 70.3 s | 2.7× | 234,519 / 2,110,031 | 275,523 / 2,087,257 |
| llvm-project | C++ | 182,611 | 46.8M | **382 s** | crash | — | 2,209,544 / 7,810,938 | — |
| Linux kernel | C | 95,862 | 43.8M | **264 s** | out of memory | — | 4,728,503 / 11,556,269 | — |

Across the 11 repositories that both engines complete, code-cortex-mcp is 1.1× to 5.7×
faster on macOS and 1.1× to 4.4× faster on Linux. The median is 2.4× on both machines. The
gap is widest on small and medium repositories. It narrows on the largest Java and Go graphs
(Elasticsearch, Kubernetes).

## Graph sizes

Both engines build graphs of nearly the same node count on every repository except Rails,
where code-cortex-mcp emits 100,649 nodes against 64,354. Edge counts differ more:
code-cortex-mcp emits 65% more edges on Kubernetes (3.3M against 2.0M) and 9% fewer on
Elasticsearch (5.2M against 5.7M). Rows with a large size difference measure different
amounts of work, so read their ratios with that in mind.

Node and edge counts also drift by a few units between runs of the same engine on the same
tree. For example, etcd on Linux gives 14,831 / 14,836 / 14,831 nodes. Both engines show
this drift. It contradicts the byte-identical-graph claim in earlier README revisions. The
cause is not yet identified.

## Failures

### llvm-project: crash in both engines, fixed in code-cortex-mcp

Before this change, both engines crashed with SIGSEGV in the index worker on llvm-project,
on both machines. code-cortex-mcp's supervisor quarantined eight files and then gave up;
codebase-memory-mcp has no retry and stopped at the first crash. The crash reports on macOS
show two faults:

1. `c_resolve_name_to_type` reads a `CBMType` at address `0x9`. Structured-binding
   decomposition (`auto [a, b, c] = s;`) indexed a registered type's NULL-terminated
   `field_types` list by binding position without a bound. When the binding has more names
   than the struct has fields, the read runs past the terminator into the next arena
   allocation. That allocation is the `CBMType` of the last field. Its leading `kind` word
   (9 = BUILTIN) becomes the bound type pointer, and the next lookup dereferences it.
   codebase-memory-mcp's crash report shows the same frame and address. Fixed in
   `internal/cbm/lsp/c_lsp.cpp` (`c_field_type_at`); regression test
   `clsp_nocrash_structured_binding_excess_names`, which UBSan flags as a misaligned
   access at `0x9` without the fix.
2. `simplecpp::TokenList::TokenList` calls `outputList->emplace_back()` with a NULL list
   when it cannot open an `#include`d header. `cbm_preprocess_with_map` passed no output
   list. Fixed in `internal/cbm/preprocessor.cpp` by passing a list.

With the fix, code-cortex-mcp indexes llvm-project in 382 s on Linux and 487 s on macOS,
with three consistent runs each.

### Linux kernel: codebase-memory-mcp runs out of memory

On Linux, codebase-memory-mcp's index worker died with SIGKILL in each of three runs after
122–173 s. Its resident set grew from 23 GB to 57.5 GB in 90 s, until the machine had 1 GB
free. During that time, 32 workers parsed the AMD GPU register headers in
`drivers/gpu/drm/amd/include/asic_reg/`, single files of 15–23 MB. On macOS, we stopped the
run by hand after 10 minutes with 19 GB of swap in use.

code-cortex-mcp completes the kernel. Its peak resident set is also large, and it does not
depend on the worker count:

| Engine | Workers | Peak RSS | Wall clock | Result |
|---|---:|---:|---:|---|
| code-cortex-mcp | 32 | 61.2 GB | 4:23 | 4,728,503 nodes |
| code-cortex-mcp | 16 | 61.2 GB | 5:17 | 4,728,503 nodes |
| code-cortex-mcp | 8 | 60.8 GB | 6:55 | 4,728,503 nodes |
| codebase-memory-mcp | 32 | > 57 GB | killed at ~2:20 | SIGKILL |
| codebase-memory-mcp | 8 | > 60 GB (machine unresponsive, load 107) | killed at 15:16 | SIGKILL |

The kernel graph is 2.2× larger than the 2.1M-node figure in earlier README revisions,
because header files are now `File` nodes with resolved `#include` edges. The 36 GB macOS
machine indexes it with 20 GB of swap in use. That is why its wall clock (368 s) is longer
than the earlier "3 minutes" figure. The memory footprint, about 13 KB per node at peak, is
the next optimization target.
