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
static int helper_focus = -1;
static int focused_window = 1;
static int focus_when_ready = -1;
static int resumed;
static int focus_calls;

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
/* what the helper hands back on the next poll, as it does when ownership is
   released */
static const char *handoff_text;
int  macos_voice_poll(macos_voice *v, int timeout_ms)
{
    (void)v;
    (void)timeout_ms;
    if (handoff_text && cb) {
        const char *text = handoff_text;
        handoff_text = NULL;
        cb(cb_ud, "final", text);
        return 1;
    }
    if (need_ready && cb) {
        need_ready = 0;
        if (focus_when_ready >= 0)
            focused_window = focus_when_ready;
        cb(cb_ud, "ready", NULL);
        return 1;
    }
    return 0;
}
int  macos_voice_say(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_announce(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_finish(macos_voice *v) { (void)v; return 0; }
static int ncancels;
int  macos_voice_cancel(macos_voice *v) { (void)v; ncancels++; return 0; }
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
int  macos_voice_focus(macos_voice *v, int focused) { (void)v; focus_calls++; helper_focus = focused; return 0; }
int  macos_voice_resumed(const macos_voice *v) { (void)v; return resumed; }
int  macos_voice_handoff(macos_voice *v) { (void)v; return 0; }
void macos_voice_protect_handoff(void) {}
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

int tty_focused(void) { return focused_window; }

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

static const char *typed_box_line(void *ud)
{
    (void)ud;
    return "typed first";
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

    if (voice_drop())
        fail("dropping an empty voice input is not handled");
    if (voice_drop())
        fail("a drop hold is not fresh voice input");
    /* Clear the residue hold before testing recognition below. */
    voice_set_mic(0);
    voice_set_mic(1);
    clear_chimes();

    char long_text[800] = "listen ";
    for (int i = 0; i < 65; i++)
        strcat(long_text, "dictation ");
    strcat(long_text, "illustration");
    char terminated[850];
    snprintf(terminated, sizeof terminated, "%s ok done", long_text);
    fire("final", terminated);
    char *long_line = voice_take_line();
    eq_str("long dictation preserves the last word and terminator", long_line, long_text);
    free(long_line);
    voice_set_mic(0);
    voice_set_mic(1);
    clear_sent();
    clear_chimes();

    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    fire("partial", long_text);
    eq_str("long partial is fully previewed", box, long_text);
    voice_set_mic(0);
    if (nsent != 1)
        fail("mic-off commits a long draft once");
    else
        eq_str("mic-off preserves the last word of a long draft", sent[0], long_text);
    voice_set_mic(1);
    voice_on_heard(NULL, NULL);
    voice_on_draft(NULL, NULL);
    clear_sent();
    clear_chimes();

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

    /* releasing the helper makes it hand back what it had heard; that answer
       belongs to the window being left */
    voice_arm(1);
    handoff_text = "handed back on the way out";
    voice_arm(0);
    if (nsent != 1)
        fail("the helper's handoff commits to the window being left");
    else
        eq_str("handoff final", sent[0], "handed back on the way out");
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

    fire("final", "dictated after it");
    voice_on_draft(typed_box_line, NULL);
    voice_commit(&sess);
    if (nsent)
        fail("a voice commit does not bypass a typed draft");
    clear_chimes();
    line = voice_take_line();
    eq_str("speech beside a typed draft stays queued", line,
           "dictated after it");
    if (nchimes)
        fail("taking speech beside a typed draft does not chime");
    free(line);
    voice_on_draft(NULL, NULL);

    clear_chimes();
    fire("final", "sent on its own");
    line = voice_take_line();
    if (nchimes != 1)
        fail("taking speech with an empty box still chimes");
    else
        eq_str("chime on empty take", chimes[0], "sent");
    free(line);

    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    box[0] = '\0';
    box_released = 0;
    fire("final", "first turn");
    fire("partial", "next words");
    clear_chimes();
    line = voice_take_line();
    eq_str("a take while the next words preview", line, "first turn");
    if (nchimes != 1)
        fail("a take while the box is only the next preview still chimes");
    else
        eq_str("chime on preview-only take", chimes[0], "sent");
    free(line);
    clear_chimes();

    box[0] = '\0';
    fire("partial", "Pause.");
    if (box[0])
        fail("the pause command is not previewed");
    fire("final", "Pause.");
    if (box[0] || voice_take_line())
        fail("the pause command is not sent");
    eq_str("pause label", voice_label(), "voice paused");
    if (nchimes != 1)
        fail("pausing chimes once");
    else
        eq_str("chime on pause", chimes[0], "interrupted");
    clear_chimes();
    sess.running = 1;
    sess.abort = 0;
    fire("partial", "this is not for the prompt");
    if (box[0])
        fail("speech while paused is not previewed");
    ncancels = 0;
    fire("final", "this is not for the prompt");
    if (ncancels != 1)
        fail("a turn dropped while paused releases the helper");
    fire("final", "stop");
    fire("interrupt", NULL);
    if (voice_take_line() || sess.abort || box[0])
        fail("speech while paused is not acted on");
    fire("partial", "resume");
    if (box[0])
        fail("the resume command is not previewed");
    fire("final", "okay resume");
    if (voice_take_line() || box[0])
        fail("the resume command is not sent");
    if (!strcmp(voice_label(), "voice paused"))
        fail("resume clears the pause label");
    if (nchimes != 1)
        fail("resuming chimes once");
    else
        eq_str("chime on resume", chimes[0], "listening");
    sess.running = 0;
    fire("partial", "pause the build");
    eq_str("a turn that grows past a command word previews", box, "pause the build");
    fire("final", "pause the build");
    line = voice_take_line();
    eq_str("a sentence starting with pause is a prompt", line, "pause the build");
    free(line);
    clear_chimes();

    fire("final", "listen write a note");
    fire("final", "pause");
    eq_str("pausing keeps the dictation in the box", box, "listen write a note");
    fire("final", "not part of the note");
    fire("final", "resume");
    fire("final", "about the build ok done");
    line = voice_take_line();
    eq_str("a dictation carries on across a pause", line,
           "listen write a note about the build");
    free(line);
    clear_chimes();

    fire("partial", "Pause listening");
    eq_str("pause listening pauses from a partial", voice_label(), "voice paused");
    if (nchimes != 1)
        fail("an early pause chimes once");
    fire("final", "Pause, listening.");
    eq_str("the final of an early pause keeps it paused", voice_label(), "voice paused");
    if (nchimes != 1 || voice_take_line())
        fail("the final of an early pause does nothing more");
    fire("partial", "Resume listening");
    if (!strcmp(voice_label(), "voice paused"))
        fail("resume listening resumes from a partial");
    fire("final", "Resume listening.");
    if (nchimes != 2 || voice_take_line() || box[0])
        fail("the final of an early resume does nothing more");
    fire("partial", "pause");
    if (!strcmp(voice_label(), "voice paused"))
        fail("a bare pause partial waits for the final");
    fire("final", "pause");
    fire("final", "resume");
    clear_chimes();

    fire("final", "Pause, pause, pause, pause.");
    if (voice_take_line())
        fail("a repeated pause command is not sent");
    eq_str("a repeated pause command pauses", voice_label(), "voice paused");
    fire("final", "resume resume");
    if (!strcmp(voice_label(), "voice paused"))
        fail("a repeated resume command resumes");
    clear_chimes();

    fire("final", "pause");
    voice_set_mic(0);
    voice_set_mic(1);
    if (!strcmp(voice_label(), "voice paused"))
        fail("turning the mic on by hand clears the pause");
    clear_chimes();
    voice_on_heard(NULL, NULL);
    voice_on_draft(NULL, NULL);

    voice_stop();
    clear_chimes();
    clear_sent();

    /* a window started out of front leaves the microphone to the one in front */
    focused_window = 0;
    if (!start_voice())
        return 1;
    if (nchimes)
        fail("starting out of front does not chime");
    if (helper_focus != 0)
        fail("background startup must not claim helper focus");
    fire("final", "meant for the other window");
    if (voice_take_line())
        fail("speech is not taken by a window out of front");
    voice_stop();

    focused_window = 1;
    focus_when_ready = 0;
    if (!start_voice())
        return 1;
    if (helper_focus != 0 || nchimes)
        fail("focus lost while helper starts must not claim the microphone");
    voice_stop();

    focused_window = 0;
    focus_when_ready = 1;
    if (!start_voice())
        return 1;
    if (helper_focus != 1)
        fail("focus gained while helper starts claims the microphone");
    voice_stop();

    clear_chimes();
    resumed = 1;
    focus_when_ready = -1;
    focused_window = 1;
    helper_focus = 0;
    focus_calls = 0;
    if (!start_voice())
        return 1;
    if (focus_calls || helper_focus != 0 || nchimes)
        fail("resumed background connection must not steal focus or chime");
    voice_arm(0);
    voice_arm(1);
    if (helper_focus != 1 || focus_calls != 2)
        fail("real focus edge still claims a resumed connection");
    /* a window that missed its focus-out is already armed, and still has to ask
       for the helper back */
    focus_calls = 0;
    helper_focus = 0;
    voice_arm(1);
    if (helper_focus != 1 || focus_calls != 1)
        fail("a repeated focus-in reclaims the helper");
    voice_stop();

    helper_focus = 1;
    focus_calls = 0;
    focused_window = 0;
    if (!start_voice())
        return 1;
    if (focus_calls || helper_focus != 1)
        fail("resuming must also preserve the current helper owner");
    voice_stop();

    if (fails)
        return 1;
    puts("voicetest: ok");
    return 0;
}
