#include "voice.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "prompt.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "vendor/agents/backend.h"
#include "vendor/macos_voice.h"
#include "workspace.h"

#ifndef VOICE_HELPER_PATH
#define VOICE_HELPER_PATH "VoiceHelper.app"
#endif

#define READY_WAIT_MS 30000
#define LINE_MAX_QUEUE 16
/* room for a dictation held across many turns */
#define LISTEN_MAX 8192
#define DROP_HOLD_MS 700
/* the hold after the draft is submitted by hand, where only the tail of the
   utterance already sent has to be swallowed */
#define SENT_HOLD_MS 200
/* AVSpeechUtteranceDefaultSpeechRate, what 100 percent means */
#define AV_RATE_DEFAULT 0.5

/* prefixed to spoken input while replies are read back, so the answer is
   shaped for the ear; the pane shows the words without it */
#define SPOKEN_PREAMBLE                                                        \
    "The message below was spoken aloud, and your reply will be read back "    \
    "aloud. Answer in a sentence or two of plain prose. No lists, code "       \
    "blocks, or long paths unless they are asked for.\n\n"

static macos_voice *voice;
static int          ready;
static int          speaking;
static int          speak = 1;
static int          armed = 1;
static int          mic = 1;
static int          dropping;
static long         drop_until;
static int          hearing;
/* draft heard while unarmed or dropping; the helper can re-emit it after
   focus, and that residue must not preview or send */
static char         stale[512];
static int          stale_hit;
static char         draft[512];
/* dictation opened by the wake word: every turn is held here until the
   terminator, so a long input is not cut up by pauses */
static int          listen_mode;
static char         listen_buf[LISTEN_MAX];
static char         failure[256];
static char        *queue[LINE_MAX_QUEUE];
static int          nqueue;
static char         label[32];
static void       (*heard_fn)(void *ud, const char *text);
static void        *heard_ud;
static const char *(*draft_fn)(void *ud);
static void        *draft_ud;
static void       (*release_fn)(void *ud);
static void        *release_ud;
/* the words heard for this utterance were erased in the box; what is left of
   it keeps arriving and is dropped rather than typed back in */
static int          erased;
/* the words last put in the input box; what is there now is measured against
   this to see whether they were changed by hand */
static char         shown[LISTEN_MAX];

static void listen_append(const char *text);

static void show(const char *text)
{
    snprintf(shown, sizeof shown, "%s", text ? text : "");
    heard_fn(heard_ud, text);
}

/* the box when it no longer holds the words put there; NULL while they are
   still in it. Text typed around them leaves them whole, and the prompt keeps
   that text beside the words on its own */
static const char *box_edit(void)
{
    if (!draft_fn || !shown[0])
        return NULL;
    const char *cur = draft_fn(draft_ud);
    if (!cur || strstr(cur, shown))
        return NULL;
    return cur;
}

/* 1 when the box holds text voice did not put there. A taken line is then
   folded into that draft rather than sent, and must not chime as if it left */
static int box_has_typed(void)
{
    const char *cur = draft_fn ? draft_fn(draft_ud) : NULL;
    if (!cur || !*cur)
        return 0;
    if (!shown[0])
        return 1;
    const char *found = strstr(cur, shown);
    if (!found)
        return 1;
    if (found > cur && !(found == cur + 1 && (cur[0] == ' ' || cur[0] == '\n')))
        return 1;
    return found[strlen(shown)] != '\0';
}

/* adopt an edit made in the box, which is what gets sent: a word deleted there
   is gone from the dictation, and an emptied box leaves nothing to send.
   1 when there was one, with the box copied to out */
static int take_edit(char *out, size_t n)
{
    const char *cur = box_edit();
    if (!cur)
        return 0;
    snprintf(out, n, "%s", cur);
    /* the words of an utterance still arriving must not be typed back over what
       is being erased, so that utterance is dropped. An edit made between two of
       them takes nothing with it: the dictation carries on from what is kept */
    erased = draft[0] != 0;
    if (listen_mode) {
        listen_buf[0] = '\0';
        listen_append(out);
    } else if (release_fn) {
        /* the line is theirs now, and what they kept is left to send by hand */
        release_fn(release_ud);
    }
    shown[0] = '\0';
    return 1;
}

