#include "child.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_SLOTS 6
#define OUTPUT_MAX  (1u << 18)

struct slot {
    pid_t  pid;
    char   key[CHILD_KEY_MAX];
    int    fd;
    char  *buf;
    size_t len, cap;
    int    done;
    int    ok;
    int    truncated;
};

static struct slot slots[CHILD_SLOTS];

static void slot_free(struct slot *s)
{
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

int child_running(const char *key)
{
    if (!key)
        return 0;
    for (int i = 0; i < CHILD_SLOTS; i++)
        if (slots[i].pid && !slots[i].done && !strcmp(slots[i].key, key))
            return 1;
    return 0;
}

static struct slot *free_slot(const char *key)
{
    for (int i = 0; i < CHILD_SLOTS; i++)
        if (slots[i].pid && !strcmp(slots[i].key, key))
            return NULL;
    for (int i = 0; i < CHILD_SLOTS; i++)
        if (!slots[i].pid)
            return &slots[i];
    return NULL;
}

static int spawn(struct slot *s, const char *key, char *const argv[],
                 const char *cwd)
{
    int pipes[2];
    if (pipe(pipes) != 0)
        return 0;
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipes[0]);

        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        if (pipes[1] > STDERR_FILENO)
            close(pipes[1]);
        fcntl(STDOUT_FILENO, F_SETFD, 0);
        fcntl(STDERR_FILENO, F_SETFD, 0);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null != STDIN_FILENO)
                close(null);
        }
        if (cwd && *cwd && chdir(cwd) != 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipes[1]);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    memset(s, 0, sizeof *s);
    s->pid = pid;
    s->fd = pipes[0];
    snprintf(s->key, sizeof s->key, "%s", key);
    return 1;
}

int child_start(const char *key, char *const argv[], const char *cwd)
{
    if (!key || !*key || !argv || !argv[0])
        return 0;
    struct slot *s = free_slot(key);
    if (!s)
        return 0;
    return spawn(s, key, argv, cwd);
}

/* Past the cap the output is discarded rather than left in the pipe: a child
   that keeps writing would otherwise block forever and never be reaped. */
static int discard(struct slot *s)
{
    char    sink[4096];
    ssize_t k = read(s->fd, sink, sizeof sink);
    if (k <= 0)
        return 0;
    s->truncated = 1;
    return 1;
}

static void drain(struct slot *s)
{
    for (;;) {
        if (s->len + 4096 > s->cap) {
            if (s->cap >= OUTPUT_MAX) {
                if (!discard(s))
                    return;
                continue;
            }
            size_t cap = s->cap ? s->cap * 2 : 8192;
            char  *grown = realloc(s->buf, cap);
            if (!grown) {
                if (!discard(s))
                    return;
                continue;
            }
            s->buf = grown;
            s->cap = cap;
        }
        ssize_t k = read(s->fd, s->buf + s->len, s->cap - s->len - 1);
        if (k <= 0)
            return;
        s->len += (size_t)k;
        s->buf[s->len] = '\0';
    }
}

static void note_truncation(struct slot *s)
{
    static const char note[] = "\n[output truncated]\n";
    if (!s->truncated || !s->buf)
        return;
    char *grown = realloc(s->buf, s->len + sizeof note);
    if (!grown)
        return;
    memcpy(grown + s->len, note, sizeof note);
    s->buf = grown;
    s->len += sizeof note - 1;
    s->cap = s->len + 1;
    s->truncated = 0;
}

int child_reap(char *key, size_t keysize, char **out, int *ok)
{
    for (int i = 0; i < CHILD_SLOTS; i++) {
        struct slot *s = &slots[i];
        if (!s->pid || s->done)
            continue;

        drain(s);

        int   status = 0;
        pid_t went = waitpid(s->pid, &status, WNOHANG);
        if (went != s->pid)
            continue;

        drain(s);
        note_truncation(s);
        s->done = 1;
        s->ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

        if (key)
            snprintf(key, keysize, "%s", s->key);
        if (ok)
            *ok = s->ok;
        if (out) {
            *out = s->buf;
            s->buf = NULL;
            s->len = s->cap = 0;
        }
        slot_free(s);
        return 1;
    }
    return 0;
}

int child_stop(const char *key)
{
    if (!key || !*key)
        return 0;
    for (int i = 0; i < CHILD_SLOTS; i++) {
        if (!slots[i].pid || strcmp(slots[i].key, key))
            continue;
        if (!slots[i].done) {
            kill(slots[i].pid, SIGTERM);
            waitpid(slots[i].pid, NULL, 0);
        }
        slot_free(&slots[i]);
        return 1;
    }
    return 0;
}

void child_close_all(void)
{
    for (int i = 0; i < CHILD_SLOTS; i++) {
        if (!slots[i].pid)
            continue;
        if (!slots[i].done) {
            kill(slots[i].pid, SIGTERM);
            waitpid(slots[i].pid, NULL, 0);
        }
        slot_free(&slots[i]);
    }
}
