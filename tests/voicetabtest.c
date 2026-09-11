#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "settings.h"

#include "chrome.h"
#include "cmd.h"
#include "gitinfo.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "sessionview.h"
#include "sidechannel.h"
#include "status.h"
#include "relay.h"
#include "tg.h"
#include "ui.h"
#include "viewport.h"
#include "tty.h"
#include "vendor/macos_voice.h"
#include "voice.h"
#include "workspace.h"

void restart_shield_thread(void) {}

int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }
void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }

void gitinfo_forget(void) {}
void tg_refocus(void) {}
void tg_forget_session(struct session *s) { (void)s; }
void relay_refocus(void) {}
void relay_forget_session(struct session *s) { (void)s; }
void cmd_forget_session(struct session *s) { (void)s; }
void view_collapse(int on) { (void)on; }

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

struct session {
    int running;
    int work;
    int busy;
    const char *cwd;
};

int session_turn_running(const struct session *s) { return s && s->running; }
int session_work_count(const struct session *s) { return s ? s->work : 0; }
int session_busy(const struct session *s) { return s && (s->busy || s->running); }
int session_compact(const struct session *s) { (void)s; return 0; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
const char *session_cwd(const struct session *s) { return s && s->cwd ? s->cwd : "."; }
const char *session_id(const struct session *s) { (void)s; return NULL; }
const char *session_backend(const struct session *s) { (void)s; return "grok"; }
const char *session_model(const struct session *s) { (void)s; return "default"; }
const char *session_effort(const struct session *s) { (void)s; return "default"; }
const char *session_saved_model(const char *backend) { (void)backend; return NULL; }
const char *session_saved_effort(const char *backend) { (void)backend; return NULL; }
const char *session_permission_name(int index) { (void)index; return "bypass"; }
int session_permission_default(void) { return 0; }
int session_unseen(const struct session *s) { (void)s; return 0; }
void session_set_unseen(struct session *s, int on) { (void)s; (void)on; }
void session_set_customizations(struct session *s, int on) { (void)s; (void)on; }
void session_set_thinking(struct session *s, int on) { (void)s; (void)on; }
void session_set_compact(struct session *s, int on) { (void)s; (void)on; }
int session_set_permission(struct session *s, const char *mode)
{
    (void)s;
    (void)mode;
    return 1;
}
void session_set_system_extra(struct session *s, const char *text) { (void)s; (void)text; }
void session_adopt_id(struct session *s, const char *id) { (void)s; (void)id; }
int session_start(struct session *s) { (void)s; return 1; }
void session_free(struct session *s) { (void)s; }
struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    (void)backend;
    (void)cwd;
    (void)model;
    (void)effort;
    return NULL;
}
struct session *session_set_drawing(struct session *s)
{
    static struct session *live;
    struct session *was = live;
    live = s;
    return was;
}
int session_turn_begin(struct session *s, const char *text)
{
    (void)text;
    if (!s)
        return 0;
    s->running = 1;
    return 1;
}
int session_turn_pump(struct session *s) { return s && s->running; }
void session_turn_wait(struct session *s) { (void)s; }
int session_idle_pump(struct session *s) { (void)s; return 0; }
int session_wake_fd(const struct session *s) { (void)s; return -1; }
int session_idle_fd(const struct session *s) { (void)s; return -1; }
int session_stall_armed(const struct session *s) { (void)s; return 0; }
int session_stalled(struct session *s) { (void)s; return 0; }
double session_turn_elapsed(const struct session *s) { (void)s; return 0; }
void session_spin_word(const struct session *s)
{
    (void)s;
    status_set_word("working");
}
const char *session_failed_prompt(const struct session *s) { (void)s; return NULL; }
/* The helper, answering nothing but what each check fires. */
struct macos_voice { int fd; };
static macos_voice fake;
static macos_voice_cb helper_cb;
static void *helper_ud;
static int need_ready;

