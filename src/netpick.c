#include "netpick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netd.h"
#include "ask.h"
#include "cmd.h"
#include "tailnet.h"
#include "intercom.h"
#include "keyhelp.h"
#include "newsession.h"
#include "pick.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

#define MAX_ROWS 256
#define KEY_SEND 's'
#define SURVEY_SETTLE_MS 150

static const struct keyhelp_row NET_KEYS[] = {
    {"GO", "enter/\xe2\x86\x92", "open in a tab"},
    {"GO", "s", "send a message"},
    {"GO", "tab/esc", "close the list"},
    {"LIST", "up/down", "move"},
    {"LIST", "/", "search"},
};

struct row {
    char label[128];
    char detail[1200];
    char target[TAILNET_HOST_MAX + INTERCOM_NAME_MAX + 2];
    char id[128];
    char machine[TAILNET_HOST_MAX];
    int  spawn;
    int  self;
};

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

static int build(cJSON *machines, struct row *rows, unsigned char *heading, unsigned char *spin,
                 const char **marks, unsigned char *roles)
{
    int    n = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, machines)
    {
        const char *machine = jstr(m, "machine"), *error = jstr(m, "error");
        int         self = cJSON_IsTrue(cJSON_GetObjectItem(m, "self"));
        cJSON      *list = cJSON_GetObjectItem(m, "sessions");
        if (n >= MAX_ROWS)
            break;
        heading[n] = PICK_HEADING;
        if (*error)
            snprintf(rows[n].label, sizeof rows[n].label, "%s \xc2\xb7 %s", machine, error);
        else if (!cJSON_GetArraySize(list))
            snprintf(rows[n].label, sizeof rows[n].label, "%s \xc2\xb7 no live sessions", machine);
        else
            snprintf(rows[n].label, sizeof rows[n].label, "%s", machine);
        n++;
        cJSON *o;
        cJSON_ArrayForEach(o, list)
        {
            const char *name = jstr(o, "name");
            if (!*name || n >= MAX_ROWS)
                continue;
            char where[1024];
            path_home_relative(jstr(o, "cwd"), where, sizeof where);
            struct row *r = &rows[n];
            heading[n++] = 0;
            snprintf(r->label, sizeof r->label, "@%s", name);
            spin[n - 1] = !strcmp(jstr(o, "status"), "working");
            marks[n - 1] = !strcmp(jstr(o, "status"), "errored") ? "e" : "";
            roles[n - 1] = UI_ERROR;
            snprintf(r->detail, sizeof r->detail, "%s  %s", where,
                     *jstr(o, "title") ? jstr(o, "title") : "untitled");
            snprintf(r->id, sizeof r->id, "%s", self ? jstr(o, "id") : "");
            snprintf(r->target, sizeof r->target, "%s%s@%s", self ? "" : machine,
                     self ? "" : ":", name);
        }
        if (!*error && n < MAX_ROWS) {
            struct row *r = &rows[n];
            heading[n++] = 0;
            snprintf(r->label, sizeof r->label, "+ new session");
            snprintf(r->machine, sizeof r->machine, "%s", machine);
            r->spawn = 1;
            r->self = self;
        }
    }
    return n;
}

static void send_to(struct session *s, const char *target)
{
    char title[300];
    snprintf(title, sizeof title, "message to %s", target);
    char *text = ask_run(title, NULL);
    if (text && *text) {
        char msg[1200];
        if (intercom_send(s ? session_name(s) : NULL, target, text, msg, sizeof msg))
            ui_error("%s", msg);
        else
            ui_note("%s", msg);
        ui_put("\n");
        ui_flush();
    }
    free(text);
}

static void spawn_on(const struct row *r)
{
    if (r->self) {
        newsession_run();
        return;
    }
    char title[300];
    snprintf(title, sizeof title, "folder on %s", r->machine);
    char *cwd = ask_run(title, "~");
    if (!cwd)
        return;
    char target[sizeof r->target], msg[1200];
    ui_note("starting a session on %s\xe2\x80\xa6", r->machine);
    ui_flush();
    if (tailnet_spawn(r->machine, cwd, target, sizeof target, msg, sizeof msg))
        cmd_attach(target);
    else {
        ui_error("%s", msg);
        ui_put("\n");
        ui_flush();
    }
    free(cwd);
}

