#include "sessionswitch.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "activelog.h"
#include "ask.h"
#include "cmd.h"
#include "dirpick.h"
#include "handoff.h"
#include "hud.h"
#include "keyhelp.h"
#include "livelist.h"
#include "models.h"
#include "newsession.h"
#include "parent.h"
#include "pick.h"
#include "scrollback.h"
#include "sessionload.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "title.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "workspace.h"

#define KEY_CLOSE  'x'
#define KEY_NEW    'n'
#define KEY_GO     'g'
#define KEY_RENAME 'r'
#define KEY_ASK    'p'
#define KEY_HERE   'c'
#define KEY_ALL    '*'
#define KEY_PULL   'a'
#define KEY_TAKE   'y'
#define KEY_STACK  ','

static int show_all = 1;
static int stacked;

static const struct keyhelp_row SESSION_KEYS[] = {
    {"GO", "enter/\xe2\x86\x92", "switch to it"},
    {"GO", "g", "switch to it"},
    {"GO", "y", "pull into this window"},
    {"GO", "a", "pull every other window"},
    {"GO", "tab/esc", "close the list"},
    {"CHANGE", "r", "rename"},
    {"CHANGE", "x", "close the session"},
    {"CHANGE", "n", "new session"},
    {"CHANGE", "c", "new, same directory"},
    {"CHANGE", "p", "new, like this, with a prompt"},
    {"LIST", "up/down", "move"},
    {"LIST", "*", "this window only / every window"},
    {"LIST", ",", "details on their own line"},
};

#define KEY_CTRL(c) ((c) & 0x1f)

#define MAX_ROWS 128

enum row_kind {
    ROW_TAB,
    ROW_LIVE,
    ROW_NEW,
    ROW_HEAD,
};

struct row {
    enum row_kind kind;
    int  at;
    int  spin;
    char mark[4];
    unsigned char role;
    char id[128];
    char parent[128];
    char cwd[512];
    char label[256];
    char detail[512];
    char when[48];
    long ts;
};

static void row_status(struct row *r, const char *status)
{
    r->spin = status && !strcmp(status, "working");
    int errored = status && !strcmp(status, "errored");
    snprintf(r->mark, sizeof r->mark, "%s", errored ? "e" : "");
    r->role = errored ? UI_ERROR : UI_OK;
}

static long last_active(const char *name)
{
    char path[4400];
    return path_config_file(path, sizeof path, "active") ? activelog_last(path, name) : 0;
}

static void tab_rows(struct row *rows, int *n)
{
    for (int i = 0; i < workspace_count() && *n < MAX_ROWS; i++) {
        struct session *s = workspace_at(i);
        const char *title = session_title(s);
        const char *status = workspace_status(s);
        struct row *r = &rows[(*n)++];
        r->kind = ROW_TAB;
        r->at = i;

        path_home_relative(session_cwd(s), r->cwd, sizeof r->cwd);
        r->ts = last_active(session_name(s));
        snprintf(r->label, sizeof r->label, "%s %s",
                 i == workspace_index() ? "\xe2\x96\xb8" : "\xe2\xa7\x89",
                 title && *title ? title : "untitled");
        snprintf(r->id, sizeof r->id, "%s", session_id(s) ? session_id(s) : "");
        if (r->id[0])
            parent_of(r->id, r->parent, sizeof r->parent);
        snprintf(r->detail, sizeof r->detail, "%s %s @%s", session_backend(s),
                 models_short_name(session_backend(s), session_model_label(s)),
                 session_name(s));
        row_status(r, status);
    }
}

static void fill_live(struct row *r, const struct live_session *v)
{
    char when[32] = "";
    if (r->ts)
        text_ago(r->ts, 1, when, sizeof when);

    snprintf(r->label, sizeof r->label, "%s",
             v->title[0] ? v->title : "untitled");
    snprintf(r->detail, sizeof r->detail, "%s %s%s%s", v->backend,
             models_short_name(v->backend, v->label[0] ? v->label : v->model),
             v->name[0] ? " @" : "", v->name);
    snprintf(r->when, sizeof r->when, "%s", when);
    row_status(r, v->status);
}

