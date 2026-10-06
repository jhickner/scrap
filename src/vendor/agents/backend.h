/*
 * backend.h — common backend interface for headless coding agents.
 *
 * Includes adapters for the nested claude/, codex/, grok/, pi/, and grokbot/ drivers. Define
 * BACKEND_IMPLEMENTATION in exactly one C file; compile it with one bundled
 * cJSON.c (for example, claude/cJSON.c).
 *
 *     #define BACKEND_IMPLEMENTATION
 *     #include "backend.h"
 *
 *     Backend *be = backend_open("pi", "openrouter/pareto-code", system);
 *     char *reply = be->ask(be, "fix the failing tests");
 *     free(reply);
 *     be->reset(be);       // discard conversation context
 *     be->close(be);
 *
 * backend_open_ex() takes the full option set and the vtable carries the rest of
 * what an interactive front end needs: an explicit start, streamed events, an
 * abort predicate, per-turn accounting, and the session id to resume from.
 */
#ifndef BACKEND_H
#define BACKEND_H

typedef struct Backend Backend;

#define BACKEND_CACHE_MARK "\x1e"

/* Nothing here is retained: a backend copies what it needs. */
typedef struct {
    const char *name;           /* "claude" | "codex" | "grok" | "pi" | "grokbot" | "core";
                                   NULL -> claude                                    */
    const char *model;          /* driver/CLI model identifier; NULL -> its default   */
    const char *effort;         /* reasoning/thinking effort; NULL -> its default     */
    const char *system;         /* applied to every turn; NULL -> none                */
    const char *cwd;            /* where the agent runs its tools; NULL -> inherit    */
    const char *session_file;   /* path exported to the child as MUX_SESSION_FILE, the
                                   file the host writes this session's id into        */
    const char *const *env;     /* NAME=VALUE entries added to the child's environment,
                                   NULL-ended; NULL -> none                          */
    const char *resume_session; /* continue a prior session (claude, codex, grok, pi) */
    int fork_session;           /* copy resumed context into a new session id        */
    const char *permission_mode;/* claude: --permission-mode; NULL -> bypassPermissions*/
    const char *session_name;   /* optional display name; currently used by claude     */
    int ephemeral;              /* do not persist this helper conversation             */
    int disable_tools;          /* helper needs text generation, not machine access    */
    int allow_customizations;   /* claude: load skills, CLAUDE.md, MCP servers, ...   */
    int no_browser_login;       /* claude: report expired auth instead of opening the
                                   browser, for runs with nobody watching it        */
    int chrome;                 /* claude: --chrome, Claude in Chrome browser tools */
    const char *plugin_dir;     /* claude: --plugin-dir, one plugin for this session.
                                   Needs allow_customizations for its hooks to run */
} backend_opts;

/* One interesting event from a turn's stream. Only the fields a kind documents
 * are set, and a driver that reports less leaves more of them NULL. */
typedef enum {
    BACKEND_EV_ASSISTANT,   /* text: an assistant text block                    */
    BACKEND_EV_THINKING,    /* text: a reasoning block                          */
    BACKEND_EV_TOOL,        /* name, plus input_json or arg: a tool invocation  */
    BACKEND_EV_TOOL_RESULT, /* text: output; diff: optional patch; failed: error */
    BACKEND_EV_INIT,        /* name: the model the CLI resolved                 */
    BACKEND_EV_CWD,         /* text: the directory the agent works in now,
                               which is not the launch cwd once it moves into
                               a worktree                                      */
    BACKEND_EV_TRUST,       /* the current project needs a trust decision       */
    BACKEND_EV_WARNING,     /* text: an actionable backend configuration notice */
    BACKEND_EV_TASK,        /* id, name (status), text (description or summary),
                               arg (subagent type or workflow name), task_type:
                               a background task the agent
                               started, which outlives the turn that started it */
} backend_event_kind;

typedef struct {
    backend_event_kind kind;
    const char *text;
    const char *name;
    const char *input_json; /* tool input as compact JSON; NULL when the driver
                               reports no structured input                      */
    const char *arg;        /* the driver's own one-line rendering of the input,
                               for the drivers that have no input_json          */
    const char *diff;       /* authoritative unified patch for a tool result;
                               NULL when the driver does not report one         */
    const char *id;         /* TASK: the backend's own id for the task          */
    const char *task_type;  /* TASK: what the task is (local_bash, local_agent,
                               ...); NULL when the driver does not say          */
    const char *parent;     /* the tool call this event belongs to rather than
                               the session itself -- a subagent's own work comes
                               up the same stream. NULL for the session's own,
                               and for drivers that do not say                  */
    int failed;             /* TOOL_RESULT: the tool did not succeed            */
} backend_event;

/* Accounting for one turn, zeroed before each. Token counts are that turn's;
 * a driver that does not report a field leaves it 0. */
typedef struct {
    double cost_usd;      /* cumulative for the session, as the CLI reports it */
    long   input_tokens;
    long   output_tokens;
    long   cache_read_tokens;
    long   cache_creation_tokens;
    long   context_tokens; /* latest primary-model request, including output */
    long   context_window;
    int    is_error;
    int    interrupted;   /* the abort predicate ended the turn */
    char   subtype[32];   /* the driver's own word for how the turn ended, when
                             it reports one: "success", "error_max_turns", ... */
} backend_result;

/* A tool call the agent asks permission for. Strings are borrowed for the
 * callback; any may be NULL. */
typedef struct {
    const char *tool;
    const char *description;
    const char *input_json;
    const char *path;        /* the path that triggered the prompt, if any */
} backend_permission;

/* Subscription rate limit reported by a backend's local protocol. This is
 * deliberately separate from context usage above: one measures account quota,
 * the other measures how full the current model request is. */
typedef struct {
    int  available;
    int  used_percent;
    long resets_at;
    long window_minutes;
} backend_rate_limit;

/* What a driver supports, in Backend.caps. */
#define BACKEND_CAP_RESUME      1u /* start() can adopt a prior session id */
#define BACKEND_CAP_EFFORT      2u /* set_effort() is supported            */
#define BACKEND_CAP_LIVE_EFFORT 4u /* set_effort() preserves the process   */
#define BACKEND_CAP_TASKS       8u /* reports BACKEND_EV_TASK per subagent  */

struct Backend {
    unsigned caps;

    char *(*ask)(Backend *b, const char *user); /* malloc'd text or NULL */
    int   (*reset)(Backend *b);                 /* fresh context; nonzero on success */
    void  (*close)(Backend *b);                 /* frees b                           */

    /* Start (or restart) the child process now, adopting `resume_session` where
     * the driver supports it (NULL, or an unsupported driver, starts fresh), so
     * a missing CLI surfaces before the first turn. ask() starts lazily when
     * this was never called. Returns nonzero on success. */
    int (*start)(Backend *b, const char *resume_session);
    int (*connect)(Backend *b);

    /* As ask(), also filling *meta, which is zeroed first. `meta` may be NULL. */
    char *(*ask_ex)(Backend *b, const char *user, backend_result *meta);

    /* Start a model turn without appending a user message. Drivers expose this
     * only when their protocol has a native empty-input continuation. */
    char *(*continue_ex)(Backend *b, backend_result *meta);

    /* Context occupancy as the driver knows it right now: during a turn this
     * tracks the latest model request rather than waiting for the turn to end,
     * so a caller painting a live display can follow it. Either output is 0
     * when unknown, and the whole hook is NULL for a driver that never reports
     * usage mid-turn. */
    void (*usage)(Backend *b, long *context_tokens, long *context_window);

    /* Latest primary subscription-rate-limit window, when the backend exposes
     * it locally. Zeroes *out when no reading is available. */
    void (*rate_limit)(Backend *b, backend_rate_limit *out);