struct watch {
    struct tailnet_survey *survey;
    int                    version;
};

static int survey_tick(void *ud)
{
    struct watch *w = ud;
    return tailnet_survey_version(w->survey) != w->version ? PICK_TICK_REOPEN : 0;
}

static int find_row(const struct row *rows, int n, const struct row *want)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].label, want->label) && !strcmp(rows[i].target, want->target) &&
            !strcmp(rows[i].machine, want->machine))
            return i;
    return -1;
}

void netpick_run(struct session *s)
{
    netd_ensure();
    struct watch w = {.survey = tailnet_survey_start()};
    if (!w.survey) {
        ui_error("tailscale status is unavailable");
        ui_put("\n");
        ui_flush();
        return;
    }
    tailnet_survey_wait(w.survey, SURVEY_SETTLE_MS);
    struct row       *rows = calloc(MAX_ROWS, sizeof *rows);
    unsigned char    *heading = calloc(MAX_ROWS, 1);
    struct pick_item *items = calloc(MAX_ROWS, sizeof *items);
    unsigned char    *spin = calloc(MAX_ROWS, 1), *roles = calloc(MAX_ROWS, 1);
    const char      **marks = calloc(MAX_ROWS, sizeof *marks);
    struct row        was = {0};
    int               n = 0, picked = -1, pressed = 0, cursor = -1, pending = 0;
    for (;;) {
        cJSON *machines = tailnet_survey_result(w.survey, &w.version, &pending);
        memset(rows, 0, MAX_ROWS * sizeof *rows);
        memset(heading, 0, MAX_ROWS);
        memset(spin, 0, MAX_ROWS);
        n = rows && heading && items && spin && roles && marks
                ? build(machines, rows, heading, spin, marks, roles)
                : 0;
        cJSON_Delete(machines);

        int initial = cursor >= 0 ? find_row(rows, n, &was) : -1;
        for (int i = 0; initial < 0 && i < n; i++)
            if (!heading[i])
                initial = i;
        for (int i = 0; i < n; i++) {
            items[i].label = rows[i].label;
            items[i].detail = rows[i].detail;
        }

        char             shortcuts[] = {KEY_SEND, PICK_KEY_RIGHT, '\t', 0};
        struct pick_live shown = {.heading = heading, .spin = spin, .mark = marks,
                                  .mark_role = roles, .align = 1, .keys = NET_KEYS,
                                  .nkeys = (int)(sizeof NET_KEYS / sizeof *NET_KEYS),
                                  .tick = pending ? survey_tick : NULL, .ud = &w,
                                  .cursor = &cursor};
        picked = n ? pick_run_live("net", items, n, initial < 0 ? 0 : initial, &shown,
                                   PICK_SEARCH_SLASH, shortcuts, &pressed)
                   : -1;
        if (picked != PICK_REOPEN)
            break;
        if (cursor >= 0 && cursor < n)
            was = rows[cursor];
    }
    tailnet_survey_end(w.survey);

    char target[sizeof rows->target] = "", id[128] = "";
    struct row spawn = {0};
    if (picked >= 0 && !heading[picked]) {
        snprintf(target, sizeof target, "%s", rows[picked].target);
        snprintf(id, sizeof id, "%s", rows[picked].id);
        if (rows[picked].spawn)
            spawn = rows[picked];
    }
    free(items);
    free(rows);
    free(heading);
    free(spin);
    free(roles);
    free(marks);
    int here = id[0] ? workspace_find_id(id) : -1;
    if (spawn.spawn) {
        spawn_on(&spawn);
        return;
    }
    if (!target[0])
        return;
    if (pressed == KEY_SEND)
        send_to(s, target);
    else if (here >= 0)
        workspace_show(here);
    else
        cmd_attach(target);
}
