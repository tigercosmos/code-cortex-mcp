# Reproduce the complete-task comparison

This how-to runs the Code Cortex, shell, and upstream comparison from a clean host.
It also verifies the published evidence without rerunning paid model sessions.

The checked-in [configuration](config.json) contains the complete experiment configuration.
It pins source revisions, fixture inputs, harness files, tools, limits, and analysis gates.
The [runner](reproduce.py) implements the five controlled stages.
The [tests](test_reproduce.py) check the frozen schedule, seals, and analysis gates.

## Understand the comparison

The frozen schedule contains 64 serial rows and 24 distinct tasks.
Each scale has four symbol lookups and four three-hop call-chain tasks.

| Arm | 10K lines | 1M lines | 100M lines | Total rows |
|---|---:|---:|---:|---:|
| Code Cortex MCP | 8 | 8 | 8 | 24 |
| Shell tools | 8 | 8 | 8 | 24 |
| Upstream MCP | 8 | 8 | 0 | 16 |

Upstream has no 100M rows because its indexing failed under sealed 4 GiB and 32 GiB budgets.
The runner never estimates that missing result.

Setup and indexing are outside the timer.
An MCP row starts timing before MCP process startup.
Every timer stops at the terminal timestamp reported by `codexmon`.
Therefore, MCP timing includes startup, initialization, tool discovery, retrieval, and model work.

## Requirements

Use a dedicated x86-64 Linux host with a working systemd user session.
The published run used the following minimum admission requirements.

| Resource | Minimum |
|---|---:|
| Logical CPUs | 32 |
| Memory | 60 GiB |
| Free disk space | 100 GiB |

Use Ubuntu 24.04 amd64 for an exact published-binary reproduction.
Install Git, Python 3, GNU `time`, systemd tools, curl, Node.js, npm, and the native headers.

Add Kitware's Ubuntu repository for the pinned CMake package:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl git gpg liblz4-dev nodejs npm python3 time zlib1g-dev
curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
  | gpg --dearmor > /tmp/kitware-archive-keyring.gpg
sudo install -m 0644 /tmp/kitware-archive-keyring.gpg \
  /usr/share/keyrings/kitware-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' \
  | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
sudo apt-get update
sudo apt-get install -y \
  gcc-13-x86-64-linux-gnu=13.3.0-6ubuntu2~24.04.1 \
  g++-13-x86-64-linux-gnu=13.3.0-6ubuntu2~24.04.1 \
  cmake=4.3.4-0kitware3ubuntu24.04.1 \
  make=4.3-4.1build2
mkdir -p "$HOME/.local/bin"
ln -sfn /usr/bin/x86_64-linux-gnu-gcc-13 "$HOME/.local/bin/cc"
ln -sfn /usr/bin/x86_64-linux-gnu-g++-13 "$HOME/.local/bin/c++"
export PATH="$HOME/.local/bin:$PATH"
```

Add that `PATH` export to your shell profile before opening a new benchmark shell.
The exact package versions also appear in `config.json`.

The published binaries used these exact build-tool identities:

| Tool | Version text | Executable SHA-256 |
|---|---|---|
| C compiler | GCC 13.3.0 | `1b99826121ae6682a634e5efe09bd3e3df58ce58e0b28f849114ab5b89139c26` |
| C++ compiler | GCC 13.3.0 | `1353e9bdd29a7295c7226bf6c63abccce056d8cac31f112e5cdbecc3f28c2769` |
| CMake | 4.3.4 | `a52693f2d246cc136a2211fc1c812e9d66b86e8e22d5c18afbd9c513670a1783` |
| GNU Make | 4.3 | `d78b8f1d099fbcfb6f2f49ab87223b9b68fb3956642f92d6ec6de812e8afa965` |

The runner records and rechecks their resolved paths, modes, devices, inodes, versions, and hashes.
Use a derived configuration if your distribution produces different executable hashes.

Published binary hashes are reference receipts, not build gates.
The candidate binary embeds absolute source paths, so a different checkout path changes its hash.
The analysis report and results table show each new hash and its reference match status.

Install Codex CLI 0.152.1 for `x86_64-unknown-linux-musl`.
Authenticate it before the run by following the
[official Codex CLI guide](https://learn.chatgpt.com/docs/codex/cli).
For an npm installation, pin the historical package version:

```bash
npm install -g @openai/codex@0.152.1
codex_payload=$(find "$(npm root -g)" -type f \
  -path '*/@openai/codex-linux-x64/vendor/x86_64-unknown-linux-musl/bin/codex' \
  -print -quit)
