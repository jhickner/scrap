#include "api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <microhttpd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "apicore.h"
#include "app.h"
#include "restart.h"
#include "session.h"
#include "settings.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "vendor/wsd.h"

#define PORT_DEFAULT  8791
#define TOKEN_MIN     32
#define BODY_MAX      (1024 * 1024)
#define REPLY_WAIT_S  35
#define RING_MAX      10000
#define HEARTBEAT_S   15
#define TICK_MS       250

/* one request crossing from an HTTP thread to the main thread. whichever side
   finishes last frees it: the HTTP thread after a reply, or the main thread
   when the HTTP thread gave up waiting */
struct job {
    struct apicall call;
    char          *method, *path, *query;
    cJSON         *body;
    int            done;
    int            abandoned;
    struct job    *next;
};

struct request {
    char  *body;
    size_t len;
    int    too_large;
};

struct event {
    long  id;
    char  name[24];
    char *data;
};

/* an SSE connection's position in the ring, and the unsent tail of the
   frame it is writing */
struct stream {
    long   next;
    int    reset;
    char  *pending;
    size_t sent, len;
};

static struct {
    int                active;
    int                stopping;
    struct MHD_Daemon *daemon;
    char               token[256];
    int                wake[2];
    pthread_mutex_t    lock;
    pthread_cond_t     cond;     /* job replies and new events */
    struct job        *inbox, *inbox_tail;
    struct event       ring[RING_MAX];
    long               next_id;  /* id the next event gets */
    struct timespec    last_tick;
} api = {.wake = {-1, -1}, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
         .next_id = 1};

static void wake_up(void)
{
    if (api.wake[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(api.wake[1], &b, 1);
        (void)ignored;
    }
}

static void job_free(struct job *j)
{
    cJSON_Delete(j->call.out);
    cJSON_Delete(j->body);
    free(j->method);
    free(j->path);
    free(j->query);
    free(j);
}

/* apicore's reply callback, on the main thread */
static void on_reply(struct apicall *c)
{
    struct job *j = c->ud;
    pthread_mutex_lock(&api.lock);
    if (j->abandoned) {
        pthread_mutex_unlock(&api.lock);
        job_free(j);
        return;
    }
    j->done = 1;
    pthread_cond_broadcast(&api.cond);
    pthread_mutex_unlock(&api.lock);
}

/* apicore's event callback, on the main thread */
static void on_emit(const char *name, cJSON *data)
{
    char *text = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    if (!text)
        return;
    pthread_mutex_lock(&api.lock);
    struct event *e = &api.ring[api.next_id % RING_MAX];
    free(e->data);
    e->id = api.next_id++;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->data = text;
    pthread_cond_broadcast(&api.cond);
    pthread_mutex_unlock(&api.lock);
}

static int token_ok(const char *auth)
{
    if (!auth || strncasecmp(auth, "Bearer ", 7))
        return 0;
    const char *got = auth + 7;
    size_t n = strlen(api.token), m = strlen(got);
    unsigned char diff = (unsigned char)(n != m);
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(api.token[i] ^ (i < m ? got[i] : 0));
    return diff == 0;
}

static enum MHD_Result send_json(struct MHD_Connection *c, int status, const cJSON *o)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    struct MHD_Response *r = MHD_create_response_from_buffer(s ? strlen(s) : 2, s ? s : (void *)"{}",
                                                             s ? MHD_RESPMEM_MUST_FREE : MHD_RESPMEM_PERSISTENT);
    if (!r) {
        free(s);
        return MHD_NO;
    }
    MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    enum MHD_Result res = MHD_queue_response(c, (unsigned)status, r);
    MHD_destroy_response(r);
    return res;
}

static enum MHD_Result send_error(struct MHD_Connection *c, int status, const char *code, const char *msg)
{
    cJSON *o = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    enum MHD_Result r = send_json(c, status, o);
    cJSON_Delete(o);
    return r;
}

/* the frame for one event, or a reset when the client's position has left the ring */
static char *frame(const struct event *e)
{
    size_t n = strlen(e->data) + strlen(e->name) + 64;
    char *f = malloc(n);
    if (f)
        snprintf(f, n, "id: %ld\nevent: %s\ndata: %s\n\n", e->id, e->name, e->data);
    return f;
}

