#include "apicore.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "dispatch.h"
#include "workspace.h"

#define AGENTS_MAX  256
/* ponytail: runs are kept for the life of the instance; recycle the oldest
   finished ones if an instance ever outlives this */
#define RUNS_MAX    4096
#define ID_WAIT_MS  30000
#define TOOL_TEXT_MAX 4096

enum { RUN_QUEUED, RUN_RUNNING, RUN_FINISHED, RUN_ERROR, RUN_CANCELLED };
static const char *const RUN_STATUS[] = {"queued", "running", "finished", "error", "cancelled"};

struct run {
    int            id;
    int            agent;   /* index into agents */
    int            status;
    int            from_tab; /* a turn someone started by typing in the tab */
    int            cancel;
    char          *prompt;
    char          *result;
    char          *error;
    backend_result usage;
    int            has_usage;
    char           created[32], started[32], finished[32];
};

struct agent {
    int              id;
    char            *backend, *model, *effort, *cwd, *title;
    char             sid[128];
    struct session  *s;      /* used only until sid is known */
    int              exited;
    const char      *shown;  /* last status sent as an agent event */
    char             created[32], updated[32];
    struct apicall  *held;   /* a create waiting for the session id */
    struct timespec  held_since;
    int              held_run;
};

static struct agent     agents[AGENTS_MAX];
static int              nagents;
static struct run       runs[RUNS_MAX];
static int              nruns;
static apicore_reply_fn on_reply;
static apicore_emit_fn  on_emit;

static void now_iso(char out[32])
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static long since_ms(const struct timespec *then)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - then->tv_sec) * 1000 + (now.tv_nsec - then->tv_nsec) / 1000000;
}

static char *dup_or_null(const char *s)
{
    return s && *s ? strdup(s) : NULL;
}

static void finish(struct apicall *c, int status, cJSON *out)
{
    c->status = status;
    c->out = out;
    if (on_reply)
        on_reply(c);
}

static void fail(struct apicall *c, int status, const char *code, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cJSON *o = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    finish(c, status, o);
}

static void emit(const char *event, cJSON *data)
{
    if (on_emit)
        on_emit(event, data);
    else
        cJSON_Delete(data);
}

/* the agent's tab index, or -1 once the tab is gone */
static int tab_of(struct agent *a)
{
    if (a->exited)
        return -1;
    int at = a->sid[0] ? workspace_find_id(a->sid) : workspace_index_of(a->s);
    if (at >= 0 && !a->sid[0]) {
        const char *id = session_id(workspace_at(at));
        if (id && *id) {
            snprintf(a->sid, sizeof a->sid, "%s", id);
            a->s = NULL;
        }
    }
    return at;
}

static struct agent *agent_for(struct session *s)
{
    for (int i = 0; i < nagents; i++) {
        int at = tab_of(&agents[i]);
        if (at >= 0 && workspace_at(at) == s)
            return &agents[i];
    }
    return NULL;
}

static const char *agent_status(struct agent *a)
{
    int at = tab_of(a);
    if (at < 0)
        return "exited";
    if (session_turn_running(workspace_at(at)) || workspace_queued(at))
        return "busy";
    for (int i = 0; i < nruns; i++)
        if (runs[i].agent == a - agents && runs[i].status <= RUN_RUNNING)
            return "busy";
    return "idle";
}

static struct run *running_run(struct agent *a)
{
    for (int i = 0; i < nruns; i++)
        if (runs[i].agent == a - agents && runs[i].status == RUN_RUNNING)
            return &runs[i];
    return NULL;
}

static struct run *latest_run(struct agent *a)
{
    for (int i = nruns - 1; i >= 0; i--)
        if (runs[i].agent == a - agents)
            return &runs[i];
    return NULL;
}

static void put_id(cJSON *o, const char *key, const char *prefix, int id)
{
    char s[32];
    snprintf(s, sizeof s, "%s%d", prefix, id);
    cJSON_AddStringToObject(o, key, s);
}

static void put_str(cJSON *o, const char *key, const char *v)
{
    if (v && *v)
        cJSON_AddStringToObject(o, key, v);
    else
        cJSON_AddNullToObject(o, key);
}

