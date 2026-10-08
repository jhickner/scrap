#include "stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cmd.h"
#include "dispatch.h"
#include "session.h"
#include "sidechannel.h"
#include "transcript.h"
#include "workspace.h"

#define SUB_MAX     16
#define COMMAND_MAX 65536

static struct sub {
    int             fd;
    struct session *s;
    char           *buf;
    size_t          len;
    int             begin;
} subs[SUB_MAX] = {[0 ... SUB_MAX - 1] = {.fd = -1}};

static const char *const KINDS[] = {
    [BACKEND_EV_ASSISTANT] = "assistant", [BACKEND_EV_THINKING] = "thinking",
    [BACKEND_EV_TOOL] = "tool",           [BACKEND_EV_TOOL_RESULT] = "tool_result",
    [BACKEND_EV_INIT] = "init",           [BACKEND_EV_CWD] = "cwd",
    [BACKEND_EV_TRUST] = "trust",         [BACKEND_EV_WARNING] = "warning",
    [BACKEND_EV_TASK] = "task",           [BACKEND_EV_USER] = "user",
};

const char *stream_kind_name(int kind)
{
    return kind >= 0 && kind < (int)(sizeof KINDS / sizeof *KINDS) && KINDS[kind] ? KINDS[kind]
                                                                                 : "";
}

int stream_kind_of(const char *name)
{
    for (int i = 0; name && i < (int)(sizeof KINDS / sizeof *KINDS); i++)
        if (KINDS[i] && !strcmp(KINDS[i], name))
            return i;
    return -1;
}

static void drop(struct sub *u)
{
    if (u->fd >= 0)
        close(u->fd);
    free(u->buf);
    *u = (struct sub){.fd = -1};
}

static void put(struct sub *u, cJSON *o)
{
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) {
        drop(u);
        return;
    }
    size_t len = strlen(json);
    json[len] = '\n';
    for (size_t off = 0; off < len + 1;) {
        ssize_t n = send(u->fd, json + off, len + 1 - off, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(json);
            drop(u);
            return;
        }
        off += (size_t)n;
    }
    free(json);
}

static int alive(const struct sub *u)
{
    return u->fd >= 0 && workspace_index_of(u->s) >= 0;
}

static void add_str(cJSON *o, const char *key, const char *value)
{
    if (value)
        cJSON_AddStringToObject(o, key, value);
}

static cJSON *history(struct session *s)
{
    cJSON                   *h = cJSON_CreateObject();
    const struct transcript *t = session_transcript(s);
    cJSON                   *turns = cJSON_AddArrayToObject(h, "turns");
    for (size_t i = 0; t && i < t->count; i++) {
        cJSON *turn = cJSON_CreateObject();
        add_str(turn, "user", t->turns[i].user);
        add_str(turn, "assistant", t->turns[i].assistant);
        if (t->turns[i].interrupted)
            cJSON_AddBoolToObject(turn, "interrupted", 1);
        cJSON_AddItemToArray(turns, turn);
    }
    add_str(h, "id", session_id(s));
    add_str(h, "name", session_name(s));
    add_str(h, "title", session_title(s));
    add_str(h, "backend", session_backend(s));
    add_str(h, "model", session_model_label(s));
    add_str(h, "cwd", session_cwd(s));
    add_str(h, "status", workspace_status(s));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "history", h);
    if (session_turn_running(s)) {
        cJSON_AddStringToObject(o, "running", session_prompt(s) ? session_prompt(s) : "");
    }
    return o;
}

static struct session *find(const cJSON *o)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, "attach"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, "name"));
    int         at = id && *id ? workspace_find_id(id) : -1;
    for (int i = 0; at < 0 && name && *name && i < workspace_count(); i++)
        if (!strcmp(session_name(workspace_at(i)), name))
            at = i;
    return workspace_at(at);
}

int stream_serve(const cJSON *o, int fd)
{
    if (!cJSON_GetObjectItem((cJSON *)o, "attach"))
        return 0;
    struct sub      *u = NULL;
    struct session  *s = find(o);
    for (int i = 0; i < SUB_MAX && !u; i++)
        if (subs[i].fd < 0)
            u = &subs[i];
    struct sub reject = {.fd = fd};
    if (!s || !u) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", s ? "too many attached clients" : "no such session");
        put(&reject, e);
        drop(&reject);
        return 1;
    }
    int on = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    (void)on;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    *u = (struct sub){.fd = fd, .s = s};
    put(u, history(s));
    return 1;
}

int stream_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < SUB_MAX && n < max; i++)
        if (subs[i].fd >= 0)
            out[n++] = subs[i].fd;
    return n;
}

static void clear(struct session *s, void *ud)
{
    (void)ud;
    cmd_dispatch(s, "/clear");
}

