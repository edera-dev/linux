#!/usr/bin/env bash
# Tests for .github/scripts/verify-upstream-rebase.sh.
#
# Builds a small throwaway repository with an upstream line and a downstream
# series on it, rebases the series onto a newer upstream, and then damages
# copies of that result in each way the checker exists to catch. The nightly
# rebase force-pushes edera/mainline on this script's say-so, so every
# damaged copy must be refused, and every honest replay must pass.
#
# Run from anywhere: bash .github/scripts/tests/verify-upstream-rebase.test.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$HERE/../verify-upstream-rebase.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

failures=0
expect() {
	# expect <description> <exit-status> <old> <new> <upstream> [<grep pattern>]
	local desc=$1 want=$2 got out
	out=$(bash "$SCRIPT" "$3" "$4" "$5")
	got=$?
	if [ "$got" -ne "$want" ]; then
		echo "FAIL $desc: exit $got, want $want" >&2
		printf '     %s\n' "${out//$'\n'/$'\n'     }" >&2
		failures=$((failures + 1))
	elif [ $# -ge 6 ] && ! grep -qF -- "$6" <<<"$out"; then
		echo "FAIL $desc: output lacks '$6'" >&2
		printf '     %s\n' "${out//$'\n'/$'\n'     }" >&2
		failures=$((failures + 1))
	else
		echo "ok   $desc"
	fi
}

git init -q -b upstream .
git config user.name test
git config user.email test@example.invalid
commit() { git add -A && git commit -qm "$1"; }

# Upstream: two files with room for both sides to edit.
seq 1 40 >a.c
seq 1 40 >b.c
commit "upstream: base"
base=$(git rev-parse HEAD)

# Downstream series. Two commits share a subject, as real series do.
git switch -q -c downstream
sed -i 's/^10$/10 downstream/' a.c
commit "downstream: edit a"
sed -i 's/^30$/30 downstream/' b.c
commit "apply dep patch"
echo "new" >c.c
commit "downstream: add c"
sed -i 's/^35$/35 downstream/' b.c
commit "apply dep patch"
old=$(git rev-parse HEAD)

# Upstream moves on in two steps. The first edits only lines far from
# anything downstream touches; the second edits line 12, which sits inside the
# hunk context of "downstream: edit a" (line 10) without conflicting with it.
git switch -q upstream
sed -i 's/^1$/1 upstream/' b.c
commit "upstream: edit b far away"
far=$(git rev-parse HEAD)
sed -i 's/^12$/12 upstream/' a.c
commit "upstream: edit a near downstream"
up=$(git rev-parse HEAD)

replay() { # replay <branch> <onto>
	git switch -q -c "$1" "$old" && git rebase -q --onto "$2" "$base" "$1"
}

replay far "$far"
expect "replay with untouched context passes" 0 "$old" "$(git rev-parse HEAD)" "$far" "identical patch-ids"
expect "a no-op rebase passes" 0 "$old" "$old" "$base" "identical patch-ids"

# Context moved but the commit's own lines did not: still clean.
replay rebased "$up"
good=$(git rev-parse HEAD)
expect "replay with moved context passes" 0 "$old" "$good" "$up" "1 changed only in hunk context"

# Damage 1: one of the two same-subject commits edited.
git switch -q -c tamper "$good"
# shellcheck disable=SC2016 # expanded by the shell git rebase starts
git rebase -q --exec 'if [ "$(git log -1 --format=%s)" = "apply dep patch" ] && grep -q "35 downstream" b.c; then echo extra >>b.c; git commit -qa --amend --no-edit; fi' "$up"
expect "edited commit with a repeated subject fails" 1 "$old" "$(git rev-parse HEAD)" "$up" "1 commit(s) lost, added, or with different +/- lines"

# Damage 2: a commit dropped.
git switch -q -c dropped "$good"
drop=$(git log --format='%H %s' "$up..HEAD" | awk '/downstream: add c/ { print $1 }')
git rebase -q --onto "$drop^" "$drop" dropped
expect "dropped commit fails" 1 "$old" "$(git rev-parse HEAD)" "$up" "downstream: add c"

# Damage 3: a commit added.
git switch -q -c added "$good"
echo "sneaky" >d.c
commit "innocent looking"
expect "added commit fails" 1 "$old" "$(git rev-parse HEAD)" "$up" "innocent looking"

# Damage 4: not on the upstream tip.
expect "stale base is refused" 2 "$old" "$old" "$up" "not based on the upstream tip"

# Damage 5: a merge commit in the series.
git switch -q -c merged "$good"
git switch -q -c side "$up"
echo side >e.c
commit "side"
git switch -q merged
git merge -q --no-edit side
expect "merge in the series is refused" 2 "$old" "$(git rev-parse HEAD)" "$up" "merge commit"

# A series that merged a feature branch, as both kernel trees once did: the
# first rebase flattens it, and that must still pass, since no commit is
# lost and the tree moves by exactly the upstream delta.
git switch -q -c withmerge "$old"
git switch -q -c feature "$old~1"
sed -i 's/^20$/20 feature/' a.c
commit "feature: edit a"
git switch -q withmerge
git merge -q --no-ff --no-edit feature >/dev/null
sed -i 's/^25$/25 after merge/' b.c
commit "downstream: after the merge"
mold=$(git rev-parse HEAD)
git switch -q -c flattened "$mold"
git rebase -q --onto "$up" "$base" flattened
expect "flattening a merged series passes" 0 "$mold" "$(git rev-parse HEAD)" "$up" "All 6 downstream commits replayed"

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)" >&2
	exit 1
fi
echo "all passed"
