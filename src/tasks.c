#include "tasks.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "text.h"
#include "toolstyle.h"
#include "vendor/cJSON.h"

void tasks_reset(struct tasktab *t, const char *tag)
{
    if (!t)
        return;
    memset(t, 0, sizeof *t);
    snprintf(t->tag, sizeof t->tag, "%s", tag && *tag ? tag : "task");
}

int tasks_done(const struct task *a)
{
    return a && strcmp(a->status, "running") != 0 && strcmp(a->status, "pending") != 0;
}

int tasks_running(const struct tasktab *t)
{
    int n = 0;
    for (int i = 0; t && i < t->n; i++)
        if (!tasks_done(&t->v[i]))
            n++;
    return n;
}

int tasks_pending(const struct tasktab *t)
{
    int n = 0;
    for (int i = 0; t && i < t->n; i++)
        if (!t->v[i].inferred && !tasks_done(&t->v[i]))
            n++;
    return n;
}

int tasks_count(const struct tasktab *t)
{
    return t ? t->n : 0;
}

const struct task *tasks_at(const struct tasktab *t, int i)
{
    return t && i >= 0 && i < t->n ? &t->v[i] : NULL;
}

const struct task *tasks_by_parent(const struct tasktab *t, const char *tool_use_id)
{
    if (!t || !tool_use_id || !*tool_use_id)
        return NULL;
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].parent, tool_use_id))
            return &t->v[i];
    return NULL;
}

#define TASK_LABEL_CELLS 14

static int label_filler(const char *word, size_t n)
{
    static const char *const skip[] = {
        "the", "a", "an", "of", "to", "and", "for", "in", "on", "its", "it",
        "then", "that", "this", "with", "from", "into", "back", "up", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strlen(skip[i]) == n && !strncmp(skip[i], word, n))
            return 1;
    return 0;
}

void tasks_label(const struct task *a, char *out, size_t size)
{
    const char *from = a ? (a->desc[0] ? a->desc : (a->type[0] ? a->type : a->id)) : "";
    char        flat[160];
    size_t      at = 0;

    if (!size)
        return;
    text_one_line(from, flat, sizeof flat);
    for (char *p = flat; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    for (const char *p = flat; *p && at + 1 < size && at < TASK_LABEL_CELLS;) {
        size_t n = strcspn(p, " ");
        if (!label_filler(p, n)) {
            size_t want = n + (at ? 1 : 0);

            if (n && at + want <= TASK_LABEL_CELLS && at + want < size) {
                if (at)
                    out[at++] = ' ';
                memcpy(out + at, p, n);
                at += n;
            } else if (at) {
                break;
            } else {
                n = n < TASK_LABEL_CELLS ? n : TASK_LABEL_CELLS;
                if (n >= size)
                    n = size - 1;
                memcpy(out, p, n);
                at = n;
            }
        }
        p += n;
        while (*p == ' ')
            p++;
    }
    while (at && out[at - 1] == ' ')
        at--;
    out[at] = '\0';
}

static struct task *find(struct tasktab *t, const char *id)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].id, id))
            return &t->v[i];
    return NULL;
}

static struct task *add(struct tasktab *t, const char *id)
{
    if (t->n < TASKS_MAX) {
        struct task *a = &t->v[t->n++];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    for (int i = 0; i < t->n; i++) {
        if (!tasks_done(&t->v[i]))
            continue;
        memmove(&t->v[i], &t->v[i + 1], (size_t)(t->n - i - 1) * sizeof t->v[0]);
        struct task *a = &t->v[t->n - 1];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    return NULL;
}

static const struct task *note_lifecycle(struct tasktab *t, const backend_event *ev,
                                         int *repeat)
{
    struct task *prev = find(t, ev->id);
    int          was_done = prev && tasks_done(prev);

    struct task *a = prev;
    int          fresh = 0;
    if (!a) {
        if (!(a = add(t, ev->id)))
            return NULL;
        a->started = time(NULL);
        snprintf(a->status, sizeof a->status, "running");
        fresh = 1;
    }
    if (ev->arg && *ev->arg)
        snprintf(a->type, sizeof a->type, "%s", ev->arg);
    if (ev->parent && *ev->parent && !a->parent[0])
        snprintf(a->parent, sizeof a->parent, "%s", ev->parent);
    if (ev->text && *ev->text) {
        if (!a->desc[0])
            text_trunc(a->desc, sizeof a->desc, ev->text);
        else
            text_trunc(a->latest, sizeof a->latest, ev->text);
    }

    int changed = fresh;
    if (ev->name && *ev->name && strcmp(a->status, ev->name) &&
        !(tasks_done(a) && !strcmp(ev->name, "running"))) {
        snprintf(a->status, sizeof a->status, "%s", ev->name);
        changed = 1;
    }
    for (int i = 0; !a->cmd[0] && ev->parent && i < TASKS_MAX; i++)
        if (t->shell[i].id[0] && !strcmp(t->shell[i].id, ev->parent))
            snprintf(a->cmd, sizeof a->cmd, "%s", t->shell[i].cmd);
    if (tasks_done(a) && !a->ended)
        a->ended = time(NULL);

    if (was_done && !changed && a->repeats++ >= 1 && repeat)
        *repeat = 1;
    return changed ? a : NULL;
}

static int is_spawn_tool(const char *name)
{
    static const char *const spawn[] = {
        "task", "agent", "spawn_subagent", "spawn_agent", "workflow", NULL
    };
    char lower[64];
    toolstyle_label(lower, sizeof lower, name);
    for (int i = 0; spawn[i]; i++)
        if (!strcmp(lower, spawn[i]))
            return 1;
    return 0;
}

static const char *spawn_desc(const cJSON *in)
{
    static const char *const keys[] = {"description", "prompt", "task", "instructions"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(in, keys[i]));
        if (v && *v)
            return v;
    }
    return NULL;
}

static void note_shell(struct tasktab *t, const backend_event *ev)
{
    if (!ev->id || !*ev->id || !ev->input_json || !toolstyle_is_shell(ev->name))
        return;
    cJSON      *in = cJSON_Parse(ev->input_json);
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(in, "command"));
    if (cmd && *cmd) {
        int i = t->shell_next++ % TASKS_MAX;
        snprintf(t->shell[i].id, sizeof t->shell[i].id, "%s", ev->id);
        text_one_line(cmd, t->shell[i].cmd, sizeof t->shell[i].cmd);
    }
    cJSON_Delete(in);
}

