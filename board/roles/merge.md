---
runs: command
tier: med
skippable: 1
fail step: worktree
fail prompt: landing
lock: repo
---

echo '== rebase onto {base}'
git fetch . {base}:{base} >/dev/null 2>&1 || true
git rebase {base} || { git rebase --abort >/dev/null 2>&1; echo 'rebase failed'; exit 1; }

echo '== check'
make check || { echo 'check failed'; exit 1; }

echo '== merge'
git -C {root} merge --ff-only {branch} || { echo 'merge failed'; exit 1; }

echo '== tidy'
git -C {root} worktree remove --force {tree} >/dev/null 2>&1
git -C {root} branch -D {branch} >/dev/null 2>&1
echo merged
