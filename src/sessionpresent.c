#include "sessionpresent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md.h"
#include "prompt.h"
#include "status.h"
#include "tasks.h"
#include "text.h"
#include "toolstyle.h"
#include "transcript.h"
#include "ui.h"
#include "viewport.h"

#define TASK_HOLD_SECONDS 3.0

static void stream_append(struct sessionpresent *p, const char *value)
{
    if (!value || !*value)
        return;
    size_t add = strlen(value);
    if (p->streamed_len + add + 1 > p->streamed_cap) {
        size_t want = p->streamed_cap ? p->streamed_cap : 1024;
        while (want < p->streamed_len + add + 1)
            want *= 2;
        char *grown = realloc(p->streamed, want);
        if (!grown)
            return;
        p->streamed = grown;
        p->streamed_cap = want;
    }
    memcpy(p->streamed + p->streamed_len, value, add + 1);
    p->streamed_len += add;
}

static void stream_reset(struct sessionpresent *p)
{
    free(p->streamed);
    p->streamed = NULL;
    p->streamed_len = p->streamed_cap = 0;
}

void sessionpresent_free(struct sessionpresent *p)
{
    if (!p)
        return;
    stream_reset(p);
    filediff_clear(&p->filediff);
}

void sessionpresent_turn_begin(struct sessionpresent *p)
{
    if (!p)
        return;
    stream_reset(p);
    p->view.after_collapse = 0;
    p->call_open = 0;
    view_keep_break();
}

void sessionpresent_turn_end(struct sessionpresent *p)
{
    if (p)
        p->call_open = 0;
}

const char *sessionpresent_streamed(const struct sessionpresent *p)
{
    return p ? p->streamed : NULL;
}

static void paint_note(const char *line)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", line);
    viewport_item_end();
}

static int task_hold(struct sessionpresent *p, const char *line)
{
    if (!p->call_open || p->task_held >= SESSIONPRESENT_TASK_HOLD_MAX)
        return 0;
    if (!p->task_held)
        p->task_held_at = now_seconds();
    snprintf(p->task_hold[p->task_held], sizeof p->task_hold[0], "%s", line);
    p->task_held++;
    return 1;
}

static int task_unhold(struct sessionpresent *p)
{
    int held = p->task_held;
    for (int i = 0; i < held; i++)
        paint_note(p->task_hold[i]);
    p->task_held = 0;
    return held;
}

void sessionpresent_expire(struct sessionpresent *p, int quiet)
{
    if (!p || !p->task_held || quiet)
        return;
    if (p->call_open && now_seconds() - p->task_held_at < TASK_HOLD_SECONDS)
        return;
    int hide = !viewport_held();
    if (hide)
        status_pause();
    task_unhold(p);
    if (hide)
        status_resume();
    ui_flush();
}

void sessionpresent_break(struct sessionpresent *p)
{
    if (p)
        p->view.after_collapse = 0;
    view_keep_break();
}

