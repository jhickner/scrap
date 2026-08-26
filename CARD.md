# /mux command capabilities

I want to add some more capabilities to the /mux command.
- send a followup command to all sessions
- continue the conversation in a particular session, jumping into it in a new session


worktree /Users/jhickner/working/mux/.claude/worktrees/fngf

## Notes

- board: started
- board: started
- plan: `plans/mux-followup-and-open.md`

**The finding that shapes everything:** `fanout.c` opens every matrix row with `o.ephemeral = 1` — `--no-session-persistence` for claude, `no_session` for grok/pi, an unmaterialized thread for codex. Nothing from a `/mux` run is resumable today, and no transcript is written. Both requested capabilities are the same fix: drop `ephemeral`, capture each row's `session_id()` after its turn, and reopen against it.

**The plan:**

- `/mux followup <prompt>` — reopens each saved row with `resume_session` and runs the same board. Resume-by-id rather than holding 12 CLI children alive between commands.
- `/mux open [row]` — `workspace_spawn()` a tab adopting that row's id, then `sessionload_into()` to replay it, mirroring `yank()` in `sessionswitch.c`. Needs a small `workspace_spawn_ex()` so the row's standing prompt survives the jump (`system_extra` must be set before `session_start()`).
- `fanout_run()` splits into a shared `fan_go()` plus a file-static snapshot of the last run (specs, ids, and the cwd it ran in).

**Traps called out:** the followup must resume in the *stored* cwd, not the session's current one, or claude/codex won't find the transcript; opening a row must release its id from the snapshot so a later followup doesn't put two writers on one session; persisting rows means they now clutter `/resume`, mitigated by `title_set()` — which rejects rather than truncates labels over 80 bytes; `/restart` drops the snapshot.

Naming choices I made: no `more` alias (it would swallow `/mux more tests please`), and `open` counts as a subcommand only when followed by nothing or digits.
- plan: `plans/mux-matrix-view.md` (renamed — the old `mux-followup-and-open.md` is gone, since subcommands aren't the deliverable any more)

**What the research changed.** A scrollback item can't be clicked: mouse reporting is on globally and `TK_MOUSE_DOWN` carries row/col, but `viewport.h` has no hit-testing and the REPL loop drops the event into the line editor. Every clickable surface in mux — `form.c`, `boardgrid.c`, `pick.c` — is a chrome modal that owns `tty_read` and builds its own row map. So the matrix becomes a `chrome_full(1)` modal, like the board grid.

**And that unlocks the real simplification:** once it's a modal, each cell should be a workspace tab, not a fanout `struct worker` on a raw pthread. `workspace.c:412` already reads `hold = hold || chrome_modal_active()` — the workspace is built to drive sessions while a modal holds the screen. Then: talking to a cell is `workspace_send` (which queues if it's busy), the followup-to-all is that in a loop, and "access its session as a tab in the sessions list" is free — `^s` lists tabs straight off `workspace_at()`, so the hotkey is a jump, not a hand-off. Two bonuses: the observer that feeds each cell's log runs on the main thread (events are queued and drained in `session_turn_pump`), so `board_lock` and the atomics all go; and the `ephemeral` blocker from the last draft disappears, because `session_new` never sets it.

The plan keeps fanout's cell renderer intact — it's the one thing that already paints a conversation into a fixed-width box — and deletes the thread pool underneath it.

**One thing I want you to rule on before implementation:** I specced a *single* input at the bottom whose target switches (`all ›` vs `2 codex ›`) when you click a cell, rather than a per-cell editor plus a separate master. Your wording supports both readings; I picked one line because two live line editors on one screen is a lot of chrome for one prompt at a time. Also flagged: `^g` for "go to this cell as a tab" shadows `^g`-opens-$EDITOR inside the modal — say the word and it becomes `^o`.
- plan: `plans/mux-matrix-view.md` — same file, rewritten. Now in two phases: the view, then parentage.

**On not shadowing hotkeys — the receipts.** Every control code is already taken: the line editor eats `^A ^C ^E ^K ^R ^U ^W ^Y ^_` (`vendor/repl.h:1087`), the REPL prompt binds `^B ^C ^D ^F ^G ^L ^N ^O ^T ^V` (`prompt.c:694`), `^H ^I ^J ^M` are backspace/tab/newline/enter, and `^Q ^S ^Z` belong to the terminal. That leaves `^P` and `^X`, and both still cost the `p`/`x` muscle memory from the sessions list.

