---
job: landing
step: landing
---

This branch could not be landed. What the attempt said is below.

Find out which step failed before changing anything. The message comes from the
landing script, whose steps fail for unrelated reasons:

- `rebase failed` is a conflict with the base branch. Resolve it here.
- `check failed` is the check the script runs. Fix the branch.
- `merge failed` is `git merge --ff-only` in the main checkout. It usually means
  that checkout has uncommitted changes to files this branch touches, or is not
  on the base branch. Neither is something this branch can fix.

Compare the branch against the base and run the check yourself. If the base has
not moved and the check passes, the branch is not at fault: report what is
blocking the merge, name the files or state responsible, and stop. Do not invent
changes to a branch that is already good, and do not commit, stash or revert
uncommitted work in the main checkout.

Otherwise rebase onto the branch it is going back to, resolve what is in the
way, and make the check pass. Commit the result on the branch you are already
on, then stop and say what you had to change.

Do not merge it yourself: the board lands it once the branch is clean.
