#!/usr/bin/env bash
# Tests for .github/scripts/pending-backports.sh, backport-blockers.sh and
# verify-backport.sh.
#
# Builds a small throwaway repository with two upstreams (an "lts" line and a
# "mainline" line that has moved on), an Edera series on each, and checks
# that pending-backports.sh finds exactly the mainline commits the LTS series
# lacks, and that backport-blockers.sh holds a pull request back only for
# earlier pending commits that touch its files. It then backports them
# honestly and in each way the checker exists to catch. A backport is only
# proposed on verify-backport.sh's say-so, so every damaged backport must be
# flagged or refused and the honest one must pass.
#
# Run from anywhere: bash .github/scripts/tests/backport.test.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PENDING="$HERE/../pending-backports.sh"
VERIFY="$HERE/../verify-backport.sh"
BLOCKERS="$HERE/../backport-blockers.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/repo" && cd "$WORK/repo" || exit 1

failures=0
ok() { echo "ok   $1"; }
bad() {
	echo "FAIL $1" >&2
	[ -z "${2:-}" ] || printf '     %s\n' "${2//$'\n'/$'\n'     }" >&2
	failures=$((failures + 1))
}

git init -q -b lts-up .
git config user.name test
git config user.email test@example.invalid
commit() { git add -A && git commit -qm "$1"; }

# Upstreams: mainline is the LTS base plus an unrelated change.
seq 1 60 >a.c
seq 1 60 >b.c
commit "upstream: base"
git switch -q -c mainline-up
echo "upstream main" >u.c
commit "upstream: mainline moves on"

# Edera on LTS: one commit both trees share, one ported with a version tag,
# one reworded but identical in content, one LTS-only, and a repeated subject
# once.
git switch -q -c lts lts-up
sed -i 's/^5$/5 shared/' a.c
commit "xen: shared fix"
sed -i 's/^15$/15 ported/' a.c
commit "9p/xen: share one frontend (6.18)"
sed -i 's/^25$/25 reworded/' a.c
commit "xen: lts wording of a fix"
echo lts-only >l.c
commit "drm: lts only"
sed -i 's/^30$/30 dep one/' b.c
commit "apply dep patch"
lts_tip=$(git rev-parse HEAD)

# Edera on mainline: the same, plus three new commits (one with the repeated
# subject, one to be skipped), a merge, and an automation-only commit, which
# is never offered.
git switch -q -c main mainline-up
sed -i 's/^5$/5 shared/' a.c
commit "xen: shared fix"
sed -i 's/^15$/15 ported/' a.c
commit "9p/xen: share one frontend"
sed -i 's/^25$/25 reworded/' a.c
commit "xen: mainline wording of a fix"
sed -i 's/^30$/30 dep one/' b.c
commit "apply dep patch"
git switch -q -c side
sed -i 's/^40$/40 new one/' b.c
commit "xen: new on mainline"
git switch -q main
git merge -q --no-ff --no-edit side >/dev/null
sed -i 's/^50$/50 dep two/' b.c
commit "apply dep patch"
echo mainline-only >m.c
commit "xen: mainline only"
mkdir -p .github/workflows
echo "on: push" >.github/workflows/x.yml
commit "automation: a workflow"
main_tip=$(git rev-parse HEAD)

printf '# never backport\n\n  xen: mainline only  \n' >"$WORK/skip"
want=$(printf '%s\n' "xen: new on mainline" "apply dep patch")
got=$(bash "$PENDING" "$main_tip" mainline-up "$lts_tip" lts-up "$WORK/skip" | cut -f2)
if [ "$got" = "$want" ]; then ok "pending lists only what LTS lacks, oldest first"; else
	bad "pending lists only what LTS lacks, oldest first" "got: $got"; fi

got=$(bash "$PENDING" "$main_tip" mainline-up "$lts_tip" lts-up | cut -f2 | tail -n1)
if [ "$got" = "xen: mainline only" ]; then ok "without a skip file, skipped commits are pending"; else
	bad "without a skip file, skipped commits are pending" "got: $got"; fi

got=$(bash "$PENDING" "$lts_tip" lts-up "$lts_tip" lts-up)
if [ -z "$got" ]; then ok "a tree against itself has nothing pending"; else
	bad "a tree against itself has nothing pending" "got: $got"; fi

# Blockers. A "pull request" here is one mainline commit, as the request
# file lists it.
bash "$PENDING" "$main_tip" mainline-up "$lts_tip" lts-up >"$WORK/pending-all"
nth() { git log --reverse --format=%H --grep="^$1\$" "$main_tip" | sed -n "${2:-1}p"; }
pr() { for c in "$@"; do printf '%s\t%s\n' "$c" "$(git log -1 --format=%s "$c")"; done >"$WORK/pr"; }
blockers() { bash "$BLOCKERS" "$WORK/pending-all" "$WORK/pr" "$lts_tip" lts-up "$WORK/todo" | cut -f2; }
check() { # check <description> <want blockers> <want todo count>
	local got
	got=$(blockers)
	if [ "$got" != "$2" ]; then
		bad "$1" "blockers: $got"
	elif [ "$(grep -c . "$WORK/todo")" -ne "$3" ]; then
		bad "$1" "todo: $(cat "$WORK/todo")"
	else
		ok "$1"
	fi
}

