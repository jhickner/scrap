#include "tgbridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "sessionlist.h"
#include "text.h"
#include "workspace.h"

void tgbridge_init(struct tgbridge *b, struct session *s,
                   tgbridge_bind_fn bind, void *ud)
{
    *b = (struct tgbridge){
        .session = s,
        .bind = bind,
        .bind_ud = ud,
    };
}

void tgbridge_set_output(struct tgbridge *b, const struct tgbridge_output *output)
{
    b->output = output ? *output : (struct tgbridge_output){0};
}

struct session *tgbridge_session(const struct tgbridge *b)
{
    return b->session;
}

void tgbridge_forget(struct tgbridge *b, struct session *s)
{
    if (b->session == s)
        b->session = NULL;
}

void tgbridge_focus(struct tgbridge *b, struct session *s)
{
    if (b->session == s)
        return;
    if (b->session && b->bind)
        b->bind(b->session, 0, b->bind_ud);
    b->session = s;
    if (b->session && b->bind)
        b->bind(b->session, 1, b->bind_ud);
}

void tgbridge_refocus(struct tgbridge *b)
{
    tgbridge_focus(b, workspace_current());
}

int tgbridge_switch(struct tgbridge *b, int index)
{
    if (index < 0 || index >= workspace_count())
        return 0;
    workspace_show(index);
    tgbridge_focus(b, workspace_at(index));
    return 1;
}

int tgbridge_open(const char *cwd, const char *id)
{
    struct session *from = workspace_current();
    if (!from)
        return -1;
    return workspace_spawn(session_backend(from), session_model(from),
                           session_effort(from),
                           cwd && *cwd ? cwd : session_cwd(from), id);
}

static void note(struct tgbridge *b, const char *text)
{
    if (b->output.note)
        b->output.note(b->output.ud, text);
}

__attribute__((format(printf, 2, 3)))
static void notef(struct tgbridge *b, const char *fmt, ...)
{
    char text[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    note(b, text);
}

void tgbridge_send_here(struct tgbridge *b)
{
    struct session *s = b->session;
    if (!s)
        return;
    const char *title = session_title(s);
    notef(b, "%d/%d  %s  in %s", workspace_index() + 1, workspace_count(),
          title && *title ? title : "untitled",
          tgbridge_dir_name(session_cwd(s)));
}

void tgbridge_send_tabs(struct tgbridge *b, int menu_max)
{
    int n = workspace_count();
    if (n <= 0) {
        note(b, "no conversations here");
        return;
    }
    if (!b->output.menu_begin || !b->output.menu_add || !b->output.menu_send)
        return;
    b->output.menu_begin(b->output.ud, "tab");
    for (int i = 0; i < n && i < menu_max - 3; i++) {
        char label[80], payload[200];
        tgbridge_tab_label(i, label, sizeof label);
        tgbridge_tab_payload(i, payload, sizeof payload);
        b->output.menu_add(b->output.ud, label, payload);
    }
    if (n < WORKSPACE_MAX)
        b->output.menu_add(b->output.ud, "+ new conversation", "new");
    b->output.menu_add(b->output.ud, "resume past session", "resume");
    b->output.menu_add(b->output.ud, "cancel", "cancel");
    b->output.menu_send(b->output.ud, "conversations", 1);
}

void tgbridge_send_resume(struct tgbridge *b, int menu_max)
{
    if (!b->session) {
        note(b, "no conversation to resume alongside");
        return;
    }
    const char *backend = session_backend(b->session);
    if (!sessionlist_available(backend)) {
        notef(b, "%s keeps no list of past conversations", backend);
        return;
    }

    struct past_session *past = NULL;
    int n = sessionlist_load(backend, session_cwd(b->session),
                             session_id(b->session), &past);
    if (n <= 0) {
        free(past);
        note(b, "nothing to resume here");
        return;
    }
    if (!b->output.menu_begin || !b->output.menu_add || !b->output.menu_send) {
        free(past);
        return;
    }

    int room = menu_max - 1;
    b->output.menu_begin(b->output.ud, "resume");
    for (int i = 0; i < n && i < room; i++) {
        char label[80];
        snprintf(label, sizeof label, "%s  %s", past[i].when, past[i].label);
        b->output.menu_add(b->output.ud, label, past[i].id);
    }
    free(past);
    b->output.menu_add(b->output.ud, "< back", "back");
    char title[80];
    snprintf(title, sizeof title, "past conversations here%s",
             n > room ? " (the newest few)" : "");
    b->output.menu_send(b->output.ud, title, 1);
}

void tgbridge_open_tab(struct tgbridge *b, const char *cwd, const char *id)
{
    if (workspace_count() >= WORKSPACE_MAX) {
        notef(b, "that is all %d conversations; close one first", WORKSPACE_MAX);
        return;
    }
    char *expanded = cwd && *cwd ? path_expand_home(cwd) : NULL;
    char resolved[4096];
    const char *where = NULL;
    if (cwd && *cwd) {
        if (!realpath(expanded ? expanded : cwd, resolved)) {
            notef(b, "no such directory: %s", cwd);
            free(expanded);
            return;
        }
        where = resolved;
    }
    free(expanded);

    int index = tgbridge_open(where, id);
    if (index < 0) {
        note(b, "could not start another conversation");
        return;
    }
    tgbridge_focus(b, workspace_at(index));
    tgbridge_send_here(b);
}

void tgbridge_close_tab(struct tgbridge *b, int index)
{
    if (index < 0 || index >= workspace_count()) {
        note(b, "no such conversation");
        return;
    }
    if (workspace_count() == 1) {
        note(b, "that is the only conversation; /quit to stop instead");
        return;
    }
    workspace_close(index);
    tgbridge_send_here(b);
}

void tgbridge_switch_tab(struct tgbridge *b, int index)
{
    if (!tgbridge_switch(b, index)) {
        note(b, "no such conversation");
        return;
    }
    tgbridge_send_here(b);
}

const char *tgbridge_dir_name(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

void tgbridge_tab_label(int index, char *out, size_t size)
{
    struct session *s = workspace_at(index);
    const char *title = session_title(s);
    if (!title || !*title)
        title = tgbridge_dir_name(session_cwd(s));

    const char *mark = index == workspace_index() ? "> "
                     : session_unseen(s)          ? "* "
                                                  : "  ";
    const char *what = session_turn_running(s) ? "  (working)" : "";
    snprintf(out, size, "%s%d  %s%s", mark, index + 1, title, what);
}

void tgbridge_tab_payload(int index, char *out, size_t size)
{
    const char *id = session_id(workspace_at(index));
    if (id && *id)
        snprintf(out, size, "%s", id);
    else
        snprintf(out, size, "#%d", index);
}

int tgbridge_tab_from_payload(const char *payload)
{
    if (!payload || !*payload)
        return -1;
    if (*payload == '#') {
        int index = atoi(payload + 1);
        return index >= 0 && index < workspace_count() ? index : -1;
    }
    return workspace_find_id(payload);
}

int tgbridge_workspace_fds(void *ud, int *out, int max)
{
    (void)ud;
    return workspace_fds(out, max);
}

void tgbridge_workspace_ready(void *ud)
{
    (void)ud;
    workspace_drain();
}
