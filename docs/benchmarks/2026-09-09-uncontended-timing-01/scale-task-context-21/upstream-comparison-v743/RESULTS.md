# V743 timed-run results

The runner completed 64/64 rows. Exact answer grading marked 62/64 rows correct.

## Arm totals

| Arm | Terminal | Correct | Incorrect | Terminal seconds | Correct-only seconds |
| --- | ---: | ---: | ---: | ---: | ---: |
| candidate-mcp | 24/24 | 22 | 2 | 238.483973 | 213.805663 |
| shell | 24/24 | 24 | 0 | 555.469152 | 555.469152 |
| upstream-mcp | 16/16 | 16 | 0 | 253.506759 | 253.506759 |

## candidate to shell

| Group | Eligible pairs | Left seconds | Right seconds | Geometric ratio | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| all | 22/24 | 213.805663 | 505.602988 | 0.421279 | 57.872% |
| scale:100m | 8/8 | 71.065784 | 179.398005 | 0.406477 | 59.352% |
| scale:10k | 8/8 | 76.734084 | 186.212898 | 0.422987 | 57.701% |
| scale:1m | 6/8 | 66.005794 | 139.992085 | 0.439481 | 56.052% |
| archetype:multi-step | 10/12 | 85.441982 | 331.829570 | 0.242678 | 75.732% |
| archetype:one-shot | 12/12 | 128.363680 | 173.773418 | 0.667093 | 33.291% |

## upstream to shell

| Group | Eligible pairs | Left seconds | Right seconds | Geometric ratio | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| all | 16/16 | 253.506759 | 376.071147 | 0.711637 | 28.836% |
| scale:10k | 8/8 | 115.629498 | 186.212898 | 0.680559 | 31.944% |
| scale:1m | 8/8 | 137.877261 | 189.858249 | 0.744135 | 25.586% |
| archetype:multi-step | 8/8 | 136.931324 | 258.589200 | 0.517877 | 48.212% |
| archetype:one-shot | 8/8 | 116.575435 | 117.481947 | 0.977892 | 2.211% |

## candidate to upstream

| Group | Eligible pairs | Left seconds | Right seconds | Geometric ratio | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| all | 14/16 | 142.739878 | 207.003552 | 0.625604 | 37.440% |
| scale:10k | 8/8 | 76.734084 | 115.629498 | 0.621529 | 37.847% |
| scale:1m | 6/8 | 66.005794 | 91.374055 | 0.631080 | 36.892% |
| archetype:multi-step | 6/8 | 56.562571 | 90.428118 | 0.574302 | 42.570% |
| archetype:one-shot | 8/8 | 86.177307 | 116.575435 | 0.667066 | 33.293% |

## Correctness exceptions

The source-oracle and retrieval checks passed for both rows below. The model output broke the exact-answer contract by adding suffix text to symbols.

- `1m-chain-1-candidate-mcp`: completed in 6.696298 s, but emitted `capacity_function_3279bbe3bdc5e; capacity_function_3278bbe3bdc5eλεύ; capacity_function_3277bbe3bdc5e; capacity_function_3276bbe3bdc5e` instead of the exact symbol list.
- `1m-chain-3-candidate-mcp`: completed in 17.982012 s, but emitted `capacity_function_17317(crash); capacity_function_17316; capacity_function_17315/not existing?; capacity_function_17314` instead of the exact symbol list.

## Gates

All 64 admissions passed. The maximum one-minute load was 1.728027; no foreign process reached 50% CPU. The runtime source oracle passed. The final immutable cache check passed. Upstream 100m has no timing estimate after 4 GiB and 32 GiB setup failures.

## Curated verification records

Version v02 adds summary, grade, retrieval-receipt, and tool-audit records for all 64 rows. The records keep each original run directory name under `run/<row>/`.
