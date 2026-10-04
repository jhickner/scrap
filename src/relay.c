#include "relay.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WSD_IMPLEMENTATION
#include "vendor/wsd.h"
#include "vendor/cJSON.h"
#include "vendor/httpd.h"

#include "app.h"
#include "bash.h"
#include "chatnav.h"
#include "chrome.h"
#include "cmd.h"
#include "frontend.h"
#include "livelist.h"
#include "prompt.h"
#include "session.h"
#include "sessionview.h"
#include "settings.h"
#include "text.h"
#include "tg.h"
#include "transcript.h"
#include "toolstyle.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define INBOX_MAX 32
#define MENU_MAX  16
#define PORT_DEFAULT 8790
#define HISTORY_TURNS 6
#define HISTORY_BYTES 6000

enum { ITEM_LINE, ITEM_PICK, ITEM_HELLO, ITEM_STOP };

struct inbox_item {
    char *text;
    int   kind;

    char  tab_id[80];
    int   tab;
};

static struct {
    struct chatnav nav;
    wsd            *ws;
    httpd          *files;
    char            files_dir[4200];
    char            files_base[160];
    int             active;
    int             mirror;
    int             wake[2];
    char            label[64];
    char            token[128];
    int             port;
    char            bind[64];
    int             draining;
    char            system_note[900];
    int             repeat_task;
    int             busy_sent;
    double          mirrored_turn;
    char           *sent;
    struct inbox_item inbox[INBOX_MAX];
    int             inbox_head;
    int             inbox_count;
    pthread_mutex_t inbox_lock;
} rt = {
    .wake = {-1, -1},
    .inbox_lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct settings cfg;
static const char *cfg_get(const char *key, const char *dflt)
{
    const char *v = settings_get(&cfg, key, NULL);
    return (v && *v) ? v : dflt;
}

struct session *relay_session(void)
{
    return chatnav_session(&rt.nav);
}

static void wake_up(void)
{
    if (rt.wake[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(rt.wake[1], &b, 1);
        (void)ignored;
    }
}

static void wake_drain(void)
{
    char buf[64];
    while (rt.wake[0] >= 0 && read(rt.wake[0], buf, sizeof buf) > 0)
        ;
}

static int inbox_push(char *text, int kind, const char *tab_id, int tab)
{
    if (!text)
        return 0;
    pthread_mutex_lock(&rt.inbox_lock);
    int ok = rt.inbox_count < INBOX_MAX;
    if (ok) {
        int at = (rt.inbox_head + rt.inbox_count) % INBOX_MAX;
        rt.inbox[at].text = text;
        rt.inbox[at].kind = kind;
        snprintf(rt.inbox[at].tab_id, sizeof rt.inbox[at].tab_id, "%s",
                 tab_id ? tab_id : "");
        rt.inbox[at].tab = tab;
        rt.inbox_count++;
    }
    pthread_mutex_unlock(&rt.inbox_lock);
    if (ok)
        wake_up();
    else
        free(text);
    return ok;
}

static int inbox_take(struct inbox_item *out, int all)
{
    pthread_mutex_lock(&rt.inbox_lock);
    struct inbox_item *it = &rt.inbox[rt.inbox_head];
    int ok = rt.inbox_count > 0 &&
             (all || it->kind == ITEM_HELLO || it->kind == ITEM_STOP);
    if (ok) {
        *out = *it;
        rt.inbox_head = (rt.inbox_head + 1) % INBOX_MAX;
        rt.inbox_count--;
    }
    pthread_mutex_unlock(&rt.inbox_lock);
    return ok;
}

static void inbox_clear(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    while (rt.inbox_count > 0) {
        free(rt.inbox[rt.inbox_head].text);
        rt.inbox[rt.inbox_head] = (struct inbox_item){0};
        rt.inbox_head = (rt.inbox_head + 1) % INBOX_MAX;
        rt.inbox_count--;
    }
    rt.inbox_head = 0;
    pthread_mutex_unlock(&rt.inbox_lock);
}

int relay_pending(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int n = rt.inbox_count;
    pthread_mutex_unlock(&rt.inbox_lock);
    return n;
}

int relay_fds(int *out, int max)
{
    if (!rt.active || max < 1 || rt.wake[0] < 0)
        return 0;
    out[0] = rt.wake[0];
    return 1;
}

static void send_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return;
    if (rt.ws)
        wsd_send(rt.ws, s, 0);
    free(s);
}

static cJSON *frame(const char *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    return o;
}

static void send_text(const char *t, const char *text)
{
    if (!text || !*text)
        return;
    cJSON *o = frame(t);
    cJSON_AddStringToObject(o, "text", text);
    send_json(o);
}

static void send_note(const char *text)  { send_text("note", text); }
static void send_pre(const char *text)   { send_text("pre", text); }

static void stamp_timing(cJSON *o)
{
    double now = now_seconds();
    double queued = session_event_queued_at();
    cJSON_AddNumberToObject(o, "ts", now);
    cJSON_AddNumberToObject(o, "queue", queued > 0 ? now - queued : 0);
}

static void send_reply(const char *text)
{
    if (!text || !*text)
        return;
    cJSON *o = frame("reply");
    cJSON_AddStringToObject(o, "text", text);
    stamp_timing(o);
    send_json(o);
}

static void send_idle(void)
{
    send_json(frame("idle"));
}

static void send_busy(int on)
{
    on = on ? 1 : 0;
    if (rt.busy_sent == on)
        return;
    rt.busy_sent = on;
    cJSON *o = frame("busy");
    cJSON_AddBoolToObject(o, "on", on);
    send_json(o);
}

static void add_clipped(cJSON *o, const char *key, const char *text, size_t max)
{
    if (!text)
        text = "";
    if (strlen(text) <= max) {
        cJSON_AddStringToObject(o, key, text);
        return;
    }
    char *cut = malloc(max + 2);
    if (!cut)
        return;
    snprintf(cut, max + 2, "%.*s\u2026", (int)max, text);
    cJSON_AddStringToObject(o, key, cut);
    free(cut);
}

static cJSON *image_list(const char *text)
{
    cJSON *list = cJSON_CreateArray();
    if (!rt.files || !text)
        return list;
    for (const char *p = text; (p = strstr(p, "![")) != NULL;) {
        const char *open = strstr(p, "](");
        if (!open)
            break;
        open += 2;
        const char *close = strchr(open, ')');
        p = open;
        if (!close || *open != '/' || close - open > 1023)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%.*s", (int)(close - open), open);
        struct stat st;
        if (stat(path, &st) != 0 || access(path, R_OK) != 0)
            continue;
        unsigned long long h = 1469598103934665603ULL ^ (unsigned long long)st.st_mtime;
        for (const char *c = path; *c; c++)
            h = (h ^ (unsigned char)*c) * 1099511628211ULL;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char name[300], link[4600], url[5000];
        snprintf(name, sizeof name, "%016llx-%s", h, base);
        snprintf(link, sizeof link, "%s/%s", rt.files_dir, name);
        if (symlink(path, link) != 0 && errno != EEXIST)
            continue;
        snprintf(url, sizeof url, "%s/%s", rt.files_base, name);
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "url", url);
        cJSON_AddStringToObject(it, "path", path);
        cJSON_AddItemToArray(list, it);
        p = close;
    }
    return list;
}

