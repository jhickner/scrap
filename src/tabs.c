#include "tabs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chain.h"
#include "chrome.h"
#include "hud.h"
#include "scrollback.h"
#include "session.h"
#include "sessionload.h"
#include "workspace.h"

static void replay_tab(struct session *s, void *ud)
{
    const char *screen = ud;
    struct stat st;

    if (screen && *screen && stat(screen, &st) == 0 && st.st_size > 0 &&
        scrollback_restore(screen)) {
        hud_refresh(s);
        return;
    }
    if (session_remote(s)) {
        session_replay(s);
        return;
    }
    hud_print(s);
    sessionload_into(s);
}

struct pending_tab {
    char           *screen;
    struct session *s;
};

static struct pending_tab pending_tabs[WORKSPACE_MAX];
static int               npending_tabs;

int tabs_parse(char *line, struct tab_args *t)
{
    memset(t, 0, sizeof *t);
    line[strcspn(line, "\n")] = '\0';
    char *rest = line;
    t->screen = strsep(&rest, "\t");
    for (char *arg; (arg = strsep(&rest, "\t"));) {
        if (arg[0] != '-' || !strcmp(arg, "-s"))
            continue;
        char *value = strsep(&rest, "\t");
        if (!value)
            break;
        if (!strcmp(arg, "-b"))
            t->backend = value;
        else if (!strcmp(arg, "-C"))
            t->cwd = value;
        else if (!strcmp(arg, "-m"))
            t->model = value;
        else if (!strcmp(arg, "-e"))
            t->effort = value;
        else if (!strcmp(arg, "--session"))
            t->id = value;
        else if (!strcmp(arg, "--attach"))
            t->remote = value;
    }
    if (t->remote || !t->backend || !*t->backend || !t->id || !*t->id)
        return t->remote != NULL;
    char                chain[CHAIN_ID_MAX];
    struct chain_record r;
    if (chain_find(t->id, chain, sizeof chain) && chain_read(chain, &r)) {
        if (r.n && !strcmp(r.seg[r.n - 1].backend, t->backend)) {
            snprintf(t->latest, sizeof t->latest, "%s", r.seg[r.n - 1].id);
            t->id = t->latest;
        }
        chain_free(&r);
    }
    return 1;
}

void tabs_write(FILE *f, const struct session *s, const char *screen)
{
    char *args[SESSION_ARGV_MAX];
    int   n = session_argv(s, args, SESSION_ARGV_MAX, SESSION_ARGV_CWD | SESSION_ARGV_RESUME);
    fputs(screen, f);
    for (int a = 0; a < n; a++)
        fprintf(f, "\t%s", args[a]);
    fputc('\n', f);
}

void tabs_prepare(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[6144];
    while (npending_tabs < WORKSPACE_MAX - 1 && fgets(line, sizeof line, f)) {
        struct tab_args t;
        if (!tabs_parse(line, &t))
            continue;

        struct session *s = t.remote ? workspace_prepare("claude", NULL, NULL, t.cwd, NULL)
                                     : workspace_prepare(t.backend, t.model, t.effort, t.cwd, t.id);
        if (s && t.remote && !session_set_remote(s, t.remote)) {
            session_free(s);
            s = NULL;
        }
        if (!s)
            continue;
        tabs_queue(s, t.screen);
    }
    fclose(f);
}

static void tabs_drop(int at)
{
    session_free(pending_tabs[at].s);
    if (pending_tabs[at].screen)
        unlink(pending_tabs[at].screen);
    free(pending_tabs[at].screen);
    pending_tabs[at].s = NULL;
    pending_tabs[at].screen = NULL;
}

int tabs_start(struct session *front)
{
    struct session *batch[1 + WORKSPACE_MAX];
    int             n = 0;

    batch[n++] = front;
    for (int i = 0; i < npending_tabs; i++)
        batch[n++] = pending_tabs[i].s;

    session_start_batch(batch, n);
    return session_start_wait(front);
}

static void tabs_take(int at)
{
    struct session *s = pending_tabs[at].s;

    if (!session_start_wait(s)) {
        tabs_drop(at);
        return;
    }

    int index = workspace_insert(s);
    if (index < 0) {
        tabs_drop(at);
        return;
    }
    workspace_render(index, replay_tab, pending_tabs[at].screen
                                            ? pending_tabs[at].screen : (void *)"");
    if (pending_tabs[at].screen)
        unlink(pending_tabs[at].screen);
    free(pending_tabs[at].screen);
    pending_tabs[at].screen = NULL;
    pending_tabs[at].s = NULL;
}

void tabs_admit(int all)
{
    int taken = 0;
    int left = 0;

    if (!npending_tabs)
        return;

    session_start_drain();
    for (int i = 0; i < npending_tabs; i++) {
        if (!pending_tabs[i].s)
            continue;
        if (!all && !session_start_done(pending_tabs[i].s)) {
            left = 1;
            continue;
        }
        tabs_take(i);
        taken = 1;
    }
    if (!left)
        npending_tabs = 0;
    if (taken)
        chrome_paint();
}

int tabs_pending(void)
{
    return npending_tabs;
}

int tabs_queue(struct session *s, const char *screen)
{
    if (!s || npending_tabs >= WORKSPACE_MAX - 1)
        return 0;
    pending_tabs[npending_tabs].s = s;
    pending_tabs[npending_tabs].screen = screen && *screen ? strdup(screen) : NULL;
    npending_tabs++;
    return 1;
}

void tabs_drop_all(void)
{
    for (int i = 0; i < npending_tabs; i++)
        tabs_drop(i);
    npending_tabs = 0;
}
