#include "cmd.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "boardview.h"
#include "boardwork.h"
#include "chrome.h"
#include "matrix.h"
#include "frontend.h"
#include "hud.h"
#include "relay.h"
#include "models.h"
#include "muxcfg.h"
#include "muxmake.h"
#include "orchstatus.h"
#include "pick.h"
#include "reopen.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "image.h"
#include "sessionfork.h"
#include "sessionlist.h"
#include "gitinfo.h"
#include "sessionload.h"
#include "sessionview.h"
#include "viewport.h"
#include "views.h"
#include "workspace.h"
#include "sidechannel.h"
#include "settings.h"
#include "settingsui.h"
#include "text.h"
#include "tg.h"
#include "status.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "voice.h"

static const struct pick_item CLAUDE_EFFORTS[] = {
    {"default", "auto: use the model's default effort"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning"},
    {"max", "maximum reasoning"},
    {"ultracode", "xhigh effort with dynamic workflow orchestration"},
};
static const struct pick_item CODEX_EFFORTS[] = {
    {"default", "whatever Codex is configured to use"},
    {"none", "no reasoning"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning"},
    {"max", "maximum reasoning"},
};
static const struct pick_item GROK_EFFORTS[] = {
    {"default", "GROK_EFFORT or the CLI default"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning, grok-4.6 only"},
};
static const struct pick_item DEFAULT_EFFORT[] = {
    {"default", "whatever the CLI is configured to use"},
};
static const struct pick_item PI_EFFORTS[] = {
    {"default", "the thinking level active when pi started"},
    {"off", "no thinking"},
    {"minimal", "minimal thinking"},
    {"low", "faster, lighter thinking"},
    {"medium", "balanced thinking"},
    {"high", "more thorough thinking"},
    {"xhigh", "extra-high thinking, when the model supports it"},
    {"max", "maximum thinking, when the model supports it"},
};

static int is_claude(const struct session *s)
{
    return strcmp(session_backend(s), "claude") == 0;
}

static int known_backend(const char *name)
{
    for (const char *const *p = backend_names(); name && *p; p++)
        if (strcmp(name, *p) == 0)
            return 1;
    return 0;
}

const struct pick_item *cmd_model_choices(const char *backend, int *count)
{
    const struct pick_item *v = NULL;
    *count = models_for(backend, &v);
    return v;
}

static const struct pick_item *model_choices(const struct session *s, int *count)
{
    return cmd_model_choices(session_backend(s), count);
}

const struct pick_item *cmd_effort_choices(const char *backend, int *count)
{
    if (!strcmp(backend, "claude")) {
        *count = COUNT(CLAUDE_EFFORTS);
        return CLAUDE_EFFORTS;
    }
    if (!strcmp(backend, "codex")) {
        *count = COUNT(CODEX_EFFORTS);
        return CODEX_EFFORTS;
    }
    if (!strcmp(backend, "grok")) {
        *count = COUNT(GROK_EFFORTS);
        return GROK_EFFORTS;
    }
    if (!strcmp(backend, "pi")) {
        *count = COUNT(PI_EFFORTS);
        return PI_EFFORTS;
    }
    *count = COUNT(DEFAULT_EFFORT);
    return DEFAULT_EFFORT;
}

const struct pick_item *cmd_backend_choices(int *count)
{
    static struct pick_item items[8];
    static int              n;

    if (!n) {
        for (const char *const *p = backend_names(); *p && n < COUNT(items); p++)
            items[n++] = (struct pick_item){*p, NULL};
    }
    *count = n;
    return items;
}

const char *cmd_default_backend(void)
{
    const char *name = settings_get_str(SETTING_BACKEND, "claude");
    return known_backend(name) ? name : "claude";
}

static const struct pick_item *effort_choices(const struct session *s, int *count)
{
    const struct pick_item *v = cmd_effort_choices(session_backend(s), count);
    if (v == DEFAULT_EFFORT)
        *count = 0;
    return *count ? v : NULL;
}

__attribute__((format(printf, 2, 3)))
static void reply(int error, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (error)
        ui_error("%s", text);
    else
        ui_note("%s", text);
    viewport_item_end();
    ui_flush();
}

#define reply_note(...)  reply(0, __VA_ARGS__)
#define reply_error(...) reply(1, __VA_ARGS__)

static void help_row(const char *label, const char *text)
{
    ui_put("  ");
    ui_put(label);
    size_t width = strlen(label);
    for (size_t i = width; i < 16; i++)
        ui_put(" ");
    if (width >= 16)
        ui_put(" ");
    ui_esc(ui_style(UI_DIM));
    ui_put(text);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void help_heading(const char *text)
{
    ui_esc(ui_style(UI_CHROME));
    ui_put(text);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static int copy_to_clipboard(const char *text)
{
#if defined(__APPLE__)
    const char *tool = "pbcopy";
#else
    const char *tool = "xclip -selection clipboard 2>/dev/null || wl-copy";
#endif
    FILE *pipe = popen(tool, "w");
    if (!pipe)
        return 0;
    int ok = fputs(text, pipe) != EOF;
    return pclose(pipe) == 0 && ok;
}

static void note_identity(const struct session *s)
{
    char effort[64] = "", ctx[32] = "";
    if (session_can_set_effort(s))
        snprintf(effort, sizeof effort, " \xc2\xb7 %s effort", session_effort(s));

    long window = session_context_window(s);
    if (window >= 1000000)
        snprintf(ctx, sizeof ctx, " \xc2\xb7 %.3gM context", (double)window / 1e6);
    else if (window > 0)
        snprintf(ctx, sizeof ctx, " \xc2\xb7 %ldK context", window / 1000);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s \xc2\xb7 %s%s%s", session_backend(s),
            models_short_name(session_backend(s), session_model_label(s)), effort, ctx);
    viewport_item_end();
    ui_flush();
}

static int can_pick(const char *usage)
{
    if (chrome_modal_active()) {
        reply_note("%s \xe2\x80\x94 a list is already open", usage);
        return 0;
    }
    if (!frontend_has_keyboard()) {
        reply_note("%s \xe2\x80\x94 nothing here to pick from a list with", usage);
        return 0;
    }
    return 1;
}

/* A typed name must be on the backend's list, with or without the "claude-"
   prefix the list carries. A backend without a list takes any name. */
static int known_model(const struct session *s, const char *name)
{
    int count = 0;
    const struct pick_item *choices = model_choices(s, &count);
    if (!count)
        return 1;
    const char *backend = session_backend(s);
    for (int i = 0; i < count; i++) {
        if (!strcmp(choices[i].label, name))
            return 1;
        if (!strcmp(models_short_name(backend, choices[i].label), name))
            return 1;
    }
    return 0;
}

static void do_model(struct session *s, const char *arg)
{
    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/model <name>"))
            return;
        int count = 0, initial = 0;
        const struct pick_item *choices = model_choices(s, &count);
        if (!count) {
            reply_note("/model <name> — %s has no model list here", session_backend(s));
            return;
        }
        const char *current = session_model(s);
        for (int i = 0; i < count; i++)
            if (strcmp(choices[i].label, current) == 0)
                initial = i;
        int index = pick_run_filter("select model", choices, count, initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    } else if (!known_model(s, chosen)) {
        reply_error("unknown model %s", chosen);
        return;
    }

    const char *model = strcmp(chosen, "default") == 0 ? NULL : chosen;
    if (!session_set_model(s, model)) {
        reply_error("could not restart on %s", chosen);
        return;
    }
    note_identity(s);
}

static void do_effort(struct session *s, const char *arg)
{
    if (!session_can_set_effort(s)) {
        reply_note("%s does not support changing effort", session_backend(s));
        return;
    }

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/effort <level>"))
            return;
        int count = 0, initial = 0;
        const struct pick_item *choices = effort_choices(s, &count);
        const char *current = session_effort(s);
        for (int i = 0; i < count; i++)
            if (!strcmp(choices[i].label, current))
                initial = i;
        int index = pick_run("set effort", choices, count, initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    }

    const char *effort = !strcmp(chosen, "default") ? NULL : chosen;
    if (!session_set_effort(s, effort)) {
        reply_error("could not set effort to %s", chosen);
        return;
    }
    note_identity(s);
}

static void do_backend(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/backend <claude|codex|grok|pi>");
        return;
    }
    if (!known_backend(arg)) {
        reply_error("unknown backend '%s'", arg);
        return;
    }
    if (strcmp(arg, session_backend(s)) == 0) {
        reply_note("already using %s", arg);
        return;
    }

    char *from = strdup(session_backend(s));
    if (!from) {
        reply_error("could not prepare the backend handoff");
        return;
    }
    if (!session_switch_backend(s, arg)) {
        reply_error("could not start %s; still using %s", arg, from);
        free(from);
        return;
    }

    hud_print(s);
    free(from);
}

static void do_default(struct session *s, const char *arg)
{
    (void)s;

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!frontend_has_keyboard()) {
            reply_note("default backend is %s \xe2\x80\x94 /default <name> to change it",
                       cmd_default_backend());
            return;
        }
        if (!can_pick("/default <name>"))
            return;
        int count = 0, initial = 0;
        const struct pick_item *choices = cmd_backend_choices(&count);
        const char *current = cmd_default_backend();
        for (int i = 0; i < count; i++)
            if (!strcmp(choices[i].label, current))
                initial = i;
        int index = pick_run("default backend", choices, count, initial);
        if (index < 0)
            return;
        chosen = choices[index].label;
    }
    if (!known_backend(chosen)) {
        reply_error("unknown backend '%s'", chosen);
        return;
    }
    settings_set_str(SETTING_BACKEND, chosen);
    reply_note("default backend is %s", chosen);
}

static void do_permission(struct session *s, const char *arg)
{
    if (!is_claude(s)) {
        ui_note("/permission only applies to claude");
        return;
    }

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/permission <mode>"))
            return;

        int               count = session_permission_count();
        struct pick_item *choices = calloc((size_t)count, sizeof *choices);
        if (!choices)
            return;
        for (int i = 0; i < count; i++) {
            choices[i].label = session_permission_name(i);
            choices[i].detail = session_permission_desc(i);
        }

        int initial = session_permission_index(session_permission(s));
        int index = pick_run("gate tool calls", choices, count, initial < 0 ? 0 : initial);
        /* the labels are the names of a static table, so they outlive the array */
        chosen = index >= 0 ? choices[index].label : NULL;
        free(choices);
        if (!chosen)
            return;
    }

    int index = session_permission_index(chosen);
    if (index < 0) {
        reply_error("unknown mode '%s'", chosen);
        return;
    }

    if (!session_set_permission(s, session_permission_name(index))) {
        reply_error("could not restart in %s", chosen);
        return;
    }
    settings_set_int(SETTING_PERMISSION, index);
    reply_note("tool calls: %s", session_permission_desc(index));
}