static cJSON *usage_json(const backend_result *m)
{
    cJSON *u = cJSON_CreateObject();
    if (m->input_tokens)
        cJSON_AddNumberToObject(u, "input_tokens", (double)m->input_tokens);
    if (m->output_tokens)
        cJSON_AddNumberToObject(u, "output_tokens", (double)m->output_tokens);
    if (m->cache_read_tokens)
        cJSON_AddNumberToObject(u, "cache_read_tokens", (double)m->cache_read_tokens);
    if (m->cache_creation_tokens)
        cJSON_AddNumberToObject(u, "cache_creation_tokens", (double)m->cache_creation_tokens);
    if (m->cost_usd > 0)
        cJSON_AddNumberToObject(u, "cost_usd", m->cost_usd);
    if (m->context_tokens)
        cJSON_AddNumberToObject(u, "context_tokens", (double)m->context_tokens);
    if (m->context_window)
        cJSON_AddNumberToObject(u, "context_window", (double)m->context_window);
    return u;
}

static cJSON *run_json(const struct run *r)
{
    cJSON *o = cJSON_CreateObject();
    put_id(o, "id", "run_", r->id);
    put_id(o, "agent_id", "ag_", agents[r->agent].id);
    cJSON_AddStringToObject(o, "status", RUN_STATUS[r->status]);
    cJSON_AddStringToObject(o, "source", r->from_tab ? "tab" : "api");
    put_str(o, "prompt", r->prompt);
    put_str(o, "result", r->result);
    if (r->error)
        cJSON_AddStringToObject(o, "error", r->error);
    if (r->has_usage)
        cJSON_AddItemToObject(o, "usage", usage_json(&r->usage));
    cJSON_AddStringToObject(o, "created_at", r->created);
    put_str(o, "started_at", r->started);
    put_str(o, "finished_at", r->finished);
    return o;
}

static cJSON *agent_json(struct agent *a)
{
    cJSON *o = cJSON_CreateObject();
    put_id(o, "id", "ag_", a->id);
    put_str(o, "backend", a->backend);
    put_str(o, "model", a->model);
    put_str(o, "effort", a->effort);
    put_str(o, "cwd", a->cwd);
    put_str(o, "title", a->title);
    tab_of(a);
    put_str(o, "session_id", a->sid);
    cJSON_AddStringToObject(o, "status", agent_status(a));
    struct run *r = latest_run(a);
    if (r)
        put_id(o, "latest_run_id", "run_", r->id);
    else
        cJSON_AddNullToObject(o, "latest_run_id");
    cJSON_AddStringToObject(o, "created_at", a->created);
    cJSON_AddStringToObject(o, "updated_at", a->updated);
    return o;
}

static cJSON *event_base(struct agent *a, struct run *r)
{
    cJSON *o = cJSON_CreateObject();
    put_id(o, "agent_id", "ag_", a->id);
    if (r)
        put_id(o, "run_id", "run_", r->id);
    return o;
}

/* an agent event when its status differs from the last one sent */
static void note_agent(struct agent *a)
{
    const char *st = agent_status(a);
    if (a->shown && !strcmp(a->shown, st))
        return;
    a->shown = st;
    now_iso(a->updated);
    cJSON *o = event_base(a, NULL);
    cJSON_AddStringToObject(o, "status", st);
    emit("agent", o);
}

static void run_status(struct run *r, int status)
{
    r->status = status;
    if (status == RUN_RUNNING)
        now_iso(r->started);
    else if (status > RUN_RUNNING)
        now_iso(r->finished);
    struct agent *a = &agents[r->agent];
    cJSON *o = event_base(a, r);
    cJSON_AddStringToObject(o, "status", RUN_STATUS[status]);
    if (r->error)
        cJSON_AddStringToObject(o, "error", r->error);
    emit("status", o);
    if (status > RUN_RUNNING)
        emit("done", event_base(a, r));
}