static void heard(const char *text)
{
    char edit[LISTEN_MAX];
    if (heard_fn)
        take_edit(edit, sizeof edit);
    if (text && *text)
        snprintf(draft, sizeof draft, "%s", text);
    else
        draft[0] = '\0';
    if (!heard_fn)
        return;
    if (erased && !listen_mode)
        return;
    const char *tail = erased ? "" : draft;
    if (listen_mode && (listen_buf[0] || erased)) {
        char *joined = text_dsprintf("%s%s%s", listen_buf, tail[0] ? " " : "", tail);
        show(joined ? joined : listen_buf);
        free(joined);
        return;
    }
    show(text);
}

static void enqueue(const char *text)
{
    if (nqueue >= LINE_MAX_QUEUE)
        return;
    queue[nqueue++] = strdup(text);
}

static void clear_queue(void)
{
    for (int i = 0; i < nqueue; i++)
        free(queue[i]);
    nqueue = 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void hold_drop_for(int ms)
{
    dropping = 1;
    drop_until = now_ms() + ms;
}

static void hold_drop(void)
{
    hold_drop_for(DROP_HOLD_MS);
}

static int still_dropping(void)
{
    if (!dropping)
        return 0;
    if (now_ms() < drop_until)
        return 1;
    dropping = 0;
    return 0;
}

static void remember_stale(const char *text)
{
    if (text && *text)
        snprintf(stale, sizeof stale, "%s", text);
}

static void forget_stale(void)
{
    stale[0] = '\0';
    stale_hit = 0;
}

static int same_draft(const char *a, const char *b)
{
    if (!a || !b || !*a || !*b)
        return 0;
    size_t na = strlen(a), nb = strlen(b);
    if (na <= nb)
        return strncmp(a, b, na) == 0;
    return strncmp(b, a, nb) == 0;
}

static int is_stale(const char *text)
{
    if (!same_draft(stale, text))
        return 0;
    stale_hit = 1;
    return 1;
}

static int is_stop_command(const char *text)
{
    char words[256];
    size_t n = 0;
    int gap = 1;

    if (!text)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n + 1 < sizeof words; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (gap && n)
                words[n++] = ' ';
            if (n + 1 >= sizeof words)
                break;
            words[n++] = (char)c;
            gap = 0;
        } else if (c != '\'') {
            gap = 1;
        }
    }
    words[n] = '\0';
    if (!n)
        return 0;

    static const char *pad[] = {
        "okay", "ok", "please", "now", "hey", "um", "uh", "just",
    };
    char *start = words;
    for (;;) {
        int matched = 0;
        for (int i = 0; i < (int)(sizeof pad / sizeof pad[0]); i++) {
            size_t len = strlen(pad[i]);
            if (!strncmp(start, pad[i], len) && (start[len] == ' ' || !start[len])) {
                start += len;
                if (*start == ' ')
                    start++;
                matched = 1;
                break;
            }
        }
        if (!matched)
            break;
    }
    char *end = start + strlen(start);
    for (;;) {
        int matched = 0;
        while (end > start && end[-1] == ' ')
            *--end = '\0';
        for (int i = 0; i < (int)(sizeof pad / sizeof pad[0]); i++) {
            size_t len = strlen(pad[i]);
            if (end - start >= (ptrdiff_t)len && !memcmp(end - len, pad[i], len) &&
                (end - start == (ptrdiff_t)len || end[-len - 1] == ' ')) {
                end -= len;
                *end = '\0';
                matched = 1;
                break;
            }
        }
        if (!matched)
            break;
    }
    if (!*start)
        return 0;

    static const char *stops[] = {
        "stop", "wait", "quiet", "be quiet", "shush", "shh", "hush", "stop talking",
        "stop it", "enough", "thats enough", "shut up", "silence", "hold on",
        "one second",
    };
    for (int i = 0; i < (int)(sizeof stops / sizeof stops[0]); i++)
        if (!strcmp(start, stops[i]))
            return 1;
    return 0;
}

static int listening(void) { return armed && mic; }

static void chime(const char *name)
{
    if (voice && listening())
        macos_voice_chime(voice, name);
}

/* the word "listen" where it starts, or NULL when the turn does not carry it.
   The word is looked for anywhere, not just at the front: the microphone gate
   closes while a reply is read back, so a dictation opened over the tail of one
   reaches here with its first words already missing */
