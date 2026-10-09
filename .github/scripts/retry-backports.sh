#!/usr/bin/env bash
# Dispatches backports of merged edera/mainline pull requests again.
#
#   - Every mainline pull request labelled backport-waiting: something it was
#     waiting for may have landed on edera/6.18-lts since.
#   - With --stale, also every open backport pull request GitHub can no
#     longer merge: a rebase of edera/6.18-lts rewrote the history it was
#     built on, so it is prepared again from the new tip.
#
# Each one is a run of kernel-nightly.yml on edera/6.18-lts with backport_pr
# set (lts-backport.yml). Needs GH_TOKEN with actions write and pull requests
# read, and GITHUB_REPOSITORY.
#
# Usage: retry-backports.sh [--stale]

set -euo pipefail

SOURCE=edera/mainline
TARGET=edera/6.18-lts
PREFIX=backport/edera-6.18-lts/pr-
R=$GITHUB_REPOSITORY

{
	gh pr list --repo "$R" --base "$SOURCE" --state merged --label backport-waiting \
		--limit 200 --json number -q '.[].number'
	if [ "${1:-}" = --stale ]; then
		gh pr list --repo "$R" --base "$TARGET" --state open --limit 200 \
			--json headRefName,mergeable \
			-q ".[] | select(.headRefName | startswith(\"$PREFIX\")) | select(.mergeable == \"CONFLICTING\") | .headRefName" |
			sed "s|^$PREFIX||"
	fi
} | grep -xE '[0-9]+' | sort -un | while read -r n; do
	echo "Dispatching the backport of #$n"
	gh workflow run kernel-nightly.yml --repo "$R" --ref "$TARGET" -f backport_pr="$n"
done