static void do_mux(struct session *s, const char *arg);

static void do_mux(struct session *s, const char *arg)
{
    if (arg && !strcmp(arg, "config")) {
        muxcfg_run();
        return;
    }
    if (arg && !strncmp(arg, "make ", 5)) {
        if (muxmake_run(arg + 5))
            do_mux(s, NULL);
        return;
    }
    if (!arg || !*arg) {
        if (matrix_reopen(s))
            return;
        struct mux_spec v[MUX_MAX];
        int             n = muxcfg_load(v, MUX_MAX);
        reply_note("/mux <prompt> — asks the whole matrix at once; "
                   "/mux config to change it, /mux make <what> to have one "
                   "laid out for you");
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_note("%s", muxcfg_active());
        ui_put("\n");
        for (int i = 0; i < n; i++) {
            char label[160];
            muxcfg_label(&v[i], label, sizeof label);
            if (*v[i].prompt)
                ui_note("  %s — %s", label, v[i].prompt);
            else
                ui_note("  %s", label);
            ui_put("\n");
        }
        viewport_item_end();
        ui_flush();
        return;
    }
    matrix_run(s, arg);
}

static void do_btw(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/btw <prompt> — answers on a one-turn fork of this "
                   "conversation, without waiting for the current turn");
        return;
    }

    char label[4096];
    snprintf(label, sizeof label, "/btw %s", arg);
    sidechannel_start(s, arg, label);
    ui_flush();
}