static void send_images(const char *text)
{
    cJSON *list = image_list(text), *it;
    cJSON_ArrayForEach(it, list)
    {
        cJSON *o = frame("image");
        cJSON_AddStringToObject(o, "url", cJSON_GetStringValue(cJSON_GetObjectItem(it, "url")));
        cJSON_AddStringToObject(o, "path", cJSON_GetStringValue(cJSON_GetObjectItem(it, "path")));
        send_json(o);
    }
    cJSON_Delete(list);
}

static void send_history(struct session *s)
{
    const struct transcript *t = session_transcript(s);
    cJSON *o = frame("history");
    cJSON *turns = cJSON_AddArrayToObject(o, "turns");
    size_t from = t && t->count > HISTORY_TURNS ? t->count - HISTORY_TURNS : 0;
    for (size_t i = from; t && i < t->count; i++) {
        cJSON *it = cJSON_CreateObject();
        add_clipped(it, "user", t->turns[i].user, HISTORY_BYTES);
        add_clipped(it, "assistant", t->turns[i].assistant, HISTORY_BYTES);
        cJSON *images = image_list(t->turns[i].assistant);
        if (cJSON_GetArraySize(images))
            cJSON_AddItemToObject(it, "images", images);
        else
            cJSON_Delete(images);
        if (t->turns[i].interrupted)
            cJSON_AddBoolToObject(it, "stopped", 1);
        cJSON_AddItemToArray(turns, it);
    }
    send_json(o);
}

