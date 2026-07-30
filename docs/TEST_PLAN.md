# MCP vs Explorer Quality Benchmark — Test Plan (v8: 66 Languages)

## Purpose

Compare code-cortex-mcp's structured graph queries against Claude Code's Explore agent (Grep/Glob/Read only) across all supported languages. Both phases use AI agents with the same 12-question set — the MCP agent gets MCP tools, the Explorer agent gets Grep/Glob/Read. Same questions, fair comparison.

## Prerequisites

- Built `code-cortex-mcp` binary at `~/.local/bin/code-cortex-mcp`
- Internet access for cloning repositories
- ~5GB disk space for repos in `/tmp/bench/`
- Claude Code session for both agent phases

## Phase 0: Repository Setup

```bash
scripts/clone-bench-repos.sh /tmp/bench
```

Clones repositories (shallow, `--depth 1`) and creates symlinks for shared repos. All **66 languages** from `AllLanguages()` are benchmarked — shared-repo languages get their own Group E agent runs against the same repo, testing language-specific parsing.

### Programming Languages — Tier 1 (44 languages)

| # | Language | Repository | Notes |
|---|----------|-----------|-------|
| 1 | Go | go-chi/chi | — |
| 2 | Python | httpie/cli | — |
| 3 | JavaScript | expressjs/express | HTML shares this repo (symlink) |
| 4 | TypeScript | trpc/trpc | — |
| 5 | TSX | shadcn-ui/ui | CSS shares this repo (symlink) |
| 6 | Java | spring-petclinic | SQL shares this repo (symlink) |
| 7 | Kotlin | JetBrains/Exposed | — |
| 8 | Scala | playframework/play-samples | — |
| 9 | Rust | meilisearch/meilisearch | TOML shares this repo (symlink) |
| 10 | C | redis/redis | — |
| 11 | C++ | nlohmann/json | CUDA shares this repo (symlink) |
| 12 | C# | ardalis/CleanArchitecture | — |
| 13 | PHP | koel/koel | — |
| 14 | Ruby | sinatra/sinatra | — |
| 15 | Lua | awesomeWM/awesome | — |
| 16 | Bash | bash-it/bash-it | — |
| 17 | Zig | tigerbeetle/tigerbeetle | — |
| 18 | Haskell | jgm/pandoc | — |
| 19 | OCaml | ocaml/dune | — |
| 20 | Elixir | plausible/analytics | — |
| 21 | Erlang | ninenines/cowboy | — |
| 22 | Objective-C | AFNetworking/AFNetworking | — |
| 23 | Swift | Alamofire/Alamofire | — |
| 24 | Dart | felangel/bloc | — |
| 25 | Perl | mojolicious/mojo | — |
| 26 | Groovy | spockframework/spock | — |
| 27 | R | tidyverse/dplyr | — |
| 28 | Clojure | clojure/clojure | Functional |
| 29 | F# | giraffe-fsharp/Giraffe | Pure F# web framework |
| 30 | Julia | SciML/DifferentialEquations.jl | Pure Julia, scientific |
| 31 | Vim Script | SpaceVim/SpaceVim | 1000+ .vim files |
| 32 | Nix | NixOS/nixpkgs | Config/scripting |
| 33 | Common Lisp | lem-project/lem | 200+ .lisp files |
| 34 | Elm | elm/compiler | Functional |
| 35 | Fortran | cp2k/cp2k | Systems/scientific |
| 36 | CUDA | NVIDIA/cuda-samples | ~100 .cu files |
| 37 | COBOL | OCamlPro/gnucobol | Legacy |
| 38 | Verilog | YosysHQ/yosys | Hardware |
| 39 | Emacs Lisp | emacs-mirror/emacs | Scripting |
| 40 | MATLAB | acristoffers/tree-sitter-matlab | Scientific (test corpus) |
| 41 | Lean 4 | leanprover-community/mathlib4 | Theorem prover, scientific |
| 42 | FORM | vermaseren/form | Symbolic algebra |
| 43 | Magma | ← symlink to lean | .m files disambiguated via markers |
| 44 | Wolfram | WolframResearch/WolframLanguageForJupyter | Symbolic computing |

### Helper Languages — Tier 2 (22 languages)

| # | Language | Repository | Notes |
|---|----------|-----------|-------|
| 45 | YAML | kubernetes/examples | Config language |
| 46 | HCL | terraform-aws-modules/terraform-aws-eks | Config language |
| 47 | SCSS | twbs/bootstrap | Dedicated repo |
| 48 | Dockerfile | docker-library/official-images | Dedicated repo |
| 49 | HTML | ← symlink to javascript | Shared repo (expressjs/express) — benchmarked with Group E questions |
| 50 | CSS | ← symlink to tsx | Shared repo (shadcn-ui/ui) — benchmarked with Group E questions |
| 51 | TOML | ← symlink to rust | Shared repo (meilisearch/meilisearch) — benchmarked with Group E questions |
| 52 | SQL | ← symlink to java | Shared repo (spring-petclinic) — benchmarked with Group E questions |
| 53 | JSON | ← symlink to typescript | Shared repo (trpc/trpc) — benchmarked with Group E questions |
| 54 | XML | ← symlink to java | Shared repo (spring-petclinic) — benchmarked with Group E questions |
| 55 | Markdown | ← symlink to python | Shared repo (httpie/cli) — benchmarked with Group E questions |
| 56 | Makefile | ← symlink to c | Shared repo (redis/redis) — benchmarked with Group E questions |
| 57 | CMake | kitware/CMake | Build system |
| 58 | Protobuf | googleapis/googleapis | Schema language |
| 59 | GraphQL | graphql/graphql-spec | Schema language |
| 60 | Vue | vuejs/vue | Frontend framework |
| 61 | Svelte | sveltejs/svelte | Frontend framework |
| 62 | Meson | mesonbuild/meson | Build system |
| 63 | GLSL | repalash/Open-Shaders | Hundreds of .glsl/.vert/.frag files |
| 64 | INI | ← symlink to python | Shared repo (httpie/cli) — .cfg/.ini files |
| 65 | Kubernetes | ← symlink to yaml | YAML subtype, detected by content (kustomization.yaml, Deployment, etc.) |
| 66 | Kustomize | ← symlink to yaml | YAML subtype, detected by kustomization.yaml presence |