static const char *listen_wake(const char *text)
{
    for (const char *p = text; *p; p++) {
        if (p != text && isalnum((unsigned char)p[-1]))
            continue;
        if (strncasecmp(p, "listen", 6) || isalnum((unsigned char)p[6]))
            continue;
        return p;
    }
    return NULL;
}

/* the offset where word ends at end, or -1 */
static ptrdiff_t word_ends_at(const char *text, size_t end, const char *word)
{
    size_t len = strlen(word);
    if (end < len)
        return -1;
    size_t start = end - len;
    if (strncasecmp(text + start, word, len))
        return -1;
    if (start && isalnum((unsigned char)text[start - 1]))
        return -1;
    return (ptrdiff_t)start;
}

/* strips a closing "ok done"; 1 when it was there. Only the very end counts,
   so "okay, done with that" mid-thought does not cut the dictation short */
static int listen_end(char *text)
{
    size_t n = strlen(text);
    while (n && !isalnum((unsigned char)text[n - 1]))
        n--;
    ptrdiff_t done = word_ends_at(text, n, "done");
    if (done < 0)
        return 0;
    size_t e = (size_t)done;
    while (e && !isalnum((unsigned char)text[e - 1]))
        e--;
    ptrdiff_t start = word_ends_at(text, e, "ok");
    if (start < 0)
        start = word_ends_at(text, e, "okay");
    if (start < 0)
        return 0;
    text[start] = '\0';
    return 1;
}

static void listen_append(const char *text)
{
    while (*text == ' ')
        text++;
    size_t len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\n'))
        len--;
    if (!len)
        return;
    size_t n = strlen(listen_buf);
    if (n + len + 2 > sizeof listen_buf)
        return;
    snprintf(listen_buf + n, sizeof listen_buf - n, "%s%.*s", n ? " " : "", (int)len, text);
}

static void listen_clear(void)
{
    listen_mode = 0;
    listen_buf[0] = '\0';
}

/* end the dictation with tail as its last utterance and queue the whole thing */
static void listen_flush(const char *tail)
{
    char body[sizeof draft];
    if (tail && *tail) {
        snprintf(body, sizeof body, "%s", tail);
        listen_end(body);
        listen_append(body);
    }
    listen_mode = 0;
    if (listen_buf[0])
        enqueue(listen_buf);
    listen_buf[0] = '\0';
}

/* holds a finished turn in the dictation; 0 when this turn is not one */
static int listen_take(const char *text)
{
    const char *body = text;

    if (!listen_mode) {
        body = listen_wake(text);
        if (!body)
            return 0;
        listen_mode = 1;
        listen_buf[0] = '\0';
        chime("listening");
    }
    /* the dictation starts at the wake word rather than after it: the entry then shows
       the words as they were said, and an opening turn carrying nothing else leaves the
       word standing instead of an empty entry. What precedes it is gate residue and goes */
    char rest[sizeof draft];
    snprintf(rest, sizeof rest, "%s", body);
    if (listen_end(rest)) {
        listen_flush(rest);
        return 1;
    }
    listen_append(rest);
    return 1;
}

static void send_line(struct session *s, const char *line)
{
    int tab = workspace_index_of(s);
    if (tab < 0 || !line || !*line)
        return;
    if (!session_turn_running(s))
        prompt_echo_message(line);
    char *full = voice_with_preamble(line);
    workspace_send(tab, full ? full : line, full ? line : NULL);
    free(full);
    chime("sent");
}

static void discard_speech(const char *text)
{
    remember_stale(text);
    hearing = 0;
    if (listening())
        heard("");
}

