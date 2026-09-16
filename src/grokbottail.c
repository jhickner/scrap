#include "grokbottail.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "md.h"
#include "prompt.h"
#include "session.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/agents/grokbot/grokbot.h"

static const char *str(const cJSON *o, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

static double entry_ms(const cJSON *e)
{
    const cJSON *t = cJSON_GetObjectItem(e, "timestampMs");
    return cJSON_IsNumber(t) ? t->valuedouble : 0;
}

static void when(double ms, double now_ms, char *out, size_t size)
{
    if (ms <= 0) {
        snprintf(out, size, "unknown time");
        return;
    }
    double s = (now_ms - ms) / 1000.0;
    if (s < 60)
        snprintf(out, size, "just now");
    else if (s < 3600)
        snprintf(out, size, "%dm ago", (int)(s / 60));
    else if (s < 86400)
        snprintf(out, size, "%dh ago", (int)(s / 3600));
    else if (s < 7 * 86400)
        snprintf(out, size, "%dd ago", (int)(s / 86400));
    else {
        time_t t = (time_t)(ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, size, "%b %e %H:%M", &tm);
    }
}

static void header(const char *label, const char *at)
{
    viewport_item_begin(VIEWPORT_ROWS(1, 0));
    if (label)
        ui_bar(ui_style(UI_DIM), "%s \xc2\xb7 %s", label, at);
    else
        ui_bar(ui_style(UI_DIM), "%s", at);
    viewport_item_end();
}

static int draw_entry(const cJSON *e, const char *bot, double now_ms)
{
    const char *kind = str(e, "kind");
    const char *text = NULL;
    const char *role = str(e, "role");
    char at[64], label[300];

    if (!kind)
        return 0;
    if (!strcmp(kind, "send-message")) {
        text = str(cJSON_GetObjectItem(e, "message"), "content");
        role = "assistant";
    } else if (!strcmp(kind, "message")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(e, "isStreaming")))
            return 0;
        text = str(e, "content");
    }
    if (!text || !*text || !role)
        return 0;

    when(entry_ms(e), now_ms, at, sizeof at);
    const char *from = str(cJSON_GetObjectItem(e, "fromAgent"), "name");
    const char *to = str(cJSON_GetObjectItem(e, "toAgent"), "name");

    if (!strcmp(role, "user") && !from) {
        header(NULL, at);
        prompt_echo_message(text);
        return 1;
    }
    if (!strcmp(role, "user") || (!strcmp(role, "assistant") && to)) {
        snprintf(label, sizeof label, "%s \xe2\x86\x92 %s", from ? from : bot,
                 to ? to : bot);
        header(label, at);
        md_render_kept(text, 0);
        return 1;
    }
    if (!strcmp(role, "assistant")) {
        header(bot, at);
        md_render_kept(text, 0);
        return 1;
    }
    return 0;
}

int grokbottail_render(const cJSON *tail, const char *bot,
                       const struct grokbottail_mark *since, double now_ms,
                       struct grokbottail_mark *mark)
{
    const cJSON *entries = cJSON_GetObjectItem(tail, "entries");
    if (!cJSON_IsArray(entries))
        return 0;

    int count = cJSON_GetArraySize(entries), start = 0;
    double floor_ms = 0;
    if (since && since->id[0]) {
        start = -1;
        for (int i = count - 1; i >= 0; i--) {
            const char *id = str(cJSON_GetArrayItem(entries, i), "id");
            if (id && !strcmp(id, since->id)) {
                start = i + 1;
                break;
            }
        }
        if (start < 0)
            start = 0;
    }
    if (since)
        floor_ms = since->ms;

    int drawn = 0;
    for (int i = 0; i < count; i++) {
        const cJSON *e = cJSON_GetArrayItem(entries, i);
        const char *id = str(e, "id");
        if (mark && id) {
            snprintf(mark->id, sizeof mark->id, "%s", id);
            if (entry_ms(e) > mark->ms)
                mark->ms = entry_ms(e);
        }
        if (i < start || (floor_ms > 0 && entry_ms(e) <= floor_ms))
            continue;
        drawn += draw_entry(e, bot ? bot : "bot", now_ms);
    }
    return drawn;
}

int grokbottail_applies(const struct session *s)
{
    const char *backend = s ? session_backend(s) : NULL;
    return backend && !strcmp(backend, "grokbot") && session_model(s);
}

static double wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000;
}

int grokbottail_show(struct session *s, int limit, int only_new)
{
    if (!grokbottail_applies(s))
        return -1;
    if (limit < 1)
        limit = GROKBOTTAIL_DEFAULT;
    if (limit > GROKBOTTAIL_MAX)
        limit = GROKBOTTAIL_MAX;

    const char *bot = session_model(s);
    grokbot *g = grokbot_open(NULL);
    cJSON *tail = g ? grokbot_transcript_tail(g, bot, limit) : NULL;
    if (!tail) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("grokbot: %s", g ? grokbot_error(g) : "gateway session unavailable");
        viewport_item_end();
        ui_flush();
        grokbot_close(g);
        return -1;
    }
    grokbot_close(g);

    struct grokbottail_mark since = {0}, mark = {0};
    int have = only_new && session_tail_mark(s, bot, &since);
    int drawn = grokbottail_render(tail, bot, have ? &since : NULL, wall_ms(), &mark);
    cJSON_Delete(tail);

    if (!mark.id[0] && have)
        mark = since;
    if (have && since.ms > mark.ms)
        mark.ms = since.ms;
    session_set_tail_mark(s, bot, &mark);
    ui_flush();
    return drawn;
}