static void live_rows(struct row *rows, int *n, const struct live_session *live, int count)
{
    if (!show_all)
        return;
    for (int i = 0; i < count && *n < MAX_ROWS; i++) {
        const struct live_session *v = &live[i];
        if (v->mine || !v->id[0])
            continue;

        struct row *r = &rows[(*n)++];
        r->kind = ROW_LIVE;
        r->at = i;
        snprintf(r->id, sizeof r->id, "%s", v->id);
        snprintf(r->parent, sizeof r->parent, "%s", v->parent);
        path_home_relative(v->cwd, r->cwd, sizeof r->cwd);
        r->ts = last_active(v->name);
        fill_live(r, v);
    }
}

static int in_group(const struct row *in, int n, const char *group, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(in[i].cwd, group) && in[i].id[0] && !strcmp(in[i].id, id))
            return 1;
    return 0;
}

static void nest_label(struct row *r, int depth)
{
    const char *text = r->label;
    if (r->kind == ROW_TAB) {
        const char *sp = strchr(r->label, ' ');
        if (sp)
            text = sp + 1;
    }

    char under[sizeof r->label];
    snprintf(under, sizeof under, "%*s\xe2\x94\x94 %s", depth * 2, "", text);
    snprintf(r->label, sizeof r->label, "%s", under);
}

static void note_parent(struct row *r)
{
    char name[200];
    if (!title_lookup(r->parent, name, sizeof name))
        snprintf(name, sizeof name, "%.8s", r->parent);

    char said[sizeof r->detail];
    snprintf(said, sizeof said, "%s \xc2\xb7 from %s", r->detail, name);
    snprintf(r->detail, sizeof r->detail, "%s", said);
}

#define NEST_MAX 4

static void emit_tree(const struct row *in, int n, char *used, struct row *out,
                      unsigned char *heading, int *m, int max, const char *group,
                      const char *under, int depth)
{
    if (depth > NEST_MAX)
        return;

    for (int i = 0; i < n; i++) {
        if (used[i] || strcmp(in[i].cwd, group) != 0)
            continue;

        int nested = in[i].parent[0] && in_group(in, n, group, in[i].parent);
        if (!under) {
            if (nested)
                continue;
        } else if (!nested || strcmp(in[i].parent, under)) {
            continue;
        }

        used[i] = 1;
        if (*m < max) {
            out[*m] = in[i];
            if (depth)
                nest_label(&out[*m], depth);
            else if (out[*m].parent[0])
                note_parent(&out[*m]);
            heading[*m] = 0;
            (*m)++;
        }
        if (in[i].id[0])
            emit_tree(in, n, used, out, heading, m, max, group, in[i].id,
                      depth + 1);
    }
}

static long group_ts(const struct row *in, int n, const char *group)
{
    long ts = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(in[i].cwd, group) && in[i].ts > ts)
            ts = in[i].ts;
    return ts;
}

static int group_rows(const struct row *in, int n, struct row *out,
                      unsigned char *heading, int max)
{
    char used[MAX_ROWS] = {0};
    int m = 0;
    int recent = !strcmp(settings_get_str(SETTING_FOLDER_SORT, "name"), "recent");

    for (;;) {
        const char *group = NULL;
        long best = 0;
        for (int i = 0; i < n; i++) {
            if (used[i])
                continue;
            long ts = recent ? group_ts(in, n, in[i].cwd) : 0;
            if (!group || ts > best || (ts == best && strcmp(in[i].cwd, group) < 0)) {
                group = in[i].cwd;
                best = ts;
            }
        }
        if (!group)
            break;

        if (*group && m < max) {
            out[m] = (struct row){.kind = ROW_HEAD};
            snprintf(out[m].label, sizeof out[m].label, "%s", group);
            heading[m++] = PICK_HEADING;
        }

        emit_tree(in, n, used, out, heading, &m, max, group, NULL, 0);

        for (int i = 0; i < n; i++) {
            if (used[i] || strcmp(in[i].cwd, group) != 0)
                continue;
            used[i] = 1;
            if (m >= max)
                continue;
            out[m] = in[i];
            heading[m] = 0;
            m++;
        }
    }
    return m;
}

static void here_name(char *out, size_t size)
{
    struct session *s = workspace_current();
    snprintf(out, size, "%s", s ? session_name(s) : "");
}

static int tab_named(const char *name)
{
    for (int i = 0; i < workspace_count(); i++)
        if (!strcmp(session_name(workspace_at(i)), name))
            return i;
    return -1;
}