static void on_event(void *ud, const char *kind, const char *text)
{
    (void)ud;
    if (!strcmp(kind, "ready")) {
        ready = 1;
    } else if (!strcmp(kind, "partial")) {
        if (!listening()) {
            discard_speech(text);
            return;
        }
        if (erased) {
            hearing = 0;
            return;
        }
        if (text && *text && (still_dropping() || is_stale(text))) {
            discard_speech(text);
            return;
        }
        if (text && *text) {
            forget_stale();
            hearing = 1;
            heard(text);
            return;
        }
        hearing = 0;
        heard("");
        if (stale_hit)
            forget_stale();
    } else if (!strcmp(kind, "final")) {
        if (!listening()) {
            discard_speech(text);
            return;
        }
        char edit[LISTEN_MAX];
        take_edit(edit, sizeof edit);
        hearing = 0;
        heard("");
        if (erased) {
            erased = 0;
            /* the turn being erased is gone, but a terminator in it still ends
               the dictation and sends what was kept */
            if (listen_mode) {
                char rest[sizeof draft];
                snprintf(rest, sizeof rest, "%s", text ? text : "");
                if (listen_end(rest))
                    listen_flush(NULL);
                heard("");
            }
            return;
        }
        /* a dictation outranks the hold, which is there to swallow the tail of
           a line already sent, not the words held for one */
        if (text && *text && !is_stale(text) &&
            (listen_mode || listen_wake(text))) {
            forget_stale();
            listen_take(text);
            heard("");
            return;
        }
        if (still_dropping() || is_stale(text)) {
            discard_speech(text);
            forget_stale();
            return;
        }
        forget_stale();
        if (text && *text && is_stop_command(text)) {
            struct session *s = workspace_current();
            if (s && session_turn_running(s))
                session_interrupt(s);
            chime("interrupted");
            return;
        }
        if (text && *text)
            enqueue(text);
    } else if (!strcmp(kind, "interrupt")) {
        if (!listening())
            return;
        clear_queue();
        listen_clear();
        hearing = 0;
        heard("");
        struct session *s = workspace_current();
        if (s && session_turn_running(s))
            session_interrupt(s);
        chime("interrupted");
    } else if (!strcmp(kind, "speaking")) {
        speaking = text && *text == '1';
        status_touch();
    } else if (!strcmp(kind, "notice")) {
        if (text && *text)
            status_set_alert(text);
    } else if (!strcmp(kind, "error")) {
        if (text && !strncmp(text, "unknown command:", 16))
            return;
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
    if (!speak || !armed)
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

static void wait_tick(void *ud)
{
    (void)ud;
    status_tick();
}

int voice_start(char *err, size_t size)
{
    if (voice)
        return 1;
    ready = 0;
    speaking = 0;
    /* a window that is not in front must not take the microphone from the one
       that is: every window starting at once after a restart would otherwise
       leave the last of them holding it */
    armed = tty_focused();
    mic = 1;
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    erased = 0;
    forget_stale();
    listen_clear();
    draft[0] = '\0';
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
        .rate        = voice_rate() / 100.0 * AV_RATE_DEFAULT,
        .silence     = voice_silence(),
        .volume      = voice_volume() / 100.0,
        .input       = settings_get_str(SETTING_VOICE_INPUT, NULL),
        .tick        = wait_tick,
    };
    voice = macos_voice_start(&opts, on_event, NULL);
    if (!voice) {
        snprintf(err, size, "could not launch %s", opts.helper_path);
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
        return 0;
    }
    armed = tty_focused();
    /* An inherited connection already has the correct helper-side ownership.
       Reasserting remembered terminal focus here would let restart order
       decide which instance gets the microphone. Only real focus edges claim it. */
    if (!macos_voice_resumed(voice)) {
        macos_voice_focus(voice, armed);
        if (armed)
            chime("listening");
    }
    macos_voice_volume(voice, voice_volume() / 100.0);
    macos_voice_rate(voice, voice_rate() / 100.0 * AV_RATE_DEFAULT);
    /* the helper may already be running for another client, with its silence */
    macos_voice_silence(voice, voice_silence());
    session_add_listener(on_session_event, NULL);
    return 1;
}

/* end_helper quits the shared process for every client, not just this one. */
static void teardown(int end_helper)
{
    if (!voice)
        return;
    session_remove_listener(on_session_event, NULL);
    macos_voice_focus(voice, 0);
    if (end_helper) {
        macos_voice_shutdown(voice);
        macos_voice_reap(settings_get_str(SETTING_VOICE_HELPER, VOICE_HELPER_PATH));
    } else {
        macos_voice_stop(voice);
    }
    voice = NULL;
    ready = 0;
    speaking = 0;
    armed = 1;
    mic = 1;
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    erased = 0;
    forget_stale();
    listen_clear();
    draft[0] = '\0';
    clear_queue();
    heard("");
    status_touch();
}

void voice_stop(void) { teardown(0); }

void voice_protect_handoff(void) { macos_voice_protect_handoff(); }

void voice_handoff(void)
{
    if (voice && macos_voice_handoff(voice) != 0)
        voice_stop();
}

int voice_restart(char *err, size_t size)
{
    int was_on = voice != NULL, was_speaking = speak;
    teardown(1);
    if (!was_on) {
        /* no client here to shut the helper down through, but one started by another
           client is still holding the microphone and the socket */
        macos_voice_reap(settings_get_str(SETTING_VOICE_HELPER, VOICE_HELPER_PATH));
        return 1;
    }
    if (!voice_start(err, size))
        return 0;
    voice_set_speak(was_speaking);
    return 1;
}

int voice_on(void) { return voice != NULL; }

void voice_set_speak(int on)
{
    speak = on ? 1 : 0;
    if (voice && !speak)
        macos_voice_mute(voice);
    status_touch();
}

int voice_speak(void) { return speak; }

static int clamp_volume(int n)
{
    if (n < 0)
        return 0;
    if (n > 100)
        return 100;
    return n;
}

int voice_volume(void)
{
    return clamp_volume(settings_get_int(SETTING_VOICE_VOLUME, VOICE_VOLUME_DEFAULT));
}

void voice_set_volume(int percent)
{
    percent = clamp_volume(percent);
    settings_set_int(SETTING_VOICE_VOLUME, percent);
    if (voice)
        macos_voice_volume(voice, percent / 100.0);
}

static int clamp_rate(int n)
{
    if (n < VOICE_RATE_MIN)
        return VOICE_RATE_MIN;
    if (n > VOICE_RATE_MAX)
        return VOICE_RATE_MAX;
    return n;
}

int voice_rate(void)
{
    return clamp_rate(settings_get_int(SETTING_VOICE_RATE, VOICE_RATE_DEFAULT));
}

void voice_set_rate(int percent)
{
    percent = clamp_rate(percent);
    settings_set_int(SETTING_VOICE_RATE, percent);
    if (voice)
        macos_voice_rate(voice, percent / 100.0 * AV_RATE_DEFAULT);
}

static double clamp_silence(double n)
{
    if (n < VOICE_SILENCE_MIN)
        return VOICE_SILENCE_MIN;
    if (n > VOICE_SILENCE_MAX)
        return VOICE_SILENCE_MAX;
    return n;
}

double voice_silence(void)
{
    double n = atof(settings_get_str(SETTING_VOICE_SILENCE, ""));
    return clamp_silence(n > 0 ? n : VOICE_SILENCE_DEFAULT);
}

void voice_set_silence(double seconds)
{
    seconds = clamp_silence(seconds);
    char text[32];
    snprintf(text, sizeof text, "%g", seconds);
    settings_set_str(SETTING_VOICE_SILENCE, text);
    if (voice)
        macos_voice_silence(voice, seconds);
}

int voice_apply(int on, int speak_on, char *err, size_t size)
{
    if (!on) {
        if (voice)
            voice_stop();
        settings_set_int(SETTING_VOICE, 0);
        return 1;
    }
    voice_set_speak(speak_on);
    settings_set_int(SETTING_VOICE_SPEAK, speak);
    if (!voice && !voice_start(err, size))
        return 0;
    settings_set_int(SETTING_VOICE, 1);
    return 1;
}

int voice_mic(void) { return voice && mic; }

void voice_set_mic(int on)
{
    on = on ? 1 : 0;
    if (on == mic)
        return;
    if (!on)
        voice_commit(workspace_current());
    mic = on;
    if (voice)
        macos_voice_mic(voice, on);
    if (on)
        chime("listening");
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    erased = 0;
    listen_clear();
    heard("");
    if (voice) {
        drain();
        macos_voice_cancel(voice);
    }
    status_touch();
}

void voice_on_heard(void (*fn)(void *ud, const char *text), void *ud)
{
    heard_fn = fn;
    heard_ud = ud;
}

void voice_on_draft(const char *(*fn)(void *ud), void *ud)
{
    draft_fn = fn;
    draft_ud = ud;
}

void voice_on_release(void (*fn)(void *ud), void *ud)
{
    release_fn = fn;
    release_ud = ud;
}

const char *voice_label(void)
{
    if (!voice)
        return NULL;
    if (!ready)
        snprintf(label, sizeof label, "voice starting");
    else if (!mic)
        snprintf(label, sizeof label, "voice mic off");
    else if (!armed)
        snprintf(label, sizeof label, "voice paused");
    else if (listen_mode)
        snprintf(label, sizeof label, "voice dictation");
    else if (!speak)
        snprintf(label, sizeof label, "voice listen");
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
    still_dropping();
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
    if (!box_has_typed())
        chime("sent");
    return line;
}

void voice_turn_begin(struct session *s)
{
    if (voice && s == workspace_current())
        macos_voice_busy(voice, 1);
}

void voice_turn_done(struct session *s)
{
    if (!voice)
        return;
    if (s == workspace_current()) {
        macos_voice_finish(voice);
        macos_voice_busy(voice, 0);
    }
    if (armed || !s || session_last_interrupted(s))
        return;
    if (!settings_get_int(SETTING_VOICE_COMPLETE, 1))
        return;
    const char *name = session_title(s);
    if (!name || !*name)
        name = APP_NAME;
    char line[192];
    snprintf(line, sizeof line, "%s complete", name);
    macos_voice_announce(voice, line);
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

void voice_commit(struct session *s)
{
    if (!voice || !s)
        return;
    drain();
    char edit[LISTEN_MAX];
    take_edit(edit, sizeof edit);
    if (listen_mode) {
        const char *tail = draft[0] && !is_stale(draft) ? draft : NULL;
        if (tail)
            remember_stale(tail);
        listen_flush(tail);
    } else if (erased) {
        erased = 0;
        if (draft[0])
            remember_stale(draft);
    } else if (draft[0] && !is_stale(draft)) {
        remember_stale(draft);
        if (is_stop_command(draft)) {
            if (session_turn_running(s))
                session_interrupt(s);
            chime("interrupted");
        } else {
            enqueue(draft);
        }
    }
    hearing = 0;
    heard("");
    /* A focus or workspace edge can commit speech before the prompt loop gets
       to take it.  If the box already has a typed draft, leave the speech in
       the queue so chat_line can merge it into that draft; sending here would
       bypass the box and make the model answer the speech on its own. */
    const char *composing = draft_fn ? draft_fn(draft_ud) : NULL;
    if (composing && *composing)
        return;
    while (nqueue > 0) {
        char *line = queue[0];
        for (int i = 1; i < nqueue; i++)
            queue[i - 1] = queue[i];
        nqueue--;
        send_line(s, line);
        free(line);
    }
}

void voice_draft_sent(void)
{
    if (!voice || !listening())
        return;
    /* the same words come back as a final once the turn endpoints; remember
       them so that copy is dropped instead of sent again */
    if (draft[0])
        remember_stale(draft);
    erased = 0;
    listen_clear();
    hold_drop_for(SENT_HOLD_MS);
    hearing = 0;
    heard("");
    drain();
    macos_voice_cancel(voice);
}

int voice_drop(void)
{
    if (!voice || !listening())
        return 0;
    /* The drop hold only swallows recognition residue after a cancellation; it
       is not itself voice input.  Reporting it as something dropped makes a
       quick second Escape renew the hold instead of reaching the active turn. */
    int had = nqueue > 0 || hearing || listen_mode;
    hold_drop();
    clear_queue();
    erased = 0;
    listen_clear();
    hearing = 0;
    forget_stale();
    heard("");
    drain();
    macos_voice_cancel(voice);
    return had;
}

/* Releasing ownership makes the helper hand back whatever it had heard. Wait
   for that answer, while this window is still armed to take it, so the words
   land in the window they were spoken in rather than the one being switched
   to. */
#define HANDOFF_WAIT_MS 40
static void await_handoff(void)
{
    if (!voice)
        return;
    if (macos_voice_poll(voice, HANDOFF_WAIT_MS) > 0)
        drain();
}

void voice_arm(int on)
{
    on = on ? 1 : 0;
    if (on == armed)
        return;
    if (on) {
        armed = 1;
        if (voice)
            macos_voice_focus(voice, 1);
    } else {
        if (voice)
            macos_voice_focus(voice, 0);
        await_handoff();
        voice_commit(workspace_current());
        armed = 0;
    }
    if (!voice)
        return;
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    heard("");
    if (armed) {
        struct session *s = workspace_current();
        macos_voice_busy(voice, s && session_turn_running(s));
    }
    status_touch();
}

char *voice_with_preamble(const char *line)
{
    if (!voice || !speak || !line || !*line)
        return NULL;
    return text_dsprintf("%s%s", SPOKEN_PREAMBLE, line);
}

int voice_speaking(void) { return voice && speaking; }

void voice_mute(void)
{
    if (!voice)
        return;
    macos_voice_mute(voice);
    speaking = 0;
    status_touch();
}