static ssize_t stream_read(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct stream *st = cls;
    if (!st->pending) {
        pthread_mutex_lock(&api.lock);
        long oldest = api.next_id - RING_MAX > 1 ? api.next_id - RING_MAX : 1;
        if (st->next < oldest) {
            st->reset = 1;
            st->next = oldest;
        }
        if (!st->reset && st->next >= api.next_id && !api.stopping) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += HEARTBEAT_S;
            while (st->next >= api.next_id && !api.stopping &&
                   pthread_cond_timedwait(&api.cond, &api.lock, &until) != ETIMEDOUT)
                ;
        }
        if (api.stopping) {
            pthread_mutex_unlock(&api.lock);
            return MHD_CONTENT_READER_END_OF_STREAM;
        }
        if (st->reset) {
            st->reset = 0;
            st->pending = strdup("event: reset\ndata: {}\n\n");
        } else if (st->next < api.next_id)
            st->pending = frame(&api.ring[st->next++ % RING_MAX]);
        else
            st->pending = strdup(": heartbeat\n\n");
        pthread_mutex_unlock(&api.lock);
        if (!st->pending)
            return MHD_CONTENT_READER_END_WITH_ERROR;
        st->sent = 0;
        st->len = strlen(st->pending);
    }
    size_t n = st->len - st->sent < max ? st->len - st->sent : max;
    memcpy(buf, st->pending + st->sent, n);
    st->sent += n;
    if (st->sent == st->len) {
        free(st->pending);
        st->pending = NULL;
    }
    return (ssize_t)n;
}

static void stream_free(void *cls)
{
    struct stream *st = cls;
    free(st->pending);
    free(st);
}

static enum MHD_Result send_events(struct MHD_Connection *c)
{
    struct stream *st = calloc(1, sizeof *st);
    if (!st)
        return MHD_NO;
    const char *last = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Last-Event-ID");
    pthread_mutex_lock(&api.lock);
    st->next = last && *last ? strtol(last, NULL, 10) + 1 : api.next_id;
    pthread_mutex_unlock(&api.lock);
    struct MHD_Response *r = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4096, stream_read, st, stream_free);
    if (!r) {
        free(st);
        return MHD_NO;
    }
    MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE, "text/event-stream");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    enum MHD_Result res = MHD_queue_response(c, MHD_HTTP_OK, r);
    MHD_destroy_response(r);
    return res;
}

static enum MHD_Result add_arg(void *cls, enum MHD_ValueKind kind, const char *key, const char *value)
{
    (void)kind;
    char *q = cls;
    size_t n = strlen(q);
    snprintf(q + n, 1024 - n, "%s%s=%s", n ? "&" : "", key, value ? value : "");
    return MHD_YES;
}

/* hands the call to the main thread and waits for its reply */
static enum MHD_Result run_job(struct MHD_Connection *c, const char *url, const char *method, cJSON *body)
{
    struct job *j = calloc(1, sizeof *j);
    char query[1024] = "";
    MHD_get_connection_values(c, MHD_GET_ARGUMENT_KIND, add_arg, query);
    j->method = strdup(method);
    j->path = strdup(url);
    j->query = query[0] ? strdup(query) : NULL;
    j->body = body;
    j->call = (struct apicall){.method = j->method, .path = j->path, .query = j->query, .body = body, .ud = j};

    pthread_mutex_lock(&api.lock);
    if (api.stopping) {
        pthread_mutex_unlock(&api.lock);
        job_free(j);
        return send_error(c, 503, "stopping", "this mux is shutting down");
    }
    if (api.inbox_tail)
        api.inbox_tail->next = j;
    else
        api.inbox = j;
    api.inbox_tail = j;
    wake_up();
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += REPLY_WAIT_S;
    int timed_out = 0;
    while (!j->done && !api.stopping && !timed_out)
        timed_out = pthread_cond_timedwait(&api.cond, &api.lock, &until) == ETIMEDOUT;
    if (!j->done) {
        j->abandoned = 1;
        pthread_mutex_unlock(&api.lock);
        return send_error(c, api.stopping ? 503 : 504, api.stopping ? "stopping" : "timeout",
                          "no reply from the mux main loop");
    }
    pthread_mutex_unlock(&api.lock);
    enum MHD_Result r = send_json(c, j->call.status, j->call.out);
    job_free(j);
    return r;
}

