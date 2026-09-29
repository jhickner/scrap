#include "remote.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "intercom.h"
#include "stream.h"

#define LINE_MAX_BYTES (8 * 1024 * 1024)
#define HISTORY_WAIT_MS 10000
#define TICK_MS         20

struct remote {
    char  *target;
    int    fd;
    char  *buf;
    size_t len;
    int    closed;
    cJSON *history;
    cJSON *side;
    pthread_mutex_t side_lock;
    char  *model;
    char  *pending_prompt;
    int    pending;
    char   err[512];
    void (*on_event)(void *ud, const backend_event *ev);
    void *event_ud;
    int (*abort)(void);
};

static void side_keep(struct remote *r, cJSON *o)
{
    cJSON *n = cJSON_DetachItemFromObject(o, "side");
    if (!n)
        return;
    pthread_mutex_lock(&r->side_lock);
    cJSON_AddItemToArray(r->side, n);
    pthread_mutex_unlock(&r->side_lock);
}

static struct remote *R(Backend *b)
{
    return b->ctx;
}

static void replace(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static char *take_line(struct remote *r)
{
    char *nl = r->buf ? memchr(r->buf, '\n', r->len) : NULL;
    if (!nl)
        return NULL;
    size_t n = (size_t)(nl - r->buf);
    char  *line = malloc(n + 1);
    if (!line)
        return NULL;
    memcpy(line, r->buf, n);
    line[n] = '\0';
    r->len -= n + 1;
    memmove(r->buf, nl + 1, r->len);
    return line;
}

static char *next_line(struct remote *r, int wait_ms)
{
    char *line = take_line(r);
    if (line || r->closed || r->fd < 0)
        return line;
    struct pollfd p = {.fd = r->fd, .events = POLLIN};
    if (poll(&p, 1, wait_ms) <= 0)
        return NULL;
    char    chunk[65536];
    ssize_t n = recv(r->fd, chunk, sizeof chunk, 0);
    if (n <= 0 || r->len + (size_t)n > LINE_MAX_BYTES) {
        r->closed = 1;
        snprintf(r->err, sizeof r->err, "the connection to %s closed", r->target);
        return NULL;
    }
    char *grown = realloc(r->buf, r->len + (size_t)n);
    if (!grown) {
        r->closed = 1;
        return NULL;
    }
    r->buf = grown;
    memcpy(r->buf + r->len, chunk, (size_t)n);
    r->len += (size_t)n;
    return take_line(r);
}

static int put(struct remote *r, cJSON *o)
{
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json || r->fd < 0) {
        free(json);
        return 0;
    }
    size_t len = strlen(json);
    json[len] = '\n';
    int ok = send(r->fd, json, len + 1, 0) == (ssize_t)(len + 1);
    free(json);
    return ok;
}

static const char *jstr(const cJSON *o, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
}

static void forward(struct remote *r, const cJSON *e)
{
    int kind = stream_kind_of(jstr(e, "kind"));
    if (kind < 0 || !r->on_event)
        return;
    backend_event ev = {
        .kind = (backend_event_kind)kind,
        .text = jstr(e, "text"),
        .name = jstr(e, "name"),
        .input_json = jstr(e, "input_json"),
        .arg = jstr(e, "arg"),
        .diff = jstr(e, "diff"),
        .id = jstr(e, "id"),
        .parent = jstr(e, "parent"),
        .task_type = jstr(e, "task_type"),
        .failed = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)e, "failed")),
    };
    r->on_event(r->event_ud, &ev);
}

static double num(const cJSON *o, const char *key)
{
    return cJSON_GetNumberValue(cJSON_GetObjectItem((cJSON *)o, key));
}

static void fill(backend_result *m, const cJSON *res)
{
    if (!m || !res)
        return;
    m->cost_usd = num(res, "cost_usd");
    m->input_tokens = (long)num(res, "input_tokens");
    m->output_tokens = (long)num(res, "output_tokens");
    m->cache_read_tokens = (long)num(res, "cache_read_tokens");
    m->cache_creation_tokens = (long)num(res, "cache_creation_tokens");
    m->context_tokens = (long)num(res, "context_tokens");
    m->context_window = (long)num(res, "context_window");
    m->is_error = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)res, "is_error"));
    m->interrupted = cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)res, "interrupted"));
    snprintf(m->subtype, sizeof m->subtype, "%s", jstr(res, "subtype") ? jstr(res, "subtype") : "");
}