static void command(struct sub *u, const char *line)
{
    cJSON      *o = cJSON_Parse(line);
    const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem(o, "prompt"));
    const char *btw = cJSON_GetStringValue(cJSON_GetObjectItem(o, "btw"));
    const char *steer = cJSON_GetStringValue(cJSON_GetObjectItem(o, "steer"));
    if (prompt && *prompt)
        dispatch_send(workspace_index_of(u->s), prompt);
    else if (steer && *steer) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
        const char *shown = cJSON_GetStringValue(cJSON_GetObjectItem(o, "shown"));
        int         ok = session_steer_id(u->s, steer, shown, id);
        if (id && *id) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, ok ? "steer_ok" : "steer_refused", id);
            put(u, r);
        } else if (!ok)
            dispatch_send(workspace_index_of(u->s), steer);
    }
    else if (btw && *btw) {
        char label[4096];
        snprintf(label, sizeof label, "/btw %s", btw);
        sidechannel_start(u->s, btw, label);
    }
    else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "interrupt")))
        session_interrupt(u->s);
    else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "clear")) && !session_turn_running(u->s))
        workspace_render(workspace_index_of(u->s), clear, NULL);
    cJSON_Delete(o);
}

static void take(struct sub *u)
{
    char chunk[4096];
    for (;;) {
        ssize_t n = recv(u->fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0 || u->len + (size_t)n > COMMAND_MAX) {
            drop(u);
            return;
        }
        char *grown = realloc(u->buf, u->len + (size_t)n + 1);
        if (!grown) {
            drop(u);
            return;
        }
        u->buf = grown;
        memcpy(u->buf + u->len, chunk, (size_t)n);
        u->len += (size_t)n;
        u->buf[u->len] = '\0';
        char *nl;
        while (u->fd >= 0 && (nl = strchr(u->buf, '\n'))) {
            *nl = '\0';
            command(u, u->buf);
            if (u->fd < 0)
                return;
            u->len -= (size_t)(nl + 1 - u->buf);
            memmove(u->buf, nl + 1, u->len + 1);
        }
    }
}

static void begun(struct sub *u)
{
    if (!u->begin || u->fd < 0)
        return;
    u->begin = 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "turn", "begin");
    add_str(o, "prompt", session_prompt(u->s));
    put(u, o);
}

static void each(const struct session *s, cJSON *o)
{
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].fd >= 0 && subs[i].s == s) {
            begun(&subs[i]);
            put(&subs[i], cJSON_Duplicate(o, 1));
        }
    cJSON_Delete(o);
}

void stream_poll(void)
{
    for (int i = 0; i < SUB_MAX; i++) {
        if (subs[i].fd < 0)
            continue;
        if (!alive(&subs[i])) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "closed", 1);
            put(&subs[i], o);
            drop(&subs[i]);
            continue;
        }
        begun(&subs[i]);
        take(&subs[i]);
    }
}

void stream_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "kind", stream_kind_name(ev->kind));
    add_str(e, "text", ev->text);
    add_str(e, "name", ev->name);
    add_str(e, "input_json", ev->input_json);
    add_str(e, "arg", ev->arg);
    add_str(e, "diff", ev->diff);
    add_str(e, "id", ev->id);
    add_str(e, "parent", ev->parent);
    add_str(e, "task_type", ev->task_type);
    if (ev->failed)
        cJSON_AddBoolToObject(e, "failed", 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "ev", e);
    each(s, o);
}

void stream_turn_begin(struct session *s)
{
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].fd >= 0 && subs[i].s == s)
            subs[i].begin = 1;
}

void stream_turn_done(struct session *s)
{
    const backend_result *r = session_last_result(s);
    cJSON                *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "turn", "done");
    add_str(o, "reply", session_last_reply(s));
    cJSON *m = cJSON_AddObjectToObject(o, "result");
    cJSON_AddNumberToObject(m, "cost_usd", r->cost_usd);
    cJSON_AddNumberToObject(m, "input_tokens", (double)r->input_tokens);
    cJSON_AddNumberToObject(m, "output_tokens", (double)r->output_tokens);
    cJSON_AddNumberToObject(m, "cache_read_tokens", (double)r->cache_read_tokens);
    cJSON_AddNumberToObject(m, "cache_creation_tokens", (double)r->cache_creation_tokens);
    cJSON_AddNumberToObject(m, "context_tokens", (double)r->context_tokens);
    cJSON_AddNumberToObject(m, "context_window", (double)r->context_window);
    if (r->is_error)
        cJSON_AddBoolToObject(m, "is_error", 1);
    if (r->interrupted)
        cJSON_AddBoolToObject(m, "interrupted", 1);
    add_str(m, "subtype", r->subtype);
    add_str(o, "status", workspace_status(s));
    each(s, o);
}

int stream_watched(const struct session *s)
{
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].fd >= 0 && subs[i].s == s)
            return 1;
    return 0;
}

void stream_side(const struct session *s, const char *question, const char *answer,
                 int failed)
{
    cJSON *n = cJSON_CreateObject();
    add_str(n, "question", question);
    add_str(n, "answer", answer);
    if (failed)
        cJSON_AddBoolToObject(n, "failed", 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "side", n);
    each(s, o);
}