void sessionpresent_event(struct sessionpresent *p, const backend_event *ev,
                          const char *cwd, const struct tasktab *tasks,
                          const struct task *task_change, int thinking)
{
    if (!p || !ev)
        return;

    int  nested = ev->parent && *ev->parent;
    char whose[32] = "";
    if (nested)
        tasks_label(tasks_by_parent(tasks, ev->parent), whose, sizeof whose);
    view_keep_nest(nested, whose);

    int paused = 0;
    int hide = !viewport_held();
    if (p->task_held && !p->call_open) {
        if (hide) {
            status_pause();
            paused = 1;
        }
        task_unhold(p);
    }

    switch (ev->kind) {
    case BACKEND_EV_INIT:
    case BACKEND_EV_CWD:
    case BACKEND_EV_TRUST:
        break;

    case BACKEND_EV_TASK: {
        char line[240];
        if (!task_change || (p->call_open && !tasks_done(task_change)))
            break;
        tasks_line(task_change, line, sizeof line);
        if (task_hold(p, line))
            break;
        if (hide) {
            status_pause();
            paused = 1;
        }
        paint_note(line);
        break;
    }

    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            viewport_item_begin(VIEWPORT_ROWS(1, 1));
            ui_note("%s", ev->text);
            viewport_item_end();
        }
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        if (hide) {
            status_pause();
            paused = 1;
        }
        if (nested) {
            view_keep_break();
            view_keep_activity("", ev->text, UI_DIM);
        } else {
            md_render_kept(ev->text, 0);
            stream_append(p, ev->text);
            view_keep_break();
        }
        p->view.after_collapse = 0;
        break;

    case BACKEND_EV_THINKING:
        if (!thinking || !ev->text || !*ev->text)
            break;
        if (hide) {
            status_pause();
            paused = 1;
        }
        view_keep_break();
        view_keep_activity("\xe2\x9c\xbb", ev->text, UI_THINKING);
        p->view.after_collapse = 0;
        break;

    case BACKEND_EV_TOOL: {
        const char *name = ev->name ? ev->name : "?";
        char arg[4096];
        view_tool_argument(ev, cwd, arg, sizeof arg);
        int collapses = toolstyle_collapses(name, ev->input_json, ev->arg);
        if (hide) {
            status_pause();
            paused = 1;
        }
        if (!nested)
            p->call_open = 1;
        view_keep_tool_call(name, arg, collapses);

        char path[4096];
        if (!collapses && view_tool_path(ev->input_json, cwd, path, sizeof path))
            filediff_snapshot(&p->filediff, path);
        else
            filediff_clear(&p->filediff);
        p->view.after_collapse = collapses;
        break;
    }

    case BACKEND_EV_TOOL_RESULT:
        if (ev->failed) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            view_keep_break();
            filediff_clear(&p->filediff);
            const char *why = ev->text && *ev->text ? ev->text : NULL;
            if (!why || !strcmp(why, "failed")) {
                view_keep_output("failed", UI_ERROR, 0);
            } else {
                char line[4096];
                snprintf(line, sizeof line, "failed: %s", why);
                view_keep_output(line, UI_ERROR, 1);
            }
            p->view.after_collapse = 0;
            break;
        }

        if (!p->view.after_collapse) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            char *patch;
            if (ev->diff) {
                patch = strdup(ev->diff);
                filediff_clear(&p->filediff);
            } else {
                patch = filediff_take_patch(&p->filediff);
            }
            if (filediff_patch_draws(patch)) {
                view_keep_diff(patch);
            } else {
                free(patch);
                view_keep_output(ev->text, UI_DIM, 0);
            }
        }
        break;
    }

    if (ev->kind == BACKEND_EV_TOOL_RESULT && !nested) {
        p->call_open = 0;
        if (p->task_held) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            task_unhold(p);
        }
    }

    view_keep_nest(0, NULL);
    if (paused)
        status_resume();
    ui_flush();
}

void sessionpresent_replay(const struct transcript *transcript)
{
    if (!transcript)
        return;
    for (size_t i = 0; i < transcript->count; i++) {
        const struct transcript_turn *t = &transcript->turns[i];
        if (t->user && *t->user)
            prompt_echo_message(t->user);
        if (t->assistant && *t->assistant)
            md_render_kept(t->assistant, 0);
        if (t->interrupted) {
            viewport_item_begin(VIEWPORT_ROWS(0, 1));
            ui_error("  interrupted");
            viewport_item_end();
            ui_flush();
        }
    }
    ui_flush();
}

static int effort_is_off(const char *effort)
{
    return !effort || !*effort || !strcmp(effort, "none") ||
           !strcmp(effort, "off");
}

void sessionpresent_spin(const char *backend, const char *effort, double quiet,
                         double quiet_threshold)
{
    if (effort_is_off(effort)) {
        status_set_word("working");
    } else {
        char phrase[64];
        snprintf(phrase, sizeof phrase, "thinking with %s effort", effort);
        status_set_word(phrase);
    }

    if (quiet < quiet_threshold) {
        status_set_alert(NULL);
    } else {
        char since[32], text[64];
        text_duration(quiet, since, sizeof since);
        snprintf(text, sizeof text, "%s quiet for %s", backend, since);
        status_set_alert(text);
    }
}

void sessionpresent_failure(const char *backend, const char *detail)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (detail)
        ui_error("%s: %s", backend, detail);
    else
        ui_error("the %s process stopped responding", backend);
    viewport_item_end();
    ui_flush();
}

void sessionpresent_turn_result(struct sessionpresent *p, const char *backend,
                                const char *reply, const char *last_block,
                                const char *detail, const backend_result *meta,
                                int quiet)
{
    int shown = (last_block && strcmp(reply, last_block) == 0) ||
                (p && p->streamed && strcmp(reply, p->streamed) == 0);
    if (quiet) {
        const char *tail = *reply ? reply : (last_block ? last_block : "");
        if (*tail) {
            ui_put(tail);
            ui_put("\n");
        } else {
            char why[128];
            if (meta->subtype[0] && strcmp(meta->subtype, "success") != 0)
                snprintf(why, sizeof why, "the turn ended without a reply (%s)",
                         meta->subtype);
            else
                snprintf(why, sizeof why, "the turn ended without a reply");
            fprintf(stderr, "%s\n",
                    detail && *detail ? detail
                    : meta->interrupted ? "the turn was interrupted"
                    : meta->is_error ? "the turn ended in an error"
                                     : why);
        }
    } else if (!*reply && meta->is_error) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        if (detail && *detail)
            ui_error("%s: %s", backend, detail);
        else
            ui_error("the %s turn ended in an error", backend);
        viewport_item_end();
        ui_flush();
    } else if (*reply && !shown) {
        md_render_kept(reply, 0);
    }

    if (meta->interrupted) {
        viewport_item_begin(VIEWPORT_ROWS(0, 1));
        ui_error("  interrupted");
        viewport_item_end();
        ui_flush();
    }
}

