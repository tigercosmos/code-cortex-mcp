#!/usr/bin/env bash
# List upstream commits newer than the last-synced marker that this fork does
# NOT already have. Dedups by commit *subject* — SHAs diverge across the fork,
# so range membership alone is not evidence a commit is unported.
#
# Usage: .claude/skills/sync-upstream/list-new.sh [marker]
#   marker defaults to the "Upstream commit" SHA in UPSTREAM_SYNC.md.
set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

marker=${1:-}
if [ -z "$marker" ]; then
  marker=$(grep -m1 '\*\*Upstream commit\*\*' UPSTREAM_SYNC.md \
    | grep -o '`[0-9a-f]\{7,40\}`' | head -1 | tr -d '`')
fi
if [ -z "$marker" ]; then
  echo "could not determine marker; pass one explicitly" >&2
  exit 1
fi

git fetch upstream main --quiet

total=$(git rev-list --count "$marker"..upstream/main)
echo "marker: $marker   range: $marker..upstream/main   ($total commits)"
echo

# Subjects already present anywhere in this fork's history.
ours=$(git log --format='%s' main)

new=0
while IFS=$'\t' read -r sha subject; do
  if printf '%s\n' "$ours" | grep -qxF "$subject"; then
    continue
  fi
  new=$((new + 1))
  printf '%s\t%s\n' "$sha" "$subject"
done < <(git log --reverse --format='%h%x09%s' "$marker"..upstream/main)

echo
echo "$new genuinely-new commits (of $total in range)"
echo
echo "Next: triage by message, NOT by diff —"
echo "  git log --format='%h %s%n%b%n---' $marker..upstream/main"
echo "  git show --stat <sha>"
