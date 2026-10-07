
#ifndef SESSIONVIEW_H
#define SESSIONVIEW_H

#include "vendor/cJSON.h"

#include <stddef.h>

#include "ui.h"
#include "vendor/agents/backend.h"

struct turnview {
    int after_collapse;
};

void view_tool_argument(const backend_event *ev, const char *cwd, char *out, size_t size);

const char *view_tool_arg_value(const cJSON *input);

/* The argument a one-line view shows for a call: the optchat tools' ids or
 * task, else view_tool_arg_value(). Fills scratch only for the former. */
const char *view_tool_value(const char *name, const cJSON *input, char *scratch, size_t size);

int  view_tool_path(const char *input_json, const char *cwd, char *out, size_t size);

void view_tool_call(const char *name, const char *arg);

void view_tool_error(const char *text);

void view_keep_nest(int on, const char *label);

void view_keep_activity(const char *marker, const char *text, enum ui_role role);

void view_keep_tool_call(const char *name, const char *arg, int collapses);
void view_keep_tool_call_bg(const char *name, const char *arg, int collapses, int background,
                            const char *call);

void view_keep_background(const char *call);

void view_keep_break(void);

void view_keep_output(const char *text, enum ui_role role, int error);

void view_keep_diff(char *patch);

int  view_collapsed(void);
void view_collapse(int on);

#define VIEW_KEEP_KIND "keep"
void view_keep_load(const cJSON *st);

#endif