static struct run *new_run(struct agent *a, const char *prompt, int from_tab)
{
    if (nruns >= RUNS_MAX)
        return NULL;
    struct run *r = &runs[nruns];
    memset(r, 0, sizeof *r);
    r->id = nruns + 1;
    r->agent = (int)(a - agents);
    r->from_tab = from_tab;
    r->prompt = dup_or_null(prompt);
    now_iso(r->created);
    nruns++;
    return r;
}

static void end_runs(struct agent *a, int status, const char *why)
{
    for (int i = 0; i < nruns; i++) {
        struct run *r = &runs[i];
        if (r->agent != a - agents || r->status > RUN_RUNNING)
            continue;
        if (why && !r->error)
            r->error = strdup(why);
        run_status(r, status);
    }
}

/* ids look like ag_7 / run_12 */
static int parse_id(const char *s, size_t n, const char *prefix)
{
    size_t p = strlen(prefix);
    if (n <= p || strncmp(s, prefix, p))
        return -1;
    int v = 0;
    for (size_t i = p; i < n; i++) {
        if (!isdigit((unsigned char)s[i]) || v > 100000000)
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static struct agent *find_agent(int id)
{
    return id >= 1 && id <= nagents ? &agents[id - 1] : NULL;
}

static struct run *find_run(struct agent *a, int id)
{
    if (id < 1 || id > nruns || runs[id - 1].agent != a - agents)
        return NULL;
    return &runs[id - 1];
}

static int query_flag(const char *query, const char *name)
{
    if (!query)
        return 0;
    size_t n = strlen(name);
    for (const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
        if (!strncmp(p, name, n) && p[n] == '=' && p[n + 1] == '1')
            return 1;
    return 0;
}

static const char *body_str(const cJSON *body, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    return cJSON_IsString(v) && *v->valuestring ? v->valuestring : NULL;
}

static const char *prompt_text(const cJSON *body)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(body, "prompt");
    return body_str(p, "text");
}

static int known_backend(const char *name)
{
    for (const char *const *b = backend_names(); b && *b; b++)
        if (!strcmp(*b, name))
            return 1;
    return 0;
}

static int env_name_ok(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return 0;
    return 1;
}

static void free_env(char **env)
{
    for (char **e = env; e && *e; e++)
        free(*e);
    free(env);
}

/* {"NAME": "value", ...} as a NULL-ended NAME=VALUE list; NULL and *bad set on error */
static char **env_list(const cJSON *obj, const char **bad)
{
    int n = cJSON_GetArraySize(obj);
    char **env = calloc((size_t)n + 1, sizeof *env);
    int i = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, obj) {
        if (!env_name_ok(v->string) || !cJSON_IsString(v)) {
            *bad = v->string;
            free_env(env);
            return NULL;
        }
        size_t len = strlen(v->string) + strlen(v->valuestring) + 2;
        env[i] = malloc(len);
        snprintf(env[i++], len, "%s=%s", v->string, v->valuestring);
    }
    return env;
}

static cJSON *created_json(struct agent *a, struct run *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "agent", agent_json(a));
    cJSON_AddItemToObject(o, "run", run_json(r));
    return o;
}

