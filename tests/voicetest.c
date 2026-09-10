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

static char *chimes[8];
static int nchimes;
static char *sent[8];
static char *sent_full[8];
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
int  macos_voice_chime(macos_voice *v, const char *name)
{
    (void)v;
    if (nchimes < (int)(sizeof chimes / sizeof chimes[0]))
        chimes[nchimes++] = strdup(name);
    return 0;
}
int  macos_voice_mic(macos_voice *v, int on) { (void)v; (void)on; return 0; }
int  macos_voice_busy(macos_voice *v, int busy) { (void)v; (void)busy; return 0; }
int  macos_voice_volume(macos_voice *v, double volume) { (void)v; (void)volume; return 0; }
int  macos_voice_rate(macos_voice *v, double rate) { (void)v; (void)rate; return 0; }
int  macos_voice_silence(macos_voice *v, double seconds) { (void)v; (void)seconds; return 0; }
int  macos_voice_focus(macos_voice *v, int focused) { (void)v; (void)focused; return 0; }
void macos_voice_stop(macos_voice *v) { (void)v; }
void macos_voice_shutdown(macos_voice *v) { (void)v; }
int  macos_voice_reap(const char *helper_path) { (void)helper_path; return 0; }

int settings_get_int(const char *key, int fallback) { (void)key; return fallback; }
const char *settings_get_str(const char *key, const char *fallback)
{
    (void)key;
    return fallback;
}
void settings_set_int(const char *key, int value) { (void)key; (void)value; }
void settings_set_str(const char *key, const char *value) { (void)key; (void)value; }

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
    if (nsent < (int)(sizeof sent / sizeof sent[0])) {
        sent_full[nsent] = strdup(line);
        sent[nsent] = strdup(shown ? shown : line);
        nsent++;
    }
    return 1;
}

int  session_turn_running(const struct session *s) { return s && s->running; }
void session_interrupt(struct session *s) { if (s) s->abort = 1; }
int  session_last_interrupted(const struct session *s) { (void)s; return 0; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
int  session_add_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; return 1; }
void session_remove_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; }

void prompt_echo_message(const char *text) { (void)text; }

/* stands in for the input box: the preview is the whole line there */
static char box[1024];

static int box_released;

static void box_heard(void *ud, const char *text)
{
    (void)ud;
    if (box_released) {
        /* the prompt keeps what is there and writes the next words beside it */
        if (!text || !*text)
            return;
        box_released = 0;
        size_t n = strlen(box);
        snprintf(box + n, sizeof box - n, "%s%s", n ? " " : "", text);
        return;
    }
    snprintf(box, sizeof box, "%s", text ? text : "");
}

static void box_release(void *ud)
{
    (void)ud;
    box_released = 1;
}

static const char *box_line(void *ud)
{
    (void)ud;
    return box;
}

static void fire(const char *kind, const char *text)
{
    if (cb)
        cb(cb_ud, kind, text);
}

static void clear_chimes(void)
{
    for (int i = 0; i < nchimes; i++)
        free(chimes[i]);
    nchimes = 0;
}

