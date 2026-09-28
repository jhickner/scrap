#include "dispatch.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "prompt.h"
#include "session.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "workspace.h"

#define REQUEST_MAX  65536
#define CLIENT_MAX   16

#define ID_WAIT_MS   30000

#define PAIR_WINDOW  60
#define PAIR_CAP     6
#define PAIR_SLOTS   64

int dispatch_dir(char *out, size_t size)
{
    const char *env = getenv("SCRAP_DISPATCH_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    return path_config_subdir(out, size, "dispatch");
}

static const char *field(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s && *s ? s : NULL;
}

int dispatch_socket_path(long pid, char *out, size_t size)
{
    char dir[4200];
    if (!dispatch_dir(dir, sizeof dir))
        return 0;
    return (size_t)snprintf(out, size, "%s/%ld.sock", dir, pid) < size &&
           strlen(out) < sizeof ((struct sockaddr_un *)0)->sun_path;
}

static void reply(int fd, const char *json)
{
    if (fd < 0)
        return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    size_t len = strlen(json);
    char  *line = malloc(len + 2);
    if (line) {
        memcpy(line, json, len);
        line[len] = '\n';
        for (size_t off = 0; off < len + 1;) {
            ssize_t n = send(fd, line + off, len + 1 - off, 0);
            if (n <= 0 && errno != EINTR)
                break;
            if (n > 0)
                off += (size_t)n;
        }
        free(line);
    }
    close(fd);
}

static void reply_error(int fd, const char *what, const char *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", what);
    if (id)
        cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    reply(fd, json ? json : "{\"error\": \"oom\"}");
    free(json);
    cJSON_Delete(r);
}

static void echo_prompt(struct session *s, void *ud)
{
    (void)s;
    prompt_echo_message(ud);
}

static int index_of_id(const cJSON *target)
{
    if (!cJSON_IsString(target))
        return -1;
    const char *id = target->valuestring;
    if (!id || !*id)
        return -1;
    return workspace_find_id(id);
}

static void close_session(int fd, const cJSON *target)
{
    if (!cJSON_IsString(target) || !target->valuestring || !*target->valuestring) {
        reply_error(fd, "close takes a session id", NULL);
        return;
    }
    const char *id = target->valuestring;
    int at = index_of_id(target);
    struct session *s = workspace_at(at);
    if (!s) {
        reply_error(fd, "no such session", id);
        return;
    }
    if (at == workspace_index()) {
        reply_error(fd, "session is in view", id);
        return;
    }
    if (session_turn_running(s) || workspace_queued(at)) {
        reply_error(fd, "session is busy", id);
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    workspace_close(at);
    reply(fd, json ? json : "{\"ok\":true}");
    free(json);
    cJSON_Delete(r);
}

static struct {
    char   pair[200];
    double at;
} delivered[PAIR_SLOTS];

static int pair_allowed(const char *from, const char *to)
{
    char pair[200];
    snprintf(pair, sizeof pair, "%s>%s", from, to);
    double now = now_seconds();
    int    seen = 0, slot = 0;
    for (int i = 0; i < PAIR_SLOTS; i++) {
        if (now - delivered[i].at < PAIR_WINDOW && !strcmp(delivered[i].pair, pair))
            seen++;
        if (delivered[i].at < delivered[slot].at)
            slot = i;
    }
    if (seen >= PAIR_CAP)
        return 0;
    snprintf(delivered[slot].pair, sizeof delivered[slot].pair, "%s", pair);
    delivered[slot].at = now;
    return 1;
}

static int deliver(int at, const char *line, const char *shown)
{
    if (!session_turn_running(workspace_at(at)))
        workspace_render(at, echo_prompt, (void *)shown);
    return workspace_send(at, line, shown);
}

static void send_session(int fd, const cJSON *o, const cJSON *send)
{
    const char *line = cJSON_GetStringValue(send);
    cJSON *target = cJSON_GetObjectItem((cJSON *)o, "session");
    const char *id = cJSON_GetStringValue(target), *name = field(o, "name");
    int at = index_of_id(target);
    /* A session has no id until its first turn; address it by name. */
    for (int i = 0; at < 0 && (!id || !*id) && name && i < workspace_count(); i++) {
        const char *mine = session_name(workspace_at(i));
        if (mine && !strcmp(mine, name))
            at = i;
    }
    if ((!id || !*id) && !name) {
        reply_error(fd, "send takes a session id or name", NULL);
        return;
    }
    if (!id || !*id)
        id = name;
    if (!workspace_at(at)) {
        reply_error(fd, "no such session", id);
        return;
    }
    if (!line || !*line) {
        reply_error(fd, "bad line", id);
        return;
    }

    const char *sender = field(o, "from"), *host = field(o, "host");
    char        from[200] = "";
    if (sender)
        snprintf(from, sizeof from, "%s%s@%s", host ? host : "", host ? ":" : "", sender);
    if (sender && !pair_allowed(from, id)) {
        reply_error(fd, "too many messages to this session in the last minute", id);
        return;
    }
    char *framed = sender ? text_dsprintf("[from %s] %s", from, line) : NULL;
    char *shown = sender ? text_dsprintf("from %s: %s", from, line) : NULL;
    int   sent = sender ? framed && shown && deliver(at, framed, shown) : dispatch_send(at, line);
    free(framed);
    free(shown);
    if (!sent)
        reply_error(fd, "could not send line", id);
    else {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddStringToObject(r, "session", id);
        char *json = cJSON_PrintUnformatted(r);
        reply(fd, json ? json : "{\"ok\":true}");
        free(json);
        cJSON_Delete(r);
    }
}

struct pending {
    int             fd;
    struct session *s;
    struct timespec since;
};

static struct pending pendings[WORKSPACE_MAX];

static long since_ms(const struct timespec *then, const struct timespec *now)
{
    return (now->tv_sec - then->tv_sec) * 1000 + (now->tv_nsec - then->tv_nsec) / 1000000;
}

static void reply_spawn(int fd, struct session *s)
{
    const char *id = session_id(s), *addr = session_addr(s), *name = session_name(s);
    cJSON      *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "session", id);
    if (name && *name)
        cJSON_AddStringToObject(r, "name", name);
    if (addr && *addr)
        cJSON_AddStringToObject(r, "addr", addr);
    char *json = cJSON_PrintUnformatted(r);
    reply(fd, json ? json : "{\"error\": \"oom\"}");
    free(json);
    cJSON_Delete(r);
}

static void hold_spawn(int fd, struct session *s)
{
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (pendings[i].s)
            continue;
        pendings[i].fd = fd;
        pendings[i].s = s;
        clock_gettime(CLOCK_MONOTONIC, &pendings[i].since);
        return;
    }
    reply_error(fd, "session id unavailable", NULL);
}

