#include "settingsui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ask.h"
#include "cmd.h"
#include "image.h"
#include "pick.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "ui.h"
#include "voice.h"

enum kind { S_FLAG, S_PERMISSION, S_ROWS, S_BACKEND, S_CHOICE };

struct entry {
    const char *name;
    const char *about;
    enum kind   kind;

    const char *off;
    const char *on;

    int         low;
    int         high;
    const char *unbounded;
    const char *key;
    int         def;

    const char *const *choices;
    int                nchoices;

};

static const char *const FOLDER_SORTS[] = {"name", "recent"};

static const struct entry ENTRIES[] = {
    {.name = "reasoning", .kind = S_FLAG,
     .about = "show what the model thinks on the way to an answer",
     .off = "hidden", .on = "shown"},
    {.name = "tool calls", .kind = S_FLAG,
     .about = "how much of each call is shown",
     .off = "full blocks with output", .on = "one row each"},
    {.name = "permission", .kind = S_PERMISSION,
     .about = "how the CLI gates tool calls"},
    {.name = "floating prompt", .kind = S_FLAG,
     .about = "keep what you typed above the spinner",
     .off = "off", .on = "on"},
    {.name = "name badge", .kind = S_FLAG,
     .about = "session name in the top-right corner",
     .off = "off", .on = "on",
     .key = SETTING_NAME_BADGE, .def = 1},
    {.name = "stamp", .kind = S_FLAG,
     .about = "banner over the screen when the current session finishes a turn",
     .off = "off", .on = "on",
     .key = SETTING_STAMP, .def = 1},
    {.name = "stamp check", .kind = S_FLAG,
     .about = "stamp shows a check mark instead of a phrase",
     .off = "off", .on = "on",
     .key = SETTING_STAMP_CHECK, .def = 0},
    {.name = "voice", .kind = S_FLAG,
     .about = "read replies aloud",
     .off = "off", .on = "on",
     .key = SETTING_VOICE, .def = 0},
    {.name = "voice complete", .kind = S_FLAG,
     .about = "say the session name when a turn ends unfocused",
     .off = "off", .on = "on",
     .key = SETTING_VOICE_COMPLETE, .def = 1},
    {.name = "voice volume", .kind = S_ROWS,
     .about = "how loud spoken replies are, as a percent",
     .low = 0, .high = 100,
     .key = SETTING_VOICE_VOLUME, .def = VOICE_VOLUME_DEFAULT},
    {.name = "image rows", .kind = S_ROWS,
     .about = "tallest an inline image may be drawn",
     .low = IMAGE_ROWS_MIN, .high = IMAGE_ROWS_MAX},
    {.name = "echoed rows", .kind = S_ROWS,
     .about = "how much of a long typed line is echoed back",
     .low = 0, .high = 100, .unbounded = "all of it",
     .key = SETTING_ECHO_ROWS, .def = ECHO_ROWS_DEFAULT},
    {.name = "task stall seconds", .kind = S_ROWS,
     .about = "wait before a nudge when background tasks end with no reply",
     .low = 0, .high = 600, .unbounded = "no nudge",
     .key = SETTING_TASK_STALL, .def = TASK_STALL_DEFAULT},
    {.name = "status seconds", .kind = S_ROWS,
     .about = "interval between status updates during a long turn",
     .low = 0, .high = 1800, .unbounded = "no updates",
     .key = SETTING_STATUS_INTERVAL, .def = STATUS_INTERVAL_DEFAULT},
    {.name = "auto-handoff", .kind = S_ROWS,
     .about = "context percent that hands off to a fresh conversation",
     .low = 0, .high = AUTO_HANDOFF_MAX, .unbounded = "off",
     .key = SETTING_AUTO_HANDOFF, .def = 0},
    {.name = "auto-backend", .kind = S_ROWS,
     .about = "quota percent that switches Claude to Codex to Grok",
     .low = 0, .high = 100, .unbounded = "off",
     .key = SETTING_AUTO_BACKEND, .def = 0},
    {.name = "jump length", .kind = S_ROWS,
     .about = "sessions ctrl-o steps back through",
     .low = 1, .high = JUMP_LENGTH_MAX,
     .key = SETTING_JUMP_LENGTH, .def = JUMP_LENGTH_DEFAULT},
    {.name = "folder sort", .kind = S_CHOICE,
     .about = "order of working folders in /sessions: by name, or most recently active first",
     .key = SETTING_FOLDER_SORT, .choices = FOLDER_SORTS, .nchoices = (int)COUNT(FOLDER_SORTS)},
    {.name = "backend", .kind = S_BACKEND,
     .about = "the CLI scrap starts on"},
};

static const char *choice_of(const struct entry *e)
{
    const char *now = settings_get_str(e->key, e->choices[0]);
    for (int i = 0; i < e->nchoices; i++)
        if (!strcmp(now, e->choices[i]))
            return e->choices[i];
    return e->choices[0];
}

static int flag_of(const struct session *s, int at)
{
    const struct entry *e = &ENTRIES[at];
    if (e->key) {
        if (!strcmp(e->key, SETTING_VOICE))
            return voice_on();
        return settings_get_int(e->key, e->def);
    }
    switch (at) {
    case 0:
        return session_thinking(s);
    case 1:
        return session_compact(s);
    default:
        return status_sticky_enabled();
    }
}