static int jump(const struct live_session *v)
{
    char why[256];
    if (!livelist_jump(v, why, sizeof why)) {
        ui_error("%s", why);
        ui_put("\n");
        ui_flush();
        return 0;
    }
    return 1;
}

struct others {
    struct live_session *live;
    int                  n;
};

static const struct live_session *other(const struct others *o, const char *name)
{
    for (int i = 0; i < o->n; i++)
        if (!o->live[i].mine && !strcmp(o->live[i].name, name))
            return &o->live[i];
    return NULL;
}

static int open_somewhere(const char *name, void *ud)
{
    return tab_named(name) >= 0 || other(ud, name);
}

int sessionswitch_show_open(const char *id)
{
    int at = id && *id ? workspace_find_id(id) : -1;
    if (at >= 0) {
        workspace_show(at);
        return 1;
    }
    struct live_session *live = NULL;
    int n = id && *id ? livelist_load(&live) : 0;
    int shown = 0;
    for (int i = 0; i < n && !shown; i++)
        if (!live[i].mine && !strcmp(live[i].id, id) && livelist_alive(live[i].pid))
            shown = jump(&live[i]);
    free(live);
    return shown;
}

void sessionswitch_step(int dir)
{
    char path[4400], here[128], id[128];
    here_name(here, sizeof here);
    struct others o = {NULL, 0};
    o.n = livelist_load(&o.live);

    if (path_config_file(path, sizeof path, "active") &&
        activelog_step(path, here, dir,
                       settings_get_int(SETTING_JUMP_LENGTH, JUMP_LENGTH_DEFAULT),
                       open_somewhere, &o, id, sizeof id)) {
        int at = tab_named(id);
        if (at >= 0)
            workspace_show(at);
        else
            jump(other(&o, id));
    } else {
        ui_note("%s", dir < 0 ? "no previous session" : "no next session");
        ui_put("\n");
        ui_flush();
    }
    free(o.live);
}

static void waiting(int waited_ms, void *ud)
{
    int *said = ud;
    if (waited_ms < 1000 || *said)
        return;
    *said = 1;
    ui_bar(ui_style(UI_DIM), "no reply yet \xc2\xb7 waiting\xe2\x80\xa6");
    ui_put("\n");
    ui_flush();
}

static void yank(const struct live_session *v)
{
    ui_bar(ui_style(UI_DIM), "asking %s for the session\xe2\x80\xa6",
           v->pane[0] ? v->pane : "the other window");
    ui_put("\n");
    ui_flush();

    char screen[4400];
    int said = 0;
    if (!handoff_ask(v->pid, v->id, screen, sizeof screen, waiting, &said)) {
        ui_error("could not take the session");
        ui_put("\n");
        ui_flush();
        return;
    }

    int at = workspace_spawn(v->backend, v->model, v->effort, v->cwd, v->id);
    if (at < 0) {
        unlink(screen);
        ui_error("could not open the session");
        ui_put("\n");
        ui_flush();
        return;
    }

    struct stat st;
    if (stat(screen, &st) == 0 && st.st_size > 0)
        scrollback_restore(screen);
    else
        sessionload_into(workspace_current());
    unlink(screen);
    ui_bar(ui_style(UI_DIM), "session is here \xc2\xb7 %s",
           v->title[0] ? v->title : v->backend);
    ui_put("\n");
    ui_flush();
}

static void yank_all(const struct live_session *live, int nlive)
{
    int any = 0;
    for (int i = 0; i < nlive; i++) {
        if (live[i].mine || !live[i].id[0])
            continue;
        yank(&live[i]);
        any = 1;
    }
    if (!any) {
        ui_note("no sessions in other windows");
        ui_put("\n");
        ui_flush();
    }
}

static int pid_count(const struct live_session *live, int n, long pid)
{
    int found = 0;
    for (int i = 0; i < n; i++)
        if (live[i].pid == pid)
            found++;
    return found;
}

static void close_live(const struct live_session *v, const struct live_session *live,
                       int nlive)
{
    char screen[4400];
    int said = 0;
    if (handoff_kill(v->pid, v->id, screen, sizeof screen, waiting, &said)) {
        unlink(screen);
        return;
    }

    if (pid_count(live, nlive, v->pid) == 1 && v->pid > 0 &&
        kill((pid_t)v->pid, SIGTERM) == 0)
        return;

    ui_error("could not close that session");
    ui_put("\n");
    ui_flush();
}