## Phase 1: Indexing (Automated)

### 1a. Database Cleanup (mandatory before each benchmark run)

Delete all existing project databases so every language gets a **cold, fresh index** with accurate timing:

```bash
# Remove all project DBs (per-project .db files in cache dir)
rm -f ~/.cache/code-cortex-mcp/*.db

# Remove stale bench-results metrics from previous runs
rm -rf /tmp/bench-results
mkdir -p /tmp/bench-results
```

> **Why**: Re-indexing an already-indexed project reuses cached data and produces artificially low index times. A cold index reflects the real-world first-run cost that users experience.

### 1b. Index all 66 languages

```bash
ALL_LANGS="go python javascript typescript tsx java kotlin scala rust c cpp csharp php ruby lua bash zig haskell ocaml elixir erlang objc swift dart perl groovy r clojure fsharp julia vimscript nix commonlisp elm fortran cuda cobol verilog emacslisp matlab lean form magma wolfram yaml hcl scss dockerfile html css toml sql json xml markdown makefile cmake protobuf graphql vue svelte meson glsl ini kubernetes kustomize"

for lang in $ALL_LANGS; do
  scripts/benchmark-index.sh ~/.local/bin/code-cortex-mcp "$lang" /tmp/bench/"$lang" /tmp/bench-results
done
```

### What the script does per language

1. **Count LOC** (lines of code across all source files, excluding .git/node_modules/vendor/target/build/dist)
2. **Count source files** (same file extensions as LOC)
3. **Index repository** via `code-cortex-mcp cli --raw index_repository` — timed end-to-end
4. **Save all metrics** to `/tmp/bench-results/<lang>/`

### Output files per language

```
/tmp/bench-results/<lang>/
  00-index.json         # Full index response (project name, node/edge counts)
  loc.txt               # Total lines of code (source files only)
  file-count.txt        # Number of source files
  index-time.txt        # Cold index time in milliseconds
  project.txt           # Project name (used by agent phases)
  nodes.txt             # Graph node count
  edges.txt             # Graph edge count
```

### Indexing metrics table (fill in after Phase 1 completes)

| Language | Repo | Files | LOC | Index (ms) | Nodes | Edges |
|----------|------|-------|-----|------------|-------|-------|
| _(generated)_ | | | | | | |

To generate this table after indexing:
```bash
echo "| Language | Repo | Files | LOC | Index (ms) | Nodes | Edges |"
echo "|----------|------|-------|-----|------------|-------|-------|"
for lang in $ALL_LANGS; do
  d="/tmp/bench-results/$lang"
  proj=$(cat "$d/project.txt" 2>/dev/null || echo "-")
  files=$(cat "$d/file-count.txt" 2>/dev/null || echo "-")
  loc=$(cat "$d/loc.txt" 2>/dev/null || echo "-")
  ms=$(cat "$d/index-time.txt" 2>/dev/null || echo "-")
  nodes=$(cat "$d/nodes.txt" 2>/dev/null || echo "-")
  edges=$(cat "$d/edges.txt" 2>/dev/null || echo "-")
  echo "| $lang | $proj | $files | $loc | $ms | $nodes | $edges |"
done
```

## Language Groups

Each group gets 12 task-oriented questions tailored to its structural features. Questions simulate real developer tasks — the kind of questions you'd ask when joining a project, fixing a bug, or adding a feature.

### Group A: Class-based OOP

**Languages**: Java, Kotlin, Scala, C#, PHP, Ruby, Dart, Groovy, Swift, Objective-C

| # | Developer Task | What it tests |
|---|----------------|---------------|
| 1 | "I'm auditing the API surface. Find every controller/handler class and list their route-handling methods." | Route/handler discovery |
| 2 | "I need to understand the type system. What are the main interfaces/protocols and what classes implement them?" | Inheritance + interface mapping |
| 3 | "A dependency needs upgrading. Find all places where `<most-used class>` is instantiated or referenced." | Usage tracking + impact analysis |
| 4 | "Show me the source code of the largest service/controller class. I need to understand it before refactoring." | Code snippet retrieval |
| 5 | "I found a bug in `<central method>`. What does it call? Show the full outbound call tree." | Outbound call trace |
| 6 | "For that same method — who calls it? Trace back to the entry point." | Inbound call trace |
| 7 | "How does a request flow from HTTP entry to database and back? Describe the architectural layers." | Architecture + data flow |
| 8 | "Which methods have the most callers? I need to know what's riskiest to change." | Hub detection / impact ranking |
| 9 | "I'm adding a new feature. Find all abstract classes or interfaces I should implement." | Extension point discovery |
| 10 | "Where is input validation done? Find all validation, sanitization, or guard functions." | Pattern search (domain-specific) |
| 11 | "What external libraries does the main entry point depend on? List all third-party imports." | Dependency analysis |
| 12 | "I'm onboarding. Describe the package structure and what each major package/namespace does." | Project structure |