    /* Takes effect at the next start(). */
    void (*set_model)(Backend *b, const char *model);

    /* Change the reasoning/thinking effort. For LIVE_EFFORT backends this
     * applies to the existing process; the others take effect at next start. */
    int (*set_effort)(Backend *b, const char *effort);

    /* Takes effect at the next start(). Ignored by the drivers with no
     * permission model of their own. */
    void (*set_permission)(Backend *b, const char *mode);

    /* Persist this folder as trusted in the backend's user configuration.
     * NULL for backends without a project trust model. */
    int (*trust_project)(Backend *b, const char *path);

    /* Per-event sink for a turn, called on the ask() thread as events arrive;
     * the event and its strings are borrowed for the call. Pass NULL to clear. */
    void (*set_event_cb)(Backend *b, void (*cb)(void *ud, const backend_event *ev),
                         void *ud);

    /* Predicate polled while a turn is in flight; when it returns nonzero the
     * turn is abandoned. It is called every ~20ms, so it doubles as both a tick
     * for a caller painting its own display and the point at which a caller
     * that keeps its input live can pick up a keystroke. */
    void (*set_abort_check)(Backend *b, int (*cb)(void));

    /* Approver for tool calls the permission mode would ask about, called on
     * the thread reading the stream (ask() or idle_pump()) and allowed to
     * block. Nonzero allows the call. Takes effect at the next start(); with
     * none set those calls are refused. NULL for drivers without prompts. */
    void (*set_permission_cb)(Backend *b,
                              int (*cb)(void *ud, const backend_permission *req),
                              void *ud);

    /* Some agents run turns between sends — a finished background task wakes
     * the model with no prompt. idle_fd() is an fd that becomes readable when
     * such a turn produces output (-1 while there is nothing to watch, so it is
     * asked again before each wait) and idle_pump() consumes what is there,
     * reporting it through the event sink and returning nonzero while such a
     * turn is still open or announced. Both are for the gap between turns:
     * calling the pump with a turn in flight would steal that turn's stream.
     * NULL for a driver whose agent only speaks when spoken to. */
    int  (*idle_fd)(Backend *b);
    int  (*idle_pump)(Backend *b);
    /* Consume a coalesced continuation request raised by idle_pump(). */
    int  (*take_continuation)(Backend *b);

    /* Work the agent still has running with no turn in flight — background
     * subagents and detached commands outlive the send that started them, so a
     * turn ending is not the work ending. NULL for a driver that cannot say. */
    int  (*busy)(Backend *b);

    /* NULL until known, or when the driver never reports it. */
    const char *(*session_id)(Backend *b);
    const char *(*model)(Backend *b);       /* the model the CLI resolved       */
    const char *(*effort)(Backend *b);      /* resolved effort, or NULL         */
    const char *(*auth_source)(Backend *b); /* "none" -> a subscription login   */
    const char *(*last_error)(Backend *b);  /* the tail of the child's stderr   */

    void *ctx;
};

/* Open "claude", "codex", "grok", "pi", or "grokbot". model is the driver/CLI model identifier
 * (NULL -> its default). system is applied to every turn (NULL -> none).
 * Returns NULL for an unknown name or allocation failure. */
Backend *backend_open(const char *name, const char *model, const char *system);

/* As backend_open, with the rest of the options. `opts` may be NULL. */
Backend *backend_open_ex(const backend_opts *opts);

/* The names backend_open accepts, NULL-terminated. For "grokbot" the model is a
 * bot or group name; with none, the first the gateway lists. */
const char *const *backend_names(void);

/* Fan n prompts across independent agent sessions. Each worker owns one
 * Backend, restarting its context after restart_every calls (0 -> never).
 * on_result calls are serialized; reply is valid only for the callback and is
 * freed afterward. Returns failed call count, or -1 for an unknown backend. */
int backend_run_pool(const char *name, const char *model, const char *system,
                     int workers, int restart_every,
                     const char **prompts, int n,
                     void (*on_result)(void *ud, int index, const char *reply),
                     void *ud);

#endif /* BACKEND_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef BACKEND_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---------- shared adapter scaffolding ----------
 *
 * Every adapter's context starts with a backend_state, so the option copies,
 * the event sink and the abort predicate are managed in one place. A driver's
 * client is created lazily and can be replaced (a restart, a model change), at
 * which point the callbacks are registered again from here.
 *
 * This block comes before the driver includes so that pi.h's own adapter can
 * use it too. */

typedef struct {
    char *model, *effort, *system, *cwd, *resume, *permission, *session_name;
    char *session_file;
    char **env;
    int   allow_customizations, ephemeral, disable_tools, fork_session;
    int   no_browser_login;
    int   chrome;
    char *plugin_dir;
    void (*on_event)(void *ud, const backend_event *ev);
    void *event_ud;
    int (*on_permission)(void *ud, const backend_permission *req);
    void *permission_ud;
    int (*abort)(void);
    /* Assistant and reasoning text arriving in fragments, held until the block
     * it belongs to is complete. */
    backend_event_kind pending_kind;
    char  *pending;
    size_t pending_len, pending_cap;
} backend_state;

static char *backend_dup(const char *s) { return (s && *s) ? strdup(s) : NULL; }

static void backend_set(char **slot, const char *value) {
    free(*slot);
    *slot = backend_dup(value);
}

static void backend_state_init(backend_state *st, const backend_opts *o) {
    st->model  = backend_dup(o->model);
    st->effort = backend_dup(o->effort);
    st->system = backend_dup(o->system);
    st->cwd    = backend_dup(o->cwd);
    st->session_file = backend_dup(o->session_file);
    st->env = NULL;
    if (o->env) {
        size_t n = 0;
        while (o->env[n]) n++;
        st->env = calloc(n + 1, sizeof *st->env);
        for (size_t i = 0; st->env && i < n; i++) st->env[i] = strdup(o->env[i]);
    }
    st->resume = backend_dup(o->resume_session);
    st->permission = backend_dup(o->permission_mode);
    st->session_name = backend_dup(o->session_name);
    st->ephemeral = o->ephemeral;
    st->disable_tools = o->disable_tools;
    st->allow_customizations = o->allow_customizations;
    st->fork_session = o->fork_session;
    st->no_browser_login = o->no_browser_login;
    st->chrome = o->chrome;
    st->plugin_dir = o->plugin_dir ? strdup(o->plugin_dir) : NULL;
}

static void backend_state_free(backend_state *st) {
    free(st->model); free(st->effort); free(st->system); free(st->cwd); free(st->resume);
    free(st->permission); free(st->session_name); free(st->pending);
    free(st->session_file);
    for (char **e = st->env; e && *e; e++) free(*e);
    free(st->env);
    free(st->plugin_dir);
}

static void backend_emit(backend_state *st, const backend_event *ev) {
    if (st->on_event) st->on_event(st->event_ud, ev);
}

/* Release the accumulated text as one block. A caller that renders markdown or
 * wraps a line needs whole blocks, not the word-at-a-time deltas most of these
 * CLIs stream, so nothing reaches it until the block is closed: by an event of
 * another kind, or by the end of the turn. */
static void backend_flush(backend_state *st) {
    if (!st->pending_len) return;
    backend_event ev = { .kind = st->pending_kind, .text = st->pending };
    if (st->on_event) st->on_event(st->event_ud, &ev);
    st->pending_len = 0;
    st->pending[0] = '\0';
}

