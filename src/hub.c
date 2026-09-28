#include "hub.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <limits.h>
#include <microhttpd.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "app.h"
#include "intercom.h"
#include "settings.h"
#include "text.h"
#include "vendor/wsd.h"

#define PORT_DEFAULT   8792
#define BODY_MAX       (256 * 1024)
#define CLI_MAX        (4 * 1024 * 1024)
#define SEND_TIMEOUT   20
#define SURVEY_TIMEOUT 3
#define RETRY_S        10
#define LAUNCH_LABEL   "scrap.hub"

static struct settings cfg;
static int             cfg_loaded;

static const char *cfg_get(const char *key, const char *fallback)
{
    if (!cfg_loaded) {
        char path[4200];
        settings_load(&cfg, path_config_file(path, sizeof path, "hub") ? path : NULL);
        cfg_loaded = 1;
    }
    const char *v = settings_get(&cfg, key, NULL);
    return v && *v ? v : fallback;
}

static int port(void)
{
    int p = atoi(cfg_get("port", "0"));
    return p > 0 && p < 65536 ? p : PORT_DEFAULT;
}

static const char *cli(void)
{
    static const char *const paths[] = {
        "/usr/local/bin/tailscale",
        "/opt/homebrew/bin/tailscale",
        "/usr/bin/tailscale",
        "/Applications/Tailscale.app/Contents/MacOS/Tailscale",
        NULL,
    };
    const char *set = cfg_get("tailscale", NULL);
    if (set)
        return set;
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    return "tailscale";
}

static cJSON *tailscale(const char *args)
{
    char cmd[4400];
    snprintf(cmd, sizeof cmd, "'%s' %s 2>/dev/null", cli(), args);
    FILE *p = popen(cmd, "r");
    if (!p)
        return NULL;
    char  *buf = malloc(CLI_MAX + 1);
    size_t got = buf ? fread(buf, 1, CLI_MAX, p) : 0;
    pclose(p);
    if (!buf)
        return NULL;
    buf[got] = '\0';
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    return o;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

static void label(const char *dns, char *out, size_t size)
{
    snprintf(out, size, "%.*s", (int)strcspn(dns, "."), dns);
}

static int whois(const char *ip, char *login, size_t lsize, char *host, size_t hsize)
{
    char args[128];
    for (const char *p = ip; *p; p++)
        if (!isxdigit((unsigned char)*p) && *p != '.' && *p != ':')
            return 0;
    snprintf(args, sizeof args, "whois --json %s", ip);
    cJSON *o = tailscale(args);
    snprintf(login, lsize, "%s", jstr(cJSON_GetObjectItem(o, "UserProfile"), "LoginName"));
    label(jstr(cJSON_GetObjectItem(o, "Node"), "Name"), host, hsize);
    cJSON_Delete(o);
    return login[0] && host[0];
}

const char *hub_split(const char *target, char *host, size_t size)
{
    const char *colon = strchr(target, ':');
    if (!colon || colon == target || colon[1] != '@' || !colon[2])
        return NULL;
    for (const char *p = target; p < colon; p++)
        if (!isalnum((unsigned char)*p) && !strchr(".-_", *p))
            return NULL;
    if ((size_t)(colon - target) >= size)
        return NULL;
    snprintf(host, size, "%.*s", (int)(colon - target), target);
    return colon + 1;
}

static int is_phone(const cJSON *node)
{
    const char *os = jstr(node, "OS");
    return !strcasecmp(os, "iOS") || !strcasecmp(os, "android") || !strcasecmp(os, "tvOS");
}

static const char *node_ip(const cJSON *node)
{
    const char *ip = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem((cJSON *)node, "TailscaleIPs"), 0));
    return ip ? ip : "";
}

static cJSON *next_node(cJSON *st, cJSON *it)
{
    cJSON *self = cJSON_GetObjectItem(st, "Self"), *peers = cJSON_GetObjectItem(st, "Peer");
    if (!it)
        return self;
    double user = cJSON_GetNumberValue(cJSON_GetObjectItem(self, "UserID"));
    for (it = it == self ? (peers ? peers->child : NULL) : it->next; it; it = it->next)
        if (cJSON_GetNumberValue(cJSON_GetObjectItem(it, "UserID")) == user)
            return it;
    return NULL;
}

static void resolve(const char *host, char *ip, size_t size)
{
    snprintf(ip, size, "%s", host);
    cJSON *st = tailscale("status --json");
    for (cJSON *it = next_node(st, NULL); it; it = next_node(st, it)) {
        char name[256];
        label(jstr(it, "DNSName"), name, sizeof name);
        if ((!strcasecmp(name, host) || !strcasecmp(jstr(it, "HostName"), host)) && *node_ip(it)) {
            snprintf(ip, size, "%s", node_ip(it));
            break;
        }
    }
    cJSON_Delete(st);
}

