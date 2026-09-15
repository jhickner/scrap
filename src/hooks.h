#ifndef HOOKS_H
#define HOOKS_H

#include <stddef.h>

#include "vendor/agents/backend.h"

/* Tool hooks from ~/.config/mux/hooks.yaml: a list of entries, each with
   `tool` (the backend's tool matcher: a name, "Edit|Write", or omitted for
   every tool), optional `match` (a substring, or a list of them, one of which
   the tool's command or file path must contain), optional `event` (PreToolUse
   unless given) and `context` (the text injected into the model's context
   when the hook fires). */

#define HOOKS_MAX       64
#define HOOKS_MATCH_MAX 32

struct hook {
    char *event;
    char *tool;
    char *match[HOOKS_MATCH_MAX];
    int   match_count;
    char *context;
};

/* Reload the file when it changed; returns the number of hooks. */
int hooks_load(void);

/* Replace the loaded set with the entries parsed from text; for tests. */
int hooks_parse(const char *text);

int                hooks_count(void);
const struct hook *hooks_at(int i);

/* Fill out with the loaded set's backend registration; returns the count. */
int hooks_backend(backend_hook *out, int max);

/* The context hook i injects for this tool call, malloc'd, or NULL when its
   match does not apply. */
char *hooks_context(int i, const char *tool, const char *input_json);

int hooks_path(char *out, size_t cap);

#endif
