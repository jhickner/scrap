#include "apistubs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apicore.h"
#include "cmd.h"
#include "dispatch.h"

static struct session store[WORKSPACE_MAX + 2];
struct session       *tabs[WORKSPACE_MAX];
int                   ntabs;
int                   in_view;
int                   spawn_fails;
int                   hide_ids;
static int            next_id = 1;

const char *const *backend_names(void)
{
    static const char *const names[] = {"claude", "codex", NULL};
    return names;
}
const char *cmd_default_backend(void) { return "claude"; }
int workspace_count(void) { return ntabs; }
int workspace_index(void) { return in_view; }
struct session *workspace_at(int i) { return i >= 0 && i < ntabs ? tabs[i] : NULL; }
int workspace_index_of(const struct session *s)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i] == s)
            return i;
    return -1;
}
int workspace_find_id(const char *id)
{
    for (int i = 0; i < ntabs; i++)
        if (!strcmp(tabs[i]->id, id))
            return i;
    return -1;
}
int workspace_queued(int i) { return workspace_at(i) ? tabs[i]->nqueue : 0; }
int workspace_dequeue(int i, const char *line)
{
    struct session *s = workspace_at(i);
    for (int k = 0; s && k < s->nqueue; k++)
        if (!strcmp(s->queue[k], line)) {
            memmove(s->queue[k], s->queue[k + 1], (size_t)(s->nqueue - k - 1) * sizeof s->queue[0]);
            s->nqueue--;
            return 1;
        }
    return 0;
}
int workspace_close(int i)
{
    if (!workspace_at(i))
        return 0;
    memmove(&tabs[i], &tabs[i + 1], (size_t)(ntabs - i - 1) * sizeof tabs[0]);
    ntabs--;
    return 1;
}
const char *session_id(const struct session *s) { return s->id[0] ? s->id : NULL; }
const char *session_model(const struct session *s) { return s->model[0] ? s->model : "default"; }
int session_turn_running(const struct session *s) { return s && s->running; }
void session_interrupt(struct session *s) { s->interrupted = 1; }
const char *session_last_reply(const struct session *s) { return s->reply; }
const backend_result *session_last_result(const struct session *s) { return &s->result; }
const char *session_last_error(const struct session *s) { (void)s; return "boom"; }

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *const *env, const char *prompt)
{
    (void)backend; (void)model; (void)effort; (void)cwd; (void)title; (void)prompt;
    if (spawn_fails || ntabs >= WORKSPACE_MAX)
        return -1;
    struct session *s = &store[next_id % (WORKSPACE_MAX + 2)];
    memset(s, 0, sizeof *s);
    if (!hide_ids)
        snprintf(s->id, sizeof s->id, "sess-%d", next_id);
    next_id++;
    for (int i = 0; env && env[i] && i < 4; i++)
        snprintf(s->env[i], sizeof s->env[i], "%s", env[i]);
    tabs[ntabs] = s;
    return ntabs++;
}

int dispatch_send(int at, const char *line)
{
    struct session *s = workspace_at(at);
    if (!s)
        return 0;
    if (s->running) {
        snprintf(s->queue[s->nqueue++], sizeof s->queue[0], "%s", line);
        return 1;
    }
    s->running = 1;
    apicore_turn_begin(s);
    return 1;
}

void end_turn(struct session *s, const char *reply, int interrupted)
{
    s->running = 0;
    free(s->reply);
    s->reply = reply ? strdup(reply) : NULL;
    memset(&s->result, 0, sizeof s->result);
    s->result.output_tokens = 42;
    s->result.interrupted = interrupted;
    apicore_turn_done(s);
    if (s->nqueue) {
        memmove(s->queue[0], s->queue[1], (size_t)(s->nqueue - 1) * sizeof s->queue[0]);
        s->nqueue--;
        s->running = 1;
        apicore_turn_begin(s);
    }
}
