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
static int mic_off_alls;
int  macos_voice_mic_off_all(macos_voice *v) { (void)v; mic_off_alls++; return 0; }
int  macos_voice_busy(macos_voice *v, int busy) { (void)v; (void)busy; return 0; }
int  macos_voice_volume(macos_voice *v, double volume) { (void)v; (void)volume; return 0; }
int  macos_voice_rate(macos_voice *v, double rate) { (void)v; (void)rate; return 0; }
int  macos_voice_silence(macos_voice *v, double seconds) { (void)v; (void)seconds; return 0; }
int  macos_voice_focus(macos_voice *v, int focused) { (void)v; focus_calls++; helper_focus = focused; return 0; }
int  macos_voice_resumed(const macos_voice *v) { (void)v; return resumed; }
int  macos_voice_launched(const macos_voice *v) { (void)v; return 1; }
int  macos_voice_handoff(macos_voice *v) { (void)v; return 0; }
void macos_voice_protect_handoff(void) {}
void macos_voice_stop(macos_voice *v) { (void)v; }
void macos_voice_shutdown(macos_voice *v) { (void)v; }
int  macos_voice_reap(const char *helper_path) { (void)helper_path; return 0; }

static int wake_setting;
int settings_get_int(const char *key, int fallback)
{
    return !strcmp(key, SETTING_VOICE_WAKE) ? wake_setting : fallback;
}
const char *settings_get_str(const char *key, const char *fallback)
{
    (void)key;
    return fallback;
}
void settings_set_int(const char *key, int value)
{
    if (!strcmp(key, SETTING_VOICE_WAKE)) wake_setting = value;
}
void settings_set_str(const char *key, const char *value) { (void)key; (void)value; }

void status_tick(void) {}
int  status_work_begin(const char *text) { (void)text; return 1; }
void status_work_end(int owned) { (void)owned; }
void status_set_alert(const char *text) { (void)text; }
static char last_note[64];
void status_set_note(const char *text) { snprintf(last_note, sizeof last_note, "%s", text); }
void status_touch(void) {}
void chrome_live_label(const char *(*fn)(void)) { (void)fn; }

/* the tab in front, and a second one to switch to */
static struct session other;
static struct session *front = &sess;
struct session *workspace_current(void) { return front; }
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
const char *session_last_reply(const struct session *s) { (void)s; return NULL; }
int  session_add_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; return 1; }
void session_remove_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; }

void prompt_echo_message(const char *text) { (void)text; }

int tty_focused(void) { return focused_window; }
int tty_is_raw(void) { return 0; }
int tty_read(tty_event *ev, int timeout_ms) { (void)ev; (void)timeout_ms; return 0; }

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

/* the prompt tracks words already in the line as the preview again */
static int box_claim(void *ud, const char *text)
{
    (void)ud;
    if (!strstr(box, text))
        return 0;
    box_released = 0;
    return 1;
}