macos_voice *macos_voice_start(const macos_voice_opts *opts, macos_voice_cb fn, void *ud)
{
    (void)opts;
    helper_cb = fn;
    helper_ud = ud;
    need_ready = 1;
    return &fake;
}
int  macos_voice_fd(const macos_voice *v) { (void)v; return -1; }
int  macos_voice_poll(macos_voice *v, int timeout_ms)
{
    (void)v;
    (void)timeout_ms;
    if (!need_ready)
        return 0;
    need_ready = 0;
    helper_cb(helper_ud, "ready", NULL);
    return 1;
}
int  macos_voice_say(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_announce(macos_voice *v, const char *text) { (void)v; (void)text; return 0; }
int  macos_voice_finish(macos_voice *v) { (void)v; return 0; }
int  macos_voice_cancel(macos_voice *v) { (void)v; return 0; }
int  macos_voice_mute(macos_voice *v) { (void)v; return 0; }
int  macos_voice_chime(macos_voice *v, const char *name) { (void)v; (void)name; return 0; }
int  macos_voice_mic(macos_voice *v, int on) { (void)v; (void)on; return 0; }
int  macos_voice_busy(macos_voice *v, int busy) { (void)v; (void)busy; return 0; }
int  macos_voice_volume(macos_voice *v, double volume) { (void)v; (void)volume; return 0; }
int  macos_voice_rate(macos_voice *v, double rate) { (void)v; (void)rate; return 0; }
int  macos_voice_silence(macos_voice *v, double seconds) { (void)v; (void)seconds; return 0; }
int  macos_voice_focus(macos_voice *v, int focused) { (void)v; (void)focused; return 0; }
int  macos_voice_resumed(const macos_voice *v) { (void)v; return 0; }
int  macos_voice_handoff(macos_voice *v) { (void)v; return 0; }
void macos_voice_protect_handoff(void) {}
void macos_voice_stop(macos_voice *v) { (void)v; }
void macos_voice_shutdown(macos_voice *v) { (void)v; }
int  macos_voice_reap(const char *helper_path) { (void)helper_path; return 0; }

int  session_add_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; return 1; }
void session_remove_listener(session_listener_fn fn, void *ud) { (void)fn; (void)ud; }
void session_interrupt(struct session *s) { (void)s; }
int  session_last_interrupted(const struct session *s) { (void)s; return 0; }

static struct prompt *box;
static struct session a, b;
static char sent_a[2048], sent_b[2048];
static int sends;

static void heard(void *ud, const char *text) { prompt_set_preview(ud, text); }
static const char *line(void *ud) { return prompt_line(ud); }
static void release(void *ud) { prompt_release_preview(ud); }
static int claim(void *ud, const char *text) { return prompt_claim_preview(ud, text); }
static int discard(void *ud) { (void)ud; return voice_discard(); }

static void eq(const char *what, const char *got, const char *want)
{
    if (got && want && !strcmp(got, want))
        return;
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
            want ? want : "(null)");
    failures++;
}

/* main's chat_line: a finished line goes into a box already holding text,
   and is sent from an empty one */
static void pump(void)
{
    char *taken = voice_take_line();
    if (!taken)
        return;
    const char *cur = prompt_line(box);
    if (cur && *cur) {
        prompt_insert(box, taken);
    } else {
        char *to = workspace_current() == &a ? sent_a : sent_b;
        snprintf(to, sizeof sent_a, "%s", taken);
        sends++;
    }
    free(taken);
}

static void partial(const char *text)
{
    helper_cb(helper_ud, "partial", text);
    pump();
}

/* the helper clears the preview, then delivers the turn */
static void final(const char *text)
{
    helper_cb(helper_ud, "partial", "");
    helper_cb(helper_ud, "final", text);
    pump();
}

static void type(const char *text)
{
    for (const char *c = text; *c; c++) {
        tty_event ev = {0};
        ev.key = TK_CHAR;
        ev.cp = (unsigned char)*c;
        prompt_live_key(box, &ev);
    }
}

static void press(tty_key key)
{
    tty_event ev = {0};
    ev.key = key;
    prompt_live_key(box, &ev);
}

static void show_tab(struct session *s)
{
    workspace_show(workspace_index_of(s));
}

