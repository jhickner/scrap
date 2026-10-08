#include "bridges.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "filelock.h"
#include "hud.h"
#include "settings.h"
#include "text.h"
#include "tg.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

struct bridge {
    const char *name;
    int  (*running)(void);
    int  (*start)(struct session *s);
    void (*stop)(void);
    const char *(*error)(void);
    int  lock;
    int  failed;
};

static int tg_running(void)    { return tg_label() != NULL; }

static int api_begin(struct session *s)
{
    (void)s;
    return api_start();
}

static struct bridge bridges[] = {
    {"telegram", tg_running, tg_start, tg_stop, tg_start_error, -1, 0},
    {"api", api_active, api_begin, api_stop, api_start_error, -1, 0},
};

#define NBRIDGES (int)(sizeof bridges / sizeof bridges[0])

static struct bridge *find(const char *name)
{
    for (int i = 0; i < NBRIDGES; i++)
        if (!strcmp(bridges[i].name, name))
            return &bridges[i];
    return NULL;
}

static void release(struct bridge *b)
{
    filelock_release(b->lock);
    b->lock = -1;
}

/* A bridge that is on in settings but failed to come back (at startup or after
   a restart) says so once; it stays off until /<name> on retries it. */
static void not_restored(const struct bridge *b)
{
    const char *why = b->error ? b->error() : NULL;
    char line[900];
    snprintf(line, sizeof line, "%s%s%s not restored (/%s on to retry)",
             why ? why : "", why ? "; " : "", b->name, b->name);
    if (!viewport_active()) {
        fprintf(stderr, "%s\n", line);
        return;
    }
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_error("%s", line);
    viewport_item_end();
    ui_flush();
}

static int sync_one(struct bridge *b, int quiet)
{
    int want = settings_get_int(b->name, 0);
    if (!want)
        b->failed = 0;
    if (!want && b->running()) {
        b->stop();
        release(b);
        return 1;
    }
    if (!want || b->running() || b->failed)
        return 0;
    struct session *s = workspace_current();
    char            path[4200];
    if (!s || !path_config_file(path, sizeof path, b->name) ||
        (b->lock = filelock_acquire(path, LOCK_EX | LOCK_NB)) < 0)
        return 0;
    if (b->start(s))
        return 1;
    b->failed = 1;
    release(b);
    if (!quiet)
        not_restored(b);
    return 0;
}

void bridges_tick(void)
{
    static time_t last;
    time_t        now = time(NULL);
    if (now == last)
        return;
    last = now;
    settings_reload();
    int changed = 0;
    for (int i = 0; i < NBRIDGES; i++)
        changed |= sync_one(&bridges[i], 0);
    if (changed) {
        hud_refresh(workspace_current());
        workspace_republish();
    }
}

int bridges_wanted(const char *name)
{
    settings_reload();
    return settings_get_int(name, 0);
}

int bridges_set(const char *name, int on, char *msg, size_t size)
{
    struct bridge *b = find(name);
    if (!b) {
        snprintf(msg, size, "no bridge named %s", name);
        return 0;
    }
    settings_reload();
    settings_set_int(name, on);
    b->failed = 0;
    if (sync_one(b, 1)) {
        hud_refresh(workspace_current());
        workspace_republish();
    }
    if (!on)
        snprintf(msg, size, "%s off", name);
    else if (b->running())
        snprintf(msg, size, "%s on in this window", name);
    else if (!b->failed)
        snprintf(msg, size, "%s on in another window", name);
    else {
        const char *why = b->error ? b->error() : NULL;
        if (why)
            snprintf(msg, size, "%s", why);
        else
            snprintf(msg, size, "could not enable %s", name);
        settings_set_int(name, 0);
        return 0;
    }
    return 1;
}