struct buf {
    char  *data;
    size_t len;
};

static size_t collect(char *p, size_t size, size_t n, void *ud)
{
    struct buf *b = ud;
    size_t      add = size * n;
    if (b->len + add > CLI_MAX)
        return 0;
    char *grown = realloc(b->data, b->len + add + 1);
    if (!grown)
        return 0;
    memcpy(grown + b->len, p, add);
    b->data = grown;
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

static char *http(const char *ip, const char *path, const char *body, long timeout, long *status,
                  char *err, size_t esize)
{
    char url[1024];
    snprintf(url, sizeof url, strchr(ip, ':') ? "http://[%s]:%d%s" : "http://%s:%d%s", ip, port(),
             path);
    CURL      *c = curl_easy_init();
    struct buf b = {0};
    if (!c) {
        snprintf(err, esize, "curl is unavailable");
        return NULL;
    }
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, timeout < 3 ? timeout : 3L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (body)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    CURLcode rc = curl_easy_perform(c);
    *status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc == CURLE_COULDNT_CONNECT)
        snprintf(err, esize, "no hub");
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        snprintf(err, esize, "no answer");
    else if (rc != CURLE_OK)
        snprintf(err, esize, "%s", curl_easy_strerror(rc));
    if (rc != CURLE_OK) {
        free(b.data);
        return NULL;
    }
    if (!b.data)
        b.data = strdup("");
    return b.data;
}

static char *call(const char *host, const char *path, const char *body, long timeout, char *msg,
                  size_t size)
{
    char ip[256], err[256] = "";
    long status;
    resolve(host, ip, sizeof ip);
    char *out = http(ip, path, body, timeout, &status, err, sizeof err);
    if (!out) {
        snprintf(msg, size, "%s: %s", host, err);
        return NULL;
    }
    if (status != 200) {
        cJSON      *o = cJSON_Parse(out);
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(o, "error"));
        snprintf(msg, size, "%s: %s", host, e ? e : "bad reply");
        cJSON_Delete(o);
        free(out);
        return NULL;
    }
    return out;
}

int hub_send(const char *host, const char *from, const char *target, const char *text, char *msg,
             size_t size)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "to", target);
    cJSON_AddStringToObject(o, "text", text);
    if (from && *from)
        cJSON_AddStringToObject(o, "from", from);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *out = body ? call(host, "/v1/send", body, SEND_TIMEOUT, msg, size) : NULL;
    free(body);
    if (!out)
        return 1;
    snprintf(msg, size, "sent to %s:%s", host, target);
    free(out);
    return 0;
}

char *hub_read(const char *host, const char *target, long turns, long bytes, char *msg,
               size_t size)
{
    char *esc = curl_easy_escape(NULL, target, 0);
    char  path[1024];
    snprintf(path, sizeof path, "/v1/read?to=%s&n=%ld&bytes=%ld", esc ? esc : "", turns, bytes);
    curl_free(esc);
    return call(host, path, NULL, SEND_TIMEOUT, msg, size);
}

struct probe {
    char       machine[256];
    char       ip[64];
    cJSON     *result;
    pthread_t  thread;
    int        started;
};

static void *probe_run(void *ud)
{
    struct probe *p = ud;
    char          err[256] = "";
    long          status;
    char         *out = http(p->ip, "/v1/sessions", NULL, SURVEY_TIMEOUT, &status, err, sizeof err);
    cJSON        *o = out && status == 200 ? cJSON_Parse(out) : NULL;
    cJSON        *list = cJSON_DetachItemFromObject(o, "sessions");
    if (list)
        cJSON_AddItemToObject(p->result, "sessions", list);
    else
        cJSON_AddStringToObject(p->result, "error", err[0] ? err : "bad reply");
    cJSON_Delete(o);
    free(out);
    return NULL;
}

static int by_machine(const void *a, const void *b)
{
    return strcasecmp(((const struct probe *)a)->machine, ((const struct probe *)b)->machine);
}

