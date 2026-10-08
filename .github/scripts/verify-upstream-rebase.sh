#!/usr/bin/env bash
# Checks a rebased downstream branch against the branch it replaces, without
# trusting whoever did the rebase.
#
# The nightly upstream-rebase workflow lets a model drive the rebase, but
# nothing it says about its own result is taken on faith. This script is the
# gate. It proves, from git alone, that:
#
#   1. the new branch sits directly on the upstream tip it claims to;
#   2. the downstream series is linear (no merge commits slipped in);
#   3. no downstream commit was lost, added, or changed: patch-ids match, or,
#      where they do not, the commit's own +/- lines are identical and only a
#      hunk's context anchor moved;
#   4. the tree changed by exactly the upstream delta and nothing else (only
#      the +/- lines are compared, so moved hunks and blob hashes do not raise
#      false alarms).
#
# A rebase that needed conflict resolution will normally fail check 4: the
# resolution is, by definition, a change to downstream lines. That is the
# point. Such a result is not wrong, but it is not "clean", and a human has to
# read it before it replaces the branch.
#
# Usage:
#   verify-upstream-rebase.sh <old-tip> <new-tip> <upstream-tip>
#
# Writes a Markdown report to stdout. Exit status:
#   0  clean: all four checks pass
#   1  drift: the branch is well-formed (1-2) but 3 or 4 found differences
#   2  broken: 1 or 2 failed, or the inputs are unusable

set -euo pipefail

if [ $# -ne 3 ]; then
	echo "usage: $0 <old-tip> <new-tip> <upstream-tip>" >&2
	exit 2
fi

old=$(git rev-parse --verify "$1^{commit}")
new=$(git rev-parse --verify "$2^{commit}")
up=$(git rev-parse --verify "$3^{commit}")
old_base=$(git merge-base "$old" "$up")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

status=0
fail() { [ "$status" -ge "$1" ] || status=$1; }
short() { git rev-parse --short=12 "$1"; }
changed_lines() { grep -E '^[+-]' | grep -vE '^(\+\+\+|---)' || true; }
changed_files() { grep -E '^(\+\+\+|---) ' || true; }

echo "| | |"
echo "| --- | --- |"
echo "| Old tip | \`$(short "$old")\` |"
echo "| New tip | \`$(short "$new")\` |"
echo "| Old upstream base | \`$(short "$old_base")\` |"
echo "| New upstream base | \`$(short "$up")\` |"
echo

# 1. Base.
if [ "$(git merge-base "$new" "$up")" = "$up" ]; then
	echo "- [x] New branch is based on the upstream tip."
else
	echo "- [ ] **New branch is not based on the upstream tip** \`$(short "$up")\`."
	fail 2
fi

# 2. Linearity.
merges=$(git rev-list --merges "$up..$new" | wc -l)
if [ "$merges" -eq 0 ]; then
	echo "- [x] Downstream series is linear."
else
	echo "- [ ] **Downstream series contains $merges merge commit(s).**"
	fail 2
fi

if [ "$status" -ge 2 ]; then
	echo
	echo "Stopped: the remaining checks assume a linear series on the upstream tip."
	exit "$status"
fi

# 3. Commit set.
#
# Each commit is keyed by its subject and how many earlier commits in the
# series share that subject, so the k-th "apply dep patch" before pairs with
# the k-th after. A rebase keeps the series order, so this pairs every commit
# with its own replay even when subjects repeat.
#
# A changed patch-id is fine when the commit's own +/- lines are unchanged and
# only a hunk's context anchor moved, which is what happens wherever upstream
# edited the same region. Anything else is a real difference.
series() {
	git log --no-merges --reverse --format='commit %H' --patch "$1" |
		git patch-id --stable | awk '{ print $2 "\t" $1 }' >"$work/pid.map"
	git log --no-merges --reverse --format='%H%x09%s' "$1" |
		awk -F'\t' 'NR == FNR { pid[$1] = $2; next }
			{ k = $2 "#" ++seen[$2]; print k "\t" $1 "\t" ($1 in pid ? pid[$1] : "-") }' \
			"$work/pid.map" -
}
series "$old_base..$old" >"$work/old.series"
series "$up..$new" >"$work/new.series"
n_old=$(wc -l <"$work/old.series")
n_new=$(wc -l <"$work/new.series")

# key -> "old-commit new-commit verdict"; verdict is same, check, lost, added.
awk -F'\t' 'NR == FNR { oc[$1] = $2; op[$1] = $3; order[++n] = $1; next }
	{
		if ($1 in oc) { print oc[$1], $2, (op[$1] == $3 ? "same" : "check"); delete oc[$1] }
		else print "-", $2, "added"
	}
	END { for (i = 1; i <= n; i++) if (order[i] in oc) print oc[order[i]], "-", "lost" }' \
	"$work/old.series" "$work/new.series" >"$work/pairs"

own_change() {
	local patch
	patch=$(git show --format= "$1")
	changed_files <<<"$patch"
	changed_lines <<<"$patch"
}
: >"$work/moved"
: >"$work/changed"
while read -r c d verdict; do
	case $verdict in
	same) ;;
	check)
		if [ "$(own_change "$c")" = "$(own_change "$d")" ]; then
			echo "$c $d" >>"$work/moved"
		else
			echo "$c $d" >>"$work/changed"
		fi ;;
	*) echo "$c $d" >>"$work/changed" ;;
	esac