So the view binds **nothing new**. It reuses the idiom the prompt already uses — a key means one thing with text in the buffer, another when it's empty (`prompt.c:783`, `:820`, `:838`): arrows move the cursor or the cell selection; **Enter sends, or, on an empty input, goes to the selected cell as a tab**. Sending an empty followup was never meaningful, so that action was free. Clicking the already-selected cell does the same thing, matching the board grid.

**On the DAG.** One parent, fixed at creation, so it's a forest — no cycles by construction, and every UI question is one lookup. The link belongs in an append-only `parent` file beside `titles` in `~/.config/mux`, modeled on `title.c`, which already solves exactly this problem: a fact about a session id that must outlive the process, be visible to other windows, and survive a resume. That gets parentage across `/restart` with no new tabs-file field and no new CLI flag, and `livelist_publish` can carry it to other windows the same way it carries the title.

For `/sessions`: it's already two-level (cwd headings), and every row's label already starts with a marker glyph — so nesting is a prefix plus an ordering pass, not a new rendering mode. I've written the specifics and flagged the one genuine judgment call: **what to do with a child whose cwd differs from its parent's.** I proposed leaving it at top level in its own directory group rather than nesting it under a heading that would then misdescribe its rows — but "parentage beats directory grouping" is defensible and changes `group_rows` more deeply. That one's yours.

Two traps worth surfacing now: a session that has taken no turn has **no id**, so `/mux` as the first thing typed in a fresh tab produces children with nothing to hang under — they must open anyway and record no parent. And I deliberately scoped `^b`'s new tab and the board's worker tabs *out* — the mechanism should be general, but wiring `^b` to it would start nesting rows in everyone's `/sessions` for a relationship nobody asked to see.
- : `plans/mux-matrix-view.md` — same file, now with a **Phase 0** in front that's pure refactor, no user-visible change.

**You were right, and here's the specific duplication.** `replframe` (render a Repl into a cell grid) and `replkeys` (tty event → repl event, paste) are shared and healthy. What's written out three separate times is the wrapper around them — set width → ask for rows → render → scroll the caret into view → paint rows → route keys with `^V`. I put the line-by-line comparison in the plan: `prompt.c:444/265/270/224/690`, `ask.c:29/45/47/52/100/150`, `form.c:171/400/474/523/481/884`. `ask.c` and `form.c` are near line-for-line identical for the single-field case; `prompt.c` differs only in the paint step, where it rewrites the `>` glyph. A matrix rolling its own makes four.

So: `src/replbox.{c,h}` owns Repl + replframe + width + scroll window + key routing, with `replbox_key` returning 0 for keys it didn't consume so each host keeps Enter/Escape/its own bindings. **`ask.c` and `form.c` get converted in the same change** — that's the whole point, and I wrote the acceptance test as *the diff must delete more input-plumbing lines than it adds*. If it doesn't, the abstraction is wrong and should be dropped rather than shipped next to what it failed to replace. `prompt.c` stays on its own painter deliberately (history, completion dropdown, overlays, typeahead); `replbox_frame()` leaves the door open.

**A second wheel I found while looking.** The line → session pipeline exists once, but as inline control flow in `main.c:748-784`. `cmd_dispatch` is already session-parameterized, so I lifted the tail into `cmd_submit(session, line)`. The matrix calling `workspace_send` directly would have been a second answer to "what happens when you submit a line" — one where `/model` silently does nothing. Routing through `cmd_submit` instead means `/model`, `/effort`, `/permission` all work on the selected cell for free, and it's already safe: `can_pick()` refuses with "a list is already open" when `chrome_modal_active()` (`cmd.c:213`).

**One wheel I deliberately left spinning:** every modal writes its own `tty_read` loop (`ask.c`, `form.c`, `boardgrid.c`, `pick.c`), and the matrix makes a fifth. Their key semantics genuinely differ — search, menus, fields, grid layout — so unifying them is a bigger refactor than this feature earns. Phase 0 removes the input half of each, which is the part that actually is the same. Flagged rather than hidden.
- : Yes — written to `plans/mux-matrix-view.md` (398 lines), including everything from this round:

- **The wheels that already exist** — the inventory with file:line receipts for the three duplicated input wrappers, plus the two wheels I'm deliberately leaving alone.
- **Phase 0** — `replbox` (with the "convert `ask.c` and `form.c` in the same change" requirement and the diff-must-shrink acceptance test) and `cmd_submit()`.
- **Phase 1** — the view, cells-as-tabs, and the key map that binds nothing new.
- **Phase 2** — the parent link in a `parent` file beside `titles`, and the `/sessions` tree.
- Traps and verification, including the phase-0 check that's a diff read rather than a test run.