static void begin(void)
{
    char err[128];
    show_tab(&b);
    prompt_adopt_draft(NULL, 0);
    show_tab(&a);
    prompt_adopt_draft(NULL, 0);
    sent_a[0] = sent_b[0] = '\0';
    sends = 0;
    if (!voice_start(err, sizeof err))
        fail(err);
}

#define SAID_A "Listen. Okay, we'll test switching tabs again and see what happens. " \
               "I'm talking, talking, and talking. Now I'm going to switch a tab. And then say, Here,"
#define SAID_B "Talk over here, see what happens here."

/* dictation left mid-sentence: the recognizer finishes the utterance on the
   other tab, whose words are that tab's; the first tab keeps its dictation
   open and sends it only on the end phrase */
static void check_switch_mid_sentence(void)
{
    begin();
    partial("Listen.");
    partial(SAID_A);
    show_tab(&b);
    partial(SAID_A " " SAID_B);
    final(SAID_A " " SAID_B);
    eq("the other tab sends only what was said there", sent_b, SAID_B);
    if (sent_a[0])
        fail("the dictation tab sends nothing while it is out of view");
    partial("Something else on this tab");
    final("Something else on this tab");
    show_tab(&a);
    eq("returning resumes the dictation", voice_label(), "voice dictation");
    eq("the dictation comes back in its box", prompt_line(box), SAID_A);
    partial("I'm back on the first tab.");
    final("I'm back on the first tab.");
    partial("And still talking.");
    final("And still talking.");
    if (sent_a[0])
        fail("speech after returning does not send without the end phrase");
    eq("speech after returning joins the dictation", prompt_line(box),
       SAID_A " I'm back on the first tab. And still talking.");
    final("ok done");
    eq("the end phrase sends the tab's whole dictation in order", sent_a,
       SAID_A " I'm back on the first tab. And still talking.");
    eq("the box is empty once sent", prompt_line(box), "");
    voice_stop();
}

/* the same with the first utterance finished before the switch */
static void check_switch_between_utterances(void)
{
    begin();
    partial(SAID_A);
    final(SAID_A);
    show_tab(&b);
    partial(SAID_B);
    final(SAID_B);
    eq("the other tab sends its own turn", sent_b, SAID_B);
    show_tab(&a);
    partial("More words.");
    final("More words.");
    if (sent_a[0])
        fail("a resumed dictation waits for its end phrase");
    final("okay, done");
    eq("the resumed dictation sends whole", sent_a, SAID_A " More words.");
    voice_stop();
}

/* typed text before the dictated words: a tab round trip leaves the caret
   where it was put */
static void check_caret_kept(void)
{
    begin();
    type("typed note");
    partial("Listen. spoken one");
    final("Listen. spoken one");
    eq("typed text and dictation share the box", prompt_line(box),
       "typed note Listen. spoken one");
    press(TK_HOME);
    press(TK_WORD_RIGHT);
    int caret = prompt_cursor(box);
    show_tab(&b);
    show_tab(&a);
    if (prompt_cursor(box) != caret)
        fail("a tab round trip leaves the caret where it was");
    type("new ");
    eq("typing after a round trip lands at the caret", prompt_line(box),
       "typed new note Listen. spoken one");
    voice_stop();
}

/* after returning, text typed right before the dictated words keeps them
   tracked: speech joins the dictation instead of replacing the whole line */
static void check_typed_before_resumed_words(void)
{
    begin();
    type("typed note");
    partial("Listen. spoken one");
    final("Listen. spoken one");
    show_tab(&b);
    show_tab(&a);
    press(TK_HOME);
    press(TK_WORD_RIGHT);
    press(TK_WORD_RIGHT);
    /* speech arrives while the word is still being typed against the words */
    type("more");
    eq("typing lands before the words", prompt_line(box), "typed note moreListen. spoken one");
    partial("spoken two");
    eq("speech keeps the typed text", prompt_line(box),
       "typed note more Listen. spoken one spoken two");
    final("spoken two");
    eq("the dictation holds both utterances", prompt_line(box),
       "typed note more Listen. spoken one spoken two");
    if (sent_a[0] || sent_b[0])
        fail("nothing is sent without the end phrase");
    voice_stop();
}