static int toggle_arg(const char *arg, const char *on_word, const char *off_word,
                      int current, const char *command)
{
    if (!arg || !*arg)
        return !current;
    if (!strcmp(arg, on_word))
        return 1;
    if (!strcmp(arg, off_word))
        return 0;
    reply_error("%s takes %s, %s, or nothing to flip it", command, on_word, off_word);
    return -1;
}

static void do_thinking(struct session *s, const char *arg)
{
    int on = toggle_arg(arg, "on", "off", session_thinking(s), "/thinking");
    if (on < 0)
        return;

    session_set_thinking(s, on);
    settings_set_int(SETTING_THINKING, on);
    reply_note("reasoning %s", on ? "shown" : "hidden");
}

static void do_tools(struct session *s, const char *arg)
{
    int compact = toggle_arg(arg, "compact", "full", session_compact(s), "/tools");
    if (compact < 0)
        return;

    session_set_compact(s, compact);
    settings_set_int(SETTING_COMPACT, compact);
    view_collapse(compact);
    reply_note("tool calls: %s", compact ? "one row each" : "full blocks with output");
}

static void do_sticky(struct session *s, const char *arg)
{
    (void)s;
    int on = toggle_arg(arg, "on", "off", status_sticky_enabled(), "/sticky");
    if (on < 0)
        return;

    status_sticky_set(on);
    settings_set_int(SETTING_STICKY, on);
    reply_note("floating prompt %s", on ? "on" : "off");
}

static void do_relay(struct session *s, const char *arg)
{
    int current = relay_label() != NULL;
    int on = toggle_arg(arg, "on", "off", current, "/relay");
    if (on < 0)
        return;
    if (on == current) {
        reply_note("relay already %s", on ? "on" : "off");
        return;
    }
    if (on) {
        if (!relay_start(s))
            reply_error("%s", relay_start_error() ? relay_start_error()
                                                   : "could not enable relay");
        return;
    }
    relay_stop();
    reply_note("relay off");
}

static void do_telegram(struct session *s, const char *arg)
{
    int current = tg_label() != NULL;
    int on = toggle_arg(arg, "on", "off", current, "/telegram");
    if (on < 0)
        return;
    if (on == current) {
        reply_note("telegram already %s", on ? "on" : "off");
        return;
    }
    if (on) {
        if (!tg_start(s))
            reply_error("%s", tg_start_error() ? tg_start_error()
                                                : "could not enable telegram");
        return;
    }
    tg_stop();
    reply_note("telegram off");
}

