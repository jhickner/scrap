#include "hub.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "dispatch.h"
#include "handoff.h"
#include "intercom.h"
#include "job.h"
#include "livelist.h"
#include "proxyproto.h"
#include "tailnet.h"
#include "title.h"
#include "vendor/cJSON.h"

#define REQUEST_MAX 65536
#define READ_TIMEOUT_S 10
#define CHECK_S 5
#define BACKLOG 16

static int file_in_dispatch(const char *leaf, char *out, size_t size)
{
    char dir[4200];
    return dispatch_dir(dir, sizeof dir) && (size_t)snprintf(out, size, "%s/%s", dir, leaf) < size;
}

int hub_self_path(char *out, size_t size)
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

void hub_ensure(void)
{
    char lock[4400], log[4400], exe[PATH_MAX];
    if (!file_in_dispatch("hub.lock", lock, sizeof lock) ||
        !file_in_dispatch("hub.log", log, sizeof log) || !hub_self_path(exe, sizeof exe))
        return;
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    if (held)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        int in = open("/dev/null", O_RDONLY), out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        dup2(in, 0);
        dup2(out >= 0 ? out : in, 1);
        dup2(out >= 0 ? out : in, 2);
        for (int i = 3; i < 1024; i++)
            close(i);
        execl(exe, exe, "hub", (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
}

static void answer(int fd, cJSON *r)
{
    char *json = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (json) {
        size_t len = strlen(json);
        if (send(fd, json, len, 0) == (ssize_t)len)
            send(fd, "\n", 1, 0);
        free(json);
    }
    close(fd);
}

static void fail(int fd, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    answer(fd, r);
}

static char *request_line(int fd)
{
    char  *buf = malloc(REQUEST_MAX + 1);
    size_t len = 0;
    while (buf && len < REQUEST_MAX) {
        ssize_t n = recv(fd, buf + len, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (buf[len] == '\n') {
            buf[len] = '\0';
            return buf;
        }
        len++;
    }
    free(buf);
    return NULL;
}

static int recv_all(int fd, unsigned char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        got += (size_t)r;
    }
    return 1;
}

static int proxy_peer(int fd, char *ip, size_t size)
{
    unsigned char hdr[PROXYPROTO_HEAD + 216];
    if (!recv_all(fd, hdr, PROXYPROTO_HEAD))
        return 0;
    size_t n = proxyproto_len(hdr);
    return n && n <= sizeof hdr && recv_all(fd, hdr + PROXYPROTO_HEAD, n - PROXYPROTO_HEAD) &&
           proxyproto_source(hdr, n, ip, size);
}

static long newest_scrap(void)
{
    struct live_session *v = NULL;
    int                  n = livelist_load(&v);
    long                 pid = 0, ts = -1;
    for (int i = 0; i < n; i++)
        if (v[i].ts > ts) {
            ts = v[i].ts;
            pid = v[i].pid;
        }
    free(v);
    return pid;
}

static pthread_mutex_t serve_lock = PTHREAD_MUTEX_INITIALIZER;

struct conn {
    int  fd;
    char ip[INET6_ADDRSTRLEN];
};

static void rename_session(int fd, cJSON *o)
{
    const char *renamed = cJSON_GetStringValue(cJSON_GetObjectItem(o, "rename"));
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(o, "title"));
    char        msg[600] = "";
    cJSON_DeleteItemFromObject(o, "to");
    cJSON_AddStringToObject(o, "to", renamed ? renamed : "");
    long        pid = intercom_owner(o, msg, sizeof msg);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "session"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
    char        at[200];
    if (pid > 0 && (!id || !*id) && name && *name) {
        snprintf(at, sizeof at, "@%s", name);
        id = at;
    }
    if (pid <= 0 || !id || !*id) {
        fail(fd, msg[0] ? msg : "no such session");
        return;
    }
    if (!title || !title_set(id, title)) {
        fail(fd, "bad name");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    answer(fd, r);
}

static void route(int fd, cJSON *o)
{
    char msg[600] = "";
    if (cJSON_GetObjectItem(o, "rename"))
        rename_session(fd, o);
    else if (cJSON_GetObjectItem(o, "to")) {
        long pid = intercom_owner(o, msg, sizeof msg);
        if (pid > 0)
            dispatch_hand_off(fd, pid, o);
        else
            fail(fd, msg);
    } else if (cJSON_GetObjectItem(o, "ls") || cJSON_GetObjectItem(o, "read")) {
        char *out = intercom_serve(o);
        cJSON *r = out ? cJSON_Parse(out) : NULL;
        free(out);
        if (r)
            answer(fd, r);
        else
            fail(fd, "bad request");
    } else if (cJSON_GetObjectItem(o, "send") || cJSON_GetObjectItem(o, "attach"))
        fail(fd, "send and attach take to");
    else {
        long pid = newest_scrap();
        if (pid > 0)
            dispatch_hand_off(fd, pid, o);
        else
            fail(fd, "no scrap running");
    }
}

static void *serve(void *ud)
{
    struct conn   *c = ud;
    int            fd = c->fd;
    struct timeval limit = {.tv_sec = READ_TIMEOUT_S}, none = {0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    char *line = !tailnet_serve() || proxy_peer(fd, c->ip, sizeof c->ip) ? request_line(fd) : NULL;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    char   host[256];
    cJSON *o = line ? cJSON_Parse(line) : NULL;
    free(line);
    if (!o)
        close(fd);
    else if (!tailnet_peer(c->ip, host, sizeof host))
        fail(fd, "not the same tailscale user");
    else {
        cJSON_DeleteItemFromObject(o, "host");
        cJSON_AddStringToObject(o, "host", host);
        pthread_mutex_lock(&serve_lock);
        route(fd, o);
        pthread_mutex_unlock(&serve_lock);
    }
    cJSON_Delete(o);
    free(c);
    return NULL;
}

static int listen_on(const char *ip, int port)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    int                on = 1, fd;
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1 || (fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, BACKLOG) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static volatile sig_atomic_t reload;

static void on_reload(int sig)
{
    (void)sig;
    reload = 1;
}

int hub_main(int argc, char **argv)
{
    char lock[4400], exe[PATH_MAX];
    if (!file_in_dispatch("hub.lock", lock, sizeof lock) || !hub_self_path(exe, sizeof exe))
        return 1;
    int lfd = -1, lock_fd;
    if (argc == 3 && !strcmp(argv[1], "--lock"))
        lock_fd = atoi(argv[2]);
    else if (argc == 1) {
        lock_fd = open(lock, O_RDWR | O_CREAT, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
            return 0;
    } else {
        fprintf(stderr, "usage: scrap hub\n");
        return 2;
    }
    struct stat held, now;
    if (fstat(lock_fd, &held) != 0)
        return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_reload};
    sigaction(SIGURG, &sa, NULL);
    int  port = tailnet_dir_port();
    char bound[64] = "";
    tailnet_self_name();
    for (;;) {
        job_tick(exe, time(NULL));
        if (reload) {
            char fdarg[16];
            snprintf(fdarg, sizeof fdarg, "%d", lock_fd);
            if (lfd >= 0)
                close(lfd);
            execl(exe, exe, "hub", "--lock", fdarg, (char *)NULL);
            return 1;
        }
        if (stat(lock, &now) != 0 || now.st_ino != held.st_ino || now.st_dev != held.st_dev)
            return 0;
        const char *ip = !tailnet_broker() ? NULL : tailnet_serve() ? "127.0.0.1" : tailnet_bind_ip();
        if (lfd >= 0 && (!ip || strcmp(ip, bound))) {
            printf("scrap hub: %s went away\n", bound);
            close(lfd);
            lfd = -1;
        }
        if (lfd < 0 && ip) {
            lfd = listen_on(ip, port);
            if (lfd >= 0) {
                snprintf(bound, sizeof bound, "%s", ip);
                printf("scrap hub: %s on %s:%d\n", tailnet_self_name(), bound, port);
                if (tailnet_serve() && !tailnet_serve_forward(port))
                    printf("scrap hub: tailscale serve --tcp %d failed\n", port);
            } else
                printf("scrap hub: cannot listen on %s:%d: %s\n", ip, port, strerror(errno));
        }
        if (lfd < 0) {
            sleep(CHECK_S);
            continue;
        }
        struct pollfd p = {.fd = lfd, .events = POLLIN};
        if (poll(&p, 1, CHECK_S * 1000) != 1)
            continue;
        struct sockaddr_in from;
        socklen_t          len = sizeof from;
        int                fd = accept(lfd, (struct sockaddr *)&from, &len);
        struct conn       *c = fd >= 0 ? calloc(1, sizeof *c) : NULL;
        pthread_t          t;
        if (!c) {
            if (fd >= 0)
                close(fd);
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        c->fd = fd;
        inet_ntop(AF_INET, &from.sin_addr, c->ip, sizeof c->ip);
        if (pthread_create(&t, NULL, serve, c) == 0)
            pthread_detach(t);
        else {
            close(fd);
            free(c);
        }
    }
}