static void create_agent(struct apicall *c)
{
    const cJSON *b = c->body;
    if (!cJSON_IsObject(b))
        return fail(c, 400, "invalid_input", "body must be a JSON object");
    const char *backend = body_str(b, "backend");
    if (!backend)
        backend = cmd_default_backend();
    if (!backend || !known_backend(backend))
        return fail(c, 400, "unknown_backend", "unknown backend %s", backend ? backend : "(none)");
    const char *text = prompt_text(b);
    if (!text)
        return fail(c, 400, "invalid_input", "prompt.text is required");
    const char *cwd = body_str(b, "cwd");
    struct stat st;
    if (cwd && (cwd[0] != '/' || stat(cwd, &st) || !S_ISDIR(st.st_mode)))
        return fail(c, 400, "invalid_input", "cwd must be an existing absolute directory");
    if (cJSON_GetObjectItemCaseSensitive(b, "worktree") || cJSON_GetObjectItemCaseSensitive(b, "idempotency_key"))
        return fail(c, 400, "invalid_input", "worktree and idempotency_key are not supported yet");
    char **env = NULL;
    const cJSON *ej = cJSON_GetObjectItemCaseSensitive(b, "env");
    if (ej) {
        const char *bad = NULL;
        if (!cJSON_IsObject(ej) || !(env = env_list(ej, &bad)))
            return fail(c, 400, "invalid_input", "env must map variable names to strings (%s)",
                        bad ? bad : "not an object");
    }
    if (nagents >= AGENTS_MAX || nruns >= RUNS_MAX) {
        free_env(env);
        return fail(c, 429, "no_capacity", "this instance has created its maximum of agents");
    }
    if (workspace_count() >= WORKSPACE_MAX) {
        free_env(env);
        return fail(c, 429, "no_capacity", "all %d tabs are in use", WORKSPACE_MAX);
    }

    int at = dispatch_spawn(backend, body_str(b, "model"), body_str(b, "effort"), cwd,
                            body_str(b, "title"), (const char *const *)env, NULL);
    free_env(env);
    if (at < 0)
        return fail(c, 502, "start_failed", "could not start the %s CLI", backend);

    struct agent *a = &agents[nagents];
    memset(a, 0, sizeof *a);
    a->id = nagents + 1;
    a->backend = strdup(backend);
    a->model = dup_or_null(body_str(b, "model"));
    a->effort = dup_or_null(body_str(b, "effort"));
    a->cwd = dup_or_null(cwd);
    a->title = dup_or_null(body_str(b, "title"));
    a->s = workspace_at(at);
    now_iso(a->created);
    now_iso(a->updated);
    nagents++;

    struct run *r = new_run(a, text, 0);
    run_status(r, RUN_QUEUED);
    if (!dispatch_send(at, text)) {
        r->error = strdup("could not send the prompt");
        run_status(r, RUN_ERROR);
    }
    note_agent(a);

    if (tab_of(a) < 0 || a->sid[0])
        return finish(c, 201, created_json(a, r));
    a->held = c;
    a->held_run = (int)(r - runs);
    clock_gettime(CLOCK_MONOTONIC, &a->held_since);
}

static void list_agents(struct apicall *c)
{
    int all = query_flag(c->query, "include_exited");
    cJSON *o = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(o, "agents");
    for (int i = 0; i < nagents; i++)
        if (all || tab_of(&agents[i]) >= 0)
            cJSON_AddItemToArray(list, agent_json(&agents[i]));
    finish(c, 200, o);
}

static void delete_agent(struct apicall *c, struct agent *a)
{
    int at = tab_of(a);
    if (at < 0)
        return fail(c, 409, "agent_exited", "ag_%d has exited", a->id);
    int force = query_flag(c->query, "force");
    if (!force && at == workspace_index())
        return fail(c, 409, "agent_in_view", "ag_%d is the tab in view; pass force=1", a->id);
    if (!force && !strcmp(agent_status(a), "busy"))
        return fail(c, 409, "agent_busy", "ag_%d is working; pass force=1", a->id);
    struct session *s = workspace_at(at);
    if (session_turn_running(s))
        session_interrupt(s);
    workspace_close(at);
    end_runs(a, RUN_CANCELLED, "agent deleted");
    a->exited = 1;
    note_agent(a);
    finish(c, 200, agent_json(a));
}

static void create_run(struct apicall *c, struct agent *a)
{
    int at = tab_of(a);
    if (at < 0)
        return fail(c, 409, "agent_exited", "ag_%d has exited", a->id);
    const char *text = prompt_text(c->body);
    if (!text)
        return fail(c, 400, "invalid_input", "prompt.text is required");
    struct run *r = new_run(a, text, 0);
    if (!r)
        return fail(c, 429, "no_capacity", "this instance has recorded its maximum of runs");
    run_status(r, RUN_QUEUED);
    if (!dispatch_send(at, text)) {
        r->error = strdup("could not send the prompt");
        run_status(r, RUN_ERROR);
    }
    note_agent(a);
    finish(c, 201, run_json(r));
}