### Group B: Systems Languages

**Languages**: Go, Rust, C, C++, Zig, CUDA, Fortran, GLSL, Verilog, COBOL, MATLAB, Lean 4, FORM

| # | Developer Task | What it tests |
|---|----------------|---------------|
| 1 | "Find all exported/public functions that form the library's public API surface." | Public API discovery |
| 2 | "What interfaces (Go) / traits (Rust) / header-declared types exist, and what implements them?" | Interface/trait mapping |
| 3 | "I need to understand error handling. Find all functions that return errors and how errors propagate." | Error flow analysis |
| 4 | "Show the source of the most complex function. I need to refactor it." | Code snippet retrieval |
| 5 | "Pick a central function in the hot path. What does it call? Map the dependency tree." | Outbound call trace |
| 6 | "For that function — who calls it? Show all call sites across the codebase." | Inbound call trace |
| 7 | "What runs at initialization? Find all init/main/setup functions and what they configure." | Init/startup discovery |
| 8 | "How is the codebase layered? What are the package boundaries and dependency direction between them?" | Architecture understanding |
| 9 | "Which functions have the highest fan-out (call the most other functions)? These are complexity hotspots." | Complexity / hub detection |
| 10 | "Find all resource management patterns — alloc/free, defer, Drop, close, cleanup functions." | Resource lifecycle analysis |
| 11 | "What does the main package/module import? List all direct dependencies." | Dependency analysis |
| 12 | "Describe the module/package structure and what each major component is responsible for." | Project structure |

> **COBOL note**: COBOL is a procedural programming language (programs, divisions, sections, paragraphs) and belongs here despite its legacy status. Interpret Group B questions as: Q1=PARAs/SECTIONs forming the public API, Q2=COPY books and 01-level data structures in lieu of interfaces/types, Q3=error handling via ON EXCEPTION/INVALID KEY/NOT ON SIZE ERROR, Q7=IDENTIFICATION/ENVIRONMENT divisions and entry points, Q10=OPEN/CLOSE/READ/WRITE file-handling verbs as resource management.

### Group C: Dynamic / Scripting Languages

**Languages**: Python, JavaScript, TypeScript, TSX, Lua, Perl, R, Bash, Vim Script, Nix, Emacs Lisp

| # | Developer Task | What it tests |
|---|----------------|---------------|
| 1 | "Find all route definitions or request handlers. List URL patterns and handler functions." | Route/handler discovery |
| 2 | "Find all middleware, decorators, or higher-order functions that wrap behavior." | Wrapper/decorator discovery |
| 3 | "What are the main classes or modules? Show the class hierarchy or module dependency graph." | Structural overview |
| 4 | "Show the source of the most complex function. I need to understand it." | Code snippet retrieval |
| 5 | "Pick a key business logic function. What does it call? Trace the outbound call chain." | Outbound call trace |
| 6 | "For that function — who calls it? Find all call sites." | Inbound call trace |
| 7 | "Where is configuration loaded? Find all config, env var, or settings access points." | Config discovery |
| 8 | "How does a request flow through the app? Describe the layers from entry to response." | Architecture + data flow |
| 9 | "What does this package export? List all public functions/classes intended for consumers." | Public API discovery |
| 10 | "Which functions are called from the most places? These are the most impactful to change." | Hub detection / impact ranking |
| 11 | "What does the main entry file import? List all dependencies and what they provide." | Dependency analysis |
| 12 | "Describe the directory layout and what each major directory is responsible for." | Project structure |

### Group D: Functional Languages

**Languages**: Haskell, OCaml, Elixir, Erlang, Clojure, F#, Julia, Common Lisp, Elm, Magma, Wolfram

| # | Developer Task | What it tests |
|---|----------------|---------------|
| 1 | "Find all exported/public functions that serve as the module's API." | Public API discovery |
| 2 | "What are the main type definitions — records, ADTs, typeclasses (Haskell/OCaml), behaviours (Erlang/Elixir)? List them with fields/variants." | Type system mapping |
| 3 | "Find the most complex pattern-matching function — show all its clauses and what it dispatches on." | Pattern match analysis |
| 4 | "Show the source of the longest function. What does it do?" | Code snippet retrieval |
| 5 | "Pick a central function. What does it call? Trace the transformation pipeline." | Outbound call trace |
| 6 | "For that function — who calls it? Show the dependency chain back to entry points." | Inbound call trace |
| 7 | "Find all behaviour/typeclass implementations and what callbacks they define." | Behaviour/typeclass mapping |
| 8 | "How is the application structured? Describe the supervision tree (Erlang/Elixir) or module hierarchy." | Architecture understanding |
| 9 | "Find all stateful components — GenServers, agents, processes, IORef, MVar, ETS tables." | State management discovery |
| 10 | "Which functions are the most connected (most callers + callees)? These are architectural hotspots." | Hub detection |
| 11 | "What does the main module depend on? List all imports and what each provides." | Dependency analysis |
| 12 | "Describe the module structure and how code is organized into applications/libraries." | Project structure |

### Group E: Config / Markup Languages