It's the same file as the last two revisions — only `plans/mux-matrix-view.md` exists; the earlier `mux-followup-and-open.md` was removed when the subject changed.

One thing still open from my side: the phase-2 judgment call on what to do with a child whose cwd differs from its parent's. I proposed leaving it at top level in its own directory group; the alternative — parentage beats directory grouping — changes `group_rows` more deeply.
- : No. Three facts:

- **`plans/` is untracked** — `git status` shows `?? plans/`. It's not ignored, just never committed, so it exists only in this worktree's working tree. Anyone on another branch, worktree, or machine sees nothing.
- **CARD.md can't hold a pointer.** It's generated from the board card by `card_write()` (`boardwork.c:315`) — title, body, worktree, and a `## Notes` section rendered from the card's log — and `boardwork.c:295` adds it to `.git/info/exclude`. Anything typed into it directly gets clobbered the next time the card is written out.
- **The repo's own convention was to commit it.** History shows `plans/triggered-actions.md` tracked on a branch, amended as the work went (`9151e10 plan: mark the traps that outlived the change`), and deleted when it landed (`366fcd2 remove plans`).

There's a sharper version of the problem: the card body still says the original two bullets — "send a followup command to all sessions", "continue the conversation in a particular session". That's no longer what the plan builds. An implementer who reads the card and never finds the plan builds the wrong thing.

Two things would fix it, and they're complementary:

1. **Commit `plans/mux-matrix-view.md` on `worktree-fngf`** — matches the prior convention, and puts the plan in front of anyone who checks out the branch.
2. **Add a note to the board card** pointing at `plans/mux-matrix-view.md` — the card log is the durable side of CARD.md, so the pointer survives regeneration and is the first thing whoever picks the card up reads.

I'm in plan mode, so I haven't done either — both are commits/writes outside the plan file. Say the word and I'll do them, or tell me which one you want.
- : First, a correction that changes the answer: **CARD.md is already a working write-back channel.** I said it gets clobbered — wrong. `boardfile.h` states the contract outright: *"The board writes it once when the worktree is made and the worker owns it from then on, so the board only ever takes a copy of it verbatim and puts that copy back."* `boardfile_keep()` runs at the end of every worker turn (`boardwork.c:879`) and copies the worktree's CARD.md to `~/.config/mux/board-cards/<id>.md`; `write_card_file()` restores that copy into any later worktree, generating from card fields only as a fallback (`boardwork.c:335`). This session is card `fngf` with `"done":["plan"]`, so my turn ends in a capture. Your preferred option B exists in the plumbing today — nothing points the planner at it.

**The real gap is worse than the one you asked about.** `implement.md` opens with *"The plan below was approved."* What's actually below is `card_write(out, c)` — the card in full, notes and all (`boardwork.c:441-470`). Nothing puts a plan there: `needs:` only sequences actions (`boardflow.c:20`), and `boardstep_fill` knows just `{branch} {worktree} {repo} {onto} {base}` (`boardstep.c:29`). The implementer is promised a plan it is never handed, and then falls back to the card body — which, for this card, still says the original two bullets the plan has long since outgrown.

**Recommendation — both halves, they're complementary:**

1. **Planner writes a `## Plan` block into CARD.md**: the path, a one-line synopsis, and "this supersedes the card body where they differ." It rides in `board-cards/<id>.md`, survives the worktree being dropped, and gets inlined verbatim into the implementer's first turn. That's B, and it needs no code.
2. **Planner commits the plan file.** `implement.md` already says "uncommitted work does not exist to anything downstream" — an untracked `plans/` file is exactly that. That's A's durable half: the card names the path, git carries the content. It also matches the repo's own history (`plans/triggered-actions.md` was committed, amended, then deleted on landing).
3. **Both prompts need editing**, because `plan.md` currently forbids exactly these two acts ("The one file you write is the plan file itself", "do not make any commit"), and `implement.md`'s first line should say what's true: read the file the card's `## Plan` names; it's the spec; it supersedes the card body.

One cost to know: action prompts are **compiled in**. `read_actions()` reads only `board_defaults` (`boardcfg.c:243`), and `boardcfg.c:435` actively deletes stale action files from the config dir — which is why `~/.config/mux/board/` has only `backends/` and `settings.md`. So this is a source edit to `board/actions/*.md` plus a rebuild, not a config tweak.

