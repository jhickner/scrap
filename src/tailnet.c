#include "tailnet.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "intercom.h"
#include "settings.h"
#include "text.h"
#include "vendor/wsd.h"

extern char **environ;

#define PORT_DEFAULT   8792
#define CLI_MAX        (4 * 1024 * 1024)
#define REPLY_MAX      (4 * 1024 * 1024)
#define SEND_TIMEOUT   20
#define SURVEY_TIMEOUT 3
#define SPAWN_TIMEOUT  40
#define WHOIS_TTL      60
#define WHOIS_SLOTS    32

static struct settings cfg;
static int             cfg_loaded;

static const char *cfg_get(const char *key, const char *fallback)
{
    if (!cfg_loaded) {
        char path[4200];
        settings_load(&cfg, path_config_file(path, sizeof path, "net") ? path : NULL);
        cfg_loaded = 1;
    }
    const char *v = settings_get(&cfg, key, NULL);
    return v && *v ? v : fallback;
}

int tailnet_dir_port(void)
{
    int p = atoi(cfg_get("port", "0"));
    return p > 0 && p < 65536 ? p : PORT_DEFAULT;
}

const char *tailnet_bind_ip(void)
{
    const char *set = cfg_get("bind", NULL);
    return set ? set : wsd_tailscale_ip();
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

static cJSON *tailscale(const char *verb, const char *arg)
{
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    posix_spawn_file_actions_adddup2(&acts, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&acts, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&acts, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)cli(), (char *)verb, (char *)"--json", (char *)arg, NULL};
    pid_t pid;
    int   ok = posix_spawnp(&pid, argv[0], &acts, NULL, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&acts);
    close(fds[1]);
    char  *buf = ok ? malloc(CLI_MAX + 1) : NULL;
    size_t got = 0;
    while (buf && got < CLI_MAX) {
        ssize_t n = read(fds[0], buf + got, CLI_MAX - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fds[0]);
    if (ok)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
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
    for (const char *p = ip; *p; p++)
        if (!isxdigit((unsigned char)*p) && *p != '.' && *p != ':')
            return 0;
    cJSON *o = tailscale("whois", ip);
    snprintf(login, lsize, "%s", jstr(cJSON_GetObjectItem(o, "UserProfile"), "LoginName"));
    label(jstr(cJSON_GetObjectItem(o, "Node"), "Name"), host, hsize);
    cJSON_Delete(o);
    return login[0] && host[0];
}

static struct {
    char   ip[64];
    char   login[256];
    char   host[256];
    time_t at;
} seen[WHOIS_SLOTS];

static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static int whois_cached(const char *ip, char *login, size_t lsize, char *host, size_t hsize)
{
    pthread_mutex_lock(&seen_lock);
    int found = 0;
    time_t now = time(NULL);
    int    slot = 0;
    for (int i = 0; i < WHOIS_SLOTS && !found; i++) {
        if (!strcmp(seen[i].ip, ip) && now - seen[i].at < WHOIS_TTL) {
            snprintf(login, lsize, "%s", seen[i].login);
            snprintf(host, hsize, "%s", seen[i].host);
            found = 1;
        } else if (seen[i].at < seen[slot].at)
            slot = i;
    }
    pthread_mutex_unlock(&seen_lock);
    if (found)
        return 1;
    if (!whois(ip, login, lsize, host, hsize))
        return 0;
    pthread_mutex_lock(&seen_lock);
    snprintf(seen[slot].ip, sizeof seen[slot].ip, "%s", ip);
    snprintf(seen[slot].login, sizeof seen[slot].login, "%s", login);
    snprintf(seen[slot].host, sizeof seen[slot].host, "%s", host);
    seen[slot].at = now;
    pthread_mutex_unlock(&seen_lock);
    return 1;
}

int tailnet_peer(const char *ip, char *host, size_t size)
{
    char        me[256], mine[256], login[256];
    const char *bind = tailnet_bind_ip();
    return bind && whois_cached(bind, me, sizeof me, mine, sizeof mine) &&
           whois_cached(ip, login, sizeof login, host, size) && !strcmp(login, me);
}

const char *tailnet_split(const char *target, char *host, size_t size)
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
    const char *ip = cJSON_GetStringValue(
        cJSON_GetArrayItem(cJSON_GetObjectItem((cJSON *)node, "TailscaleIPs"), 0));
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

void tailnet_resolve(const char *host, char *ip, size_t size)
{
    snprintf(ip, size, "%s", host);
    cJSON *st = tailscale("status", NULL);
    for (cJSON *it = next_node(st, NULL); it; it = next_node(st, it)) {
        char name[256];
        label(jstr(it, "DNSName"), name, sizeof name);
        if ((!strcasecmp(name, host) || !strcasecmp(jstr(it, "HostName"), host)) &&
            *node_ip(it)) {
            snprintf(ip, size, "%s", node_ip(it));
            break;
        }
    }
    cJSON_Delete(st);
}

int tailnet_connect(const char *ip, int port, int timeout_s, char *err, size_t esize)
{
    struct sockaddr_storage sa = {0};
    socklen_t               len;
    struct sockaddr_in     *v4 = (struct sockaddr_in *)&sa;
    struct sockaddr_in6    *v6 = (struct sockaddr_in6 *)&sa;
    if (inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        len = sizeof *v4;
    } else if (inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)port);
        len = sizeof *v6;
    } else {
        snprintf(err, esize, "unknown machine");
        return -1;
    }
    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, esize, "%s", strerror(errno));
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr *)&sa, len);
    if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        int           soerr = 0;
        socklen_t     sl = sizeof soerr;
        rc = poll(&p, 1, timeout_s * 1000) == 1 &&
                     getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && !soerr
                 ? 0
                 : -1;
        if (rc != 0)
            errno = soerr ? soerr : ETIMEDOUT;
    }
    if (rc != 0) {
        snprintf(err, esize, "%s", errno == ECONNREFUSED ? "no scrap running"
                                   : errno == ETIMEDOUT  ? "no answer"
                                                         : strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static char *exchange(const char *ip, int port, cJSON *req, int timeout_s, char *err,
                      size_t esize)
{
    int fd = tailnet_connect(ip, port, timeout_s < 3 ? timeout_s : 3, err, esize);
    if (fd < 0)
        return NULL;
    char  *json = cJSON_PrintUnformatted(req);
    size_t len = json ? strlen(json) : 0;
    int    ok = json && send(fd, json, len, 0) == (ssize_t)len && send(fd, "\n", 1, 0) == 1;
    free(json);
    char  *out = ok ? malloc(REPLY_MAX + 1) : NULL;
    size_t got = 0;
    time_t until = time(NULL) + timeout_s;
    while (out && got < REPLY_MAX) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           left = (int)(until - time(NULL));
        if (left <= 0 || poll(&p, 1, left * 1000) != 1)
            break;
        ssize_t n = recv(fd, out + got, REPLY_MAX - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (memchr(out + got - n, '\n', (size_t)n))
            break;
    }
    close(fd);
    if (out)
        out[got] = '\0';
    char *nl = out ? strchr(out, '\n') : NULL;
    if (nl)
        *nl = '\0';
    if (!out || !*out) {
        snprintf(err, esize, "no answer");
        free(out);
        return NULL;
    }
    return out;
}

static cJSON *ask(const char *ip, int port, cJSON *req, int timeout_s, char *msg, size_t size)
{
    char  err[256] = "";
    char *out = exchange(ip, port, req, timeout_s, err, sizeof err);
    cJSON_Delete(req);
    cJSON *r = out ? cJSON_Parse(out) : NULL;
    free(out);
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(r, "error"));
    if (!r || e) {
        snprintf(msg, size, "%s", e ? e : err[0] ? err : "bad reply");
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

static cJSON *directory(const char *ip, char *msg, size_t size)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddBoolToObject(req, "ls", 1);
    return ask(ip, tailnet_dir_port(), req, SURVEY_TIMEOUT, msg, size);
}

static const cJSON *find(const cJSON *dir, const char *target)
{
    const char *name = target[0] == '@' ? target + 1 : NULL;
    cJSON      *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem((cJSON *)dir, "sessions"))
    {
        if (name ? !strcmp(jstr(o, "name"), name)
                 : *jstr(o, "id") && !strncmp(jstr(o, "id"), target, strlen(target)))
            return o;
    }
    return NULL;
}

int tailnet_locate(const char *host, const char *target, char *ip, size_t ipsize, int *port,
                   char *id, size_t idsize, char *msg, size_t size)
{
    char   err[512];
    tailnet_resolve(host, ip, ipsize);
    cJSON *dir = directory(ip, err, sizeof err);
    if (!dir) {
        snprintf(msg, size, "%s: %s", host, err);
        return 0;
    }
    const cJSON *e = find(dir, target);
    cJSON       *port_item = e ? cJSON_GetObjectItem((cJSON *)e, "port") : NULL;
    int          p = cJSON_IsNumber(port_item) ? (int)port_item->valuedouble : 0;
    if (!e)
        snprintf(msg, size, "%s: no live session matches %s", host, target);
    else if (p <= 0)
        snprintf(msg, size, "%s: %s is not reachable on the tailnet", host, target);
    else {
        *port = p;
        snprintf(id, idsize, "%s", jstr(e, "id"));
    }
    cJSON_Delete(dir);
    return e && p > 0;
}

int tailnet_send(const char *host, const char *from, const char *target, const char *text,
                 char *msg, size_t size)
{
    char ip[256], id[128];
    int  port;
    if (!tailnet_locate(host, target, ip, sizeof ip, &port, id, sizeof id, msg, size))
        return 1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "send", text);
    cJSON_AddStringToObject(req, "session", id);
    if (target[0] == '@')
        cJSON_AddStringToObject(req, "name", target + 1);
    if (from && *from)
        cJSON_AddStringToObject(req, "from", from);
    char   err[512];
    cJSON *r = ask(ip, port, req, SEND_TIMEOUT, err, sizeof err);
    if (!r) {
        snprintf(msg, size, "%s: %s", host, err);
        return 1;
    }
    cJSON_Delete(r);
    snprintf(msg, size, "sent to %s:%s", host, target);
    return 0;
}