**Languages**: YAML, HCL, SCSS, Dockerfile, HTML, CSS, TOML, SQL, JSON, XML, Markdown, Makefile, CMake, Protobuf, GraphQL, Vue, Svelte, Meson, INI, Kubernetes, Kustomize

| # | Developer Task | What it tests |
|---|----------------|---------------|
| 1 | "What resources/objects/selectors are defined? List all top-level definitions with file paths." | Definition discovery |
| 2 | "What's the overall structure? Describe the hierarchy of definitions and how they nest." | Structural overview |
| 3 | "Find all definitions related to networking, security, or access control." | Domain-specific pattern search |
| 4 | "Show the content of the most complex/longest definition file." | Content retrieval |
| 5 | "What variables, references, or cross-file dependencies exist? Find all cross-references." | Reference tracking |
| 6 | "Find similar or duplicated definitions across files." | Duplication detection |
| 7 | "How are files organized? What naming conventions and directory patterns are used?" | File organization |
| 8 | "What are the dependencies between files? (imports, includes, references, extends)" | Dependency analysis |
| 9 | "Which files are largest? Which have the most definitions?" | Size / density analysis |
| 10 | "What naming conventions are used? Find all definitions matching common patterns." | Naming pattern analysis |
| 11 | "What are the main/root files? What do they include or reference?" | Entry point discovery |
| 12 | "Describe the overall project layout and what each directory contains." | Project structure |

## Phase 2: MCP Agent Phase (Claude Code agents)

For each language, spawn a **general-purpose agent** with strict MCP-only constraints answering the 12 questions using **only** graph tools.

### MCP Agent Configuration

- `subagent_type="general-purpose"`, `run_in_background=true`, `max_turns=8`
- Tool budget: **8 turns total** — 1 ToolSearch + 6 MCP calls + 1 Bash write
- Forbidden: Grep, Glob, Read, Edit, Write — calling any of these invalidates the run
- The hard `max_turns=7` cap enforces the budget mechanically

### Tool Budget Design

With only 5 MCP calls for 12 questions, the agent **must** choose calls that cover multiple questions at once. This is intentional — it measures whether MCP tools deliver high information density:

| Turn | Tool | Questions covered |
|------|------|-------------------|
| 1 | ToolSearch("+code-cortex") | — loads tools |
| 2 | get_architecture(aspects=["all"]) | Q7, Q8, Q12 (init, layers, structure) |
| 3 | search_graph(label="Function", min_degree=3) | Q1, Q9 (public API, fan-out hotspots) |
| 4 | search_graph(label="Interface"/"Trait"/"Type") | Q2, Q10 (interfaces, validation patterns) |
| 5 | trace_call_path(function, direction="both") | Q5, Q6 (outbound + inbound call tree) |
| 6 | get_code_snippet(most complex function) | Q3, Q4 (error handling, code retrieval) |
| 7 | Bash — write output file | Q11, Q12 answered from arch call above |

> **Benchmark insight**: An MCP agent that answers 12 questions in 5 tool calls demonstrates the core value proposition of structured graph queries over exploratory grep. Token target: ≤ 10k per agent.

### Context Window Constraint

- Agents write output via **one Bash call at the very end** (heredoc into file).
- No SendMessage. No team_name. Agents write file and exit.
- Completion detected by checking file existence: `ls v9/*-mcp.md | wc -l`
- MCP results are compact (~300–600 tokens each). 6 calls = ~4k tokens of raw results.

### MCP Agent Prompt Template

```
TOTAL TOOL BUDGET: 7 turns. Spend them wisely.
  Turn 1: ToolSearch — loads MCP tools (mandatory)
  Turns 2–7: 6 MCP tool calls — cover all 12 questions
  Turn 8: Bash — write output file (one heredoc, no other Bash)

FORBIDDEN (calling these invalidates the run):
  Grep, Glob, Read, Edit, Write, WebSearch, WebFetch

You are a benchmark agent for code-cortex-mcp on <LANGUAGE>.
Project: <PROJECT> | LOC: <LOC> | Nodes: <NODES> | Edges: <EDGES>

STRATEGY — cover multiple questions per call:
- get_architecture(aspects=["all"]) → answers structure, layers, init, dependencies
- search_graph(label=<primary type>, min_degree=3) → answers API surface, hotspots
- search_graph(label=<interface/trait type>) → answers type system, validation patterns
- trace_call_path(function=<central fn>, direction="both") → answers call chains
- get_code_snippet(qualified_name=<complex fn>) → answers code retrieval, error handling

Answer ALL 12 questions from these 5 calls. If a call returns 0 results,
adjust label/pattern in your reasoning — do NOT make an extra tool call.

<GROUP-SPECIFIC QUESTIONS — see Language Groups section>

FORMAT (compose all answers in your reasoning, then write file in Turn 7):
### Q<N>: <question summary>
**Answer**: <specific names, files, counts from tool results>
**Confidence**: HIGH / MEDIUM / LOW
**Zero results**: yes/no

Turn 7 — write file with single Bash heredoc:
  bash: cat > /Users/.../benchmark-results/v<X>/<LANG>-mcp.md << 'BENCHMARK_EOF'
  # <Language> — MCP Benchmark (v<X>)
  Repo: <REPO> | LOC: <LOC> | Nodes: <NODES> | Edges: <EDGES>
  [all 12 Q&A blocks]
  BENCHMARK_EOF

No SendMessage. No team. Write the file and exit.
```

> **Enforcement**: `max_turns=7` is a hard cap. The agent physically cannot make more than 7 tool calls. This guarantees MCP-only behavior and forces strategic multi-question coverage per call.

