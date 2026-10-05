#include "relay.h"

#include <ctype.h>
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
#include "chrome.h"
#include "cmd.h"
#include "frontend.h"
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
#define PORT_DEFAULT 8790
#define HISTORY_BYTES 6000
#define HISTORY_BATCH 4000
#define UPLOAD_MAX    8
#define MESSAGE_MAX   (64u << 20)

enum { ITEM_LINE, ITEM_HELLO, ITEM_STOP, ITEM_MORE };

struct inbox_item {
    char *text;
    int   kind;

    char  tab_id[80];
    int   tab;
};

static struct {
    struct session *s;
    wsd            *ws;
    httpd          *files;
    char            files_dir[4200];
    char            files_base[160];
    char            upload_dir[64];
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
    return rt.s;
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
             (all || it->kind == ITEM_HELLO || it->kind == ITEM_STOP || it->kind == ITEM_MORE);
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

static size_t turn_bytes(const struct transcript_turn *t)
{
    size_t u = t->user ? strlen(t->user) : 0, a = t->assistant ? strlen(t->assistant) : 0;
    return (u < HISTORY_BYTES ? u : HISTORY_BYTES) + (a < HISTORY_BYTES ? a : HISTORY_BYTES);
}

static void send_history(struct session *s, long before)
{
    const struct transcript *t = session_transcript(s);
    size_t end = t ? t->count : 0;
    if (before >= 0 && (size_t)before < end)
        end = (size_t)before;
    size_t from = end, bytes = 0;
    while (from > 0 && (from == end || bytes + turn_bytes(&t->turns[from - 1]) <= HISTORY_BATCH))
        bytes += turn_bytes(&t->turns[--from]);
    cJSON *o = frame("history");
    cJSON_AddStringToObject(o, "id", session_id(s) ? session_id(s) : "");
    cJSON_AddNumberToObject(o, "start", (double)from);
    cJSON_AddBoolToObject(o, "older", before >= 0);
    cJSON *turns = cJSON_AddArrayToObject(o, "turns");
    for (size_t i = from; i < end; i++) {
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

static void send_view(void)
{
    struct session *s = relay_session();
    if (s)
        send_history(s, -1);
    rt.mirrored_turn = 0;
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
    struct session *s = relay_session();
    if (!s)
        return;
    if (bash_is_command(it->text)) {
        send_note("shell lines run at the terminal only");
        return;
    }
    workspace_render(workspace_index_of(s), submit, it->text);
}

static void send_older(const struct inbox_item *it)
{
    struct session *s = relay_session();
    const char     *id = s ? session_id(s) : NULL;
    if (id && !strcmp(id, it->tab_id))
        send_history(s, it->tab);
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
    case ITEM_MORE:
        send_older(it);
        return;
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
}

static size_t b64_decode(const char *in, unsigned char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned v = 0;
    int      bits = 0;
    size_t   n = 0;
    for (; *in && *in != '='; in++) {
        const char *at = strchr(A, *in);
        if (!at)
            continue;
        v = v << 6 | (unsigned)(at - A);
        if ((bits += 6) >= 8)
            out[n++] = (unsigned char)(v >> (bits -= 8));
    }
    return n;
}

static int save_upload(const cJSON *f, char *path, size_t size)
{
    static int  seq;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
    const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(f, "data"));
    if (!data || !rt.upload_dir[0])
        return 0;
    char safe[120] = "file";
    if (name && *name) {
        size_t k = 0;
        for (const char *c = name; *c && k < sizeof safe - 1; c++)
            safe[k++] = isalnum((unsigned char)*c) || strchr("._-", *c) ? *c : '_';
        safe[k] = '\0';
    }
    snprintf(path, size, "%s/%ld-%d-%s", rt.upload_dir, (long)time(NULL), ++seq, safe);
    unsigned char *bytes = malloc(strlen(data) / 4 * 3 + 3);
    FILE          *out = bytes ? fopen(path, "wb") : NULL;
    size_t         len = bytes ? b64_decode(data, bytes) : 0;
    int            ok = out && fwrite(bytes, 1, len, out) == len;
    if (out && fclose(out) != 0)
        ok = 0;
    free(bytes);
    if (!ok)
        unlink(path);
    return ok;
}

static char *with_uploads(const char *text, const cJSON *files)
{
    char   paths[UPLOAD_MAX][200];
    int    n = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, files)
    {
        if (n < UPLOAD_MAX && save_upload(f, paths[n], sizeof paths[n]))
            n++;
    }
    if (!n)
        return text && *text ? strdup(text) : NULL;
    size_t cap = (text ? strlen(text) : 0) + 64 + n * sizeof paths[0];
    char  *out = malloc(cap);
    if (!out)
        return NULL;
    size_t at = (size_t)snprintf(out, cap, "I sent %s:\n", n == 1 ? "a file" : "some files");
    for (int i = 0; i < n; i++)
        at += (size_t)snprintf(out + at, cap - at, "- %s\n", paths[i]);
    if (text && *text)
        snprintf(out + at, cap - at, "\n%s", text);
    return out;
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
    const char *tab_id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
    if (!t)
        ;
    else if (!strcmp(t, "line"))
        inbox_push(with_uploads(v, cJSON_GetObjectItem(o, "files")), ITEM_LINE, NULL, 0);
    else if (!strcmp(t, "stop"))
        inbox_push(strdup(""), ITEM_STOP, NULL, 0);
    else if (!strcmp(t, "more"))
        inbox_push(strdup(""), ITEM_MORE, tab_id, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "before")));
    else if (!strcmp(t, "hello"))
        inbox_push(strdup(""), ITEM_HELLO, NULL, 0);
    cJSON_Delete(o);
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
    if (rt.upload_dir[0])
        rmdir(rt.upload_dir);
    rt.upload_dir[0] = '\0';
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
    rt.s = NULL;
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

    rt.s = s;
    session_add_listener(on_event, NULL);
    rt.busy_sent = -1;

    snprintf(rt.upload_dir, sizeof rt.upload_dir, "/tmp/" APP_NAME "_relay_XXXXXX");
    if (!mkdtemp(rt.upload_dir))
        rt.upload_dir[0] = '\0';
    wsd_opts o = {.bind_ip = rt.bind, .port = rt.port, .token = rt.token,
                  .max_message = MESSAGE_MAX};
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

void relay_forget_session(struct session *s)
{
    if (s && s == rt.s)
        relay_stop();
}
