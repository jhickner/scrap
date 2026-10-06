#include "cmd.h"

#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "bridges.h"
#include "chrome.h"
#include "frontend.h"
#include "hud.h"
#include "models.h"
#include "newsession.h"
#include "pick.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "image.h"
#include "tailnet.h"
#include "intercom.h"
#include "sessionfork.h"
#include "sessionlist.h"
#include "gitinfo.h"
#include "grokbottail.h"
#include "block.h"
#include "sessionload.h"
#include "sessionview.h"
#include "viewport.h"
#include "relay.h"
#include "workspace.h"
#include "vendor/agents/grokbot/grokbot.h"
#include "sessionswitch.h"
#include "sidechannel.h"
#include "settings.h"
#include "settingsui.h"
#include "text.h"
#include "status.h"
#include "instance.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "vncinset.h"
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
static const struct pick_item CORE_EFFORTS[] = {
    {"default", "the model's default reasoning effort"},
    {"none", "no reasoning"},
    {"minimal", "minimal reasoning"},
    {"low", "faster, lighter reasoning"},
    {"medium", "balanced reasoning"},
    {"high", "more thorough reasoning"},
    {"xhigh", "extra-high reasoning, when the model supports it"},
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

#define GROKBOT_LIST_MAX 128
#define GROKBOT_LIST_TTL 30

static const struct pick_item *grokbot_choices(int *count)
{
    static struct pick_item items[GROKBOT_LIST_MAX];
    static char             label[GROKBOT_LIST_MAX][96];
    static char             detail[GROKBOT_LIST_MAX][96];
    static int              n;
    static time_t           fetched;

    time_t now = time(NULL);
    if (fetched && now - fetched < GROKBOT_LIST_TTL) {
        *count = n;
        return items;
    }
    fetched = now;
    n = 0;

    grokbot *g = grokbot_open();
    cJSON   *list = g ? grokbot_list_agents(g) : NULL;
    cJSON   *a;
    cJSON_ArrayForEach(a, list) {
        if (n >= GROKBOT_LIST_MAX)
            break;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
        const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(a, "description"));
        if (!name || !*name)
            continue;
        snprintf(label[n], sizeof label[n], "%s", name);
        cJSON *members = cJSON_GetObjectItem(a, "memberIds");
        int    group = cJSON_IsTrue(cJSON_GetObjectItem(a, "isGroup")) ||
                    cJSON_GetArraySize(members) > 0;
        snprintf(detail[n], sizeof detail[n], "%s%s", group ? "group: " : "",
                 desc ? desc : "");
        items[n] = (struct pick_item){label[n], detail[n]};
        n++;
    }
    cJSON_Delete(list);
    grokbot_close(g);
    if (!n)
        fetched = 0;
    *count = n;
    return items;
}

