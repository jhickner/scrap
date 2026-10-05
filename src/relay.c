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

#include "app.h"
#include "askblock.h"
#include "bash.h"
#include "chrome.h"
#include "cmd.h"
#include "frontend.h"
#include "highlight.h"
#include "hud.h"
#include "pick.h"
#include "prompt.h"
#include "session.h"
#include "sessionswitch.h"
#include "sessionview.h"
#include "settings.h"
#include "text.h"
#include "tg.h"
#include "transcript.h"
#include "toolstyle.h"
#include "ui.h"
#include "viewport.h"
#include "tabs.h"
#include "workspace.h"

#define PROTOCOL      2
#define INBOX_MAX     256
#define PORT_DEFAULT  8790
#define CLIP          16000
#define RESULT_CLIP   8000
#define WINDOW        50
#define OLDER_MAX     200
#define UPLOAD_MAX    8
#define MESSAGE_MAX   (64u << 20)
#define QUEUE_MAX     (96u << 20)
#define FILE_MAX      (32u << 20)
#define DONE_MAX      64
#define SENT_MAX      16
#define OPEN_TOOLS    64
#define REPLAY_MAX    256
#define LOGS_MAX      32

struct item {
    int    client;
    cJSON *msg;
};

struct client {
    int  id;
    char uuid[64];
};

struct done {
    char  key[160];
    char *res;
};

struct sent {
    char req[160];
    char *text;
};

/* A tab's entries, recorded whether or not it is the served session. */
struct log {
    struct session *s;
    cJSON          *entries;
    long            open_tools[OPEN_TOOLS];
    int             nopen;
    int             repeat_task;
    double          mirrored_turn;
    char            sid[128];
    size_t          tcount;
};

static struct {
    struct session *s;
    wsd            *ws;
    char            upload_dir[64];
    int             active;
    int             wake[2];
    char            label[64];
    char            token[128];
    int             port;
    char            bind[64];
    int             draining;
    char            system_note[900];
    char            server[40];

    struct client   clients[WSD_MAX_CLIENTS];
    long            seq;
    long            replay_base;
    char           *replay[REPLAY_MAX];
    int             binding;

    struct log      logs[LOGS_MAX];
    struct log     *cur;
    long            next_id;
    char           *session_json;
    char           *live_json;

    struct askblock *ask;
    long            ask_id;

    struct sent     sent[SENT_MAX];
    int             nsent;
    struct done     done[DONE_MAX];
    int             done_next;

    struct item     inbox[INBOX_MAX];
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

/* ---- inbox: wsd thread to main thread ---------------------------------- */

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

static const char *op_of(const cJSON *msg)
{
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "op"));
    return op ? op : "";
}

/* Requests that change the session wait for the terminal to be idle; the
 * rest run on arrival, also inside a running turn. */
static int needs_idle(const cJSON *msg)
{
    const char *op = op_of(msg);
    return !strcmp(op, "send") || !strcmp(op, "answer") || !strcmp(op, "open") ||
           !strcmp(op, "new") || !strcmp(op, "close");
}

static int inbox_push(int client, cJSON *msg)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int ok = rt.inbox_count < INBOX_MAX;
    if (ok)
        rt.inbox[rt.inbox_count++] = (struct item){client, msg};
    pthread_mutex_unlock(&rt.inbox_lock);
    if (ok)
        wake_up();
    return ok;
}

static int inbox_take(struct item *out, int idle)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int at = -1;
    for (int i = 0; i < rt.inbox_count && at < 0; i++)
        if (idle || !needs_idle(rt.inbox[i].msg))
            at = i;
    if (at >= 0) {
        *out = rt.inbox[at];
        memmove(rt.inbox + at, rt.inbox + at + 1,
                (size_t)(rt.inbox_count - at - 1) * sizeof rt.inbox[0]);
        rt.inbox_count--;
    }
    pthread_mutex_unlock(&rt.inbox_lock);
    return at >= 0;
}

static void inbox_clear(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    for (int i = 0; i < rt.inbox_count; i++)
        cJSON_Delete(rt.inbox[i].msg);
    rt.inbox_count = 0;
    pthread_mutex_unlock(&rt.inbox_lock);
}

int relay_pending(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int n = 0;
    for (int i = 0; i < rt.inbox_count; i++)
        n += needs_idle(rt.inbox[i].msg);
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

/* ---- output ------------------------------------------------------------- */

static void send_to(int client, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s && rt.ws)
        wsd_send(rt.ws, client, s, 0);
    free(s);
}

static cJSON *frame(const char *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", t);
    return o;
}

static cJSON *res_ok(const char *id)
{
    cJSON *o = frame("res");
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "ok", 1);
    return o;
}

static cJSON *res_error(const char *id, const char *code, const char *msg)
{
    cJSON *o = frame("res");
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "msg", msg);
    return o;
}

static struct client *client_find(int id)
{
    for (int i = 0; i < WSD_MAX_CLIENTS; i++)
        if (rt.clients[i].id == id)
            return &rt.clients[i];
    return NULL;
}

static void emit(cJSON *op)
{
    cJSON *o = frame("delta");
    cJSON_AddNumberToObject(o, "seq", (double)++rt.seq);
    cJSON *ops = cJSON_AddArrayToObject(o, "ops");
    cJSON_AddItemToArray(ops, op);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    for (int i = 0; s && rt.ws && i < WSD_MAX_CLIENTS; i++)
        if (rt.clients[i].id)
            wsd_send(rt.ws, rt.clients[i].id, s, 0);
    char **slot = &rt.replay[rt.seq % REPLAY_MAX];
    free(*slot);
    *slot = s;
    if (!s)
        rt.replay_base = rt.seq;
}

static void replay_clear(void)
{
    for (int i = 0; i < REPLAY_MAX; i++) {
        free(rt.replay[i]);
        rt.replay[i] = NULL;
    }
}

static cJSON *op2(const char *name, cJSON *a)
{
    cJSON *op = cJSON_CreateArray();
    cJSON_AddItemToArray(op, cJSON_CreateString(name));
    cJSON_AddItemToArray(op, a ? a : cJSON_CreateNull());
    return op;
}

/* ---- entries ------------------------------------------------------------ */

static cJSON *clipped_string(const char *text, size_t max, int *cut)
{
    if (!text)
        text = "";
    size_t n = strlen(text);
    if (n <= max)
        return cJSON_CreateString(text);
    while (max > 0 && ((unsigned char)text[max] & 0xC0) == 0x80)
        max--;
    char *s = malloc(max + 1);
    if (!s)
        return cJSON_CreateString("");
    memcpy(s, text, max);
    s[max] = '\0';
    cJSON *o = cJSON_CreateString(s);
    free(s);
    *cut = 1;
    return o;
}