static void do_voice(struct session *s, const char *arg)
{
    (void)s;
    int want, speak;

    if (arg && !strncmp(arg, "complete", 8) && (!arg[8] || arg[8] == ' ')) {
        const char *rest = arg + 8;
        while (*rest == ' ')
            rest++;
        int on = toggle_arg(*rest ? rest : NULL, "on", "off",
                            settings_get_int(SETTING_VOICE_COMPLETE, 1),
                            "/voice complete");
        if (on < 0)
            return;
        settings_set_int(SETTING_VOICE_COMPLETE, on);
        reply_note("voice complete %s", on ? "on" : "off");
        return;
    }

    if (arg && !strncmp(arg, "volume", 6) && (!arg[6] || arg[6] == ' ')) {
        const char *rest = arg + 6;
        while (*rest == ' ')
            rest++;
        if (!*rest) {
            reply_note("voice volume %d", voice_volume());
            return;
        }
        char *end;
        long n = strtol(rest, &end, 10);
        if (*end == '%')
            end++;
        while (*end == ' ')
            end++;
        if (*end || n < 0 || n > 100) {
            reply_error("/voice volume takes a number from 0 to 100");
            return;
        }
        voice_set_volume((int)n);
        reply_note("voice volume %d", (int)n);
        return;
    }

    if (arg && !strncmp(arg, "rate", 4) && (!arg[4] || arg[4] == ' ')) {
        const char *rest = arg + 4;
        while (*rest == ' ')
            rest++;
        if (!*rest) {
            reply_note("voice rate %d", voice_rate());
            return;
        }
        char *end;
        long n = strtol(rest, &end, 10);
        if (*end == '%')
            end++;
        while (*end == ' ')
            end++;
        if (*end || n < VOICE_RATE_MIN || n > VOICE_RATE_MAX) {
            reply_error("/voice rate takes a number from %d to %d",
                        VOICE_RATE_MIN, VOICE_RATE_MAX);
            return;
        }
        voice_set_rate((int)n);
        reply_note("voice rate %d", (int)n);
        return;
    }

    if (arg && !strncmp(arg, "silence", 7) && (!arg[7] || arg[7] == ' ')) {
        const char *rest = arg + 7;
        while (*rest == ' ')
            rest++;
        if (!*rest) {
            reply_note("voice silence %gs", voice_silence());
            return;
        }
        char  *end;
        double n = strtod(rest, &end);
        if (*end == 's')
            end++;
        while (*end == ' ')
            end++;
        if (*end || n < VOICE_SILENCE_MIN || n > VOICE_SILENCE_MAX) {
            reply_error("/voice silence takes seconds from %g to %g",
                        VOICE_SILENCE_MIN, VOICE_SILENCE_MAX);
            return;
        }
        voice_set_silence(n);
        reply_note("voice silence %gs", voice_silence());
        return;
    }

    if (arg && !strcmp(arg, "restart")) {
        char err[300];
        int was_on = voice_on();
        if (!voice_restart(err, sizeof err)) {
            reply_error("voice: %s", err);
            return;
        }
        reply_note(was_on ? "voice restarted" : "voice helper stopped");
        return;
    }

    if (!arg || !*arg) {
        want = !voice_on();
        speak = voice_speak();
    } else if (!strcmp(arg, "off")) {
        want = 0;
        speak = voice_speak();
    } else if (!strcmp(arg, "on")) {
        want = 1;
        speak = 1;
    } else if (!strcmp(arg, "listen")) {
        want = 1;
        speak = 0;
    } else {
        reply_error("/voice takes on, off, listen, restart, complete, volume, rate, silence, or nothing to flip it");
        return;
    }

    char err[300];
    if (!voice_apply(want, speak, err, sizeof err)) {
        reply_error("voice: %s", err);
        return;
    }
    if (!want)
        reply_note("voice off");
    else
        reply_note(speak ? "voice on: listening" : "voice on: listen only");
}

static void do_image(struct session *s, const char *arg)
{
    (void)s;
    if (arg && *arg) {
        char *end;
        long rows = strtol(arg, &end, 10);
        while (*end == ' ')
            end++;
        if (*end || rows < IMAGE_ROWS_MIN || rows > IMAGE_ROWS_MAX) {
            reply_error("/image takes a row count between %d and %d",
                     IMAGE_ROWS_MIN, IMAGE_ROWS_MAX);
            return;
        }
        image_set_rows((int)rows);
        settings_set_int(SETTING_IMAGE_ROWS, image_rows());
    }

    if (image_available()) {
        reply_note("inline images: up to %d rows tall", image_rows());
    } else {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_note("inline images: up to %d rows tall, but this terminal has no "
                "graphics support", image_rows());
        viewport_item_end();
        ui_flush();
    }
}