static const struct task *note_launch(struct tasktab *t, const backend_event *ev)
{
    if (!ev->name || !is_spawn_tool(ev->name))
        return NULL;

    char id[40];
    snprintf(id, sizeof id, "%s-%d", t->tag[0] ? t->tag : "task", ++t->seq);
    struct task *a = add(t, id);
    if (!a)
        return NULL;

    a->started = time(NULL);
    a->inferred = 1;
    snprintf(a->status, sizeof a->status, "launched");

    cJSON      *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *d = in ? spawn_desc(in) : ev->arg;
    text_trunc(a->desc, sizeof a->desc, d ? d : ev->name);
    cJSON_Delete(in);
    return a;
}

const struct task *tasks_note(struct tasktab *t, const backend_event *ev, int *repeat)
{
    if (repeat)
        *repeat = 0;
    if (!t || !ev)
        return NULL;

    if (ev->kind == BACKEND_EV_TASK) {
        t->lifecycle = 1;
        if (!ev->id || !*ev->id)
            return NULL;
        return note_lifecycle(t, ev, repeat);
    }
    if (ev->kind == BACKEND_EV_TOOL)
        note_shell(t, ev);
    if (ev->kind == BACKEND_EV_TOOL && !t->lifecycle)
        return note_launch(t, ev);
    return NULL;
}

int tasks_drop(struct tasktab *t)
{
    int n = 0;
    for (int i = 0; t && i < t->n; i++) {
        struct task *a = &t->v[i];
        if (a->inferred || tasks_done(a))
            continue;
        snprintf(a->status, sizeof a->status, "dropped");
        a->ended = time(NULL);
        n++;
    }
    return n;
}

void tasks_duration(char *out, size_t size, long secs)
{
    if (secs < 0)
        secs = 0;
    if (secs < 60)
        snprintf(out, size, "%lds", secs);
    else if (secs < 3600)
        snprintf(out, size, "%ldm%02lds", secs / 60, secs % 60);
    else
        snprintf(out, size, "%ldh%02ldm", secs / 3600, (secs % 3600) / 60);
}

void tasks_line(const struct task *a, char *out, size_t size, size_t *cmd_at, size_t *cmd_len)
{
    char took[32] = "";
    char what[160];
    char head[48];

    if (cmd_at)
        *cmd_at = 0;
    if (cmd_len)
        *cmd_len = 0;
    if (!a) {
        snprintf(out, size, "%s", "");
        return;
    }
    if (tasks_done(a))
        tasks_duration(took, sizeof took, (long)(a->ended - a->started));
    text_one_line(a->cmd[0] ? a->cmd : a->desc[0] ? a->desc : a->id, what, sizeof what);
    snprintf(head, sizeof head, "%s %s: ", a->cmd[0] ? "bash" : "agent", a->status);
    snprintf(out, size, "%s%s%s%s", head, what, took[0] ? " in " : "", took);
    size_t at = strlen(head), len = strlen(out);
    if (a->cmd[0] && at < len) {
        if (cmd_at)
            *cmd_at = at;
        if (cmd_len)
            *cmd_len = len - at < strlen(what) ? len - at : strlen(what);
    }
}