static int attached_here(const char *target)
{
    for (int i = 0; i < workspace_count(); i++) {
        const char *remote = session_remote(workspace_at(i));
        if (remote && !strcmp(remote, target))
            return 1;
    }
    return 0;
}

static cJSON *other_windows(void)
{
    static cJSON  *cached;
    static time_t  at;
    time_t         now = time(NULL);
    if (cached && now == at)
        return cJSON_Duplicate(cached, 1);
    cJSON_Delete(cached);
    cached = cJSON_CreateArray();
    at = now;
    struct live_session *v = NULL;
    int                  n = livelist_load(&v);
    for (int i = 0; i < n; i++) {
        if (v[i].pid == (long)getpid() || (!v[i].name[0] && !v[i].id[0]))
            continue;
        char target[200];
        snprintf(target, sizeof target, "%s%s", v[i].name[0] ? "@" : "",
                 v[i].name[0] ? v[i].name : v[i].id);
        if (attached_here(target))
            continue;
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "target", target);
        if (v[i].name[0])
            cJSON_AddStringToObject(it, "name", v[i].name);
        cJSON_AddStringToObject(it, "label",
                                v[i].title[0] ? v[i].title : chatnav_dir_name(v[i].cwd));
        cJSON_AddStringToObject(it, "cwd", v[i].cwd);
        cJSON_AddBoolToObject(it, "busy", !strcmp(v[i].status, "working"));
        cJSON_AddBoolToObject(it, "unseen", v[i].unseen != 0);
        cJSON_AddItemToArray(cached, it);
    }
    free(v);
    return cJSON_Duplicate(cached, 1);
}

static void send_tabs(int force)
{
    static char *sent;
    cJSON *o = frame("tabs");
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    int n = workspace_count();
    for (int i = 0; i < n; i++) {
        struct session *s = workspace_at(i);
        const char *title = session_title(s);
        if (!title || !*title)
            title = chatnav_dir_name(session_cwd(s));
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "index", i + 1);
        cJSON_AddStringToObject(it, "id", session_id(s) ? session_id(s) : "");
        cJSON_AddStringToObject(it, "label", title);
        if (session_name(s) && *session_name(s))
            cJSON_AddStringToObject(it, "name", session_name(s));
        cJSON_AddStringToObject(it, "cwd", session_cwd(s));
        cJSON_AddBoolToObject(it, "current", s == relay_session());
        cJSON_AddBoolToObject(it, "busy", session_busy(s));
        cJSON_AddBoolToObject(it, "unseen", session_unseen(s));
        cJSON_AddItemToArray(items, it);
    }
    cJSON_AddItemToObject(o, "others", other_windows());
    char *now = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!now || (!force && sent && !strcmp(now, sent))) {
        free(now);
        return;
    }
    if (rt.ws)
        wsd_send(rt.ws, now, 0);
    free(sent);
    sent = now;
}

static void send_view(void)
{
    struct session *s = relay_session();
    if (s)
        send_history(s);
    send_tabs(1);
    rt.busy_sent = -1;
    send_busy(session_busy(s));
}

static void send_hello(void)
{
    struct session *s = relay_session();
    cJSON *o = frame("hello");
    cJSON_AddStringToObject(o, "name", APP_NAME);
    cJSON_AddStringToObject(o, "label", rt.label);
    if (s) {
        cJSON_AddStringToObject(o, "cwd", session_cwd(s));
        cJSON_AddStringToObject(o, "title", session_title(s));
        cJSON_AddStringToObject(o, "backend", session_backend(s));
    }
    send_json(o);
    send_view();
}