int cmd_resume(struct session *s)
{
    if (!sessionlist_available(session_backend(s))) {
        ui_note("%s keeps no transcripts to resume from", session_backend(s));
        return 0;
    }

    struct past_session *list = NULL;
    int count = sessionlist_load(session_backend(s), session_cwd(s), session_id(s),
                                 &list);
    if (count == 0) {
        reply_note("no past conversations for this directory");
        return 0;
    }

    struct pick_item *items = calloc((size_t)count, sizeof *items);
    if (!items) {
        free(list);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        items[i].label = list[i].when;
        items[i].detail = list[i].label;
    }

    int resumed = 0;
    int index = pick_run("resume which conversation", items, count, 0);
    if (index >= 0) {
        if (session_resume(s, list[index].id)) {
            status_sticky_prompt(NULL);

            viewport_clear();
            hud_print(s);
            sessionload_into(s);
            viewport_item_begin(VIEWPORT_ROWS(1, 1));
            ui_bar(ui_style(UI_DIM), "resumed \xc2\xb7 %s", list[index].label);
            viewport_item_end();
            ui_flush();
            resumed = 1;
        } else {
            reply_error("could not resume that conversation");
        }
    }
    free(items);
    free(list);
    ui_flush();
    return resumed;
}

static void do_new(struct session *s, const char *arg)
{
    (void)arg;
    if (!session_clear(s)) {
        reply_error("could not clear the conversation");
        return;
    }

    status_sticky_prompt(NULL);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_bar(ui_style(UI_DIM), "new conversation");
    viewport_item_end();
    ui_flush();
}

static void do_cd(struct session *s, const char *arg)
{
    char shown[4096];
    if (!arg || !*arg) {
        path_home_relative(session_cwd(s), shown, sizeof shown);
        reply_note("%s", shown);
        return;
    }

    char *expanded = path_expand_home(arg);
    char  resolved[4096];
    const char *want = expanded ? expanded : arg;
    int ok = realpath(want, resolved) != NULL;
    free(expanded);
    if (!ok) {
        reply_error("no such directory: %s", arg);
        return;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        reply_error("not a directory: %s", arg);
        return;
    }
    if (!strcmp(resolved, session_cwd(s))) {
        path_home_relative(resolved, shown, sizeof shown);
        reply_note("already in %s", shown);
        return;
    }

    if (!session_set_cwd(s, resolved)) {
        reply_error("could not start %s in %s", session_backend(s), resolved);
        return;
    }
    if (chdir(resolved) != 0)
        reply_error("the agent moved, but mux could not follow");
    prompt_rehome(resolved);

    status_sticky_prompt(NULL);
    path_home_relative(resolved, shown, sizeof shown);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_bar(ui_style(UI_DIM), "new conversation in %s", shown);
    viewport_item_end();
    ui_flush();
}

static void do_copy(struct session *s, const char *arg)
{
    (void)arg;
    const char *reply = session_last_reply(s);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (!reply) {
        ui_note("nothing to copy yet");
    } else if (copy_to_clipboard(reply)) {
        ui_note("copied %zu characters", strlen(reply));
    } else {
        ui_error("could not reach the clipboard");
    }
    viewport_item_end();
    ui_flush();
}

static void do_split(struct session *s, const char *arg)
{
    enum fork_where where = FORK_SPLIT_H;
    if (arg && (*arg == 'v' || *arg == 'V'))
        where = FORK_SPLIT_V;
    else if (arg && (*arg == 'w' || *arg == 'W'))
        where = FORK_WINDOW;
    sessionfork_shell(s, where, 0);
}

static void do_rename(struct session *s, const char *arg)
{
    int named = arg && *arg;
    enum session_rename why = session_rename(s, named ? arg : NULL);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (why != SESSION_RENAME_OK)
        ui_error("%s", session_rename_error(why));
    else if (named)
        ui_note("renamed to %s", session_title(s));
    else
        ui_note("naming this session again");
    viewport_item_end();
    ui_flush();
}

static int split_command(const char *line, char *name, size_t size, const char **arg)
{
    if (*line != '/')
        return 0;

    const char *space = strchr(line, ' ');
    size_t name_len = space ? (size_t)(space - line) : strlen(line);
    if (name_len >= size)
        return 0;

    const char *rest = space ? space + 1 : "";
    while (*rest == ' ')
        rest++;
    *arg = rest;

    memcpy(name, line, name_len);
    name[name_len] = '\0';
    return 1;
}

enum {
    CMD_HIDDEN      = 1u << 0,
    CMD_QUITS       = 1u << 1,

    CMD_SELF_ECHOES = 1u << 2,

    CMD_LIVE        = 1u << 3,
};

struct cmd {
    const char *name;
    const char *desc;
    const char *args;
    unsigned    flags;
    void      (*run)(struct session *s, const char *arg);
};

static void do_help(struct session *s, const char *arg);

static void do_settings(struct session *s, const char *arg)
{
    (void)arg;
    if (!can_pick("/settings"))
        return;
    settingsui_run(s);
}

static void do_resume(struct session *s, const char *arg)
{
    (void)arg;

    if (!can_pick("/resume"))
        return;
    cmd_resume(s);
}

static void do_reopen(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    if (!can_pick("/reopen"))
        return;
    if (!reopen_available()) {
        reply_note("no window to reopen");
        return;
    }

    int opened = reopen_run();
    if (opened)
        reply_note("reopening %d session%s", opened, opened == 1 ? "" : "s");
}

