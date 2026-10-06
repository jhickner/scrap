#include <stdlib.h>

#include "filediff.h"
#include "md.h"
#include "prompt.h"
#include "sessionview.h"
#include "status.h"
#include "tasks.h"
#include "toolstyle.h"

void filediff_snapshot(struct filediff_snapshot *snap, const char *path)
{
    (void)snap;
    (void)path;
}

char *filediff_take_patch(struct filediff_snapshot *snap)
{
    (void)snap;
    return NULL;
}

int filediff_patch_draws(const char *patch)
{
    (void)patch;
    return 0;
}

void filediff_clear(struct filediff_snapshot *snap)
{
    free(snap->before);
    snap->before = NULL;
    snap->have = 0;
}

void md_render_kept(const char *text, int indent)
{
    (void)text;
    (void)indent;
}

void prompt_echo_message(const char *text) { (void)text; }
void status_pause(void) {}
void status_resume(void) {}
void status_set_alert(const char *text) { (void)text; }
void status_set_word(const char *text) { (void)text; }

const struct task *tasks_by_parent(const struct tasktab *tasks, const char *parent)
{
    (void)tasks;
    (void)parent;
    return NULL;
}

int tasks_done(const struct task *task)
{
    (void)task;
    return 0;
}

void tasks_label(const struct task *task, char *out, size_t size)
{
    (void)task;
    if (size)
        out[0] = '\0';
}

void tasks_line(const struct task *task, char *out, size_t size)
{
    (void)task;
    if (size)
        out[0] = '\0';
}

int toolstyle_collapses(const char *name, const char *input_json, const char *arg)
{
    (void)name;
    (void)input_json;
    (void)arg;
    return 0;
}

void view_tool_argument(const backend_event *ev, const char *cwd, char *out,
                        size_t size)
{
    (void)ev;
    (void)cwd;
    if (size)
        out[0] = '\0';
}

int view_tool_path(const char *input_json, const char *cwd, char *out, size_t size)
{
    (void)input_json;
    (void)cwd;
    if (size)
        out[0] = '\0';
    return 0;
}

void view_keep_nest(int on, const char *label)
{
    (void)on;
    (void)label;
}

void view_keep_activity(const char *marker, const char *text, enum ui_role role)
{
    (void)marker;
    (void)text;
    (void)role;
}

void view_keep_tool_call(const char *name, const char *arg, int collapses)
{
    (void)name;
    (void)arg;
    (void)collapses;
}

void view_keep_tool_call_bg(const char *name, const char *arg, int collapses, int background,
                            const char *call)
{
    (void)name;
    (void)arg;
    (void)collapses;
    (void)background;
    (void)call;
}

void view_keep_background(const char *call) { (void)call; }

void view_keep_break(void) {}

void view_keep_output(const char *text, enum ui_role role, int error)
{
    (void)text;
    (void)role;
    (void)error;
}

void view_keep_diff(char *patch) { free(patch); }