cJSON *hub_survey(void)
{
    cJSON *st = tailscale("status --json");
    if (!st)
        return NULL;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int           cap = cJSON_GetArraySize(cJSON_GetObjectItem(st, "Peer")) + 1, n = 0;
    struct probe *probes = calloc((size_t)cap, sizeof *probes);
    cJSON        *self = cJSON_GetObjectItem(st, "Self");
    for (cJSON *it = next_node(st, NULL); it && probes; it = next_node(st, it)) {
        if (is_phone(it) || !*node_ip(it))
            continue;
        struct probe *p = &probes[n++];
        label(jstr(it, "DNSName"), p->machine, sizeof p->machine);
        snprintf(p->ip, sizeof p->ip, "%s", node_ip(it));
        p->result = cJSON_CreateObject();
        cJSON_AddStringToObject(p->result, "machine", p->machine);
        if (it == self) {
            cJSON_AddBoolToObject(p->result, "self", 1);
            cJSON_AddItemToObject(p->result, "sessions", intercom_live_json());
        }
        else if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "Online")))
            cJSON_AddStringToObject(p->result, "error", "offline");
        else
            p->started = pthread_create(&p->thread, NULL, probe_run, p) == 0;
    }
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
        if (probes[i].started)
            pthread_join(probes[i].thread, NULL);
    if (n > 1)
        qsort(probes + 1, (size_t)n - 1, sizeof *probes, by_machine);
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(out, probes[i].result);
    free(probes);
    cJSON_Delete(st);
    return out;
}

static char self_login[256], self_host[256];

struct request {
    char  *body;
    size_t len;
    int    too_large;
};

static enum MHD_Result respond(struct MHD_Connection *c, unsigned status, const char *type,
                               const char *text)
{
    struct MHD_Response *r =
        MHD_create_response_from_buffer(strlen(text), (void *)text, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE, type);
    enum MHD_Result res = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return res;
}

static enum MHD_Result respond_json(struct MHD_Connection *c, unsigned status, cJSON *o)
{
    char           *text = cJSON_PrintUnformatted(o);
    enum MHD_Result res = respond(c, status, "application/json", text ? text : "{}");
    free(text);
    cJSON_Delete(o);
    return res;
}

static enum MHD_Result fail(struct MHD_Connection *c, unsigned status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return respond_json(c, status, o);
}

static int peer(struct MHD_Connection *c, char *host, size_t size)
{
    const union MHD_ConnectionInfo *info =
        MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    char ip[INET6_ADDRSTRLEN] = "", login[256];
    if (!info || !info->client_addr)
        return 0;
    if (info->client_addr->sa_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)info->client_addr)->sin_addr, ip, sizeof ip);
    else if (info->client_addr->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)info->client_addr)->sin6_addr, ip, sizeof ip);
    return ip[0] && whois(ip, login, sizeof login, host, size) && !strcmp(login, self_login);
}

static enum MHD_Result handle(void *cls, struct MHD_Connection *c, const char *url,
                              const char *method, const char *version, const char *upload,
                              size_t *upload_size, void **ctx)
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

    char host[256];
    if (!peer(c, host, sizeof host))
        return fail(c, 403, "not the same tailscale user");
    if (rq->too_large)
        return fail(c, 413, "request too large");

    char msg[1200];
    if (!strcmp(url, "/v1/sessions") && !strcmp(method, "GET")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "machine", self_host);
        cJSON_AddItemToObject(o, "sessions", intercom_live_json());
        return respond_json(c, 200, o);
    }
    if (!strcmp(url, "/v1/send") && !strcmp(method, "POST")) {
        cJSON      *o = rq->body ? cJSON_Parse(rq->body) : NULL;
        const char *to = jstr(o, "to"), *text = jstr(o, "text"), *from = jstr(o, "from");
        if (!*to || !*text) {
            cJSON_Delete(o);
            return fail(c, 400, "send takes to and text");
        }
        int rc = intercom_deliver(host, *from ? from : NULL, to, text, msg, sizeof msg);
        cJSON_Delete(o);
        if (rc)
            return fail(c, 409, msg);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "message", msg);
        return respond_json(c, 200, r);
    }
    if (!strcmp(url, "/v1/read") && !strcmp(method, "GET")) {
        const char *to = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "to");
        const char *n = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "n");
        const char *b = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "bytes");
        long        turns = n ? atol(n) : 3, bytes = b ? atol(b) : 16000;
        if (!to || !*to || turns < 1 || bytes < 1)
            return fail(c, 400, "read takes to, n, bytes");
        char *text = intercom_read(to, turns, bytes, msg, sizeof msg);
        if (!text)
            return fail(c, 404, msg);
        enum MHD_Result res = respond(c, 200, "text/plain; charset=utf-8", text);
        free(text);
        return res;
    }
    return fail(c, 404, "no such route");
}

static void request_done(void *cls, struct MHD_Connection *c, void **ctx,
                         enum MHD_RequestTerminationCode t)
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

static int self_path(char *out, size_t size)
{
    char raw[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0)
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
    if (n <= 0)
        return 0;
    raw[n] = '\0';
#endif
    char real[PATH_MAX];
    return realpath(raw, real) && (size_t)snprintf(out, size, "%s", real) < size;
}

