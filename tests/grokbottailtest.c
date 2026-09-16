#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grokbottail.h"
#include "md.h"
#include "prompt.h"
#include "session.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/agents/grokbot/grokbot.h"

static int  failures;
static char log_buf[4096];

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static void logf_(const char *tag, const char *text)
{
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof log_buf - n, "%s:%s|", tag, text);
}

void prompt_echo_message(const char *text) { logf_("user", text); }
void md_render_kept(const char *text, int indent) { (void)indent; logf_("bot", text); }
const char *ui_style(enum ui_role role) { (void)role; return ""; }
void ui_bar(const char *style, const char *fmt, ...)
{
    (void)style;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    logf_("bar", line);
}
void ui_error(const char *fmt, ...) { (void)fmt; }
void ui_flush(void) {}
unsigned viewport_item_begin(const struct viewport_entry *e) { (void)e; return 0; }
void viewport_item_end(void) {}

const char *session_backend(const struct session *s) { (void)s; return "grokbot"; }
const char *session_model(const struct session *s) { (void)s; return "Reginald"; }
int session_tail_mark(const struct session *s, const char *bot, struct grokbottail_mark *out)
{
    (void)s; (void)bot; (void)out;
    return 0;
}
void session_set_tail_mark(struct session *s, const char *bot,
                           const struct grokbottail_mark *mark)
{
    (void)s; (void)bot; (void)mark;
}
grokbot *grokbot_open(const grokbot_opts *o) { (void)o; return NULL; }
void grokbot_close(grokbot *g) { (void)g; }
const char *grokbot_error(grokbot *g) { (void)g; return ""; }
cJSON *grokbot_transcript_tail(grokbot *g, const char *ref, int limit)
{
    (void)g; (void)ref; (void)limit;
    return NULL;
}

static const char *TAIL =
    "{\"entries\":["
    "{\"kind\":\"message\",\"id\":\"t1u\",\"role\":\"user\",\"content\":\"hi\","
    " \"timestampMs\":1000000},"
    "{\"kind\":\"send-message\",\"id\":\"t1s0\",\"message\":{\"content\":\"hello\"},"
    " \"timestampMs\":1001000},"
    "{\"kind\":\"event\",\"id\":\"e1\",\"timestampMs\":1001500},"
    "{\"kind\":\"message\",\"id\":\"out1\",\"role\":\"assistant\",\"content\":\"fyi\","
    " \"timestampMs\":1002000,\"toAgent\":{\"name\":\"BCG\"}},"
    "{\"kind\":\"message\",\"id\":\"t2u\",\"role\":\"user\",\"content\":\"note\","
    " \"timestampMs\":1003000,\"fromAgent\":{\"name\":\"Yossi\"}},"
    "{\"kind\":\"send-message\",\"id\":\"t2s0\",\"message\":{\"content\":\"noted\"},"
    " \"timestampMs\":1004000}"
    "]}";

int main(void)
{
    cJSON *tail = cJSON_Parse(TAIL);
    double now = 1004000 + 5 * 60 * 1000;

    struct grokbottail_mark mark = {0};
    int n = grokbottail_render(tail, "Reginald", NULL, now, &mark);
    expect(n == 5, "full render draws every message entry");
    expect(!strcmp(mark.id, "t2s0") && mark.ms == 1004000, "mark is the newest entry");
    expect(strstr(log_buf, "user:hi|") != NULL, "user send drawn as a user turn");
    expect(strstr(log_buf, "bar:Reginald \xe2\x86\x92 BCG \xc2\xb7 5m ago|bot:fyi|") != NULL,
           "outbound message labeled with recipient");
    expect(strstr(log_buf, "bar:Yossi \xe2\x86\x92 Reginald \xc2\xb7 5m ago|bot:note|") != NULL,
           "inbound message labeled with sender");

    log_buf[0] = 0;
    struct grokbottail_mark since = {"out1", 1002000}, next = {0};
    n = grokbottail_render(tail, "Reginald", &since, now, &next);
    expect(n == 2, "render after a mark draws only newer entries");
    expect(strstr(log_buf, "fyi") == NULL, "entry at the mark is not repeated");

    log_buf[0] = 0;
    n = grokbottail_render(tail, "Reginald", &next, now, &next);
    expect(n == 0 && !log_buf[0], "render at the newest mark draws nothing");

    struct grokbottail_mark gone = {"aged-out", 1002500};
    n = grokbottail_render(tail, "Reginald", &gone, now, NULL);
    expect(n == 2, "unknown mark id falls back to its time");

    struct grokbottail_mark live = {"out1", 1003500};
    n = grokbottail_render(tail, "Reginald", &live, now, NULL);
    expect(n == 1, "entries already drawn live are skipped by time");

    cJSON_Delete(tail);
    if (!failures)
        printf("grokbottailtest: all checks passed\n");
    return failures != 0;
}
