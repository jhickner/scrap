#include "muxcfg.h"

#include "muxmake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "ask.h"
#include "cmd.h"
#include "pick.h"
#include "text.h"
#include "ui.h"
#include "vendor/agents/backend.h"

static int pick_field(const char *title, const struct pick_item *items, int count,
                      char *out, size_t cap, int filter)
{
    int initial = 0;
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, *out ? out : "default"))
            initial = i;

    int index = filter ? pick_run_filter(title, items, count, initial)
                       : pick_run(title, items, count, initial);
    if (index < 0)
        return 0;
    snprintf(out, cap, "%s", strcmp(items[index].label, "default") ? items[index].label : "");
    return 1;
}

static int pick_model(struct mux_spec *m)
{
    int                     count = 0;
    const struct pick_item *items = cmd_model_choices(m->backend, &count);
    return pick_field("model", items, count, m->model, sizeof m->model, 1);
}

static int pick_effort(struct mux_spec *m)
{
    int                     count = 0;
    const struct pick_item *items = cmd_effort_choices(m->backend, &count);
    return pick_field("effort", items, count, m->effort, sizeof m->effort, 0);
}

static int pick_backend(char *out, size_t cap, const char *current)
{
    struct pick_item items[16];
    int              count = 0;

    for (const char *const *p = backend_names(); *p && count < (int)COUNT(items); p++)
        items[count++] = (struct pick_item){*p, NULL};
    if (!count)
        return 0;

    int initial = 0;
    for (int i = 0; i < count; i++)
        if (current && !strcmp(items[i].label, current))
            initial = i;

    int index = pick_run("backend", items, count, initial);
    if (index < 0)
        return 0;
    snprintf(out, cap, "%s", items[index].label);
    return 1;
}

static void row_prompt(struct mux_spec *m)
{
    char title[160];
    char label[80];
    muxcfg_label(m, label, sizeof label);
    snprintf(title, sizeof title, "%s \xc2\xb7 always tell it", label);

    char *text = ask_run(title, m->prompt);
    if (!text)
        return;
    snprintf(m->prompt, sizeof m->prompt, "%s", text);
    free(text);
}

static void row_backend(struct mux_spec *m)
{
    char was[32];
    snprintf(was, sizeof was, "%s", m->backend);

    if (!pick_backend(m->backend, sizeof m->backend, was) || !strcmp(was, m->backend))
        return;
    m->model[0] = m->effort[0] = '\0';
    pick_model(m);
    pick_effort(m);
}

static int add_row(struct mux_spec *v, int n)
{
    if (n >= MUX_MAX)
        return n;

    struct mux_spec m = {0};
    if (!pick_backend(m.backend, sizeof m.backend, NULL))
        return n;
    if (!pick_model(&m))
        return n;
    pick_effort(&m);
    v[n++] = m;
    return n;
}

static int row_duplicate(struct mux_spec *v, int n, int at)
{
    if (n >= MUX_MAX)
        return n;
    memmove(&v[at + 2], &v[at + 1], (size_t)(n - at - 1) * sizeof *v);
    v[at + 1] = v[at];
    return n + 1;
}

static int row_remove(struct mux_spec *v, int n, int at)
{
    memmove(&v[at], &v[at + 1], (size_t)(n - at - 1) * sizeof *v);
    return n - 1;
}

static int edit_row(struct mux_spec *v, int n, int at)
{
    static const struct pick_item ACTIONS[] = {
        {"backend", "answer from another CLI"},
        {"model", "answer with another model"},
        {"effort", "reasoning effort"},
        {"prompt", "standing instructions for this row"},
        {"duplicate", "another row on the same backend"},
        {"remove", "drop this row from the matrix"},
    };

    char title[160];
    muxcfg_label(&v[at], title, sizeof title);

    switch (pick_run(title, ACTIONS, (int)COUNT(ACTIONS), 0)) {
    case 0:
        row_backend(&v[at]);
        break;
    case 1:
        pick_model(&v[at]);
        break;
    case 2:
        pick_effort(&v[at]);
        break;
    case 3:
        row_prompt(&v[at]);
        break;
    case 4:
        n = row_duplicate(v, n, at);
        break;
    case 5:
        n = row_remove(v, n, at);
        break;
    }
    return n;
}

static int name_config(const char *title, char *out, size_t cap, int except)
{
    const struct mux_set *was = except >= 0 ? muxcfg_at(except) : NULL;

    char *text = ask_run(title, was ? was->name : NULL);
    if (!text)
        return 0;

    text_chomp(text);
    if (!*text || muxcfg_name_taken(text, except)) {
        free(text);
        return 0;
    }
    snprintf(out, cap, "%s", text);
    free(text);
    return 1;
}