char *tailnet_read(const char *host, const char *target, long turns, long bytes, char *msg,
                   size_t size)
{
    char ip[256], err[512];
    tailnet_resolve(host, ip, sizeof ip);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "read", target);
    cJSON_AddNumberToObject(req, "n", (double)turns);
    cJSON_AddNumberToObject(req, "bytes", (double)bytes);
    cJSON *r = ask(ip, tailnet_dir_port(), req, SEND_TIMEOUT, err, sizeof err);
    if (!r) {
        snprintf(msg, size, "%s: %s", host, err);
        return NULL;
    }
    char *text = strdup(jstr(r, "text"));
    cJSON_Delete(r);
    return text;
}

int tailnet_spawn(const char *host, const char *cwd, char *target, size_t tsize, char *msg,
                  size_t size)
{
    char ip[256], err[512];
    tailnet_resolve(host, ip, sizeof ip);
    cJSON *req = cJSON_CreateObject();
    if (cwd && *cwd)
        cJSON_AddStringToObject(req, "cwd", cwd);
    cJSON *r = ask(ip, tailnet_dir_port(), req, SPAWN_TIMEOUT, err, sizeof err);
    if (!r) {
        snprintf(msg, size, "%s: %s", host, err);
        return 0;
    }
    if (*jstr(r, "name"))
        snprintf(target, tsize, "%s:@%s", host, jstr(r, "name"));
    else
        snprintf(target, tsize, "%s:%s", host, jstr(r, "session"));
    cJSON_Delete(r);
    return 1;
}