static void settle_pending(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!pendings[i].s)
            continue;
        int         at = workspace_index_of(pendings[i].s);
        const char *id = at >= 0 ? session_id(workspace_at(at)) : NULL;
        if (id)
            reply_spawn(pendings[i].fd, workspace_at(at));
        else if (at < 0)
            reply_error(pendings[i].fd, "session ended before it reported an id", NULL);
        else if (since_ms(&pendings[i].since, &now) < ID_WAIT_MS)
            continue;
        else
            reply_error(pendings[i].fd, "session id unavailable", NULL);
        pendings[i].fd = -1;
        pendings[i].s = NULL;
    }
}

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *resume, const char *const *env,
                   const char *prompt)
{
    struct session *was = workspace_current();
    char here[4096];
    if (!cwd && getcwd(here, sizeof here))
        cwd = here;
    int at = workspace_spawn_env(backend, model, effort, cwd, resume, env);
    if (at < 0)
        return -1;

    if (title) {
        char name[81];
        struct session *s = workspace_at(at);
        snprintf(name, sizeof name, "%s", title);
        if (session_rename(s, name) == SESSION_RENAME_OK)
            session_set_naming(s, 0);
    }

    workspace_show(workspace_index_of(was));
    at = workspace_index_of(workspace_at(at));
    if (prompt)
        dispatch_send(at, prompt);
    return at;
}

int dispatch_send(int at, const char *line)
{
    return deliver(at, line, line);
}

static struct dispatch_net net;