static void do_sessions(struct session *s, const char *arg)
{
    (void)arg;
    if (!can_pick("/sessions"))
        return;
    views_sessions(session_cwd(s));
}

static void do_card(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_error("/card <text> \xe2\x80\x94 nothing to put on the board");
        return;
    }
    char id[16] = {0};
    if (boardview_capture(arg, session_cwd(s), id, sizeof id))
        reply_note("card %s", id);
    else
        reply_error("could not write to the board");
}

static void do_close(struct session *s, const char *arg)
{
    (void)arg;

    const char *id = boardwork_card_of(s);
    if (!id) {
        reply_error("/close \xe2\x80\x94 this session is not working a card");
        return;
    }

    char why[256] = "";
    if (!boardview_close(id, why, sizeof why))
        reply_error("%s", why);
    else if (why[0])
        reply_note("card %s closed \xc2\xb7 %s", id, why);
    else
        reply_note("card %s closed", id);
}

static void do_run(struct session *s, const char *arg)
{
    const char *held = boardwork_card_of(s);
    if (!held) {
        reply_error("/run \xe2\x80\x94 this session is not working a card");
        return;
    }

    char id[BOARD_ID_MAX];
    snprintf(id, sizeof id, "%s", held);

    char why[512] = "";
    if (!boardview_trigger(id, arg, why, sizeof why)) {
        reply_error("%s", why);
        return;
    }

    reply_note("card %s runs %s next", id, arg);
    boardwork_step(id);
}

static void do_board(struct session *s, const char *arg)
{
    (void)arg;
    if (!can_pick("/board"))
        return;
    views_board(session_cwd(s));
}

static void do_status(struct session *s, const char *arg)
{
    (void)arg;
    /* the repo may have moved from outside this session since the last turn */
    gitinfo_forget();
    hud_print(s);
}

static void do_tasks(struct session *s, const char *arg)
{
    (void)s;
    int all = arg && (!strcmp(arg, "all") || !strcmp(arg, "--all"));
    if (arg && *arg && !all) {
        reply_error("/tasks [all] \xe2\x80\x94 expected all or no argument");
        return;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) {
        reply_error("could not find the orchestrator task directory");
        return;
    }
    char projects[4200], live[4200];
    if (snprintf(projects, sizeof projects, "%s/.config/orchestrator/projects", home)
            >= (int)sizeof projects) {
        reply_error("orchestrator task directory path is too long");
        return;
    }
    const char *live_env = getenv("MUX_LIVE_DIR");
    if (live_env && *live_env)
        snprintf(live, sizeof live, "%s", live_env);
    else if (snprintf(live, sizeof live, "%s/.config/mux/live", home)
                 >= (int)sizeof live) {
        live[0] = '\0';
    }

    struct orch_task *tasks = NULL;
    int count = orchstatus_load(projects, live, all, &tasks);
    if (count < 0) {
        reply_error("could not read orchestrator tasks");
        return;
    }
    if (!count) {
        free(tasks);
        reply_note("no %sorchestrator tasks", all ? "" : "open ");
        return;
    }

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    char title[80];
    snprintf(title, sizeof title, "%d %sorchestrator task%s", count,
             all ? "" : "open ", count == 1 ? "" : "s");
    help_heading(title);

    struct orchstatus_columns widths;
    orchstatus_columns(&widths, ui_columns(), tasks, count);
    char project_head[16], task_head[16], status_head[16], agent_head[32], age_head[16];
    orchstatus_cell(project_head, sizeof project_head, "PROJECT", (size_t)widths.project);
    orchstatus_cell(task_head, sizeof task_head, "TASK", (size_t)widths.task);
    orchstatus_cell(status_head, sizeof status_head, "STATUS", (size_t)widths.status);
    orchstatus_cell(agent_head, sizeof agent_head, "BACKEND / MODEL", (size_t)widths.agent);
    orchstatus_cell(age_head, sizeof age_head, "AGE", (size_t)widths.age);
    ui_esc(ui_style(UI_DIM));
    ui_printf("  %-*s %-*s %-*s %-*s %-*s\n",
              widths.project, project_head, widths.task, task_head,
              widths.status, status_head, widths.agent, agent_head,
              widths.age, age_head);
    ui_esc(ui_style(UI_RESET));

    time_t now = time(NULL);
    for (int i = 0; i < count; i++) {
        struct orch_task *t = &tasks[i];
        char project[128], desc[512], status[80], agent[220], age[16], age_cell[16];
        char full_status[80], full_agent[220];
        orchstatus_status(full_status, sizeof full_status, t);
        orchstatus_agent(full_agent, sizeof full_agent, t);
        orchstatus_cell(project, sizeof project, t->project, (size_t)widths.project);
        const char *rest = orchstatus_wrap(desc, sizeof desc,
                                           t->desc[0] ? t->desc : t->id,
                                           (size_t)widths.task);
        orchstatus_cell(status, sizeof status, full_status, (size_t)widths.status);
        orchstatus_cell(agent, sizeof agent, full_agent, (size_t)widths.agent);
        orchstatus_age(age, sizeof age, t->updated ? t->updated : t->created, now);
        orchstatus_cell(age_cell, sizeof age_cell, age, (size_t)widths.age);
        ui_printf("  %-*s %-*s %-*s %-*s %-*s\n",
                  widths.project, project, widths.task, desc,
                  widths.status, status, widths.agent, agent,
                  widths.age, age_cell);
        while (*rest) {
            rest = orchstatus_wrap(desc, sizeof desc, rest, (size_t)widths.task);
            ui_printf("  %*s %s\n", widths.project, "", desc);
        }
    }
    free(tasks);
    viewport_item_end();
    ui_flush();
}

