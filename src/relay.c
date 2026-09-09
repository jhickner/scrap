#include "relay.h"

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
#include "cmd.h"
#include "frontend.h"
#include "gitinfo.h"
#include "restart.h"
#include "session.h"
#include "sessionview.h"
#include "settings.h"
#include "status.h"
#include "tasks.h"
#include "text.h"
#include "tgbridge.h"
#include "transcript.h"
#include "toolstyle.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define INBOX_MAX 32
#define MENU_MAX  16
#define PORT_DEFAULT 8790
#define HISTORY_TURNS 6
#define HISTORY_BYTES 6000

enum { MIRROR_OFF = 0, MIRROR_REMOTE = 1, MIRROR_ALL = 2 };
enum { ITEM_LINE = 0, ITEM_PICK = 1, ITEM_HELLO = 2 };

struct inbox_item {
    char *text;
    int   kind;
};

struct relay_menu {
    char kind[16];
    char payload[MENU_MAX][200];
    int  count;
};

static struct {
    struct tgbridge bridge;
    wsd            *ws;
    httpd          *files;
    char            files_dir[4200];
    char            files_base[160];
    int             active;
    int             mirror;
    int             wake[2];
    char            label[64];
    char            client[64];
    char            token[128];
    int             port;
    char            bind[64];
    volatile int    stop_wanted;
    int             from_chat;
    int             repeat_task;
    int             busy_sent;
    struct inbox_item inbox[INBOX_MAX];
    int             inbox_head;
    int             inbox_count;
    pthread_mutex_t inbox_lock;
    struct relay_menu menu;
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

static struct session *current_session(void)
{
    return tgbridge_session(&rt.bridge);
}

/* ---- inbox: server thread -> main loop ---------------------------------- */

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

static int inbox_push(char *text, int kind)
{
    if (!text)
        return 0;
    pthread_mutex_lock(&rt.inbox_lock);
    int ok = rt.inbox_count < INBOX_MAX;
    if (ok) {
        int at = (rt.inbox_head + rt.inbox_count) % INBOX_MAX;
        rt.inbox[at].text = text;
        rt.inbox[at].kind = kind;
        rt.inbox_count++;
    }
    pthread_mutex_unlock(&rt.inbox_lock);
    if (ok)
        wake_up();
    else
        free(text);
    return ok;
}

static char *inbox_take(int *kind)
{
    pthread_mutex_lock(&rt.inbox_lock);
    char *text = NULL;
    if (rt.inbox_count > 0) {
        text = rt.inbox[rt.inbox_head].text;
        *kind = rt.inbox[rt.inbox_head].kind;
        rt.inbox_head = (rt.inbox_head + 1) % INBOX_MAX;
        rt.inbox_count--;
    }
    int remaining = rt.inbox_count;
    pthread_mutex_unlock(&rt.inbox_lock);
    if (text) {
        wake_drain();
        if (remaining)
            wake_up();
    }
    return text;
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

/* ---- outbound frames ---------------------------------------------------- */

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
static void send_reply(const char *text) { send_text("reply", text); }
static void send_pre(const char *text)   { send_text("pre", text); }

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
        if (t->turns[i].interrupted)
            cJSON_AddBoolToObject(it, "stopped", 1);
        cJSON_AddItemToArray(turns, it);
    }
    send_json(o);
}

static void send_tabs(void)
{
    cJSON *o = frame("tabs");
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    int n = workspace_count();
    for (int i = 0; i < n; i++) {
        struct session *s = workspace_at(i);
        const char *title = session_title(s);
        if (!title || !*title)
            title = tgbridge_dir_name(session_cwd(s));
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "index", i + 1);
        cJSON_AddStringToObject(it, "label", title);
        cJSON_AddStringToObject(it, "cwd", session_cwd(s));
        cJSON_AddBoolToObject(it, "current", s == current_session());
        cJSON_AddBoolToObject(it, "busy", session_busy(s));
        cJSON_AddBoolToObject(it, "unseen", session_unseen(s));
        cJSON_AddItemToArray(items, it);
    }
    send_json(o);
}

static void send_hello(void)
{
    struct session *s = current_session();
    cJSON *o = frame("hello");
    cJSON_AddStringToObject(o, "name", APP_NAME);
    cJSON_AddStringToObject(o, "label", rt.label);
    if (s) {
        cJSON_AddStringToObject(o, "cwd", session_cwd(s));
        cJSON_AddStringToObject(o, "title", session_title(s));
        cJSON_AddStringToObject(o, "backend", session_backend(s));
    }
    send_json(o);
    if (s)
        send_history(s);
    send_tabs();
    rt.busy_sent = -1;
    send_busy(session_busy(s));
}

/* Images the reply points at (![](/abs/path)) are published as links: the
   file is linked into a private directory the file server roots at. */