/* text typed into the dictated words, ending in a space, is adopted with them;
   after a round trip the end phrase still sends and leaves the box empty */
static void check_typed_inside_dictation(void)
{
    begin();
    partial("Listen. alpha gamma");
    final("Listen. alpha gamma");
    press(TK_END);
    type(" tail ");
    press(TK_HOME);
    press(TK_WORD_RIGHT);
    type("beta ");
    int caret = prompt_cursor(box);
    show_tab(&b);
    show_tab(&a);
    if (prompt_cursor(box) != caret)
        fail("a round trip after an edit leaves the caret where it was");
    eq("the edited dictation comes back", voice_label(), "voice dictation");
    final("ok done");
    eq("the end phrase sends the edited dictation", sent_a, "Listen. beta alpha gamma tail");
    eq("nothing is left in the box", prompt_line(box), "");
    voice_stop();
}

#define LIVE_FIRST "Listen. Okay, I'm gonna try the test again. This 1st part is dictated."
#define LIVE_TYPED "\n\nThen this is typed. Now I'll switch tabs..."
#define LIVE_BACK "And now I'm switched back."

static void check_live_roundtrip(void)
{
    const char *ends[] = {"okay, done", "okay done", "ok, done", "OK DONE!", "Okay, DONE."};
    for (int i = 0; i < 10; i++) {
        begin();
        partial(LIVE_FIRST);
        if (i < 5)
            final(LIVE_FIRST);
        tty_event pasted = {.key = TK_TEXT, .text = strdup(LIVE_TYPED)};
        prompt_live_key(box, &pasted);
        int caret = prompt_cursor(box);
        show_tab(&b);
        show_tab(&a);
        if (prompt_cursor(box) != caret)
            fail("live roundtrip preserves caret after typed line");
        partial(LIVE_BACK);
        eq("return speech follows the typed line", prompt_line(box),
           LIVE_FIRST LIVE_TYPED " " LIVE_BACK);
        final(LIVE_BACK);
        if (sends)
            fail("live roundtrip never sends before terminator");
        partial(ends[i % 5]);
        final(ends[i % 5]);
        eq("punctuated terminator sends the entire mixed draft", sent_a,
           LIVE_FIRST LIVE_TYPED " " LIVE_BACK);
        pump();
        if (sends != 1)
            fail("terminator submits exactly once");
        eq("submitted mixed draft is cleared", prompt_line(box), "");
        voice_stop();
    }
}

/* The spoken sequence from the report, with the typed follow-up entered
   after returning. With the fix it is a new draft after the voice submit. */
static void check_reported_sequence(void)
{
    begin();
    partial(LIVE_FIRST);
    final(LIVE_FIRST);
    show_tab(&b);
    show_tab(&a);
    partial(LIVE_BACK);
    final(LIVE_BACK);
    eq("reported speech stays in chronological order", prompt_line(box),
       LIVE_FIRST " " LIVE_BACK);
    if (sends)
        fail("reported sequence waits for its terminator");
    partial("Okay, this dictation appeared in the completely wrong place.");
    final("Okay, this dictation appeared in the completely wrong place.");
    partial("And, okay, done");
    final("And, okay, done");
    eq("reported punctuated end phrase submits", sent_a,
       LIVE_FIRST " " LIVE_BACK " Okay, this dictation appeared in the completely wrong place. And,");
    pump();
    if (sends != 1)
        fail("reported sequence submits exactly once");
    type("Then this is typed. Now I'll switch tabs...");
    eq("typed follow-up is a fresh draft", prompt_line(box),
       "Then this is typed. Now I'll switch tabs...");
    voice_stop();
}

static void check_typed_prefix_submit(void)
{
    begin();
    type("Typed first.");
    partial(LIVE_FIRST);
    final(LIVE_FIRST);
    show_tab(&b);
    show_tab(&a);
    final(LIVE_BACK);
    final("okay, done with that thought");
    if (sends)
        fail("a nonterminal end phrase does not submit");
    final("okay, done.");
    eq("terminator includes a typed prefix", sent_a,
       "Typed first. " LIVE_FIRST " " LIVE_BACK " okay, done with that thought");
    if (sends != 1)
        fail("mixed prefix draft submits exactly once");
    voice_stop();
}

