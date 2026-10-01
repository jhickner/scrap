#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "session.h"
#include "settings.h"
#include "status.h"
#include "tty.h"
#include "vendor/agents/backend.h"
#include "voice.h"
#include "workspace.h"

struct session {
    int interrupted;
};

static struct session front, back;
static struct session *current = &front;
static int focused = 1;
static int voice_setting;
static session_listener_fn listener;
static char logpath[256];
static int fails;

int settings_get_int(const char *key, int fallback)
{
    return !strcmp(key, SETTING_VOICE) ? voice_setting : fallback;
}
const char *settings_get_str(const char *key, const char *fallback)
{
    return !strcmp(key, SETTING_VOICE_NAME) ? "Jason" : fallback;
}
void settings_set_int(const char *key, int value)
{
    if (!strcmp(key, SETTING_VOICE))
        voice_setting = value;
}
void status_touch(void) {}
int  tty_focused(void) { return focused; }
struct session *workspace_current(void) { return current; }
int  session_last_interrupted(const struct session *s) { return s->interrupted; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
int  session_add_listener(session_listener_fn fn, void *ud)
{
    (void)ud;
    listener = fn;
    return 1;
}

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    fails++;
}

static void reply(struct session *s, const char *text)
{
    backend_event ev = {.kind = BACKEND_EV_ASSISTANT, .text = text};
    listener(NULL, s, &ev);
}

static void settle(void)
{
    struct timespec tick = {0, 10 * 1000 * 1000};
    for (int i = 0; i < 300 && voice_speaking(); i++) {
        nanosleep(&tick, NULL);
        voice_pending();
    }
}

static char *spoken(void)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(logpath, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    unlink(logpath);
    return buf;
}

static void expect_silent(const char *what)
{
    settle();
    if (*spoken())
        fail(what);
}

static void expect_spoken(const char *what, const char *text)
{
    settle();
    if (!strstr(spoken(), text))
        fail(what);
}

int main(void)
{
    char dir[] = "/tmp/voicetestXXXXXX";
    if (!mkdtemp(dir))
        return 1;
    snprintf(logpath, sizeof logpath, "%s/log", dir);
    char script[300];
    snprintf(script, sizeof script, "%s/pvsay", dir);
    FILE *f = fopen(script, "w");
    if (!f)
        return 1;
    fprintf(f, "#!/bin/sh\n{ echo \"argv: $*\"; cat; } >> %s\n"
               "case \"$(cat %s)\" in *slow*) exec sleep 30;; esac\n", logpath, logpath);
    fclose(f);
    chmod(script, 0755);
    char path[600];
    snprintf(path, sizeof path, "%s:%s", dir, getenv("PATH"));
    setenv("PATH", path, 1);

    voice_init();
    reply(&front, "off");
    expect_silent("voice off says nothing");
    if (voice_label())
        fail("voice off has no label");

    voice_set_on(1);
    if (!voice_setting)
        fail("turning voice on saves the setting");
    reply(&front, "hello **there**");
    expect_spoken("a reply on the current tab is spoken",
                  "argv: --markdown -v Jason -r 175 --volume 1.00\nhello **there**");

    reply(&back, "elsewhere");
    expect_silent("a reply on another tab says nothing");

    voice_arm(0);
    reply(&front, "unfocused");
    expect_silent("a reply while unfocused says nothing");
    voice_turn_done(&front);
    expect_spoken("an unfocused turn end is announced", "tab complete");
    front.interrupted = 1;
    voice_turn_done(&front);
    expect_silent("an interrupted turn end is not announced");
    front.interrupted = 0;
    voice_arm(1);

    reply(&front, "slow");
    if (!voice_speaking())
        fail("a reply in progress is speaking");
    voice_mute();
    if (voice_speaking())
        fail("mute stops the speaker");
    spoken();
    reply(&front, "muted");
    expect_silent("the rest of a muted reply says nothing");
    voice_turn_begin(&front);
    reply(&front, "next turn");
    expect_spoken("mute ends with the next turn", "next turn");

    char *full = voice_with_preamble("hi");
    if (!full || strncmp(full, "Your reply will be read aloud", 29) || !strstr(full, "\n\nhi"))
        fail("the preamble leads the line");
    free(full);

    voice_set_on(0);
    if (voice_with_preamble("hi"))
        fail("voice off adds no preamble");

    unlink(script);
    rmdir(dir);
    if (fails)
        return 1;
    printf("voicetest: ok\n");
    return 0;
}