static void list_runs(struct apicall *c, struct agent *a)
{
    cJSON *o = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(o, "runs");
    for (int i = 0; i < nruns; i++)
        if (runs[i].agent == a - agents)
            cJSON_AddItemToArray(list, run_json(&runs[i]));
    finish(c, 200, o);
}

static void cancel_run(struct apicall *c, struct agent *a, struct run *r)
{
    if (r->status > RUN_RUNNING)
        return fail(c, 409, "run_finished", "run_%d is already %s", r->id, RUN_STATUS[r->status]);
    int at = tab_of(a);
    if (at < 0)
        return fail(c, 409, "agent_exited", "ag_%d has exited", a->id);
    if (r->status == RUN_QUEUED) {
        workspace_dequeue(at, r->prompt);
        run_status(r, RUN_CANCELLED);
        note_agent(a);
        return finish(c, 200, run_json(r));
    }
    r->cancel = 1;
    session_interrupt(workspace_at(at));
    finish(c, 202, run_json(r));
}

static void capacity(struct apicall *c)
{
    cJSON *o = cJSON_CreateObject();
    int used = workspace_count();
    cJSON_AddNumberToObject(o, "slots", WORKSPACE_MAX);
    cJSON_AddNumberToObject(o, "used", used);
    cJSON_AddNumberToObject(o, "free", WORKSPACE_MAX - used);
    char host[256] = "";
    if (!gethostname(host, sizeof host - 1)) {
        char *dot = strchr(host, '.');
        if (dot)
            *dot = '\0';
    }
    cJSON_AddStringToObject(o, "host", host);
    cJSON_AddNumberToObject(o, "pid", (double)getpid());
    cJSON *list = cJSON_AddArrayToObject(o, "backends");
    for (const char *const *b = backend_names(); b && *b; b++)
        cJSON_AddItemToArray(list, cJSON_CreateString(*b));
    finish(c, 200, o);
}

void apicore_handle(struct apicall *c)
{
    const char *m = c->method, *p = c->path;
    int get = !strcmp(m, "GET"), post = !strcmp(m, "POST"), del = !strcmp(m, "DELETE");

    if (!strcmp(p, "/v1/capacity"))
        return get ? capacity(c) : fail(c, 405, "method_not_allowed", "use GET");
    if (!strcmp(p, "/v1/agents")) {
        if (post)
            return create_agent(c);
        return get ? list_agents(c) : fail(c, 405, "method_not_allowed", "use GET or POST");
    }
    if (strncmp(p, "/v1/agents/", 11))
        return fail(c, 404, "not_found", "no route %s", p);

    /* /v1/agents/{id}[/runs[/{run}[/cancel]]] */
    const char *seg[4] = {0};
    size_t len[4] = {0};
    int n = 0;
    for (const char *s = p + 11; *s && n < 4;) {
        const char *e = strchr(s, '/');
        seg[n] = s;
        len[n++] = e ? (size_t)(e - s) : strlen(s);
        if (!e)
            break;
        s = e + 1;
    }
    struct agent *a = find_agent(parse_id(seg[0], len[0], "ag_"));
    if (!a)
        return fail(c, 404, "not_found", "no such agent");
    if (n == 1) {
        if (get)
            return finish(c, 200, agent_json(a));
        return del ? delete_agent(c, a) : fail(c, 405, "method_not_allowed", "use GET or DELETE");
    }
    if (len[1] != 4 || strncmp(seg[1], "runs", 4))
        return fail(c, 404, "not_found", "no route %s", p);
    if (n == 2) {
        if (post)
            return create_run(c, a);
        return get ? list_runs(c, a) : fail(c, 405, "method_not_allowed", "use GET or POST");
    }
    struct run *r = find_run(a, parse_id(seg[2], len[2], "run_"));
    if (!r)
        return fail(c, 404, "not_found", "no such run");
    if (n == 3)
        return get ? finish(c, 200, run_json(r)) : fail(c, 405, "method_not_allowed", "use GET");
    if (len[3] == 6 && !strncmp(seg[3], "cancel", 6))
        return post ? cancel_run(c, a, r) : fail(c, 405, "method_not_allowed", "use POST");
    fail(c, 404, "not_found", "no route %s", p);
}

