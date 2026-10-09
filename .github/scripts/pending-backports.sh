#!/usr/bin/env bash
# Lists the Edera commits on edera/mainline that edera/6.18-lts does not have.
#
# Edera kernel work lands on edera/mainline first and flows down to
# edera/6.18-lts. Both trees are rebased onto their upstream nightly, so commit
# hashes are no use for telling which commits already made it across. A commit
# counts as present on the LTS tree when its series there has:
#
#   - a commit with the same subject, ignoring a trailing version tag such as
#     " (6.18)" that a port may add (the k-th mainline commit with a subject
#     pairs with the k-th LTS one, so repeated subjects are counted, not
#     merged); or
#   - a commit with the same patch-id, which catches a port whose subject was
#     reworded but whose change was not.
#
# A commit that must never reach the LTS tree (it fixes code only mainline
# has, say) is listed by subject in the skip file, one per line (the
# backport workflow adds the commits of mainline pull requests labelled
# no-backport); blank lines
# and lines starting with # are ignored. Commits that change only .github/ or
# .automation/ are never offered: both trees carry the automation, it is
# landed on each by hand, and the backport is not allowed to touch it.
#
# Usage:
#   pending-backports.sh <mainline-tip> <mainline-upstream> <lts-tip> <lts-upstream> [<skip-file>]
#
# Prints "<sha> TAB <subject>" for each pending commit, oldest first, and
# nothing when the trees agree. Merge commits are ignored on both sides: their
# content is in the commits they merged.

set -euo pipefail

if [ $# -lt 4 ] || [ $# -gt 5 ]; then
	echo "usage: $0 <mainline-tip> <mainline-upstream> <lts-tip> <lts-upstream> [<skip-file>]" >&2
	exit 2
fi

m_tip=$(git rev-parse --verify "$1^{commit}")
m_base=$(git merge-base "$m_tip" "$(git rev-parse --verify "$2^{commit}")")
l_tip=$(git rev-parse --verify "$3^{commit}")
l_base=$(git merge-base "$l_tip" "$(git rev-parse --verify "$4^{commit}")")
skip=${5:-/dev/null}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# <sha> TAB <normalized subject> TAB <patch-id>, oldest first.
series() {
	git log --no-merges --reverse --format='commit %H' --patch "$1" |
		git patch-id --stable | awk '{ print $2 "\t" $1 }' >"$work/pid.map"
	git log --no-merges --reverse --format='%H%x09%s' "$1" |
		awk -F'\t' 'NR == FNR { pid[$1] = $2; next }
			{
				s = $2
				sub(/[ \t]*\((v?[0-9]+\.[0-9]+(\.[0-9y]+)?(-lts)?)\)$/, "", s)
				print $1 "\t" s "\t" ($1 in pid ? pid[$1] : "-")
			}' "$work/pid.map" -
}
series "$m_base..$m_tip" >"$work/mainline"
series "$l_base..$l_tip" >"$work/lts"
# Mainline commits whose every changed path is automation.
git log --no-merges --format='commit %H' --name-only "$m_base..$m_tip" |
	awk '/^commit / { if (c != "" && only) print c; c = $2; only = 0; seen = 0; next }
		NF { if (!seen) only = 1; seen = 1; if ($0 !~ /^\.(github|automation)\//) only = 0 }
		END { if (c != "" && only) print c }' >"$work/automation"
grep -vE '^[[:space:]]*(#|$)' "$skip" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g' >"$work/skip" || true

awk -F'\t' '
	FILENAME == ARGV[1] { auto[$0] = 1; next }
	FILENAME == ARGV[2] { skip[$0] = 1; next }
	FILENAME == ARGV[3] { have[$2]++; if ($3 != "-") bypid[$3] = $2; next }
	{
		if (($1 in auto) || ($2 in skip)) next
		# A patch-id match uses up the LTS commit it matched, under the
		# subject that commit has there, so it cannot also pair with another.
		if ($3 in bypid) { have[bypid[$3]]--; delete bypid[$3]; next }
		if (have[$2] > 0) { have[$2]--; next }
		print $1 "\t" $2
	}
' "$work/automation" "$work/skip" "$work/lts" "$work/mainline"