static void send_images(const char *text)
{
    if (!rt.files || !text)
        return;
    for (const char *p = text; (p = strstr(p, "![")) != NULL;) {
        const char *open = strstr(p, "](");
        if (!open)
            return;
        open += 2;
        const char *close = strchr(open, ')');
        p = open;
        if (!close || *open != '/' || close - open > 1023)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%.*s", (int)(close - open), open);
        if (access(path, R_OK) != 0)
            continue;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char name[300], link[4600], url[5000];
        snprintf(name, sizeof name, "%ld-%s", (long)time(NULL), base);
        snprintf(link, sizeof link, "%s/%s", rt.files_dir, name);
        if (symlink(path, link) != 0 && errno != EEXIST)
            continue;
        snprintf(url, sizeof url, "%s/%s", rt.files_base, name);
        cJSON *o = frame("image");
        cJSON_AddStringToObject(o, "url", url);
        cJSON_AddStringToObject(o, "path", path);
        send_json(o);
        p = close;
    }
}

/* ---- menus (tabs, resume) ----------------------------------------------- */

static void menu_begin(const char *kind)
{
    rt.menu.count = 0;
    snprintf(rt.menu.kind, sizeof rt.menu.kind, "%s", kind);
}

static void menu_add(const char *label, const char *payload)
{
    (void)label;
    if (rt.menu.count >= MENU_MAX)
        return;
    snprintf(rt.menu.payload[rt.menu.count], sizeof rt.menu.payload[0], "%s",
             payload ? payload : "");
    rt.menu.count++;
}

static cJSON *menu_items;

static void bridge_menu_begin(void *ud, const char *kind)
{
    (void)ud;
    menu_begin(kind);
    cJSON_Delete(menu_items);
    menu_items = cJSON_CreateArray();
}

static void bridge_menu_add(void *ud, const char *label, const char *payload)
{
    (void)ud;
    menu_add(label, payload);
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "label", label);
    cJSON_AddStringToObject(it, "payload", payload ? payload : "");
    cJSON_AddItemToArray(menu_items, it);
}

static void bridge_menu_send(void *ud, const char *title, int per_row)
{
    (void)ud;
    (void)per_row;
    cJSON *o = frame("menu");
    cJSON_AddStringToObject(o, "kind", rt.menu.kind);
    cJSON_AddStringToObject(o, "title", title ? title : "");
    cJSON_AddItemToObject(o, "items", menu_items ? menu_items : cJSON_CreateArray());
    menu_items = NULL;
    send_json(o);
}

static void bridge_note(void *ud, const char *text)
{
    (void)ud;
    send_note(text);
}

static int menu_known(const char *payload)
{
    for (int i = 0; i < rt.menu.count; i++)
        if (!strcmp(rt.menu.payload[i], payload))
            return 1;
    return 0;
}

static char *menu_line(const char *payload)
{
    char line[256];
    if (!menu_known(payload))
        return NULL;
    if (!strcmp(rt.menu.kind, "tab")) {
        if (!strcmp(payload, "new")) {
            snprintf(line, sizeof line, "/open");
        } else if (!strcmp(payload, "resume")) {
            snprintf(line, sizeof line, "/resume");
        } else if (!strcmp(payload, "cancel")) {
            return NULL;
        } else {
            int at = tgbridge_tab_from_payload(payload);
            if (at < 0) {
                send_note("that conversation is gone");
                return NULL;
            }
            snprintf(line, sizeof line, "/tab %d", at + 1);
        }
    } else if (!strcmp(rt.menu.kind, "resume")) {
        if (!strcmp(payload, "back"))
            snprintf(line, sizeof line, "/tabs");
        else
            snprintf(line, sizeof line, "/resume %s", payload);
    } else {
        return NULL;
    }
    return strdup(line);
}

/* ---- session events ----------------------------------------------------- */

static int mirroring(void)
{
    if (!rt.active || rt.mirror == MIRROR_OFF)
        return 0;
    return rt.mirror == MIRROR_ALL || rt.from_chat;
}

static const char *short_path(const char *p)
{
    const char *cwd = current_session() ? session_cwd(current_session()) : NULL;
    size_t n = cwd ? strlen(cwd) : 0;
    if (n && !strncmp(p, cwd, n) && p[n] == '/')
        return p + n + 1;
    return p;
}