void apicore_tick(void)
{
    for (int i = 0; i < nagents; i++) {
        struct agent *a = &agents[i];
        if (a->exited)
            continue;
        int at = tab_of(a);
        if (a->held) {
            struct apicall *c = a->held;
            if (a->sid[0] || at < 0 || since_ms(&a->held_since) >= ID_WAIT_MS) {
                a->held = NULL;
                if (at < 0 && !a->sid[0])
                    fail(c, 502, "start_failed", "the session ended before it reported an id");
                else
                    finish(c, 201, created_json(a, &runs[a->held_run]));
            }
        }
        if (at < 0) {
            end_runs(a, RUN_ERROR, "tab closed");
            a->exited = 1;
        }
        note_agent(a);
    }
}

void apicore_turn_begin(struct session *s)
{
    struct agent *a = agent_for(s);
    if (!a)
        return;
    struct run *r = NULL;
    for (int i = 0; i < nruns && !r; i++)
        if (runs[i].agent == a - agents && runs[i].status == RUN_QUEUED)
            r = &runs[i];
    if (!r && !(r = new_run(a, NULL, 1)))
        return;
    run_status(r, RUN_RUNNING);
    note_agent(a);
}

void apicore_turn_done(struct session *s)
{
    struct agent *a = agent_for(s);
    struct run *r = a ? running_run(a) : NULL;
    if (!r)
        return;
    const backend_result *m = session_last_result(s);
    const char *reply = session_last_reply(s);
    r->usage = *m;
    r->has_usage = 1;
    int status;
    if (r->cancel || m->interrupted)
        status = RUN_CANCELLED;
    else if (!reply || m->is_error) {
        const char *why = session_last_error(s);
        r->error = strdup(why && *why ? why : "the turn failed");
        status = RUN_ERROR;
    } else {
        r->result = strdup(reply);
        status = RUN_FINISHED;
    }
    cJSON *o = event_base(a, r);
    if (status == RUN_FINISHED) {
        cJSON_AddStringToObject(o, "result", r->result);
        cJSON_AddItemToObject(o, "usage", usage_json(&r->usage));
        emit("result", o);
    } else if (status == RUN_ERROR) {
        cJSON_AddStringToObject(o, "message", r->error);
        emit("error", o);
    } else
        cJSON_Delete(o);
    run_status(r, status);
    note_agent(a);
}

void apicore_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    const char *name;
    switch (ev->kind) {
    case BACKEND_EV_ASSISTANT:   name = "assistant";   break;
    case BACKEND_EV_THINKING:    name = "thinking";    break;
    case BACKEND_EV_TOOL:        name = "tool_call";   break;
    case BACKEND_EV_TOOL_RESULT: name = "tool_result"; break;
    default: return;
    }
    struct agent *a = agent_for(s);
    if (!a)
        return;
    cJSON *o = event_base(a, running_run(a));
    if (ev->parent && *ev->parent)
        cJSON_AddStringToObject(o, "parent", ev->parent);
    if (ev->kind == BACKEND_EV_TOOL) {
        put_str(o, "name", ev->name);
        put_str(o, "input", ev->input_json ? ev->input_json : ev->arg);
    } else if (ev->kind == BACKEND_EV_TOOL_RESULT) {
        cJSON_AddBoolToObject(o, "failed", ev->failed);
        char *text = ev->text ? strndup(ev->text, TOOL_TEXT_MAX) : NULL;
        put_str(o, "text", text);
        free(text);
    } else
        put_str(o, "text", ev->text);
    emit(name, o);
}

void apicore_init(apicore_reply_fn reply, apicore_emit_fn emit_fn)
{
    on_reply = reply;
    on_emit = emit_fn;
}

void apicore_reset(void)
{
    for (int i = 0; i < nagents; i++) {
        struct agent *a = &agents[i];
        free(a->backend);
        free(a->model);
        free(a->effort);
        free(a->cwd);
        free(a->title);
    }
    for (int i = 0; i < nruns; i++) {
        free(runs[i].prompt);
        free(runs[i].result);
        free(runs[i].error);
    }
    nagents = nruns = 0;
}