**If you want it robust rather than convention-based**, the code version is small: a `plan:` frontmatter key or a `{plan}` fill that inlines a named file into the action prompt, so the hand-off doesn't depend on the planner remembering. `first_turn()` is the one place to do it.

I can't do any of this from here — `plan.md` forbids the edits and the commit, which is the recursion in the problem. Want this written up as its own plan file? It's a different subject from the matrix view, so it'd be a new slug rather than a rewrite of `mux-matrix-view.md`.
- : Done. The plan now lives in `CARD.md` as a `## Plan` section between the card body and `## Notes`; `plans/` is gone, and the worktree is clean (CARD.md is git-excluded by design, `boardwork.c:295`).

The section opens by saying it supersedes the two bullets above, since those are the original ask and the design moved on. Headings are demoted so `## Plan` and `## Notes` remain the card's only two top-level sections.

**This survives on its own.** `boardfile_keep()` runs at the end of this turn (`boardwork.c:879`) and copies CARD.md to `~/.config/mux/board-cards/fngf.md`; `write_card_file()` restores that copy verbatim into any future worktree for this card, generating from card fields only when no copy exists. So the plan outlives this worktree.

**But one string blocks the hand-off.** I re-checked `first_turn()`: the prompt it builds inlines `card_write(out, c)` — the card from the *store* (title, body, worktree, log), not the contents of CARD.md — and then says:

> "Do not open it to start: it holds nothing you have not been given."

That was true when CARD.md was just a rendering of the card. It's false now, and it's the exact sentence that will make the implementer skip the plan. Two edits make your chosen approach work:

- `boardwork.c:464-467` — that sentence must instead tell the worker to read CARD.md, because the card in the prompt is no longer the whole of it.
- `board/actions/implement.md` — "The plan below was approved" should say the plan is the `## Plan` section of CARD.md, and that it supersedes the card body.

Both are compiled in (`read_actions` reads only `board_defaults`; `boardcfg.c:435` deletes stale config copies), so they need a source edit and a rebuild. And `board/actions/plan.md` still says "The one file you write is the plan file itself" and names `plans/<slug>.md` — that wants updating to the new convention too, or the next planner will recreate `plans/`.

I haven't touched any of those three — they're mux source changes, outside what plan mode permits me here. Want them as the next card?
- : Good change on balance — better on the three things that matter, and it deletes code. But two conditions, or it's a net regression.

**What genuinely improves.** Today there are two copies: the worktree's CARD.md and `~/.config/mux/board-cards/<id>.md`, reconciled by `boardfile_keep()` at the end of every worker turn. Git makes the branch the single record, and `boardfile.c` (~70 lines), both keep calls, and `ignore_card_file` all delete. You also get history — `boardfile_keep` is last-write-wins, whereas this plan's three revision rounds would have been three diffable commits. And it travels: the kept copy lives in one machine's config dir, a branch goes wherever the repo goes. As a bonus, implement.md's own rule ("uncommitted work does not exist to anything downstream") finally holds for the plan.

**Condition 1 — strip on the branch, before the merge, not after.** `merge.md` lands with `git merge --ff-only {branch}`. A fast-forward means master gets exactly the branch's commits, so if CARD.md is tracked at the tip, **master ends up with CARD.md at its root, plan and all.** The strip has to be a `git rm CARD.md` commit as the last step on the branch. That also disposes of a problem you'd otherwise hit fast: every card carries `/CARD.md`, so two cards merging to master would conflict on that one path every time. Stripped before the merge, they never meet. And it stays recoverable — after an ff-merge master's history holds both the add and the remove, so `git show <sha>:CARD.md` gets the plan back.

**Condition 2 — nothing may drop the worktree while CARD.md is dirty.** The `boardfile_keep(c)` call in `drop_worktree` (`boardwork.c:1131`) is today's safety net. Git has no equivalent, and `tidy_step` runs `git worktree remove --force` followed by `git branch -D` (`:227`) — force plus `-D` means an uncommitted or unmerged plan vanishes silently. `drop_worktree` has to refuse, or auto-commit, when the tree is dirty. This is the one place where "just use git" is strictly weaker unless handled.

**One regression to accept knowingly.** A card closed *without* merging gets `branch -D`, and the plan is gone; today the kept copy survives until the card is deleted. I'd accept that — an abandoned card's plan is usually worth abandoning — but make close-without-merge say what it's about to destroy.