done <"$work/pairs"

if [ "$n_old" -eq "$n_new" ] && [ ! -s "$work/changed" ]; then
	if [ -s "$work/moved" ]; then
		echo "- [x] All $n_new downstream commits replayed;" \
			"$(wc -l <"$work/moved") changed only in hunk context (listed below)."
	else
		echo "- [x] All $n_new downstream commits replayed with identical patch-ids."
	fi
else
	echo "- [ ] **Downstream commit set changed:** $n_old before, $n_new after;" \
		"$(wc -l <"$work/changed") commit(s) lost, added, or with different +/- lines."
	fail 1
fi

# 4. Drift.
git diff "$old..$new" >"$work/rebase.diff"
git diff "$old_base..$up" >"$work/upstream.diff"
changed_files <"$work/rebase.diff" >"$work/rebase.files"
changed_files <"$work/upstream.diff" >"$work/upstream.files"
changed_lines <"$work/rebase.diff" >"$work/rebase.lines"
changed_lines <"$work/upstream.diff" >"$work/upstream.lines"

if cmp -s "$work/rebase.files" "$work/upstream.files" &&
	cmp -s "$work/rebase.lines" "$work/upstream.lines"; then
	echo "- [x] Zero downstream drift: the tree changed by exactly the upstream delta."
else
	echo "- [ ] **Downstream drift:** the tree changed by more than the upstream delta."
	fail 1
fi

# Details for anything that did not match.
oneline() { if [ "$1" = - ]; then echo "(none)"; else git log -1 --format='%h %s' "$1"; fi; }
if [ -s "$work/moved" ]; then
	echo
	echo "<details><summary>Commits whose hunk context moved (own +/- lines identical)</summary>"
	echo
	while read -r c d; do
		echo "- \`$(oneline "$c")\` → \`$(git rev-parse --short "$d")\`"
	done <"$work/moved"
	echo
	echo "</details>"
fi
if [ -s "$work/changed" ]; then
	echo
	echo "<details><summary>Commits lost, added, or whose own +/- lines changed</summary>"
	echo
	echo "| Before | After |"
	echo "| --- | --- |"
	while read -r c d; do
		echo "| \`$(oneline "$c")\` | \`$(oneline "$d")\` |"
	done <"$work/changed"
	echo
	echo "</details>"
fi

if ! cmp -s "$work/rebase.lines" "$work/upstream.lines" ||
	! cmp -s "$work/rebase.files" "$work/upstream.files"; then
	echo
	echo "<details><summary>Drift (changed lines: upstream delta vs. actual change)</summary>"
	echo
	echo '```diff'
	{
		diff -u --label upstream-files --label actual-files \
			"$work/upstream.files" "$work/rebase.files" || true
		diff -u --label upstream-lines --label actual-lines \
			"$work/upstream.lines" "$work/rebase.lines" || true
	} | head -n 400
	echo '```'
	echo
	echo "</details>"
fi

exit "$status"
