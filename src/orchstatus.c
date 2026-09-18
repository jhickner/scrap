#include "orchstatus.h"

#include <stdlib.h>
#include <string.h>

static time_t last_change(const struct orch_rec *t)
{
    return t->updated ? t->updated : t->created;
}

static int order_recent(const void *a, const void *b)
{
    const struct orch_rec *x = a, *y = b;
    time_t tx = last_change(x), ty = last_change(y);
    if (tx != ty)
        return tx < ty ? 1 : -1;
    int project = strcmp(x->project, y->project);
    if (project)
        return project;
    return strcmp(x->id, y->id);
}

static int order_grouped(const void *a, const void *b)
{
    const struct orch_rec *x = a, *y = b;
    int project = strcmp(x->project, y->project);
    if (project)
        return project;
    return order_recent(a, b);
}

void orchstatus_sort(struct orch_rec *recs, int n, int include_closed)
{
    qsort(recs, (size_t)n, sizeof *recs, include_closed ? order_recent : order_grouped);
}

static void shrink(int *width, int minimum, int *need)
{
    int take = *width - minimum;
    if (take > *need)
        take = *need;
    *width -= take;
    *need -= take;
}

void orchstatus_status(char *out, size_t size, const struct orch_rec *t)
{
    char pending[16] = "";
    if (t->pending > 0)
        snprintf(pending, sizeof pending, " +%d", t->pending);

    if (!strcmp(t->status, "dispatched") && t->live[0])
        snprintf(out, size, "%s/%s%s", t->status, t->live, pending);
    else
        snprintf(out, size, "%s%s", t->status, pending);
}

void orchstatus_task(char *out, size_t size, const struct orch_rec *t)
{
    const char *text = t->desc[0] ? t->desc : t->id;
    if (t->checkpoint)
        snprintf(out, size, "checkpoint: %s", text);
    else
        snprintf(out, size, "%s", text);
}

void orchstatus_agent(char *out, size_t size, const struct orch_rec *t)
{
    if (t->backend[0] && t->model[0])
        snprintf(out, size, "%s / %s", t->backend, t->model);
    else if (t->backend[0] || t->model[0])
        snprintf(out, size, "%s%s", t->backend, t->model);
    else
        snprintf(out, size, "-");
}

static void fit(int *width, size_t len, int cap)
{
    if ((int)len > *width)
        *width = (int)len < cap ? (int)len : cap;
}

void orchstatus_columns(struct orchstatus_columns *out, int columns,
                        const struct orch_rec *tasks, int count)
{
    *out = (struct orchstatus_columns){
        .project = 7,
        .id = 2,
        .task = 4,
        .status = 6,
        .agent = 15,
        .age = 3,
    };
    char buf[256];
    for (int i = 0; i < count; i++) {
        fit(&out->project, strlen(tasks[i].project), 16);
        fit(&out->id, strlen(tasks[i].id), 16);
        orchstatus_status(buf, sizeof buf, &tasks[i]);
        fit(&out->status, strlen(buf), 24);
        orchstatus_agent(buf, sizeof buf, &tasks[i]);
        fit(&out->agent, strlen(buf), 24);
    }

    /* Leave room for the indent, separators, and terminal right margin. The id
       column is never shrunk: a truncated id cannot be used to name a task. */
    int available = columns - 8;
    if (available < 13 + out->id)
        available = 13 + out->id;
    int task = available - out->project - out->id - out->status - out->agent - out->age;
    if (task < 24) {
        int need = 24 - task;
        shrink(&out->agent, 10, &need);
        shrink(&out->status, 10, &need);
        shrink(&out->project, 8, &need);
        task = available - out->project - out->id - out->status - out->agent - out->age;
    }
    if (task < 4) {
        int need = 4 - task;
        shrink(&out->agent, 2, &need);
        shrink(&out->status, 2, &need);
        shrink(&out->project, 2, &need);
        task = available - out->project - out->id - out->status - out->agent - out->age;
    }
    out->task = task;
}

const char *orchstatus_wrap(char *out, size_t size, const char *in, size_t width)
{
    while (*in == ' ' || *in == '\n' || *in == '\r' || *in == '\t')
        in++;
    size_t limit = width < size - 1 ? width : size - 1;
    size_t len = strlen(in);
    size_t take = len;
    if (len > limit) {
        take = limit;
        while (take > 0 && !strchr(" \n\r\t", in[take]))
            take--;
        if (take == 0) {
            take = limit;
            while (take > 0 && ((unsigned char)in[take] & 0xc0) == 0x80)
                take--;
        }
    }
    for (size_t i = 0; i < take; i++)
        out[i] = in[i] == '\n' || in[i] == '\r' || in[i] == '\t' ? ' ' : in[i];
    while (take > 0 && out[take - 1] == ' ')
        take--;
    out[take] = '\0';
    in += take;
    while (*in == ' ' || *in == '\n' || *in == '\r' || *in == '\t')
        in++;
    return in;
}

void orchstatus_cell(char *out, size_t size, const char *in, size_t width)
{
    size_t at = 0;
    for (; in && *in && at < width && at + 1 < size; in++) {
        char c = *in;
        out[at++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    }
    if (in && *in && at >= 3) {
        out[at - 3] = '.';
        out[at - 2] = '.';
        out[at - 1] = '.';
    }
    out[at] = '\0';
}