static int is_tool(const cJSON *e)
{
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(e, "kind"));
    return kind && !strcmp(kind, "tool");
}

/* An entry as sent in a view or delta: long text fields clipped, with
 * `clipped` set so the client can fetch the whole entry. A tool's input,
 * result and diff are held back behind `held` until the client asks. */
static cJSON *entry_out(const cJSON *e, int tool)
{
    cJSON *o = cJSON_CreateObject();
    int    cut = 0, held = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, e)
    {
        if (tool && (!strcmp(f->string, "input") || !strcmp(f->string, "result") ||
                     !strcmp(f->string, "diff")))
            held = 1;
        else if (cJSON_IsString(f) && strlen(f->valuestring) > CLIP)
            cJSON_AddItemToObject(o, f->string, clipped_string(f->valuestring, CLIP, &cut));
        else
            cJSON_AddItemToObject(o, f->string, cJSON_Duplicate(f, 1));
    }
    if (cut)
        cJSON_AddBoolToObject(o, "clipped", 1);
    if (held)
        cJSON_AddBoolToObject(o, "held", 1);
    return o;
}

static cJSON *entry_new(const char *kind, const char *text)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "id", (double)++rt.next_id);
    cJSON_AddStringToObject(e, "kind", kind);
    cJSON_AddNumberToObject(e, "ts", now_seconds());
    if (text)
        cJSON_AddStringToObject(e, "text", text);
    return e;
}

static void entry_add(struct log *l, cJSON *e)
{
    cJSON_AddItemToArray(l->entries, e);
    if (l == rt.cur)
        emit(op2("add", entry_out(e, is_tool(e))));
}

static cJSON *entry_find(struct log *l, long id, int *at)
{
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, l->entries)
    {
        if ((long)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id")) == id) {
            if (at)
                *at = i;
            return e;
        }
        i++;
    }
    return NULL;
}

/* Merge fields into an entry and publish them. */
static void entry_set(struct log *l, long id, cJSON *fields)
{
    cJSON *e = entry_find(l, id, NULL);
    if (!e) {
        cJSON_Delete(fields);
        return;
    }
    cJSON *f;
    cJSON_ArrayForEach(f, fields)
    {
        cJSON_DeleteItemFromObject(e, f->string);
        cJSON_AddItemToObject(e, f->string, cJSON_Duplicate(f, 1));
    }
    if (l != rt.cur) {
        cJSON_Delete(fields);
        return;
    }
    cJSON *op = cJSON_CreateArray();
    cJSON_AddItemToArray(op, cJSON_CreateString("set"));
    cJSON_AddItemToArray(op, cJSON_CreateNumber((double)id));
    cJSON_AddItemToArray(op, entry_out(fields, is_tool(e)));
    cJSON_Delete(fields);
    emit(op);
}

/* Absolute paths of the readable images a reply links as ![..](/abs/path). */
static cJSON *image_list(const char *text)
{
    cJSON *list = cJSON_CreateArray();
    for (const char *p = text; p && (p = strstr(p, "![")) != NULL;) {
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
        if (access(path, R_OK) == 0)
            cJSON_AddItemToArray(list, cJSON_CreateString(path));
        p = close;
    }
    return list;
}

static cJSON *assistant_entry(const char *text)
{
    cJSON *e = entry_new("assistant", text);
    cJSON *images = image_list(text);
    if (cJSON_GetArraySize(images))
        cJSON_AddItemToObject(e, "images", images);
    else
        cJSON_Delete(images);
    return e;
}

/* `keep` appends to the current entries, as a terminal keeps scrollback across /clear. */
static void entries_load(struct log *l, int keep)
{
    struct session *s = l->s;
    if (!keep || !l->entries) {
        cJSON_Delete(l->entries);
        l->entries = cJSON_CreateArray();
    } else if (cJSON_GetArraySize(l->entries)) {
        cJSON_AddItemToArray(l->entries, entry_new("note", "── new conversation ──"));
    }
    l->nopen = 0;
    const struct transcript *t = session_transcript(s);
    for (size_t i = 0; t && i < t->count; i++) {
        const struct transcript_turn *turn = &t->turns[i];
        if (turn->user && *turn->user)
            cJSON_AddItemToArray(l->entries, entry_new("user", turn->user));
        if (turn->assistant && *turn->assistant)
            cJSON_AddItemToArray(l->entries, assistant_entry(turn->assistant));
        if (turn->interrupted) {
            cJSON *e = entry_new("end", NULL);
            cJSON_AddBoolToObject(e, "stopped", 1);
            cJSON_AddItemToArray(l->entries, e);
        }
    }
    l->tcount = t ? t->count : 0;
    snprintf(l->sid, sizeof l->sid, "%s", session_id(s) ? session_id(s) : "");
    l->mirrored_turn = 0;
    l->repeat_task = 0;
}

static void mirror_prompt(struct log *l);

static struct log *log_find(const struct session *s)
{
    for (int i = 0; s && i < LOGS_MAX; i++)
        if (rt.logs[i].s == s)
            return &rt.logs[i];
    return NULL;
}

static void log_free(struct log *l)
{
    cJSON_Delete(l->entries);
    *l = (struct log){0};
}

/* The session's log, started from its transcript the first time it is seen. */
static struct log *log_for(struct session *s)
{
    struct log *l = log_find(s);
    if (l || !s)
        return l;
    for (int i = 0; i < LOGS_MAX && !l; i++)
        if (!rt.logs[i].s)
            l = &rt.logs[i];
    /* ponytail: evicts the first unserved log when full; LRU if 32 tabs is ever too few. */
    for (int i = 0; i < LOGS_MAX && !l; i++)
        if (&rt.logs[i] != rt.cur) {
            l = &rt.logs[i];
            log_free(l);
        }
    l->s = s;
    entries_load(l, 0);
    mirror_prompt(l);
    return l;
}

/* ---- view --------------------------------------------------------------- */

static cJSON *session_obj(struct session *s)
{
    cJSON *o = cJSON_CreateObject();
    const char *id = session_id(s), *model = session_model_label(s);
    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddStringToObject(o, "title", session_title(s));
    cJSON_AddStringToObject(o, "cwd", session_cwd(s));
    cJSON_AddStringToObject(o, "backend", session_backend(s));
    cJSON_AddStringToObject(o, "model", model ? model : "");
    cJSON_AddStringToObject(o, "name", session_name(s));
    cJSON_AddItemToObject(o, "hud", hud_rows(s));
    return o;
}