static void backend_delta(backend_state *st, backend_event_kind kind, const char *text) {
    if (!text || !*text) return;
    if (st->pending_len && st->pending_kind != kind) backend_flush(st);
    st->pending_kind = kind;
    size_t n = strlen(text);
    if (st->pending_len + n + 1 > st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap : 512;
        while (cap < st->pending_len + n + 1) cap *= 2;
        char *grown = realloc(st->pending, cap);
        if (!grown) return;
        st->pending = grown; st->pending_cap = cap;
    }
    memcpy(st->pending + st->pending_len, text, n + 1);
    st->pending_len += n;
}

static void backend_set_model_generic(Backend *b, const char *model) {
    backend_set(&((backend_state *)b->ctx)->model, model);
}
static const char *backend_stored_effort(Backend *b) {
    return ((backend_state *)b->ctx)->effort;
}
static void backend_set_permission_generic(Backend *b, const char *mode) {
    backend_set(&((backend_state *)b->ctx)->permission, mode);
}
static void backend_set_permission_none(Backend *b, const char *mode) {
    (void)b; (void)mode;
}
static const char *backend_none(Backend *b) { (void)b; return NULL; }

#define CLAUDE_IMPLEMENTATION
#include "claude/claude.h"
#define CODEX_IMPLEMENTATION
#include "codex/codex.h"
#define GROK_IMPLEMENTATION
#include "grok/grok.h"
#define PI_IMPLEMENTATION
#define PI_BACKEND_IMPLEMENTATION
#include "pi/pi.h"
#define GROKBOT_IMPLEMENTATION
#include "grokbot/grokbot.h"
#define CORE_AGENT_IMPLEMENTATION
#include "core/core.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* ---------- claude ---------- */

/* `live` is the turn's result block, written in place as events arrive, so it
 * doubles as the running usage report. The window only lands with the turn's
 * final event, so mid-turn it reads 0 and the caller falls back to whatever it
 * last saw. */
typedef struct { backend_state st; claude_client *client; claude_result live; } backend_claude;

static void backend_claude_event(void *ud, const claude_event *e) {
    backend_claude *x = ((Backend *)ud)->ctx;
    backend_event ev = { .text = e->text, .name = e->name, .input_json = e->input_json,
                         .arg = e->arg, .id = e->id, .parent = e->parent,
                         .task_type = e->task_type,
                         .failed = e->failed };
    switch (e->kind) {
    case CLAUDE_EV_ASSISTANT:   ev.kind = BACKEND_EV_ASSISTANT;   break;
    case CLAUDE_EV_THINKING:    ev.kind = BACKEND_EV_THINKING;    break;
    case CLAUDE_EV_TOOL:        ev.kind = BACKEND_EV_TOOL;        break;
    case CLAUDE_EV_TOOL_RESULT: ev.kind = BACKEND_EV_TOOL_RESULT; break;
    case CLAUDE_EV_INIT:        ev.kind = BACKEND_EV_INIT;        break;
    case CLAUDE_EV_CWD:         ev.kind = BACKEND_EV_CWD;         break;
    case CLAUDE_EV_TASK:        ev.kind = BACKEND_EV_TASK;        break;
    default: return;
    }
    backend_emit(&x->st, &ev);
}

static int backend_claude_permission(void *ud, const claude_permission *p) {
    backend_claude *x = ((Backend *)ud)->ctx;
    if (!x->st.on_permission) return 0;
    backend_permission req = { .tool = p->tool, .description = p->description,
                               .input_json = p->input_json, .path = p->path };
    return x->st.on_permission(x->st.permission_ud, &req);
}

static int backend_claude_start(Backend *b, const char *resume) {
    backend_claude *x = b->ctx;
    claude_opts o = {0};
    o.cwd = x->st.cwd;
    o.session_file = x->st.session_file;
    o.env = (const char *const *)x->st.env;
    o.model = x->st.model;
    o.effort = x->st.effort;
    o.append_system = x->st.system;
    o.permission_mode = x->st.permission ? x->st.permission : "bypassPermissions";
    o.use_subscription = 1;
    o.allow_customizations = x->st.allow_customizations;
    o.chrome = x->st.chrome;
    o.plugin_dir = x->st.plugin_dir;
    o.resume_session = resume;
    o.fork_session = x->st.fork_session;
    o.session_name = x->st.session_name;
    o.no_session_persistence = x->st.ephemeral;
    if (x->st.disable_tools) o.tools = "";
    o.permission_prompt = x->st.on_permission != NULL;
    claude_client *c = claude_start(&o);
    if (!c) return 0;
    claude_set_event_cb(c, backend_claude_event, b);
    claude_set_permission_cb(c, backend_claude_permission, b);
    claude_set_abort_check(c, x->st.abort);
    if (x->client) claude_stop(x->client);
    x->client = c;
    x->live.context_tokens = x->live.context_window = 0;
    backend_set(&x->st.resume, resume);
    return 1;
}

static int backend_claude_ready(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client || backend_claude_start(b, x->st.resume);
}

static char *backend_claude_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_claude *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!backend_claude_ready(b)) return NULL;
    claude_result *cr = &x->live;
    char *reply = claude_send_ex(x->client, user, cr);

    /* A subscription access token normally refreshes invisibly. If its refresh
     * token has expired too, let the CLI open its browser login, then replace
     * only the headless child and reconnect it to the same persisted session.
     * Retrying once keeps a second auth failure from becoming a login loop. */
    if (reply && cr->is_error &&
        strstr(reply, "OAuth session expired and could not be refreshed")) {
        backend_event notice = {
            .kind = BACKEND_EV_WARNING,
            .text = "Claude login expired; signing in again in your browser..."
        };

        /* A browser login needs someone at the browser. A run nobody is
         * watching would wait on a login page forever, and a fleet of them
         * opens a page apiece, so it reports the expiry and returns it. */
        if (x->st.no_browser_login) {
            notice.text = "Claude login expired; run: claude auth login";
            backend_emit(&x->st, &notice);
            goto done;
        }

        backend_emit(&x->st, &notice);

        char resume[128] = {0};
        const char *id = claude_session_id(x->client);
        if (!id) id = x->st.resume;
        if (id) snprintf(resume, sizeof resume, "%s", id);

        if (claude_auth_login(x->client)) {
            notice.text = "Claude sign-in complete; retrying your message...";
            backend_emit(&x->st, &notice);
            free(reply);
            reply = NULL;

            /* This is a reconnect, not the user-requested fork that may have
             * created this session originally. */
            int fork_session = x->st.fork_session;
            x->st.fork_session = 0;
            int restarted = backend_claude_start(b, *resume ? resume : NULL);
            x->st.fork_session = fork_session;
            if (restarted) {
                cr = &x->live;
                reply = claude_send_ex(x->client, user, cr);
            }
        } else {
            notice.text = "Claude sign-in did not complete.";
            backend_emit(&x->st, &notice);
        }
    }
done:
    if (meta) {
        meta->cost_usd = cr->cost_usd;
        meta->input_tokens = cr->input_tokens;
        meta->output_tokens = cr->output_tokens;
        meta->cache_read_tokens = cr->cache_read_tokens;
        meta->cache_creation_tokens = cr->cache_creation_tokens;
        meta->context_tokens = cr->context_tokens;
        meta->context_window = cr->context_window;
        meta->is_error = cr->is_error;
        meta->interrupted = cr->interrupted;
        snprintf(meta->subtype, sizeof meta->subtype, "%s", cr->subtype);
    }
    return reply;
}

static void backend_claude_usage(Backend *b, long *tokens, long *window) {
    backend_claude *x = b->ctx;
    *tokens = x->live.context_tokens;
    *window = x->live.context_window;
}

