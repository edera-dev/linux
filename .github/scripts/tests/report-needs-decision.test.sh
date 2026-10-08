#!/usr/bin/env bash
# Tests for .github/scripts/report-needs-decision.sh.
#
# A clean nightly rebase is force-pushed to its downstream branch unless this script says
# Claude's report asks for a decision, so a report that does ask must never
# read as one that does not.
#
# Run from anywhere: bash .github/scripts/tests/report-needs-decision.test.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$HERE/../report-needs-decision.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failures=0
expect() {
	# expect <description> <exit-status>   (report on stdin)
	local desc=$1 want=$2 got
	cat >"$WORK/report.md"
	bash "$SCRIPT" "$WORK/report.md" >/dev/null 2>&1
	got=$?
	if [ "$got" -eq "$want" ]; then
		echo "ok   $desc"
	else
		echo "FAIL $desc: exit $got, want $want" >&2
		failures=$((failures + 1))
	fi
}

expect "no section, nothing unsure" 1 <<'EOF'
## Summary
All 221 commits replayed cleanly.

## Conflicts resolved
None.
EOF

expect "section says None." 1 <<'EOF'
## Summary
Clean.

## Needs a decision
None.

## Conflicts resolved
None.
EOF

expect "section empty before the next heading" 1 <<'EOF'
## Needs a decision

## Conflicts resolved
None.
EOF

expect "section with an item" 0 <<'EOF'
## Summary
Clean replay.

## Needs a decision
- Upstream 0822a2e890 now reaches Arm dom0 vPCI; see below.

## Conflicts resolved
None.
EOF

expect "UNSURE outside the section" 0 <<'EOF'
## Summary
Clean replay.

## Conflicts resolved
- foo.c: kept downstream's lock order. **UNSURE** whether upstream intended otherwise.
EOF

expect "heading case and trailing space" 0 <<'EOF'
## NEEDS A DECISION
- something
EOF

expect "a level-3 heading stays inside the section" 0 <<'EOF'
## Needs a decision
### Arm vPCI
- something
## Builds
pass
EOF

expect "a report with one UNSURE item asks for a decision" 0 <<'EOF'
## Summary
Rebased cleanly.

## Needs a decision
- **UNSURE: upstream `0123456789ab` ("xen/events: rework lateeoi handling") now reaches the downstream _on_node bind helpers.**

## Conflicts resolved
None.
EOF

bash "$SCRIPT" "$WORK/missing.md" >/dev/null 2>&1
if [ $? -eq 2 ]; then echo "ok   unreadable report is an error"; else
	echo "FAIL unreadable report is an error" >&2
	failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)" >&2
	exit 1
fi
echo "all passed"