static cJSON *live_obj(struct session *s)
{
    cJSON *o = cJSON_CreateObject();
    int busy = session_busy(s);
    cJSON_AddBoolToObject(o, "busy", busy);
    if (busy && session_turn_started(s) > 0)
        cJSON_AddNumberToObject(o, "started", session_turn_started(s));
    int pct = session_context_percent(s);
    if (pct > 0)
        cJSON_AddNumberToObject(o, "context", pct);
    cJSON *q = cJSON_AddArrayToObject(o, "queue");
    int tab = workspace_index_of(s);
    for (int i = 0; i < workspace_queued(tab); i++)
        cJSON_AddItemToArray(q, cJSON_CreateString(workspace_pending_at(tab, i)));
    /* The window's tabs, as the terminal's tab box shows them. */
    cJSON *tabs = cJSON_AddArrayToObject(o, "tabs");
    for (int i = 0; i < workspace_count(); i++) {
        const struct session *t = workspace_at(i);
        char at[256];
        const char *name = at;
        if (session_remote(t) || session_name(t)[0])
            session_address(t, at, sizeof at);
        else
            name = session_title(t) ? session_title(t) : "";
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", name);
        cJSON_AddStringToObject(e, "status", workspace_status(t));
        if (session_unseen(t))
            cJSON_AddBoolToObject(e, "unseen", 1);
        if (session_ask_open(t) && !session_busy(t)) {
            struct askblock *b = askblock_parse(session_last_block(t));
            if (b)
                cJSON_AddBoolToObject(e, "asking", 1);
            askblock_free(b);
        }
        int ctx = session_context_percent(t);
        if (ctx > 0)
            cJSON_AddNumberToObject(e, "context", ctx);
        if (t == s)
            cJSON_AddBoolToObject(e, "here", 1);
        cJSON_AddItemToArray(tabs, e);
    }
    return o;
}

static cJSON *ask_obj(void)
{
    if (!rt.ask)
        return cJSON_CreateNull();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)rt.ask_id);
    cJSON *qs = cJSON_AddArrayToObject(o, "questions");
    for (int i = 0; i < rt.ask->n; i++) {
        const struct askq *q = &rt.ask->q[i];
        cJSON *jq = cJSON_CreateObject();
        cJSON_AddStringToObject(jq, "text", q->text ? q->text : "");
        cJSON *opts = cJSON_AddArrayToObject(jq, "options");
        for (int k = 0; k < q->nopt; k++) {
            cJSON *opt = cJSON_CreateObject();
            cJSON_AddStringToObject(opt, "label", q->label[k] ? q->label[k] : "");
            if (q->detail[k] && *q->detail[k])
                cJSON_AddStringToObject(opt, "detail", q->detail[k]);
            cJSON_AddItemToArray(opts, opt);
        }
        cJSON_AddItemToArray(qs, jq);
    }
    return o;
}

static cJSON *entries_before(int end, int limit)
{
    cJSON *list = cJSON_CreateArray();
    int from = end - limit < 0 ? 0 : end - limit;
    for (int i = from; i < end; i++)
    {
        const cJSON *e = cJSON_GetArrayItem(rt.cur->entries, i);
        cJSON_AddItemToArray(list, entry_out(e, is_tool(e)));
    }
    return list;
}

static void send_view(int client)
{
    struct session *s = relay_session();
    cJSON *o = frame("view");
    cJSON_AddNumberToObject(o, "seq", (double)rt.seq);
    cJSON_AddNumberToObject(o, "binding", rt.binding);
    cJSON_AddItemToObject(o, "session", session_obj(s));
    int n = cJSON_GetArraySize(rt.cur->entries);
    cJSON_AddItemToObject(o, "entries", entries_before(n, WINDOW));
    cJSON_AddBoolToObject(o, "older", n > WINDOW);
    cJSON_AddItemToObject(o, "live", live_obj(s));
    cJSON_AddItemToObject(o, "ask", ask_obj());
    send_to(client, o);
}

static void send_view_all(void)
{
    rt.replay_base = rt.seq;
    for (int i = 0; i < WSD_MAX_CLIENTS; i++)
        if (rt.clients[i].id)
            send_view(rt.clients[i].id);
}

/* Publish `next` under `name` when it differs from what was last sent. */
static void publish(const char *name, char **last, cJSON *next)
{
    char *s = cJSON_PrintUnformatted(next);
    if (s && *last && !strcmp(s, *last)) {
        free(s);
        cJSON_Delete(next);
        return;
    }
    free(*last);
    *last = s;
    emit(op2(name, next));
}

static void ask_set(struct askblock *b)
{
    if (!b && !rt.ask)
        return;
    askblock_free(rt.ask);
    rt.ask = b;
    if (b)
        rt.ask_id++;
    emit(op2("ask", ask_obj()));
}

static void rebind(void)
{
    askblock_free(rt.ask);
    rt.ask = NULL;
    rt.binding++;
    free(rt.session_json);
    free(rt.live_json);
    rt.session_json = rt.live_json = NULL;
    send_view_all();
}

/* ---- session events ----------------------------------------------------- */

