#ifndef AGENTSYNC_H
#define AGENTSYNC_H

#include <stdio.h>

/* `mux sync`: one home for skills and hooks across every backend.

   Skills: ~/.claude/skills is canonical and ~/.agents/skills links to it; codex,
   pi and grok all read ~/.agents/skills. Redundant links in the per-backend
   skill dirs are removed; stale copies are reported, and removed with prune.

   Hooks: ~/.config/mux/hooks.json (Claude's hook schema, seeded from
   ~/.claude/settings.json on first run) is rendered into ~/.claude/settings.json,
   ~/.codex/hooks.json and a pi extension. grok reads the Claude file itself. A
   handler may carry "claude", "codex" or "pi" keys: a string swaps the command
   for that backend, false drops the handler there. */
int agentsync_run(const char *home, int prune, FILE *log);

int agentsync_main(int argc, char **argv);

#endif
