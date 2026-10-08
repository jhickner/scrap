#include "sessionpresent.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "askblock.h"
#include "highlight.h"
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

void sessionpresent_ask_release(struct sessionpresent *p, int show)
{
    if (!p || !p->ask_mark)
        return;
    if (show)
        md_kept_hide(p->ask_mark, 0, 0, 0);
    p->ask_mark = 0;
}

/* A reply ending in an @ask block is drawn with the block already hidden, so
 * it never flashes up before the ask form takes its place; a later reply in
 * the turn is the one asking, so an earlier hidden block comes back. */
static void render_reply(struct sessionpresent *p, const char *text)
{
    size_t from, to;
    sessionpresent_ask_release(p, 1);
    if (p && askblock_span(text, &from, &to))
        p->ask_mark = md_render_kept_hiding(text, 0, from, to);
    else
        md_render_kept(text, 0);
}

void sessionpresent_turn_begin(struct sessionpresent *p)
{
    if (!p)
        return;
    sessionpresent_ask_release(p, 1);
    stream_reset(p);
    p->view.after_collapse = 0;
    p->call_open = 0;
    p->lookups = 0;
    p->calls = 0;
    view_keep_break();
}

void sessionpresent_turn_end(struct sessionpresent *p)
{
    if (p)
        p->call_open = 0;
}

static void paint_note(const char *line)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", line);
    viewport_item_end();
}

static void paint_task(const char *line, size_t cmd_at, size_t cmd_len)
{
    size_t         len = strlen(line);
    unsigned char *spans = malloc(len + 1);
    if (!spans) {
        paint_note(line);
        return;
    }
    memset(spans, (unsigned char)UI_RESET, len);
    size_t tag = strcspn(line, "]");
    memset(spans, (unsigned char)UI_TOOL, tag < len ? tag + 1 : 0);
    if (cmd_len && cmd_at + cmd_len <= len)
        highlight_shell(line + cmd_at, cmd_len, spans + cmd_at);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_put("  ");
    ui_put_spans(line, len, spans, UI_DIM);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    ui_flush();
    viewport_item_end();
    free(spans);
}

static int task_hold(struct sessionpresent *p, const char *line, size_t cmd_at, size_t cmd_len)
{
    if (!p->call_open || p->task_held >= SESSIONPRESENT_TASK_HOLD_MAX)
        return 0;
    if (!p->task_held)
        p->task_held_at = now_seconds();
    snprintf(p->task_hold[p->task_held], sizeof p->task_hold[0], "%s", line);
    p->task_hold_cmd[p->task_held][0] = cmd_at;
    p->task_hold_cmd[p->task_held][1] = cmd_len;
    p->task_held++;
    return 1;
}

static int task_unhold(struct sessionpresent *p)
{
    int held = p->task_held;
    for (int i = 0; i < held; i++)
        paint_task(p->task_hold[i], p->task_hold_cmd[i][0], p->task_hold_cmd[i][1]);
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
        if (ev->backgrounded)
            view_keep_background(ev->parent);
        if (!task_change || (p->call_open && !tasks_done(task_change)))
            break;
        size_t cmd_at, cmd_len;
        tasks_line(task_change, line, sizeof line, &cmd_at, &cmd_len);
        if (task_hold(p, line, cmd_at, cmd_len))
            break;
        if (hide) {
            status_pause();
            paused = 1;
        }
        paint_task(line, cmd_at, cmd_len);
        break;
    }

    case BACKEND_EV_USER:
        if (nested || !ev->text || !*ev->text)
            break;
        if (hide) {
            status_pause();
            paused = 1;
        }
        view_keep_break();
        prompt_echo_message(ev->text);
        p->view.after_collapse = 0;
        break;

    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            if (ev->name && *ev->name) {
                char line[4400];
                text_one_line(ev->text, line, sizeof line);
                view_keep_lookup(line, UI_DIM);
                break;
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
            render_reply(p, ev->text);
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
        if (!nested) {
            p->call_open = 1;
            if (p->calls < 32) {
                if (view_tool_is_lookup(name))
                    p->lookups |= 1u << p->calls;
                p->calls++;
            }
        }
        view_keep_tool_call_bg(name, arg, collapses, toolstyle_background(ev->input_json),
                               ev->id);

        char path[4096];
        if (!collapses && view_tool_path(ev->input_json, cwd, path, sizeof path))
            filediff_snapshot(&p->filediff, path);
        else
            filediff_clear(&p->filediff);
        p->view.after_collapse = collapses;
        break;
    }

    case BACKEND_EV_TOOL_RESULT: {
        int lookup = 0;
        if (!nested && p->calls) {
            lookup = p->lookups & 1;
            p->lookups >>= 1;
            p->calls--;
        }
        if (ev->failed) {
            if (hide) {
                status_pause();
                paused = 1;
            }
            view_keep_break();
            filediff_clear(&p->filediff);
            const char *why = ev->text && *ev->text ? ev->text : NULL;
            char denied[4096];
            if (view_policy_denial(why, denied, sizeof denied)) {
                /* Claude Code's own deny rule, not a prompt that went missing */
                char line[4200];
                snprintf(line, sizeof line, "denied by policy%s%s", *denied ? ": " : "", denied);
                view_keep_output(line, UI_ERROR, 1);
            } else if (!why || !strcmp(why, "failed")) {
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
                if (lookup)
                    view_keep_lookup(ev->text, UI_DIM);
                else
                    view_keep_output(ev->text, UI_DIM, 0);
            }
        }
        break;
    }
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

