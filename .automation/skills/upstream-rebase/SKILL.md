---
name: upstream-rebase
description: Nightly rebase of edera/mainline onto torvalds master, run unattended in CI by .github/workflows/upstream-rebase.yml. Covers the replay, conflict resolution, build checks, the audit of upstream fixes that meet the series, and the report the workflow publishes.
---

# Nightly upstream rebase

You are rebasing the Edera downstream kernel series (around 80 commits:
Hyper-V-nested-on-Xen, the Xen PV-IOMMU, NUMA-aware Xen backends, 2 MiB
ballooning, OpenPaX, and assorted fixes) on `edera/mainline` onto Linus'
tree. The goal is a replay that changes **nothing downstream**: every
difference in the final tree must come from the upstream delta alone. Where
that is impossible, because upstream and downstream touched the same code, you
resolve the conflict the way a careful kernel maintainer would and you say
exactly what you did.

This runs unattended. Nobody is watching while you work, and nobody can answer
a question. Everything a reviewer needs has to end up in the report.

## What happens to your result

You cannot push, and nothing you write can cause the branch to be pushed.
Your report can only stop it. When you finish, the workflow independently:

1. runs `verify-upstream-rebase.sh` against your branch, which checks from git
   alone that the series sits on the upstream tip, is linear, lost or gained
   no commit, and changed the tree by exactly the upstream delta;
2. builds x86_64 and arm64 `vmlinux` with `kernel-build.sh`, which turns on
   every downstream feature as a built-in.

If all of that passes **and your report's "Needs a decision" section is
`None.` with nothing marked UNSURE anywhere** (checked by
`report-needs-decision.sh`), the downstream branch is force-pushed to your
branch. Otherwise your branch goes up as a pull request with your report as
the body, and a maintainer reads it.

So a question you raise is never lost to an automatic push. Raise one whenever
a maintainer should see something before the branch moves, even when the
rebase itself is clean, for example an upstream change that reaches a
downstream feature without touching the same lines. Do not use the word UNSURE
for anything else.

So: a conflict you resolve **will** show up as drift and **will** go to a
human. That is the intended outcome, not a failure. Do not try to make a
resolution look clean, and never drop, squash, reorder or reword a downstream
commit to get past the checks. A rebase that reaches review with an honest
report is a success; one that hides a judgement call is not.

## Inputs

The workflow gives you, in the prompt:

- `DOWNSTREAM`: the branch being rebased, `edera/mainline`.
- `OLD_TIP`: its current commit (already checked out).
- `UPSTREAM`: the upstream commit to rebase onto, fetched as the local branch
  `upstream-target`.
- `RESULT`: the local branch name your result must end up on.
- `REPORT`: the path your report must be written to.

The automation's own scripts are in `.rebase-tools/scripts/`, copied from the
default branch. Use those, never a copy inside the tree you are rebasing: the
tree is what is under test.

### How to run commands here

Your shell commands are checked against an allowlist that matches the
command's first word. Shell variables, `$(...)`, `<(...)` and `VAR=x cmd`
prefixes will be refused, so in every snippet below `OLD_TIP`, `UPSTREAM`,
`RESULT`, `MB` and `<...>` are placeholders: substitute the literal SHA or
path, or a path you choose. Put scratch files, worktrees and build output
under `.rebase-scratch/` in the repository root (`mkdir -p .rebase-scratch`);
the kernel's `.gitignore` ignores it and nothing reads it after you.

This is a blobless clone: file contents are fetched from GitHub the first time
a command needs them, so the first `git log -p` or checkout over a range is
slower than you expect. That is not a hang.

## 1. Survey

```sh
git merge-base OLD_TIP UPSTREAM               # call the result MB
git rev-list --count --no-merges MB..OLD_TIP  # downstream commits
git log --oneline --no-merges MB..UPSTREAM    # what is new upstream
```

The upstream range is large (a few days of mainline, far more during a merge
window), so do not read it all. Find where it meets the
series:

```sh
git diff --name-only --output=.rebase-scratch/down.files MB..OLD_TIP
git diff --name-only --output=.rebase-scratch/up.files MB..UPSTREAM
comm -12 .rebase-scratch/down.files .rebase-scratch/up.files   # both sorted already
git log --oneline --no-merges MB..UPSTREAM -- <each shared path>
```

Read the messages and diffs of those upstream commits. They are where a
textually clean replay can still be semantically wrong: a helper downstream
calls that changed its locking or return convention, a struct field that
moved, a Kconfig symbol that was renamed. Note them; you come back to them in
step 4.

The series may contain a merge commit from an old pull request. Rebasing
flattens it into its commits, which is expected and the checker accepts it;
do not use `--rebase-merges`.

## 2. Rebase

```sh
git switch -c RESULT OLD_TIP
git rebase --onto UPSTREAM MB RESULT
```

When a commit conflicts:

- Read the upstream change that caused it **and** the downstream commit's
  intent (its message, and the rest of its diff). Resolve so the downstream
  commit does what it did before, on top of what upstream now does.
