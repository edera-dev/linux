---
name: lts-audit
description: Weekly audit of edera/6.18-lts for Edera commits from edera/mainline that it lacks, run unattended in CI by .github/workflows/lts-audit.yml. Covers telling a commit that is really missing from one that was ported in another form or does not apply to 6.18, and the triage notes that go into the audit issue.
---

# LTS audit triage

Edera kernel work lands on `edera/mainline` first and flows down to
`edera/6.18-lts`, one backport pull request per mainline pull request. Some
commits still fall through. The workflow has already listed, from git alone,
every Edera commit on mainline that the LTS tree does not seem to have, and
it files that list as an issue whatever you say. Your job is to tell the
maintainers which of those are **really** missing, and what to do about the
rest.

The list comes from `pending-backports.sh`, which counts a mainline commit as
present on the LTS tree only when the LTS series has a commit with the same
subject (ignoring a trailing version tag such as ` (6.18)`) or the same
patch-id. So a commit can be listed although it is there: ported under a
reworded subject and with a changed diff, folded into another commit, or
already in `linux-6.18.y` upstream. And a commit can be listed because it
does not belong on 6.18 at all.

This runs unattended. Nobody can answer a question. Everything a maintainer
needs has to end up in the report.

## Inputs

The workflow gives you, in the prompt:

- `LTS_TIP`: the `edera/6.18-lts` commit audited (checked out). Local branch
  `lts-upstream` is its upstream, `linux-6.18.y`; the Edera series is
  `<lts-base>..LTS_TIP`, where `<lts-base>` is their merge-base.
- `MAINLINE_TIP`: the `edera/mainline` commit audited (fetched). Local branch
  `mainline-upstream` is its upstream, Linus' `master`, and `<mainline-base>`
  their merge-base.
- `PENDING`: the commits listed as missing, one per line as `<sha> TAB
  <subject>`, oldest first. The shas are mainline commits.
- `STATUS`: what is already happening to them, one per line as `<where> TAB
  <subject>`. `<where>` is `#N` for an open pull request into
  `edera/6.18-lts` that carries a commit with that subject, or `#N
  backport-waiting` / `#N backport-needs-attention` for a merged mainline
  pull request whose backport is in that state. It may be empty.
- `REPORT`: the path your report must be written to.

You change nothing: no commits, no branches, no checkouts, no pushes. Read
with `git log`, `git show`, `git diff`, `git range-diff`, `git grep` and the
like.

### How to run commands here

Your shell commands are checked against an allowlist that matches the
command's first words. Shell variables, `$(...)`, `<(...)`, pipes into
commands that are not on the list, and `VAR=x cmd` prefixes will be refused,
so `LTS_TIP`, `<sha>` and `<...>` below are placeholders: substitute the
literal value. Work out the merge-bases once with `git merge-base` and reuse
the hashes.

This is a blobless clone: file contents are fetched from GitHub the first time
a command needs them, so the first `git show` of a commit can be slow. That is
not a hang.

## 1. Read the list

```sh
cat PENDING
cat STATUS
git merge-base LTS_TIP lts-upstream
git merge-base MAINLINE_TIP mainline-upstream
```

A commit with an open backport pull request in `STATUS` is in flight: note
the pull request and move on, unless something in it looks wrong.

## 2. Judge each commit

For every other commit, `git show <sha>` and decide which one of these it is.
Look, in this order:

1. **Ported in another form.** Search the LTS series for the change: the
   files it touches (`git log --format='%h %s' <lts-base>..LTS_TIP --
   <path>`), distinctive identifiers it adds (`git grep -n <identifier>
   LTS_TIP -- <path>`), and its subject with different wording. If an LTS
   commit carries the same change, adapted or not, name it, and say how it
   differs. Several mainline commits squashed into one LTS commit count.
2. **Already upstream in 6.18.** Edera fixes are sometimes sent upstream and
   land in a stable release: look for the change in `lts-upstream` (`git log
   --format='%h %s' lts-upstream -- <path>`, then `git show`).
3. **Not applicable to 6.18.** The commit fixes or extends code that only
   mainline has, upstream or Edera, and the LTS tree has nothing for it to
   apply to. Say what is absent.
4. **Missing.** None of the above: the LTS tree lacks the change. Say what
   it does in one line, which later Edera commits in `PENDING` depend on it,
   and whether it looks likely to apply to 6.18 as is, or needs adapting (and
   why).

When you cannot tell, say **UNSURE**, give both readings and what would
settle it. Never call a commit present on a guess: a maintainer will act on
"ported" by adding it to the skip file, and a commit skipped by mistake is
lost from the LTS tree for good.

## 3. Write the report

Write `REPORT` as Markdown. It is pasted under a table of the commits, so do
not repeat the table. Start with one short paragraph saying how many are
really missing, how many are in flight, and how many need a maintainer to
skip them. Then one entry per commit, in `PENDING` order:

```markdown
### `<sha12>` <subject>

**Missing** | **In flight (#N)** | **Ported as `<lts-sha12>`** | **Upstream in 6.18 as `<sha12>`** | **Not applicable** | **UNSURE**

<two to five sentences: the evidence, and what to do>
```

What to do, by verdict:

- Missing: backport it. If its mainline pull request is in `STATUS` as
  waiting or needing attention, point at that pull request.
- Ported, upstream, or not applicable: add its subject, exactly as `PENDING`
  has it, to `.automation/backport-skip` on `edera/6.18-lts`, or label its
  mainline pull request `no-backport`.

Keep it factual and short. Do not quote large diffs.