const char *relay_system_note(void)
{
    snprintf(rt.system_note, sizeof rt.system_note,
        "## This session is also on a phone\n\n"
        "The user is at a terminal, but the same session is served to a phone "
        "app, where replies show as chat messages. Markdown renders; wide "
        "tables and long code listings do not. Keep answers short and say the "
        "answer first.\n\n"
        "A markdown image with an absolute local path, ![alt](/abs/path.png), "
        "is shown on the phone; the file has to be on this machine. That only "
        "sends it to them; it does not show it to you.\n");
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

/* Highlighter roles as [[text, role], ...]; role is null where the default applies. */
static cJSON *runs_of(const char *text, size_t len, const unsigned char *roles)
{
    cJSON *runs = cJSON_CreateArray();
    for (size_t i = 0; i < len;) {
        size_t j = i;
        while (j < len && roles[j] == roles[i])
            j++;
        char *piece = strndup(text + i, j - i);
        const char *key = roles[i] == UI_RESET ? NULL
                        : ui_role_key((enum ui_role)roles[i], &(unsigned){0}, &(unsigned){0},
                                      &(const char *){NULL});
        cJSON *run = cJSON_CreateArray();
        cJSON_AddItemToArray(run, cJSON_CreateString(piece ? piece : ""));
        cJSON_AddItemToArray(run, key ? cJSON_CreateString(key) : cJSON_CreateNull());
        cJSON_AddItemToArray(runs, run);
        free(piece);
        i = j;
    }
    return runs;
}

static void tool_entry(struct log *l, const backend_event *ev)
{
    char label[48], arg[600];
    toolstyle_label(label, sizeof label, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? view_tool_arg_value(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    text_trunc(arg, sizeof arg, v ? v : "");
    cJSON_Delete(in);
    cJSON *e = entry_new("tool", NULL);
    cJSON_AddStringToObject(e, "name", label);
    cJSON_AddStringToObject(e, "arg", arg);
    size_t len = strlen(arg);
    unsigned char *roles = toolstyle_is_shell(label) && len ? malloc(len) : NULL;
    if (roles) {
        highlight_shell(arg, len, roles);
        cJSON_AddItemToObject(e, "runs", runs_of(arg, len, roles));
        free(roles);
    }
    if (ev->input_json) {
        int cut = 0;
        cJSON_AddItemToObject(e, "input", clipped_string(ev->input_json, RESULT_CLIP, &cut));
    }
    if (l->nopen == OPEN_TOOLS) {
        memmove(l->open_tools, l->open_tools + 1, (OPEN_TOOLS - 1) * sizeof l->open_tools[0]);
        l->nopen--;
    }
    l->open_tools[l->nopen++] = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "id"));
    entry_add(l, e);
}

/* Results carry no call id, so each one closes the oldest open tool call. */
static void tool_result(struct log *l, const backend_event *ev)
{
    if (!l->nopen)
        return;
    long id = l->open_tools[0];
    memmove(l->open_tools, l->open_tools + 1, (size_t)(--l->nopen) * sizeof l->open_tools[0]);
    cJSON *f = cJSON_CreateObject();
    int cut = 0;
    cJSON_AddBoolToObject(f, "done", 1);
    if (ev->text && *ev->text)
        cJSON_AddItemToObject(f, "result", clipped_string(ev->text, RESULT_CLIP, &cut));
    if (ev->diff && *ev->diff)
        cJSON_AddItemToObject(f, "diff", clipped_string(ev->diff, RESULT_CLIP, &cut));
    if (ev->failed)
        cJSON_AddBoolToObject(f, "failed", 1);
    entry_set(l, id, f);
}

static void on_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (!rt.active || (ev->parent && *ev->parent) || workspace_index_of(s) < 0)
        return;
    struct log *l = log_for(s);
    mirror_prompt(l);

    if (ev->kind == BACKEND_EV_TASK) {
        if (session_task_repeat(s))
            l->repeat_task = 1;
        return;
    }
    if (l->repeat_task) {
        if (ev->kind == BACKEND_EV_ASSISTANT)
            l->repeat_task = 0;
        return;
    }

    switch (ev->kind) {
    case BACKEND_EV_TOOL:
        tool_entry(l, ev);
        break;
    case BACKEND_EV_TOOL_RESULT:
        tool_result(l, ev);
        break;
    case BACKEND_EV_ASSISTANT:
        if (ev->text && *ev->text)
            entry_add(l, assistant_entry(ev->text));
        break;
    case BACKEND_EV_THINKING:
        if (session_thinking(s) && ev->text && *ev->text)
            entry_add(l, entry_new("thinking", ev->text));
        break;
    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text)
            entry_add(l, entry_new("note", ev->text));
        break;
    default:
        break;
    }
}

/* A started turn becomes a user entry. A prompt that contains a line the
 * phone sent is tagged with that request's id. */
static void mirror_prompt(struct log *l)
{
    struct session *s = l->s;
    if (!session_busy(s))
        return;
    double started = session_turn_started(s);
    if (started == l->mirrored_turn)
        return;
    l->mirrored_turn = started;
    if (l == rt.cur)
        ask_set(NULL);
    l->nopen = 0;
    const char *p = session_prompt(s);
    if (!p || !*p)
        return;
    cJSON *e = entry_new("user", p);
    for (int i = 0; l == rt.cur && i < rt.nsent; i++) {
        if (!strstr(p, rt.sent[i].text))
            continue;
        cJSON_AddStringToObject(e, "req", rt.sent[i].req);
        free(rt.sent[i].text);
        memmove(rt.sent + i, rt.sent + i + 1, (size_t)(rt.nsent - i - 1) * sizeof rt.sent[0]);
        rt.nsent--;
        break;
    }
    entry_add(l, e);
}

void relay_turn_done(struct session *s)
{
    struct log *l = rt.active && workspace_index_of(s) >= 0 ? log_for(s) : NULL;
    if (!l)
        return;
    cJSON *e = entry_new("end", NULL);
    double started = session_turn_started(s);
    cJSON_AddNumberToObject(e, "secs", started > 0 ? now_seconds() - started : 0);
    if (session_last_result(s)->is_error) {
        const char *why = session_last_error(s);
        cJSON_AddStringToObject(e, "text", why ? why : "the turn failed");
        cJSON_AddBoolToObject(e, "failed", 1);
    } else if (session_last_interrupted(s)) {
        cJSON_AddBoolToObject(e, "stopped", 1);
    }
    entry_add(l, e);
    if (l == rt.cur && !session_last_result(s)->is_error && !session_last_interrupted(s))
        ask_set(askblock_parse(session_last_block(s)));
    const struct transcript *t = session_transcript(s);
    l->tcount = t ? t->count : 0;
}

void relay_btw(const struct session *owner, const char *question,
               const char *answer, int failed)
{
    struct log *l = rt.active ? log_find(owner) : NULL;
    if (!l)
        return;
    cJSON *e = entry_new("btw", question);
    cJSON_AddStringToObject(e, "answer", answer ? answer : "");
    if (failed)
        cJSON_AddBoolToObject(e, "failed", 1);
    entry_add(l, e);
}

/* A cleared or replaced conversation in the tab starts a new binding. */
static void check_reset(struct log *l)
{
    const struct transcript *t = session_transcript(l->s);
    size_t count = t ? t->count : 0;
    const char *id = session_id(l->s) ? session_id(l->s) : "";
    int replaced = l->sid[0] && strcmp(l->sid, id);
    if (!l->sid[0] && id[0])
        snprintf(l->sid, sizeof l->sid, "%s", id);
    if (replaced || count < l->tcount) {
        entries_load(l, 1);
        if (l == rt.cur)
            rebind();
    } else {
        l->tcount = count;
    }
}