static enum MHD_Result handle(void *cls, struct MHD_Connection *c, const char *url, const char *method,
                              const char *version, const char *upload, size_t *upload_size, void **ctx)
{
    (void)cls;
    (void)version;
    if (!*ctx) {
        *ctx = calloc(1, sizeof(struct request));
        return *ctx ? MHD_YES : MHD_NO;
    }
    struct request *rq = *ctx;
    if (*upload_size) {
        if (rq->len + *upload_size > BODY_MAX)
            rq->too_large = 1;
        else {
            char *grown = realloc(rq->body, rq->len + *upload_size + 1);
            if (!grown)
                return MHD_NO;
            rq->body = grown;
            memcpy(rq->body + rq->len, upload, *upload_size);
            rq->len += *upload_size;
            rq->body[rq->len] = '\0';
        }
        *upload_size = 0;
        return MHD_YES;
    }
    if (!token_ok(MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization")))
        return send_error(c, 401, "unauthorized", "missing or invalid token");
    if (rq->too_large)
        return send_error(c, 413, "too_large", "request body over 1 MiB");
    if (!strcmp(url, "/v1/events"))
        return strcmp(method, "GET") ? send_error(c, 405, "method_not_allowed", "use GET") : send_events(c);
    cJSON *body = NULL;
    if (rq->len) {
        body = cJSON_ParseWithLength(rq->body, rq->len);
        if (!body)
            return send_error(c, 400, "invalid_json", "body must be JSON");
    }
    return run_job(c, url, method, body);
}

static void request_done(void *cls, struct MHD_Connection *c, void **ctx, enum MHD_RequestTerminationCode t)
{
    (void)cls;
    (void)c;
    (void)t;
    struct request *rq = *ctx;
    if (rq) {
        free(rq->body);
        free(rq);
        *ctx = NULL;
    }
}

int api_start(void)
{
    if (api.active)
        return 1;
    char path[4200];
    struct settings cfg;
    if (!path_config_file(path, sizeof path, "api")) {
        fputs(APP_NAME ": --api: no config directory\n", stderr);
        return 0;
    }
    settings_load(&cfg, path);
    const char *token = settings_get(&cfg, "token", NULL);
    if (!token || strlen(token) < TOKEN_MIN) {
        fprintf(stderr, APP_NAME ": --api needs `token` (%d+ chars) in %s\n", TOKEN_MIN, path);
        return 0;
    }
    snprintf(api.token, sizeof api.token, "%s", token);
    const char *port_env = getenv("MUX_API_PORT");
    const char *port_cfg = settings_get(&cfg, "port", NULL);
    int port = atoi(port_env && *port_env ? port_env : port_cfg && *port_cfg ? port_cfg : "0");
    if (port <= 0 || port > 65535)
        port = PORT_DEFAULT;
    const char *bind = settings_get(&cfg, "bind", NULL);
    if (!bind || !*bind)
        bind = wsd_tailscale_ip();
    if (!bind) {
        fprintf(stderr, APP_NAME ": --api: no tailscale address; set `bind` in %s\n", path);
        return 0;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, bind, &addr.sin_addr) != 1) {
        fprintf(stderr, APP_NAME ": --api: bind must be an IPv4 address, got %s\n", bind);
        return 0;
    }
    if (pipe(api.wake) != 0) {
        fprintf(stderr, APP_NAME ": --api: wake pipe: %s\n", strerror(errno));
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(api.wake[i], F_SETFL, O_NONBLOCK);
        fcntl(api.wake[i], F_SETFD, FD_CLOEXEC);
    }
    api.stopping = 0;
    apicore_init(on_reply, on_emit);
    api.daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD, (uint16_t)port,
                                  NULL, NULL, handle, NULL, MHD_OPTION_SOCK_ADDR, &addr,
                                  MHD_OPTION_NOTIFY_COMPLETED, request_done, NULL, MHD_OPTION_END);
    if (!api.daemon) {
        fprintf(stderr, APP_NAME ": --api: can't listen on %s:%d\n", bind, port);
        close(api.wake[0]);
        close(api.wake[1]);
        api.wake[0] = api.wake[1] = -1;
        return 0;
    }
    session_add_listener(apicore_event, NULL);
    api.active = 1;
    restart_flag("--api");
    fprintf(stderr, APP_NAME ": worker API on http://%s:%d\n", bind, port);
    return 1;
}

void api_stop(void)
{
    if (!api.active)
        return;
    pthread_mutex_lock(&api.lock);
    api.stopping = 1;
    pthread_cond_broadcast(&api.cond);
    pthread_mutex_unlock(&api.lock);
    MHD_stop_daemon(api.daemon);
    api.daemon = NULL;
    session_remove_listener(apicore_event, NULL);
    /* every HTTP thread has returned, so no one waits on what is left */
    for (struct job *j = api.inbox, *next; j; j = next) {
        next = j->next;
        job_free(j);
    }
    api.inbox = api.inbox_tail = NULL;
    apicore_init(NULL, NULL);
    apicore_reset();
    close(api.wake[0]);
    close(api.wake[1]);
    api.wake[0] = api.wake[1] = -1;
    api.active = 0;
}

int api_active(void)
{
    return api.active;
}

int api_fds(int *out, int max)
{
    if (!api.active || max < 1 || api.wake[0] < 0)
        return 0;
    out[0] = api.wake[0];
    return 1;
}

void api_poll(void)
{
    if (!api.active)
        return;
    char buf[64];
    while (read(api.wake[0], buf, sizeof buf) > 0)
        ;
    for (;;) {
        pthread_mutex_lock(&api.lock);
        struct job *j = api.inbox;
        if (j) {
            api.inbox = j->next;
            if (!api.inbox)
                api.inbox_tail = NULL;
        }
        pthread_mutex_unlock(&api.lock);
        if (!j)
            break;
        apicore_handle(&j->call);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - api.last_tick.tv_sec) * 1000 + (now.tv_nsec - api.last_tick.tv_nsec) / 1000000;
    if (ms >= TICK_MS) {
        api.last_tick = now;
        apicore_tick();
    }
}

void api_turn_begin(struct session *s)
{
    if (api.active)
        apicore_turn_begin(s);
}

void api_turn_done(struct session *s)
{
    if (api.active)
        apicore_turn_done(s);
}