static void flag_set(struct session *s, int at, int on)
{
    const struct entry *e = &ENTRIES[at];
    if (e->key) {
        if (!strcmp(e->key, SETTING_VOICE)) {
            voice_set_on(on);
            return;
        }
        settings_set_int(e->key, on);
        return;
    }
    switch (at) {
    case 0:
        session_set_thinking(s, on);
        settings_set_int(SETTING_THINKING, on);
        break;
    case 1:
        session_set_compact(s, on);
        settings_set_int(SETTING_COMPACT, on);
        break;
    default:
        status_sticky_set(on);
        settings_set_int(SETTING_STICKY, on);
        break;
    }
}

static int rows_of(const struct entry *e)
{
    return e->key ? settings_get_int(e->key, e->def) : image_rows();
}

static void rows_set(const struct entry *e, int rows)
{
    if (!e->key) {
        image_set_rows(rows);
        settings_set_int(SETTING_IMAGE_ROWS, image_rows());
        return;
    }
    if (!strcmp(e->key, SETTING_VOICE_VOLUME)) {
        voice_set_volume(rows);
        return;
    }
    settings_set_int(e->key, rows);
}

static void value_of(const struct session *s, int at, char *out, size_t cap)
{
    const struct entry *e = &ENTRIES[at];

    switch (e->kind) {
    case S_FLAG:
        snprintf(out, cap, "%s", flag_of(s, at) ? e->on : e->off);
        break;
    case S_PERMISSION: {
        int index = session_permission_index(session_permission(s));
        snprintf(out, cap, "%s", session_permission_name(index < 0 ? 0 : index));
        break;
    }
    case S_ROWS: {
        int rows = rows_of(e);
        if (!rows && e->unbounded)
            snprintf(out, cap, "%s", e->unbounded);
        else
            snprintf(out, cap, "%d", rows);
        break;
    }
    case S_BACKEND:
        snprintf(out, cap, "%s", cmd_default_backend());
        break;
    case S_CHOICE:
        snprintf(out, cap, "%s", choice_of(e));
        break;
    }
}

static void edit_permission(struct session *s)
{
    struct pick_item items[8];
    int              count = session_permission_count();

    if (count > (int)COUNT(items))
        count = (int)COUNT(items);
    for (int i = 0; i < count; i++)
        items[i] = (struct pick_item){session_permission_name(i),
                                      session_permission_desc(i)};

    int now = session_permission_index(session_permission(s));
    int index = pick_run("gate tool calls", items, count, now < 0 ? 0 : now);
    if (index < 0 || index == now)
        return;

    if (session_set_permission(s, session_permission_name(index)))
        settings_set_int(SETTING_PERMISSION, index);
}

static void edit_rows(const struct entry *e)
{
    char title[160];
    snprintf(title, sizeof title, "%s \xc2\xb7 %d to %d", e->name, e->low, e->high);

    for (int tries = 0; tries < 3; tries++) {
        char now[16];
        snprintf(now, sizeof now, "%d", rows_of(e));

        char *text = ask_run(title, now);
        if (!text)
            return;

        text_chomp(text);
        char *end;
        long  rows = strtol(text, &end, 10);
        int   ok = end != text && !*end && rows >= e->low && rows <= e->high;
        free(text);

        if (ok) {
            rows_set(e, (int)rows);
            return;
        }
        snprintf(title, sizeof title, "%s \xe2\x80\x94 a number from %d to %d",
                 e->name, e->low, e->high);
    }
}

static void edit_backend(void)
{
    int count = 0, initial = 0;
    const struct pick_item *items = cmd_backend_choices(&count);
    const char *now = cmd_default_backend();
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, now))
            initial = i;

    int index = pick_run("default backend", items, count, initial);
    if (index < 0 || !strcmp(items[index].label, now))
        return;
    settings_set_str(SETTING_BACKEND, items[index].label);
}

static void edit_choice(const struct entry *e)
{
    struct pick_item items[8];
    int              count = e->nchoices > (int)COUNT(items) ? (int)COUNT(items) : e->nchoices;
    const char      *now = choice_of(e);
    int              initial = 0;

    for (int i = 0; i < count; i++) {
        items[i] = (struct pick_item){e->choices[i], NULL};
        if (!strcmp(e->choices[i], now))
            initial = i;
    }
    int index = pick_run(e->name, items, count, initial);
    if (index < 0 || !strcmp(e->choices[index], now))
        return;
    settings_set_str(e->key, e->choices[index]);
}

static void edit(struct session *s, int at)
{
    const struct entry *e = &ENTRIES[at];

    switch (e->kind) {
    case S_FLAG:
        flag_set(s, at, !flag_of(s, at));
        break;
    case S_PERMISSION:
        edit_permission(s);
        break;
    case S_ROWS:
        edit_rows(e);
        break;
    case S_BACKEND:
        edit_backend();
        break;
    case S_CHOICE:
        edit_choice(e);
        break;
    }
}

void settingsui_run(struct session *s)
{
    int sel = 0;

    for (;;) {
        char             labels[COUNT(ENTRIES)][96];
        struct pick_item items[COUNT(ENTRIES) + 1];
        int              count = 0;

        for (int i = 0; i < (int)COUNT(ENTRIES); i++) {
            char value[64];
            value_of(s, i, value, sizeof value);
            snprintf(labels[i], sizeof labels[i], "%-16s %s", ENTRIES[i].name, value);
            items[count++] = (struct pick_item){labels[i], ENTRIES[i].about};
        }
        items[count++] = (struct pick_item){"done", NULL};

        int index = pick_run(APP_NAME " settings", items, count, sel);
        if (index < 0 || index == count - 1)
            break;
        sel = index;
        edit(s, index);
    }
}