static void send_tool_line(const backend_event *ev)
{
    char label[48], raw[600], one[200];
    toolstyle_label(label, sizeof label, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? view_tool_arg_value(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    text_trunc(raw, sizeof raw, v ? v : "");
    cJSON_Delete(in);
    text_trunc(one, sizeof one, raw);
    for (size_t i = 0; one[i]; i++)
        if (one[i] == '\n' || one[i] == '\r' || one[i] == '\t')
            one[i] = ' ';
    cJSON *o = frame("tool");
    cJSON_AddStringToObject(o, "name", label);
    cJSON_AddStringToObject(o, "text", one);
    send_json(o);
}

static void on_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (s != current_session() || !mirroring())
        return;
    if (ev->parent && *ev->parent)
        return;

    if (ev->kind == BACKEND_EV_TASK) {
        if (session_task_repeat(s))
            rt.repeat_task = 1;
        return;
    }
    if (rt.repeat_task && !rt.from_chat) {
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

int relay_poll(struct session *live)
{
    if (!rt.active)
        return 0;
    if (live == current_session() || !live)
        send_busy(session_busy(current_session()));
    if (rt.stop_wanted && (!live || live == current_session())) {
        rt.stop_wanted = 0;
        return 1;
    }
    return 0;
}

/* ---- server thread callbacks -------------------------------------------- */

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
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
    if (!t) {
        cJSON_Delete(o);
        return;
    }
    if (!strcmp(t, "line") && v && *v) {
        inbox_push(strdup(v), ITEM_LINE);
    } else if (!strcmp(t, "pick") && payload) {
        inbox_push(strdup(payload), ITEM_PICK);
    } else if (!strcmp(t, "stop")) {
        rt.stop_wanted = 1;
    } else if (!strcmp(t, "hello")) {
        snprintf(rt.client, sizeof rt.client, "%s", name ? name : "client");
        inbox_push(strdup(""), ITEM_HELLO);
    }
    cJSON_Delete(o);
}

static void on_state(void *ud, int connected)
{
    (void)ud;
    if (!connected)
        rt.client[0] = '\0';
}

/* ---- lines from the phone ----------------------------------------------- */

static const char HELP[] =
    "Anything you say is one turn in a live mux session, including slash "
    "commands.\n\n"
    "/tabs        the conversations open here, to switch between\n"
    "/open [dir]  another conversation, here or somewhere else\n"
    "/close [n]   close one\n"
    "/resume      reopen a past conversation from this directory\n"
    "/stop        abandon the turn in flight\n"
    "/relay       the bridge's own settings, and this\n\n"
    "Settings live in ~/.config/mux/relay.";

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

static void send_status(void)
{
    char msg[600];
    snprintf(msg, sizeof msg,
             "%-8s %s\n%-8s %s:%d\n%-8s %s\n%-8s %s\n%-8s %d of %d",
             "bridge", rt.label, "listen", rt.bind[0] ? rt.bind : "*", rt.port,
             "client", rt.client[0] ? rt.client : "none", "mirror",
             rt.mirror == MIRROR_OFF ? "off" : rt.mirror == MIRROR_REMOTE ? "remote" : "all",
             "tab", workspace_index() + 1, workspace_count());
    send_pre(msg);
}

static int bridge_command(const char *line)
{
    const char *arg;
    if (!strcmp(line, "/tabs") || !strcmp(line, "/tab") || !strcmp(line, "/sessions")) {
        tgbridge_send_tabs(&rt.bridge, MENU_MAX);
        return 1;
    }
    if ((arg = arg_of(line, "/tab")) != NULL && *arg) {
        tgbridge_switch_tab(&rt.bridge, atoi(arg) - 1);
        return 1;
    }
    if ((arg = arg_of(line, "/open")) != NULL) {
        tgbridge_open_tab(&rt.bridge, *arg ? arg : NULL, NULL);
        return 1;
    }
    if ((arg = arg_of(line, "/close")) != NULL) {
        tgbridge_close_tab(&rt.bridge, *arg ? atoi(arg) - 1 : workspace_index());
        return 1;
    }
    if ((arg = arg_of(line, "/resume")) != NULL) {
        if (*arg) {
            int at = workspace_find_id(arg);
            if (at >= 0)
                tgbridge_switch_tab(&rt.bridge, at);
            else
                tgbridge_open_tab(&rt.bridge, NULL, arg);
        } else {
            tgbridge_send_resume(&rt.bridge, MENU_MAX);
        }
        return 1;
    }
    if (!strcmp(line, "/relay")) {
        send_status();
        send_pre(HELP);
        return 1;
    }
    if (!strcmp(line, "/stop"))
        return 1;
    return 0;
}

static void send_turn_reply(int ok)
{
    struct session *s = current_session();
    cJSON *o = frame("done");
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

static void run_line(char *line)
{
    rt.from_chat = 1;
    rt.repeat_task = 0;
    rt.stop_wanted = 0;
    frontend_push(0);

    if (bridge_command(line))
        goto done;

    if (!current_session())
        tgbridge_refocus(&rt.bridge);
    if (!current_session()) {
        send_note("that session is gone — start a new one at the terminal");
        goto done;
    }

    if (bash_is_command(line)) {
        tty_watch(tgbridge_workspace_fds, tgbridge_workspace_ready, NULL);
        bash_run(line);
        tty_watch(NULL, NULL, NULL);
        gitinfo_forget();
        char *context = bash_take_context();
        if (context) {
            send_pre(context);
            send_turn_reply(session_turn(current_session(), context));
            cmd_run_deferred(current_session());
            free(context);
        }
        goto done;
    }

    ui_sink_begin_tee();
    enum cmd_result r = cmd_dispatch(current_session(), line);
    char *raw = ui_sink_end();
    char *shown = ui_plain(raw, 1);
    free(raw);
    if (r != CMD_NOT_A_COMMAND) {
        if (shown && *shown)
            send_pre(shown);
        else
            send_note("ok");
    }
    free(shown);

    if (r == CMD_QUIT) {
        send_note("the terminal owns this session; /quit there");
        goto done;
    }
    if (r == CMD_NOT_A_COMMAND) {
        status_sticky_prompt(line);
        send_turn_reply(session_turn(current_session(), line));
        cmd_run_deferred(current_session());
    }

done:
    rt.from_chat = 0;
    frontend_pop();
    free(line);
    relay_poll(NULL);
}

void relay_run_line(char *line)
{
    run_line(line);
}

char *relay_take_line(void)
{
    if (!rt.active)
        return NULL;
    for (;;) {
        int kind = 0;
        char *line = inbox_take(&kind);
        if (!line)
            return NULL;
        if (kind == ITEM_LINE)
            return line;
        if (kind == ITEM_HELLO) {
            free(line);
            send_hello();
            continue;
        }
        char *cmd = menu_line(line);
        free(line);
        if (cmd)
            return cmd;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

static void bind_session(struct session *s, int active, void *ud)
{
    (void)s;
    (void)ud;
    if (active) {
        rt.busy_sent = -1;
        send_hello();
    }
}

__attribute__((format(printf, 1, 2)))
static void note_up(const char *fmt, ...)
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
    int port = atoi(cfg_get("files_port", "0"));
    if (port <= 0)
        port = rt.port + 1;
    rt.files = httpd_start(rt.files_dir, rt.bind[0] ? rt.bind : NULL, port, rt.token);
    if (!rt.files) {
        note_up("relay: no file server on %s:%d (port taken?)", rt.bind[0] ? rt.bind : "*", port);
        return;
    }
    snprintf(rt.files_base, sizeof rt.files_base, "http://%s:%d/%s",
             rt.bind[0] ? rt.bind : "127.0.0.1", port, rt.token);
}

static void cleanup(void)
{
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
    cJSON_Delete(menu_items);
    menu_items = NULL;
    tgbridge_forget(&rt.bridge, current_session());
    rt.label[0] = rt.client[0] = '\0';
    rt.from_chat = rt.repeat_task = rt.stop_wanted = 0;
    rt.busy_sent = -1;
    rt.menu = (struct relay_menu){0};
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
    const char *port_env = getenv("MUX_RELAY_PORT");
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
    const char *m = cfg_get("mirror", "all");
    rt.mirror = !strcmp(m, "off") ? MIRROR_OFF : !strcmp(m, "remote") ? MIRROR_REMOTE
                                                                    : MIRROR_ALL;
    snprintf(rt.label, sizeof rt.label, "relay");

    if (pipe(rt.wake) != 0) {
        fprintf(stderr, APP_NAME ": relay wake pipe: %s\n", strerror(errno));
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(rt.wake[i], F_SETFL, O_NONBLOCK);
        fcntl(rt.wake[i], F_SETFD, FD_CLOEXEC);
    }

    tgbridge_init(&rt.bridge, s, bind_session, NULL);
    const struct tgbridge_output out = {
        .note = bridge_note,
        .menu_begin = bridge_menu_begin,
        .menu_add = bridge_menu_add,
        .menu_send = bridge_menu_send,
    };
    tgbridge_set_output(&rt.bridge, &out);
    session_add_listener(on_event, NULL);
    rt.busy_sent = -1;

    wsd_opts o = {.bind_ip = rt.bind, .port = rt.port, .token = rt.token};
    rt.ws = wsd_start(&o, on_text, on_state, NULL);
    if (!rt.ws) {
        fprintf(stderr, APP_NAME ": relay: can't listen on %s:%d\n",
                rt.bind[0] ? rt.bind : "*", rt.port);
        goto fail;
    }
    files_start();
    restart_flag("--relay");
    rt.active = 1;
    note_up("relay on ws://%s:%d", rt.bind[0] ? rt.bind : "*", rt.port);
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

struct session *relay_session(void)
{
    return current_session();
}

void relay_refocus(void)
{
    if (rt.active)
        tgbridge_refocus(&rt.bridge);
}

void relay_forget_session(struct session *s)
{
    tgbridge_forget(&rt.bridge, s);
}