/* ---- requests ----------------------------------------------------------- */

static void remember_sent(const char *req, const char *text)
{
    if (rt.nsent == SENT_MAX) {
        free(rt.sent[0].text);
        memmove(rt.sent, rt.sent + 1, (SENT_MAX - 1) * sizeof rt.sent[0]);
        rt.nsent--;
    }
    snprintf(rt.sent[rt.nsent].req, sizeof rt.sent[0].req, "%s", req);
    rt.sent[rt.nsent].text = strdup(text);
    if (rt.sent[rt.nsent].text)
        rt.nsent++;
}

static const char *done_find(const char *key)
{
    for (int i = 0; i < DONE_MAX; i++)
        if (rt.done[i].res && !strcmp(rt.done[i].key, key))
            return rt.done[i].res;
    return NULL;
}

static void done_add(const char *key, const cJSON *res)
{
    struct done *d = &rt.done[rt.done_next];
    rt.done_next = (rt.done_next + 1) % DONE_MAX;
    free(d->res);
    snprintf(d->key, sizeof d->key, "%s", key);
    d->res = cJSON_PrintUnformatted(res);
}

struct submit_ctx {
    const char *req;
    const char *text;
    cJSON      *res;
};

/* A list the command would have shown; the client answers with `command` and a label. */
static cJSON *pick_obj(const struct pick_capture *p, const char *line)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", p->title);
    char command[64];
    snprintf(command, sizeof command, "%.*s", (int)strcspn(line, " \t\n"), line);
    cJSON_AddStringToObject(o, "command", command);
    cJSON_AddNumberToObject(o, "initial", p->initial);
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    for (int i = 0; i < p->count; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "label", p->items[i].label);
        if (p->items[i].detail && *p->items[i].detail)
            cJSON_AddStringToObject(it, "detail", p->items[i].detail);
        cJSON_AddItemToArray(items, it);
    }
    return o;
}

static void submit(struct session *s, void *ud)
{
    struct submit_ctx *c = ud;
    const char *line = c->text;
    int command = cmd_is_command(line);
    if (!cmd_self_echoes(line) && (command || !session_turn_running(s)))
        prompt_echo_message(line);

    frontend_push(0);
    if (!command) {
        rt.cur->repeat_task = 0;
        remember_sent(c->req, line);
        cmd_submit(s, line);
        frontend_pop();
        c->res = res_ok(c->req);
        return;
    }
    ui_sink_begin_tee();
    pick_capture_begin();
    enum cmd_result r = cmd_submit(s, line);
    struct pick_capture pick;
    int picked = pick_capture_end(&pick);
    char *raw = ui_sink_end();
    char *shown = ui_plain(raw, 1);
    free(raw);
    frontend_pop();
    if (r == CMD_QUIT) {
        c->res = res_error(c->req, "refused", "the terminal owns this session; /quit there");
    } else {
        c->res = res_ok(c->req);
        if (picked)
            cJSON_AddItemToObject(c->res, "pick", pick_obj(&pick, line));
        else if (shown && *shown)
            entry_add(rt.cur, entry_new("note", shown));
    }
    pick_capture_free(&pick);
    free(shown);
}

void relay_banner(struct session *s)
{
    if (!rt.active || !s || s != relay_session())
        return;
    cJSON *e = entry_new("hud", NULL);
    cJSON_AddItemToObject(e, "hud", hud_rows(s));
    entry_add(rt.cur, e);
}

static void blank(struct session *s, void *ud)
{
    (void)ud;
    hud_print(s);
}

static cJSON *run_line(const char *req, const char *text)
{
    struct session *s = relay_session();
    if (!text)
        return res_error(req, "invalid", "no text");
    if (!*text) {
        workspace_render(workspace_index_of(s), blank, NULL);
        relay_banner(s);
        return res_ok(req);
    }
    if (bash_is_command(text))
        return res_error(req, "shell", "shell lines run at the terminal only");
    struct submit_ctx c = {req, text, NULL};
    workspace_render(workspace_index_of(s), submit, &c);
    return c.res ? c.res : res_error(req, "failed", "not submitted");
}

static cJSON *do_answer(const char *req, const cJSON *msg)
{
    long id = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "ask"));
    if (!rt.ask || id != rt.ask_id)
        return res_error(req, "stale", "that question is no longer open");
    const cJSON *jc = cJSON_GetObjectItem(msg, "choice");
    const cJSON *jt = cJSON_GetObjectItem(msg, "text");
    int n = rt.ask->n;
    int *choice = calloc((size_t)n + 1, sizeof *choice);
    const char **text = calloc((size_t)n + 1, sizeof *text);
    cJSON *res = NULL;
    if (choice && text) {
        for (int i = 0; i < n; i++) {
            const cJSON *c = cJSON_GetArrayItem(jc, i);
            choice[i] = cJSON_IsNumber(c) ? (int)c->valuedouble : -1;
            text[i] = cJSON_GetStringValue(cJSON_GetArrayItem(jt, i));
        }
        char *line = askblock_answer(rt.ask, choice, text,
                                     cJSON_GetStringValue(cJSON_GetObjectItem(msg, "reply")));
        res = line && *line ? run_line(req, line) : res_error(req, "invalid", "no answer given");
        free(line);
    }
    free(choice);
    free(text);
    return res ? res : res_error(req, "failed", "out of memory");
}

static cJSON *do_older(const char *req, const cJSON *msg)
{
    long before = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "before"));
    int  limit = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "limit"));
    if (limit <= 0 || limit > OLDER_MAX)
        limit = WINDOW;
    int at = 0;
    if (!entry_find(rt.cur, before, &at))
        return res_error(req, "not_found", "no such entry");
    cJSON *o = res_ok(req);
    cJSON_AddItemToObject(o, "entries", entries_before(at, limit));
    cJSON_AddBoolToObject(o, "older", at > limit);
    return o;
}

static cJSON *do_entry(const char *req, const cJSON *msg)
{
    cJSON *e = entry_find(rt.cur, (long)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "id")), NULL);
    if (!e)
        return res_error(req, "not_found", "no such entry");
    cJSON *o = res_ok(req);
    cJSON_AddItemToObject(o, "entry", cJSON_Duplicate(e, 1));
    return o;
}

static cJSON *do_unqueue(const char *req, const cJSON *msg)
{
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
    if (!text || !workspace_dequeue(workspace_index_of(relay_session()), text))
        return res_error(req, "not_found", "not in the queue");
    return res_ok(req);
}

