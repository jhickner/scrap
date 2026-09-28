#include "restart.h"

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "hud.h"
#include "session.h"
#include "sessionfork.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "voice.h"
#include "workspace.h"

#define RESTART_SIGNAL SIGURG

static volatile sig_atomic_t wanted;

static void on_signal(int sig)
{
    (void)sig;
    wanted = 1;
    tty_wake();
}

void restart_arm(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);

    sigaction(RESTART_SIGNAL, &sa, NULL);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, RESTART_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

void restart_shield_thread(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, RESTART_SIGNAL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

#define RESTART_FLAGS 4
static const char *extra[RESTART_FLAGS];
static int         extra_n;

void restart_flag(const char *flag)
{
    for (int i = 0; i < extra_n; i++)
        if (!strcmp(extra[i], flag))
            return;
    if (extra_n < RESTART_FLAGS)
        extra[extra_n++] = flag;
}

void restart_unflag(const char *flag)
{
    for (int i = 0; i < extra_n; i++) {
        if (strcmp(extra[i], flag))
            continue;
        for (int j = i + 1; j < extra_n; j++)
            extra[j - 1] = extra[j];
        extra_n--;
        return;
    }
}

void restart_request(void)
{
    wanted = 1;
}

int restart_wanted(void)
{
    return wanted != 0;
}

void restart_clear(void)
{
    wanted = 0;
}

static char  pool[8192];
static size_t pool_used;

static char *arg_copy(const char *s)
{
    size_t n = strlen(s) + 1;
    if (pool_used + n > sizeof pool)
        return NULL;
    char *out = pool + pool_used;
    memcpy(out, s, n);
    pool_used += n;
    return out;
}

static int tmp_path(char *out, size_t n, const char *what, int index)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        len--;
    if (index < 0)
        return snprintf(out, n, "%.*s/" APP_NAME "-%s-%ld", (int)len, tmp, what,
                        (long)getpid()) < (int)n;
    return snprintf(out, n, "%.*s/" APP_NAME "-%s-%ld-%d", (int)len, tmp, what,
                    (long)getpid(), index) < (int)n;
}

static int dump_path(char *out, size_t n)
{
    return tmp_path(out, n, "restore", -1);
}

static int path_lookup(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        return 0;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t      len = end ? (size_t)(end - path) : strlen(path);
        if (len > 0 && len < size &&
            snprintf(out, size, "%.*s/%s", (int)len, path, name) < (int)size &&
            access(out, X_OK) == 0)
            return 1;
        if (!end)
            break;
        path = end + 1;
    }
    return 0;
}

static int tabs_path(char *out, size_t n)
{
    return tmp_path(out, n, "tabs", -1);
}

static int tabs_dump(const struct session *front, const char *path)
{
    int wrote = 0;
    FILE *f = NULL;

    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        const char *id = session_id(s);

        if (s == front || (!session_remote(s) && (!id || !*id || !session_can_resume(s))))
            continue;
        if (!f && !(f = fopen(path, "w")))
            return 0;

        char screen[4096];
        if (!tmp_path(screen, sizeof screen, "tab", i) || !workspace_dump(i, screen))
            screen[0] = '\0';

        char *args[SESSION_ARGV_MAX];
        int   n = session_argv(s, args, COUNT(args),
                               SESSION_ARGV_CWD | SESSION_ARGV_RESUME);
        fputs(screen, f);
        for (int a = 0; a < n; a++)
            fprintf(f, "\t%s", args[a]);
        fputc('\n', f);
        wrote = 1;
    }
    if (f && fclose(f) != 0)
        wrote = 0;
    if (!wrote)
        unlink(path);
    return wrote;
}

int restart_exec(struct session *s)
{
    wanted = 0;
    pool_used = 0;

    char *argv[28];
    int   n = 0;
    argv[n++] = (char *)sessionfork_program();
    n += session_argv(s, argv + n, SESSION_ARGV_MAX,
                      SESSION_ARGV_CWD | SESSION_ARGV_RESUME | SESSION_ARGV_SAFE);

    for (int i = 0; i < extra_n; i++)
        argv[n++] = (char *)extra[i];

    for (int i = 0; i < n; i++)
        if (!(argv[i] = arg_copy(argv[i])))
            return 0;
    argv[n] = NULL;

    if (strchr(argv[0], '/') && access(argv[0], X_OK) != 0) {
        char found[4096];
        if (!path_lookup(APP_NAME, found, sizeof found))
            return 0;
        char *arg = arg_copy(found);
        if (!arg)
            return 0;
        argv[0] = arg;
    }

    hud_restarted();

    char tabs[4096];
    if (tabs_path(tabs, sizeof tabs) && tabs_dump(s, tabs)) {
        char *arg = arg_copy(tabs);
        if (arg) {
            argv[n++] = "--tabs";
            argv[n++] = arg;
            argv[n] = NULL;
        } else {
            unlink(tabs);
        }
    }

    char path[4096];
    int carried = dump_path(path, sizeof path) && viewport_dump(path);
    if (carried) {
        char *arg = arg_copy(path);
        if (arg) {
            argv[n++] = "--restore";
            argv[n++] = arg;
            argv[n] = NULL;
        } else {
            unlink(path);
            carried = 0;
        }
    }

    if (workspace_index_of(s) >= 0)
        workspace_end();
    else
        session_free(s);
    if (carried)
        viewport_handoff();
    else
        viewport_end();
    ui_raw(0);
    tty_raw_handoff();

    voice_handoff();
    execvp(argv[0], argv);

    if (carried) {
        unlink(path);
        viewport_end();
    }
    ui_raw(0);
    fprintf(stderr, APP_NAME ": could not exec %s\n", argv[0]);
    _exit(1);
}
