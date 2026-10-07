#include "dispatch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

#define PAIR_WINDOW   60
#define PAIR_CAP      6
#define INTERRUPT_CAP 2
#define PAIR_SLOTS    64

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

static int index_of_target(const cJSON *target)
{
    int at = index_of_id(target);
    if (at >= 0)
        return at;
    const char *t = target->valuestring, *name = t + (t[0] == '@');
    size_t      n = strlen(t);
    int         prefix = -1, titled = -1;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        const char     *mine = session_name(s), *remote = session_remote(s);
        const char     *id = session_id(s), *title = session_title(s);
        if ((mine && !strcmp(mine, name)) || (remote && !strcmp(remote, t)))
            return i;
        if (t[0] == '@')
            continue;
        if (prefix < 0 && id && !strncmp(id, t, n))
            prefix = i;
        if (titled < 0 && title && strcasestr(title, t))
            titled = i;
    }
    return prefix >= 0 ? prefix : titled;
}

static void close_session(int fd, const cJSON *target)
{
    if (!cJSON_IsString(target) || !target->valuestring || !*target->valuestring) {
        reply_error(fd, "close takes a session", NULL);
        return;
    }
    const char *id = target->valuestring;
    int at = index_of_target(target);
    struct session *s = workspace_at(at);
    if (!s) {
        reply_error(fd, "no such session", id);
        return;
    }
    if (session_turn_running(s))
        session_interrupt(s);
    int last = workspace_count() == 1;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    if (!last)
        workspace_close(at);
    reply(fd, json ? json : "{\"ok\":true}");
    free(json);
    cJSON_Delete(r);
    if (last)
        raise(SIGTERM);
}

static struct {
    char   pair[200];
    double at;
} delivered[PAIR_SLOTS];

static int pair_allowed(const char *from, const char *to, const char *kind, int cap)
{
    char pair[200];
    snprintf(pair, sizeof pair, "%s>%s%s", from, to, kind);
    double now = now_seconds();
    int    seen = 0, slot = 0;
    for (int i = 0; i < PAIR_SLOTS; i++) {
        if (now - delivered[i].at < PAIR_WINDOW && !strcmp(delivered[i].pair, pair))
            seen++;
        if (delivered[i].at < delivered[slot].at)
            slot = i;
    }
    if (seen >= cap)
        return 0;
    snprintf(delivered[slot].pair, sizeof delivered[slot].pair, "%s", pair);
    delivered[slot].at = now;
    return 1;
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
    if (sender && !pair_allowed(from, id, "", PAIR_CAP)) {
        reply_error(fd, "too many messages to this session in the last minute", id);
        return;
    }
    /* A session sending a command to itself runs it, e.g. /clear once its turn ends. */
    const char *from_id = field(o, "from_id"), *own = session_id(workspace_at(at));
    if (!host && from_id && own && !strcmp(from_id, own) && cmd_is_command(line))
        sender = NULL;
    /* Interrupts past the cap queue instead, so two sessions cannot keep stopping each other. */
    int interrupt = sender && cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)o, "interrupt")) &&
                    pair_allowed(from, id, "!", INTERRUPT_CAP);
    int sent = sender ? workspace_message(at, from, line, interrupt,
                                          cJSON_IsTrue(cJSON_GetObjectItem((cJSON *)o, "reply")))
                      : dispatch_send(at, line);
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
        session_rename(s, name);
    }
    if (resume)
        workspace_replay(at);
    if (prompt)
        dispatch_send(at, prompt);
    return at;
}

int dispatch_send(int at, const char *line)
{
    if (!session_turn_running(workspace_at(at)))
        workspace_render(at, echo_prompt, (void *)line);
    return workspace_send(at, line, line);
}

static char *(*serve_extra)(const cJSON *o, int fd, int *kept);

void dispatch_hand_off(int fd, long pid, const cJSON *o)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    char              *json = cJSON_PrintUnformatted(o);
    char              *line = json ? text_dsprintf("%s\n", json) : NULL;
    int u = line && dispatch_socket_path(pid, sa.sun_path, sizeof sa.sun_path)
                ? socket(AF_UNIX, SOCK_STREAM, 0)
                : -1;
    size_t len = line ? strlen(line) : 0;
    char   ctl[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec   iov = {.iov_base = line, .iov_len = len};
    struct msghdr  m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl,
                        .msg_controllen = sizeof ctl};
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    int ok = u >= 0 && connect(u, (struct sockaddr *)&sa, sizeof sa) == 0 &&
             sendmsg(u, &m, 0) == (ssize_t)len;
    if (u >= 0)
        close(u);
    free(line);
    free(json);
    if (ok) {
        close(fd);
        return;
    }
    char why[80];
    snprintf(why, sizeof why, "scrap %ld is not running", pid);
    reply_error(fd, why, NULL);
}

static void serve(int fd, const char *text)
{
    cJSON *o = cJSON_Parse(text);
    if (!o) {
        reply(fd, "{\"error\": \"bad json\"}");
        return;
    }

    int   kept = 0;
    char *answer = serve_extra ? serve_extra(o, fd, &kept) : NULL;
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

    const char *tab = cJSON_GetStringValue(cJSON_GetObjectItem(o, "tab"));
    if (tab) {
        char why[600] = "";
        if (cmd_attach_tab(tab, why, sizeof why) < 0)
            reply_error(fd, why[0] ? why : "could not attach", NULL);
        else
            reply(fd, "{\"ok\": true}");
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
    if (field(o, "name"))
        session_set_name(workspace_at(at), field(o, "name"));

    if (session_id(workspace_at(at)))
        reply_spawn(fd, workspace_at(at));
    else
        hold_spawn(fd, workspace_at(at));
    cJSON_Delete(o);
}

static int  listen_fd = -1;
static char listen_path[200];

static struct client {
    int    fd;
    int    passed;
    char  *buf;
    size_t len;
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
        clients[i].fd = clients[i].passed = -1;
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

void dispatch_serve_with(char *(*serve)(const cJSON *o, int fd, int *kept))
{
    serve_extra = serve;
}

int dispatch_fds(int *out, int max)
{
    listen_once();
    int n = 0;
    if (listen_fd >= 0 && n < max)
        out[n++] = listen_fd;
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
    c->passed = -1;
}

static void finish(struct client *c, int answer)
{
    char *text = c->buf;
    int   fd = c->fd, passed = c->passed;
    c->buf = NULL;
    drop(c);
    if (passed >= 0) {
        close(fd);
        fd = passed;
    }
    if (answer && text && *text)
        serve(fd, text);
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
        char           ctl[CMSG_SPACE(sizeof(int))];
        struct iovec   iov = {.iov_base = chunk, .iov_len = sizeof chunk};
        struct msghdr  m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl,
                            .msg_controllen = sizeof ctl};
        ssize_t        n = recvmsg(c->fd, &m, 0);
        for (struct cmsghdr *h = n > 0 ? CMSG_FIRSTHDR(&m) : NULL; h; h = CMSG_NXTHDR(&m, h)) {
            int got;
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS)
                continue;
            memcpy(&got, CMSG_DATA(h), sizeof got);
            if (c->passed >= 0)
                close(got);
            else {
                fcntl(got, F_SETFD, FD_CLOEXEC);
                c->passed = got;
            }
        }
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
            if (c->passed >= 0)
                close(c->passed);
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

static void accept_on(int lfd)
{
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
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
    }
}

void dispatch_poll(void)
{
    listen_once();
    settle_pending();
    if (listen_fd >= 0)
        accept_on(listen_fd);
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