static char *turn(struct remote *r, const char *mine, backend_result *meta)
{
    int ours = mine == NULL, asked_stop = 0;
    for (;;) {
        if (!asked_stop && r->abort && r->abort()) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "interrupt", 1);
            put(r, o);
            asked_stop = 1;
        }
        char *line = next_line(r, TICK_MS);
        if (!line) {
            if (r->closed)
                return NULL;
            continue;
        }
        cJSON *o = cJSON_Parse(line);
        free(line);
        const char *t = jstr(o, "turn");
        cJSON      *e = cJSON_GetObjectItem(o, "ev");
        if (e)
            forward(r, e);
        else if (cJSON_GetObjectItem(o, "side"))
            side_keep(r, o);
        else if (t && !strcmp(t, "begin") && !ours && jstr(o, "prompt") &&
                 !strcmp(jstr(o, "prompt"), mine))
            ours = 1;
        else if (t && !strcmp(t, "done") && ours) {
            fill(meta, cJSON_GetObjectItem(o, "result"));
            char *reply = strdup(jstr(o, "reply") ? jstr(o, "reply") : "");
            cJSON_Delete(o);
            return reply;
        } else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "closed"))) {
            r->closed = 1;
            snprintf(r->err, sizeof r->err, "%s closed", r->target);
            cJSON_Delete(o);
            return NULL;
        }
        cJSON_Delete(o);
    }
}

static int connect_remote(struct remote *r);

static char *ask_ex(Backend *b, const char *user, backend_result *meta)
{
    struct remote *r = R(b);
    if (meta)
        memset(meta, 0, sizeof *meta);
    if (r->closed && !connect_remote(r))
        return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "prompt", user ? user : "");
    if (!put(r, o)) {
        r->closed = 1;
        snprintf(r->err, sizeof r->err, "could not reach %s", r->target);
        return NULL;
    }
    return turn(r, user ? user : "", meta);
}

static char *ask(Backend *b, const char *user)
{
    return ask_ex(b, user, NULL);
}

static char *continue_ex(Backend *b, backend_result *meta)
{
    if (meta)
        memset(meta, 0, sizeof *meta);
    return turn(R(b), NULL, meta);
}

static int idle_fd(Backend *b)
{
    struct remote *r = R(b);
    return r->closed || r->pending ? -1 : r->fd;
}

static int idle_pump(Backend *b)
{
    struct remote *r = R(b);
    while (!r->pending && !r->closed) {
        char *line = next_line(r, 0);
        if (!line)
            break;
        cJSON      *o = cJSON_Parse(line);
        const char *t = jstr(o, "turn");
        cJSON      *e = cJSON_GetObjectItem(o, "ev");
        free(line);
        if (e)
            forward(r, e);
        else if (cJSON_GetObjectItem(o, "side"))
            side_keep(r, o);
        else if (t && !strcmp(t, "begin")) {
            replace(&r->pending_prompt, jstr(o, "prompt"));
            r->pending = 1;
        } else if (cJSON_IsTrue(cJSON_GetObjectItem(o, "closed"))) {
            r->closed = 1;
            snprintf(r->err, sizeof r->err, "%s closed", r->target);
        }
        cJSON_Delete(o);
    }
    return r->pending;
}

static int take_continuation(Backend *b)
{
    struct remote *r = R(b);
    int            was = r->pending;
    r->pending = 0;
    return was;
}

static int connect_remote(struct remote *r)
{
    if (r->fd >= 0)
        close(r->fd);
    r->len = 0;
    r->pending = 0;
    r->closed = 1;
    r->fd = intercom_attach(r->target, r->err, sizeof r->err);
    if (r->fd < 0)
        return 0;
    r->closed = 0;
    char *line = NULL;
    for (int waited = 0; !line && !r->closed && waited < HISTORY_WAIT_MS; waited += 100)
        line = next_line(r, 100);
    cJSON *o = line ? cJSON_Parse(line) : NULL;
    free(line);
    cJSON *h = cJSON_DetachItemFromObject(o, "history");
    if (!h) {
        const char *e = jstr(o, "error");
        snprintf(r->err, sizeof r->err, "%s: %s", r->target,
                 e ? e : r->closed ? "connection closed" : "no answer");
        cJSON_Delete(o);
        close(r->fd);
        r->fd = -1;
        r->closed = 1;
        return 0;
    }
    cJSON_Delete(r->history);
    r->history = h;
    replace(&r->model, jstr(h, "model"));
    if (jstr(o, "running")) {
        replace(&r->pending_prompt, jstr(o, "running"));
        r->pending = 1;
    }
    cJSON_Delete(o);
    r->err[0] = '\0';
    return 1;
}

