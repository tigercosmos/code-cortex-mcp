# V743 curated benchmark evidence v02

This bundle contains the frozen v743 protocol and small verification receipts for the completed serial tri-arm experiment.

The bundle excludes runtime caches, model homes, raw rollout archives, prompts, job and status logs, and per-row bulk logs. Full preserved evidence remains outside this directory.

## Contents

- `frozen/` contains active v743 protocol files, harness files, and the sealed protocol hash manifest.
- `setup/` contains the READY, scope, cache-seal, source, and final lifecycle receipts.
- `run/` contains the run launch and scope receipts, row results, run report, analyzer report, source, cache, and lifecycle receipts.
- `run/incorrect-rows/` retains the two incorrect response and grading records from v01.
- `run/<row>/` contains exact relative row paths for all 64 summaries, grades, retrieval receipts, and tool-audit receipts.
- `ROW-RECEIPT-INVENTORY.json` maps each curated row receipt to its preserved source hash.
- `upstream-100m/` contains the preserved 4 GiB and 32 GiB upstream index failure receipts.
- `METRICS.json` contains exact counts, paired sums, geometric ratios, and eligibility data.

## Scope

The run used 64 serial rows: 24 candidate MCP, 24 shell, and 16 upstream MCP. Upstream 100m stays unavailable after the sealed 4 GiB and 32 GiB index failures. The comparison has no upstream 100m timing estimate.

Use `sha256sum -c SHA256SUMS` from this directory to check every curated file. Use `sha256sum -c frozen/ACTIVE-FILES.sha256` from `frozen/` to check the selected active protocol and harness files.