const char *relay_system_note(void)
{
    size_t n = snprintf(rt.system_note, sizeof rt.system_note,
        "## This session is also on a phone\n\n"
        "The user is at a terminal, but the same session is served to a phone "
        "client over the relay, where replies show as chat messages. Markdown "
        "renders; wide tables and long code listings do not. Keep answers short "
        "and say the answer first.\n");

    if (rt.files_base[0] && n < sizeof rt.system_note)
        snprintf(rt.system_note + n, sizeof rt.system_note - n,
            "\nA markdown image with an absolute local path, "
            "![alt](/abs/path.png), is published as a link the phone can open; "
            "the file has to be on this machine. That only sends it to them; it "
            "does not show it to you.\n");

    return rt.system_note;
}

static cJSON *menu_items;
static char   menu_kind[16];

static void nav_menu_begin(void *ud, const char *kind)
{
    (void)ud;
    snprintf(menu_kind, sizeof menu_kind, "%s", kind);
    cJSON_Delete(menu_items);
    menu_items = cJSON_CreateArray();
}

static void nav_menu_add(void *ud, const char *label, const char *payload)
{
    (void)ud;
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "label", label);
    cJSON_AddStringToObject(it, "payload", payload ? payload : "");
    cJSON_AddItemToArray(menu_items, it);
}

static void nav_menu_send(void *ud, const char *title, int per_row)
{
    (void)ud;
    (void)per_row;
    cJSON *o = frame("menu");
    cJSON_AddStringToObject(o, "kind", menu_kind);
    cJSON_AddStringToObject(o, "title", title ? title : "");
    cJSON_AddItemToObject(o, "items", menu_items ? menu_items : cJSON_CreateArray());
    menu_items = NULL;
    send_json(o);
}

static void nav_note(void *ud, const char *text)
{
    (void)ud;
    send_note(text);
}

static const char *short_path(const char *p)
{
    const char *cwd = relay_session() ? session_cwd(relay_session()) : NULL;
    size_t n = cwd ? strlen(cwd) : 0;
    if (n && !strncmp(p, cwd, n) && p[n] == '/')
        return p + n + 1;
    return p;
}

static void send_tool_line(const backend_event *ev)
{
    char label[48], raw[600];
    toolstyle_label(label, sizeof label, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? view_tool_arg_value(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    text_trunc(raw, sizeof raw, v ? v : "");
    cJSON_Delete(in);
    cJSON *o = frame("tool");
    cJSON_AddStringToObject(o, "name", label);
    cJSON_AddStringToObject(o, "text", raw);
    stamp_timing(o);
    send_json(o);
}

static void on_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (!rt.active || !rt.mirror || s != relay_session())
        return;
    if (ev->parent && *ev->parent)
        return;

    if (ev->kind == BACKEND_EV_TASK) {
        if (session_task_repeat(s))
            rt.repeat_task = 1;
        return;
    }
    if (rt.repeat_task) {
        if (ev->kind == BACKEND_EV_ASSISTANT)
            rt.repeat_task = 0;
        return;
    }

    switch (ev->kind) {
    case BACKEND_EV_TOOL:
        send_tool_line(ev);
        break;
    case BACKEND_EV_ASSISTANT:
        send_reply(ev->text);
        send_images(ev->text);
        break;
    case BACKEND_EV_WARNING:
        send_note(ev->text);
        break;
    default:
        break;
    }
}

static void mirror_prompt(struct session *s)
{
    if (!s || !rt.mirror || !session_busy(s))
        return;
    double started = session_turn_started(s);
    if (started == rt.mirrored_turn)
        return;
    rt.mirrored_turn = started;
    const char *p = session_prompt(s);
    if (rt.sent && p && strstr(p, rt.sent)) {
        free(rt.sent);
        rt.sent = NULL;
        return;
    }
    send_text("user", p);
}

static void send_done(struct session *s)
{
    cJSON *o = frame("done");
    int ok = !session_last_result(s)->is_error;
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok) {
        const char *why = session_last_error(s);
        cJSON_AddStringToObject(o, "error", why ? why : "the turn failed");
    } else if (session_last_interrupted(s)) {
        cJSON_AddBoolToObject(o, "stopped", 1);
    }
    int pct = session_context_percent(s);
    if (pct > 0)
        cJSON_AddNumberToObject(o, "context", pct);
    send_json(o);
}