static void backend_claude_rate_limit(Backend *b, backend_rate_limit *out) {
    if (!out) return;
    backend_claude *x = b->ctx;
    claude_rate_limit limit = {0};
    memset(out, 0, sizeof *out);
    claude_get_rate_limit(x->client, &limit);
    out->available = limit.available;
    out->used_percent = limit.used_percent;
    out->resets_at = limit.resets_at;
    out->window_minutes = limit.window_minutes;
}

static char *backend_claude_ask(Backend *b, const char *user) {
    return backend_claude_ask_ex(b, user, NULL);
}

static int backend_claude_reset(Backend *b) {
    backend_claude *x = b->ctx;
    x->live.context_tokens = x->live.context_window = 0;
    if (!x->client) return backend_claude_ready(b);
    if (claude_reset(x->client)) return 1;
    claude_stop(x->client);
    x->client = NULL;
    return backend_claude_start(b, NULL);
}

static void backend_claude_set_event_cb(Backend *b,
                                        void (*cb)(void *ud, const backend_event *ev),
                                        void *ud) {
    backend_claude *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) claude_set_event_cb(x->client, backend_claude_event, b);
}

static void backend_claude_set_abort(Backend *b, int (*cb)(void)) {
    backend_claude *x = b->ctx;
    x->st.abort = cb;
    if (x->client) claude_set_abort_check(x->client, cb);
}

static void backend_claude_set_permission_cb(Backend *b,
                                             int (*cb)(void *ud, const backend_permission *req),
                                             void *ud) {
    backend_claude *x = b->ctx;
    x->st.on_permission = cb; x->st.permission_ud = ud;
}

static int backend_claude_set_effort(Backend *b, const char *effort) {
    backend_claude *x = b->ctx;
    if (x->client && !claude_set_effort(x->client, effort)) return 0;
    backend_set(&x->st.effort, effort);
    return 1;
}

static int backend_claude_idle_fd(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_idle_fd(x->client) : -1;
}
static int backend_claude_idle_pump(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_idle_pump(x->client) : 0;
}
static int backend_claude_busy(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_background_tasks(x->client) : 0;
}

static const char *backend_claude_session_id(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_session_id(x->client) : NULL;
}
static const char *backend_claude_model(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_model(x->client) : NULL;
}
static const char *backend_claude_effort(Backend *b) {
    backend_claude *x = b->ctx;
    if (x->client) {
        const char *e = claude_effort(x->client);
        if (e) return e;
    }
    return x->st.effort;
}
static const char *backend_claude_auth(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_auth_source(x->client) : NULL;
}
static const char *backend_claude_error(Backend *b) {
    backend_claude *x = b->ctx;
    return x->client ? claude_last_error(x->client) : NULL;
}

static void backend_claude_close(Backend *b) {
    backend_claude *x = b->ctx;
    if (x->client) claude_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_claude_open(const backend_opts *o) {
    backend_claude *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT |
              BACKEND_CAP_TASKS;
    b->ask = backend_claude_ask;
    b->reset = backend_claude_reset;
    b->close = backend_claude_close;
    b->start = backend_claude_start;
    b->ask_ex = backend_claude_ask_ex;
    b->usage = backend_claude_usage;
    b->rate_limit = backend_claude_rate_limit;
    b->set_model = backend_set_model_generic;
    b->set_effort = backend_claude_set_effort;
    b->set_permission = backend_set_permission_generic;
    b->set_event_cb = backend_claude_set_event_cb;
    b->set_abort_check = backend_claude_set_abort;
    b->set_permission_cb = backend_claude_set_permission_cb;
    b->idle_fd = backend_claude_idle_fd;
    b->idle_pump = backend_claude_idle_pump;
    b->busy = backend_claude_busy;
    b->session_id = backend_claude_session_id;
    b->model = backend_claude_model;
    b->effort = backend_claude_effort;
    b->auth_source = backend_claude_auth;
    b->last_error = backend_claude_error;
    return b;
}

/* ---------- codex ---------- */

typedef struct { backend_state st; codex_client *client; } backend_codex;

/* Codex streams answer text as deltas. Its app-server tool lifecycle is already
 * structured, including the complete multi-file patch for an Edit result. */
static void backend_codex_event(void *ud, const codex_event *cev) {
    backend_codex *x = ((Backend *)ud)->ctx;
    if (!x->st.on_event) return;
    if (cev->kind == CODEX_EV_ASSISTANT) {
        backend_delta(&x->st, BACKEND_EV_ASSISTANT, cev->text);
    } else if (cev->kind == CODEX_EV_THINKING) {
        backend_flush(&x->st);
        backend_event ev = { .kind = BACKEND_EV_THINKING, .text = cev->text };
        backend_emit(&x->st, &ev);
    } else {
        backend_flush(&x->st);
        backend_event ev = {0};
        if (cev->kind == CODEX_EV_TOOL) {
            ev.kind = BACKEND_EV_TOOL;
            ev.name = cev->name;
            ev.input_json = cev->input_json;
        } else if (cev->kind == CODEX_EV_TOOL_RESULT) {
            ev.kind = BACKEND_EV_TOOL_RESULT;
            ev.text = cev->text;
            ev.diff = cev->diff;
            ev.failed = cev->failed;
        } else if (cev->kind == CODEX_EV_CWD) {
            ev.kind = BACKEND_EV_CWD;
            ev.text = cev->text;
        } else if (cev->kind == CODEX_EV_TRUST) {
            ev.kind = BACKEND_EV_TRUST;
            ev.text = cev->text;
        } else if (cev->kind == CODEX_EV_WARNING) {
            ev.kind = BACKEND_EV_WARNING;
            ev.text = cev->text;
        } else if (cev->kind == CODEX_EV_TASK) {
            ev.kind = BACKEND_EV_TASK;
            ev.id = cev->id;
            ev.name = cev->name;
            ev.text = cev->text;
            ev.arg = "agent";
            ev.parent = cev->parent;
        } else {
            return;
        }
        backend_emit(&x->st, &ev);
    }
}

static int backend_codex_start(Backend *b, const char *resume) {
    backend_codex *x = b->ctx;
    codex_opts o = {0};
    o.cwd = x->st.cwd;
    o.session_file = x->st.session_file;
    o.env = (const char *const *)x->st.env;
    o.model = x->st.model;
    o.effort = x->st.effort;
    o.append_system = x->st.system;
    o.bypass_approvals = 1;
    o.resume_session = resume;
    o.ephemeral = x->st.ephemeral;
    o.fork_session = x->st.fork_session;
    codex_client *c = codex_start(&o);
    if (!c) return 0;
    codex_set_event_cb(c, backend_codex_event, b);
    codex_set_abort_check(c, x->st.abort);
    if (x->client) codex_stop(x->client);
    x->client = c;
    backend_set(&x->st.resume, resume);
    return 1;
}

static int backend_codex_connect(Backend *b) {
    return codex_connect(((backend_codex *)b->ctx)->client);
}

static char *backend_codex_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_codex *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->client && !backend_codex_start(b, x->st.resume)) return NULL;
    codex_result cr = {0};
    char *reply = codex_send_ex(x->client, user, &cr);
    backend_flush(&x->st);
    if (meta) {
        meta->context_tokens = cr.context_tokens;
        meta->context_window = cr.context_window;
        meta->interrupted = cr.interrupted;
        meta->input_tokens = cr.input_tokens;
        meta->output_tokens = cr.output_tokens;
        meta->cache_read_tokens = cr.cache_read_tokens;
        meta->cache_creation_tokens = cr.cache_creation_tokens;
    }
    return reply;
}