static void do_session(struct session *s, const char *arg)
{
    (void)arg;
    session_report(s);
}

static void do_restart(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    restart_request();
}

static void do_fork_h(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_SPLIT_H);
}

static void do_fork_v(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_SPLIT_V);
}

static void do_fork_w(struct session *s, const char *arg)
{
    (void)arg;
    sessionfork_run(s, FORK_WINDOW);
}

static const struct cmd COMMANDS[] = {
    {"/new", "start a fresh conversation", NULL, 0, do_new},
    {"/clear", "alias for /new", NULL, 0, do_new},
    {"/model", "switch model", "[name]", 0, do_model},
    {"/effort", "set reasoning/thinking effort", "[level]", 0, do_effort},
    {"/backend", "continue with another backend", "<name>", 0, do_backend},
    {"/default", "set the default backend", "[name]", CMD_LIVE, do_default},
    {"/cd", "work in another directory, starting fresh there", "<path>", 0, do_cd},
    {"/mux", "ask the whole matrix the same thing", "<prompt>|config|make <what>",
     0, do_mux},
    {"/btw", "answer this on the side, without waiting", "<prompt>",
     CMD_SELF_ECHOES | CMD_LIVE, do_btw},
    {"/thinking", "show or hide the model's reasoning", "[on|off]", CMD_LIVE,
     do_thinking},
    {"/tools", "how much of each tool call to show", "[compact|full]", CMD_LIVE,
     do_tools},
    {"/sticky", "float the prompt above the spinner", "[on|off]", CMD_LIVE, do_sticky},
    {"/relay", "answer over the phone relay", "[on|off]", CMD_LIVE, do_relay},
    {"/telegram", "answer over Telegram", "[on|off]", CMD_LIVE, do_telegram},
    {"/voice", "talk instead of typing", "[on|off|listen|restart|complete|volume|rate|silence]", CMD_LIVE, do_voice},
    {"/image", "tallest an inline image may be drawn", "[rows]", CMD_LIVE, do_image},
    {"/permission", "how the CLI gates tool calls", "[mode]", 0, do_permission},
    {"/settings", "show and change every setting", NULL, 0, do_settings},
    {"/resume", "resume a past conversation", NULL, 0, do_resume},
    {"/sessions", "every session: this window's, other windows', past ones", NULL,
     CMD_LIVE, do_sessions},
    {"/reopen", "bring back the sessions of a window that is gone", NULL, 0,
     do_reopen},
    {"/fh", "fork into a horizontal tmux split", NULL, CMD_LIVE, do_fork_h},
    {"/fs", "alias for /fh", NULL, CMD_LIVE, do_fork_h},
    {"/fv", "fork into a vertical tmux split", NULL, CMD_LIVE, do_fork_v},
    {"/fw", "fork into a tmux window", NULL, CMD_LIVE, do_fork_w},
    {"/split", "open a shell split in this directory", "[h|v|w]", 0, do_split},
    {"/card", "add a card to the board", "<text>", CMD_LIVE, do_card},
    {"/board", "show the cards, by column", NULL, CMD_LIVE, do_board},
    {"/close", "close this session's card", NULL, CMD_LIVE, do_close},
    {"/run", "queue actions on this session's card", "[<action>, ...]", CMD_LIVE,
     do_run},
    {"/tasks", "show orchestrator tasks across every project", "[all]", CMD_LIVE,
     do_tasks},
    {"/status", "reprint the status bar", NULL, CMD_LIVE, do_status},
    {"/session", "show this session's info and totals", NULL, CMD_LIVE, do_session},
    {"/rename", "name this session, or ask the model to name it again", "[name]",
     0, do_rename},
    {"/copy", "copy last response to clipboard", NULL, CMD_LIVE, do_copy},
    {"/restart", "reload the mux binary, keeping this conversation", NULL, 0,
     do_restart},
    {"/help", "show this help", NULL, CMD_LIVE, do_help},
    {"/quit", "quit mux", NULL, CMD_QUITS, NULL},
    {"/exit", "alias for /quit", NULL, CMD_QUITS, NULL},
};

static const struct cmd *cmd_named(const char *name)
{
    for (size_t i = 0; i < COUNT(COMMANDS); i++)
        if (!strcmp(COMMANDS[i].name, name))
            return &COMMANDS[i];
    return NULL;
}