static cJSON *do_sessions(const char *req)
{
    cJSON *rows = sessionswitch_rows(), *r;
    cJSON_ArrayForEach(r, rows)
    {
        const cJSON *tab = cJSON_GetObjectItem(r, "tab");
        if (!cJSON_IsNumber(tab))
            continue;
        int here = workspace_at((int)tab->valuedouble) == relay_session();
        if (here)
            cJSON_AddBoolToObject(r, "relay", 1);
        /* The phone's current session is the relay's, not the terminal's visible tab. */
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(r, "label"));
        const char *title = label ? strchr(label, ' ') : NULL;
        if (title) {
            char marked[512];
            snprintf(marked, sizeof marked, "%s%s", here ? "\xe2\x96\xb8" : "\xe2\xa7\x89", title);
            cJSON_ReplaceItemInObject(r, "label", cJSON_CreateString(marked));
        }
    }
    cJSON *o = res_ok(req);
    cJSON_AddItemToObject(o, "rows", rows);
    return o;
}

/* Close a tab of this window. Closing the served tab moves the relay to a neighbour first,
 * since a closed relay session stops the relay. */
static cJSON *do_close(const char *req, const cJSON *msg)
{
    const cJSON *tab = cJSON_GetObjectItem(msg, "tab");
    int at = cJSON_IsNumber(tab) ? (int)tab->valuedouble : -1;
    if (at < 0 || at >= workspace_count())
        return res_error(req, "not_found", "no such tab");
    if (workspace_count() == 1)
        return res_error(req, "refused", "that is the only tab; /quit at the terminal");
    struct session *s = workspace_at(at);
    if (s == relay_session())
        relay_start(workspace_at(at ? at - 1 : 1));
    if (session_turn_running(s))
        session_interrupt(s);
    workspace_close(at);
    cJSON *o = res_ok(req);
    cJSON_AddNumberToObject(o, "binding", rt.binding);
    return o;
}

/* Bring a /sessions row into this window, as the picker does, and serve it. */
static cJSON *do_open(const char *req, const cJSON *msg)
{
    const char *target = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "target"));
    const cJSON *tab = cJSON_GetObjectItem(msg, "tab");
    char why[256] = "";
    int  at = cJSON_IsNumber(tab) ? (int)tab->valuedouble
            : target && *target ? cmd_attach_tab(target, why, sizeof why) : -1;
    struct session *s = at >= 0 && at < workspace_count() ? workspace_at(at) : NULL;
    if (!s)
        return res_error(req, "not_found", why[0] ? why : "no such session");
    relay_start(s);
    cJSON *o = res_ok(req);
    cJSON_AddNumberToObject(o, "binding", rt.binding);
    return o;
}

/* A new tab like the relay's session, served in its place. */
static cJSON *do_new(const char *req)
{
    struct session *cur = relay_session();
    if (!cur)
        return res_error(req, "not_found", "no session");
    int at = workspace_spawn(session_backend(cur), session_model_label(cur), session_effort(cur),
                             session_cwd(cur), NULL);
    if (at < 0)
        return res_error(req, "failed", "could not start a session");
    relay_start(workspace_at(at));
    cJSON *o = res_ok(req);
    cJSON_AddNumberToObject(o, "binding", rt.binding);
    return o;
}

/* Code block highlighting as md.c does it; no runs for an unknown language. */
static cJSON *do_highlight(const char *req, const cJSON *msg)
{
    const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "lang"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
    cJSON *o = res_ok(req);
    size_t len = text ? strlen(text) : 0;
    unsigned char *roles = lang && len ? malloc(len) : NULL;
    if (roles && highlight_code(lang, text, len, roles))
        cJSON_AddItemToObject(o, "runs", runs_of(text, len, roles));
    free(roles);
    return o;
}

static void hex_add(cJSON *o, const char *key, unsigned c)
{
    char v[8];
    snprintf(v, sizeof v, "#%06x", c & 0xffffff);
    cJSON_AddStringToObject(o, key, v);
}

/* The resolved theme: each role's colour, background wash and attribute. */
static cJSON *theme_obj(void)
{
    cJSON *o = cJSON_CreateObject();
    hex_add(o, "background", ui_background());
    cJSON *roles = cJSON_AddObjectToObject(o, "roles");
    for (int r = 0; r < UI_RESET; r++) {
        unsigned    fg, wash;
        const char *attr, *key = ui_role_key((enum ui_role)r, &fg, &wash, &attr);
        if (!key)
            continue;
        cJSON *jr = cJSON_CreateObject();
        hex_add(jr, "fg", fg);
        if (wash != ui_background())
            hex_add(jr, "wash", wash);
        if (attr)
            cJSON_AddStringToObject(jr, "style", !strcmp(attr, "1") ? "bold"
                                               : !strcmp(attr, "3") ? "italic"
                                               : !strcmp(attr, "4") ? "underline" : attr);
        cJSON_AddItemToObject(roles, key, jr);
    }
    return o;
}

/* A client reconnecting to the same relay gets the deltas it missed
 * instead of a full view, while they are all still in the replay ring. */
static int resume(int client, const cJSON *r)
{
    const char *server = cJSON_GetStringValue(cJSON_GetObjectItem(r, "server"));
    const cJSON *jseq = cJSON_GetObjectItem(r, "seq");
    const cJSON *jbind = cJSON_GetObjectItem(r, "binding");
    if (!server || strcmp(server, rt.server) || !cJSON_IsNumber(jseq) || !cJSON_IsNumber(jbind) ||
        (int)jbind->valuedouble != rt.binding)
        return 0;
    long seq = (long)jseq->valuedouble;
    if (seq < rt.replay_base || seq > rt.seq || rt.seq - seq > REPLAY_MAX)
        return 0;
    cJSON *o = frame("resumed");
    cJSON_AddNumberToObject(o, "seq", (double)seq);
    send_to(client, o);
    for (long n = seq + 1; n <= rt.seq; n++)
        wsd_send(rt.ws, client, rt.replay[n % REPLAY_MAX], 0);
    return 1;
}

