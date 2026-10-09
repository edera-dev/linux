#!/usr/bin/env bash
# Works out what a merged edera/mainline pull request still needs on
# edera/6.18-lts, and what it has to wait for first.
#
# Backports to edera/6.18-lts land in the order their commits landed on
# edera/mainline. A pull request is backported only once every Edera commit
# that came before it on mainline, is still missing from the LTS tree, and
# touches a file it touches has been backported (or skipped). Otherwise its
# commits could be applied on top of code they were never written against.
#
# Usage:
#   backport-blockers.sh <pending-file> <request-file> <lts-tip> <lts-upstream> <todo-out>
#
# <pending-file> is what pending-backports.sh prints for the whole of
# edera/mainline: the mainline commits the LTS tree lacks, oldest first.
# <request-file> lists the pull request's own commits the same way ("<sha>
# TAB <subject>", oldest first). A request commit still needs backporting when
# it matches a pending commit, and those are written to <todo-out>:
#
#   - by patch-id first, which is how a rebase-merged commit normally matches
#     its copy on mainline;
#   - otherwise, if the LTS series does not already have its patch-id, by
#     subject, ignoring a trailing version tag. The patch-id check keeps a
#     commit that is already on the LTS tree from claiming a different
#     pending commit that happens to share its subject.
#
# Prints the pending commits the pull request has to wait for, oldest first,
# and nothing when it can go ahead.

set -euo pipefail

if [ $# -ne 5 ] || [ ! -r "$1" ] || [ ! -r "$2" ]; then
	echo "usage: $0 <pending-file> <request-file> <lts-tip> <lts-upstream> <todo-out>" >&2
	exit 2
fi
pending=$1
request=$2
l_tip=$(git rev-parse --verify "$3^{commit}")
l_base=$(git merge-base "$l_tip" "$(git rev-parse --verify "$4^{commit}")")
todo=$5

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

norm() { sed -E 's/[[:space:]]*\((v?[0-9]+\.[0-9]+(\.[0-9y]+)?(-lts)?)\)$//'; }
pid() { git show "$1" | git patch-id --stable | cut -d' ' -f1; }
files() { git show --format= --name-only "$1"; }

git log --no-merges --format='commit %H' --patch "$l_base..$l_tip" |
	git patch-id --stable | cut -d' ' -f1 | sort -u >"$work/lts-pids"

# Pending commits, numbered, with patch-ids.
n=0
while IFS=$'\t' read -r sha subject; do
	n=$((n + 1))
	printf '%s\t%s\t%s\t%s\n' "$n" "$sha" "$subject" "$(pid "$sha")"
done <"$pending" >"$work/pending"

# Each request commit claims the first pending commit it matches that nothing
# else has claimed.
: >"$todo"
: >"$work/claimed"
while IFS=$'\t' read -r sha subject; do
	s=$(printf '%s' "$subject" | norm)
	p=$(pid "$sha")
	by=subject
	grep -qxF -- "$p" "$work/lts-pids" && by=none
	hit=$(awk -F'\t' -v s="$s" -v p="$p" -v by="$by" '
		FILENAME == ARGV[1] { claimed[$0] = 1; next }
		($1 in claimed) { next }
		$4 == p { print $1; found = 1; exit }
		by == "subject" && $3 == s && !first { first = $1 }
		END { if (!found && first) print first }
	' "$work/claimed" "$work/pending")
	if [ -n "$hit" ]; then
		echo "$hit" >>"$work/claimed"
		printf '%s\t%s\n' "$sha" "$s" >>"$todo"
		files "$sha" >>"$work/files"
	fi
done <"$request"
[ -s "$todo" ] || exit 0
sort -u -o "$work/files" "$work/files"
first=$(sort -n "$work/claimed" | head -n1)

awk -F'\t' -v first="$first" '
	FILENAME == ARGV[1] { claimed[$0] = 1; next }
	$1 < first && !($1 in claimed) { print $2 "\t" $3 }
' "$work/claimed" "$work/pending" | while IFS=$'\t' read -r sha subject; do
	if files "$sha" | grep -qxFf "$work/files"; then
		printf '%s\t%s\n' "$sha" "$subject"
	fi
done