static int install(void)
{
#ifndef __APPLE__
    fprintf(stderr, APP_NAME ": hub --install uses launchd; run `scrap hub` under your init system\n");
    return 1;
#else
    char exe[PATH_MAX], plist[PATH_MAX], log[4200], domain[64], cmd[PATH_MAX * 2];
    const char *home = getenv("HOME");
    if (!home || !self_path(exe, sizeof exe) || !path_config_file(log, sizeof log, "hub.log"))
        return 1;
    snprintf(plist, sizeof plist, "%s/Library/LaunchAgents/" LAUNCH_LABEL ".plist", home);
    FILE *f = fopen(plist, "w");
    if (!f) {
        fprintf(stderr, APP_NAME ": cannot write %s\n", plist);
        return 1;
    }
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n"
            "  <key>Label</key><string>" LAUNCH_LABEL "</string>\n"
            "  <key>ProgramArguments</key><array><string>%s</string><string>hub</string></array>\n"
            "  <key>RunAtLoad</key><true/>\n"
            "  <key>KeepAlive</key><true/>\n"
            "  <key>ThrottleInterval</key><integer>10</integer>\n"
            "  <key>StandardOutPath</key><string>%s</string>\n"
            "  <key>StandardErrorPath</key><string>%s</string>\n"
            "</dict>\n</plist>\n",
            exe, log, log);
    if (fclose(f) != 0)
        return 1;
    snprintf(domain, sizeof domain, "gui/%u", (unsigned)getuid());
    snprintf(cmd, sizeof cmd, "launchctl bootout %s/" LAUNCH_LABEL " 2>/dev/null; launchctl bootstrap %s '%s'",
             domain, domain, plist);
    if (system(cmd) != 0) {
        fprintf(stderr, APP_NAME ": launchctl bootstrap failed\n");
        return 1;
    }
    printf("hub installed: %s runs %s hub; log %s\n", plist, exe, log);
    return 0;
#endif
}

static int uninstall(void)
{
#ifndef __APPLE__
    return 1;
#else
    char plist[PATH_MAX], cmd[PATH_MAX * 2];
    const char *home = getenv("HOME");
    if (!home)
        return 1;
    snprintf(plist, sizeof plist, "%s/Library/LaunchAgents/" LAUNCH_LABEL ".plist", home);
    snprintf(cmd, sizeof cmd, "launchctl bootout gui/%u/" LAUNCH_LABEL " 2>/dev/null", (unsigned)getuid());
    if (system(cmd) != 0)
        fprintf(stderr, APP_NAME ": hub was not loaded\n");
    unlink(plist);
    printf("hub removed\n");
    return 0;
#endif
}

static void reload(int sig)
{
    (void)sig;
    _exit(0);
}

static int serve(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGURG, reload);
    const char *bind = cfg_get("bind", NULL);
    while (!bind && !(bind = wsd_tailscale_ip())) {
        fprintf(stderr, APP_NAME " hub: no tailscale address; retrying\n");
        sleep(RETRY_S);
    }
    char bind_ip[64];
    snprintf(bind_ip, sizeof bind_ip, "%s", bind);
    while (!whois(bind_ip, self_login, sizeof self_login, self_host, sizeof self_host)) {
        fprintf(stderr, APP_NAME " hub: tailscale whois %s failed; retrying\n", bind_ip);
        sleep(RETRY_S);
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port())};
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, APP_NAME " hub: bind must be an IPv4 address, got %s\n", bind_ip);
        return 1;
    }
    struct MHD_Daemon *d = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, (uint16_t)port(), NULL, NULL, handle, NULL,
        MHD_OPTION_SOCK_ADDR, &addr, MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)30,
        MHD_OPTION_NOTIFY_COMPLETED, request_done, NULL, MHD_OPTION_END);
    if (!d) {
        fprintf(stderr, APP_NAME " hub: cannot listen on %s:%d\n", bind_ip, port());
        return 1;
    }
    printf(APP_NAME " hub: %s (%s) on %s:%d\n", self_host, self_login, bind_ip, port());
    for (;;) {
        sleep(RETRY_S);
        const char *now = cfg_get("bind", NULL) ? bind_ip : wsd_tailscale_ip();
        if (!now || strcmp(now, bind_ip)) {
            fprintf(stderr, APP_NAME " hub: tailscale address changed; exiting\n");
            MHD_stop_daemon(d);
            return 1;
        }
    }
}

int hub_main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--install"))
        return install();
    if (argc == 2 && !strcmp(argv[1], "--uninstall"))
        return uninstall();
    if (argc != 1) {
        fprintf(stderr, "usage: " APP_NAME " hub [--install | --uninstall]\n");
        return 2;
    }
    return serve();
}