static void key(int cp)
{
    tty_event ev = {0};
    ev.key = TK_CHAR;
    ev.cp = cp;
    prompt_live_key(box, &ev);
}

static void lacks(const char *what, const char *got, const char *word)
{
    if (got && strstr(got, word)) {
        fprintf(stderr, "FAIL %s: \"%s\" holds \"%s\"\n", what, got, word);
        failures++;
    }
}

#define CTRL_C 3

/* ctrl-c on an open dictation: its words are gone from the box, and neither the
   dictation held so far nor the rest of the utterance being spoken returns */
static void check_ctrl_c_mid_utterance(void)
{
    begin();
    partial("Listen. one two");
    final("Listen. one two");
    partial("three");
    eq("dictation shows before ctrl-c", prompt_line(box), "Listen. one two three");
    key(CTRL_C);
    eq("ctrl-c empties the box", prompt_line(box), "");
    partial("three four");
    lacks("the utterance cut by ctrl-c is not previewed", prompt_line(box), "three");
    final("three four");
    partial("Listen. five");
    final("Listen. five");
    lacks("resumed dictation drops the cleared words", prompt_line(box), "one");
    lacks("resumed dictation drops the cut utterance", prompt_line(box), "three");
    final("ok done");
    lacks("the next send has no cleared words", sent_a, "one");
    lacks("the next send has no cut utterance", sent_a, "three");
    voice_stop();
}

static void check_ctrl_c_between_utterances(void)
{
    begin();
    partial("Listen. one two");
    final("Listen. one two");
    key(CTRL_C);
    eq("ctrl-c empties the box", prompt_line(box), "");
    partial("Listen. three");
    final("Listen. three");
    lacks("resumed dictation drops the cleared words", prompt_line(box), "one");
    final("ok done");
    lacks("the next send has no cleared words", sent_a, "one");
    voice_stop();
}

/* the helper ends a turn with a dropped final while more of it is still heard */
static void check_ctrl_c_then_dropped(void)
{
    begin();
    partial("alpha beta");
    key(CTRL_C);
    helper_cb(helper_ud, "dropped", NULL);
    partial("alpha beta gamma");
    lacks("a dropped final does not bring back the cleared words", prompt_line(box), "alpha");
    final("alpha beta gamma");
    lacks("the cleared utterance is not inserted", prompt_line(box), "alpha");
    lacks("the cleared utterance is not sent", sent_a, "alpha");
    voice_stop();
}

/* the recognizer revises the words already heard, so they no longer match */
static void check_ctrl_c_revised(void)
{
    begin();
    partial("hello world");
    key(CTRL_C);
    partial("Hello, world, again");
    lacks("revised words of the cleared utterance are not previewed", prompt_line(box), "world");
    final("Hello, world, again");
    lacks("revised words of the cleared utterance are not inserted", prompt_line(box), "world");
    lacks("revised words of the cleared utterance are not sent", sent_a, "world");
    /* past the residue hold that follows a drop */
    usleep(800 * 1000);
    partial("fresh words");
    eq("speech after the cleared turn previews", prompt_line(box), "fresh words");
    voice_stop();
}

static void check_ctrl_c_tab_round_trip(void)
{
    begin();
    partial("Listen. one two");
    final("Listen. one two");
    key(CTRL_C);
    show_tab(&b);
    show_tab(&a);
    partial("Listen. three");
    final("Listen. three");
    lacks("a tab round trip does not restore cleared dictation", prompt_line(box), "one");
    voice_stop();
}

static void check_ctrl_c_keeps_typed_edit(void)
{
    begin();
    type("typed");
    key(CTRL_C);
    eq("ctrl-c clears typed text", prompt_line(box), "");
    partial("spoken");
    eq("speech after clearing typed text previews", prompt_line(box), "spoken");
    final("spoken");
    eq("speech after clearing typed text sends", sent_a, "spoken");
    voice_stop();
}