### MCP Agent Output File Format (mandatory)

Every MCP agent **must** write its output file in this exact structure. Consistent formatting enables automated score extraction and cross-language comparison.

```markdown
# <Language> — MCP Benchmark (v<X>)
Repo: <org/repo> | LOC: <LOC> | Nodes: <NODES> | Edges: <EDGES>

---

### Q1: <question tag, e.g. "Route/handler discovery">
**Answer**: <specific names, files, counts — no vague summaries>
**Confidence**: HIGH / MEDIUM / LOW
**Zero results**: yes / no

### Q2: <question tag>
**Answer**: ...
**Confidence**: HIGH / MEDIUM / LOW
**Zero results**: yes / no

[... Q3 through Q12 in same format ...]

---

## Summary Scorecard
**Overall**: <N> HIGH, <N> MEDIUM, <N> LOW, <N> zero-result answers.
<One sentence on what MCP graph tools could and couldn't answer for this language.>
```

**Confidence definitions** (use consistently — do NOT use GOOD/EXCELLENT/PARTIAL/NONE):

| Rating | Meaning | Score weight |
|--------|---------|-------------|
| `HIGH` | Answer is specific, names real identifiers/files, covers the question fully | 1.0 |
| `MEDIUM` | Answer has useful content but is incomplete, or relies on inference | 0.5 |
| `LOW` | Answer is vague, approximate, or answered only from general project knowledge | 0.1 |

**Zero results**: set to `yes` if the primary MCP tool call for that question returned 0 items.

**Summary Scorecard** is the source for automated score extraction:
```
(HIGH_count × 1.0 + MEDIUM_count × 0.5 + LOW_count × 0.1) / 12 = MCP Score
```

### Explorer Agent Output File Format (mandatory)

```markdown
# <Language> — Explorer Benchmark (v<X>)
Repo: /tmp/bench/<lang>/ | LOC: <LOC>

---

### Q1: <question tag>
**Tool calls used**: <N>
**Answer**: <specific results with names, files, line numbers>
**Confidence**: HIGH / MEDIUM / LOW

[... Q3 through Q12 in same format ...]

---

## Summary Scorecard
**Overall**: <N> HIGH, <N> MEDIUM, <N> LOW.
**Total tool calls**: <N> (Grep: <N>, Glob: <N>, Read: <N>)
<One sentence on coverage breadth and any questions that couldn't be answered.>
```

## Phase 3: Explorer Agent Phase (Claude Code agents)

Same 12 questions per language group. Agent uses **Grep, Glob, Read only** — no MCP access, unlimited tool calls.

### Explorer Agent Configuration

- `subagent_type="Explore"`, `run_in_background=true`, `max_turns=40`
- Agent has access to: Grep, Glob, Read
- No tool call limit per question — can search exhaustively

### Explorer Agent Prompt Template

```
RULES: Never use sleep. No background commands.
You MUST write your results to a file — do NOT return long answers in your
final message.

You are benchmarking code exploration on a <LANGUAGE> repository at /tmp/bench/<LANG>/
LOC: <LOC>

Answer these 12 questions using ONLY Grep, Glob, and Read tools.
Be thorough — search exhaustively for each question.

<GROUP-SPECIFIC QUESTIONS — same as MCP agent, see Language Groups section>

FORMAT each answer as (mandatory — use ONLY these confidence labels):
### Q<N>: <question tag>
**Tool calls used**: <count>
**Answer**: <specific results with names, files, line numbers>
**Confidence**: HIGH / MEDIUM / LOW

Confidence: HIGH = specific+complete, MEDIUM = partial or inferred, LOW = vague.

CRITICAL — OUTPUT INSTRUCTIONS:
1. Write ALL 12 answers to: benchmark-results/v<X>/<LANG>-explorer.md
   Use the exact header format:
   # <Language> — Explorer Benchmark (v<X>)
   Repo: /tmp/bench/<LANG>/ | LOC: <LOC>
2. End the file with this Summary Scorecard:
   ## Summary Scorecard
   **Overall**: <N> HIGH, <N> MEDIUM, <N> LOW.
   **Total tool calls**: <N> (Grep: <N>, Glob: <N>, Read: <N>)
   <One sentence summary.>
3. Your final message back MUST be only 2-3 lines:
   "Done. Wrote <LANG>-explorer.md. HIGH: <N>/12, MEDIUM: <N>/12, LOW: <N>/12."
   Do NOT repeat answers in your message — they are in the file.
```

### Spawning — Direct Agent Tool, No Teams

> **IMPORTANT: Do NOT use TeamCreate/team members for benchmark agents.**
> The team pattern introduces idle notifications, shutdown coordination, and
> message routing overhead that is buggy at this scale (59 agents). Agents
> that receive a shutdown request mid-run will terminate before writing their
> output file, losing all results.
>
> **Correct approach**: spawn agents directly via the `Agent` tool with
> `run_in_background=true` and NO `team_name`. Agents write their output file
> as their LAST action and then terminate cleanly. Completion is tracked by
> checking file existence (`ls v9/*-mcp.md | wc -l`), not by messages.
>
> **No SendMessage in agent prompts.** Agents do not message back — they
> simply write the file and exit. The main channel detects completion by
> checking for the output file after each batch.

