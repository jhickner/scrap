
#ifndef SESSIONVIEW_H
#define SESSIONVIEW_H

#include "vendor/cJSON.h"

#include <stddef.h>

#include "ui.h"
#include "vendor/agents/backend.h"

struct turnview {
    int after_collapse; /* the last call was one the tool style shows as a row */
};

void view_tool_argument(const backend_event *ev, const char *cwd, char *out, size_t size);

/* the one argument that stands for a tool call, by the key order above */
const char *view_tool_arg_value(const cJSON *input);

int  view_tool_path(const char *input_json, const char *cwd, char *out, size_t size);

void view_tool_call(const char *name, const char *arg);

void view_tool_error(const char *text);

/* Draw what is kept from here on one step in, as the named subagent's work
   rather than the session's own. */
void view_keep_nest(int on, const char *label);

void view_keep_activity(const char *marker, const char *text, enum ui_role role);

void view_keep_tool_call(const char *name, const char *arg, int collapses);

void view_keep_break(void);

void view_keep_output(const char *text, enum ui_role role, int error);

void view_keep_diff(char *patch);

int  view_collapsed(void);
void view_collapse(int on);

#define VIEW_KEEP_KIND "keep"
void view_keep_load(const cJSON *st);

#endif
