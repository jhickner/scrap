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
#include "voicetrace.h"
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
#define SPOKEN_BREVITY                                                         \
    "Reply in one or two short sentences of plain prose. Answer only what "    \
    "was asked: no background, caveats, alternatives, or offers of further "   \
    "help. No lists or paths unless they are asked for. Any command or "     \
    "code you give must be verbatim in a fenced code block, never spelled "  \
    "out or paraphrased for speech.\n\n"
#define SPOKEN_PREAMBLE                                                        \
    "The message below was spoken aloud, and your reply will be read back "    \
    "aloud. " SPOKEN_BREVITY
/* a queued message is answered after another reply, which a listener cannot
   tell apart from the one before it */
#define QUEUED_PREAMBLE                                                        \
    "The message below was spoken aloud while an earlier reply was running, "  \
    "and your reply will be read back aloud. Open with a few words restating " \
    "what it asks. " SPOKEN_BREVITY

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
static char         stale[LISTEN_MAX];
static int          stale_hit;
static char         draft[LISTEN_MAX];
/* dictation opened by the wake word: every turn is held here until the
   terminator, so a long input is not cut up by pauses */
static int          listen_mode;
static char         listen_buf[LISTEN_MAX];
/* open dictations of tabs not in front, resumed when their tab is again */
static struct {
    const struct session *s;
    char                 *text;
} held[WORKSPACE_MAX];
static int          nheld;
/* the utterance in progress as last heard, and the part of it spoken before
   the tab changed: the recognizer carries on with it, but only the rest
   belongs to the tab now in front */
static char         utter[LISTEN_MAX];
static char         carry[LISTEN_MAX];
/* set by the spoken pause command: the recognizer keeps running so the resume
   command is heard, and every other turn is discarded */
static int          paused;
/* the pause state was changed from a partial; the final of that turn is
   consumed without changing it again */
static int          command_early;
/* another client turned the mic off; applied once the helper poll returns */
static int          mic_off_pending;
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
static int        (*claim_fn)(void *ud, const char *text);
static void        *claim_ud;
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

/* Adopt edits to dictated words and typed continuations after them. A typed
   prefix can remain outside the span; it is included when the draft submits. */
static const char *box_edit(void)
{
    if (!draft_fn || !shown[0])
        return NULL;
    const char *cur = draft_fn(draft_ud);
    if (!cur)
        return NULL;
    const char *found = strstr(cur, shown);
    /* A typed continuation belongs before the next spoken utterance. Keeping
       only the old spoken span would insert new speech before that suffix. */
    if (found && !(listen_mode && found[strlen(shown)]))
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
        /* the box as it is, spacing included, so showing it again changes nothing */
        snprintf(listen_buf, sizeof listen_buf, "%s", out);
        if (claim_fn)
            claim_fn(claim_ud, out);
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
        size_t n = strlen(listen_buf);
        int gap = tail[0] && n && listen_buf[n - 1] != ' ' && listen_buf[n - 1] != '\n';
        char *joined = text_dsprintf("%s%s%s", listen_buf, gap ? " " : "", tail);
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
    voice_trace("voice.queue", "text=%s", text);
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

/* 1 when the turn, lowercased and without punctuation or filler words, is one
   of the phrases in list, possibly repeated */
static int is_command(const char *text, const char *const *list, int count)
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

    for (int i = 0; i < count; i++) {
        size_t len = strlen(list[i]);
        const char *p = start;
        while (!strncmp(p, list[i], len) && (p[len] == ' ' || !p[len])) {
            p += len;
            if (!*p)
                return 1;
            p++;
        }
    }
    return 0;
}

static int is_stop_command(const char *text)
{
    static const char *const stops[] = {
        "stop", "wait", "quiet", "be quiet", "shush", "shh", "hush", "stop talking",
        "stop it", "enough", "thats enough", "shut up", "silence", "hold on",
        "one second",
    };
    return is_command(text, stops, (int)(sizeof stops / sizeof stops[0]));
}

static int is_pause_command(const char *text)
{
    static const char *const words[] = { "pause", "pause listening" };
    return is_command(text, words, (int)(sizeof words / sizeof words[0]));
}

static int is_resume_command(const char *text)
{
    static const char *const words[] = { "resume", "resume listening" };
    return is_command(text, words, (int)(sizeof words / sizeof words[0]));
}