static int start(Backend *b, const char *resume)
{
    (void)resume;
    struct remote *r = R(b);
    if (r->fd < 0 || r->closed)
        connect_remote(r);
    return 1;
}

static void close_remote(Backend *b)
{
    struct remote *r = R(b);
    if (r->fd >= 0)
        close(r->fd);
    cJSON_Delete(r->history);
    cJSON_Delete(r->side);
    pthread_mutex_destroy(&r->side_lock);
    free(r->target);
    free(r->buf);
    free(r->model);
    free(r->pending_prompt);
    free(r);
    free(b);
}

static int reset(Backend *b)
{
    struct remote *r = R(b);
    if ((r->closed && !connect_remote(r)) || r->pending)
        return 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "clear", 1);
    return put(r, o);
}

static void usage(Backend *b, long *tokens, long *window)
{
    (void)b;
    *tokens = *window = 0;
}

static void rate_limit(Backend *b, backend_rate_limit *out)
{
    (void)b;
    memset(out, 0, sizeof *out);
}

static void set_model(Backend *b, const char *model)
{
    (void)b;
    (void)model;
}

static int set_effort(Backend *b, const char *effort)
{
    (void)b;
    (void)effort;
    return 0;
}

static void set_permission(Backend *b, const char *mode)
{
    (void)b;
    (void)mode;
}

static void set_event_cb(Backend *b, void (*cb)(void *ud, const backend_event *ev), void *ud)
{
    R(b)->on_event = cb;
    R(b)->event_ud = ud;
}

static void set_abort_check(Backend *b, int (*cb)(void))
{
    R(b)->abort = cb;
}

static int busy(Backend *b)
{
    (void)b;
    return 0;
}

static const char *none(Backend *b)
{
    (void)b;
    return NULL;
}

static const char *model(Backend *b)
{
    return R(b)->model;
}

static const char *last_error(Backend *b)
{
    return R(b)->err[0] ? R(b)->err : NULL;
}

Backend *remote_open(const char *target)
{
    Backend       *b = calloc(1, sizeof *b);
    struct remote *r = calloc(1, sizeof *r);
    if (!b || !r || !(r->target = strdup(target)) || !(r->side = cJSON_CreateArray())) {
        if (r)
            free(r->target);
        free(b);
        free(r);
        return NULL;
    }
    pthread_mutex_init(&r->side_lock, NULL);
    r->fd = -1;
    *b = (Backend){
        .ask = ask,
        .reset = reset,
        .close = close_remote,
        .start = start,
        .ask_ex = ask_ex,
        .continue_ex = continue_ex,
        .usage = usage,
        .rate_limit = rate_limit,
        .set_model = set_model,
        .set_effort = set_effort,
        .set_permission = set_permission,
        .set_event_cb = set_event_cb,
        .set_abort_check = set_abort_check,
        .idle_fd = idle_fd,
        .idle_pump = idle_pump,
        .take_continuation = take_continuation,
        .busy = busy,
        .session_id = none,
        .model = model,
        .effort = none,
        .auth_source = none,
        .last_error = last_error,
        .ctx = r,
    };
    return b;
}

const cJSON *remote_history(Backend *b)
{
    return b ? R(b)->history : NULL;
}

int remote_connected(Backend *b)
{
    return b && !R(b)->closed;
}

const char *remote_prompt(Backend *b)
{
    return b ? R(b)->pending_prompt : NULL;
}

cJSON *remote_side_take(Backend *b)
{
    if (!b)
        return NULL;
    struct remote *r = R(b);
    pthread_mutex_lock(&r->side_lock);
    cJSON *n = cJSON_DetachItemFromArray(r->side, 0);
    pthread_mutex_unlock(&r->side_lock);
    return n;
}

int remote_btw(Backend *b, const char *prompt)
{
    struct remote *r = R(b);
    if (r->closed && !connect_remote(r))
        return 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "btw", prompt);
    return put(r, o);
}