```python
# Spawn agents in parallel batches (max 9 concurrent):
# Batch 1 (9): Go, Python, JavaScript, TypeScript, TSX, Java, Kotlin, Scala, Rust
# Batch 2 (9): C, C++, C#, PHP, Ruby, Lua, Bash, Zig, Haskell
# Batch 3 (9): OCaml, Elixir, Erlang, Objective-C, Swift, Dart, Perl, Groovy, R
# Batch 4 (9): Clojure, F#, Julia, Vim Script, Nix, Common Lisp, Elm, Fortran, CUDA
# Batch 5 (9): COBOL, Verilog, Emacs Lisp, MATLAB, Lean 4, FORM, Magma, Wolfram, YAML
# Batch 6 (9): HCL, SCSS, Dockerfile, HTML, CSS, TOML, SQL, JSON, XML
# Batch 7 (9): Markdown, Makefile, CMake, Protobuf, GraphQL, Vue, Svelte, Meson, GLSL
# Batch 8 (3): INI, Kubernetes, Kustomize
```

### Measurement: What's Included and Excluded

Each agent run produces raw metrics. To get a fair comparison, we measure the **question-answering cost** — what it takes to answer the 12 questions — and exclude overhead that's identical between approaches.

**Included in per-language metrics:**
- **Tokens**: All input + output tokens consumed during the 12-question answering phase (tool calls, Claude's reasoning about tool results, answer formulation). This includes Claude Code interpreting MCP results or reading grep output — that interpretation cost is part of the approach's real cost.
- **Time**: Wall-clock duration from first tool call to final answer for the 12 questions.
- **Tool calls**: Total tool invocations during question answering.

**Excluded (identical overhead, filters out):**
- Agent spawn/teardown time and tokens (prompt injection, system message, initial context)
- Summary file generation tokens (formatting the final report output)
- Grading/evaluation work done after answers are collected

**How to capture**: Each agent prompt instructs the agent to emit `<!-- BENCH_START -->` before Q1 and `<!-- BENCH_END -->` after Q12. Token counts are measured between these markers via the Claude Code API usage delta. Wall-clock time is the delta between the two markers.

## Phase 3.5: Retry Policy

After all agents in a batch complete, inspect each language's final message before proceeding to grading. Retry once if either condition is true:

| Trigger | Condition |
|---------|-----------|
| **Low confidence** | Agent summary reports `Confident: ≤ 3/12` |
| **Pervasive zero results** | Agent summary reports `ZeroResults: ≥ 6/12` |

A single retry is allowed per language per phase (MCP or Explorer). Do not retry a second time — if the retry also scores ≤ 3/12, accept the score and flag it in the IMPROVEMENTS.md as a systemic gap.

### Retry Prompt Additions (append to original prompt)

```
RETRY CONTEXT: A previous attempt on this language scored only <N>/12 confident
with <M>/12 zero-result questions. This usually means label mismatches or overly
specific search patterns. Apply these strategies:

1. START with get_graph_schema to see exactly what node labels and edge types exist
   for this project before writing any queries.
2. Try ALL label variants: Function, Method, Procedure, Program, Paragraph, Module,
   Block, Definition, Declaration — use whichever the schema shows.
3. For zero-result traces: first search_graph to find any function, then
   trace_call_path on that exact name. Never guess a function name.
4. For COBOL specifically: labels are likely "Procedure" or "Paragraph" not "Function".
5. For config/markup (Group E): use search_graph(label="Variable") for definitions,
   search_code for content patterns.
6. When a search returns 0, broaden: remove name_pattern, use just label=.
7. Report in each answer what schema labels you found and which one worked.
```

## Phase 4: Grading

### Grading Scale

Each answer is scored on three dimensions (0.0 – 1.0):
- **Correctness**: Are the facts accurate? Do function names/paths/line numbers match reality?
- **Completeness**: Does the answer cover the full scope? (e.g., all functions vs just a sample)
- **Specificity**: Are results concrete (exact names, paths, lines) vs vague summaries?

### Grade Thresholds

| Grade | Avg Score | Meaning |
|-------|-----------|---------|
| PASS (P) | ≥ 0.80 | Answer is correct, complete, and specific |
| PARTIAL (/) | 0.40 – 0.79 | Answer has useful content but is incomplete or partially wrong |
| FAIL (F) | < 0.40 | No useful answer or fundamentally incorrect |

### Grading Method

Both phases are graded by manual review of agent text output. Each answer compared against:
1. Cross-validation between MCP and Explorer results
2. Direct spot-checks of the actual source code (sample 3-5 results per question)

### Scoring

Per-question: P=1.0, /=0.5, F=0.0.
Per-language score = average of all 12 question grades.

## Phase 5: Report Generation

Each benchmark run produces a **self-contained snapshot** in `benchmark-results/v(x)/`. No references to previous runs — each version directory stands alone. Two report files serve different audiences.

### Per-Language Report (`benchmark-results/v(x)/<lang>.md`)

```markdown
# <Language> — MCP vs Explorer Benchmark
Repository: <org/repo> | Group: <A/B/C/D/E>
LOC: <n> | Files: <n> | Indexed: <n> nodes, <n> edges | Index time: <n>ms
Retried: yes/no

## Results

| Q# | Task | MCP Grade | Exp Grade | MCP Calls | Exp Calls | MCP Tokens | Exp Tokens |
|----|------|-----------|-----------|-----------|-----------|------------|------------|
| 1  | ...  | P/\/F     | P/\/F     | n/6       | n         | n          | n          |
| .. | ...  | ...       | ...       | ...       | ...       | ...        | ...        |

## Totals
MCP Score: <n>/1.0 | Explorer Score: <n>/1.0 | Tier: <A/B/C/D>
MCP Tokens: <n> | Explorer Tokens: <n> | Token Ratio: <n>x
MCP Time: <n>s | Explorer Time: <n>s | Speed Ratio: <n>x

## Failure Analysis

> Required for every question graded F (MCP score < 0.40). Skip if all questions pass.
> Grader must open the actual source files and read 2-3 code segments to produce this.

### Q<N> FAIL — <question summary>

**What the MCP agent found**: <actual tool results — e.g., "0 results from search_graph(label='Function')">

**What should have been found** (verified from source):
- `<name>` at `<file>:<line>` — <one-line description>
- `<name>` at `<file>:<line>` — <one-line description>

**Code sample** (representative snippet the parser should have extracted):
```<language>
<actual source code, 5-15 lines, copied from the repo>
```

**Root cause**: <one of the following categories, with explanation>
- `LABEL_MISMATCH` — searched `Function` but nodes are `Method` / `Procedure` / `Paragraph`
- `EXTRACTION_GAP` — parser does not extract this construct for this language (tree-sitter node not in spec)
- `CALLS_MISSING` — CALLS edges not generated for this language/pattern; graph is structurally incomplete
- `QUERY_STRATEGY` — correct nodes exist but agent used wrong search pattern or didn't broaden after 0 results
- `PARSE_ERROR` — grammar failed to parse these files; nodes were silently skipped

**Educated assessment**: <2-4 sentences. Given the code sample above, explain exactly why
the pipeline produced this outcome. Reference the specific tree-sitter node type that was
or wasn't matched, and what a fix would look like in the pipeline.>

---
```

### `SUMMARY.md` — Shareable Results (no technical recommendations)

This file is the public-facing benchmark report. It presents aggregate quality, token efficiency, and speed metrics. No internal improvement suggestions — keep it factual and clean, suitable for README references and external sharing.

```markdown
# code-cortex-mcp Benchmark — v<x>

<date> | <n> languages | <n> repositories

## Overview

| Metric | MCP | Explorer | Ratio |
|--------|-----|----------|-------|
| Avg Quality Score | <n>/1.0 | <n>/1.0 | — |
| Avg Tokens per Language | <n> | <n> | <n>x reduction |
| Avg Time per Language | <n>s | <n>s | <n>x faster |

## Quality by Language

| Language | Group | Repo | Nodes | Edges | MCP Score | Exp Score | Tier |
|----------|-------|------|-------|-------|-----------|-----------|------|
| Go       | B     | go-chi/chi | 1234 | 5678 | 0.83 | 0.75 | A |
| ...      | ...   | ... | ... | ... | ... | ... | ... |

## Token Efficiency by Language

| Language | MCP Tokens | Exp Tokens | Reduction | MCP Calls | Exp Calls |
|----------|------------|------------|-----------|-----------|-----------|
| Go       | 3,200      | 410,000    | 99.2%     | 42        | 187       |
| ...      | ...        | ...        | ...       | ...       | ...       |

## Quality by Language Group

| Group | Languages | Avg MCP | Avg Exp | Avg Token Reduction |
|-------|-----------|---------|---------|---------------------|
| A: OOP | 10 | ... | ... | ... |
| B: Systems | 13 | ... | ... | ... |
| C: Dynamic | 11 | ... | ... | ... |
| D: Functional | 11 | ... | ... | ... |
| E: Config | 21 | ... | ... | ... |

## Quality by Question Type

| Task Type | Questions | Avg MCP | Avg Exp |
|-----------|-----------|---------|---------|
| API/Route Discovery | Q1 | ... | ... |
| Type/Interface Mapping | Q2 | ... | ... |
| Usage/Impact Analysis | Q3 | ... | ... |
| Code Snippet Retrieval | Q4 | ... | ... |
| Outbound Call Trace | Q5 | ... | ... |
| Inbound Call Trace | Q6 | ... | ... |
| Architecture/Flow | Q7 | ... | ... |
| Hub/Hotspot Detection | Q8/Q9/Q10 | ... | ... |
| Dependency Analysis | Q11 | ... | ... |
| Project Structure | Q12 | ... | ... |

## Tier Distribution

| Tier | Score Range | Count | Languages |
|------|------------|-------|-----------|
| A — Excellent | ≥ 0.67 | <n> | ... |
| B — Good | 0.50–0.66 | <n> | ... |
| C — Partial | 0.42–0.49 | <n> | ... |
| D — Weak | < 0.42 | <n> | ... |
```

### `IMPROVEMENTS.md` — Internal Technical Analysis

This file is for internal development use. It identifies specific technical gaps, systematic failures, and actionable improvements to the MCP pipeline. Not intended for external sharing.

```markdown
# Benchmark v<x> — Technical Improvements

<date> | Based on <n>-language benchmark run

## Systematic Failures

Patterns where MCP consistently underperforms Explorer across multiple languages.

| Pattern | Affected Languages | Questions | Root Cause | Severity |
|---------|--------------------|-----------|------------|----------|
| <description> | Go, Rust, ... | Q3, Q6 | <technical reason> | HIGH/MED/LOW |
| ... | ... | ... | ... | ... |

## Per-Language Gaps

Languages where MCP scores significantly below Explorer. Each entry MUST include:
- The specific failing questions
- A code sample from the actual repo (read the file, paste 5-15 lines)
- An educated assessment of the root cause grounded in that code

### <Language> (MCP: <n>/1.0, Exp: <n>/1.0, Retried: yes/no)

**Q<n> — <task summary>**
Root cause: `EXTRACTION_GAP` / `LABEL_MISMATCH` / `CALLS_MISSING` / `QUERY_STRATEGY` / `PARSE_ERROR`
Code sample that wasn't found:
```<language>
<5-15 lines of actual source that the pipeline missed>
```
Assessment: <2-3 sentences explaining what tree-sitter node type this is, whether the
lang spec covers it (check internal/lang/<name>.go), and what change would fix it.
E.g.: "This is a `function_declaration` node in the Go grammar. The lang spec sets
FunctionNodeTypes to `["function_declaration"]` which should match — the issue is that
these are method literals assigned to variables (`var f = func()`) which are
`func_literal` nodes not covered by the spec. Adding `func_literal` to
FunctionNodeTypes would capture them.">

**Q<n> — <task summary>**
Root cause: ...
Code sample: ...
Assessment: ...

**Overall fix for <Language>**: <1-2 sentence summary of the highest-impact change>

---

## Missing Capabilities

Features or extractions that Explorer can answer but MCP cannot due to pipeline limitations.

| Capability | Example Question | Languages | Root Cause | Effort |
|------------|-----------------|-----------|------------|--------|
| <description> | <question> | ... | EXTRACTION_GAP / CALLS_MISSING / ... | S/M/L |
| ... | ... | ... | ... | ... |

## Node Type / Spec Gaps

Languages where tree-sitter node types in lang specs are incomplete or wrong.
Each row must cite the actual node type name from the grammar (verify with tree-sitter playground or ast dump).

| Language | Missing Node Type | lang spec file | Impact | Suggested Fix |
|----------|-------------------|----------------|--------|---------------|
| ... | `func_literal` | internal/lang/go.go | Functions assigned to vars not indexed | Add to FunctionNodeTypes |
| ... | ... | ... | ... | ... |

## Recommendations (Priority Order)

Ordered by (severity × languages affected). Each item must cite which benchmark questions it would fix.

1. **<title>** — <description, root cause category, affected languages, questions fixed, expected score improvement>
2. **<title>** — ...
3. ...
```

## Language Tier Classification

| Tier | MCP Score Range | Meaning |
|------|----------------|---------|
| A | ≥ 0.67 | Strong MCP support, competitive with Explorer |
| B | 0.50 – 0.66 | Good support with identifiable gaps |
| C | 0.42 – 0.49 | Partial support, significant weaknesses |
| D | < 0.42 | Weak support, Explorer strongly preferred |

## Agent Strategy Notes

### Shared-Repo Languages

Languages that share a repository with another language (e.g., TOML shares Rust's repo, JSON shares TypeScript's repo) require `file_pattern` filtering to scope queries to the target language's files. Without `file_pattern`, the MCP agent's search results will be dominated by the primary language.

**Example**: To find TOML config entries in the meilisearch repo:
```
search_graph(label="Variable", file_pattern="*.toml")
search_graph(label="Class", file_pattern="*.toml")
```

### Config Language Agent Strategy

Config languages (TOML, INI, JSON, XML, YAML, Markdown) now produce sub-file nodes:

| Language | Class/Section nodes | Variable nodes | Use case |
|---|---|---|---|
| TOML | `table`, `table_array_element` → Class | `pair` → Variable | Config sections, key-value pairs |
| INI | `section` → Class | `setting` → Variable | Config sections, settings |
| JSON | — | `pair` → Variable | JSON keys |
| XML | `element` → Class | — | XML structure |
| YAML | — | `block_mapping_pair` → Variable | YAML keys |
| Markdown | `atx_heading`, `setext_heading` → Section | — | Document structure |

**CONFIGURES edges** connect config nodes to code:
- `search_graph(relationship="CONFIGURES")` — find all config↔code links
- `trace_call_path(function_name="...", direction="inbound")` on config nodes — find what code uses them
- Three linking strategies:
  1. **Key→Symbol**: `max_connections` in TOML → `getMaxConnections()` in Go
  2. **Dependency→Import**: `serde` in Cargo.toml → `use serde::Serialize` in Rust
  3. **File Reference**: `"config/database.toml"` string literal → config Module node

## Reproducibility

```bash
# 1. Clone all repos (skip existing)
scripts/clone-bench-repos.sh /tmp/bench

# 2. Index all 66 languages
mkdir -p /tmp/bench-results
ALL_LANGS="go python javascript typescript tsx java kotlin scala rust c cpp csharp php ruby lua bash zig haskell ocaml elixir erlang objc swift dart perl groovy r clojure fsharp julia vimscript nix commonlisp elm fortran cuda cobol verilog emacslisp matlab lean form magma wolfram yaml hcl scss dockerfile html css toml sql json xml markdown makefile cmake protobuf graphql vue svelte meson glsl ini kubernetes kustomize"

for lang in $ALL_LANGS; do
  scripts/benchmark-index.sh ~/.local/bin/code-cortex-mcp "$lang" /tmp/bench/"$lang" /tmp/bench-results
done

# 3. Run MCP agent phase (in Claude Code session)
# Spawn Explore agents per language with MCP-only instructions — see Phase 2

# 4. Run Explorer agent phase (in Claude Code session)
# Spawn Explore agents per language with Grep/Glob/Read — see Phase 3

# 5. Grade and write reports
# Write per-language and summary reports to benchmark-results/v(x)/
```
