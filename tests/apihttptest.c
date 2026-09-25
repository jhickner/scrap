#include <curl/curl.h>
#include <microhttpd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "apicore.h"
#include "restart.h"
#include "settings.h"
#include "text.h"
#include "stubs/apistubs.h"
#include "vendor/cJSON.h"
#include "vendor/wsd.h"

#define TOKEN "test-token-0123456789abcdef0123456789"

static char base[64];
static int  failures;

static void fail(const char *what)
{
    fprintf(stderr, "apihttptest: %s\n", what);
    failures++;
}

int path_config_file(char *out, size_t size, const char *leaf)
{
    snprintf(out, size, "/nonexistent/%s", leaf);
    return 1;
}
void settings_load(struct settings *s, const char *path) { (void)s; (void)path; }
const char *settings_get(const struct settings *s, const char *key, const char *fallback)
{
    (void)s;
    if (!strcmp(key, "token"))
        return TOKEN;
    if (!strcmp(key, "bind"))
        return "127.0.0.1";
    return fallback;
}
void restart_flag(const char *flag) { (void)flag; }
int session_add_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; return 1; }
void session_remove_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; }
const char *wsd_tailscale_ip(void) { return NULL; }

static volatile int stop_loop, want_end_turn, want_burst;

static void *main_loop(void *ud)
{
    (void)ud;
    while (!stop_loop) {
        api_poll();
        if (want_end_turn) {
            end_turn(tabs[0], "all done", 0);
            want_end_turn = 0;
        }
        if (want_burst) {
            backend_event ev = {.kind = BACKEND_EV_ASSISTANT, .text = "x"};
            for (int i = 0; i < want_burst; i++)
                apicore_event(NULL, tabs[0], &ev);
            want_burst = 0;
        }
        usleep(1000);
    }
    return NULL;
}

static void wait_for(volatile int *flag)
{
    for (int i = 0; i < 5000 && *flag; i++)
        usleep(1000);
}

struct buf {
    char  *s;
    size_t n;
    int    frames_wanted;
};

static size_t collect(char *p, size_t size, size_t n, void *ud)
{
    struct buf *b = ud;
    size_t k = size * n;
    b->s = realloc(b->s, b->n + k + 1);
    memcpy(b->s + b->n, p, k);
    b->n += k;
    b->s[b->n] = '\0';
    if (b->frames_wanted) {
        int frames = 0;
        for (const char *q = b->s; (q = strstr(q, "\n\n")); q += 2)
            frames++;
        if (frames >= b->frames_wanted)
            return 0;
    }
    return k;
}

static long http(const char *method, const char *path, const char *token, const char *body,
                 const char *header, struct buf *out)
{
    char url[256], auth[128];
    snprintf(url, sizeof url, "%s%s", base, path);
    CURL *c = curl_easy_init();
    struct curl_slist *h = NULL;
    if (token) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
        h = curl_slist_append(h, auth);
    }
    if (header)
        h = curl_slist_append(h, header);
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return status;
}

static long req(const char *method, const char *path, const char *body, struct buf *out)
{
    free(out->s);
    memset(out, 0, sizeof *out);
    return http(method, path, TOKEN, body, NULL, out);
}

struct sse {
    pthread_t  th;
    struct buf b;
    char       header[64];
};

static void *sse_run(void *ud)
{
    struct sse *s = ud;
    http("GET", "/v1/events", TOKEN, NULL, s->header[0] ? s->header : NULL, &s->b);
    return NULL;
}

static void sse_open(struct sse *s, int frames, const char *last_id)
{
    memset(s, 0, sizeof *s);
    s->b.frames_wanted = frames;
    if (last_id)
        snprintf(s->header, sizeof s->header, "Last-Event-ID: %s", last_id);
    pthread_create(&s->th, NULL, sse_run, s);
    usleep(100000);
}

static char dlv_set[4096];

static enum MHD_Result dlv_handle(void *cls, struct MHD_Connection *c, const char *url, const char *method,
                                  const char *version, const char *data, size_t *size, void **st)
{
    (void)cls; (void)method; (void)version;
    if (!*st) {
        *st = calloc(1, 4096);
        return MHD_YES;
    }
    char *body = *st;
    if (*size) {
        strncat(body, data, *size < 4000 - strlen(body) ? *size : 4000 - strlen(body));
        *size = 0;
        return MHD_YES;
    }
    const char *auth = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    const char *reply = "{\"error\":{\"message\":\"bad key\"}}";
    unsigned code = 401;
    if (auth && !strcmp(auth, "Bearer dlv-key")) {
        code = 200;
        if (!strcmp(url, "/dlv/v1/backends"))
            reply = "{\"backends\":{\"mux\":{\"settings\":{\"hosts\":[{\"name\":\"other\",\"url\":\"http://10.0.0.9:8791\"},"
                    "{\"name\":\"testhost\",\"url\":\"http://stale:1\"}]}}}}";
        else {
            snprintf(dlv_set, sizeof dlv_set, "%s", body);
            reply = "{}";
        }
    }
    struct MHD_Response *r = MHD_create_response_from_buffer(strlen(reply), (void *)reply, MHD_RESPMEM_MUST_COPY);
    enum MHD_Result ret = MHD_queue_response(c, code, r);
    MHD_destroy_response(r);
    free(body);
    *st = NULL;
    return ret;
}

int gethostname(char *name, size_t len)
{
    snprintf(name, len, "testhost.local");
    return 0;
}