/* escape drops the dictated words and the dictation behind them */
static void check_escape_drops_dictation(void)
{
    begin();
    type("typed");
    partial("Listen. one two");
    final("Listen. one two");
    partial("three");
    press(TK_ESCAPE);
    eq("escape leaves the typed text", prompt_line(box), "typed");
    partial("three four");
    final("three four");
    partial("Listen. five");
    final("Listen. five");
    lacks("dictation after escape drops the old words", prompt_line(box), "one");
    lacks("dictation after escape drops the cut utterance", prompt_line(box), "three");
    voice_stop();
}

static void check_trace(void)
{
    char home[4096], config[4200], settings[4300], log[4300];
    if (!getcwd(home, sizeof home)) {
        fail("trace temporary home");
        return;
    }
    size_t n = strlen(home);
    snprintf(home + n, sizeof home - n, "/build/voice-trace-XXXXXX");
    if (!mkdtemp(home)) {
        fail("trace temporary directory");
        return;
    }
    snprintf(config, sizeof config, "%s/.config", home);
    mkdir(config, 0700);
    snprintf(config, sizeof config, "%s/.config/mux", home);
    mkdir(config, 0700);
    snprintf(settings, sizeof settings, "%s/settings", config);
    snprintf(log, sizeof log, "%s/voice-events.log", config);
    setenv("HOME", home, 1);
    settings_open(settings);
    check_typed_prefix_submit();
    if (!access(log, F_OK))
        fail("trace is off by default");
    settings_set_int(SETTING_VOICE_TRACE, 1);
    check_live_roundtrip();
    FILE *f = fopen(log, "r");
    if (!f) {
        fail("enabled trace creates a log");
    } else {
        char row[32768];
        int mask = 0;
        const char *events[] = {"helper.event", "tab.switch", "draft.save", "draft.restore",
                                "span=", "voice.end-match", "voice.take", "\\n\\nThen"};
        while (fgets(row, sizeof row, f)) {
            for (int i = 0; i < 8; i++)
                if (strstr(row, events[i]))
                    mask |= 1 << i;
            if (!strstr(row, "mono=") || !strstr(row, "pid="))
                fail("trace events have timestamps and process identity");
        }
        fclose(f);
        if (mask != 255)
            fail("trace covers transcripts, switches, drafts, spans, matches and takes");
    }
    settings_open(NULL);
    setenv("HOME", "/nonexistent", 1);
    unlink(log);
    unlink(settings);
    rmdir(config);
    snprintf(config, sizeof config, "%s/.config", home);
    rmdir(config);
    rmdir(home);
}

int main(void)
{
    setenv("COLUMNS", "100", 1);
    setenv("LINES", "24", 1);
    setenv("HOME", "/nonexistent", 1);

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int null = open("/dev/null", O_WRONLY);
    if (saved >= 0 && null >= 0)
        dup2(null, STDOUT_FILENO);
    if (null >= 0)
        close(null);

    ui_init();
    viewport_begin();
    box = prompt_new(NULL, 0);
    chrome_bind(box);

    voice_on_heard(heard, box);
    voice_on_draft(line, box);
    voice_on_release(release, box);
    voice_on_claim(claim, box);
    prompt_set_discard(box, discard, NULL);

    if (!workspace_begin(&a, 0) || workspace_open(&b) < 0) {
        fail("open two tabs");
    } else {
        check_live_roundtrip();
        check_typed_prefix_submit();
        check_reported_sequence();
        check_trace();
        check_switch_mid_sentence();
        check_switch_between_utterances();
        check_caret_kept();
        check_typed_before_resumed_words();
        check_typed_inside_dictation();
        check_ctrl_c_mid_utterance();
        check_ctrl_c_between_utterances();
        check_ctrl_c_then_dropped();
        check_ctrl_c_revised();
        check_ctrl_c_tab_round_trip();
        check_ctrl_c_keeps_typed_edit();
        check_escape_drops_dictation();
    }

    while (workspace_count())
        workspace_close(0);
    workspace_end();
    chrome_bind(NULL);
    prompt_free(box);
    viewport_end();

    if (saved >= 0) {
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    if (failures)
        return 1;
    puts("voicetabtest: ok");
    return 0;
}
