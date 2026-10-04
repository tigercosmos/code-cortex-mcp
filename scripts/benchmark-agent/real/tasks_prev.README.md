# tasks_prev.json

Earlier audited real-repository tasks (`tasks_ladder.json`, `tasks_main.json`), ported to sim1 on 2026-10-03.

- `tasks_prev.json`: 34 kept tasks. Each task's `verify_cmd` reproduced its `ground_truth` on sim1.
- `tasks_prev_dropped.json`: 2 dropped tasks. The `port_note` field gives the difference.
- Not ported: modmesh and Yatagarasu tasks.

## Repositories

Each repository is at `~/prevbench-20261003/repos/<repo_key>`. It is a `git clone --filter=blob:none` of the upstream URL, checked out at the commit below. Each working tree is clean.

| repo_key | upstream | commit | language |
|---|---|---|---|
| jansson | akheron/jansson | 851a2145e3256f2e67e5dfe24b0e456bf198b741 | C |
| lz4 | lz4/lz4 | 0774d05537f9762f838f7ab541b7765f1a729cb5 | C |
| elfuse | sysprog21/elfuse | be4321e1418f63718af35e18c71ab219a8dfb90b | C |
| pcapplusplus | seladb/PcapPlusPlus | ad344a87e68c3d37aed45d42fd13f165f8801c20 | C++ |
| cgal | CGAL/cgal | 67d6244bded075903f6961b393371ab22a57d138 | C++ |
| opencv | opencv/opencv | ebd55c1b41feaae9b1fd3bf73ed7087a6502c5a1 | C++ |
| codecortex | tigercosmos/code-cortex-mcp | 6e12128d9df2ccf2efc06c55ba754bdbc7cd83b2 | C++ |

## Task fields

These fields were added or changed:

- `id`: `<source_set>_<orig_id>`. Both source files use the IDs `pcapplusplus_callers`, `pcapplusplus_callchain`, `pcapplusplus_impact` and `pcapplusplus_locate`, so the prefix keeps each ID unique. `orig_id` and `source_set` (`ladder` or `main`) hold the original values.
- `repo_path`: the sim1 path. The Mac path was replaced with the sim1 path in every field (question, alternates, grading_notes, evidence, verify_cmd).
- `language`, `commit`, `clone`, `port_note`, `status`.
- `verify_output_sim1`: the stdout of `verify_cmd` on sim1, run from the repository root with bash.

## Grading rules by answer_kind (from grade.py)

`grade.py` reads `repo_path` from the task and removes that prefix from absolute paths in the answer. Run it with `tasks_prev.json` as the task file.

- `set_of_paths`: The grader takes the lines that contain only a path with a known source extension. If there are no such lines, it takes all path-shaped tokens in the reply. It removes a leading `./`, the `repo_path` prefix and any `:line` suffix. Then it computes precision, recall and F1 against `ground_truth` as sets. If the F1 is less than 1, it also compares basenames only and keeps the higher score. Order does not matter.
- `path_and_line`: The score is 0.5 for the path plus 0.5 for the line. The path counts if it is among the extracted paths or if its basename occurs in the reply. The line counts if any number in the reply is within ±3 of the ground-truth line.
- `ordered_list`: The grader splits the reply on newlines, `->`, `→`, `⇒`, `,` and `;`, and keeps only the part of each name after the last `::`. The score is the higher of two values: the `difflib.SequenceMatcher` ratio between the reply list and the gold list (case-insensitive), and the fraction of gold names found anywhere in the reply. The grader checks `ground_truth` and each list in `alternates`, and keeps the best score.
- `set_of_names`: The grader takes the lines that contain only an identifier, after it removes bullets and numbering. If there are no such lines, it takes the extracted lines plus capitalized words. It removes argument lists and qualifiers (only the part after the last `::` stays). Then it computes precision, recall and F1 against `ground_truth` as sets.
- `exact_string`: The score is 1 if `ground_truth[0]` occurs in the reply as a case-insensitive substring, or if it occurs after all whitespace is removed from both. Otherwise the score is 0.