/* the two-word forms are acted on from a partial, ahead of the final and the
   endpoint silence; a bare word could still grow into a sentence */
static int is_early_pause_command(const char *text)
{
    static const char *const words[] = { "pause listening" };
    return is_command(text, words, 1);
}

static int is_early_resume_command(const char *text)
{
    static const char *const words[] = { "resume listening" };
    return is_command(text, words, 1);
}

static int listening(void) { return armed && mic; }

int voice_wake(void) { return settings_get_int(SETTING_VOICE_WAKE, 0); }

void voice_set_wake(int on)
{
    settings_set_int(SETTING_VOICE_WAKE, !!on);
    status_touch();
}

static void chime(const char *name)
{
    if (voice && listening())
        macos_voice_chime(voice, name);
}

static void set_paused(int on)
{
    paused = on;
    hearing = 0;
    heard("");
    chime(on ? "interrupted" : "listening");
    status_touch();
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
    voice_trace("voice.end-match", "offset=%td text=%s", start, text);
    text[start] = '\0';
    return 1;
}

/* strips a closing "cancel", "cancel this" or "cancel that"; 1 when it was there */
static int listen_cancel(char *text)
{
    size_t n = strlen(text);
    while (n && !isalnum((unsigned char)text[n - 1]))
        n--;
    ptrdiff_t start = word_ends_at(text, n, "this");
    if (start < 0)
        start = word_ends_at(text, n, "that");
    if (start >= 0) {
        size_t e = (size_t)start;
        while (e && !isalnum((unsigned char)text[e - 1]))
            e--;
        start = word_ends_at(text, e, "cancel");
    } else {
        start = word_ends_at(text, n, "cancel");
    }
    if (start < 0)
        return 0;
    voice_trace("voice.cancel-match", "offset=%td text=%s", start, text);
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
    int gap = n && listen_buf[n - 1] != ' ' && listen_buf[n - 1] != '\n';
    snprintf(listen_buf + n, sizeof listen_buf - n, "%s%.*s", gap ? " " : "", (int)len, text);
}

static void listen_clear(void)
{
    listen_mode = 0;
    listen_buf[0] = '\0';
}

static int held_find(const struct session *s)
{
    for (int i = 0; i < nheld; i++)
        if (held[i].s == s)
            return i;
    return -1;
}

static void held_remove(int i)
{
    free(held[i].text);
    held[i] = held[--nheld];
    held[nheld].s = NULL;
    held[nheld].text = NULL;
}

static void held_put(const struct session *s, const char *text)
{
    int i = held_find(s);
    if (i >= 0)
        held_remove(i);
    if (!s || nheld >= WORKSPACE_MAX)
        return;
    char *copy = strdup(text);
    if (!copy)
        return;
    held[nheld].s = s;
    held[nheld].text = copy;
    nheld++;
}

static void held_clear(void)
{
    while (nheld)
        held_remove(nheld - 1);
}

static void carry_clear(void)
{
    utter[0] = carry[0] = '\0';
}

static const char *skip_word(const char *p)
{
    while (*p && !isalnum((unsigned char)*p))
        p++;
    while (*p && !isspace((unsigned char)*p))
        p++;
    return p;
}

/* text past the words spoken before the tab changed. Case and punctuation the
   recognizer revised still match; a revised word further in is skipped by
   count, as long as the first word is the same. Text that does not start the
   same way is a new utterance and is left whole */
