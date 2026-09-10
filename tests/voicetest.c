#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prompt.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "vendor/macos_voice.h"
#include "voice.h"
#include "workspace.h"

struct macos_voice { int fd; };

struct session {
    int running;
    int abort;
};

static struct session sess;
static macos_voice fake;
static macos_voice_cb cb;
static void *cb_ud;
static int need_ready;

static char *sent[8];
static int nsent;
static int fails;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    fails++;
}

static void eq_str(const char *what, const char *got, const char *want)
{
    if ((!got && !want) || (got && want && !strcmp(got, want)))
        return;
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n",
            what, got ? got : "(null)", want ? want : "(null)");
    fails++;
}

macos_voice *macos_voice_start(const macos_voice_opts *opts, macos_voice_cb fn, void *ud)
{
    (void)opts;
    cb = fn;
    cb_ud = ud;
    need_ready = 1;
    fake.fd = 3;
    return &fake;
}

int  macos_voice_fd(const macos_voice *v) { (void)v; return -1; }
int  macos_voice_poll(macos_voice *v, int timeout_ms)
{
    (void)v;
    (void)timeout_ms;
    if (need_ready && cb) {
        need_ready = 0;
        cb(cb_ud, "ready", NULL);
        return 1;
    }
    return 0;
}
int  macos_voice_say(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_announce(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_finish(macos_voice *v) { (void)v; return 0; }
int  macos_voice_cancel(macos_voice *v) { (void)v; return 0; }
int  macos_voice_mute(macos_voice *v) { (void)v; return 0; }
int  macos_voice_busy(macos_voice *v, int busy) { (void)v; (void)busy; return 0; }
int  macos_voice_volume(macos_voice *v, double volume) { (void)v; (void)volume; return 0; }
int  macos_voice_rate(macos_voice *v, double rate) { (void)v; (void)rate; return 0; }
int  macos_voice_focus(macos_voice *v, int focused) { (void)v; (void)focused; return 0; }
void macos_voice_stop(macos_voice *v) { (void)v; }

int settings_get_int(const char *key, int fallback) { (void)key; return fallback; }
const char *settings_get_str(const char *key, const char *fallback)
{
    (void)key;
    return fallback;
}
void settings_set_int(const char *key, int value) { (void)key; (void)value; }

void status_resume(void) {}
int  status_spinning(void) { return 0; }
void status_set_word(const char *text) { (void)text; }
void status_begin(void) {}
void status_tick(void) {}
void status_end(void) {}
void status_set_alert(const char *text) { (void)text; }
void status_touch(void) {}

struct session *workspace_current(void) { return &sess; }
int workspace_index_of(const struct session *s) { (void)s; return 0; }
int workspace_send(int index, const char *line, const char *shown)
{
    (void)index;
    (void)shown;
    if (nsent < (int)(sizeof sent / sizeof sent[0]))
        sent[nsent++] = strdup(line);
    return 1;
}

int  session_turn_running(const struct session *s) { return s && s->running; }
void session_interrupt(struct session *s) { if (s) s->abort = 1; }
int  session_last_interrupted(const struct session *s) { (void)s; return 0; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
int  session_add_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; return 1; }
void session_remove_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; }

void prompt_echo_message(const char *text) { (void)text; }

static void fire(const char *kind, const char *text)
{
    if (cb)
        cb(cb_ud, kind, text);
}

static void clear_sent(void)
{
    for (int i = 0; i < nsent; i++)
        free(sent[i]);
    nsent = 0;
}

static int start_voice(void)
{
    char err[128];
    if (!voice_start(err, sizeof err)) {
        fprintf(stderr, "FAIL voice_start: %s\n", err);
        fails++;
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!start_voice())
        return 1;

    sess.running = 1;
    fire("final", "and then do this");
    char *line = voice_take_line();
    eq_str("speech during a turn is queued", line, "and then do this");
    free(line);

    sess.abort = 0;
    fire("final", "stop");
    if (!sess.abort)
        fail("stop during a turn interrupts");
    if (voice_take_line())
        fail("stop is not queued as a prompt");

    sess.abort = 0;
    fire("interrupt", NULL);
    if (!sess.abort)
        fail("interrupt event aborts a running turn");

    sess.running = 1;
    fire("final", "stop the volcano");
    line = voice_take_line();
    eq_str("stop inside a sentence is a prompt", line, "stop the volcano");
    free(line);

    sess.running = 0;
    sess.abort = 0;
    fire("partial", "what I was saying");
    voice_commit(&sess);
    if (nsent != 1)
        fail("commit sends the in-progress draft");
    else
        eq_str("committed draft", sent[0], "what I was saying");
    clear_sent();

    fire("partial", "left in the other window");
    voice_arm(0);
    if (nsent != 1)
        fail("leaving the window commits the draft");
    else
        eq_str("focus-out draft", sent[0], "left in the other window");
    clear_sent();

    voice_arm(1);
    fire("partial", "sent by hand");
    voice_draft_sent();
    fire("final", "sent by hand");
    if (voice_take_line())
        fail("a hand-submitted draft is not queued again");
    clear_sent();

    voice_stop();
    if (fails)
        return 1;
    puts("voicetest: ok");
    return 0;
}