/* workspace_show: leave the tab in front, stash its box, load the other */
static void switch_tab(struct session *to, char *stash, size_t size, const char *load)
{
    voice_leave(front);
    snprintf(stash, size, "%s", box);
    snprintf(box, sizeof box, "%s", load);
    box_released = 1;
    front = to;
    voice_refocus();
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
    voice_on_release(box_release, NULL);
    fire("partial", long_text);
    eq_str("long partial is fully previewed", box, long_text);
    voice_set_mic(0);
    if (nsent || voice_take_line())
        fail("mic-off never submits an unterminated dictation");
    eq_str("mic-off preserves the long dictation as a draft", box, long_text);
    voice_set_mic(1);
    voice_on_heard(NULL, NULL);
    voice_on_draft(NULL, NULL);
    voice_on_release(NULL, NULL);
    box[0] = '\0';
    box_released = 0;
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
    else if (strstr(sent_full[0], "restating"))
        fail("an idle send does not ask for a restatement");
    clear_sent();

    sess.running = 1;
    fire("partial", "queued behind a reply");
    voice_commit(&sess);
    sess.running = 0;
    if (nsent != 1)
        fail("commit sends the draft mid-turn");
    else if (!strstr(sent_full[0], "restating") ||
             !strstr(sent_full[0], "queued behind a reply"))
        fail("a queued send asks for a restatement");
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

    voice_set_mic(0);
    voice_set_mic(1);
    box[0] = '\0';
    box_released = 0;
    fire("partial", "ok commit and make");
    box[0] = '\0';
    fire("partial", "ok commit and make install");
    fire("dropped", NULL);
    fire("partial", "ok commit and make install");
    eq_str("an utterance after a dropped erased turn shows", box, "ok commit and make install");
    fire("final", "ok commit and make install");
    line = voice_take_line();
    eq_str("an utterance after a dropped erased turn is sent", line, "ok commit and make install");
    free(line);
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

    /* a tab switch leaves voice listening: the dictation is held for its tab,
       the rest of an utterance spanning the switch goes to the new tab, and
       the dictation resumes where it was on return. Exercise both a wake-word
       partial and a dictation spanning finals. */
    for (int multi = 0; multi < 2; multi++) {
        if (!start_voice())
            return 1;
        voice_on_heard(box_heard, NULL);
        voice_on_draft(box_line, NULL);
        voice_on_release(box_release, NULL);
        voice_on_claim(box_claim, NULL);
        box[0] = '\0';
        box_released = 0;
        front = &sess;
        if (multi)
            fire("final", "listen first part");
        const char *raw = multi ? "second part" : "listen first part";
        fire("partial", raw);
        const char *want = multi ? "listen first part second part" : "listen first part";
        eq_str("the dictation previews before switching", box, want);
        char stash_a[sizeof box], stash_b[sizeof box];
        switch_tab(&other, stash_a, sizeof stash_a, "");
        eq_str("switch keeps all heard text in the departing box", stash_a, want);
        if (nsent || voice_take_line())
            fail("switch does not submit or queue an unfinished dictation");
        eq_str("switch leaves voice listening", voice_label(), "voice");

        char spanning[256];
        snprintf(spanning, sizeof spanning, "%s, and this is for the worker", raw);
        fire("partial", spanning);
        eq_str("the rest of a spanning utterance previews on the new tab", box,
               "and this is for the worker");
        /* the recognizer revises case and punctuation of the carried words */
        fire("final", multi ? "Second part. And this is for the worker."
                            : "Listen, first part. And this is for the worker.");
        char *taken = voice_take_line();
        eq_str("the rest of a spanning utterance is the new tab's turn", taken,
               "And this is for the worker.");
        free(taken);
        if (!strcmp(voice_label(), "voice dictation"))
            fail("the new tab does not inherit the dictation");
        box[0] = '\0';
        box_released = 0;

        fire("partial", "listen worker note");
        switch_tab(&sess, stash_b, sizeof stash_b, stash_a);
        eq_str("the new tab keeps its own dictation", stash_b, "listen worker note");
        eq_str("returning resumes the dictation", voice_label(), "voice dictation");
        eq_str("returning restores the box", box, want);
        fire("partial", "third part");
        char more[256];
        snprintf(more, sizeof more, "%s third part", want);
        eq_str("the resumed dictation appends to the saved draft", box, more);
        fire("final", "third part");
        if (nsent || voice_take_line())
            fail("a resumed dictation still waits for its end phrase");

        switch_tab(&other, stash_a, sizeof stash_a, stash_b);
        eq_str("each tab resumes its own dictation", box, "listen worker note");
        fire("final", "about the build ok done");
        taken = voice_take_line();
        eq_str("the worker dictation sends on its own tab", taken,
               "listen worker note about the build");
        free(taken);
        box[0] = '\0';
        box_released = 0;

        switch_tab(&sess, stash_b, sizeof stash_b, stash_a);
        fire("final", "ok done");
        taken = voice_take_line();
        eq_str("the first dictation sends whole on its tab", taken, more);
        free(taken);
        if (nsent)
            fail("switching never sends to a tab directly");

        voice_stop();
        voice_on_heard(NULL, NULL);
        voice_on_draft(NULL, NULL);
        voice_on_release(NULL, NULL);
        voice_on_claim(NULL, NULL);
        front = &sess;
        clear_sent();
        clear_chimes();
    }

    /* the carried words are taken off a spanning utterance even when the
       recognizer revised a word in them or the switch fell inside a word */
    if (!start_voice())
        return 1;
    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    voice_on_release(box_release, NULL);
    voice_on_claim(box_claim, NULL);
    {
        static const char *const spans[][3] = {
            {"listen alpha for", "listen alpha four gamma", "gamma"},
            {"listen alpha be", "listen alpha beta gamma", "gamma"},
        };
        char stash[sizeof box];
        for (int i = 0; i < 2; i++) {
            box[0] = '\0';
            box_released = 0;
            front = &sess;
            fire("partial", spans[i][0]);
            switch_tab(&other, stash, sizeof stash, "");
            fire("final", spans[i][1]);
            char *taken = voice_take_line();
            eq_str("a spanning utterance leaves its carried words behind", taken,
                   spans[i][2]);
            free(taken);
            voice_forget(&sess);
        }
    }
    front = &sess;
    voice_stop();
    clear_sent();
    clear_chimes();

    /* a closed tab's dictation is not resumed by a tab that takes its place */
    if (!start_voice())
        return 1;
    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    voice_on_release(box_release, NULL);
    voice_on_claim(box_claim, NULL);
    box[0] = '\0';
    box_released = 0;
    {
        char stash[sizeof box];
        fire("final", "listen for a closed tab");
        switch_tab(&other, stash, sizeof stash, "");
        voice_forget(&sess);
        switch_tab(&sess, stash, sizeof stash, stash);
        if (!strcmp(voice_label(), "voice dictation"))
            fail("a forgotten dictation does not resume");
        /* a box that no longer holds the dictation does not resume it either */
        fire("final", "listen held words");
        switch_tab(&other, stash, sizeof stash, "");
        switch_tab(&sess, stash, sizeof stash, "something else");
        if (!strcmp(voice_label(), "voice dictation"))
            fail("a dictation missing from its box does not resume");
    }
    voice_stop();
    clear_sent();
    clear_chimes();

    if (!start_voice())
        return 1;
    box[0] = '\0';
    box_released = 0;
    fire("final", "finished before switching");
    fire("partial", "next words");
    voice_leave(front);
    eq_str("queued finals stay before the partial in the departing draft", box,
           "finished before switching next words");
    if (nsent || voice_take_line())
        fail("no queued speech leaks across a tab switch");
    voice_stop();
    clear_chimes();
    box[0] = '\0';
    box_released = 0;
    if (!start_voice())
        return 1;
    fire("final", "listen held across a focus change");
    fire("partial", "unfinished tail");
    voice_arm(0);
    eq_str("focus loss preserves dictation", box,
           "listen held across a focus change unfinished tail");
    if (nsent || voice_take_line())
        fail("focus loss does not flush dictation without ok done");
    voice_arm(1);
    eq_str("focus return keeps the dictation open", voice_label(), "voice dictation");
    fire("final", "and more ok done");
    line = voice_take_line();
    eq_str("the dictation carries on after focus returns", line,
           "listen held across a focus change unfinished tail and more");
    free(line);
    voice_stop();
    voice_on_heard(NULL, NULL);
    voice_on_draft(NULL, NULL);
    voice_on_release(NULL, NULL);
    clear_chimes();

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

    if (!start_voice())
        return 1;
    mic_off_alls = 0;
    last_note[0] = '\0';
    fire("mic", "0");
    if (!voice_mic())
        fail("mic off from the helper waits for the poll to return");
    voice_pending();
    if (voice_mic())
        fail("mic off from the helper turns the mic off");
    eq_str("mic off from the helper shows the local note", last_note, "mic off");
    if (mic_off_alls)
        fail("mic off from the helper is not sent back to every client");
    voice_set_mic(1);
    fire("mic", "1");
    voice_pending();
    if (!voice_mic())
        fail("only mic off is taken from the helper");
    voice_mic_off(1);
    if (voice_mic() || mic_off_alls != 1)
        fail("mic off with every asks the helper to turn off every client");
    voice_mic_off(1);
    if (mic_off_alls != 1)
        fail("a mic already off sends nothing");
    voice_stop();

    focused_window = 1;
    focus_when_ready = -1;
    resumed = 0;
    voice_set_wake(1);
    if (!start_voice())
        return 1;
    voice_on_heard(box_heard, NULL);
    voice_on_draft(box_line, NULL);
    voice_on_release(box_release, NULL);
    voice_on_claim(box_claim, NULL);
    box[0] = '\0';
    box_released = 0;
    clear_sent();
    eq_str("wake mode waits for its word", voice_label(), "voice: say listen");
    fire("partial", "background conversation");
    eq_str("background speech stays out of the box", box, "");
    voice_commit(front);
    fire("final", "background conversation");
    fire("final", "listener ok done");
    if (nsent || voice_take_line())
        fail("wake mode ignores speech without the complete wake word");
    fire("partial", "noise Listen write a note");
    eq_str("wake preview starts at the wake word", box, "Listen write a note");
    fire("final", "noise Listen write a note");
    eq_str("wake mode opens dictation", voice_label(), "voice dictation");
    fire("final", "with two paragraphs");
    if (voice_take_line())
        fail("silence does not submit wake dictation");
    fire("final", "okay done with the introduction");
    if (voice_take_line())
        fail("a closing phrase inside a sentence does not send");
    fire("final", "OK, done.");
    line = voice_take_line();
    eq_str("wake mode sends joined speech without the closing phrase", line,
           "Listen write a note with two paragraphs okay done with the introduction");
    free(line);
    eq_str("sending returns to wake waiting", voice_label(), "voice: say listen");
    fire("final", "more background speech");
    if (voice_take_line())
        fail("each wake message needs a new wake word");
    fire("final", "listen second message ok done");
    line = voice_take_line();
    eq_str("wake and closing phrase can share one turn", line, "listen second message");
    free(line);
    fire("partial", "listen keep this unsent");
    voice_commit(front);
    if (nsent || voice_take_line())
        fail("focus commit holds a wake partial without sending");
    fire("final", "listen keep this unsent");
    fire("final", "ok done");
    line = voice_take_line();
    eq_str("committed partial is not duplicated", line, "listen keep this unsent");
    free(line);
    fire("final", "listen split closing phrase okay");
    if (voice_take_line())
        fail("half a closing phrase does not send");
    fire("final", "done");
    line = voice_take_line();
    eq_str("closing phrase can span recognition turns", line, "listen split closing phrase");
    free(line);
    fire("final", "listen throw this away");
    fire("final", "cancel this");
    if (voice_take_line())
        fail("cancel does not send the dictation");
    eq_str("cancel returns to wake waiting", voice_label(), "voice: say listen");
    eq_str("cancel empties the box", box, "");
    fire("final", "listen cancel the meeting");
    if (voice_take_line())
        fail("cancel inside a sentence does not drop the dictation");
    fire("final", "ok done");
    line = voice_take_line();
    eq_str("cancel mid-sentence is kept as words", line, "listen cancel the meeting");
    free(line);
    fire("final", "listen drop it cancel that");
    if (voice_take_line())
        fail("cancel that drops the dictation");
    fire("final", "listen after cancel ok done");
    line = voice_take_line();
    eq_str("wake word works again after a cancel", line, "listen after cancel");
    free(line);
    fire("partial", "Listen");
    fire("final", "Listen");
    eq_str("bare wake word is previewed", box, "Listen");
    fire("partial", "Cancel");
    eq_str("cancel partial joins the preview", box, "Listen Cancel");
    fire("final", "Cancel");
    if (voice_take_line())
        fail("cancel right after the wake word sends nothing");
    eq_str("cancel right after the wake word empties the box", box, "");
    eq_str("cancel right after the wake word returns to waiting", voice_label(), "voice: say listen");
    fire("partial", "Listen");
    fire("final", "Listen");
    fire("partial", "Cancel");
    fire("partial", "");
    fire("cancelled", NULL);
    if (voice_take_line())
        fail("the helper cancel event sends nothing");
    eq_str("the helper cancel event empties the box", box, "");
    eq_str("the helper cancel event returns to waiting", voice_label(), "voice: say listen");
    fire("partial", "Listen cancel");
    fire("final", "Listen cancel");
    eq_str("wake word and cancel in one turn empty the box", box, "");
    fire("final", "listen one more thing, cancel.");
    if (voice_take_line())
        fail("cancel sharing a turn with the wake word sends nothing");
    eq_str("cancel in the opening turn returns to waiting", voice_label(), "voice: say listen");
    voice_stop();
    if (!start_voice())
        return 1;
    if (!voice_wake())
        fail("wake mode survives voice restart");
    voice_set_wake(0);
    fire("final", "ordinary voice again");
    line = voice_take_line();
    eq_str("turning wake mode off restores ordinary voice", line, "ordinary voice again");
    free(line);
    voice_stop();

    if (fails)
        return 1;
    puts("voicetest: ok");
    return 0;
}