static const char *after_carry(const char *text)
{
    if (!carry[0] || !text)
        return text;
    const char *c = carry, *p = text;
    for (;;) {
        while (*c && !isalnum((unsigned char)*c))
            c++;
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*c || !*p || tolower((unsigned char)*c) != tolower((unsigned char)*p))
            break;
        c++;
        p++;
    }
    if (*c) {
        const char *cw = carry, *tw = text;
        while (*cw && !isalnum((unsigned char)*cw))
            cw++;
        while (*tw && !isalnum((unsigned char)*tw))
            tw++;
        size_t n = strcspn(cw, " \t\n");
        if (strcspn(tw, " \t\n") != n || strncasecmp(cw, tw, n))
            return text;
        p = text;
        for (c = carry;;) {
            while (*c && !isalnum((unsigned char)*c))
                c++;
            if (!*c)
                break;
            while (*c && !isspace((unsigned char)*c))
                c++;
            p = skip_word(p);
        }
    } else if (isalnum((unsigned char)*p) && p > text && isalnum((unsigned char)p[-1])) {
        /* the carried text ended inside a word the recognizer has since finished */
        while (*p && !isspace((unsigned char)*p))
            p++;
    }
    while (*p && (isspace((unsigned char)*p) || ispunct((unsigned char)*p)))
        p++;
    return p;
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
    size_t n = strlen(listen_buf);
    while (n && isspace((unsigned char)listen_buf[n - 1]))
        listen_buf[--n] = '\0';
    /* The terminator submits the whole input, including typed text before
       the dictation. Materialize the final words before taking that snapshot,
       then claim the whole draft so clearing it leaves no typed residue for
       chat_line to mistake for an unfinished composition. */
    if (heard_fn)
        show(listen_buf);
    const char *cur = draft_fn ? draft_fn(draft_ud) : NULL;
    char *line = strdup(cur ? cur : listen_buf);
    if (cur && claim_fn)
        claim_fn(claim_ud, cur);
    listen_mode = 0;
    listen_buf[0] = '\0';
    if (heard_fn)
        show("");
    if (line && *line)
        enqueue(line);
    free(line);
}

/* end the dictation and drop everything in it, the box included */
static void listen_discard(void)
{
    if (heard_fn)
        show(listen_buf);
    const char *cur = draft_fn ? draft_fn(draft_ud) : NULL;
    if (cur && claim_fn)
        claim_fn(claim_ud, cur);
    listen_mode = 0;
    listen_buf[0] = '\0';
    if (heard_fn)
        show("");
    chime("interrupted");
    /* the recognizer starts over, so the cancelled utterance does not carry
       into the next wake word */
    if (voice && !speaking)
        macos_voice_cancel(voice);
}

/* holds a finished turn in the dictation; 0 when this turn is not one. cue
   chimes when the turn opens it */
