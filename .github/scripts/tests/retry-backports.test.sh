#!/usr/bin/env bash
# Tests for .github/scripts/retry-backports.sh against a fake gh.
#
# The fake answers the two pull request listings from WAITING and STALE and
# records every dispatch, so each case checks exactly which backports the
# script started, and that having nothing to retry is a success.
#
# Run from anywhere: bash .github/scripts/tests/retry-backports.test.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RETRY="$HERE/../retry-backports.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/bin"
cat >"$WORK/bin/gh" <<'GH'
#!/usr/bin/env bash
case "$1 $2" in
"pr list")
	case " $* " in
	*" --label backport-waiting "*) [ -z "$WAITING" ] || printf '%s\n' $WAITING ;;
	*) [ -z "$STALE" ] || printf '%s\n' $STALE ;;
	esac ;;
"workflow run") echo "$*" >>"$DISPATCHED" ;;
*) echo "unexpected: gh $*" >&2; exit 2 ;;
esac
GH
chmod +x "$WORK/bin/gh"
export PATH="$WORK/bin:$PATH" GITHUB_REPOSITORY=edera-dev/linux DISPATCHED="$WORK/dispatched"

failures=0
ok() { echo "ok   $1"; }
bad() {
	echo "FAIL $1" >&2
	[ -z "${2:-}" ] || printf '     %s\n' "${2//$'\n'/$'\n'     }" >&2
	failures=$((failures + 1))
}

# check <name> <args> <waiting> <stale> <expected pr numbers, space separated>
check() {
	local name=$1 args=$2 out got
	: >"$DISPATCHED"
	# shellcheck disable=SC2086 # $args is zero or one word
	if ! out=$(WAITING=$3 STALE=$4 bash "$RETRY" $args 2>&1); then
		bad "$name" "exited non-zero: $out"
		return
	fi
	got=$(sed -E 's/.*backport_pr=([0-9]+).*/\1/' "$DISPATCHED" | xargs)
	if [ "$got" = "$5" ]; then ok "$name"; else bad "$name" "dispatched '$got', expected '$5'"; fi
}

check "nothing waiting, no --stale" "" "" "" ""
check "nothing waiting, nothing stale" --stale "" "" ""
check "waiting pull requests are dispatched" "" "7 12" "" "7 12"
check "stale backports are dispatched by number" --stale "" \
	"backport/edera-6.18-lts/pr-23 backport/edera-6.18-lts/pr-9" "9 23"
check "stale backports are ignored without --stale" "" "" "backport/edera-6.18-lts/pr-23" ""
check "a pull request both waiting and stale goes once" --stale "23" \
	"backport/edera-6.18-lts/pr-23" "23"

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)" >&2
	exit 1
fi
echo "all passed"