void relay_turn_done(struct session *s)
{
    if (rt.active && rt.mirror && s == relay_session() &&
        !workspace_queued(workspace_index_of(s)))
        send_done(s);
}

static void stop_turn(void)
{
    session_interrupt(relay_session());
}

static void open_or_switch(const char *id)
{
    int at = workspace_find_id(id);
    if (at >= 0)
        chatnav_cmd_switch(&rt.nav, at);
    else
        chatnav_cmd_open(&rt.nav, NULL, id);
}

static const char *arg_of(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) || (line[n] && line[n] != ' '))
        return NULL;
    const char *arg = line + n;
    while (*arg == ' ')
        arg++;
    return arg;
}

static int nav_command(const char *line)
{
    const char *arg;
    if (!strcmp(line, "/tabs") || !strcmp(line, "/tab") || !strcmp(line, "/sessions")) {
        chatnav_send_tabs(&rt.nav, MENU_MAX);
        return 1;
    }
    if ((arg = arg_of(line, "/tab")) != NULL) {
        chatnav_cmd_switch(&rt.nav, atoi(arg) - 1);
        return 1;
    }
    if ((arg = arg_of(line, "/open")) != NULL) {
        chatnav_cmd_open(&rt.nav, *arg ? arg : NULL, NULL);
        return 1;
    }
    if ((arg = arg_of(line, "/close")) != NULL) {
        chatnav_cmd_close(&rt.nav, *arg ? atoi(arg) - 1 : workspace_index());
        return 1;
    }
    if ((arg = arg_of(line, "/resume")) != NULL) {
        if (*arg)
            open_or_switch(arg);
        else
            chatnav_send_resume(&rt.nav, MENU_MAX);
        return 1;
    }
    if ((arg = arg_of(line, "/attach")) != NULL && *arg) {
        char why[600];
        int  at = cmd_attach_tab(arg, why, sizeof why);
        if (at < 0)
            send_note(why);
        else
            chatnav_cmd_switch(&rt.nav, at);
        return 1;
    }
    if (!strcmp(line, "/stop")) {
        stop_turn();
        return 1;
    }
    return 0;
}

static void run_pick(const char *payload)
{
    if (!strcmp(payload, "new"))
        chatnav_cmd_open(&rt.nav, NULL, NULL);
    else if (!strcmp(payload, "resume"))
        chatnav_send_resume(&rt.nav, MENU_MAX);
    else if (!strcmp(payload, "back"))
        chatnav_send_tabs(&rt.nav, MENU_MAX);
    else if (*payload == '#')
        chatnav_cmd_switch(&rt.nav, chatnav_tab_from_payload(payload));
    else if (strcmp(payload, "cancel"))
        open_or_switch(payload);
}

static int stamped_tab(const struct inbox_item *it)
{
    if (it->tab_id[0]) {
        int at = workspace_find_id(it->tab_id);
        return at >= 0 ? at : -2;
    }
    if (it->tab >= 1)
        return it->tab <= workspace_count() ? it->tab - 1 : -2;
    return -1;
}

static void submit(struct session *s, void *ud)
{
    const char *line = ud;
    int command = cmd_is_command(line);
    if (!cmd_self_echoes(line) && (command || !session_turn_running(s)))
        prompt_echo_message(line);

    frontend_push(0);
    if (!command) {
        rt.repeat_task = 0;
        free(rt.sent);
        rt.sent = strdup(line);
        cmd_submit(s, line);
        frontend_pop();
        return;
    }
    ui_sink_begin_tee();
    enum cmd_result r = cmd_submit(s, line);
    char *raw = ui_sink_end();
    char *shown = ui_plain(raw, 1);
    free(raw);
    frontend_pop();
    if (r == CMD_QUIT)
        send_note("the terminal owns this session; /quit there");
    else if (shown && *shown)
        send_pre(shown);
    else
        send_note("ok");
    free(shown);
}