static void test_register_dlv(void)
{
    char home[] = "/tmp/apihttptest.XXXXXX", path[256], msg[512];
    if (!mkdtemp(home))
        return fail("mkdtemp");
    setenv("HOME", home, 1);
    if (api_register_dlv(msg, sizeof msg) != 0)
        fail("no dlv client config means no registration");
    struct MHD_Daemon *d = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, 18794, NULL, NULL, dlv_handle, NULL, MHD_OPTION_END);
    snprintf(path, sizeof path, "%s/.config", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/dlv", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/dlv/client.json", home);
    FILE *f = fopen(path, "w");
    fputs("{\"url\":\"http://127.0.0.1:18794/dlv\",\"key\":\"dlv-key\"}", f);
    fclose(f);
    if (api_register_dlv(msg, sizeof msg) != 1 || !strstr(msg, "registered with dlv as testhost"))
        fail(msg);
    cJSON *set = cJSON_Parse(dlv_set);
    cJSON *hosts = cJSON_GetObjectItem(cJSON_GetObjectItem(set, "settings"), "hosts");
    char want[128];
    snprintf(want, sizeof want, "%s", base);
    if (cJSON_GetArraySize(hosts) != 2 || strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(hosts, 0), "name")), "other") ||
        strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(hosts, 1), "url")), want))
        fail("registration keeps other hosts and replaces its own entry with the API url");
    if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(set, "secrets"), "host_tokens"), "testhost")), TOKEN))
        fail("registration sends the API token as the host token");
    cJSON_Delete(set);
    f = fopen(path, "w");
    fputs("{\"url\":\"http://127.0.0.1:18794/dlv\",\"key\":\"wrong\"}", f);
    fclose(f);
    if (api_register_dlv(msg, sizeof msg) != -1 || !strstr(msg, "bad key"))
        fail("a rejected key says why");
    MHD_stop_daemon(d);
    unlink(path);
}

int main(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    setenv("MUX_API_PORT", "18793", 1);
    snprintf(base, sizeof base, "http://127.0.0.1:18793");
    if (!api_start()) {
        fputs("apihttptest: api_start failed\n", stderr);
        return 1;
    }
    pthread_t loop;
    pthread_create(&loop, NULL, main_loop, NULL);
    struct buf b = {0};

    if (http("GET", "/v1/capacity", NULL, NULL, NULL, &b) != 401)
        fail("no token is 401");
    if (http("GET", "/v1/capacity", "wrong-token-0123456789abcdef012345678", NULL, NULL, &b) != 401)
        fail("a wrong token is 401");
    if (req("GET", "/v1/capacity", NULL, &b) != 200 || !strstr(b.s, "\"slots\":12"))
        fail("capacity over HTTP");
    if (req("POST", "/v1/agents", "{not json", &b) != 400 || !strstr(b.s, "invalid_json"))
        fail("bad JSON is 400");
    if (req("POST", "/v1/events", NULL, &b) != 405)
        fail("POST to the event stream is 405");
    char *big = malloc(1024 * 1024 + 64);
    memset(big, 'x', 1024 * 1024 + 63);
    big[1024 * 1024 + 63] = '\0';
    if (req("POST", "/v1/agents", big, &b) != 413)
        fail("an oversized body is 413");
    free(big);

    struct sse s;
    sse_open(&s, 7, NULL);
    if (req("POST", "/v1/agents", "{\"prompt\":{\"text\":\"hello\"},\"env\":{\"DLV_KEY\":\"k\"}}", &b) != 201 ||
        !strstr(b.s, "\"id\":\"ag_1\"") || !strstr(b.s, "\"status\":\"running\""))
        fail("create over HTTP is 201 with a running run");
    want_end_turn = 1;
    wait_for(&want_end_turn);
    pthread_join(s.th, NULL);
    if (!s.b.s || !strstr(s.b.s, "event: agent") || !strstr(s.b.s, "event: result") ||
        !strstr(s.b.s, "\"result\":\"all done\"") || !strstr(s.b.s, "event: done") || !strstr(s.b.s, "id: 1\n"))
        fail("the stream carries agent, status, result, and done with ids");
    free(s.b.s);

    if (req("GET", "/v1/agents/ag_1/runs/run_1", NULL, &b) != 200 || !strstr(b.s, "\"status\":\"finished\""))
        fail("the run reads back finished");

    sse_open(&s, 1, "1");
    pthread_join(s.th, NULL);
    if (!s.b.s || strncmp(s.b.s, "id: 2\n", 6))
        fail("Last-Event-ID resumes at the next event");
    free(s.b.s);

    want_burst = 10050;
    wait_for(&want_burst);
    sse_open(&s, 2, "1");
    pthread_join(s.th, NULL);
    if (!s.b.s || strncmp(s.b.s, "event: reset\n", 13) || !strstr(s.b.s, "\nid: "))
        fail("an expired Last-Event-ID gets a reset");
    free(s.b.s);

    if (req("DELETE", "/v1/agents/ag_1", NULL, &b) != 409 || !strstr(b.s, "agent_in_view"))
        fail("delete of the tab in view is 409 over HTTP");
    if (req("DELETE", "/v1/agents/ag_1?force=1", NULL, &b) != 200 || !strstr(b.s, "\"status\":\"exited\""))
        fail("force delete over HTTP");

    test_register_dlv();

    free(b.s);
    stop_loop = 1;
    pthread_join(loop, NULL);
    api_stop();
    curl_global_cleanup();
    if (failures)
        return 1;
    puts("apihttptest: ok");
    return 0;
}