test -n "$codex_payload"
printf '%s  %s\n' \
  b82018241214a4a7c6b97b198585192d1dbc3aab1ddcdc640f04d8dee8c606f9 \
  "$codex_payload" | sha256sum -c -
install -m 0755 "$codex_payload" "$HOME/.local/bin/codex"
```

The explicit install selects the pinned native payload instead of npm's JavaScript launcher.
The published native executable SHA-256 is:

```text
b82018241214a4a7c6b97b198585192d1dbc3aab1ddcdc640f04d8dee8c606f9
```

Download the exact published `codexmon` 0.8.0 archive:

```bash
curl -fLO https://github.com/tigercosmos/codexmon/releases/download/v0.8.0/codexmon_0.8.0_linux_amd64.tar.gz
echo '8339ffe889bf8ed28feb54b95c55e97ce734ac27e2fe2a19fefa0276488591b3  codexmon_0.8.0_linux_amd64.tar.gz' | sha256sum -c -
tar -xzf codexmon_0.8.0_linux_amd64.tar.gz codexmon
mkdir -p "$HOME/.local/bin"
install -m 0755 codexmon "$HOME/.local/bin/codexmon"
export PATH="$HOME/.local/bin:$PATH"
```

The published `codexmon` executable SHA-256 is:

```text
8cc2960e50e8b81e99daf1423fffb6fb694d6302c2efbfae2199b808bc9e6e98
```

The configuration records the archive URL, archive hash, and executable hash.

Check both executable identities:

```bash
codex --version
codexmon --help | head -n 1
sha256sum "$(realpath "$(command -v codex)")"
sha256sum "$(realpath "$(command -v codexmon)")"
```

The full schedule opens 64 fresh Codex sessions.
Check account limits and expected model costs before starting.

## 1. Check out the frozen package

Clone this repository and check out the reproduction commit shown in this guide.
The exact commit will remain available after newer releases change `main`.

```bash
git clone https://github.com/tigercosmos/code-cortex-mcp.git
cd code-cortex-mcp
git checkout 7cfdd1c5dda39606eeb02f5c2469660ac0057bdf
```

The configuration also pins every reused fixture and frozen harness file by SHA-256.
The `doctor` stage rejects a mismatched checkout.

## 2. Run the host checks

Choose a new work path on a volume with at least 100 GiB free.

```bash
benchmark_work=/mnt/benchmark/code-cortex-triarm-01
python3 docs/benchmarks/complete-task-reproduction/reproduce.py \
  doctor \
  --config docs/benchmarks/complete-task-reproduction/config.json \
  --work "$benchmark_work"
```

The command checks these inputs and controls:

- host operating system, architecture, CPU count, memory, and free space;
- Ubuntu distribution identifier and version;
- compiler, headers, GNU `time`, and the systemd user session;
- Codex authentication, version, architecture, and executable hash;
- `codexmon` version and executable hash;
- candidate and upstream revision pins in the selected configuration;
- fixture, task, schedule, source-oracle, lifecycle, and adapter hashes;
- the public runner path and hash;
- the original published metrics and evidence manifest.

`--allow-version-drift` is available only for diagnostic `doctor` runs.
The `prepare` stage rejects it.
Update a copied configuration when any pinned tool changes.

## 3. Prepare both MCP backends

Run preparation on the same host that will run the timed schedule.

```bash
python3 docs/benchmarks/complete-task-reproduction/reproduce.py \
  prepare \
  --config docs/benchmarks/complete-task-reproduction/config.json \
  --work "$benchmark_work"