static char *backend_codex_continue_ex(Backend *b, backend_result *meta) {
    backend_codex *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->client && !backend_codex_start(b, x->st.resume)) return NULL;
    codex_result cr = {0};
    char *reply = codex_continue_ex(x->client, &cr);
    backend_flush(&x->st);
    if (meta) {
        meta->context_tokens = cr.context_tokens;
        meta->context_window = cr.context_window;
        meta->interrupted = cr.interrupted;
        meta->input_tokens = cr.input_tokens;
        meta->output_tokens = cr.output_tokens;
        meta->cache_read_tokens = cr.cache_read_tokens;
        meta->cache_creation_tokens = cr.cache_creation_tokens;
    }
    return reply;
}

static void backend_codex_usage(Backend *b, long *tokens, long *window) {
    backend_codex *x = b->ctx;
    codex_usage(x->client, tokens, window);
}

static void backend_codex_rate_limit(Backend *b, backend_rate_limit *out) {
    if (!out) return;
    backend_codex *x = b->ctx;
    codex_rate_limit limit = {0};
    memset(out, 0, sizeof *out);
    codex_get_rate_limit(x->client, &limit);
    out->available = limit.available;
    out->used_percent = limit.used_percent;
    out->resets_at = limit.resets_at;
    out->window_minutes = limit.window_minutes;
}

static char *backend_codex_ask(Backend *b, const char *user) {
    return backend_codex_ask_ex(b, user, NULL);
}

static int backend_codex_reset(Backend *b) {
    backend_codex *x = b->ctx;
    if (!x->client) return backend_codex_start(b, NULL);
    codex_reset(x->client);
    return 1;
}

static void backend_codex_set_event_cb(Backend *b,
                                       void (*cb)(void *ud, const backend_event *ev),
                                       void *ud) {
    backend_codex *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) codex_set_event_cb(x->client, backend_codex_event, b);
}

static void backend_codex_set_abort(Backend *b, int (*cb)(void)) {
    backend_codex *x = b->ctx;
    x->st.abort = cb;
    if (x->client) codex_set_abort_check(x->client, cb);
}

static int backend_codex_set_effort(Backend *b, const char *effort) {
    backend_codex *x = b->ctx;
    if (x->client && !codex_set_effort(x->client, effort)) return 0;
    backend_set(&x->st.effort, effort);
    return 1;
}

static int backend_codex_trust_project(Backend *b, const char *path) {
    backend_codex *x = b->ctx;
    return x->client && codex_trust_project(x->client, path);
}

static int backend_codex_idle_fd(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_idle_fd(x->client) : -1;
}

static int backend_codex_idle_pump(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_idle_pump(x->client) : 0;
}

static int backend_codex_take_continuation(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_take_continuation(x->client) : 0;
}

static int backend_codex_busy(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_background_tasks(x->client) : 0;
}

static const char *backend_codex_error(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_last_error(x->client) : NULL;
}

static const char *backend_codex_session_id(Backend *b) {
    backend_codex *x = b->ctx;
    return x->client ? codex_session_id(x->client) : NULL;
}

static const char *backend_codex_model(Backend *b) {
    backend_codex *x = b->ctx;
    if (x->client) {
        const char *m = codex_model(x->client);
        if (m) return m;
    }
    return x->st.model;
}

static const char *backend_codex_effort(Backend *b) {
    backend_codex *x = b->ctx;
    if (x->client) {
        const char *e = codex_effort(x->client);
        if (e) return e;
    }
    return x->st.effort;
}

static void backend_codex_close(Backend *b) {
    backend_codex *x = b->ctx;
    if (x->client) codex_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_codex_open(const backend_opts *o) {
    backend_codex *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT |
              BACKEND_CAP_TASKS;
    b->ask = backend_codex_ask;
    b->reset = backend_codex_reset;
    b->close = backend_codex_close;
    b->start = backend_codex_start;
    b->connect = backend_codex_connect;
    b->ask_ex = backend_codex_ask_ex;
    b->continue_ex = backend_codex_continue_ex;
    b->usage = backend_codex_usage;
    b->rate_limit = backend_codex_rate_limit;
    b->set_model = backend_set_model_generic;
    b->set_effort = backend_codex_set_effort;
    b->set_permission = backend_set_permission_none;
    b->trust_project = backend_codex_trust_project;
    b->set_event_cb = backend_codex_set_event_cb;
    b->set_abort_check = backend_codex_set_abort;
    b->idle_fd = backend_codex_idle_fd;
    b->idle_pump = backend_codex_idle_pump;
    b->take_continuation = backend_codex_take_continuation;
    b->busy = backend_codex_busy;
    b->session_id = backend_codex_session_id;
    b->model = backend_codex_model;
    b->effort = backend_codex_effort;
    b->auth_source = backend_none;
    b->last_error = backend_codex_error;
    return b;
}

/* ---------- grok ---------- */

typedef struct { backend_state st; grok_client *client; } backend_grok;

static void backend_grok_event(void *ud, const grok_event *e) {
    backend_grok *x = ((Backend *)ud)->ctx;
    switch (e->kind) {
    case GROK_EV_ASSISTANT:
        backend_delta(&x->st, BACKEND_EV_ASSISTANT, e->text);
        return;
    case GROK_EV_THINKING:
        backend_delta(&x->st, BACKEND_EV_THINKING, e->text);
        return;
    case GROK_EV_TOOL: {
        backend_event ev = {
            .kind = BACKEND_EV_TOOL,
            .name = e->name,
            .input_json = e->input_json,
            .arg = e->text,
        };
        backend_flush(&x->st);
        backend_emit(&x->st, &ev);
        return;
    }
    case GROK_EV_TOOL_RESULT: {
        backend_event ev = { .kind = BACKEND_EV_TOOL_RESULT, .text = e->text,
                             .diff = e->diff, .failed = e->failed };
        backend_flush(&x->st);
        backend_emit(&x->st, &ev);
        return;
    }
    case GROK_EV_CWD: {
        backend_event ev = { .kind = BACKEND_EV_CWD, .text = e->text };
        backend_emit(&x->st, &ev);
        return;
    }
    }
}

static int backend_grok_start(Backend *b, const char *resume) {
    backend_grok *x = b->ctx;
    grok_opts o = {0};
    o.cwd = x->st.cwd;
    o.session_file = x->st.session_file;
    o.env = (const char *const *)x->st.env;
    o.model = x->st.model;
    o.append_system = x->st.system;
    o.reasoning_effort = x->st.effort ? x->st.effort : getenv("GROK_EFFORT");
    o.resume_session = resume;
    o.no_session = x->st.ephemeral;
    grok_client *c = grok_start(&o);
    if (!c) return 0;
    grok_set_event_cb(c, backend_grok_event, b);
    grok_set_abort_check(c, x->st.abort);
    /* A new session stays lazy, but a requested resume must be known-good
     * before it displaces the client that is currently usable. */
    if (resume && !grok_connect(c)) {
        grok_stop(c);
        return 0;
    }
    if (x->client) grok_stop(x->client);
    x->client = c;
    backend_set(&x->st.resume, resume);
    return 1;
}

static int backend_grok_connect(Backend *b) {
    return grok_connect(((backend_grok *)b->ctx)->client);
}

static char *backend_grok_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_grok *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->client && !backend_grok_start(b, x->st.resume)) return NULL;
    grok_result gr = {0};
    char *reply = grok_send_ex(x->client, user, &gr);
    backend_flush(&x->st);
    if (meta) {
        meta->interrupted = gr.interrupted;
        meta->cost_usd = gr.cost_usd;
        meta->input_tokens = gr.input_tokens;
        meta->output_tokens = gr.output_tokens;
        meta->cache_read_tokens = gr.cache_read_tokens;
        meta->cache_creation_tokens = gr.cache_creation_tokens;
    }
    return reply;
}