static void hello(int client, const cJSON *msg)
{
    if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "v")) != PROTOCOL) {
        cJSON *o = frame("error");
        cJSON_AddStringToObject(o, "code", "version");
        cJSON_AddNumberToObject(o, "v", PROTOCOL);
        send_to(client, o);
        return;
    }
    struct client *c = client_find(client);
    if (!c)
        c = client_find(0);
    if (!c)
        return;
    c->id = client;
    const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "client"));
    snprintf(c->uuid, sizeof c->uuid, "%s", uuid ? uuid : "");
    cJSON *o = frame("hello");
    cJSON_AddNumberToObject(o, "v", PROTOCOL);
    cJSON_AddStringToObject(o, "server", rt.server);
    cJSON_AddStringToObject(o, "name", APP_NAME);
    cJSON_AddItemToObject(o, "theme", theme_obj());
    send_to(client, o);
    if (!resume(client, cJSON_GetObjectItem(msg, "resume")))
        send_view(client);
}

static void run_request(int client, const cJSON *msg)
{
    const char *req = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
    const char *op = op_of(msg);
    struct client *c = client_find(client);
    if (!c) {
        send_to(client, res_error(req, "hello", "send hello first"));
        return;
    }
    const cJSON *jb = cJSON_GetObjectItem(msg, "binding");
    int mutates = needs_idle(msg) || !strcmp(op, "unqueue");
    if (mutates && cJSON_IsNumber(jb) && (int)jb->valuedouble != rt.binding) {
        send_to(client, res_error(req, "stale", "the relay moved to another session"));
        return;
    }
    char key[160];
    snprintf(key, sizeof key, "%s\x1f%s", c->uuid, req);
    if (mutates && c->uuid[0]) {
        const char *prior = done_find(key);
        if (prior) {
            wsd_send(rt.ws, client, prior, 0);
            return;
        }
    }

    cJSON *res;
    if (!strcmp(op, "send"))
        res = run_line(req, cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text")));
    else if (!strcmp(op, "answer"))
        res = do_answer(req, msg);
    else if (!strcmp(op, "stop")) {
        session_interrupt(relay_session());
        res = res_ok(req);
    } else if (!strcmp(op, "unqueue"))
        res = do_unqueue(req, msg);
    else if (!strcmp(op, "older"))
        res = do_older(req, msg);
    else if (!strcmp(op, "entry"))
        res = do_entry(req, msg);
    else if (!strcmp(op, "sessions"))
        res = do_sessions(req);
    else if (!strcmp(op, "open"))
        res = do_open(req, msg);
    else if (!strcmp(op, "new"))
        res = do_new(req);
    else if (!strcmp(op, "close"))
        res = do_close(req, msg);
    else if (!strcmp(op, "highlight"))
        res = do_highlight(req, msg);
    else
        res = res_error(req, "invalid", "unknown op");

    cJSON *failed = cJSON_GetObjectItem(msg, "failed");
    if (cJSON_GetArraySize(failed))
        cJSON_AddItemToObject(res, "failed", cJSON_Duplicate(failed, 1));
    if (mutates && c->uuid[0])
        done_add(key, res);
    send_to(client, res);
}

static void run_item(const struct item *it)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it->msg, "t"));
    if (!t)
        return;
    if (!strcmp(t, "hello"))
        hello(it->client, it->msg);
    else if (!strcmp(t, "gone")) {
        struct client *c = client_find(it->client);
        if (c)
            *c = (struct client){0};
    } else if (!strcmp(t, "req"))
        run_request(it->client, it->msg);
}

void relay_poll(struct session *live)
{
    if (!rt.active)
        return;
    if (!rt.draining) {
        rt.draining = 1;
        wake_drain();
        struct item it;
        while (inbox_take(&it, !live && !chrome_modal_active())) {
            run_item(&it);
            cJSON_Delete(it.msg);
        }
        rt.draining = 0;
    }
    /* Requests can close or move the served session, so read it after them. */
    struct session *s = relay_session();
    if (!rt.active || !s)
        return;
    for (int i = 0; i < workspace_count(); i++) {
        struct log *l = log_for(workspace_at(i));
        if (l && l->s != live)
            check_reset(l);
        if (l)
            mirror_prompt(l);
    }
    static double hud_at;
    double now = now_seconds();
    if (!rt.session_json || now - hud_at >= 1) {
        hud_at = now;
        publish("session", &rt.session_json, session_obj(s));
    }
    publish("live", &rt.live_json, live_obj(s));
}

/* ---- wsd thread --------------------------------------------------------- */

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

/* Save a send's files and put their paths in front of its text. Files that
 * could not be saved are listed in `failed`. */
static void take_uploads(cJSON *msg)
{
    cJSON *files = cJSON_DetachItemFromObject(msg, "files");
    if (!cJSON_GetArraySize(files)) {
        cJSON_Delete(files);
        return;
    }
    char   paths[UPLOAD_MAX][200];
    int    n = 0;
    cJSON *failed = cJSON_CreateArray();
    const cJSON *f;
    cJSON_ArrayForEach(f, files)
    {
        if (n < UPLOAD_MAX && save_upload(f, paths[n], sizeof paths[n]))
            n++;
        else {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
            cJSON_AddItemToArray(failed, cJSON_CreateString(name ? name : "file"));
        }
    }
    cJSON_Delete(files);
    if (cJSON_GetArraySize(failed))
        cJSON_AddItemToObject(msg, "failed", failed);
    else
        cJSON_Delete(failed);
    if (!n)
        return;
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
    size_t cap = (text ? strlen(text) : 0) + 64 + n * sizeof paths[0];
    char  *out = malloc(cap);
    if (!out)
        return;
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        at += (size_t)snprintf(out + at, cap - at, "- %s\n", paths[i]);
    if (text && *text)
        snprintf(out + at, cap - at, "\n%s", text);
    cJSON_DeleteItemFromObject(msg, "text");
    cJSON_AddStringToObject(msg, "text", out);
    free(out);
}

static void send_file(int client, const char *req, const char *path)
{
    struct stat st;
    FILE *f = NULL;
    unsigned char *bytes = NULL;
    char *b64 = NULL;
    cJSON *o;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        o = res_error(req, "not_found", "no such file");
    else if ((size_t)st.st_size > FILE_MAX)
        o = res_error(req, "too_large", "file over 32 MB");
    else if (!(f = fopen(path, "rb")) ||
             !(bytes = malloc((size_t)st.st_size + 1)) ||
             fread(bytes, 1, (size_t)st.st_size, f) != (size_t)st.st_size ||
             !(b64 = malloc(((size_t)st.st_size + 2) / 3 * 4 + 1)))
        o = res_error(req, "unreadable", "could not read the file");
    else {
        wsd_b64(bytes, (size_t)st.st_size, b64);
        o = res_ok(req);
        const char *base = strrchr(path, '/');
        cJSON_AddStringToObject(o, "name", base ? base + 1 : path);
        cJSON_AddNumberToObject(o, "size", (double)st.st_size);
        cJSON_AddStringToObject(o, "data", b64);
    }
    if (f)
        fclose(f);
    free(bytes);
    free(b64);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s)
        wsd_send(rt.ws, client, s, 0);
    free(s);
}