static const char *row_cwd(const struct row *r, const struct live_session *live)
{
    if (r->kind == ROW_TAB) {
        const struct session *s = workspace_at(r->at);
        if (s)
            return session_cwd(s);
    } else if (r->kind == ROW_LIVE && r->at >= 0) {
        return live[r->at].cwd;
    }
    const struct session *here = workspace_current();
    return here ? session_cwd(here) : NULL;
}

static void ask_new(const struct row *r, const struct live_session *live)
{
    const char *backend = NULL, *model = NULL, *cwd = NULL;
    if (r->kind == ROW_TAB) {
        const struct session *s = workspace_at(r->at);
        if (s) {
            backend = session_backend(s);
            model = session_model_label(s);
            cwd = session_cwd(s);
        }
    } else if (r->kind == ROW_LIVE && r->at >= 0) {
        const struct live_session *v = &live[r->at];
        backend = v->backend;
        model = v->model;
        cwd = v->cwd;
    }
    if (!backend) {
        const struct session *here = workspace_current();
        if (!here)
            return;
        backend = session_backend(here);
        model = session_model_label(here);
        cwd = session_cwd(here);
    }

    char *line = ask_run("a new session, with this prompt", NULL);
    if (!line)
        return;
    if (!*line) {
        free(line);
        return;
    }

    struct session *was = workspace_current();
    int at = workspace_spawn(backend, model, NULL, cwd, NULL);
    if (at < 0) {
        const char *why = session_start_error();
        if (why)
            ui_error("could not start %s: %s", backend, why);
        else
            ui_error("could not start the %s CLI", backend);
        ui_put("\n");
        ui_flush();
        free(line);
        return;
    }

    workspace_show(workspace_index_of(was));
    workspace_send(at, line, NULL);
    free(line);
}

static void rename_row(const struct row *r, struct live_session *live)
{
    struct session *tab = r->kind == ROW_TAB ? workspace_at(r->at) : NULL;
    struct live_session *v = r->kind == ROW_LIVE ? &live[r->at] : NULL;
    if (!tab && !v)
        return;

    const char *was = tab ? session_title(tab) : (v->title[0] ? v->title : NULL);
    char *name = ask_run("rename this session", was);
    if (!name)
        return;

    enum session_rename why = tab ? session_rename(tab, name)
                                  : title_set(v->id, name) ? SESSION_RENAME_OK
                                                           : SESSION_RENAME_BAD_NAME;
    int ok = why == SESSION_RENAME_OK;
    if (ok && v)
        snprintf(v->title, sizeof v->title, "%s", name);

    if (ok && tab && tab != workspace_current())
        status_set_note(session_title(workspace_current()));
    if (!ok) {
        ui_error("%s", session_rename_error(why));
        ui_put("\n");
        ui_flush();
    }
    free(name);
}

struct listing {
    struct row          *rows;
    int                  n;
    unsigned char       *spin;
    const char         **marks;
    unsigned char       *roles;
    const char         **tails;
    struct live_session **live;
    int                 *nlive;
    double               read_at;
    unsigned long        sig;
};

static void sync_columns(struct listing *l)
{
    for (int i = 0; i < l->n; i++) {
        l->spin[i] = (unsigned char)l->rows[i].spin;
        l->marks[i] = l->rows[i].mark;
        l->roles[i] = l->rows[i].role;
        l->tails[i] = l->rows[i].when;
    }
}

static unsigned long listing_sig(const struct listing *l)
{
    unsigned long h = 5381;
    for (int i = 0; i < l->n; i++) {
        const struct row *r = &l->rows[i];
        const char *parts[] = {r->label, r->detail, r->mark, r->when};
        for (size_t p = 0; p < sizeof parts / sizeof *parts; p++)
            for (const char *c = parts[p]; c && *c; c++)
                h = h * 33 + (unsigned char)*c;
        h = h * 33 + (unsigned long)(r->spin + 1);
    }
    return h;
}

