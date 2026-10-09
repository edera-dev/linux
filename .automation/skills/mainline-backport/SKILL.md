---
name: mainline-backport
description: Backport of one merged edera/mainline pull request to edera/6.18-lts, run unattended in CI by .github/workflows/lts-backport.yml. Covers the cherry-picks, adapting a commit to the older kernel, spotting a dependency on another backport, build checks, and the report that becomes the backport pull request.
---

# Mainline pull request backport

Edera kernel work lands on `edera/mainline` first, one pull request at a time,
and every merged pull request is proposed for `edera/6.18-lts` as its own
backport pull request. You are preparing one of those. The workflow has
already worked out which of the pull request's commits the LTS tree still
lacks and hands you that list.

The goal is that each backport changes **exactly what its mainline commit
changes**. Where 6.18 differs from mainline enough that the same change does
not apply, you adapt it the way a careful stable maintainer would and you say
exactly what you did.

This runs unattended. Nobody is watching while you work, and nobody can answer
a question. Everything a reviewer needs has to end up in the report.

## What happens to your result

You cannot push, and nothing you write lands on `edera/6.18-lts`: a maintainer
merges the backport pull request, or does not. When you finish, the workflow
independently:

1. runs `verify-backport.sh`, which checks from git alone that your branch
   fast-forwards the LTS tip, is linear, and holds one commit per requested
   commit, in order, each naming its source with a `(cherry picked from
   commit <sha>)` trailer, keeping its source's subject, and changing the
   same files by the same +/- lines;
2. builds x86_64 and arm64 `vmlinux` with `kernel-build.sh`.

Both results go into the pull request next to your report. A commit you had
to adapt **will** show up as adapted there. That is expected, not a failure;
your report explains it. Never reword a subject, merge two commits into one,
or split one into two: backports are recognised later by subject, and a
renamed one would be offered again.

## Inputs

The workflow gives you, in the prompt:

- `MAINLINE_PR`: the number and title of the merged pull request.
- `OLD_TIP`: the `edera/6.18-lts` commit to build on (already checked out).
- `REQUEST`: a file listing the pull request's commits to backport, one per
  line as `<sha> TAB <subject>`, oldest first. Their objects are already
  fetched. Commits the LTS tree already has, and automation-only commits, are
  not in it.
- `PENDING`: a file listing every Edera commit on `edera/mainline` that the
  LTS tree lacks, in the same format, oldest first. The request's own commits
  are among them. The workflow has already checked that nothing earlier in
  this list touches the request's files; you look for the dependencies a file
  match cannot see.
- `RESULT`: the local branch name your result must end up on.
- `REPORT`: the path your report must be written to.

The automation's own scripts are in `.rebase-tools/scripts/`, copied from the
commit this run started from. Use those, never the copy inside the tree.

### How to run commands here

Your shell commands are checked against an allowlist that matches the
command's first word. Shell variables, `$(...)`, `<(...)` and `VAR=x cmd`
prefixes will be refused, so `OLD_TIP`, `RESULT`, `<sha>` and `<...>` below
are placeholders: substitute the literal value. Put scratch files, worktrees
and build output under `.rebase-scratch/` in the repository root; the
kernel's `.gitignore` ignores it.

This is a blobless clone: file contents are fetched from GitHub the first time
a command needs them, so the first checkout or `git show` can be slow. That is
not a hang.

## 1. Read the request

```sh
cat REQUEST
cat PENDING
git show --stat <sha>        # for each requested commit
```

For each commit, read its message and diff, and check what it depends on:
does it call a helper, use a struct field or a Kconfig symbol that 6.18
lacks? If so, where does that come from?

- From an earlier commit in the request: fine, it is backported first.
- From a commit in `PENDING` that is not in the request: this backport has to
  **wait** for that one. Stop here; see "If it has to wait".