static char *backend_grok_ask(Backend *b, const char *user) {
    return backend_grok_ask_ex(b, user, NULL);
}

/* There is no clear command, so a fresh context means a fresh process. */
static int backend_grok_reset(Backend *b) { return backend_grok_start(b, NULL); }

static void backend_grok_set_event_cb(Backend *b,
                                      void (*cb)(void *ud, const backend_event *ev),
                                      void *ud) {
    backend_grok *x = b->ctx;
    x->st.on_event = cb; x->st.event_ud = ud;
    if (x->client) grok_set_event_cb(x->client, backend_grok_event, b);
}

static void backend_grok_set_abort(Backend *b, int (*cb)(void)) {
    backend_grok *x = b->ctx;
    x->st.abort = cb;
    if (x->client) grok_set_abort_check(x->client, cb);
}

static const char *backend_grok_session_id(Backend *b) {
    backend_grok *x = b->ctx;
    return x->client ? grok_session_id(x->client) : NULL;
}

static int backend_grok_set_effort(Backend *b, const char *effort) {
    backend_grok *x = b->ctx;
    if (x->client && !grok_set_effort(x->client, effort)) return 0;
    backend_set(&x->st.effort, effort);
    return 1;
}

static const char *backend_grok_model(Backend *b) {
    backend_grok *x = b->ctx;
    if (x->client) {
        const char *m = grok_model(x->client);
        if (m && *m) return m;
    }
    return x->st.model;
}

static const char *backend_grok_effort(Backend *b) {
    backend_grok *x = b->ctx;
    if (x->client) {
        const char *e = grok_effort(x->client);
        if (e && *e) return e;
        /* Once a session is live the client is authoritative, and nothing in
         * force means the CLI's own default. */
        if (grok_session_id(x->client)) return NULL;
    }
    if (x->st.effort && *x->st.effort)
        return x->st.effort;
    const char *env = getenv("GROK_EFFORT");
    return (env && *env) ? env : NULL;
}

static const char *backend_grok_error(Backend *b) {
    backend_grok *x = b->ctx;
    return x->client ? grok_last_error(x->client) : NULL;
}

static void backend_grok_rate_limit(Backend *b, backend_rate_limit *out) {
    if (!out) return;
    backend_grok *x = b->ctx;
    grok_rate_limit limit = {0};
    memset(out, 0, sizeof *out);
    grok_get_rate_limit(x->client, &limit);
    out->available = limit.available;
    out->used_percent = limit.used_percent;
    out->resets_at = limit.resets_at;
    out->window_minutes = limit.window_minutes;
}

static void backend_grok_close(Backend *b) {
    backend_grok *x = b->ctx;
    if (x->client) grok_stop(x->client);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_grok_open(const backend_opts *o) {
    backend_grok *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT;
    b->ask = backend_grok_ask;
    b->reset = backend_grok_reset;
    b->close = backend_grok_close;
    b->start = backend_grok_start;
    b->connect = backend_grok_connect;
    b->ask_ex = backend_grok_ask_ex;
    b->set_model = backend_set_model_generic;
    b->set_effort = backend_grok_set_effort;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = backend_grok_set_event_cb;
    b->set_abort_check = backend_grok_set_abort;
    b->session_id = backend_grok_session_id;
    b->model = backend_grok_model;
    b->effort = backend_grok_effort;
    b->auth_source = backend_none;
    b->last_error = backend_grok_error;
    b->rate_limit = backend_grok_rate_limit;
    return b;
}

/* ---------- grokbot ----------
 *
 * model names a Grok Bot bot or group. A turn is sendPrompt followed by
 * transcript polling: the user entry carrying our clientNonce gives the
 * requestId, entries under it are the reply, and the turn ends once the
 * agent reports isRunningTurn false. Between turns a poll thread on its own
 * gateway handle queues entries newer than the last turn for idle_pump. */

#define BACKEND_GROKBOT_TIMEOUT_MS (5 * 60 * 1000)
#define BACKEND_GROKBOT_IDLE_MS    3000

typedef struct backend_grokbot_item {
    char *text;
    struct backend_grokbot_item *next;
} backend_grokbot_item;

typedef struct {
    backend_state st;
    grokbot *g;
    char *agent_id, *agent_name;
    char err[512];
    pthread_t thread;
    int thread_on;
    int pipe_fd[2];
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int stop, in_turn;
    double watermark;
    backend_grokbot_item *queue, *queue_tail;
} backend_grokbot;

static double backend_grokbot_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
}

static cJSON *backend_grokbot_tail(grokbot *g, const char *id, int limit) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", id);
    cJSON_AddNumberToObject(body, "limit", limit);
    return grokbot_call(g, "getAgentTranscriptTail", body);
}

/* Reply text of an entry, or NULL for entries that are not bot output. */
static char *backend_grokbot_entry_text(cJSON *e) {
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(e, "kind"));
    if (!kind) return NULL;
    if (!strcmp(kind, "send-message")) {
        const char *t = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(e, "message"), "content"));
        return backend_dup(t);
    }
    if (strcmp(kind, "message")) return NULL;
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(e, "role"));
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(e, "content"));
    if (!role || strcmp(role, "assistant") || !t || !*t) return NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItem(e, "isStreaming"))) return NULL;
    cJSON *to = cJSON_GetObjectItem(e, "toAgent");
    const char *to_name = cJSON_GetStringValue(cJSON_GetObjectItem(to, "name"));
    if (!to_name) return strdup(t);
    size_t n = strlen(to_name) + strlen(t) + 8;
    char *out = malloc(n);
    if (out) snprintf(out, n, "→ %s: %s", to_name, t);
    return out;
}

static int backend_grokbot_is_outbound(cJSON *e) {
    return cJSON_GetObjectItem(e, "toAgent") != NULL;
}

static double backend_grokbot_ts(cJSON *e) {
    cJSON *t = cJSON_GetObjectItem(e, "timestampMs");
    return cJSON_IsNumber(t) ? t->valuedouble : 0;
}

static cJSON *backend_grokbot_entries(cJSON *tail) {
    cJSON *a = cJSON_GetObjectItem(tail, "entries");
    return cJSON_IsArray(a) ? a : NULL;
}

static int backend_grokbot_running(grokbot *g, const char *id) {
    cJSON *list = grokbot_list_agents(g);
    if (!list) return -1;
    int running = -1;
    cJSON *a;
    cJSON_ArrayForEach(a, list) {
        const char *aid = cJSON_GetStringValue(cJSON_GetObjectItem(a, "id"));
        if (!aid || strcmp(aid, id)) continue;
        running = cJSON_IsTrue(cJSON_GetObjectItem(a, "isRunningTurn")) ||
                  cJSON_IsTrue(cJSON_GetObjectItem(a, "isComposingMessage"));
        break;
    }
    cJSON_Delete(list);
    return running;
}

static void *backend_grokbot_poll(void *arg) {
    backend_grokbot *x = arg;
    grokbot *g = grokbot_open();
    char *id = strdup(x->agent_id);
    pthread_mutex_lock(&x->lock);
    while (g && id && !x->stop) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += BACKEND_GROKBOT_IDLE_MS / 1000;
        pthread_cond_timedwait(&x->wake, &x->lock, &until);
        if (x->stop) break;
        if (x->in_turn) continue;
        pthread_mutex_unlock(&x->lock);
        cJSON *tail = backend_grokbot_tail(g, id, 20);
        pthread_mutex_lock(&x->lock);
        if (!tail || x->in_turn || x->stop) { cJSON_Delete(tail); continue; }
        int added = 0;
        double mark = x->watermark;
        cJSON *e;
        cJSON_ArrayForEach(e, backend_grokbot_entries(tail)) {
            double ts = backend_grokbot_ts(e);
            if (ts <= x->watermark) continue;
            if (ts > mark) mark = ts;
            char *text = backend_grokbot_entry_text(e);
            backend_grokbot_item *it = text ? calloc(1, sizeof *it) : NULL;
            if (!it) { free(text); continue; }
            it->text = text;
            if (x->queue_tail) x->queue_tail->next = it; else x->queue = it;
            x->queue_tail = it;
            added = 1;
        }
        x->watermark = mark;
        cJSON_Delete(tail);
        if (added) (void)!write(x->pipe_fd[1], "", 1);
    }
    pthread_mutex_unlock(&x->lock);
    free(id);
    grokbot_close(g);
    return NULL;
}