static void serve(int fd, const char *text, const char *host)
{
    cJSON *o = cJSON_Parse(text);
    if (!o) {
        reply(fd, "{\"error\": \"bad json\"}");
        return;
    }
    if (host) {
        cJSON_DeleteItemFromObject(o, "host");
        cJSON_AddStringToObject(o, "host", host);
    }

    int   kept = 0;
    char *answer = net.serve ? net.serve(o, fd, &kept) : NULL;
    if (kept) {
        cJSON_Delete(o);
        return;
    }
    if (answer) {
        reply(fd, answer);
        free(answer);
        cJSON_Delete(o);
        return;
    }

    cJSON *target = cJSON_GetObjectItem(o, "close");
    if (target) {
        close_session(fd, target);
        cJSON_Delete(o);
        return;
    }

    cJSON *send = cJSON_GetObjectItem(o, "send");
    if (send) {
        send_session(fd, o, send);
        cJSON_Delete(o);
        return;
    }

    const char *backend = field(o, "backend");
    if (!backend)
        backend = cmd_default_backend();

    const char *cwd = field(o, "cwd"), *home = getenv("HOME");
    char        expanded[4096];
    if (cwd && cwd[0] == '~' && (cwd[1] == '\0' || cwd[1] == '/') && home) {
        snprintf(expanded, sizeof expanded, "%s%s", home, cwd + 1);
        cwd = expanded;
    }
    int at = dispatch_spawn(backend, field(o, "model"), field(o, "effort"), cwd,
                            field(o, "title"), field(o, "resume"), NULL, field(o, "prompt"));
    if (at < 0) {
        char out[300];
        snprintf(out, sizeof out, "{\"error\": \"could not start the %s CLI\"}", backend);
        reply(fd, out);
        cJSON_Delete(o);
        return;
    }

    if (session_id(workspace_at(at)))
        reply_spawn(fd, workspace_at(at));
    else
        hold_spawn(fd, workspace_at(at));
    cJSON_Delete(o);
}

#define NET_RETRY_S 5

static int  listen_fd = -1, net_fd = -1, dir_fd = -1, net_port;
static char listen_path[200];

static struct client {
    int    fd;
    char  *buf;
    size_t len;
    char   peer[64];
} clients[CLIENT_MAX];

static void unlisten(void)
{
    if (listen_fd >= 0)
        unlink(listen_path);
}

static int nonblocking(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void listen_once(void)
{
    static long tried;
    if (listen_fd >= 0 || tried == (long)getpid())
        return;
    tried = (long)getpid();
    for (int i = 0; i < CLIENT_MAX; i++)
        clients[i].fd = -1;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (!dispatch_socket_path((long)getpid(), sa.sun_path, sizeof sa.sun_path))
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    unlink(sa.sun_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, CLIENT_MAX) != 0) {
        close(fd);
        return;
    }
    chmod(sa.sun_path, 0600);
    nonblocking(fd);
    listen_fd = fd;
    snprintf(listen_path, sizeof listen_path, "%s", sa.sun_path);
    atexit(unlisten);
}

static int listen_tcp(const char *ip, int port, int *bound)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    socklen_t len = sizeof sa;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, CLIENT_MAX) != 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
        close(fd);
        return -1;
    }
    nonblocking(fd);
    if (bound)
        *bound = ntohs(sa.sin_port);
    return fd;
}

static void listen_net(void)
{
    static struct timespec last;
    struct timespec        now;
    if (!net.bind || (net_fd >= 0 && dir_fd >= 0))
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec && now.tv_sec - last.tv_sec < NET_RETRY_S)
        return;
    last = now;
    const char *ip = net.bind();
    if (!ip)
        return;
    if (net_fd < 0)
        net_fd = listen_tcp(ip, 0, &net_port);
    if (dir_fd < 0 && net.port > 0)
        dir_fd = listen_tcp(ip, net.port, NULL);
}

void dispatch_net(const struct dispatch_net *config)
{
    net = *config;
}

int dispatch_serving(void)
{
    return net.bind != NULL;
}

int dispatch_net_port(void)
{
    return net_fd >= 0 ? net_port : 0;
}