pr "$(nth 'apply dep patch' 2)"
check "an earlier pending commit on the same file blocks" "xen: new on mainline" 1

pr "$(nth 'apply dep patch' 1)"
check "a commit already on LTS has nothing to do, even with a repeated subject" "" 0

pr "$(nth 'xen: new on mainline')"
check "the oldest pending commit is not blocked" "" 1

pr "$(nth 'xen: new on mainline')" "$(nth 'apply dep patch' 2)"
check "a pull request does not wait for its own commits" "" 2

pr "$(nth 'xen: mainline only')"
check "earlier pending commits on other files do not block" "" 1

pr "$(nth 'automation: a workflow')"
check "an automation-only pull request has nothing to do" "" 0

printf 'xen: new on mainline\n' >"$WORK/skip2"
bash "$PENDING" "$main_tip" mainline-up "$lts_tip" lts-up "$WORK/skip2" >"$WORK/pending-all"
pr "$(nth 'apply dep patch' 2)"
check "a skipped commit does not block" "" 1

bash "$PENDING" "$main_tip" mainline-up "$lts_tip" lts-up "$WORK/skip" >"$WORK/request"

expect() {
	# expect <description> <exit-status> <new-tip> [<grep pattern>]
	local desc=$1 want=$2 got out
	out=$(bash "$VERIFY" "$lts_tip" "$3" "$WORK/request")
	got=$?
	if [ "$got" -ne "$want" ]; then
		bad "$desc: exit $got, want $want" "$out"
	elif [ $# -ge 4 ] && ! grep -qF -- "$4" <<<"$out"; then
		bad "$desc: output lacks '$4'" "$out"
	else
		ok "$desc"
	fi
}

pick_all() { # pick_all <branch>
	git switch -q -c "$1" "$lts_tip"
	cut -f1 "$WORK/request" | while read -r c; do git cherry-pick -x "$c" >/dev/null; done
}

pick_all good
good=$(git rev-parse HEAD)
expect "honest backport passes" 0 "$good" "All 2 requested commits backported"

got=$(bash "$PENDING" "$main_tip" mainline-up "$good" lts-up "$WORK/skip")
if [ -z "$got" ]; then ok "after the backport nothing is pending"; else
	bad "after the backport nothing is pending" "got: $got"; fi

# Adapted: the backport changes different lines from its source.
git switch -q -c adapted "$good"
echo extra >>b.c
git commit -qa --amend --no-edit
expect "adapted commit needs review" 1 "$(git rev-parse HEAD)" "| adapted |"

# Retitled: same change, new subject, which pending would not recognise.
git switch -q -c retitled "$good"
git commit -q --amend -m "apply a dependency patch

(cherry picked from commit $(sed -n 2p "$WORK/request" | cut -f1))"
expect "retitled commit needs review" 1 "$(git rev-parse HEAD)" "| retitled |"

# Left out: only the first requested commit.
git switch -q -c leftout "$good~1"
expect "left-out commit needs review" 1 "$(git rev-parse HEAD)" "| left-out |"

# Added: an extra commit with no trailer.
git switch -q -c added "$good"
echo sneaky >s.c
commit "innocent looking"
expect "added commit needs review" 1 "$(git rev-parse HEAD)" "| added |"

# Out of order.
git switch -q -c reordered "$lts_tip"
git cherry-pick -x "$(sed -n 2p "$WORK/request" | cut -f1)" >/dev/null
git cherry-pick -x "$(sed -n 1p "$WORK/request" | cut -f1)" >/dev/null
expect "out-of-order backport is refused" 2 "$(git rev-parse HEAD)" "| stray |"

# A trailer naming a commit nobody asked for.
git switch -q -c stray "$good"
echo x >x.c
git add x.c
git commit -qm "xen: mainline only

(cherry picked from commit $main_tip)"
expect "unrequested source is refused" 2 "$(git rev-parse HEAD)" "| stray |"

# Not a fast-forward of the LTS tip.
git switch -q -c rewritten "$lts_tip~1"
cut -f1 "$WORK/request" | while read -r c; do git cherry-pick -x "$c" >/dev/null; done
expect "rewritten LTS history is refused" 2 "$(git rev-parse HEAD)" "does not contain the LTS tip"

# A merge on top.
git switch -q -c merged "$good"
git merge -q --no-ff --no-edit side >/dev/null
expect "merge on top is refused" 2 "$(git rev-parse HEAD)" "merge commit"

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)" >&2
	exit 1
fi
echo "all passed"