static void run_line(const struct inbox_item *it)
{
    int at = stamped_tab(it);
    if (at == -2) {
        send_note("that conversation is gone; the tab list has moved");
        return;
    }
    if (at >= 0 && workspace_at(at) != relay_session())
        chatnav_switch(&rt.nav, at);
    if (!relay_session())
        chatnav_refocus(&rt.nav);
    struct session *s = relay_session();
    if (!s) {
        send_note("that session is gone — start a new one at the terminal");
        return;
    }
    if (nav_command(it->text))
        return;
    if (bash_is_command(it->text)) {
        send_note("shell lines run at the terminal only");
        return;
    }
    workspace_render(workspace_index_of(s), submit, it->text);
}

static void run_item(const struct inbox_item *it)
{
    switch (it->kind) {
    case ITEM_HELLO:
        send_hello();
        return;
    case ITEM_STOP:
        stop_turn();
        return;
    case ITEM_PICK:
        run_pick(it->text);
        break;
    default:
        run_line(it);
        break;
    }
    send_idle();
}

void relay_poll(struct session *live)
{
    if (!rt.active)
        return;
    if (!rt.draining) {
        rt.draining = 1;
        wake_drain();
        struct inbox_item it;
        while (inbox_take(&it, !live && !chrome_modal_active())) {
            run_item(&it);
            free(it.text);
        }
        rt.draining = 0;
    }
    mirror_prompt(relay_session());
    send_busy(session_busy(relay_session()));
    send_tabs(0);
}

static void on_text(void *ud, const char *text, size_t n)
{
    (void)ud;
    (void)n;
    cJSON *o = cJSON_Parse(text);
    if (!o)
        return;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(o, "t"));
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
    const char *payload = cJSON_GetStringValue(cJSON_GetObjectItem(o, "payload"));
    const char *tab_id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
    cJSON *tab = cJSON_GetObjectItem(o, "tab");
    int at = cJSON_IsNumber(tab) ? (int)cJSON_GetNumberValue(tab) : 0;
    if (!t)
        ;
    else if (!strcmp(t, "line") && v && *v)
        inbox_push(strdup(v), ITEM_LINE, tab_id, at);
    else if (!strcmp(t, "pick") && payload)
        inbox_push(strdup(payload), ITEM_PICK, NULL, 0);
    else if (!strcmp(t, "stop"))
        inbox_push(strdup(""), ITEM_STOP, NULL, 0);
    else if (!strcmp(t, "hello"))
        inbox_push(strdup(""), ITEM_HELLO, NULL, 0);
    cJSON_Delete(o);
}

static void bind_session(struct session *s, int active, void *ud)
{
    (void)ud;
    session_set_system_extra(s, active ? relay_system_note() : NULL);
    if (active)
        send_view();
}

__attribute__((format(printf, 1, 2)))
static void terminal_note(const char *fmt, ...)
{
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", line);
    viewport_item_end();
    ui_flush();
}

static void files_start(void)
{
    if (!path_config_subdir(rt.files_dir, sizeof rt.files_dir, "relay-files"))
        return;
    mkdir(rt.files_dir, 0700);
    DIR *d = opendir(rt.files_dir);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        char        link[4600];
        struct stat st;
        snprintf(link, sizeof link, "%s/%s", rt.files_dir, e->d_name);
        if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode))
            unlink(link);
    }
    if (d)
        closedir(d);
    int port = atoi(cfg_get("files_port", "0"));
    if (port <= 0)
        port = rt.port + 1;
    rt.files = httpd_start(rt.files_dir, rt.bind[0] ? rt.bind : NULL, port, rt.token);
    if (!rt.files) {
        terminal_note("relay: no file server on %s:%d (port taken?)", rt.bind[0] ? rt.bind : "*", port);
        return;
    }
    snprintf(rt.files_base, sizeof rt.files_base, "http://%s:%d/%s",
             rt.bind[0] ? rt.bind : "127.0.0.1", port, rt.token);
}