static void backend_grokbot_drain_queue(backend_grokbot *x) {
    backend_grokbot_item *it = x->queue;
    x->queue = x->queue_tail = NULL;
    while (it) {
        backend_grokbot_item *next = it->next;
        free(it->text); free(it);
        it = next;
    }
    char buf[64];
    while (read(x->pipe_fd[0], buf, sizeof buf) > 0) {}
}

static void backend_grokbot_stop(backend_grokbot *x) {
    if (x->thread_on) {
        pthread_mutex_lock(&x->lock);
        x->stop = 1;
        pthread_cond_signal(&x->wake);
        pthread_mutex_unlock(&x->lock);
        pthread_join(x->thread, NULL);
        x->thread_on = 0;
        x->stop = 0;
    }
    pthread_mutex_lock(&x->lock);
    backend_grokbot_drain_queue(x);
    pthread_mutex_unlock(&x->lock);
    grokbot_close(x->g);
    x->g = NULL;
    free(x->agent_id); free(x->agent_name);
    x->agent_id = x->agent_name = NULL;
}

static int backend_grokbot_start(Backend *b, const char *resume) {
    backend_grokbot *x = b->ctx;
    (void)resume;
    backend_grokbot_stop(x);
    x->err[0] = 0;
    x->g = grokbot_open();
    if (!x->g) {
        snprintf(x->err, sizeof x->err, "%s", grokbot_error(NULL));
        return 0;
    }
    char first[256] = "";
    const char *ref = x->st.model;
    if (!ref) {
        cJSON *list = grokbot_list_agents(x->g);
        cJSON *a0 = cJSON_GetArrayItem(list, 0);
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(a0, "id"));
        if (!id) id = cJSON_GetStringValue(cJSON_GetObjectItem(a0, "agentId"));
        if (id) snprintf(first, sizeof first, "%s", id);
        else if (list) snprintf(x->err, sizeof x->err, "the Grok Bot gateway lists no bots");
        else snprintf(x->err, sizeof x->err, "%s", grokbot_error(x->g));
        cJSON_Delete(list);
        ref = first;
    }
    grokbot_agent a;
    if (!*ref || !grokbot_resolve(x->g, ref, &a)) {
        if (*ref) snprintf(x->err, sizeof x->err, "%s", grokbot_error(x->g));
        grokbot_close(x->g);
        x->g = NULL;
        return 0;
    }
    x->agent_id = strdup(a.id);
    x->agent_name = strdup(*a.name ? a.name : a.id);
    grokbot_agent_free(&a);
    double mark = backend_grokbot_now_ms();
    cJSON *tail = backend_grokbot_tail(x->g, x->agent_id, 5);
    cJSON *e;
    cJSON_ArrayForEach(e, backend_grokbot_entries(tail)) {
        if (backend_grokbot_ts(e) > mark) mark = backend_grokbot_ts(e);
    }
    cJSON_Delete(tail);
    pthread_mutex_lock(&x->lock);
    x->watermark = mark;
    x->in_turn = 0;
    pthread_mutex_unlock(&x->lock);
    x->thread_on = pthread_create(&x->thread, NULL, backend_grokbot_poll, x) == 0;
    backend_event ev = { .kind = BACKEND_EV_INIT, .name = x->agent_name };
    backend_emit(&x->st, &ev);
    return 1;
}

static int backend_grokbot_wait(backend_grokbot *x, int ms) {
    for (int t = 0; t < ms; t += 20) {
        if (x->st.abort && x->st.abort()) return 1;
        usleep(20000);
    }
    return 0;
}

static char *backend_grokbot_ask_ex(Backend *b, const char *user, backend_result *meta) {
    backend_grokbot *x = b->ctx;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->g && !backend_grokbot_start(b, NULL)) {
        if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
        return NULL;
    }
    pthread_mutex_lock(&x->lock);
    x->in_turn = 1;
    pthread_mutex_unlock(&x->lock);

    char nonce[37];
    gb_uuid(nonce);
    double sent_ms = backend_grokbot_now_ms() - 2000;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "agentId", x->agent_id);
    cJSON_AddStringToObject(body, "prompt", user ? user : "");
    cJSON_AddStringToObject(body, "clientNonce", nonce);
    cJSON *sent = grokbot_call(x->g, "sendPrompt", body);

    char *request = NULL, *reply = NULL;
    size_t reply_len = 0;
    cJSON *seen = cJSON_CreateObject();
    double mark = sent_ms, deadline = backend_grokbot_now_ms() + BACKEND_GROKBOT_TIMEOUT_MS;
    int error = !sent, interrupted = 0, got = 0;
    const char *subtype = "success";
    if (!sent) snprintf(x->err, sizeof x->err, "%s", grokbot_error(x->g));
    cJSON_Delete(sent);

    while (!error) {
        if (backend_grokbot_wait(x, 1000)) { interrupted = 1; subtype = "interrupted"; break; }
        cJSON *tail = backend_grokbot_tail(x->g, x->agent_id, 40);
        cJSON *e;
        cJSON_ArrayForEach(e, backend_grokbot_entries(tail)) {
            const char *nc = cJSON_GetStringValue(cJSON_GetObjectItem(e, "clientNonce"));
            const char *rq = cJSON_GetStringValue(cJSON_GetObjectItem(e, "requestId"));
            if (!request && nc && rq && !strcmp(nc, nonce)) request = strdup(rq);
        }
        cJSON_ArrayForEach(e, backend_grokbot_entries(tail)) {
            const char *eid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "id"));
            const char *rq = cJSON_GetStringValue(cJSON_GetObjectItem(e, "requestId"));
            double ts = backend_grokbot_ts(e);
            if (!eid || cJSON_GetObjectItem(seen, eid)) continue;
            if (request ? (!rq || strcmp(rq, request)) : ts <= sent_ms) continue;
            char *text = backend_grokbot_entry_text(e);
            if (!text) continue;
            cJSON_AddTrueToObject(seen, eid);
            if (ts > mark) mark = ts;
            got = 1;
            backend_event ev = { .kind = BACKEND_EV_ASSISTANT, .text = text };
            backend_emit(&x->st, &ev);
            if (!backend_grokbot_is_outbound(e)) {
                size_t n = strlen(text);
                char *grown = realloc(reply, reply_len + n + 3);
                if (grown) {
                    reply = grown;
                    if (reply_len) { memcpy(reply + reply_len, "\n\n", 2); reply_len += 2; }
                    memcpy(reply + reply_len, text, n + 1);
                    reply_len += n;
                }
            }
            free(text);
        }
        cJSON_Delete(tail);
        if (request && got && backend_grokbot_running(x->g, x->agent_id) == 0) break;
        if (backend_grokbot_now_ms() > deadline) {
            error = 1;
            subtype = "timeout";
            snprintf(x->err, sizeof x->err, "no reply from %s within %d s",
                     x->agent_name, BACKEND_GROKBOT_TIMEOUT_MS / 1000);
        }
    }
    if (error && !strcmp(subtype, "success")) subtype = "error";
    cJSON_Delete(seen);
    free(request);

    pthread_mutex_lock(&x->lock);
    if (mark > x->watermark) x->watermark = mark;
    backend_grokbot_drain_queue(x);
    x->in_turn = 0;
    pthread_mutex_unlock(&x->lock);

    if (meta) {
        meta->is_error = error;
        meta->interrupted = interrupted;
        snprintf(meta->subtype, sizeof meta->subtype, "%s", subtype);
    }
    if (error && !reply) return NULL;
    return reply ? reply : strdup("");
}