static const struct cmd *cmd_for_line(const char *line, const char **arg)
{
    char name[32];
    if (!split_command(line, name, sizeof name, arg))
        return NULL;
    return cmd_named(name);
}

const ReplCommand *cmd_completions(int *count)
{
    static ReplCommand table[COUNT(COMMANDS)];
    static int         n;

    if (!n) {
        for (size_t i = 0; i < COUNT(COMMANDS); i++) {
            if (COMMANDS[i].flags & CMD_HIDDEN)
                continue;
            table[n].name = COMMANDS[i].name;
            table[n].desc = COMMANDS[i].desc;
            table[n].args = COMMANDS[i].args;
            n++;
        }
    }
    *count = n;
    return table;
}

int cmd_is_command(const char *line)
{
    const char *arg;
    return cmd_for_line(line, &arg) != NULL;
}

int cmd_self_echoes(const char *line)
{
    const char *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && (c->flags & CMD_SELF_ECHOES) && arg && *arg;
}

int cmd_runs_mid_turn(const char *line)
{
    const char *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && !(c->flags & CMD_QUITS);
}

int cmd_runs_live(const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && (c->flags & CMD_LIVE);
}

#define DEFERRED_MAX 8
static struct {
    char           *line;
    struct session *s;
} deferred[DEFERRED_MAX];
static int   deferred_count;

void cmd_dispatch_live(struct session *s, const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    if (!c || (c->flags & CMD_QUITS))
        return;

    if (c->flags & CMD_LIVE) {
        c->run(s, arg);
        return;
    }

    if (deferred_count == DEFERRED_MAX) {
        reply_error("too many settings changes are already waiting");
        return;
    }
    char *copy = strdup(line);
    if (!copy) {
        reply_error("could not hold %s until the turn ends", c->name);
        return;
    }
    deferred[deferred_count].line = copy;
    deferred[deferred_count].s = s;
    deferred_count++;
    reply_note("%s applies when this turn ends", c->name);
}

void cmd_run_deferred(struct session *s)
{
    char *mine[DEFERRED_MAX];
    int   n = 0, kept = 0;

    for (int i = 0; i < deferred_count; i++) {
        if (deferred[i].s == s)
            mine[n++] = deferred[i].line;
        else
            deferred[kept++] = deferred[i];
    }
    deferred_count = kept;

    for (int i = 0; i < n; i++) {
        cmd_dispatch(s, mine[i]);
        free(mine[i]);
    }
}

void cmd_forget_session(struct session *s)
{
    int kept = 0;
    for (int i = 0; i < deferred_count; i++) {
        if (deferred[i].s == s)
            free(deferred[i].line);
        else
            deferred[kept++] = deferred[i];
    }
    deferred_count = kept;
}

enum cmd_result cmd_dispatch(struct session *s, const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    if (!c)
        return CMD_NOT_A_COMMAND;
    if (c->flags & CMD_QUITS)
        return CMD_QUIT;
    c->run(s, arg);
    return CMD_HANDLED;
}

enum cmd_result cmd_submit(struct session *s, const char *line)
{
    if (!s || !line)
        return CMD_NOT_A_COMMAND;

    if (session_turn_running(s) && cmd_runs_mid_turn(line)) {
        cmd_dispatch_live(s, line);
        return CMD_HANDLED;
    }

    enum cmd_result r = cmd_dispatch(s, line);
    if (r != CMD_NOT_A_COMMAND)
        return r;

    int tab = workspace_index_of(s);
    if (tab >= 0) {
        char *full = voice_with_preamble(line);
        workspace_send(tab, full ? full : line, full ? line : NULL);
        free(full);
    }
    return CMD_NOT_A_COMMAND;
}

static void do_help(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    help_heading("commands");
    for (size_t i = 0; i < COUNT(COMMANDS); i++) {
        if (COMMANDS[i].flags & CMD_HIDDEN)
            continue;
        char label[64];
        if (COMMANDS[i].args)
            snprintf(label, sizeof label, "%s %s", COMMANDS[i].name, COMMANDS[i].args);
        else
            snprintf(label, sizeof label, "%s", COMMANDS[i].name);
        help_row(label, COMMANDS[i].desc);
    }
    ui_put("\n");
    help_heading("shortcuts");

    help_row("!cmd", "run cmd in $SHELL instead of sending it to the agent");
    int                      key_count = 0;
    const struct prompt_key *keys = prompt_shortcuts(&key_count);
    for (int i = 0; i < key_count; i++)
        help_row(keys[i].key, keys[i].desc);
    ui_put("\n");
    help_heading("skills");
    help_row("", "your skills, CLAUDE.md, MCP servers and agents load by default.");
    help_row("", "any slash command not listed above goes to the agent CLI, so");
    help_row("", "/w, /todo and the rest work here. start with -s to run without");
    help_row("", "them; /session shows what is active.");
    viewport_item_end();
    ui_flush();
}