static int relist(void *ud)
{
    struct listing *l = ud;
    double now = now_seconds();
    if (l->read_at > 0 && now - l->read_at < 0.45)
        return 0;
    l->read_at = now;

    workspace_pump_quiet();

    struct live_session *fresh = NULL;
    int nfresh = livelist_load(&fresh);
    free(*l->live);
    *l->live = fresh;
    *l->nlive = nfresh;

    for (int i = 0; i < l->n; i++) {
        struct row *r = &l->rows[i];
        if (r->kind == ROW_TAB) {
            struct session *s = r->at < workspace_count() ? workspace_at(r->at) : NULL;
            if (s)
                row_status(r, workspace_status(s));
            continue;
        }
        if (r->kind != ROW_LIVE)
            continue;

        r->at = -1;
        for (int j = 0; j < nfresh; j++)
            if (!strcmp(fresh[j].id, r->id)) {
                r->at = j;
                break;
            }
        if (r->at >= 0) {
            fill_live(r, &fresh[r->at]);
        } else {
            r->spin = 0;
            r->mark[0] = '\0';
            r->when[0] = '\0';
        }
    }
    sync_columns(l);

    unsigned long sig = listing_sig(l);
    if (sig == l->sig)
        return 0;
    l->sig = sig;
    return 1;
}

static int resume_row = -1;

