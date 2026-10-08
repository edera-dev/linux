#!/usr/bin/env bash
# Decides whether the nightly rebase report asks a human for a decision.
#
# The upstream-rebase skill tells Claude to list anything it was unsure about
# under a "## Needs a decision" section, each item marked UNSURE. A rebase can
# be clean by every mechanical measure (no drift, builds pass) and still raise
# such a question: an upstream change that interacts with a downstream feature
# without touching the same lines. The workflow does not push those
# automatically; it opens a pull request so a maintainer sees the question
# before the branch moves.
#
# The report counts as asking for a decision when either:
#   - its "Needs a decision" section has any content other than "None.", or
#   - any line marks something UNSURE.
# Both err towards a pull request: a stray "UNSURE" costs one review, a missed
# question costs an unreviewed kernel change.
#
# Usage:
#   report-needs-decision.sh <report.md>
#
# Prints the section's content when it asks for a decision. Exit status:
#   0  a decision is needed
#   1  no decision is needed
#   2  the report cannot be read

set -euo pipefail

if [ $# -ne 1 ] || [ ! -r "$1" ]; then
	echo "usage: $0 <report.md>" >&2
	exit 2
fi

awk '
	BEGIN { insec = 0; body = ""; unsure = 0 }
	/UNSURE/ { unsure = 1 }
	/^##?[ \t]/ {
		if (tolower($0) ~ /^##?[ \t]+needs a decision[ \t]*$/) { insec = 1; next }
		insec = 0
	}
	insec {
		line = $0
		gsub(/^[ \t]+|[ \t]+$/, "", line)
		if (line == "") next
		if (tolower(line) ~ /^(_?none\.?_?|n\/a\.?)$/) next
		body = body $0 "\n"
	}
	END {
		if (body != "" || unsure) { printf "%s", body; exit 0 }
		exit 1
	}
' "$1"