- Upstream wins on fixes. If an upstream fix changed the code a downstream
  commit edits, keep every check, lock, ordering constraint and error path
  the fix introduced, and fit the downstream change around it.
- If upstream now contains the downstream change itself (it was upstreamed,
  perhaps through a subsystem tree), let the downstream commit go empty and
  let git drop it. Record that, with the upstream commit.
- If you cannot tell what the right resolution is, do not guess silently. Make
  the most conservative resolution you can defend, mark it **UNSURE** in the
  report, and explain both readings.
- Never use `-X ours`, `-X theirs`, `--skip`, or `git checkout --ours/--theirs`
  on a whole file to make a conflict go away.

For each conflict, record: the downstream commit (subject), the files, the
upstream commit it collided with, and in a sentence or two what you kept and
why.

## 3. Build

The workflow re-runs these builds itself, but run them here so you can fix
what your resolutions broke. Each takes a while; run them once the rebase is
complete, not after every commit.

```sh
bash .rebase-tools/scripts/kernel-build.sh x86_64 .rebase-scratch/obj-x86 4
bash .rebase-tools/scripts/kernel-build.sh arm64 .rebase-scratch/obj-arm64 4
```

Exit status 3 means a downstream option no longer survives `olddefconfig`:
upstream renamed or re-depended a Kconfig symbol the series relies on (or that
the build script names). Find out which, and say so in the report; if the
build script's list is what is stale, that is a **Needs a decision** item,
not something to work around.

If a build fails because of the rebase (a resolution, or an upstream change a
downstream commit has not caught up with), fix it **in the downstream commit
that is wrong**. Interactive editors do not work here, so drive the todo list
with sed: `git -c sequence.editor="sed -i 's/^pick <sha>/edit <sha>/'" rebase
-i UPSTREAM`, amend, then `git rebase --continue`. Do not add a fix-up commit
on top unless there is no single commit it belongs to; if you must, its
subject starts with `rebase: ` and the report says why.

If a build fails on the pristine upstream tree too, it is not yours: build
UPSTREAM the same way in a `.rebase-scratch/` worktree to confirm, and report
it as pre-existing.

## 4. Audit what upstream brought in

For every upstream commit flagged in step 1, check that its change **survived
the replay**. Ancestry is not enough: a downstream commit replayed on top can
edit the very lines a fix added.

Give fixes the most attention: anything with a `Fixes:` tag, a `CVE-`
reference, or `Cc: stable`. For each one that touched a file the series also
touches:

```sh
git log --oneline UPSTREAM..RESULT -- <files the fix touched>
```

If that returns anything, read the **final tree**, not the commit graph:
confirm the fix's checks and ordering are intact and that downstream code
added after it is covered by them. Grep tree-wide for any function or flag
the fix deliberately removed, in case a downstream commit brought it back.

Also look for semantic collisions that did not conflict: an upstream change
to a function's contract (locking, refcounting, return values, a new required
call) where a downstream commit calls that function. Each one you cannot rule
out is an UNSURE item.

## 5. Check yourself

Run the same checker the workflow will run, so the report can explain its
result rather than be surprised by it:

```sh
bash .rebase-tools/scripts/verify-upstream-rebase.sh OLD_TIP RESULT UPSTREAM
```

Every drift line and every changed commit it lists must be accounted for in
your report by a conflict resolution, a dropped-empty commit or a build fix.
If one is not, you made a change you did not mean to: find it and undo it.

## 6. Report

Write the report to the `REPORT` path, in Markdown. It becomes a pull request
body when a human has to look, so lead with what they must decide. No
preamble, no sign-off.

```markdown
## Summary
One paragraph: the tree, upstream range (N commits, short SHAs and the
`git describe` of each end), downstream commits replayed, conflicts resolved,
commits dropped, and whether you expect the checker to pass.

## Needs a decision
One bullet per question, each starting **UNSURE:**, with both readings and
what you committed. "None." if there is nothing; anything else here stops the
automatic push.

## Conflicts resolved
Per conflict: downstream commit, files, colliding upstream commit, what you
kept and why. "None." if none.

## Dropped downstream commits
Subject and the upstream commit that made it empty. "None." if none.

## Build fixes
What broke, which commit you fixed it in, why. "None." if none.

## Upstream commits that meet the series
`short-sha subject` for each upstream commit that touched a file the series
touches, and for each fix among them your reading of the final tree. "None."
if none.

## Builds
x86_64, arm64: pass, fail (with the first error), or pre-existing failure
(confirmed on pristine upstream).
```

Then stop. Leave RESULT checked out or not; the workflow reads the branch,
not the working tree.

## If you cannot finish

If the rebase cannot be completed (a conflict you cannot resolve at all, a
toolchain that will not run), run `git rebase --abort`, do not create the
RESULT branch, and write the report explaining where you stopped and why. The
workflow treats a missing RESULT branch as a failed night and opens an issue
with your report.