**A gotcha you'd hit in the first five minutes.** `ignore_card_file()` (`boardwork.c:265`) appends `CARD.md` to `<git-common-dir>/info/exclude` — the *shared* git dir, so it's repo-wide and permanent. It's already in this repo; I tested it just now: `git add CARD.md` → "ignored by one of your .gitignore files", needs `-f`. That function has to go, and existing repos need the stale line removed, or every planner's `git add` silently no-ops.

**What this doesn't fix.** Delivery. The implementer's prompt still inlines `card_write(out, c)` — the card from the store — and still says *"Do not open it to start: it holds nothing you have not been given."* Git makes CARD.md durable, reviewable and shareable; it does not put it in front of the implementer. Those two strings (`boardwork.c:464`, `board/actions/implement.md`) still need changing, plus `plan.md`, which still names `plans/<slug>.md` and forbids commits. Worth not letting the git change feel like it closed that loop.

Want me to work this up as its own card? It touches `boardwork.c`, `boardfile.c` and all three action prompts, so it's a real change to mux rather than something I can fold into this one.
- : Card **`gss3`** — "CARD.md lives in git, not in a board-kept copy", on `/Users/jhickner/working/mux` (it resolved to the main checkout, not this worktree, via `gitcmd_root`'s worktree-list-first path). Queue is empty; nothing runs until you send it somewhere.

The write-up is in the card **body**, not a `plans/` file — so it's inlined into whatever action picks it up, which sidesteps the delivery gap the card itself is about. 67 lines covering:

- **The code**: delete `boardfile.{c,h}` and its four call sites; drop `ignore_card_file` (plus clean the repo-wide exclude line it already wrote); `write_card_file` always generates; and `drop_worktree` must refuse a dirty worktree, since deleting `boardfile_keep` removes the only thing standing between an uncommitted plan and `worktree remove --force` + `branch -D`.
- **The prompts**: all three actions, plus the false sentence at `boardwork.c:464`, with the note that these are compiled into `boarddefaults.c` and need a rebuild.
- **The traps**: the abandoned-card regression, cards without worktrees, and the fact that a re-made worktree recovers CARD.md from the branch for free.

Given the plan is already in the body, I'd send it straight to `implement` rather than `plan` — running the planner would just re-derive what's there. Your call.

Worth noting what this card does to the card we've been working: once it lands, `plan.md` will say the plan belongs in CARD.md and gets committed. This worktree's CARD.md already has the `## Plan` section but is still untracked and excluded — so when you're ready to hand `fngf` to an implementer, it'll need `git add -f CARD.md` until `gss3` removes that exclude line.
- : Yes — land `gss3` first. Three reasons, and one caveat that decides how you sequence it.

**They don't collide.** `gss3` touches `boardfile.{c,h}`, `boardwork.c`, `board.c` and the action prompts. `fngf` touches `matrix.c`, `cmd.c`, `workspace.c`, `session.c`, `sessionswitch.c` and a new `replbox`. No overlap, so no conflict risk either way.

**`fngf` has nothing to merge yet.** Zero commits on `worktree-fngf`, clean tree — the plan action produced no code. So there's no git-level ordering question at all; "come back to it" just means handing it to an implementer.

**And that hand-off is broken until `gss3` lands.** `fngf`'s plan is in CARD.md, which is untracked, excluded, and — worse — the implementer's prompt inlines the card *body* (the two original bullets) and explicitly says not to open the file. An implementer picking up `fngf` today builds the wrong thing. `gss3` is also small next to `fngf`: delete a module, four call sites, three prompts, one guard, versus a three-phase view rewrite. Better to have the big card run its whole length under the fixed convention than to fight the broken one for weeks.

**The caveat: merging `gss3` isn't sufficient — it has to be built and installed.** The action prompts are compiled into `src/boarddefaults.c` from `board/actions/*.md`, so the running mux keeps the old prompts until the binary is replaced. `make install` handles it, including the `pkill -SIGURG -a -x mux` restart signal that's mux's own reload path. So the order is: implement `gss3` → merge → `make install` → then hand `fngf` off.

**If you'd rather not serialize**, there's a zero-dependency alternative for `fngf`: move its plan from CARD.md into the card *body*, which is already inlined into the implementer's first turn with no code change — that's exactly what I did for `gss3`. Then `fngf` can go now and `gss3` lands on its own schedule. It costs you the git history on the plan, which is precisely what `gss3` exists to fix.

My recommendation: `gss3` first, then install, then `fngf`. Want me to queue `implement` on `gss3`? Given the plan is already in its body, I'd skip the `plan` action.
