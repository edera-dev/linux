#!/usr/bin/env bash
# Checks a backport of edera/mainline commits onto edera/6.18-lts, without
# trusting whoever did it.
#
# The backport of a merged edera/mainline pull request lets a model
# cherry-pick its commits, but nothing it says about its own result is taken
# on faith. This script is the gate. It proves, from git alone, that:
#
#   1. the new branch fast-forwards the LTS tip the backport started from;
#   2. the commits on top are linear (no merge commits);
#   3. every one of them is a backport of a requested commit, in the requested
#      order, each named by its "(cherry picked from commit <sha>)" trailer,
#      with nothing extra in between and nothing requested left out;
#   4. each keeps its source's subject, so pending-backports.sh recognises it
#      next time instead of offering it again;
#   5. each changes the same files by the same +/- lines as its source. Hunk
#      offsets and context may differ; the change itself may not.
#
# A commit that had to be adapted to the older kernel fails check 5. That is
# the point: such a commit is not wrong, but the pull request has to say so,
# so its reviewer knows to read it closely.
#
# Usage:
#   verify-backport.sh <old-lts-tip> <new-tip> <requested-file>
#
# <requested-file> holds the commits asked for, as pending-backports.sh prints
# them: "<sha> TAB <subject>", oldest first.
#
# Writes a Markdown report to stdout. Exit status:
#   0  clean: every requested commit backported unchanged
#   1  differences: well-formed (1-3 hold for what is there) but a commit was
#      adapted, retitled, left out, or added; flagged in the pull request
#   2  broken: 1 or 2 failed, a trailer names something not requested or out
#      of order, or the inputs are unusable; not proposed at all

set -euo pipefail

if [ $# -ne 3 ] || [ ! -r "$3" ]; then
	echo "usage: $0 <old-lts-tip> <new-tip> <requested-file>" >&2
	exit 2
fi

old=$(git rev-parse --verify "$1^{commit}")
new=$(git rev-parse --verify "$2^{commit}")
requested=$3

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

status=0
fail() { [ "$status" -ge "$1" ] || status=$1; }
short() { git rev-parse --short=12 "$1"; }
oneline() { git log -1 --format='%h %s' "$1"; }
# The subject without a trailing version tag such as " (6.18)", as
# pending-backports.sh compares them.
subject() { git log -1 --format=%s "$1" | sed -E 's/[[:space:]]*\((v?[0-9]+\.[0-9]+(\.[0-9y]+)?(-lts)?)\)$//'; }
own_change() {
	git show --format= "$1" | grep -E '^([+-]|diff )' | grep -vE '^(\+\+\+|---) ' || true
}

echo "| | |"
echo "| --- | --- |"
echo "| LTS tip before | \`$(short "$old")\` |"
echo "| LTS tip after | \`$(short "$new")\` |"
echo "| Commits requested | $(grep -c . "$requested" || true) |"
echo

# 1. Fast-forward.
if git merge-base --is-ancestor "$old" "$new"; then
	echo "- [x] New branch fast-forwards the LTS tip."
else
	echo "- [ ] **New branch does not contain the LTS tip** \`$(short "$old")\`."
	fail 2
fi

# 2. Linearity.
if [ "$status" -lt 2 ]; then
	merges=$(git rev-list --merges "$old..$new" | wc -l)
	if [ "$merges" -eq 0 ]; then
		echo "- [x] Backported commits are linear."
	else
		echo "- [ ] **$merges merge commit(s) on top of the LTS tip.**"
		fail 2
	fi
fi

if [ "$status" -ge 2 ]; then
	echo
	echo "Stopped: the remaining checks assume a linear series on the LTS tip."
	exit "$status"
fi

# 3-5. Walk the new commits and the request list together. A commit names its
# source by trailer; it must be the next requested commit, or a later one (the
# ones skipped over were left out).
cut -f1 "$requested" | while read -r sha; do git rev-parse --verify "$sha^{commit}"; done >"$work/want"
git rev-list --reverse "$old..$new" >"$work/got"

: >"$work/rows"
next=1
n_want=$(wc -l <"$work/want")
while read -r c; do
	src=$(git log -1 --format=%B "$c" |
		sed -nE 's/^\(cherry picked from commit ([0-9a-f]{40})\)$/\1/p' | tail -n1)
	if [ -z "$src" ]; then
		echo "added	$c	-" >>"$work/rows"
		fail 1
		continue
	fi
	pos=$(grep -nxF "$src" "$work/want" | cut -d: -f1 | head -n1 || true)
	if [ -z "$pos" ] || [ "$pos" -lt "$next" ]; then
		echo "stray	$c	$src" >>"$work/rows"
		fail 2
		continue
	fi
	while [ "$next" -lt "$pos" ]; do
		echo "left-out	-	$(sed -n "${next}p" "$work/want")" >>"$work/rows"
		fail 1
		next=$((next + 1))
	done
	next=$((pos + 1))
	if [ "$(subject "$c")" != "$(subject "$src")" ]; then
		echo "retitled	$c	$src" >>"$work/rows"
		fail 1
	elif [ "$(own_change "$c")" != "$(own_change "$src")" ]; then
		echo "adapted	$c	$src" >>"$work/rows"
		fail 1
	else
		echo "same	$c	$src" >>"$work/rows"
	fi
done <"$work/got"
while [ "$next" -le "$n_want" ]; do
	echo "left-out	-	$(sed -n "${next}p" "$work/want")" >>"$work/rows"
	fail 1
	next=$((next + 1))
done

count() { awk -F'\t' -v k="$1" '$1 == k' "$work/rows" | wc -l; }
n_same=$(count same)
if [ "$n_same" -eq "$n_want" ] && [ "$(wc -l <"$work/got")" -eq "$n_want" ]; then
	echo "- [x] All $n_want requested commits backported in order, each with its source's subject and identical +/- lines."
else
	echo "- [ ] **Backport differs from the request:** $n_same of $n_want unchanged;" \
		"$(count adapted) adapted, $(count retitled) retitled, $(count left-out) left out," \
		"$(count added) added, $(count stray) not requested or out of order."
fi

if [ "$n_same" -ne "$(wc -l <"$work/rows")" ]; then
	echo
	echo "| | Backport | Mainline source |"
	echo "| --- | --- | --- |"
	while IFS=$'\t' read -r verdict c src; do
		[ "$verdict" = same ] && continue
		a='(none)'; b='(none)'
		[ "$c" = - ] || a=$(oneline "$c")
		[ "$src" = - ] || b=$(oneline "$src")
		echo "| $verdict | \`$a\` | \`$b\` |"
	done <"$work/rows"
fi

if grep -q '^adapted' "$work/rows"; then
	echo
	echo "<details><summary>Adapted commits (source +/- lines vs. backport)</summary>"
	echo
	echo '```diff'
	awk -F'\t' '$1 == "adapted" { print $2 "\t" $3 }' "$work/rows" |
		while IFS=$'\t' read -r c src; do
			echo "### $(oneline "$c")"
			diff -u --label mainline --label backport \
				<(own_change "$src") <(own_change "$c") || true
		done | head -n 400
	echo '```'
	echo
	echo "</details>"
fi

exit "$status"
