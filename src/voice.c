#include "voice.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "vendor/agents/backend.h"
#include "workspace.h"

#define PVSAY       "pvsay"
#define QUEUE_MAX   32
#define WPM_DEFAULT 175
#define KILL_WAIT_MS 300

static int   on;
static int   muted;
static int   armed = 1;
static pid_t child;
static char *queue[QUEUE_MAX];
static int   nqueue;

static int reaped(int options)
{
    if (child <= 0)
        return 1;
    if (waitpid(child, NULL, options) == 0)
        return 0;
    child = 0;
    status_touch();
    return 1;
}

static void silence(void)
{
    for (int i = 0; i < nqueue; i++)
        free(queue[i]);
    nqueue = 0;
    if (child <= 0)
        return;
    kill(child, SIGTERM);
    struct timespec tick = {0, 10 * 1000 * 1000};
    for (int ms = 0; ms < KILL_WAIT_MS && !reaped(WNOHANG); ms += 10)
        nanosleep(&tick, NULL);
    if (child > 0) {
        kill(child, SIGKILL);
        reaped(0);
    }
}

static void write_all(int fd, const char *text, size_t len)
{
    while (len) {
        ssize_t n = write(fd, text, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        text += n;
        len -= (size_t)n;
    }
}

static void spawn(const char *text)
{
    int fd[2];
    if (pipe(fd) != 0)
        return;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);

    char rate[16], volume[16];
    snprintf(rate, sizeof rate, "%d", voice_rate() * WPM_DEFAULT / 100);
    snprintf(volume, sizeof volume, "%.2f", voice_volume() / 100.0);
    const char *name = settings_get_str(SETTING_VOICE_NAME, NULL);
    const char *argv[10];
    int n = 0;
    argv[n++] = PVSAY;
    argv[n++] = "--markdown";
    if (name && *name) {
        argv[n++] = "-v";
        argv[n++] = name;
    }
    argv[n++] = "-r";
    argv[n++] = rate;
    argv[n++] = "--volume";
    argv[n++] = volume;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        dup2(fd[0], STDIN_FILENO);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execvp(PVSAY, (char *const *)argv);
        _exit(127);
    }
    close(fd[0]);
    if (pid > 0) {
        child = pid;
        write_all(fd[1], text, strlen(text));
    }
    close(fd[1]);
    status_touch();
}

static void next(void)
{
    if (child > 0 || !nqueue)
        return;
    char *text = queue[0];
    memmove(queue, queue + 1, (size_t)--nqueue * sizeof *queue);
    spawn(text);
    free(text);
}

static void enqueue(const char *text)
{
    if (nqueue == QUEUE_MAX)
        return;
    char *copy = strdup(text);
    if (!copy)
        return;
    queue[nqueue++] = copy;
    next();
}

static void on_session_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (ev->kind != BACKEND_EV_ASSISTANT || !ev->text || !*ev->text)
        return;
    if (ev->parent && *ev->parent)
        return;
    if (!on || !armed || muted || s != workspace_current())
        return;
    enqueue(ev->text);
}

void voice_init(void)
{
    on = settings_get_int(SETTING_VOICE, 0) ? 1 : 0;
    armed = tty_focused();
    session_add_listener(on_session_event, NULL);
}

void voice_stop(void) { silence(); }

int voice_on(void) { return on; }

void voice_set_on(int value)
{
    on = value ? 1 : 0;
    settings_set_int(SETTING_VOICE, on);
    if (!on)
        silence();
    status_touch();
}

static int clamp(int n, int low, int high)
{
    return n < low ? low : n > high ? high : n;
}

int voice_volume(void)
{
    return clamp(settings_get_int(SETTING_VOICE_VOLUME, VOICE_VOLUME_DEFAULT), 0, 100);
}

void voice_set_volume(int percent)
{
    settings_set_int(SETTING_VOICE_VOLUME, clamp(percent, 0, 100));
}

int voice_rate(void)
{
    return clamp(settings_get_int(SETTING_VOICE_RATE, VOICE_RATE_DEFAULT),
                 VOICE_RATE_MIN, VOICE_RATE_MAX);
}

void voice_set_rate(int percent)
{
    settings_set_int(SETTING_VOICE_RATE, clamp(percent, VOICE_RATE_MIN, VOICE_RATE_MAX));
}

void voice_pending(void)
{
    reaped(WNOHANG);
    next();
}

void voice_turn_begin(struct session *s)
{
    (void)s;
    muted = 0;
}

void voice_turn_done(struct session *s)
{
    if (!on || armed || !s || session_last_interrupted(s))
        return;
    if (!settings_get_int(SETTING_VOICE_COMPLETE, 1))
        return;
    const char *name = session_title(s);
    char line[192];
    snprintf(line, sizeof line, "%s complete", name && *name ? name : APP_NAME);
    enqueue(line);
}

void voice_turn_cancel(struct session *s)
{
    if (s == workspace_current())
        silence();
}

void voice_refocus(void) { silence(); }

void voice_arm(int value)
{
    armed = value ? 1 : 0;
    if (!armed)
        silence();
}

int voice_speaking(void) { return child > 0; }

void voice_mute(void)
{
    silence();
    muted = 1;
}

char *voice_with_preamble(const char *line)
{
    if (!on || !line || !*line)
        return NULL;
    return text_dsprintf(VOICE_PREAMBLE "%s", line);
}

const char *voice_label(void)
{
    if (!on)
        return NULL;
    return child > 0 ? "speaking" : "voice";
}
