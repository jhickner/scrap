#include "voice.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "vendor/agents/backend.h"
#include "vendor/macos_voice.h"
#include "workspace.h"

#ifndef VOICE_HELPER_PATH
#define VOICE_HELPER_PATH "VoiceHelper.app"
#endif

#define READY_WAIT_MS 30000
#define LINE_MAX_QUEUE 16

static macos_voice *voice;
static int          ready;
static int          speaking;
static char         failure[256];
static char        *queue[LINE_MAX_QUEUE];
static int          nqueue;
static char         label[32];
static int          lock_fd = -1;

/* One microphone: a second mux would hear, and answer, the same words. */
static int take_lock(void)
{
    char dir[4096], path[4200];
    if (!path_config_dir(dir, sizeof dir))
        return 1;
    snprintf(path, sizeof path, "%s/voice.lock", dir);
    lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0)
        return 1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
        return 1;
    close(lock_fd);
    lock_fd = -1;
    return 0;
}

static void drop_lock(void)
{
    if (lock_fd >= 0)
        close(lock_fd);
    lock_fd = -1;
}
static void       (*heard_fn)(void *ud, const char *text);
static void        *heard_ud;

static void heard(const char *text)
{
    if (heard_fn)
        heard_fn(heard_ud, text);
}

static void enqueue(const char *text)
{
    if (nqueue >= LINE_MAX_QUEUE)
        return;
    queue[nqueue++] = strdup(text);
}

static void on_event(void *ud, const char *kind, const char *text)
{
    (void)ud;
    if (!strcmp(kind, "ready")) {
        ready = 1;
    } else if (!strcmp(kind, "partial")) {
        heard(text ? text : "");
    } else if (!strcmp(kind, "final")) {
        heard("");
        if (text && *text)
            enqueue(text);
    } else if (!strcmp(kind, "interrupt")) {
        struct session *s = workspace_current();
        if (s && session_turn_running(s))
            session_interrupt(s);
    } else if (!strcmp(kind, "speaking")) {
        speaking = text && *text == '1';
        status_touch();
    } else if (!strcmp(kind, "error")) {
        snprintf(failure, sizeof failure, "%s", text ? text : "helper failed");
    }
}

static void on_session_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (!voice || ev->kind != BACKEND_EV_ASSISTANT || !ev->text || !*ev->text)
        return;
    if (ev->parent && *ev->parent)
        return;
    if (s != workspace_current())
        return;
    macos_voice_say(voice, ev->text);
}

static void drain(void)
{
    if (!voice)
        return;
    int n;
    while ((n = macos_voice_poll(voice, 0)) > 0)
        ;
    if (n < 0 && !failure[0])
        snprintf(failure, sizeof failure, "helper exited");
    if (failure[0]) {
        char text[300];
        snprintf(text, sizeof text, "voice stopped: %s", failure);
        voice_stop();
        status_set_alert(text);
    }
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void wait_tick(void *ud)
{
    (void)ud;
    status_tick();
}

int voice_start(char *err, size_t size)
{
    if (voice)
        return 1;
    if (!take_lock()) {
        snprintf(err, size, "already on in another mux");
        return 0;
    }
    ready = 0;
    speaking = 0;
    failure[0] = '\0';

    status_resume();
    int owned = !status_spinning();
    status_set_word("starting voice");
    if (owned)
        status_begin();
    else
        status_tick();

    macos_voice_opts opts = {
        .helper_path = settings_get_str(SETTING_VOICE_HELPER, VOICE_HELPER_PATH),
        .voice       = settings_get_str(SETTING_VOICE_NAME, NULL),
        .rate        = atof(settings_get_str(SETTING_VOICE_RATE, "0")),
        .silence     = atof(settings_get_str(SETTING_VOICE_SILENCE, "0")),
        .input       = settings_get_str(SETTING_VOICE_INPUT, NULL),
        .tick        = wait_tick,
    };
    voice = macos_voice_start(&opts, on_event, NULL);
    if (!voice) {
        snprintf(err, size, "could not launch %s", opts.helper_path);
        drop_lock();
        if (owned)
            status_end();
        return 0;
    }

    long until = now_ms() + READY_WAIT_MS;
    while (!ready && !failure[0]) {
        long left = until - now_ms();
        if (left <= 0)
            break;
        int slice = left > SPIN_FRAME_MS ? SPIN_FRAME_MS : (int)left;
        int n = macos_voice_poll(voice, slice);
        if (n < 0) {
            if (!failure[0])
                snprintf(failure, sizeof failure, "helper exited");
            break;
        }
        status_tick();
    }
    if (owned)
        status_end();
    if (failure[0] || !ready) {
        snprintf(err, size, "%s", failure[0] ? failure : "helper is still starting");
        macos_voice_stop(voice);
        voice = NULL;
        drop_lock();
        return 0;
    }
    session_set_listener(on_session_event, NULL);
    return 1;
}

void voice_stop(void)
{
    if (!voice)
        return;
    session_set_listener(NULL, NULL);
    macos_voice_stop(voice);
    voice = NULL;
    drop_lock();
    ready = 0;
    speaking = 0;
    for (int i = 0; i < nqueue; i++)
        free(queue[i]);
    nqueue = 0;
    heard("");
    status_touch();
}

int voice_on(void) { return voice != NULL; }

void voice_on_heard(void (*fn)(void *ud, const char *text), void *ud)
{
    heard_fn = fn;
    heard_ud = ud;
}

const char *voice_label(void)
{
    if (!voice)
        return NULL;
    if (!ready)
        snprintf(label, sizeof label, "voice starting");
    else
        snprintf(label, sizeof label, "voice%s", speaking ? " speaking" : "");
    return label;
}

int voice_fds(int *out, int max)
{
    int fd = macos_voice_fd(voice);
    if (!voice || fd < 0 || max < 1)
        return 0;
    out[0] = fd;
    return 1;
}

int voice_pending(void)
{
    if (!voice)
        return 0;
    drain();
    return nqueue > 0;
}

char *voice_take_line(void)
{
    if (!voice)
        return NULL;
    drain();
    if (!nqueue)
        return NULL;
    char *line = queue[0];
    for (int i = 1; i < nqueue; i++)
        queue[i - 1] = queue[i];
    nqueue--;
    return line;
}

void voice_turn_begin(struct session *s)
{
    if (voice && s == workspace_current())
        macos_voice_busy(voice, 1);
}

void voice_turn_done(struct session *s)
{
    if (!voice || s != workspace_current())
        return;
    macos_voice_finish(voice);
    macos_voice_busy(voice, 0);
}

void voice_turn_cancel(struct session *s)
{
    if (voice && s == workspace_current())
        macos_voice_cancel(voice);
}

void voice_refocus(void)
{
    if (!voice)
        return;
    macos_voice_cancel(voice);
    struct session *s = workspace_current();
    macos_voice_busy(voice, s && session_turn_running(s));
}

void voice_mute(void)
{
    if (voice)
        macos_voice_mute(voice);
}