const struct pick_item *cmd_model_choices(const char *backend, int *count)
{
    if (!strcmp(backend, "grokbot"))
        return grokbot_choices(count);
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
    if (!strcmp(backend, "core")) {
        *count = COUNT(CORE_EFFORTS);
        return CORE_EFFORTS;
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

int copy_to_clipboard(const char *text)
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

/* `remote`: the command takes the choice as its argument, so a captured list can be
 * answered by rerunning it. */
static int can_pick(const char *usage, int remote)
{
    if (chrome_modal_active()) {
        reply_note("%s \xe2\x80\x94 a list is already open", usage);
        return 0;
    }
    if (!frontend_has_keyboard() && !(remote && pick_capturing())) {
        reply_note("%s \xe2\x80\x94 nothing here to pick from a list with", usage);
        return 0;
    }
    return 1;
}

static const char *resolve_model(const struct session *s, const char *name)
{
    int count = 0;
    const struct pick_item *choices = model_choices(s, &count);
    if (!count)
        return name;
    const char *backend = session_backend(s);
    size_t      len = strlen(name);
    for (int i = 0; i < count; i++)
        if (!strcmp(choices[i].label, name) ||
            !strcmp(models_short_name(backend, choices[i].label), name))
            return choices[i].label;
    for (int i = 0; i < count; i++)
        if (!strncmp(models_short_name(backend, choices[i].label), name, len))
            return choices[i].label;
    for (int i = 0; i < count; i++)
        if (strstr(choices[i].label, name))
            return choices[i].label;
    return NULL;
}

static void do_model(struct session *s, const char *arg)
{
    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/model <name>", 1))
            return;
        int count = 0, initial = 0;
        const struct pick_item *choices = model_choices(s, &count);
        if (!count && !strcmp(session_backend(s), "grokbot")) {
            reply_note("/model <name> — the Grok Bot gateway did not answer");
            return;
        }
        if (!count) {
            reply_note("/model <name> — %s has no model list here", session_backend(s));
            return;
        }
        const char *current = session_model(s);
        for (int i = 0; i < count; i++)
            if (strcmp(choices[i].label, current) == 0)
                initial = i;
        int index = pick_run_filter("select model", choices, count, initial);
        viewport_flush();
        ui_flush();
        if (index < 0)
            return;
        chosen = choices[index].label;
    } else {
        chosen = resolve_model(s, arg);
        if (!chosen) {
            reply_error("unknown model %s", arg);
            return;
        }
    }

    const char *model = strcmp(chosen, "default") == 0 ? NULL : chosen;
    if (!session_set_model(s, model)) {
        reply_error("could not restart on %s", chosen);
        return;
    }
    note_identity(s);
    if (grokbottail_applies(s))
        grokbottail_show(s, GROKBOTTAIL_DEFAULT, 0);
}

static void do_effort(struct session *s, const char *arg)
{
    if (!session_can_set_effort(s)) {
        reply_note("%s does not support changing effort", session_backend(s));
        return;
    }

    const char *chosen = arg;
    if (!chosen || !*chosen) {
        if (!can_pick("/effort <level>", 1))
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
        if (!can_pick("/backend <claude|codex|grok|pi|grokbot|core>", 1))
            return;
        int count = 0, initial = 0;
        const struct pick_item *choices = cmd_backend_choices(&count);
        for (int i = 0; i < count; i++)
            if (!strcmp(choices[i].label, session_backend(s)))
                initial = i;
        int index = pick_run("backend", choices, count, initial);
        if (index < 0)
            return;
        arg = choices[index].label;
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
        const char *why = session_start_error();
        reply_error("could not start %s%s%s; still using %s", arg, why ? ": " : "",
                    why ? why : "", from);
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
        if (!frontend_has_keyboard() && !pick_capturing()) {
            reply_note("default backend is %s \xe2\x80\x94 /default <name> to change it",
                       cmd_default_backend());
            return;
        }
        if (!can_pick("/default <name>", 1))
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
        if (!can_pick("/permission <mode>", 1))
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

static void do_btw(struct session *s, const char *arg)
{
    if (!arg || !*arg) {
        reply_note("/btw <prompt> — answers on a one-turn fork of this "
                   "conversation, without waiting for the current turn");
        return;
    }

    if (session_remote(s)) {
        if (!session_remote_btw(s, arg))
            reply_error("could not reach %s", session_remote(s));
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

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct pick_item *)a)->label, ((const struct pick_item *)b)->label);
}

static void preview_theme(int index, void *ud)
{
    const struct pick_item *items = ud;
    if (ui_theme_set(items[index].label, 0)) {
        viewport_restyle();
        viewport_paint();
    }
}

static int pick_theme(char *chosen, size_t cap)
{
    char dir[4096];
    DIR *d = path_config_subdir(dir, sizeof dir, "themes") ? opendir(dir) : NULL;
    struct pick_item items[64];
    int count = 0, initial = 0;
    for (struct dirent *e; d && count < COUNT(items) && (e = readdir(d));)
        if (e->d_name[0] != '.')
            items[count++] = (struct pick_item){strdup(e->d_name), NULL};
    if (d)
        closedir(d);
    if (!count) {
        reply_note("no themes in %s", dir);
        return 0;
    }
    qsort(items, count, sizeof *items, by_name);

    char was[256];
    snprintf(was, sizeof was, "%s", ui_theme());
    for (int i = 0; i < count; i++)
        if (!strcmp(items[i].label, was))
            initial = i;
    struct pick_live live = {.select = preview_theme, .ud = items};
    int index = pick_run_live("theme", items, count, initial, &live, PICK_SEARCH_SLASH,
                              NULL, NULL);
    snprintf(chosen, cap, "%s", index >= 0 ? items[index].label : "");
    for (int i = 0; i < count; i++)
        free((char *)items[i].label);
    if (index < 0 && ui_theme_set(was, 0)) {
        viewport_restyle();
        viewport_paint();
    }
    viewport_flush();
    ui_flush();
    return index >= 0;
}

static void do_theme(struct session *s, const char *arg)
{
    (void)s;
    char chosen[256];
    if (!arg || !*arg) {
        if (!can_pick("/theme <name>", 1) || !pick_theme(chosen, sizeof chosen))
            return;
    } else {
        snprintf(chosen, sizeof chosen, "%s", arg);
    }

    if (!ui_theme_set(chosen, 1)) {
        reply_error("no theme %s", chosen);
        return;
    }
    viewport_restyle();
    viewport_paint();
    reply_note("theme: %s", chosen);
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

static void toggle_bridge(const char *name, const char *arg)
{
    char command[32];
    snprintf(command, sizeof command, "/%s", name);
    int on = toggle_arg(arg, "on", "off", bridges_wanted(name), command);
    if (on < 0)
        return;
    char msg[300];
    if (bridges_set(name, on, msg, sizeof msg))
        reply_note("%s", msg);
    else
        reply_error("%s", msg);
}

static void do_relay(struct session *s, const char *arg)
{
    int on = toggle_arg(arg, "on", "off", relay_session() == s, "/relay");
    if (on < 0)
        return;
    if (!on) {
        relay_stop();
        reply_note("relay off");
    } else if (relay_session() == s) {
        reply_note("relay already on in this tab");
    } else {
        if (relay_start(s))
            workspace_republish();
        else
            reply_error("could not start relay");
    }
}

static void do_telegram(struct session *s, const char *arg)
{
    (void)s;
    toggle_bridge("telegram", arg);
}

static void do_api(struct session *s, const char *arg)
{
    (void)s;
    toggle_bridge("api", arg);
}

static void do_voice(struct session *s, const char *arg)
{
    (void)s;
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

    if (arg && *arg && strcmp(arg, "on") && strcmp(arg, "off")) {
        reply_error("/voice takes on, off, volume, rate, complete, or nothing to flip it");
        return;
    }
    int on = arg && *arg ? !strcmp(arg, "on") : !voice_on();
    voice_set_on(on);
    reply_note(on ? "voice on: replies are read aloud" : "voice off");
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
    if (index >= 0 && !sessionswitch_show_open(list[index].id)) {
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

static void do_tail(struct session *s, const char *arg)
{
    if (!grokbottail_applies(s)) {
        reply_note("/tail reads a grokbot transcript; this session runs %s",
                   session_backend(s));
        return;
    }
    int count = GROKBOTTAIL_DEFAULT;
    if (arg && *arg) {
        char *end;
        long n = strtol(arg, &end, 10);
        if (*end || n < 1 || n > GROKBOTTAIL_MAX) {
            reply_error("/tail takes a count from 1 to %d", GROKBOTTAIL_MAX);
            return;
        }
        count = (int)n;
    }
    int drawn = grokbottail_show(s, count, 1);
    if (drawn == 0)
        reply_note("no new messages from %s", session_model(s));
}

static void do_vnc(struct session *s, const char *arg)
{
    if (strcmp(session_backend(s), "grokbot")) {
        reply_note("/vnc shows a grokbot desktop; this session runs %s",
                   session_backend(s));
        return;
    }
    struct vncinset *v = session_inset(s, 1);
    if (!v) {
        reply_error("could not open the desktop inset");
        return;
    }
    vncinset_set_bot(v, session_model(s));

    while (arg && *arg == ' ')
        arg++;
    if (!arg || !*arg) {
        vncinset_show(v, !vncinset_shown(v));
    } else if (!strcmp(arg, "off")) {
        vncinset_show(v, 0);
        vncinset_set_test(v, 0);
    } else if (!strcmp(arg, "test")) {
        vncinset_set_test(v, 1);
        vncinset_show(v, 1);
    } else if (!strcmp(arg, "left") || !strcmp(arg, "right")) {
        vncinset_set_side(v, arg[0] == 'l' ? VNCINSET_LEFT : VNCINSET_RIGHT);
        vncinset_show(v, 1);
    } else if (!strncmp(arg, "size", 4) && (arg[4] == ' ' || !arg[4])) {
        char *end;
        long  pct = strtol(arg + 4, &end, 10);
        while (*end == ' ')
            end++;
        if (end == arg + 4 || *end || pct < VNCINSET_WIDTH_MIN || pct > VNCINSET_WIDTH_MAX) {
            reply_error("/vnc size takes a width from %d to %d percent", VNCINSET_WIDTH_MIN,
                        VNCINSET_WIDTH_MAX);
            return;
        }
        vncinset_set_width(v, (int)pct);
        vncinset_show(v, 1);
    } else {
        reply_error("/vnc takes left, right, off, test, or size <percent>");
        return;
    }

    if (vncinset_shown(v) && !image_available())
        reply_note("this terminal has no graphics support; the desktop inset stays empty");
    viewport_touch();
}

static void clear(struct session *s, int history)
{
    char was[128];
    snprintf(was, sizeof was, "%s", !history && session_id(s) ? session_id(s) : "");
    if (!(history ? session_clear_history(s) : session_clear(s))) {
        reply_error("could not clear the conversation");
        return;
    }

    status_sticky_prompt(NULL);
    if (history) {
        block_cleared();
        hud_print(s);
    }
    sessionload_divider(was);
    ui_flush();
}

static int handoff_path(const struct session *s, char *out, size_t size)
{
    const char *id = session_id(s);
    const char *tmp = getenv("TMPDIR");
    if (!id || !*id)
        return 0;
    if (!tmp || !*tmp)
        tmp = "/tmp";
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/')
        n--;
    return (size_t)snprintf(out, size, "%.*s/" APP_NAME "-handoff-%s.md", (int)n, tmp, id) <
           size;
}

#define HANDOFF_WRITE                                                                     \
    "Write a handoff note for a new session that will continue this work with none of "   \
    "this conversation's context. Cover the goal; the user's last request, quoted "       \
    "verbatim, and whether it is answered; the current state, including uncommitted "     \
    "changes; decisions made; relevant files; dead ends and why they failed; assumptions " \
    "to verify, each with the command that checks it; and the single next step."

static const char AUTOHANDOFF_SEED[] =
    "This message is from scrap, not the user. The previous session reached its context "
    "limit and was cleared; below is the handoff note it wrote. Continue any in-progress "
    "work it describes. If there is none, reply in at most two sentences that you have "
    "the context. Do not restate the note.\n\n";

static void do_handoff(struct session *s, const char *arg)
{
    char path[4200];
    if (session_remote(s)) {
        reply_error("/handoff runs on local sessions only");
        return;
    }
    if (!handoff_path(s, path, sizeof path)) {
        reply_error("nothing to hand off yet");
        return;
    }

    char prompt[8192 + 4200 * 3], label[4200];
    snprintf(prompt, sizeof prompt,
             HANDOFF_WRITE "%s%s Save it to %s and show it with "
             "`@view %s`. Revise it when the user asks. When the user approves it, end the "
             "reply with `@handoff %s` alone on its own line.",
             arg && *arg ? " Focus: " : "", arg && *arg ? arg : "", path, path, path);
    snprintf(label, sizeof label, "/handoff%s%s", arg && *arg ? " " : "", arg ? arg : "");
    workspace_send(workspace_index_of(s), prompt, label);
}

static int handoff_marked(const char *reply, const char *path)
{
    size_t n = reply ? strlen(reply) : 0;
    while (n && (reply[n - 1] == '\n' || reply[n - 1] == ' ' || reply[n - 1] == '\r'))
        n--;
    size_t start = n;
    while (start && reply[start - 1] != '\n')
        start--;
    size_t plen = strlen(path);
    return n - start == 9 + plen && !strncmp(reply + start, "@handoff ", 9) &&
           !strncmp(reply + start + 9, path, plen);
}

static void archive_handoff(const char *path, const char *was)
{
    char dir[4200], dst[4400];
    if (path_config_subdir(dir, sizeof dir, "handoffs/") &&
        (size_t)snprintf(dst, sizeof dst, "%s%s.md", dir, was) < sizeof dst &&
        rename(path, dst) == 0)
        return;
    unlink(path);
}

static int handoff_swap(struct session *s, int automatic)
{
    char path[4200];
    if (session_remote(s) || session_last_result(s)->interrupted ||
        !handoff_path(s, path, sizeof path) || !handoff_marked(session_last_block(s), path))
        return 0;

    char *text = text_slurp(path, 1 << 20, NULL);
    if (!text || !*text) {
        reply_error("could not read the handoff at %s", path);
        free(text);
        return 1;
    }
    char was[128];
    snprintf(was, sizeof was, "%s", session_id(s));
    clear(s, 0);
    if (!strcmp(was, session_id(s) ? session_id(s) : "")) {
        free(text);
        return 1;
    }
    if (automatic) {
        archive_handoff(path, was);
        session_autohandoff_set(s, AUTOHANDOFF_SEEDED);
        char *seed = text_dsprintf("%s%s", AUTOHANDOFF_SEED, text);
        prompt_echo_message("auto-handoff");
        workspace_send(workspace_index_of(s), seed ? seed : text, "auto-handoff");
        free(seed);
    } else {
        unlink(path);
        prompt_echo_message("handoff");
        workspace_send(workspace_index_of(s), text, "handoff");
    }
    free(text);
    return 1;
}

static void autohandoff_ask(struct session *s, int interrupted)
{
    char path[4200];
    if (!handoff_path(s, path, sizeof path)) {
        session_autohandoff_set(s, AUTOHANDOFF_OFF);
        return;
    }
    char prompt[8192 + 4200 * 2];
    snprintf(prompt, sizeof prompt,
             "Context is at %d%%, past the auto-handoff threshold of %d%%%s. " HANDOFF_WRITE
             " Save it to %s, make no other changes, and end the reply with `@handoff %s` "
             "alone on its own line.",
             session_context_percent(s), settings_get_int(SETTING_AUTO_HANDOFF, 0),
             interrupted ? ", so your last turn was interrupted; include the step that was in "
                           "flight"
                         : "",
             path, path);
    session_autohandoff_set(s, AUTOHANDOFF_WRITING);
    workspace_send(workspace_index_of(s), prompt, "auto-handoff");
}

void cmd_turn_done(struct session *s)
{
    int state = session_autohandoff(s);
    if (handoff_swap(s, state == AUTOHANDOFF_WRITING))
        return;
    if (state == AUTOHANDOFF_WRITING) {
        session_autohandoff_set(s, AUTOHANDOFF_OFF);
        reply_error("auto-handoff: no handoff note came back; this session carries on");
        return;
    }
    if (state == AUTOHANDOFF_DUE ||
        (!session_last_result(s)->interrupted && session_autohandoff_ready(s))) {
        autohandoff_ask(s, state == AUTOHANDOFF_DUE);
        return;
    }
    if (state == AUTOHANDOFF_IDLE || state == AUTOHANDOFF_SEEDED)
        session_autohandoff_set(s, AUTOHANDOFF_IDLE);
}

static void do_autohandoff(struct session *s, const char *arg)
{
    (void)s;
    if (arg && *arg) {
        long at = 0;
        if (strcmp(arg, "off")) {
            char *end;
            at = strtol(arg, &end, 10);
            while (*end == '%' || *end == ' ')
                end++;
            if (*end || at < AUTO_HANDOFF_MIN || at > AUTO_HANDOFF_MAX) {
                reply_error("/autohandoff takes off or a context percent between %d and %d",
                            AUTO_HANDOFF_MIN, AUTO_HANDOFF_MAX);
                return;
            }
        }
        settings_set_int(SETTING_AUTO_HANDOFF, (int)at);
    }
    int at = settings_get_int(SETTING_AUTO_HANDOFF, 0);
    if (at > 0)
        reply_note("auto-handoff at %d%% context", at);
    else
        reply_note("auto-handoff off");
}

static void do_clear(struct session *s, const char *arg)
{
    (void)arg;
    clear(s, 0);
}

static void do_clear_history(struct session *s, const char *arg)
{
    (void)arg;
    clear(s, 1);
}

static void do_new(struct session *s, const char *arg)
{
    if ((!arg || !*arg) && frontend_has_keyboard() && !chrome_modal_active()) {
        newsession_run();
        viewport_flush();
        ui_flush();
        return;
    }
    if (!arg || !*arg) {
        do_clear(s, arg);
        return;
    }

    const char *backend = session_backend(s);
    int at = workspace_spawn(backend, session_model_label(s), session_effort(s),
                             session_cwd(s), NULL);
    if (at < 0) {
        reply_error("could not start the %s CLI", backend);
        return;
    }
    hud_print(workspace_current());
    workspace_send(at, arg, NULL);
    ui_flush();
}

static int zoxide_query(const char *arg, char *out, size_t size)
{
    char cmd[4096] = "zoxide query --";
    char word[1024], quoted[2048];
    for (const char *p = arg; *p;) {
        while (*p == ' ')
            p++;
        size_t n = strcspn(p, " ");
        if (!n)
            break;
        if (n >= sizeof word)
            return 0;
        memcpy(word, p, n);
        word[n] = '\0';
        p += n;
        if (!text_shell_quote(word, quoted, sizeof quoted) ||
            strlen(cmd) + strlen(quoted) + 2 >= sizeof cmd)
            return 0;
        strcat(cmd, " ");
        strcat(cmd, quoted);
    }
    strcat(cmd, " 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    int ok = fgets(out, (int)size, f) != NULL;
    pclose(f);
    if (!ok)
        return 0;
    out[strcspn(out, "\n")] = '\0';
    return out[0] != '\0';
}

static void zoxide_add(const char *path)
{
    char quoted[8192], cmd[8300];
    if (!text_shell_quote(path, quoted, sizeof quoted))
        return;
    snprintf(cmd, sizeof cmd, "zoxide add -- %s >/dev/null 2>&1", quoted);
    FILE *f = popen(cmd, "r");
    if (f)
        pclose(f);
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
    struct stat st;
    int ok = realpath(want, resolved) != NULL && stat(resolved, &st) == 0 &&
             S_ISDIR(st.st_mode);
    free(expanded);
    if (!ok) {
        char hit[4096];
        ok = zoxide_query(arg, hit, sizeof hit) && realpath(hit, resolved) != NULL;
    }
    if (!ok) {
        reply_error("no such directory: %s", arg);
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
        reply_error("the agent moved, but scrap could not follow");
    prompt_rehome(resolved);
    zoxide_add(resolved);

    status_sticky_prompt(NULL);
    path_home_relative(resolved, shown, sizeof shown);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_bar(ui_style(UI_DIM), "moved to %s", shown);
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

static void do_name(struct session *s, const char *arg)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (session_remote(s))
        ui_note("this tab is %s; name it on that machine", session_remote(s));
    else if (!arg || !*arg)
        ui_note("this session is @%s", session_name(s));
    else if (session_set_name(s, arg[0] == '@' ? arg + 1 : arg))
        ui_note("this session is now @%s", session_name(s));
    else
        ui_error("a name is letters, digits, - and _, and must not be in use");
    viewport_item_end();
    ui_flush();
}

static void do_send(struct session *s, const char *arg)
{
    char        target[TAILNET_HOST_MAX + INTERCOM_NAME_MAX + 2];
    const char *text = arg ? strchr(arg, ' ') : NULL;
    size_t      n = text ? (size_t)(text - arg) : 0;

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    if (!text || n >= sizeof target) {
        ui_error("usage: /send @name text");
    } else {
        memcpy(target, arg, n);
        target[n] = '\0';
        while (*text == ' ')
            text++;
        char msg[1200];
        if (!*text)
            ui_error("usage: /send @name text");
        else if (intercom_send(session_name(s), target, text, 0, msg, sizeof msg))
            ui_error("%s", msg);
        else
            ui_note("%s", msg);
    }
    viewport_item_end();
    ui_flush();
}

int cmd_attach_tab(const char *target, char *why, size_t size)
{
    char        host[TAILNET_HOST_MAX];
    const char *me = tailnet_self_name(), *local = tailnet_split(target, host, sizeof host);
    if (!local)
        return sessionswitch_yank(target, why, size);
    if (me && !strcasecmp(host, me))
        return sessionswitch_yank(local, why, size);
    for (int i = 0; i < workspace_count(); i++) {
        const char *remote = session_remote(workspace_at(i));
        if (remote && !strcmp(remote, target))
            return i;
    }
    int at = workspace_spawn_remote(target, why, size);
    if (at >= 0)
        workspace_replay(at);
    return at;
}

void cmd_attach(const char *target)
{
    char why[600];
    int  at = cmd_attach_tab(target, why, sizeof why);
    if (at < 0) {
        reply(1, "%s", why);
        return;
    }
    workspace_show(at);
}

static void do_attach(struct session *s, const char *arg)
{
    (void)s;
    if (!arg || !*arg) {
        reply(1, "usage: /attach machine:@name");
        return;
    }
    cmd_attach(arg);
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

    CMD_LIVE_ARG    = 1u << 4,
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
    if (!can_pick("/settings", 0))
        return;
    settingsui_run(s);
}

static void do_resume(struct session *s, const char *arg)
{
    (void)arg;

    if (!can_pick("/resume", 0))
        return;
    cmd_resume(s);
}

static void do_sessions(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    if (!can_pick("/sessions", 0))
        return;
    sessionswitch_run();
}

static void do_status(struct session *s, const char *arg)
{
    (void)arg;

    gitinfo_forget();
    hud_print(s);
}

static void do_session(struct session *s, const char *arg)
{
    (void)arg;
    session_report(s);
}

static void do_tokenomics(struct session *s, const char *arg)
{
    (void)arg;
    session_tokenomics(s);
}

static void do_restart(struct session *s, const char *arg)
{
    (void)s;
    (void)arg;
    restart_request();
}

static void list_instances(void)
{
    char names[2048];
    if (instance_names(names, sizeof names))
        reply_note("saved instances: %s", names);
    else
        reply_note("no saved instances");
}

static void do_save(struct session *s, const char *arg)
{
    (void)s;
    if (!arg || !*arg) {
        list_instances();
        return;
    }
    if (!intercom_name_valid(arg)) {
        reply_error("an instance name is letters, digits, - and _");
        return;
    }
    int n = instance_save(arg);
    if (n > 0)
        reply_note("saved %d tab%s as %s", n, n == 1 ? "" : "s", arg);
    else if (n == 0)
        reply_error("no tab has a conversation to save yet");
    else
        reply_error("could not write instance %s", arg);
}

static void do_load(struct session *s, const char *arg)
{
    (void)s;
    if (!arg || !*arg) {
        list_instances();
        return;
    }
    if (sessionfork_instance(arg))
        reply_note("opening %s in a new window", arg);
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

static void do_fork_t(struct session *s, const char *arg)
{
    (void)arg;
    const char *backend = session_backend(s);
    if (!session_can_resume(s)) {
        reply_error("%s cannot resume a conversation, so there is nothing to fork", backend);
        return;
    }
    const char *id = session_id(s);
    if (!id || !*id) {
        reply_error("nothing to fork yet — send a message first");
        return;
    }
    if (workspace_count() >= WORKSPACE_MAX) {
        reply_error("this window is already holding as many sessions as it can");
        return;
    }

    struct session *f = workspace_prepare(backend, session_model_label(s), session_effort(s),
                                          session_cwd(s), id);
    if (!f) {
        reply_error("could not start the %s CLI", backend);
        return;
    }
    session_set_fork(f, 1);
    if (!session_start(f) || workspace_open(f) < 0) {
        session_free(f);
        reply_error("could not start the %s CLI", backend);
        return;
    }
    hud_print(workspace_current());
    ui_flush();
}

static void do_fork(struct session *s, const char *arg)
{
    static const struct {
        const char *name;
        void      (*run)(struct session *s, const char *arg);
    } kinds[] = {
        {"tab", do_fork_t},
        {"horizontal", do_fork_h},
        {"vertical", do_fork_v},
        {"window", do_fork_w},
    };
    size_t n = strlen(arg ? arg : "");
    int    hit = n ? -1 : 0;
    for (size_t i = 0; n && i < COUNT(kinds); i++)
        if (!strncmp(kinds[i].name, arg, n))
            hit = hit == -1 ? (int)i : -2;
    if (hit < 0) {
        reply_error("usage: /fork [tab|horizontal|vertical|window]");
        return;
    }
    kinds[hit].run(s, NULL);
}

static const struct cmd COMMANDS[] = {
    {"/new", "open a new session, or a new tab running the prompt",
     "[prompt]", CMD_SELF_ECHOES | CMD_LIVE_ARG, do_new},
    {"/clear", "start a fresh conversation, keeping the scrollback", NULL, 0, do_clear},
    {"/clear-history", "start a fresh conversation and clear the scrollback", NULL, 0,
     do_clear_history},
    {"/handoff", "write a handoff for review, then continue from it in a fresh conversation",
     "[focus]", 0, do_handoff},
    {"/autohandoff", "hand off to a fresh conversation when context reaches a percent",
     "[percent|off]", CMD_LIVE, do_autohandoff},
    {"/model", "switch model", "[name]", 0, do_model},
    {"/effort", "set reasoning/thinking effort", "[level]", 0, do_effort},
    {"/backend", "continue with another backend", "<name>", 0, do_backend},
    {"/default", "set the default backend", "[name]", CMD_LIVE, do_default},
    {"/cd", "move the conversation to another directory", "<path or zoxide query>", 0, do_cd},
    {"/btw", "answer this on the side, without waiting", "<prompt>",
     CMD_SELF_ECHOES | CMD_LIVE, do_btw},
    {"/thinking", "show or hide the model's reasoning", "[on|off]", CMD_LIVE,
     do_thinking},
    {"/tools", "how much of each tool call to show", "[compact|full]", CMD_LIVE,
     do_tools},
    {"/theme", "switch the colour theme", "[name]", CMD_LIVE, do_theme},
    {"/sticky", "float the prompt above the spinner", "[on|off]", CMD_LIVE, do_sticky},
    {"/relay", "serve this tab over the phone relay", "[on|off]", CMD_LIVE, do_relay},
    {"/telegram", "answer over Telegram", "[on|off]", CMD_LIVE, do_telegram},
    {"/api", "serve the HTTP API", "[on|off]", CMD_LIVE, do_api},
    {"/voice", "read replies aloud", "[on|off|volume N|rate N|complete on|off]", CMD_LIVE, do_voice},
    {"/image", "tallest an inline image may be drawn", "[rows]", CMD_LIVE, do_image},
    {"/permission", "how the CLI gates tool calls", "[mode]", 0, do_permission},
    {"/settings", "show and change every setting", NULL, 0, do_settings},
    {"/resume", "resume a past conversation", NULL, 0, do_resume},
    {"/sessions", "sessions in every window, and on other tailnet machines", NULL,
     CMD_LIVE, do_sessions},
    {"/fork", "fork into a new tab, tmux split, or tmux window",
     "[tab|horizontal|vertical|window]", CMD_LIVE, do_fork},
    {"/split", "open a shell split in this directory", "[h|v|w]", 0, do_split},
    {"/status", "reprint the status bar", NULL, CMD_LIVE, do_status},
    {"/session", "show this session's info and totals", NULL, CMD_LIVE, do_session},
    {"/tokenomics", "token and cache breakdown for this session, per turn", NULL,
     CMD_LIVE, do_tokenomics},
    {"/title", "set this session's title, or ask the model to title it again", "[title]",
     0, do_rename},
    {"/name", "show or set this session's @name for scrap send", "[name]", 0, do_name},
    {"/send", "send a message to another session", "@name text", CMD_LIVE_ARG, do_send},
    {"/attach", "open a live session from another window or machine in a tab", "machine:@name", 0,
     do_attach},
    {"/tail", "show bot messages that arrived since the last shown", "[count]", 0,
     do_tail},
    {"/vnc", "show the bot's desktop in an inset", "[left|right|off|test|size <percent>]",
     CMD_LIVE, do_vnc},
    {"/copy", "copy last response to clipboard", NULL, CMD_LIVE, do_copy},
    {"/save", "save this window's tabs as a named instance, or list saved ones", "[name]",
     CMD_LIVE, do_save},
    {"/load", "open a saved instance in a new tmux window", "[name]", CMD_LIVE, do_load},
    {"/restart", "reload the scrap binary, keeping this conversation", NULL, 0,
     do_restart},
    {"/help", "show this help", NULL, CMD_LIVE, do_help},
    {"/quit", "quit scrap", NULL, CMD_QUITS, NULL},
    {"/exit", "alias for /quit", NULL, CMD_QUITS, NULL},
};

static const struct cmd *cmd_named(const char *name)
{
    const struct cmd *hit = NULL;
    size_t            n = strlen(name);
    for (size_t i = 0; i < COUNT(COMMANDS); i++) {
        if (!strcmp(COMMANDS[i].name, name))
            return &COMMANDS[i];
        if (n > 1 && !(COMMANDS[i].flags & CMD_HIDDEN) && !strncmp(COMMANDS[i].name, name, n)) {
            if (hit)
                return NULL;
            hit = &COMMANDS[i];
        }
    }
    return hit;
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

static int live(const struct cmd *c, const char *arg)
{
    if (c->flags & CMD_LIVE)
        return 1;
    if (!(c->flags & CMD_LIVE_ARG))
        return 0;
    if (arg && *arg)
        return 1;
    return c->run == do_new && frontend_has_keyboard() && !chrome_modal_active();
}

int cmd_runs_live(const char *line)
{
    const char       *arg;
    const struct cmd *c = cmd_for_line(line, &arg);
    return c && live(c, arg);
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

    if (live(c, arg)) {
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
        char *full = strcmp(session_backend(s), "grokbot")
                         ? voice_with_preamble(line)
                         : NULL;
        workspace_send_typed(tab, line, full);
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
    help_row("?", "key help, on an empty prompt");
    viewport_item_end();
    ui_flush();
}