static char *backend_grokbot_ask(Backend *b, const char *user) {
    return backend_grokbot_ask_ex(b, user, NULL);
}

static int backend_grokbot_reset(Backend *b) { (void)b; return 1; }

static void backend_grokbot_set_event_cb(Backend *b,
                                         void (*cb)(void *ud, const backend_event *ev),
                                         void *ud) {
    backend_grokbot *x = b->ctx;
    x->st.on_event = cb;
    x->st.event_ud = ud;
}

static void backend_grokbot_set_abort(Backend *b, int (*cb)(void)) {
    ((backend_grokbot *)b->ctx)->st.abort = cb;
}

static int backend_grokbot_idle_fd(Backend *b) {
    backend_grokbot *x = b->ctx;
    return x->thread_on ? x->pipe_fd[0] : -1;
}

static int backend_grokbot_idle_pump(Backend *b) {
    backend_grokbot *x = b->ctx;
    pthread_mutex_lock(&x->lock);
    backend_grokbot_item *it = x->queue;
    x->queue = x->queue_tail = NULL;
    char buf[64];
    while (read(x->pipe_fd[0], buf, sizeof buf) > 0) {}
    pthread_mutex_unlock(&x->lock);
    while (it) {
        backend_event ev = { .kind = BACKEND_EV_ASSISTANT, .text = it->text };
        backend_emit(&x->st, &ev);
        backend_grokbot_item *next = it->next;
        free(it->text); free(it);
        it = next;
    }
    return 0;
}

static const char *backend_grokbot_model(Backend *b) {
    backend_grokbot *x = b->ctx;
    return x->agent_name ? x->agent_name : x->st.model;
}

static const char *backend_grokbot_error(Backend *b) {
    backend_grokbot *x = b->ctx;
    return x->err[0] ? x->err : NULL;
}

static void backend_grokbot_close(Backend *b) {
    backend_grokbot *x = b->ctx;
    backend_grokbot_stop(x);
    close(x->pipe_fd[0]);
    close(x->pipe_fd[1]);
    pthread_mutex_destroy(&x->lock);
    pthread_cond_destroy(&x->wake);
    backend_state_free(&x->st);
    free(x); free(b);
}

static Backend *backend_grokbot_open(const backend_opts *o) {
    backend_grokbot *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b || pipe(x->pipe_fd) != 0) { free(x); free(b); return NULL; }
    for (int i = 0; i < 2; i++) {
        fcntl(x->pipe_fd[i], F_SETFL, fcntl(x->pipe_fd[i], F_GETFL) | O_NONBLOCK);
        fcntl(x->pipe_fd[i], F_SETFD, FD_CLOEXEC);
    }
    pthread_mutex_init(&x->lock, NULL);
    pthread_cond_init(&x->wake, NULL);
    backend_state_init(&x->st, o);
    b->ctx = x;
    b->caps = 0;
    b->ask = backend_grokbot_ask;
    b->reset = backend_grokbot_reset;
    b->close = backend_grokbot_close;
    b->start = backend_grokbot_start;
    b->ask_ex = backend_grokbot_ask_ex;
    b->set_model = backend_set_model_generic;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = backend_grokbot_set_event_cb;
    b->set_abort_check = backend_grokbot_set_abort;
    b->idle_fd = backend_grokbot_idle_fd;
    b->idle_pump = backend_grokbot_idle_pump;
    b->session_id = backend_none;
    b->model = backend_grokbot_model;
    b->effort = backend_none;
    b->auth_source = backend_none;
    b->last_error = backend_grokbot_error;
    return b;
}

/* ---------- dispatch ---------- */

static const char *const BACKEND_NAMES[] = { "claude", "codex", "grok", "pi", "grokbot", "core", NULL };

const char *const *backend_names(void) { return BACKEND_NAMES; }

Backend *backend_open_ex(const backend_opts *opts) {
    backend_opts o = opts ? *opts : (backend_opts){0};
    if (!o.name || !strcmp(o.name, "claude")) return backend_claude_open(&o);
    if (!strcmp(o.name, "codex"))             return backend_codex_open(&o);
    if (!strcmp(o.name, "grok"))              return backend_grok_open(&o);
    if (!strcmp(o.name, "pi"))                return pi_backend_open(&o);
    if (!strcmp(o.name, "grokbot"))           return backend_grokbot_open(&o);
    if (!strcmp(o.name, "core"))              return core_agent_open(&o);
    return NULL;
}

Backend *backend_open(const char *name, const char *model, const char *system) {
    backend_opts o = { .name = name, .model = model, .system = system };
    return backend_open_ex(&o);
}

typedef struct {
    const char *name, *model, *system;
    int restart_every, next, failed, n;
    const char **prompts;
    void (*on_result)(void *ud, int index, const char *reply);
    void *ud;
    pthread_mutex_t queue_lock, result_lock;
} backend_pool;

static void *backend_pool_worker(void *arg) {
    backend_pool *p = arg;
    Backend *b = backend_open(p->name, p->model, p->system);
    if (!b) return NULL;
    int since_reset = 0;
    for (;;) {
        pthread_mutex_lock(&p->queue_lock);
        int index = p->next < p->n ? p->next++ : -1;
        pthread_mutex_unlock(&p->queue_lock);
        if (index < 0) break;
        if (p->restart_every > 0 && since_reset &&
            since_reset % p->restart_every == 0) b->reset(b);
        char *reply = b->ask(b, p->prompts[index]);
        since_reset++;
        pthread_mutex_lock(&p->result_lock);
        if (!reply) p->failed++;
        p->on_result(p->ud, index, reply);
        pthread_mutex_unlock(&p->result_lock);
        free(reply);
    }
    b->close(b);
    return NULL;
}

int backend_run_pool(const char *name, const char *model, const char *system,
                     int workers, int restart_every,
                     const char **prompts, int n,
                     void (*on_result)(void *ud, int index, const char *reply),
                     void *ud) {
    Backend *probe = backend_open(name, model, system);
    if (!probe) return -1;
    probe->close(probe);
    if (n <= 0) return 0;
    if (!prompts || !on_result) return n;
    if (workers < 1) workers = 1;
    if (workers > n) workers = n;
    backend_pool p = { .name = name, .model = model, .system = system,
                       .restart_every = restart_every, .prompts = prompts,
                       .n = n, .on_result = on_result, .ud = ud };
    pthread_mutex_init(&p.queue_lock, NULL);
    pthread_mutex_init(&p.result_lock, NULL);
    pthread_t *threads = malloc((size_t)workers * sizeof *threads);
    if (!threads) { pthread_mutex_destroy(&p.queue_lock); pthread_mutex_destroy(&p.result_lock); return n; }
    for (int i = 0; i < workers; i++) pthread_create(&threads[i], NULL, backend_pool_worker, &p);
    for (int i = 0; i < workers; i++) pthread_join(threads[i], NULL);
    free(threads);
    pthread_mutex_destroy(&p.queue_lock);
    pthread_mutex_destroy(&p.result_lock);
    return p.failed;
}

#endif /* BACKEND_IMPLEMENTATION */