```

Preparation performs these actions:

1. Clone and check out both pinned source revisions.
2. Build both production binaries in isolated directories.
3. Generate deterministic 10K, 1M, and 100M-line C++ fixtures.
4. Index all candidate scales and both supported upstream scales.
5. Check every expected answer directly against source.
6. Run every MCP retrieval before any model session.
7. Require byte-identical normalized context for shared MCP tasks.
8. Seal the complete cache tree, binaries, source, protocol, and setup evidence.

The stage writes `SETUP-READY.json` only after all checks pass.
That detached receipt binds the closed setup tree to its hash manifest.
Record its printed `setup_ready_sha256` in an external CI log or signed note.
Do not store the only copy inside the benchmark work directory.

Set the recorded value before later stages:

```bash
setup_ready_sha256=PASTE_THE_EXTERNALLY_RECORDED_SHA256
```

## 4. Run the uncontended schedule

Stop unrelated builds, model sessions, virtual machines, and CPU-heavy services.
The runner rejects a row when one-minute load exceeds 8.0.
It also rejects foreign processes using at least 50 percent CPU.

Run the controller in a unique systemd user service:

```bash
scope_nonce=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
scope_unit="cbm-v743-run-${scope_nonce}.service"
systemd-run --user --quiet --collect --pipe --wait \
  --unit="$scope_unit" \
  --setenv="CBM_EXPECTED_SCOPE_UNIT=$scope_unit" \
  python3 docs/benchmarks/complete-task-reproduction/reproduce.py \
    run \
    --config docs/benchmarks/complete-task-reproduction/config.json \
    --setup-ready-sha256 "$setup_ready_sha256" \
    --work "$benchmark_work"
```

Each row uses a fresh Codex home.
All arms use the same model, reasoning effort, task, source tree, and answer contract.
The shell arm receives no client context.
Each MCP arm receives one source-checked, answer-only JSON result.

Before every row, the runner proves that both MCP backends have no surviving process.
It takes three admission samples over two seconds.
It also audits the one-second samples collected during the model session.
Each runtime sample exempts the controller, monitor, agent, and active MCP process tree.
Foreign in-task CPU activity makes that row ineligible.

An admission rejection exits with an error and records a distinct finalization state.
The runner does not retry a failed row.
It does not replace missing times with estimates.
Preserve a failed work directory and start a complete rerun at a new path.

## 5. Analyze exact matched pairs

```bash
python3 docs/benchmarks/complete-task-reproduction/reproduce.py \
  analyze \
  --config docs/benchmarks/complete-task-reproduction/config.json \
  --setup-ready-sha256 "$setup_ready_sha256" \
  --work "$benchmark_work"
```

The command writes `analysis/report.json` and `analysis/RESULTS.md`.
It calculates geometric time ratios by comparison, scale, and task type.

A row is eligible only when all these conditions pass:

- the model session reached a successful terminal state;
- the exact answer grade passed;
- the source oracle passed before and after the run;
- MCP retrieval matched the source oracle;
- the model session made no MCP call of its own;
- pre-row and in-task contention checks passed;
- summary, terminal, retrieval, and controller records agree;
- executable, protocol, configuration, cache, and evidence seals remain valid.

The overall candidate-to-shell geometric ratio must be at most 0.90.
The configured completeness gates also require all 64 rows to finish and pass admission.

## 6. Verify and publish the evidence

```bash
python3 docs/benchmarks/complete-task-reproduction/reproduce.py \
  verify \
  --config docs/benchmarks/complete-task-reproduction/config.json \
  --setup-ready-sha256 "$setup_ready_sha256" \
  --work "$benchmark_work"
```

The command verifies the setup, run, and analysis hash manifests.
It rejects missing, changed, symlinked, or unlisted evidence files.
It also rechecks the immutable project databases and published reference evidence.

Publish the complete work directory with every reported result.
At minimum, report these fields:

- candidate and upstream commits and binary hashes;
- reproduction-repository commit and configuration hash;
- Codex and `codexmon` versions and executable hashes;
- kernel, compiler, CPU count, memory, and disk receipts;
- admitted, terminal, correct, and eligible row counts;
- geometric ratios, pair counts, and every excluded row.

Independent repetitions can run concurrently on separate idle hosts.
Do not split one 64-row schedule across hosts.
Report each host as a separate complete repetition.

## Compare a future version

Copy `config.json` to a new named file.
Change the candidate revision for a product comparison.
Update the tool version and hash fields when the model toolchain changes.
Keep `published_binary_sha256` as a reference when one exists.
Use `null` only when the compared revision has no reference binary.
Record each resulting binary hash before independent repetitions.

Update compiler and build-tool identities when that toolchain changes.
Keep both prior binary hashes as references when a compiler or build tool changes.
Keep the frozen tasks, schedule, retrieval contract, and analysis rules unchanged.

Commit the derived configuration beside its complete evidence summary.
Do not overwrite the published configuration or original evidence.

Model behavior and provider infrastructure can change over time.
A future run verifies the method and produces a new measurement.
It cannot guarantee the exact 2026-09-10 percentages.

The immutable published evidence remains in
[`upstream-comparison-v743`](../2026-09-09-uncontended-timing-01/scale-task-context-21/upstream-comparison-v743/).
