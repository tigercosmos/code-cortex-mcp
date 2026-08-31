# Cut a release

This guide takes you from a green `main` to a published GitHub release.

A release starts when you push a version tag. The pipeline reads the version
from the tag name, so no file in the tree sets the released version. Two files
still carry a version for other readers, and nothing in CI checks them. Bump
them by hand, in the same commit, before you tag.

## Before you tag

1. Make sure the Dry Run workflow is green on `main`. The release pipeline runs
   the same lint, test, build and smoke jobs again. A red Dry Run is a red
   release.
2. Choose the version. Each upstream sync so far has taken one minor step:
   `v0.15.0`, `v0.16.0`, `v0.17.0`, `v0.20.0`.
3. Set the version in `server.json`. Change the `version` field and all five
   `identifier` download URLs, which contain the tag name.
4. Set the version in `UPSTREAM_SYNC.md`. Change the **Fork release** field in
   the "Last synced" table, and add `Shipped as fork **vX.Y.Z**.` to the end of
   the current sync-log row.
5. Check the manifest against the version you are about to tag:

   ```sh
   scripts/check-server-json-version.sh v0.20.0
   ```

   This parses `server.json`, and asserts the `version` field and every asset
   URL carry that exact version. CI runs it too, so steps 3–4 can no longer be
   skipped silently — see "What the pipeline gates on" below.

6. Commit both files and push to `main`.

## Tag and publish

Push the tag:

```sh
git tag v0.20.0
git push origin v0.20.0
```

The pipeline publishes the release without further action.

To run it by hand instead, use the `workflow_dispatch` trigger on the Release
workflow. It accepts a version, optional release notes, and a `replace` flag
for an existing release.

Release notes come from the commits when you supply none.

## What the pipeline gates on

The jobs run in this order:

```
manifest ┐
lint     ┴→ test → build → smoke ┐
                          build → soak ┘→ release-draft → verify → publish-final
```

`manifest` and `lint` both start at once; `test` waits for both. `security`
also starts at once and runs beside the chain — it blocks `verify` only.

| Job | Blocks | Note |
|---|---|---|
| `manifest` | `test` and everything after | `server.json` version must equal the tag |
| `lint` | everything after it | cppcheck, clang-format, NOLINT gate, `server.json` self-consistency |
| `test` | `build` | 5 platforms; the `ubuntu-24.04-arm` leg takes about 60 minutes |
| `build` | `smoke`, `soak` | 7 targets, including Windows and both portable Linux builds |
| `smoke` | `release-draft` | 5 platforms |
| `soak` | `release-draft` | 10 minutes, inside the 30-minute job timeout |
| `security` | `verify` | static scan and the CodeQL gate |
| `verify` | `publish-final` | downloads the published assets and checks them |

`publish-final` removes the draft flag. The release becomes public at that
point.

## Known conditions

The Nightly Soak workflow does not affect a release. It passes
`duration_minutes: 240` into a job whose `timeout-minutes` is 30, so GitHub
cancels it every week. The release path uses 10 minutes and passes.

Nothing in `.github/workflows` or `scripts` READS `server.json` for its own
purposes, so a stale version changes no behaviour and surfaces only when a user
follows a dead asset URL. The file went two releases out of date, then did it
again after this checklist was written — a checklist with no gate behind it is
a suggestion.

It is now gated. `scripts/check-server-json-version.sh` runs in two places:

- the `lint` job, with no argument, on every commit — the `version` field and
  all five asset URLs must agree with each other, catching a half-done bump;
- the release pipeline's `manifest` job, with the release version — they must
  all equal the tag being released.

`manifest` needs nothing, so it starts immediately and `test` waits on it. A
forgotten bump fails the release in under a minute instead of an hour in.
