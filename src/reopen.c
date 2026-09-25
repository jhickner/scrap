#include "reopen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "livelist.h"
#include "pick.h"
#include "session.h"
#include "tabs.h"
#include "text.h"
#include "workspace.h"

#define ROWS_MAX 256

struct row {
    long          pid;
    char          label[4400];
    char          detail[280];
    unsigned char kind;
};

static int window_slots(const struct live_session *list, int n, long pid, int *out,
                        int max)
{
    int count = 0;

    for (int i = 0; i < n && count < max; i++) {
        if (list[i].pid != pid || !list[i].id[0] || !list[i].backend[0])
            continue;
        int at = count++;
        for (; at > 0 && list[out[at - 1]].slot > list[i].slot; at--)
            out[at] = out[at - 1];
        out[at] = i;
    }
    return count;
}

static int build(const struct live_session *list, int n, struct row *rows, int max)
{
    long seen[ROWS_MAX];
    int  nseen = 0;
    int  m = 0;

    for (int i = 0; i < n && m < max; i++) {
        int known = 0;
        for (int j = 0; j < nseen; j++)
            known = known || seen[j] == list[i].pid;
        if (known)
            continue;
        if (nseen < ROWS_MAX)
            seen[nseen++] = list[i].pid;

        int mine[ROWS_MAX];
        int count = window_slots(list, n, list[i].pid, mine, ROWS_MAX);
        if (!count)
            continue;

        char ago[32];
        text_ago((time_t)list[i].ts, 1, ago, sizeof ago);

        struct row *head = &rows[m++];
        head->pid = list[i].pid;
        head->kind = PICK_HEADING;
        if (list[i].wname[0])
            snprintf(head->label, sizeof head->label, "%s \xc2\xb7 %s",
                     list[i].wname, ago);
        else
            snprintf(head->label, sizeof head->label, "%s", ago);

        struct row *take = &rows[m++];
        take->pid = list[i].pid;
        take->kind = 0;
        snprintf(take->label, sizeof take->label, "%d session%s", count,
                 count == 1 ? "" : "s");

        for (int k = 0; k < count && m < max; k++) {
            const struct live_session *v = &list[mine[k]];
            struct row *r = &rows[m++];
            char where[4200];
            r->pid = v->pid;
            r->kind = PICK_TEXT;
            path_home_relative(v->cwd, where, sizeof where);
            snprintf(r->label, sizeof r->label, "%s \xc2\xb7 %s", where,
                     v->title[0] ? v->title : "untitled");
        }
    }
    return m;
}

static int reopen_window(const struct live_session *list, int n, long pid)
{
    int mine[ROWS_MAX];
    int count = window_slots(list, n, pid, mine, ROWS_MAX);
    int queued = 0;

    for (int i = 0; i < count; i++) {
        const struct live_session *v = &list[mine[i]];
        if (workspace_count() + queued >= WORKSPACE_MAX)
            break;

        struct session *s = workspace_prepare(v->backend, v->model, v->effort,
                                              v->cwd, v->id);
        if (!s)
            continue;
        if (!tabs_queue(s, NULL)) {
            session_free(s);
            break;
        }
        queued++;
    }

    if (queued)
        tabs_start_queued();
    return queued;
}

int reopen_available(void)
{
    struct live_session *list = NULL;
    int n = livelist_closed_load(&list);
    free(list);
    return n > 0;
}

int reopen_run(void)
{
    struct live_session *list = NULL;
    int n = livelist_closed_load(&list);
    if (n <= 0)
        return 0;

    struct row *rows = calloc(ROWS_MAX, sizeof *rows);
    struct pick_item *items = calloc(ROWS_MAX, sizeof *items);
    unsigned char *heading = calloc(ROWS_MAX, 1);
    int opened = 0;

    if (!rows || !items || !heading)
        goto done;

    int m = build(list, n, rows, ROWS_MAX);
    for (int i = 0; i < m; i++) {
        items[i].label = rows[i].label;
        items[i].detail = rows[i].detail;
        heading[i] = rows[i].kind;
    }

    struct pick_live shown = {.heading = heading, .align = 1};
    int at = pick_run_live("reopen which window", items, m, 0, &shown,
                           PICK_SEARCH_TYPE, NULL, NULL);
    if (at >= 0 && at < m)
        opened = reopen_window(list, n, rows[at].pid);

done:
    free(heading);
    free(items);
    free(rows);
    free(list);
    return opened;
}