int dispatch_fds(int *out, int max)
{
    listen_once();
    int n = 0;
    int listeners[] = {listen_fd, net_fd, dir_fd};
    for (int i = 0; i < 3 && n < max; i++)
        if (listeners[i] >= 0)
            out[n++] = listeners[i];
    for (int i = 0; i < CLIENT_MAX && n < max; i++)
        if (clients[i].fd >= 0)
            out[n++] = clients[i].fd;
    return n;
}

static void drop(struct client *c)
{
    free(c->buf);
    c->buf = NULL;
    c->len = 0;
    c->fd = -1;
    c->peer[0] = '\0';
}

static void finish(struct client *c, int answer)
{
    char *text = c->buf;
    int   fd = c->fd;
    char  peer[64], host[256] = "";
    snprintf(peer, sizeof peer, "%s", c->peer);
    c->buf = NULL;
    drop(c);
    if (answer && peer[0] && (!net.peer || !net.peer(peer, host, sizeof host)))
        reply(fd, "{\"error\": \"not the same tailscale user\"}");
    else if (answer && text && *text)
        serve(fd, text, peer[0] ? host : NULL);
    else if (answer)
        reply(fd, "{\"error\": \"empty request\"}");
    else
        close(fd);
    free(text);
}

static void take(struct client *c)
{
    char chunk[4096];
    for (;;) {
        ssize_t n = recv(c->fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            finish(c, n == 0 && c->len);
            return;
        }
        char *grown = c->len + (size_t)n <= REQUEST_MAX ? realloc(c->buf, c->len + (size_t)n + 1)
                                                         : NULL;
        if (!grown) {
            int fd = c->fd;
            drop(c);
            reply(fd, "{\"error\": \"request too large\"}");
            return;
        }
        c->buf = grown;
        memcpy(c->buf + c->len, chunk, (size_t)n);
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        char *nl = strchr(c->buf, '\n');
        if (nl) {
            *nl = '\0';
            finish(c, 1);
            return;
        }
    }
}

static void accept_on(int lfd, int tcp)
{
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t               len = sizeof sa;
        int                     fd = accept(lfd, (struct sockaddr *)&sa, &len);
        if (fd < 0)
            break;
        struct client *slot = NULL;
        for (int i = 0; i < CLIENT_MAX && !slot; i++)
            if (clients[i].fd < 0)
                slot = &clients[i];
        if (!slot) {
            close(fd);
            continue;
        }
        nonblocking(fd);
        slot->fd = fd;
        slot->peer[0] = '\0';
        if (tcp && sa.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, slot->peer,
                      sizeof slot->peer);
        else if (tcp)
            snprintf(slot->peer, sizeof slot->peer, "?");
    }
}

void dispatch_poll(void)
{
    listen_once();
    listen_net();
    settle_pending();
    if (listen_fd >= 0)
        accept_on(listen_fd, 0);
    if (net_fd >= 0)
        accept_on(net_fd, 1);
    if (dir_fd >= 0)
        accept_on(dir_fd, 1);
    for (int i = 0; i < CLIENT_MAX; i++)
        if (clients[i].fd >= 0)
            take(&clients[i]);
}

int dispatch_request(long pid, const char *json, char *out, size_t size, int wait_s)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (!dispatch_socket_path(pid, sa.sun_path, sizeof sa.sun_path)) {
        snprintf(out, size, "{\"error\":\"no dispatch directory\"}");
        return 0;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (fd >= 0)
            close(fd);
        snprintf(out, size, "{\"error\":\"scrap %ld is not running\"}", pid);
        return 0;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t len = strlen(json);
    int    ok = send(fd, json, len, 0) == (ssize_t)len && send(fd, "\n", 1, 0) == 1;
    size_t got = 0;
    out[0] = '\0';
    for (int waited = 0; ok && waited < wait_s * 1000;) {
        if (pid == (long)getpid())
            dispatch_poll();
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 100);
        waited += 100;
        if (r <= 0)
            continue;
        ssize_t n = recv(fd, out + got, size - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        out[got] = '\0';
        if (memchr(out, '\n', got) || got == size - 1)
            break;
    }
    close(fd);
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
    if (!out[0]) {
        snprintf(out, size, "{\"error\":\"no answer from scrap %ld\"}", pid);
        return 0;
    }
    return 1;
}