void sessionpresent_prompt(const char *text)
{
    prompt_echo_message(text);
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
        render_reply(p, reply);
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
    long   fresh, cache_read, cache_write;
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
    if (f->cache_read > 0 || f->cache_write > 0) {
        long in = f->fresh + f->cache_read + f->cache_write;
        char rd[32], wr[32];
        text_humanize(f->cache_read, rd, sizeof rd);
        text_humanize(f->cache_write, wr, sizeof wr);
        APPEND(" \xc2\xb7 cache %d%% (%s read",
               (int)((double)f->cache_read * 100.0 / (double)in), rd);
        if (f->cache_write > 0)
            APPEND(", %s write", wr);
        APPEND(")");
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
                           long fresh, long cache_read, long cache_write,
                           const char *title)
{
    struct footer *f = calloc(1, sizeof *f);
    if (!f)
        return;
    f->elapsed = elapsed;
    f->tokens = tokens;
    f->window = window;
    f->cost = cost;
    f->fresh = fresh;
    f->cache_read = cache_read;
    f->cache_write = cache_write;
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
    if (r->tokens_in > 0 || r->tokens_out > 0) {
        char in[32], out[32], cached[32];
        text_humanize(r->tokens_in, in, sizeof in);
        text_humanize(r->tokens_out, out, sizeof out);
        text_humanize(r->tokens_cached, cached, sizeof cached);
        if (r->tokens_cached > 0)
            ui_note("  tokens   %s in (%s cached) / %s out", in, cached, out);
        else
            ui_note("  tokens   %s in / %s out", in, out);
    }
    if (r->cost > 0 || r->auth)
        ui_note("  cost     $%.4f%s", r->cost,
                r->auth && !strcmp(r->auth, "subscription login")
                    ? "  (list price; the subscription is not billed per token)"
                    : "");
    viewport_item_end();
    ui_flush();
}

static void tokenomics_line(enum ui_role role, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ui_esc(ui_style(role));
    ui_put(line);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void tokenomics_row(enum ui_role role, const char *label, long tokens, long input,
                           double cost, int priced)
{
    char n[32], share[16] = "", price[24] = "";
    text_humanize(tokens, n, sizeof n);
    if (input > 0)
        snprintf(share, sizeof share, "%.1f%%", 100.0 * (double)tokens / (double)input);
    if (priced)
        snprintf(price, sizeof price, "$%.4f", cost);
    tokenomics_line(role, "  %-12s %8s %7s %10s", label, n, share, price);
}

void sessionpresent_tokenomics(const struct sessionpresent_tokens *turns, int n,
                               long context_window)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (n <= 0) {
        ui_note("  no completed turns yet");
        viewport_item_end();
        ui_flush();
        return;
    }

    long fresh = 0, write = 0, read = 0, out = 0, peak = 0;
    double c_fresh = 0, c_write = 0, c_read = 0, c_out = 0, charged = 0;
    int priced = 1, mixed = 0;
    for (int i = 0; i < n; i++) {
        const struct sessionpresent_tokens *t = &turns[i];
        fresh += t->fresh;
        write += t->cache_write;
        read += t->cache_read;
        out += t->output;
        if (t->context > peak)
            peak = t->context;
        charged += t->cost;
        if (t->rate_input <= 0 && t->rate_output <= 0)
            priced = 0;
        if (strcmp(t->backend, turns[0].backend) || strcmp(t->model, turns[0].model))
            mixed = 1;
        c_fresh += (double)t->fresh * t->rate_input / 1e6;
        c_write += (double)t->cache_write * t->rate_cache_write / 1e6;
        c_read += (double)t->cache_read * t->rate_cache_read / 1e6;
        c_out += (double)t->output * t->rate_output / 1e6;
    }
    long input = fresh + write + read;

    char who[80];
    snprintf(who, sizeof who, "%s %s", turns[0].backend, turns[0].model);
    tokenomics_line(UI_HEADING, "  tokenomics \xc2\xb7 %s \xc2\xb7 %d turn%s",
                    mixed ? "several models" : who, n, n == 1 ? "" : "s");
    ui_put("\n");

    tokenomics_line(UI_DIM, "  %-12s %8s %7s %10s", "", "tokens", "share",
                    priced ? "list cost" : "");
    tokenomics_row(UI_TEXT, "fresh input", fresh, input, c_fresh, priced);
    tokenomics_row(UI_TEXT, "cache write", write, input, c_write, priced);
    tokenomics_row(UI_TEXT, "cache read", read, input, c_read, priced);
    tokenomics_row(UI_TEXT, "output", out, 0, c_out, priced);
    tokenomics_row(UI_BOLD, "total", input + out, 0, c_fresh + c_write + c_read + c_out,
                   priced);
    ui_put("\n");

    char pk[32], win[32], ctx[80];
    text_humanize(peak, pk, sizeof pk);
    text_humanize(context_window, win, sizeof win);
    if (context_window > 0)
        snprintf(ctx, sizeof ctx, "%s / %s (%ld%%)", pk, win, 100 * peak / context_window);
    else
        snprintf(ctx, sizeof ctx, "%s", peak > 0 ? pk : "not reported");
    if (input > 0)
        tokenomics_line(UI_TEXT, "  %-12s %.1f%% of input", "cache hit",
                        100.0 * (double)read / (double)input);
    if (write > 0)
        tokenomics_line(UI_TEXT, "  %-12s %.1f reads per written token", "cache reuse",
                        (double)read / (double)write);
    tokenomics_line(UI_TEXT, "  %-12s %s", "peak context", ctx);
    if (charged > 0)
        tokenomics_line(UI_TEXT, "  %-12s $%.4f", "charged", charged);
    ui_put("\n");

    char model_head[24] = "";
    if (mixed)
        snprintf(model_head, sizeof model_head, "%-18s ", "model");
    tokenomics_line(UI_DIM, "  %4s %s%7s %7s %7s %7s %7s %9s  %s", "turn", model_head,
                    "fresh", "write", "read", "out", "context", "cost", "prompt");
    int room = ui_columns() - 60 - (mixed ? 19 : 0);
    for (int i = 0; i < n; i++) {
        const struct sessionpresent_tokens *t = &turns[i];
        char f[16], w[16], r[16], o[16], c[16], cost[16] = "", model[24] = "", prompt[272];
        text_humanize(t->fresh, f, sizeof f);
        text_humanize(t->cache_write, w, sizeof w);
        text_humanize(t->cache_read, r, sizeof r);
        text_humanize(t->output, o, sizeof o);
        text_humanize(t->context, c, sizeof c);
        if (t->cost > 0)
            snprintf(cost, sizeof cost, "$%.4f", t->cost);
        if (mixed)
            snprintf(model, sizeof model, "%-18.18s ", t->model);
        snprintf(prompt, sizeof prompt, "%s", room >= 4 ? t->prompt : "");
        if (room >= 4 && ui_cells(prompt) > (size_t)room) {
            size_t keep = ui_fit_bytes(prompt, (size_t)room - 1);
            while (keep > 0 && prompt[keep - 1] == ' ')
                keep--;
            memcpy(prompt + keep, "\xe2\x80\xa6", 4);
        }
        tokenomics_line(UI_TEXT, "  %4d %s%7s %7s %7s %7s %7s %9s  %s", i + 1, model, f,
                        w, r, o, t->context ? c : "", cost, prompt);
    }
    viewport_item_end();
    ui_flush();
}