struct footer {
    double elapsed;
    long   tokens;
    long   window;
    double cost;
    char   title[128];
};

static void footer_render(void *ud, int cols)
{
    const struct footer *f = ud;
    char used[32], window[32];
    text_humanize(f->tokens, used, sizeof used);
    text_humanize(f->window, window, sizeof window);

    char line[384];
    size_t n = 0;
    #define APPEND(...)                                                        \
        do {                                                                   \
            int w = snprintf(line + n, sizeof line - n, __VA_ARGS__);          \
            if (w > 0)                                                         \
                n += (size_t)w < sizeof line - n ? (size_t)w                   \
                                                  : sizeof line - n - 1;       \
        } while (0)

    APPEND("%.0fs", f->elapsed);
    if (f->window > 0) {
        int percent = (int)((double)f->tokens * 100.0 / (double)f->window);
        APPEND(" \xc2\xb7 %s / %s (%d%%)", used, window, percent);
    } else if (f->tokens > 0) {
        APPEND(" \xc2\xb7 %s", used);
    }
    if (f->cost > 0)
        APPEND(" \xc2\xb7 $%.4f", f->cost);

    int wrapped = 0;
    if (f->title[0]) {
        int room = cols - 1;
        size_t want = ui_cells(line) + ui_cells(" \xc2\xb7 ") + ui_cells(f->title);
        if (room > 0 && want <= (size_t)room)
            APPEND(" \xc2\xb7 %s", f->title);
        else
            wrapped = 1;
    }
    #undef APPEND

    ui_esc(ui_style(UI_DIM));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    if (wrapped)
        ui_wrapped(f->title, 0, UI_DIM);
}

void sessionpresent_footer(double elapsed, long tokens, long window, double cost,
                           const char *title)
{
    struct footer *f = calloc(1, sizeof *f);
    if (!f)
        return;
    f->elapsed = elapsed;
    f->tokens = tokens;
    f->window = window;
    f->cost = cost;
    snprintf(f->title, sizeof f->title, "%s", title ? title : "");
    viewport_item_begin(&(struct viewport_entry){
        .render = footer_render, .ud = f, .free_ud = free, .reflow = 1});
    footer_render(f, ui_columns());
    viewport_item_end();
    ui_flush();
}

void sessionpresent_report(const struct sessionpresent_report *r)
{
    char used[32], window[32];
    text_humanize(r->context_tokens, used, sizeof used);
    text_humanize(r->context_window, window, sizeof window);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("  backend  %s", r->backend);
    ui_note("  model    %s", r->model);
    if (r->effort)
        ui_note("  effort   %s", r->effort);
    if (r->auth)
        ui_note("  auth     %s", r->auth);
    if (!strcmp(r->backend, "claude")) {
        ui_note("  config   %s", r->customizations ? "skills, CLAUDE.md, MCP, agents"
                                                   : "safe mode (customizations off)");
        ui_note("  tools    %s", r->permission);
    }
    ui_note("  calls    %s", r->compact ? "compact (one row each)" : "full blocks");
    if (r->chat)
        ui_note("  chat     %s", r->chat);
    if (r->id)
        ui_note("  session  %s", r->id);
    if (r->parent)
        ui_note("  parent   %s", r->parent);

    char scratch[512];
    path_home_relative(r->cwd, scratch, sizeof scratch);
    const char *dir = r->cwd ? scratch : ".";
    int room = ui_columns() - 12;
    size_t cells = ui_cells(dir);
    if (room > 8 && cells > (size_t)room) {
        while (*dir && ui_cells(dir) > (size_t)room - 1)
            dir++;
        ui_note("  cwd      …%s", dir);
    } else {
        ui_note("  cwd      %s", dir);
    }
    ui_note("  turns    %d", r->turns);
    if (r->context_window > 0)
        ui_note("  context  %s / %s", used, window);
    if (r->cost > 0 || r->auth)
        ui_note("  cost     $%.4f%s", r->cost,
                r->auth && !strcmp(r->auth, "subscription login")
                    ? "  (list price; the subscription is not billed per token)"
                    : "");
    viewport_item_end();
    ui_flush();
}