static int listen_take(const char *text, int cue)
{
    const char *body = text;

    if (!listen_mode) {
        body = listen_wake(text);
        if (!body)
            return 0;
        listen_mode = 1;
        listen_buf[0] = '\0';
        if (cue)
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
    if (listen_cancel(rest)) {
        listen_discard();
        return 1;
    }
    listen_append(rest);
    /* Recognition can endpoint between "ok" and "done". Match the joined
       dictation too, so a pause inside the closing phrase still sends. */
    if (voice_wake() && listen_end(listen_buf))
        listen_flush(NULL);
    else if (voice_wake() && listen_cancel(listen_buf))
        listen_discard();
    return 1;
}

/* the utterance in progress joins the dictation it belongs to, as if its final
   had arrived; 0 when it is not part of one */
static int listen_hold_draft(void)
{
    if (!draft[0] || erased)
        return 0;
    char text[sizeof draft];
    snprintf(text, sizeof text, "%s", draft);
    draft[0] = '\0';
    return listen_take(text, 0);
}

static void send_line(struct session *s, const char *line)
{
    int tab = workspace_index_of(s);
    if (tab < 0 || !line || !*line)
        return;
    if (!session_turn_running(s))
        prompt_echo_message(line);
    char *full = voice_with_preamble(line, session_turn_running(s));
    workspace_send(tab, full ? full : line, full ? line : NULL);
    free(full);
    chime("sent");
}

/* Discarded speech is not remembered as stale: remembering each partial would
   fold every longer partial into the stale prefix, and an utterance whose
   first words landed in a drop hold would then be swallowed whole. */
static void discard_speech(void)
{
    hearing = 0;
    if (listening())
        heard("");
}

static void handle_event(void *ud, const char *kind, const char *text)
{
    (void)ud;
    if (!strcmp(kind, "ready")) {
        ready = 1;
    } else if (!strcmp(kind, "partial")) {
        if (!listening()) {
            remember_stale(text);
            discard_speech();
            return;
        }
        snprintf(utter, sizeof utter, "%s", text ? text : "");
        text = after_carry(text);
        if (voice_wake() && !listen_mode && !paused) {
            text = text ? listen_wake(text) : NULL;
            if (!text) {
                hearing = 0;
                heard("");
                return;
            }
        }
        if (paused) {
            hearing = 0;
            if (!command_early && is_early_resume_command(text)) {
                command_early = 1;
                set_paused(0);
            }
            return;
        }
        if (!command_early && is_early_pause_command(text)) {
            command_early = 1;
            erased = 0;
            forget_stale();
            set_paused(1);
            return;
        }
        /* a command word is not previewed; it shows once the turn grows past it */
        if (is_pause_command(text) || is_resume_command(text)) {
            hearing = 1;
            heard("");
            return;
        }
        if (erased) {
            hearing = 0;
            return;
        }
        if (text && *text && (still_dropping() || is_stale(text))) {
            discard_speech();
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
            carry_clear();
            remember_stale(text);
            discard_speech();
            return;
        }
        text = after_carry(text);
        carry_clear();
        if (voice_wake() && !listen_mode && !paused) {
            text = text ? listen_wake(text) : NULL;
            if (!text) {
                hearing = 0;
                heard("");
                if (voice && !speaking)
                    macos_voice_cancel(voice);
                return;
            }
        }
        /* the helper waits for a reply to every turn it delivers and judges
           speech as echo until one ends; a turn consumed here gets none. A reply
           still playing is left to end it, rather than being cut off */
        if (command_early) {
            command_early = 0;
            if (is_pause_command(text) || is_resume_command(text)) {
                if (voice && !speaking)
                    macos_voice_cancel(voice);
                return;
            }
        }
        if (paused) {
            if (voice && !speaking)
                macos_voice_cancel(voice);
            if (is_resume_command(text))
                set_paused(0);
            return;
        }
        char edit[LISTEN_MAX];
        take_edit(edit, sizeof edit);
        hearing = 0;
        heard("");
        if (is_pause_command(text) || is_resume_command(text)) {
            erased = 0;
            forget_stale();
            if (voice && !speaking)
                macos_voice_cancel(voice);
            set_paused(is_pause_command(text));
            return;
        }
        if (erased) {
            erased = 0;
            /* the turn being erased is gone, but a terminator in it still ends
               the dictation and sends what was kept */
            if (listen_mode) {
                char rest[sizeof draft];
                snprintf(rest, sizeof rest, "%s", text ? text : "");
                if (listen_end(rest))
                    listen_flush(NULL);
                else if (listen_cancel(rest))
                    listen_discard();
                heard("");
                if (voice_wake() && listen_mode && voice && !speaking)
                    macos_voice_cancel(voice);
            }
            return;
        }
        /* a dictation outranks the hold, which is there to swallow the tail of
           a line already sent, not the words held for one */
        if (text && *text && !is_stale(text) &&
            (listen_mode || listen_wake(text))) {
            forget_stale();
            listen_take(text, 1);
            heard("");
            if (voice_wake() && listen_mode && voice && !speaking)
                macos_voice_cancel(voice);
            return;
        }
        if (still_dropping() || is_stale(text)) {
            discard_speech();
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
        if (!listening() || paused)
            return;
        clear_queue();
        listen_clear();
        hearing = 0;
        heard("");
        struct session *s = workspace_current();
        if (s && session_turn_running(s))
            session_interrupt(s);
        chime("interrupted");
        erased = 0;
    } else if (!strcmp(kind, "dropped")) {
        /* the erased turn ends here when the helper discards its final */
        carry_clear();
        discard_speech();
        erased = 0;
    } else if (!strcmp(kind, "cancelled")) {
        /* the helper dropped the turn on its own cancel phrase; in a dictation
           that phrase cancels the whole dictation, not just the last turn */
        carry_clear();
        erased = 0;
        if (listening() && listen_mode) {
            draft[0] = '\0';
            listen_discard();
        }
        discard_speech();
    } else if (!strcmp(kind, "speaking")) {
        speaking = text && *text == '1';
        status_touch();
    } else if (!strcmp(kind, "mic")) {
        if (text && !strcmp(text, "0"))
            mic_off_pending = 1;
    } else if (!strcmp(kind, "notice")) {
        if (text && *text)
            status_set_alert(text);
    } else if (!strcmp(kind, "error")) {
        if (text && !strncmp(text, "unknown command:", 16))
            return;
        snprintf(failure, sizeof failure, "%s", text ? text : "helper failed");
    }
}

static void on_event(void *ud, const char *kind, const char *text)
{
    voice_trace("helper.event", "tab=%p kind=%s text=%s listen=%d erased=%d carry=%s",
                (void *)workspace_current(), kind, text ? text : "", listen_mode, erased, carry);
    handle_event(ud, kind, text);
    voice_trace("voice.state", "tab=%p listen=%d erased=%d armed=%d paused=%d queue=%d draft=%s held=%s shown=%s",
                (void *)workspace_current(), listen_mode, erased, armed, paused, nqueue,
                draft, listen_buf, shown);
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
        return;
    }
    if (mic_off_pending) {
        mic_off_pending = 0;
        voice_mic_off(0);
    }
}

static int cancelled;

static int cancel_pressed(void)
{
    if (!tty_is_raw())
        return 0;
    tty_event ev;
    while (tty_read(&ev, 0)) {
        if (ev.key == TK_TEXT)
            free(ev.text);
        if (ev.key == TK_ESCAPE || (ev.key == TK_CHAR && ev.cp == 3) || ev.key == TK_EOF)
            cancelled = 1;
    }
    return cancelled;
}

static int wait_tick(void *ud)
{
    (void)ud;
    status_tick();
    return cancel_pressed();
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
    paused = 0;
    command_early = 0;
    forget_stale();
    listen_clear();
    held_clear();
    carry_clear();
    draft[0] = '\0';
    failure[0] = '\0';

    cancelled = 0;
    int owned = status_work_begin("starting voice");

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
        if (cancelled)
            snprintf(err, size, "start cancelled");
        else
            snprintf(err, size, "could not launch %s", opts.helper_path);
        status_work_end(owned);
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
        if (cancel_pressed()) {
            snprintf(failure, sizeof failure, "start cancelled");
            break;
        }
    }
    status_work_end(owned);
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
    paused = 0;
    command_early = 0;
    forget_stale();
    listen_clear();
    held_clear();
    carry_clear();
    draft[0] = '\0';
    mic_off_pending = 0;
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
    if (!on) {
        voice_commit(workspace_current());
        /* the dictation ends here, and what it held stays in the box */
        if (listen_mode && release_fn)
            release_fn(release_ud);
    }
    mic = on;
    if (voice)
        macos_voice_mic(voice, on);
    if (on)
        chime("listening");
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    erased = 0;
    paused = 0;
    command_early = 0;
    listen_clear();
    carry_clear();
    heard("");
    if (voice) {
        drain();
        macos_voice_cancel(voice);
    }
    status_touch();
}

