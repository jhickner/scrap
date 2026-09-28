#include "newsession.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ask.h"
#include "cmd.h"
#include "dirpick.h"
#include "hud.h"
#include "intercom.h"
#include "pick.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "workspace.h"

int newsession_spawn(const char *backend, const char *model, const char *effort,
                     const char *cwd, const char *name)
{
    struct session *here = workspace_current();
    if (!cwd)
        cwd = here ? session_cwd(here) : NULL;
    int at = workspace_spawn(backend, model, effort, cwd, NULL);
    if (at < 0) {
        const char *why = session_start_error();
        if (why)
            ui_error("could not start %s: %s", backend, why);
        else
            ui_error("could not start the %s CLI", backend);
        ui_put("\n");
        ui_flush();
        return -1;
    }
    if (name && *name && !session_set_name(workspace_current(), name))
        ui_error("the name '%s' is taken; kept @%s", name, session_name(workspace_current()));
    hud_print(workspace_current());
    ui_flush();
    return at;
}

static int index_of(const struct pick_item *items, int count, const char *label)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, label))
            return i;
    return 0;
}

static void set(char *out, size_t cap, const char *label)
{
    snprintf(out, cap, "%s", strcmp(label, "default") ? label : "");
}

int newsession_run(void)
{
    struct session *here = workspace_current();
    char backend[64], model[128] = "", effort[64] = "", cwd[4096] = "";
    char name[INTERCOM_NAME_MAX] = "", note[INTERCOM_NAME_MAX + 32] = "";
    snprintf(backend, sizeof backend, "%s", cmd_default_backend());
    if (here)
        snprintf(cwd, sizeof cwd, "%s", session_cwd(here));

    for (;;) {
        char rows[5][4200], shown[4096];
        path_home_relative(cwd, shown, sizeof shown);
        snprintf(rows[0], sizeof rows[0], "backend  %s", backend);
        snprintf(rows[1], sizeof rows[1], "model    %s", *model ? model : "default");
        snprintf(rows[2], sizeof rows[2], "effort   %s", *effort ? effort : "default");
        snprintf(rows[3], sizeof rows[3], "folder   %s", shown);
        snprintf(rows[4], sizeof rows[4], "name     %s%s", *name ? name : "generated", note);
        const struct pick_item form[] = {
            {"start", NULL},  {rows[0], NULL}, {rows[1], NULL},
            {rows[2], NULL},  {rows[3], NULL}, {rows[4], NULL},
        };

        int at = pick_run("new session", form, 6, 0);
        if (at < 0)
            return 1;

        int count = 0, which;
        const struct pick_item *list;
        switch (at) {
        case 0:
            newsession_spawn(backend, *model ? model : NULL, *effort ? effort : NULL,
                             *cwd ? cwd : NULL, name);
            return 0;
        case 1:
            list = cmd_backend_choices(&count);
            which = pick_run("backend", list, count, index_of(list, count, backend));
            if (which >= 0 && strcmp(list[which].label, backend)) {
                snprintf(backend, sizeof backend, "%s", list[which].label);
                model[0] = effort[0] = 0;
            }
            break;
        case 2:
            list = cmd_model_choices(backend, &count);
            if (!count) {
                char *typed = ask_run("model", model);
                if (typed) {
                    text_chomp(typed);
                    set(model, sizeof model, typed);
                }
                free(typed);
                break;
            }
            which = pick_run_filter("model", list, count,
                                    index_of(list, count, *model ? model : "default"));
            if (which >= 0)
                set(model, sizeof model, list[which].label);
            break;
        case 3:
            list = cmd_effort_choices(backend, &count);
            if (count < 2)
                break;
            which = pick_run("effort", list, count,
                             index_of(list, count, *effort ? effort : "default"));
            if (which >= 0)
                set(effort, sizeof effort, list[which].label);
            break;
        case 4: {
            char *dir = dirpick_run("folder");
            if (dir)
                snprintf(cwd, sizeof cwd, "%s", dir);
            free(dir);
            break;
        }
        case 5: {
            char *typed = ask_run("name", name);
            if (!typed)
                break;
            text_chomp(typed);
            const char *want = typed + (typed[0] == '@');
            note[0] = '\0';
            if (!*want)
                name[0] = '\0';
            else if (!intercom_name_valid(want))
                snprintf(note, sizeof note, "  (bad name '%s': letters, digits, - and _ only)", want);
            else if (intercom_name_taken(want, NULL))
                snprintf(note, sizeof note, "  ('%s' is taken)", want);
            else
                snprintf(name, sizeof name, "%s", want);
            free(typed);
            break;
        }
        }
    }
}