static int switch_once(void)
{
    struct live_session *live = NULL;
    int nlive = livelist_load(&live);

    struct row *found = calloc(MAX_ROWS, sizeof *found);
    struct row *rows = calloc(MAX_ROWS, sizeof *rows);
    unsigned char *heading = calloc(MAX_ROWS, 1);
    unsigned char *spin = calloc(MAX_ROWS, 1);
    const char **marks = calloc(MAX_ROWS, sizeof *marks);
    unsigned char *roles = calloc(MAX_ROWS, 1);
    const char **tails = calloc(MAX_ROWS, sizeof *tails);
    if (!found || !rows || !heading || !spin || !marks || !roles || !tails) {
        free(found);
        free(rows);
        free(heading);
        free(spin);
        free(marks);
        free(roles);
        free(tails);
        free(live);
        return 0;
    }

    int nfound = 0;
    tab_rows(found, &nfound);
    live_rows(found, &nfound, live, nlive);

    int n = group_rows(found, nfound, rows, heading, MAX_ROWS);
    free(found);

    int initial = 0;
    for (int i = 0; i < n; i++)
        if (rows[i].kind == ROW_TAB && rows[i].at == workspace_index())
            initial = i;

    if (resume_row >= 0) {
        initial = resume_row < n ? resume_row : n - 1;
        while (initial > 0 && heading[initial] == PICK_HEADING)
            initial--;
        resume_row = -1;
    }

    if (n < MAX_ROWS) {
        struct row *r = &rows[n];
        r->kind = ROW_NEW;
        snprintf(r->label, sizeof r->label, "+ new session");
        heading[n++] = PICK_APART;
    }

    struct pick_item *items = calloc((size_t)n, sizeof *items);
    if (!items) {
        free(rows);
        free(heading);
        free(spin);
        free(marks);
        free(roles);
        free(tails);
        free(live);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        items[i].label = rows[i].label;
        items[i].detail = rows[i].detail;
    }

    char shortcuts[24] = {KEY_CLOSE, KEY_NEW, KEY_ASK, KEY_GO, KEY_RENAME,
                          KEY_ALL, KEY_HERE, KEY_PULL, KEY_TAKE, KEY_STACK, '\t',
                          KEY_CTRL(KEY_CLOSE), KEY_CTRL(KEY_NEW), KEY_CTRL(KEY_ASK),
                          KEY_CTRL(KEY_GO), KEY_CTRL(KEY_RENAME),
                          '\n', PICK_KEY_RIGHT, 0};
    int pressed = 0;

    char title[256];
    snprintf(title, sizeof title, "sessions");
    struct listing listing = {rows, n, spin, marks, roles, tails, &live,
                              &nlive, 0, 0};
    sync_columns(&listing);
    listing.sig = listing_sig(&listing);
    struct pick_live shown = {.heading = heading, .spin = spin, .mark = marks, .mark_role = roles,
                              .tail = tails,
                              .align = 1, .stack = stacked, .tick = relist, .ud = &listing,
                              .keys = SESSION_KEYS,
                              .nkeys = (int)(sizeof SESSION_KEYS / sizeof *SESSION_KEYS)};
    int picked = pick_run_live(title, items, n, initial, &shown, PICK_SEARCH_SLASH,
                               shortcuts, &pressed);

    struct row chosen = {0};
    if (picked >= 0)
        chosen = rows[picked];

    if (pressed > 0 && pressed < 0x20 && pressed != '\n' &&
        pressed != PICK_KEY_RIGHT)
        pressed |= 0x60;

    if (pressed == PICK_KEY_RIGHT)
        pressed = chosen.kind == ROW_LIVE ? KEY_GO
                : chosen.kind == ROW_NEW  ? '\n'
                                          : 0;

    free(items);
    free(rows);
    free(heading);
    free(spin);
    free(marks);
    free(roles);
    free(tails);

    if (pressed == KEY_STACK) {
        stacked = !stacked;
        resume_row = picked;
        free(live);
        return 1;
    }

    if (pressed == KEY_ALL) {
        show_all = !show_all;
        resume_row = picked;
        free(live);
        return 1;
    }

    if (pressed == KEY_PULL) {
        yank_all(live, nlive);
        free(live);
        return 0;
    }

    if (picked < 0) {
        free(live);
        return 0;
    }

    if (chosen.kind == ROW_LIVE && chosen.at < 0) {
        ui_note("that session is gone");
        ui_put("\n");
        ui_flush();
        free(live);
        return 0;
    }

    if (pressed == KEY_RENAME) {
        rename_row(&chosen, live);
        free(live);
        return 1;
    }

    if (chosen.kind == ROW_NEW && (pressed == KEY_GO || pressed == '\n')) {
        int again = (newsession_run());
        resume_row = picked;
        free(live);
        return again;
    }

    if (pressed == KEY_NEW) {
        int again = (newsession_run());
        resume_row = picked;
        free(live);
        return again;
    }

    if (pressed == KEY_HERE) {
        const char *cwd = row_cwd(&chosen, live);
        int again = cwd && *cwd ? (newsession_spawn(cmd_default_backend(), NULL, NULL, cwd), 0) : 1;
        resume_row = picked;
        free(live);
        return again;
    }

    if (pressed == KEY_ASK) {
        ask_new(&chosen, live);
        free(live);
        return 1;
    }

    if (pressed == KEY_TAKE) {
        if (chosen.kind == ROW_LIVE)
            yank(&live[chosen.at]);
        free(live);
        return 0;
    }

    if (pressed == KEY_GO || pressed == '\n') {
        if (chosen.kind == ROW_LIVE) {
            jump(&live[chosen.at]);
        } else if (chosen.kind == ROW_TAB) {
            workspace_show(chosen.at);
        }
        free(live);
        return 0;
    }

    if (pressed == KEY_CLOSE) {
        if (chosen.kind == ROW_TAB) {
            int last = workspace_count() <= 1;
            if (chosen.at == workspace_index())
                sessionswitch_step(-1);
            workspace_close(chosen.at);
            if (last) {
                free(live);
                return 0;
            }
        } else if (chosen.kind == ROW_LIVE) {
            close_live(&live[chosen.at], live, nlive);
        }
        resume_row = picked;
        free(live);
        return 1;
    }

    switch (chosen.kind) {
    case ROW_TAB:
        workspace_show(chosen.at);
        break;
    case ROW_LIVE:
        jump(&live[chosen.at]);
        break;
    case ROW_NEW:
        {
            int again = (newsession_run());
            resume_row = picked;
            free(live);
            return again;
        }
    case ROW_HEAD:
        break;
    }

    free(live);
    return 0;
}

void sessionswitch_run(void)
{
    while (switch_once())
        ;
}

static int gave_last;

int sessionswitch_gave_last(void) { return gave_last; }

void sessionswitch_serve_request(void)
{
    char id[128];
    int closing = 0;
    if (!handoff_take_request(id, sizeof id, &closing))
        return;

    int at = workspace_find_id(id);
    if (at < 0) {
        handoff_refuse(id);
        return;
    }

    if (!closing)
        workspace_wait_turn(at);

    char screen[4400];
    if (handoff_screen_path(id, screen, sizeof screen))
        workspace_dump(at, screen);

    int left = workspace_close(at);
    handoff_publish(id);

    if (!left) {
        gave_last = 1;
        return;
    }
    ui_bar(ui_style(UI_DIM), "handed a session to another window");
    ui_put("\n");
    ui_flush();
}
