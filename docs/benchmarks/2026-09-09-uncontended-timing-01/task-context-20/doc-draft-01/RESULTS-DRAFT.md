The trial completed 15 of 48 scheduled tasks. All 15 answers passed the exact source checks and manual protocol audit. The other 33 rows failed their setup smoke because the CLI reported a usage limit; their task sessions never started. These are setup failures, not incorrect task answers. Their token usage and costs remain unknown.

Each contrast contains five matched task/model/repetition pairs. The table reports geometric wall-time ratios and exact two-sided sign-test p-values. A ratio below one indicates less observed wall time in the numerator. All executions were contended: **zero pairs met timing-admission requirements**, so these comparisons are descriptive.

| Contrast | Pairs | Wall-time ratio | Sign p against 1.0 | Sign p against 0.9 |
|---|---:|---:|---:|---:|
| Shell-prefetch / raw-shell | 5 | 0.825105 | 1.000 | 1.000 |
| Adaptive-context / raw-shell | 5 | 0.739033 | 0.375 | 0.375 |
| Adaptive-context / shell-prefetch | 5 | 0.895684 | 1.000 | 1.000 |

The completed data comprise the first five blocks of the frozen shuffled schedule: two Astra blocks and three Sol blocks, covering three lookup pairs and two multihop pairs. They contain only three distinct targets—Jansson `json_loadb`, the Jansson object-clear call chain, and OpenCV `getBuildInformation`. No crosslanguage task ran. Repeated targets and models also limit independence.

Both lookup prefetch arms used identical source contexts. The adaptive call-chain query found no indexed path and used shell recovery. These results therefore do not establish a graph-specific speed benefit or a general 10% improvement. Task wall includes retrieval and the timed session lifecycle; initial indexing, setup smokes, and subsequent source/grade checks are separate.

The [frozen analyzer results](../analysis-02/report.json) agree exactly with the [independent calculation](../independent-analysis-01/report.json) on paired ratios, sign-test values, and cost totals. All failed setup attempts remain in the evidence.