static void clear_sent(void)
{
    for (int i = 0; i < nsent; i++) {
        free(sent[i]);
        free(sent_full[i]);
    }
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
    fire("partial", "reading this back");
    voice_commit(&sess);
    if (nsent != 1)
        fail("commit sends the draft while speaking");
    else if (!strstr(sent_full[0], "read back") || !strstr(sent_full[0], "reading this back"))
        fail("spoken input carries the preamble");
    clear_sent();

    voice_set_speak(0);
    fire("partial", "no preamble here");
    voice_commit(&sess);
    if (nsent != 1)
        fail("commit sends the draft while listening");
    else
        eq_str("listen-only draft", sent_full[0], "no preamble here");
    clear_sent();
    voice_set_speak(1);

    clear_chimes();
    sess.running = 0;
    fire("final", "a finished turn");
    char *taken = voice_take_line();
    if (nchimes != 1)
        fail("a finished utterance chimes when it is taken");
    else
        eq_str("chime on take", chimes[0], "sent");
    free(taken);
    clear_chimes();

    fire("partial", "one more thing");
    voice_commit(&sess);
    if (nchimes != 1)
        fail("sending a spoken line chimes once");
    else
        eq_str("chime on send", chimes[0], "sent");
    clear_sent();
    clear_chimes();

    voice_set_mic(0);
    if (nchimes)
        fail("turning the mic off does not chime");
    fire("final", "heard with the mic off");
    fire("interrupt", NULL);
    if (nchimes)
        fail("speech with the mic off does not chime");
    clear_chimes();
    voice_set_mic(1);
    if (nchimes != 1)
        fail("turning the mic on chimes once");
    else
        eq_str("chime on mic", chimes[0], "listening");
    clear_chimes();
    clear_sent();


    fire("partial", "sent by hand");
    voice_draft_sent();
    fire("final", "sent by hand");
    if (voice_take_line())
        fail("a hand-submitted draft is not queued again");
    clear_sent();

    clear_chimes();
    fire("partial", "sent by hand again");
    voice_draft_sent();
    /* the wake word arrives inside the hold that follows a hand-submit, and
       with its first words clipped by the gate that closes while speaking */
    fire("final", "up listen I want to show each project");
    if (nsent || voice_take_line())
        fail("a turn that opens a dictation is not sent on its own");
    if (nchimes != 1)
        fail("opening a dictation chimes once");
    else
        eq_str("chime on dictation", chimes[0], "listening");
    fire("final", "as a card ok done");
    line = voice_take_line();
    eq_str("dictation joins its turns", line,
           "listen I want to show each project as a card");
    free(line);
    clear_sent();
    clear_chimes();

    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    voice_on_release(box_release, NULL);

    fire("final", "listen show each project");
    eq_str("the dictation shows in the box", box, "listen show each project");
    snprintf(box, sizeof box, "%s", "listen show each");
    fire("final", "as a card ok done");
    line = voice_take_line();
    eq_str("a word deleted in the box is not sent", line,
           "listen show each as a card");
    free(line);
    clear_sent();
    clear_chimes();

    fire("final", "listen show each project");
    box[0] = '\0';
    fire("final", "ok done");
    if (voice_take_line())
        fail("an emptied box sends nothing");
    if (box[0])
        fail("an emptied box stays empty");
    clear_sent();
    clear_chimes();

    /* out of the hold left by the hand-submits above */
    voice_set_mic(0);
    voice_set_mic(1);
    box[0] = '\0';
    box_released = 0;
    clear_chimes();

    fire("partial", "ok so you should");
    eq_str("an utterance shows in the box", box, "ok so you should");
    box[0] = '\0';
    fire("partial", "ok so you should hear this");
    if (box[0])
        fail("erasing an utterance keeps it out of the box");
    fire("final", "ok so you should hear this");
    if (voice_take_line())
        fail("an erased utterance is not sent");
    if (box[0])
        fail("an erased utterance stays out of the box");

    fire("partial", "the next one still lands");
    eq_str("a later utterance shows again", box, "the next one still lands");
    fire("final", "the next one still lands");
    line = voice_take_line();
    eq_str("a later utterance is sent", line, "the next one still lands");
    free(line);
    clear_sent();
    clear_chimes();

    fire("final", "listen show each project");
    fire("partial", "as a card");
    eq_str("a dictation turn shows as it is spoken", box,
           "listen show each project as a card");
    snprintf(box, sizeof box, "%s", "listen show each");
    fire("partial", "as a card in the");
    eq_str("erasing a dictation turn stops it typing back", box,
           "listen show each");
    fire("final", "as a card in the corner");
    eq_str("an erased dictation turn is dropped", box, "listen show each");
    fire("final", "with a label ok done");
    line = voice_take_line();
    eq_str("the dictation carries on from what was kept", line,
           "listen show each with a label");
    free(line);
    clear_sent();
    clear_chimes();

    voice_on_heard(NULL, NULL);
    voice_on_draft(NULL, NULL);
    voice_on_release(NULL, NULL);

    voice_stop();
    if (fails)
        return 1;
    puts("voicetest: ok");
    return 0;
}
