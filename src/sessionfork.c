#include "sessionfork.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "session.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"

static char program[4096] = APP_NAME;

void sessionfork_set_program(const char *argv0)
{
    if (!argv0 || !*argv0)
        return;
    char resolved[4096];
    if (strchr(argv0, '/') && realpath(argv0, resolved))
        snprintf(program, sizeof program, "%s", resolved);
    else
        snprintf(program, sizeof program, "%s", argv0);
}

const char *sessionfork_program(void)
{
    return program;
}

static int run(char *const argv[], const char *out_path)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int out = out_path ? open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
                           : open("/dev/null", O_WRONLY);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
            close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int sessionfork_run(const struct session *s, enum fork_where where)
{
    if (!getenv("TMUX")) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("forking needs tmux");
        viewport_item_end();
        ui_flush();
        return 0;
    }

    if (!session_can_resume(s)) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("%s cannot resume a conversation, so there is nothing to fork",
                 session_backend(s));
        viewport_item_end();
        ui_flush();
        return 0;
    }
    const char *id = session_id(s);
    if (!id || !*id) {
        ui_error("nothing to fork yet — send a message first");
        ui_put("\n");
        return 0;
    }

    char *tmux[24];
    int n = 0;
    tmux[n++] = "tmux";
    if (where == FORK_WINDOW) {
        tmux[n++] = "new-window";
    } else {
        tmux[n++] = "split-window";
        tmux[n++] = where == FORK_SPLIT_H ? "-h" : "-v";
    }

    const char *cwd = session_cwd(s);
    if (cwd && *cwd) {
        tmux[n++] = "-c";
        tmux[n++] = (char *)cwd;
    }
    tmux[n++] = program;
    n += session_argv(s, tmux + n, SESSION_ARGV_MAX,
                      SESSION_ARGV_CWD | SESSION_ARGV_RESUME | SESSION_ARGV_SAFE);
    tmux[n] = NULL;

    if (run(tmux, NULL) != 0) {
        ui_error("tmux would not open the %s", where == FORK_WINDOW ? "window" : "split");
        ui_put("\n");
        return 0;
    }

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_bar(ui_style(UI_DIM), "forked");
    viewport_item_end();
    ui_flush();
    return 1;
}

int sessionfork_shell(const struct session *s, enum fork_where where, int quiet)
{
    if (!getenv("TMUX")) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("splitting needs tmux");
        viewport_item_end();
        ui_flush();
        return 0;
    }

    char *tmux[8];
    int n = 0;
    tmux[n++] = "tmux";
    if (where == FORK_WINDOW) {
        tmux[n++] = "new-window";
    } else {
        tmux[n++] = "split-window";
        tmux[n++] = where == FORK_SPLIT_H ? "-h" : "-v";
    }

    const char *cwd = session_cwd(s);
    if (cwd && *cwd) {
        tmux[n++] = "-c";
        tmux[n++] = (char *)cwd;
    }
    tmux[n] = NULL;

    if (run(tmux, NULL) != 0) {
        ui_error("tmux would not open the %s", where == FORK_WINDOW ? "window" : "split");
        ui_put("\n");
        ui_flush();
        return 0;
    }
    if (!quiet) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_bar(ui_style(UI_DIM), "split");
        viewport_item_end();
        ui_flush();
    }
    return 1;
}

char *sessionfork_exit_note(const struct session *s)
{
    const char *id = session_id(s);
    if (!id || !*id || !session_can_resume(s))
        return NULL;

    char        here[4096];
    const char *dir = session_cwd(s);
    char        cmd[9000];
    size_t      n = 0;
    if (dir && *dir && !(getcwd(here, sizeof here) && !strcmp(here, dir))) {
        char quoted[4200];
        if (!text_shell_quote(dir, quoted, sizeof quoted))
            return NULL;
        int wrote = snprintf(cmd, sizeof cmd, "cd %s && ", quoted);
        if (wrote < 0 || (size_t)wrote >= sizeof cmd)
            return NULL;
        n = (size_t)wrote;
    }
    int wrote = snprintf(cmd + n, sizeof cmd - n, "%s", APP_NAME);
    if (wrote < 0 || (size_t)wrote >= sizeof cmd - n)
        return NULL;
    n += (size_t)wrote;

    char *args[SESSION_ARGV_MAX];
    int   count = session_argv(s, args, COUNT(args),
                               SESSION_ARGV_RESUME | SESSION_ARGV_SAFE);
    for (int i = 0; i < count; i++) {
        wrote = snprintf(cmd + n, sizeof cmd - n, " %s", args[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof cmd - n)
            break;
        n += (size_t)wrote;
    }

    const char *dim = ui_style(UI_DIM);
    const char *reset = ui_style(UI_RESET);
    const char *brand = ui_style(UI_BRAND);
    const char *label = "resume this conversation:";
    size_t need = strlen(brand) + strlen(UI_BAR) + strlen(reset) + 1 +
                  strlen(dim) + strlen(label) + strlen(reset) + 1 +
                  strlen(dim) + strlen(cmd) + strlen(reset) + 2;
    char *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need, "%s%s%s %s%s%s\n%s%s%s\n", brand, UI_BAR, reset, dim,
             label, reset, dim, cmd, reset);
    return out;
}