void voice_mic_off(int every)
{
    if (!voice_mic())
        return;
    voice_set_mic(0);
    status_set_note("mic off");
    if (every && voice)
        macos_voice_mic_off_all(voice);
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

void voice_on_claim(int (*fn)(void *ud, const char *text), void *ud)
{
    claim_fn = fn;
    claim_ud = ud;
}

const char *voice_label(void)
{
    if (!voice)
        return NULL;
    if (!ready)
        snprintf(label, sizeof label, "voice starting");
    else if (!mic)
        snprintf(label, sizeof label, "voice mic off");
    else if (paused)
        snprintf(label, sizeof label, "voice paused");
    else if (!armed)
        snprintf(label, sizeof label, "voice unfocused");
    else if (listen_mode)
        snprintf(label, sizeof label, "voice dictation");
    else if (voice_wake())
        snprintf(label, sizeof label, "voice: say listen");
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
    voice_trace("voice.take", "tab=%p typed=%d text=%s", (void *)workspace_current(), box_has_typed(), line);
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
    int i = s ? held_find(s) : -1;
    if (i < 0)
        return;
    /* the box came back with the tab; its dictation carries on where it was
       unless those words are no longer there */
    if (!claim_fn || claim_fn(claim_ud, held[i].text)) {
        listen_mode = 1;
        snprintf(listen_buf, sizeof listen_buf, "%s", held[i].text);
        snprintf(shown, sizeof shown, "%s", held[i].text);
    }
    voice_trace("voice.resume", "tab=%p listen=%d text=%s", (void *)s, listen_mode, held[i].text);
    held_remove(i);
    status_touch();
}

void voice_leave(struct session *s)
{
    if (!voice)
        return;
    drain();
    char edit[LISTEN_MAX];
    take_edit(edit, sizeof edit);
    snprintf(carry, sizeof carry, "%s", utter);
    if (listen_mode || listen_wake(draft))
        listen_hold_draft();
    /* A wake-word partial only became an open dictation above. Adopt any
       typed continuation now, before its span is parked for this tab. */
    take_edit(edit, sizeof edit);
    draft[0] = '\0';
    hearing = erased = command_early = 0;
    if (heard_fn && listen_mode)
        show(listen_buf);
    /* Finals awaiting chat_line belong before the current preview. Materialize
       them in this box before workspace saves it, never in the global queue
       that the next tab would take. Keep any surrounding typed text intact. */
    if (nqueue && heard_fn) {
        char *text = strdup("");
        for (int i = 0; text && i < nqueue; i++) {
            char *next = text_dsprintf("%s%s%s", text, *text ? " " : "", queue[i]);
            free(text);
            text = next;
        }
        if (text) {
            char *next = text_dsprintf("%s%s%s", text, shown[0] ? " " : "", shown);
            if (next) {
                show(next);
                free(next);
            }
            free(text);
        }
    }
    voice_trace("voice.leave", "tab=%p listen=%d carry=%s held=%s", (void *)s, listen_mode, carry, listen_buf);
    if (listen_mode)
        held_put(s, listen_buf);
    if (release_fn)
        release_fn(release_ud);
    clear_queue();
    listen_clear();
    shown[0] = '\0';
    status_touch();
}

void voice_forget(const struct session *s)
{
    int i = held_find(s);
    if (i >= 0)
        held_remove(i);
}

void voice_commit(struct session *s)
{
    if (!voice || !s)
        return;
    drain();
    char edit[LISTEN_MAX];
    take_edit(edit, sizeof edit);
    if (erased) {
        erased = 0;
        if (draft[0])
            remember_stale(draft);
    } else if (draft[0] && !is_stale(draft)) {
        remember_stale(draft);
        /* A wake word can still be only a partial when focus or the mic changes.
           Neither edge is the dictation's end phrase, so it stays open */
        if (listen_mode || listen_wake(draft))
            listen_hold_draft();
        else if (!voice_wake() && is_stop_command(draft)) {
            if (session_turn_running(s))
                session_interrupt(s);
            chime("interrupted");
        } else if (!voice_wake()) {
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

static void drop_input(void)
{
    int cut = draft[0] != 0;
    hold_drop();
    clear_queue();
    erased = 0;
    listen_clear();
    hearing = 0;
    /* the cancelled utterance keeps arriving as ever-longer partials and a
       final; its words so far are the prefix that marks them stale */
    forget_stale();
    remember_stale(draft);
    heard("");
    /* revised words no longer match that prefix: the rest of the utterance is
       dropped until its final */
    erased = cut;
    drain();
}

int voice_drop(void)
{
    if (!voice || !listening())
        return 0;
    /* The drop hold only swallows recognition residue after a cancellation; it
       is not itself voice input.  Reporting it as something dropped makes a
       quick second Escape renew the hold instead of reaching the active turn. */
    int had = nqueue > 0 || hearing || listen_mode;
    drop_input();
    macos_voice_cancel(voice);
    return had;
}

int voice_discard(void)
{
    if (!voice || !listening())
        return 0;
    if (!listen_mode && !hearing && !draft[0] && !shown[0] && !nqueue)
        return 0;
    drop_input();
    if (!speaking)
        macos_voice_cancel(voice);
    return 1;
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
    int changed = on != armed;
    /* Every edge reasserts the claim, whether or not this window thought it had
       focus: the helper routes speech to the client that asked for it last, and
       a window that missed its focus-out would otherwise never ask again. */
    if (on) {
        armed = 1;
        if (voice)
            macos_voice_focus(voice, 1);
    } else {
        if (voice)
            macos_voice_focus(voice, 0);
        if (changed) {
            await_handoff();
            voice_commit(workspace_current());
        }
        armed = 0;
    }
    if (!voice || !changed)
        return;
    dropping = 0;
    drop_until = 0;
    hearing = 0;
    carry_clear();
    heard("");
    if (armed) {
        struct session *s = workspace_current();
        macos_voice_busy(voice, s && session_turn_running(s));
    }
    status_touch();
}

char *voice_with_preamble(const char *line, int queued)
{
    if (!voice || !speak || !line || !*line)
        return NULL;
    return text_dsprintf("%s%s", queued ? QUEUED_PREAMBLE : SPOKEN_PREAMBLE, line);
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
