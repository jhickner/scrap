#include "vncsource.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/agents/grokbot/grokbot.h"
#include "vendor/vnc/grokvnc.h"

#define UPDATE_INTERVAL_MS ((int)(VNCINSET_FRAME_INTERVAL * 1000))

/* shared with the connect thread; freed by whichever side lets go last */
struct attempt {
    pthread_mutex_t mu;
    int             refs;
    int             done;
    char            bot[128];
    grokvnc        *vnc;
    char            status[160];
};

struct source {
    struct vncinset_source base;
    char                   bot[128];
    struct attempt        *pending;
    grokvnc               *vnc;
    int                    reconnects;
    char                   status[160];
};

static void attempt_release(struct attempt *a)
{
    pthread_mutex_lock(&a->mu);
    int left = --a->refs;
    pthread_mutex_unlock(&a->mu);
    if (left)
        return;
    if (a->vnc)
        grokvnc_close(a->vnc);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

static grokvnc *connect_box(struct attempt *a, char *status, size_t cap)
{
    grokbot *g = grokbot_open();
    if (!g) {
        snprintf(status, cap, "grokbot: %s", grokbot_error(NULL));
        return NULL;
    }
    grokvnc *v = NULL;
    for (int tries = 0; tries < 2 && !v; tries++) {
        grokbot_box box;
        if (!grokbot_box_status(g, a->bot, &box)) {
            snprintf(status, cap, "box status failed: %s", grokbot_error(g));
            break;
        }
        if (!box.state || strcmp(box.state, "running")) {
            snprintf(status, cap, "desktop %s", box.state ? box.state : "unknown");
            grokbot_box_free(&box);
            break;
        }
        grokbot_vnc_endpoint ep;
        int                  ok = grokbot_vnc_endpoint_for(g, &box, &ep);
        grokbot_box_free(&box);
        if (!ok) {
            snprintf(status, cap, "no desktop: %s", grokbot_error(g));
            break;
        }
        const char *hdr[] = {ep.header, NULL};
        grokvnc_err err;
        v = grokvnc_open(ep.url, hdr, &err);
        memset(&ep, 0, sizeof ep);
        if (v)
            break;
        snprintf(status, cap, "%s", err.msg);
        if ((err.http_status != 401 && err.http_status != 404) || !grokbot_reload(g))
            break;
    }
    grokbot_close(g);
    return v;
}

static void *connect_thread(void *arg)
{
    struct attempt *a = arg;
    char            status[sizeof a->status] = "";
    grokvnc        *v = connect_box(a, status, sizeof status);
    pthread_mutex_lock(&a->mu);
    a->vnc = v;
    snprintf(a->status, sizeof a->status, "%s", status);
    a->done = 1;
    pthread_mutex_unlock(&a->mu);
    attempt_release(a);
    return NULL;
}

static void start_attempt(struct source *s)
{
    struct attempt *a = calloc(1, sizeof *a);
    if (!a) {
        snprintf(s->status, sizeof s->status, "out of memory");
        return;
    }
    pthread_mutex_init(&a->mu, NULL);
    a->refs = 2;
    snprintf(a->bot, sizeof a->bot, "%s", s->bot);
    pthread_t      t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, connect_thread, a) != 0) {
        pthread_attr_destroy(&attr);
        pthread_mutex_destroy(&a->mu);
        free(a);
        snprintf(s->status, sizeof s->status, "could not start connect thread");
        return;
    }
    pthread_attr_destroy(&attr);
    s->pending = a;
    snprintf(s->status, sizeof s->status, "connecting");
}

static void collect(struct source *s)
{
    struct attempt *a = s->pending;
    pthread_mutex_lock(&a->mu);
    int done = a->done;
    if (done) {
        s->vnc = a->vnc;
        a->vnc = NULL;
        if (!s->vnc)
            snprintf(s->status, sizeof s->status, "%s", a->status);
    }
    pthread_mutex_unlock(&a->mu);
    if (!done)
        return;
    s->pending = NULL;
    attempt_release(a);
    if (s->vnc)
        grokvnc_set_interval(s->vnc, UPDATE_INTERVAL_MS);
}

static void pump(struct source *s)
{
    struct pollfd p = {grokvnc_fd(s->vnc), POLLIN, 0};
    if (grokvnc_want_write(s->vnc))
        p.events |= POLLOUT;
    poll(&p, 1, 0);
    if (grokvnc_pump(s->vnc))
        return;
    snprintf(s->status, sizeof s->status, "%s", grokvnc_error(s->vnc));
    grokvnc_close(s->vnc);
    s->vnc = NULL;
    if (s->reconnects++ == 0)
        start_attempt(s);
}

static int source_frame(struct vncinset_source *src, struct vncinset_frame *out)
{
    struct source *s = (struct source *)src;
    if (s->pending)
        collect(s);
    if (!s->vnc)
        return 0;
    pump(s);
    if (!s->vnc)
        return 0;
    grokvnc_stats st;
    grokvnc_stats_get(s->vnc, &st);
    int            w, h;
    uint64_t       gen;
    const uint8_t *rgb = grokvnc_frame(s->vnc, &w, &h, &gen);
    if (!st.updates || !rgb)
        return 0;
    s->status[0] = '\0';
    out->rgb = rgb;
    out->w = w;
    out->h = h;
    out->gen = gen;
    return 1;
}

static const char *source_status(struct vncinset_source *src)
{
    return ((struct source *)src)->status;
}

static void source_close(struct vncinset_source *src)
{
    struct source *s = (struct source *)src;
    if (s->pending)
        attempt_release(s->pending);
    if (s->vnc)
        grokvnc_close(s->vnc);
    free(s);
}

struct vncinset_source *vncsource_open(const char *bot)
{
    struct source *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->base.frame = source_frame;
    s->base.close = source_close;
    s->base.status = source_status;
    snprintf(s->bot, sizeof s->bot, "%s", bot ? bot : "");
    if (!*s->bot)
        snprintf(s->status, sizeof s->status, "no bot");
    else
        start_attempt(s);
    return &s->base;
}