static void reply_error(int client, const char *req, const char *code, const char *msg)
{
    cJSON *o = res_error(req, code, msg);
    char  *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s)
        wsd_send(rt.ws, client, s, 0);
    free(s);
}

static void on_text(void *ud, int client, const char *text, size_t n)
{
    (void)ud;
    (void)n;
    cJSON *msg = cJSON_Parse(text);
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "t"));
    const char *req = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
    if (!t || (strcmp(t, "hello") && strcmp(t, "req"))) {
        reply_error(client, req, "invalid", "unknown frame");
        cJSON_Delete(msg);
        return;
    }
    if (!strcmp(t, "req") && !req) {
        reply_error(client, NULL, "invalid", "request without id");
        cJSON_Delete(msg);
        return;
    }
    if (!strcmp(op_of(msg), "file")) {
        send_file(client, req, cJSON_GetStringValue(cJSON_GetObjectItem(msg, "path")));
        cJSON_Delete(msg);
        return;
    }
    if (!strcmp(op_of(msg), "send"))
        take_uploads(msg);
    if (!inbox_push(client, msg)) {
        reply_error(client, req, "full", "too many requests waiting");
        cJSON_Delete(msg);
    }
}

static void on_state(void *ud, int client, int connected)
{
    (void)ud;
    if (connected)
        return;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "gone");
    if (!inbox_push(client, msg))
        cJSON_Delete(msg);
}

/* ---- lifecycle ---------------------------------------------------------- */

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

static void uploads_remove(void)
{
    if (!rt.upload_dir[0])
        return;
    DIR *d = opendir(rt.upload_dir);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        char path[400];
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", rt.upload_dir, e->d_name);
        unlink(path);
    }
    if (d)
        closedir(d);
    rmdir(rt.upload_dir);
    rt.upload_dir[0] = '\0';
}

static void set_note(struct session *s, int on)
{
    if (!s)
        return;
    if (on)
        session_set_system_extra(s, relay_system_note());
    else
        session_set_system_extra(s, tg_label() && tg_session() == s ? tg_system_note() : NULL);
}

static void cleanup(void)
{
    rt.active = 0;
    wsd_stop(rt.ws);
    rt.ws = NULL;
    uploads_remove();
    session_remove_listener(on_event, NULL);
    if (rt.wake[0] >= 0)
        close(rt.wake[0]);
    if (rt.wake[1] >= 0)
        close(rt.wake[1]);
    rt.wake[0] = rt.wake[1] = -1;
    inbox_clear();
    set_note(rt.s, 0);
    rt.s = NULL;
    rt.label[0] = '\0';
    memset(rt.clients, 0, sizeof rt.clients);
    for (int i = 0; i < LOGS_MAX; i++)
        log_free(&rt.logs[i]);
    rt.cur = NULL;
    replay_clear();
    askblock_free(rt.ask);
    rt.ask = NULL;
    free(rt.session_json);
    free(rt.live_json);
    rt.session_json = rt.live_json = NULL;
    for (int i = 0; i < rt.nsent; i++)
        free(rt.sent[i].text);
    rt.nsent = 0;
    for (int i = 0; i < DONE_MAX; i++) {
        free(rt.done[i].res);
        rt.done[i] = (struct done){0};
    }
}

int relay_start(struct session *s)
{
    if (rt.active) {
        if (s != rt.s) {
            set_note(rt.s, 0);
            rt.s = s;
            set_note(s, 1);
            rt.cur = log_for(s);
            rebind();
        }
        return 1;
    }
    char cfgpath[4200];
    if (path_config_file(cfgpath, sizeof cfgpath, "relay"))
        settings_load(&cfg, cfgpath);
    else
        settings_load(&cfg, "");

    const char *token = cfg_get("token", NULL);
    if (!token || strlen(token) < 16) {
        fprintf(stderr, APP_NAME ": relay needs `token` (16+ chars) in %s\n", cfgpath);
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
            fprintf(stderr, APP_NAME ": relay: no tailscale address; set `bind` in the config\n");
            return 0;
        }
    }
    snprintf(rt.bind, sizeof rt.bind, "%s", !strcmp(bind, "*") ? "" : bind);
    snprintf(rt.label, sizeof rt.label, "relay");
    snprintf(rt.server, sizeof rt.server, "%lx-%lx", (long)getpid(), (long)time(NULL));

    if (pipe(rt.wake) != 0) {
        fprintf(stderr, APP_NAME ": relay wake pipe: %s\n", strerror(errno));
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(rt.wake[i], F_SETFL, O_NONBLOCK);
        fcntl(rt.wake[i], F_SETFD, FD_CLOEXEC);
    }

    rt.s = s;
    rt.seq = 0;
    rt.replay_base = 0;
    rt.cur = log_for(s);
    session_add_listener(on_event, NULL);

    snprintf(rt.upload_dir, sizeof rt.upload_dir, "/tmp/" APP_NAME "_relay_XXXXXX");
    if (!mkdtemp(rt.upload_dir))
        rt.upload_dir[0] = '\0';
    wsd_opts o = {.bind_ip = rt.bind, .port = rt.port, .token = rt.token,
                  .max_message = MESSAGE_MAX, .max_queue = QUEUE_MAX};
    rt.ws = wsd_start(&o, on_text, on_state, NULL);
    if (!rt.ws) {
        fprintf(stderr, APP_NAME ": relay: can't listen on %s:%d\n",
                rt.bind[0] ? rt.bind : "*", rt.port);
        goto fail;
    }
    set_note(s, 1);
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
    struct log *l = log_find(s);
    if (l)
        log_free(l);
}

/* After a restart, serve the session the relay served once its tab is back. */
static char resume_id[128];

void relay_resume(const char *id)
{
    snprintf(resume_id, sizeof resume_id, "%s", id ? id : "");
}

void relay_resume_poll(void)
{
    if (!resume_id[0])
        return;
    int at = workspace_find_id(resume_id);
    if (at >= 0) {
        resume_id[0] = '\0';
        if (relay_start(workspace_at(at)))
            workspace_republish();
    } else if (!tabs_pending()) {
        resume_id[0] = '\0';
    }
}