- From upstream (Linus' tree) only: see the next section.

## 2. Cherry-pick, in order

```sh
git switch -c RESULT OLD_TIP
git cherry-pick -x <sha>     # one at a time, oldest first
```

`-x` is required: the trailer it adds is how the checker pairs your commit
with its source. If you have to amend, keep the trailer and the subject.

When a pick conflicts, or applies but would not build or behave the same on
6.18:

- Read what differs between the two kernels around the change, and adapt the
  change so it does on 6.18 what it does on mainline. Prefer the smallest
  adaptation; do not backport an unrelated upstream refactor to make the
  patch apply.
- If the conflict is with Edera code that only a pending commit brings,
  that is a dependency: **wait**, as above.
- If the commit depends on an upstream change 6.18 lacks and cannot be
  adapted without it, do not pull that upstream change in. Leave the commit
  out (`git cherry-pick --abort` for that one, then continue with the next)
  and make it a **Needs a decision** item saying what it needs.
- If the commit does not apply to 6.18 at all (it fixes code only mainline
  has), leave it out the same way and say so.
- If you cannot tell what the right adaptation is, make the most conservative
  one you can defend, mark it **UNSURE** in the report, and explain both
  readings.
- Never use `-X ours`, `-X theirs`, or `git checkout --ours/--theirs` on a
  whole file to make a conflict go away.
- Never commit anything that is not a backport of a requested commit. If a
  backport needs a 6.18-only fix to build, fold it into that backport and
  report it as an adaptation.

## 3. Build

```sh
bash .rebase-tools/scripts/kernel-build.sh x86_64 .rebase-scratch/obj-x86 4
bash .rebase-tools/scripts/kernel-build.sh arm64 .rebase-scratch/obj-arm64 4
```

If a build fails because of a backport, fix it **in that backport**: drive the
todo list with sed, `git -c sequence.editor="sed -i 's/^pick <sha>/edit
<sha>/'" rebase -i OLD_TIP`, amend (keeping subject and trailer), then `git
rebase --continue`. Never rebase below OLD_TIP.

If a build fails on OLD_TIP too, it is not yours: build OLD_TIP the same way
in a `.rebase-scratch/` worktree to confirm, and report it as pre-existing.

## 4. Check yourself

```sh
bash .rebase-tools/scripts/verify-backport.sh OLD_TIP RESULT REQUEST
```

Every commit it lists as adapted or left out must be accounted for in your
report. If one is not, you changed something you did not mean to: find it and
undo it.

## 5. Report

Write the report to the `REPORT` path, in Markdown. Its **first line** is the
status, exactly one of:

- `Status: proposed` when the RESULT branch holds a backport for review;
- `Status: waiting` when it has to wait for another backport (no RESULT
  branch);
- `Status: not-applicable` when none of the request applies to 6.18 (no
  RESULT branch).

The rest becomes the pull request body (or the comment on the mainline pull
request when there is no branch), so lead with what a reviewer must decide.
No preamble, no sign-off.

```markdown
Status: proposed

## Summary
One paragraph: commits requested, backported unchanged, adapted, left out,
and whether you expect the checker to pass.

## Needs a decision
One bullet per question, each starting **UNSURE:**, with both readings and
what you committed. Every left-out commit is one. "None." if there is
nothing.

## Adapted commits
Per commit: subject, what differs on 6.18, what you changed and why. "None."
if none.

## Left out
Per commit: subject and why. "None." if none.

## Builds
x86_64, arm64: pass, fail (with the first error), or pre-existing failure
(confirmed on OLD_TIP).
```

Then stop. The workflow reads the branch, not the working tree.

## If it has to wait

Do not create the RESULT branch. Write the report with `Status: waiting`,
and under `## Waiting on` list each pending commit it needs, as `` `<sha>`
<subject> ``, with one line on what it needs from it. The workflow labels the
mainline pull request `backport-waiting` and tries again once other backports
land.

## If nothing applies, or you cannot finish

If none of the request applies to 6.18, do not create the RESULT branch and
write the report with `Status: not-applicable`, explaining why. If you could
not produce a backport for any other reason, do not create the branch either
and explain what went wrong; the first line is then whatever fits best, and
a missing branch is treated as needing a human either way.