struct probe {
    char      machine[256];
    char      ip[64];
    cJSON    *result;
    pthread_t thread;
    int       started;
};

static void *probe_run(void *ud)
{
    struct probe *p = ud;
    char          err[256] = "";
    cJSON        *dir = directory(p->ip, err, sizeof err);
    cJSON        *list = cJSON_DetachItemFromObject(dir, "sessions");
    if (list)
        cJSON_AddItemToObject(p->result, "sessions", list);
    else
        cJSON_AddStringToObject(p->result, "error", err[0] ? err : "bad reply");
    cJSON_Delete(dir);
    return NULL;
}

static int by_machine(const void *a, const void *b)
{
    return strcasecmp(((const struct probe *)a)->machine, ((const struct probe *)b)->machine);
}

cJSON *tailnet_survey(void)
{
    cJSON *st = tailscale("status", NULL);
    if (!st)
        return NULL;
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
        } else if (!cJSON_IsTrue(cJSON_GetObjectItem(it, "Online")))
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

int tailnet_is_self(const char *host)
{
    const char *me = tailnet_self_name(), *bind = tailnet_bind_ip();
    if (me && *me && !strcasecmp(host, me))
        return 1;
    char ip[256];
    tailnet_resolve(host, ip, sizeof ip);
    return bind && !strcmp(ip, bind);
}

const char *tailnet_self_name(void)
{
    static char host[256];
    char        login[256];
    const char *bind = tailnet_bind_ip();
    if (!host[0] && bind)
        whois_cached(bind, login, sizeof login, host, sizeof host);
    return host;
}