static void configs_menu(void)
{
    for (;;) {
        char             labels[MUX_SETS][MUX_NAME + 8];
        char             details[MUX_SETS][32];
        struct pick_item items[MUX_SETS + 2];
        int              count = 0;
        int              nsets = muxcfg_count();
        int              active = muxcfg_index();

        for (int i = 0; i < nsets; i++) {
            const struct mux_set *s = muxcfg_at(i);
            snprintf(labels[i], sizeof labels[i], "%s%s",
                     i == active ? "\xe2\x97\x8f " : "  ", s->name);
            snprintf(details[i], sizeof details[i], "%d row%s", s->n,
                     s->n == 1 ? "" : "s");
            items[count++] = (struct pick_item){labels[i], details[i]};
        }
        if (nsets < MUX_SETS) {
            items[count++] = (struct pick_item){"new config", "an empty matrix"};
            items[count++] = (struct pick_item){"describe one",
                                                "say what should be in it"};
        }
        items[count++] = (struct pick_item){"back", NULL};

        int pressed = 0;
        int index = pick_run_keys("configs \xc2\xb7 n new, s describe, r rename, "
                                  "x remove",
                                  items, count, active, "nsrx", &pressed);
        if (index < 0)
            break;

        if (pressed == 's' || (!pressed && index == nsets + 1 && nsets < MUX_SETS)) {
            char *what = ask_run("what should be in it", NULL);
            if (what) {
                muxmake_run(what);
                free(what);
            }
            continue;
        }
        if (pressed == 'n' || (!pressed && index == nsets && nsets < MUX_SETS)) {
            char name[MUX_NAME];
            if (name_config("name it", name, sizeof name, -1) && muxcfg_new(name) >= 0)
                muxcfg_save();
            continue;
        }
        if (index >= nsets) {
            if (!pressed)
                break;
            continue;
        }

        switch (pressed) {
        case 'r': {
            struct mux_set *s = muxcfg_at(index);
            if (name_config("rename it", s->name, sizeof s->name, index))
                muxcfg_save();
            break;
        }
        case 'x':
            if (muxcfg_drop(index))
                muxcfg_save();
            break;
        default:
            muxcfg_select(index);
            muxcfg_save();
            return;
        }
    }
}

void muxcfg_run(void)
{
    struct mux_set *s = muxcfg_at(muxcfg_index());
    int             sel = 0;

    for (;;) {
        char             labels[MUX_MAX][160];
        char             details[MUX_MAX][MUX_PROMPT];
        char             config[MUX_NAME + 16];
        struct pick_item items[MUX_MAX + 3];
        int              count = 0;

        s = muxcfg_at(muxcfg_index());
        for (int i = 0; i < s->n; i++) {
            muxcfg_label(&s->row[i], labels[i], sizeof labels[i]);
            snprintf(details[i], sizeof details[i], "%s", s->row[i].prompt);
            items[count++] = (struct pick_item){labels[i], details[i]};
        }
        if (s->n < MUX_MAX)
            items[count++] = (struct pick_item){"add a row", "another backend and model"};
        snprintf(config, sizeof config, "config: %s", s->name);
        items[count++] = (struct pick_item){config, "switch, rename, or start another"};
        items[count++] = (struct pick_item){"done", NULL};

        if (sel >= count)
            sel = count - 1;

        int pressed = 0;
        int index = pick_run_keys("mux matrix \xc2\xb7 a add, d duplicate, b backend, "
                                  "m model, e effort, p prompt, x remove, c configs",
                                  items, count, sel, "admbepxc", &pressed);
        if (index < 0)
            break;
        sel = index;

        if (pressed == 'c') {
            configs_menu();
            sel = 0;
            continue;
        }
        if (pressed == 'a') {
            s->n = add_row(s->row, s->n);
            sel = s->n > 0 ? s->n - 1 : 0;
        } else if (index >= s->n) {
            if (pressed)
                continue;
            if (index == count - 1)
                break;
            if (index == count - 2) {
                configs_menu();
                sel = 0;
                continue;
            }
            s->n = add_row(s->row, s->n);
        } else {
            switch (pressed) {
            case 'd':
                s->n = row_duplicate(s->row, s->n, index);
                break;
            case 'b':
                row_backend(&s->row[index]);
                break;
            case 'm':
                pick_model(&s->row[index]);
                break;
            case 'e':
                pick_effort(&s->row[index]);
                break;
            case 'p':
                row_prompt(&s->row[index]);
                break;
            case 'x':
                s->n = row_remove(s->row, s->n, index);
                if (sel >= s->n)
                    sel = s->n > 0 ? s->n - 1 : 0;
                break;
            default:
                s->n = edit_row(s->row, s->n, index);
                break;
            }
        }
        muxcfg_save();
    }

    muxcfg_save();

    s = muxcfg_at(muxcfg_index());
    ui_note("mux matrix \xc2\xb7 %s", s->name);
    ui_put("\n");
    for (int i = 0; i < s->n; i++) {
        char label[160];
        muxcfg_label(&s->row[i], label, sizeof label);
        if (*s->row[i].prompt)
            ui_note("%s \xe2\x80\x94 %s", label, s->row[i].prompt);
        else
            ui_note("%s", label);
        ui_put("\n");
    }
    ui_flush();
}