static void cleanup(void)
{
    struct session *s = relay_session();
    rt.active = 0;
    wsd_stop(rt.ws);
    rt.ws = NULL;
    httpd_stop(rt.files);
    rt.files = NULL;
    rt.files_base[0] = '\0';
    session_remove_listener(on_event, NULL);
    if (rt.wake[0] >= 0)
        close(rt.wake[0]);
    if (rt.wake[1] >= 0)
        close(rt.wake[1]);
    rt.wake[0] = rt.wake[1] = -1;
    inbox_clear();
    if (s)
        session_set_system_extra(s, tg_label() && tg_session() == s
                                      ? tg_system_note() : NULL);
    cJSON_Delete(menu_items);
    menu_items = NULL;
    chatnav_forget(&rt.nav, s);
    rt.label[0] = '\0';
    rt.repeat_task = 0;
    rt.busy_sent = -1;
    free(rt.sent);
    rt.sent = NULL;
}

int relay_start(struct session *s)
{
    if (rt.active)
        return 0;
    char cfgpath[4200];
    if (path_config_file(cfgpath, sizeof cfgpath, "relay"))
        settings_load(&cfg, cfgpath);
    else
        settings_load(&cfg, "");

    const char *token = cfg_get("token", NULL);
    if (!token || strlen(token) < 16) {
        char path[4200];
        path_config_file(path, sizeof path, "relay");
        fprintf(stderr, APP_NAME ": --relay needs `token` (16+ chars) in %s\n", path);
        return 0;
    }
    snprintf(rt.token, sizeof rt.token, "%s", token);
    const char *port_env = getenv("SCRAP_RELAY_PORT");
    rt.port = atoi(port_env && *port_env ? port_env : cfg_get("port", "0"));
    if (rt.port <= 0)
        rt.port = PORT_DEFAULT;
    const char *bind = cfg_get("bind", NULL);
    if (!bind) {
        bind = wsd_tailscale_ip();
        if (!bind) {
            fprintf(stderr, APP_NAME ": --relay: no tailscale address; set `bind` in the config\n");
            return 0;
        }
    }
    snprintf(rt.bind, sizeof rt.bind, "%s", !strcmp(bind, "*") ? "" : bind);
    rt.mirror = strcmp(cfg_get("mirror", "all"), "off") != 0;
    snprintf(rt.label, sizeof rt.label, "relay");

    if (pipe(rt.wake) != 0) {
        fprintf(stderr, APP_NAME ": relay wake pipe: %s\n", strerror(errno));
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(rt.wake[i], F_SETFL, O_NONBLOCK);
        fcntl(rt.wake[i], F_SETFD, FD_CLOEXEC);
    }

    chatnav_init(&rt.nav, s, bind_session, NULL);
    const struct chatnav_output out = {
        .note = nav_note,
        .menu_begin = nav_menu_begin,
        .menu_add = nav_menu_add,
        .menu_send = nav_menu_send,
    };
    chatnav_set_output(&rt.nav, &out);
    session_add_listener(on_event, NULL);
    rt.busy_sent = -1;

    wsd_opts o = {.bind_ip = rt.bind, .port = rt.port, .token = rt.token};
    rt.ws = wsd_start(&o, on_text, NULL, NULL);
    if (!rt.ws) {
        fprintf(stderr, APP_NAME ": relay: can't listen on %s:%d\n",
                rt.bind[0] ? rt.bind : "*", rt.port);
        goto fail;
    }
    files_start();
    session_set_system_extra(s, relay_system_note());
    rt.active = 1;
    terminal_note("relay on ws://%s:%d", rt.bind[0] ? rt.bind : "*", rt.port);
    return 1;

fail:
    cleanup();
    return 0;
}

void relay_stop(void)
{
    if (!rt.active && !rt.ws && rt.wake[0] < 0)
        return;
    cleanup();
}

const char *relay_label(void)
{
    return rt.active ? rt.label : NULL;
}

void relay_refocus(void)
{
    if (rt.active && !chatnav_session(&rt.nav))
        chatnav_refocus(&rt.nav);
}

void relay_forget_session(struct session *s)
{
    chatnav_forget(&rt.nav, s);
    if (workspace_current() != s)
        relay_refocus();
}
