#include "boardwork.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "gitcmd.h"
#include "md.h"
#include "boardcfg.h"
#include "boarddiff.h"
#include "boardstep.h"
#include "child.h"
#include "boardflow.h"
#include "boardlog.h"
#include "boardcmd.h"
#include "boardplan.h"
#include "boardsweep.h"
#include "session.h"
#include "text.h"
#include "workspace.h"

struct worker {
    char            id[BOARD_ID_MAX];
    struct session *session;
    char            job[32];
    char            step[BOARD_STEP_NAME];
    char            backend[32];
    enum board_runs runs;
    int             done;
    int             checked;
    int             handover;
    double          charged_usd;
    long            charged_in, charged_out;
};

static struct worker workers[WORKSPACE_MAX];

static struct worker *slot_of(const char *id)
{
    for (int i = 0; i < WORKSPACE_MAX; i++)
        if (workers[i].session && !strcmp(workers[i].id, id))
            return &workers[i];
    return NULL;
}

static struct worker *slot_by_session(const struct session *s)
{
    for (int i = 0; i < WORKSPACE_MAX; i++)
        if (workers[i].session == s)
            return &workers[i];
    return NULL;
}

/* Sessions report their own running totals; a card outlives several of them,
 * so what it takes on is what this slot's session has added since last time. */
static void charge(struct worker *w, const struct session *s,
                   struct board_card *c)
{
    double usd = session_cost(s);
    long   in = session_tokens_in(s), out = session_tokens_out(s);

    c->cost_usd += usd - w->charged_usd;
    c->tokens_in += in - w->charged_in;
    c->tokens_out += out - w->charged_out;

    w->charged_usd = usd;
    w->charged_in = in;
    w->charged_out = out;
}

static void charge_card(struct worker *w, const struct session *s)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, w->id);
    if (c) {
        struct board_card edited = *c;
        charge(w, s, &edited);
        board_update(&edited);
    }
    board_free(cards, n);
}

int boardwork_running(void)
{
    int n = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++)
        n += workers[i].session != NULL;
    return n;
}

const char *boardwork_card_of(const struct session *s)
{
    struct worker *w = slot_by_session(s);
    return w ? w->id : NULL;
}

int boardwork_tab(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w ? workspace_index_of(w->session) : -1;
}

const char *boardwork_step_job(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w && w->runs != BOARD_RUNS_WORKER ? w->job : NULL;
}

static int side_start(const struct board_card *c, const struct board_role *p,
                      const char *cwd, char *prompt)
{
    if (!prompt || !p) {
        free(prompt);
        return 0;
    }

    int front = workspace_index();
    int at = workspace_spawn(p->backend[0] ? p->backend : "claude",
                             p->model[0] ? p->model : NULL,
                             p->effort[0] ? p->effort : NULL, cwd, NULL);
    if (at < 0) {
        free(prompt);
        return 0;
    }
    workspace_show(front);

    if (!boardwork_hold(c->id, workspace_at(at), p->job, p->step, p->runs)) {
        workspace_close(at);
        free(prompt);
        return 0;
    }

    workspace_send(at, prompt, c->title);
    boardlog_turn(c->id, p->job, prompt, NULL);
    free(prompt);
    return 1;
}

static int agent_start(const struct board_card *c, const struct board_role *p)
{
    return side_start(c, p, c->worktree[0] ? c->worktree : c->cwd,
                      boardstep_prompt(c, p));
}

static int sweeping_any(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    int                yes = 0;
    for (int i = 0; i < n && !yes; i++)
        yes = boardsweep_is(&cards[i]) && slot_of(cards[i].id) != NULL;
    board_free(cards, n);
    return yes;
}

static int sweep_start(const char *cwd, char *prompt)
{
    char id[BOARD_ID_MAX];
    if (!boardsweep_open(cwd, id, sizeof id)) {
        free(prompt);
        return 0;
    }

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    const struct board_card *c = board_find(cards, n, id);
    const struct board_role *p = boardcfg_for_job("sweep");
    int                      started = 0;
    if (c && p)
        started = side_start(c, p, c->cwd, prompt);
    else
        free(prompt);
    board_free(cards, n);

    if (!started)
        board_remove(id);
    return started;
}

int boardwork_sweep_pump(void)
{
    if (sweeping_any())
        return 0;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    char               cwd[4096];
    char              *prompt = NULL;
    if (boardsweep_due(cards, n, cwd, sizeof cwd))
        prompt = boardsweep_prompt(cards, n, cwd);
    board_free(cards, n);
    if (!prompt)
        return 0;

    return sweep_start(cwd, prompt);
}

int boardwork_sweep_now(const char *cwd, char *why, int size)
{
    if (!cwd || !*cwd) {
        snprintf(why, (size_t)size, "no repo to sweep");
        return 0;
    }
    if (sweeping_any()) {
        snprintf(why, (size_t)size, "a sweep is already running");
        return 0;
    }

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    char              *prompt = boardsweep_prompt(cards, n, cwd);
    board_free(cards, n);
    if (!prompt) {
        snprintf(why, (size_t)size, "could not start a sweep");
        return 0;
    }

    if (!sweep_start(cwd, prompt)) {
        snprintf(why, (size_t)size, "could not start a sweep");
        return 0;
    }
    return 1;
}

static void card_backend(const char *id, const char *backend)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    if (c) {
        struct board_card edited = *c;
        snprintf(edited.backend, sizeof edited.backend, "%s", backend);
        edited.model[0] = '\0';
        edited.effort[0] = '\0';
        board_update(&edited);
    }
    board_free(cards, n);
}

static void card_pins(const char *id, char *backend, size_t backend_size,
                      char *tier, size_t tier_size)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    snprintf(backend, backend_size, "%s", c ? c->backend_pin : "");
    snprintf(tier, tier_size, "%s", c ? c->tier_pin : "");
    board_free(cards, n);
}

static const struct board_role *worker_role(const struct worker *w)
{
    char backend[32] = "";
    char tier[8] = "";
    if (w->runs == BOARD_RUNS_WORKER)
        card_pins(w->id, backend, sizeof backend, tier, sizeof tier);
    return boardcfg_for_backend(w->job, backend, tier);
}

static int handover(struct worker *w)
{
    const struct board_role *p = worker_role(w);

    w->handover = 0;
    if (!p || !session_switch_backend(w->session, p->backend))
        return 0;
    if (p->model[0])
        session_set_model(w->session, p->model);
    if (p->effort[0])
        session_set_effort(w->session, p->effort);

    if (w->runs == BOARD_RUNS_WORKER)
        card_backend(w->id, p->backend);

    char said[64];
    snprintf(said, sizeof said, "handed to %s", p->backend);
    board_note(w->id, "board", said);
    return 1;
}

static int follows(const struct worker *w)
{
    const struct board_role *p = worker_role(w);
    return strcmp(session_backend(w->session), p->backend) != 0;
}

int boardwork_serve(int *waiting)
{
    int moved = 0, later = 0;

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        struct worker *w = &workers[i];
        if (!w->session || !follows(w))
            continue;

        int at = workspace_index_of(w->session);
        if (session_turn_running(w->session) || workspace_queued(at)) {
            w->handover = 1;
            later++;
            continue;
        }
        moved += handover(w);
    }

    if (waiting)
        *waiting = later;
    return moved;
}

int boardwork_hold(const char *id, struct session *s, const char *job,
                   const char *step, enum board_runs runs)
{
    if (!id || !*id || !s || slot_of(id))
        return 0;

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (workers[i].session)
            continue;
        memset(&workers[i], 0, sizeof workers[i]);
        snprintf(workers[i].id, sizeof workers[i].id, "%s", id);
        snprintf(workers[i].job, sizeof workers[i].job, "%s", job);
        snprintf(workers[i].step, sizeof workers[i].step, "%s", step ? step : "");
        workers[i].session = s;
        snprintf(workers[i].backend, sizeof workers[i].backend, "%s",
                 session_backend(s));
        workers[i].runs = runs;
        return 1;
    }
    return 0;
}

void boardwork_worktree_of(const char *root, const char *id, char *out,
                           size_t size)
{
    snprintf(out, size, "%s/.claude/worktrees/%s", root, id);
}

void boardwork_branch_of(const char *id, char *out, size_t size)
{
    snprintf(out, size, "worktree-%s", id);
}

int boardwork_tidy_step(const char *root, const char *tree, const char *branch,
                        char *out, size_t size)
{
    char qroot[4200], qtree[4200];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(tree, qtree, sizeof qtree))
        return 0;

    int at = snprintf(out, size,
                      "git -C %s worktree remove --force %s >/dev/null 2>&1\n"
                      "git -C %s branch -D %s >/dev/null 2>&1\n",
                      qroot, qtree, qroot, branch);
    return at > 0 && (size_t)at < size;
}

static int worktree_make(const char *root, const char *id, const char *path,
                         char *why, int size)
{
    char branch[128];
    boardwork_branch_of(id, branch, sizeof branch);
    if (gitcmd_worktree_add(root, path, branch))
        return 1;
    snprintf(why, (size_t)size, "could not make a worktree at %s", path);
    return 0;
}

static void base_of(const char *root, const char *path, const char *kept,
                    char *out, size_t size)
{
    out[0] = '\0';
    if (kept && *kept) {
        snprintf(out, size, "%s", kept);
        return;
    }

    char onto[128], sha[64], args[192];
    if (gitcmd_line(root, "rev-parse --abbrev-ref HEAD", onto, sizeof onto)) {
        snprintf(args, sizeof args, "merge-base HEAD %s", onto);
        if (gitcmd_line(path, args, sha, sizeof sha)) {
            snprintf(args, sizeof args, "rev-parse --short %s", sha);
            gitcmd_line(path, args, out, size);
        }
    }
    if (!out[0])
        gitcmd_line(root, "rev-parse --short HEAD", out, size);
}

static void ignore_card_file(const char *path)
{
    char common[4200];
    if (!gitcmd_line(path, "rev-parse --path-format=absolute --git-common-dir",
                  common, sizeof common))
        return;

    char info[4300];
    snprintf(info, sizeof info, "%s/info", common);
    mkdir(info, 0700);

    char exclude[4400];
    snprintf(exclude, sizeof exclude, "%s/exclude", info);

    FILE *f = fopen(exclude, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            text_chomp(line);
            if (!strcmp(line, "CARD.md")) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    f = fopen(exclude, "a");
    if (!f)
        return;
    fprintf(f, "CARD.md\n");
    fclose(f);
}

static void card_write(FILE *f, const struct board_card *c)
{
    fprintf(f, "# %s\n\n%s\n", c->title, c->body ? c->body : "");
    if (c->kind[0])
        fprintf(f, "\nkind: %s\n", c->kind);
    if (c->log_n) {
        fprintf(f, "\n## Notes\n\n");
        for (int i = 0; i < c->log_n; i++)
            fprintf(f, "- %s: %s\n", c->log[i].who,
                    c->log[i].text ? c->log[i].text : "");
    }
}

static char *card_text(const struct board_card *c)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;
    card_write(f, c);
    fclose(f);
    return buf;
}

static void write_card_file(const char *path, const struct board_card *c)
{
    char file[4300];
    snprintf(file, sizeof file, "%s/CARD.md", path);
    FILE *f = fopen(file, "w");
    if (!f)
        return;
    card_write(f, c);
    fclose(f);
}

static void draw_card(struct session *s, void *ud)
{
    (void)s;
    md_render_kept(ud, 0);
}

static void show_card(int at, const struct board_card *c)
{
    char *text = card_text(c);
    if (!text)
        return;
    workspace_render(at, draw_card, text);
    free(text);
}

static const char *prompt_of(const char *job)
{
    const struct board_role *p = boardcfg_for_job(job);
    return p && p->prompt ? p->prompt : "";
}

static const char *worker_prompt(void)
{
    const struct board_role *p = boardcfg_worker();
    return p && p->prompt ? p->prompt : "";
}

static char *landing_turn(const struct board_card *c)
{
    const char *standing = worker_prompt();
    const struct board_role *p = boardcfg_for_step(c->step);
    const char *head = p && p->fail_prompt[0] ? prompt_of(p->fail_prompt) : "";
    const char *body = c->body && *c->body ? c->body : c->title;

    char onto[128] = "";
    if (!boardcmd_base(c, onto, sizeof onto) || !onto[0])
        snprintf(onto, sizeof onto, "the branch it came from");

    size_t need = strlen(standing) + strlen(head) + strlen(body) +
                  strlen(c->title) + strlen(c->stuck) + sizeof onto + 256;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%s\n\n%s\n\nIt is going back onto %s. The attempt said:\n\n%s\n\n"
             "The card it was built for:\n\n# %s\n\n%s\n",
             standing, head, onto, c->stuck, c->title, body);
    return out;
}

static char *first_turn(const struct board_card *c)
{
    if (c->stuck[0])
        return landing_turn(c);

    const struct board_kind *k = boardcfg_kind(c->kind);
    int                      lands = boardflow_lands(c->kind);

    const char *head = lands ? worker_prompt() : "";
    char       *mine = boardcfg_expand(k && k->prompt ? k->prompt : "", c->id);
    const char *body = c->body && *c->body ? c->body : c->title;
    const char *card = lands
        ? "The card is also written to CARD.md here, which is the copy to go "
          "back to rather than this message."
        : "";

    if (!mine)
        return NULL;

    const struct board_role *mine_at = boardcfg_worker();
    char *since = boardstep_since(c, mine_at ? mine_at->job : "worker");

    size_t need = strlen(head) + strlen(mine) + strlen(body) + strlen(c->title) +
                  strlen(card) + (since ? strlen(since) : 0) + 256;
    char  *out = malloc(need);
    if (!out) {
        free(since);
        free(mine);
        return NULL;
    }

    int at = snprintf(out, need, "%s", head);
    if (*mine)
        at += snprintf(out + at, need - (size_t)at, "%s%s", *head ? "\n\n" : "", mine);
    if (*card)
        at += snprintf(out + at, need - (size_t)at, "\n\n%s", card);
    at += snprintf(out + at, need - (size_t)at, "\n\n# %s\n\n%s\n", c->title, body);
    if (since)
        snprintf(out + at, need - (size_t)at,
                 "\nAn earlier attempt was sent back. What was said about it:\n\n%s",
                 since);
    free(since);
    free(mine);
    return out;
}

static const char *wanted_backend(const struct board_card *c)
{
    if (c->backend_pin[0])
        return c->backend_pin;
    const struct board_role *p = boardcfg_worker();
    const char                 *b = c->backend[0] ? c->backend
                                                  : (p ? p->backend : "");
    return b[0] ? b : "claude";
}

static void pick_after(struct worker *w);

static int card_blocked(const struct board_card *c, char *why, int size)
{
    snprintf(why, (size_t)size, "%s", "");
    if (!c)
        return 1;

    if (boardsweep_is(c)) {
        snprintf(why, (size_t)size, "a sweep card starts on its own");
        return 1;
    }

    if (!c->cwd[0]) {
        snprintf(why, (size_t)size, "no repo on the card");
        return 1;
    }
    if (slot_of(c->id)) {
        snprintf(why, (size_t)size, "already assigned to a worker");
        return 1;
    }
    return 0;
}

int boardwork_blocked(const struct board_card *c, char *why, int size)
{
    if (card_blocked(c, why, size))
        return 1;

    const struct board_cfg *cfg = boardcfg();
    if (boardwork_running() >= cfg->workers) {
        snprintf(why, (size_t)size, "all %d workers busy", cfg->workers);
        return 1;
    }
    if (workspace_count() >= WORKSPACE_MAX) {
        snprintf(why, (size_t)size, "no free tab");
        return 1;
    }

    return 0;
}

static int card_tree(const struct board_card *c, char *path, size_t path_size,
                     char *base, size_t base_size, int *lands_out, char *why,
                     int size)
{
    int lands = boardflow_lands(c->kind);
    if (lands_out)
        *lands_out = lands;
    if (base && base_size)
        base[0] = '\0';

    if (!lands) {
        snprintf(path, path_size, "%s", c->cwd);
        return 1;
    }

    char root[4096];
    if (!gitcmd_root(c->cwd, root, sizeof root)) {
        snprintf(why, (size_t)size, "%s is not in a git repo", c->cwd);
        return 0;
    }
    boardwork_worktree_of(root, c->id, path, path_size);
    if (!worktree_make(root, c->id, path, why, size))
        return 0;

    base_of(root, path, c->base, base, base_size);
    ignore_card_file(path);
    write_card_file(path, c);
    return 1;
}

static int retarget(struct session *s, const char *backend, const char *model,
                    const char *effort, const char *cwd)
{
    int switch_b = strcmp(session_backend(s), backend) != 0;
    const char *here = session_cwd(s);
    int switch_d = !here || strcmp(here, cwd) != 0;

    if (switch_b) {
        session_clear(s);
        if (!session_switch_backend(s, backend))
            return 0;
    }
    if (switch_d) {
        if (!session_set_cwd(s, cwd))
            return 0;
    } else if (!switch_b) {
        session_clear(s);
    }

    session_set_model(s, model && *model ? model : NULL);
    if (effort && *effort)
        session_set_effort(s, effort);
    else
        session_set_effort(s, NULL);
    return 1;
}

static void take_slot(struct worker *w, const char *id)
{
    snprintf(w->id, sizeof w->id, "%s", id);
    const struct board_role *p = boardcfg_worker();
    snprintf(w->job, sizeof w->job, "%s", p ? p->job : "worker");
    snprintf(w->step, sizeof w->step, "%s", p ? p->step : "");
    w->runs = BOARD_RUNS_WORKER;
    w->done = 0;
    w->checked = 0;
    w->handover = 0;
    w->charged_usd = 0;
    w->charged_in = 0;
    w->charged_out = 0;
    snprintf(w->backend, sizeof w->backend, "%s", session_backend(w->session));
}

static int start_on(const struct board_card *c, struct worker *onto, char *why,
                    int size)
{
    if (onto) {
        if (card_blocked(c, why, size))
            return 0;
    } else if (boardwork_blocked(c, why, size)) {
        return 0;
    }

    int  lands = 0;
    char path[4200];
    char base[64];
    if (!card_tree(c, path, sizeof path, base, sizeof base, &lands, why, size))
        return 0;

    const char              *backend = wanted_backend(c);
    const struct board_role *mine = boardcfg_worker();
    const char              *job = mine ? mine->job : "worker";
    const char              *step = mine ? mine->step : "";
    const struct board_role *p = boardcfg_for_backend(job, backend, c->tier_pin);
    const char *model = c->model[0] ? c->model : (p ? p->model : "");
    const char *effort = c->effort[0] ? c->effort : (p ? p->effort : "");

    int             at;
    struct session *s;
    if (onto) {
        s = onto->session;
        at = workspace_index_of(s);
        if (at < 0) {
            snprintf(why, (size_t)size, "worker tab is gone");
            return 0;
        }
        if (!retarget(s, backend, model[0] ? model : NULL,
                      effort[0] ? effort : NULL, path)) {
            snprintf(why, (size_t)size, "could not start a %s session", backend);
            return 0;
        }
        take_slot(onto, c->id);
    } else {
        int front = workspace_index();
        at = workspace_spawn(backend, model[0] ? model : NULL,
                             effort[0] ? effort : NULL, path, NULL);
        if (at < 0) {
            snprintf(why, (size_t)size, "could not start a %s session", backend);
            return 0;
        }
        workspace_show(front);
        s = workspace_at(at);
        boardwork_hold(c->id, s, job, step, BOARD_RUNS_WORKER);
    }

    show_card(at, c);

    char *turn = first_turn(c);
    if (turn) {
        workspace_send(at, turn, c->title);
        boardlog_turn(c->id, job, turn, NULL);
        free(turn);
    }

    struct board_card edited = *c;
    edited.col = BOARD_STEP;
    snprintf(edited.step, sizeof edited.step, "%s", step);
    edited.stuck[0] = '\0';
    snprintf(edited.worktree, sizeof edited.worktree, "%s", lands ? path : "");
    snprintf(edited.base, sizeof edited.base, "%s", base);
    snprintf(edited.backend, sizeof edited.backend, "%s", backend);
    board_update(&edited);

    board_note(c->id, "board", "started");
    return 1;
}

int boardwork_start(const struct board_card *c, char *why, int size)
{
    return start_on(c, NULL, why, size);
}

static const char *landed(const struct board_card *c, int *empty_out)
{
    int files = 0, lines = 0;
    int empty = boardflow_lands(c->kind) && c->worktree[0] && c->base[0] &&
                !boarddiff_size(c, &files, &lines);
    if (empty_out)
        *empty_out = empty;
    if (empty)
        return c->step;
    return boardflow_after_turn(c);
}

void boardwork_finished(struct session *s)
{
    struct worker *w = slot_by_session(s);
    if (!w)
        return;

    const char *sid = session_id(s);
    const char *reply = session_last_reply(s);
    const char *failed = session_last_error(s);

    /* stderr is only the story of the turn when the turn said nothing: a CLI
     * that logged a warning and then answered has not failed. */
    if (reply && *reply)
        failed = NULL;

    if (w->runs != BOARD_RUNS_WORKER) {
        w->done = 1;
        charge_card(w, s);
        if (failed && *failed) {
            char said[1024];
            snprintf(said, sizeof said, "the turn failed: %s", failed);
            board_note(w->id, w->job, said);
        }

        const char *answer = failed && *failed ? NULL : reply;

        struct board_card *held = NULL;
        int                m = board_load(&held);
        struct board_card *at = board_find(held, m, w->id);
        if (at && boardsweep_is(at))
            boardsweep_finished(w->id, answer);
        else if (at)
            boardstep_finished(at, boardcfg_for_step(w->step), answer);
        board_free(held, m);
        return;
    }

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, w->id);
    if (!c) {
        board_free(cards, n);
        return;
    }

    struct board_card edited = *c;
    if (sid && *sid)
        snprintf(edited.session, sizeof edited.session, "%s", sid);

    charge(w, s, &edited);

    boardlog_turn(w->id, w->job, NULL, failed && *failed ? failed : reply);

    int         empty = 0;
    const char *next = landed(c, &empty);

    if ((!failed || !*failed) && !empty) {
        edited.col = next ? BOARD_STEP : BOARD_DONE;
        snprintf(edited.step, sizeof edited.step, "%s", next ? next : "");
    }
    int stored = board_update(&edited);
    board_free(cards, n);

    if (!stored)
        board_note(w->id, "board", "the store did not take the update");

    if (failed && *failed) {
        size_t need = strlen(failed) + 32;
        char  *said = malloc(need);
        if (said) {
            snprintf(said, need, "the turn failed: %s", failed);
            board_note(w->id, w->job, said);
            free(said);
        } else {
            board_note(w->id, w->job, failed);
        }
    } else {
        board_note(w->id, w->job,
                   reply && *reply ? reply : "finished without saying anything");
    }
    if (empty)
        board_note(w->id, "board", "no commit on the branch");
    else if ((!failed || !*failed) && stored)
        pick_after(w);
}

static char pull_failed[BOARD_ID_MAX];

static int pull_priority(const struct board_card *c)
{
    return c->priority ? c->priority : boardcfg_priority(c->kind);
}

static const struct board_card *pull_next(const struct board_card *cards, int n,
                                         int reuse)
{
    const struct board_card *best = NULL;
    int                      best_p = 0;

    for (int i = 0; i < n; i++) {
        const struct board_card *c = &cards[i];
        if (c->col != BOARD_BACKLOG)
            continue;
        /* a card that has already had a worker is back here because a person
           sent it back; pulling it again would undo that */
        if (c->session[0] || c->worktree[0])
            continue;
        if (!strcmp(c->id, pull_failed))
            continue;

        char why[256];
        if (reuse) {
            if (card_blocked(c, why, sizeof why))
                continue;
        } else if (boardwork_blocked(c, why, sizeof why)) {
            continue;
        }

        int p = pull_priority(c);
        if (best && p <= best_p && (p != best_p || c->created >= best->created))
            continue;
        best = c;
        best_p = p;
    }
    return best;
}

static void pick_after(struct worker *w)
{
    if (!w || w->runs != BOARD_RUNS_WORKER || !boardcfg()->auto_pick)
        return;

    int at = workspace_index_of(w->session);
    if (at < 0 || session_turn_running(w->session) || workspace_queued(at))
        return;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    const struct board_card *c = pull_next(cards, n, 1);
    if (!c) {
        board_free(cards, n);
        return;
    }

    char why[256];
    if (!start_on(c, w, why, sizeof why)) {
        snprintf(pull_failed, sizeof pull_failed, "%s", c->id);
        board_note(c->id, "board", why[0] ? why : "could not start the card");
    }
    board_free(cards, n);
}

static int pull_pump(const struct board_card *cards, int n)
{
    const struct board_cfg *cfg = boardcfg();
    if (!cfg->auto_pull || boardwork_running() >= cfg->workers)
        return 0;

    const struct board_card *c = pull_next(cards, n, 0);
    if (!c)
        return 0;

    char why[256];
    if (boardwork_start(c, why, sizeof why))
        return 1;

    snprintf(pull_failed, sizeof pull_failed, "%s", c->id);
    board_note(c->id, "board", why[0] ? why : "could not start the card");
    return 1;
}

static int step_start(const struct board_card *c)
{
    const struct board_role *p = boardflow_role(c);
    if (!p || slot_of(c->id) || boardsweep_is(c))
        return 0;

    switch (p->runs) {
    case BOARD_RUNS_AGENT:
        return agent_start(c, p);
    case BOARD_RUNS_WORKER: {
        if (!c->stuck[0] || boardwork_tab(c->id) >= 0)
            return 0;
        char why[256];
        return boardwork_start(c, why, sizeof why);
    }
    default:
        return 0;
    }
}

int boardwork_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++)
        if (cards[i].col == BOARD_STEP)
            started = step_start(&cards[i]);
    if (!started)
        started = pull_pump(cards, n);
    board_free(cards, n);
    return started;
}

static int drop_worktree(const struct board_card *c)
{
    char root[4096], branch[128], step[BOARDWORK_TIDY_MAX];
    if (!gitcmd_root(c->cwd, root, sizeof root))
        return 1;

    boardwork_branch_of(c->id, branch, sizeof branch);
    if (!boardwork_tidy_step(root, c->worktree, branch, step, sizeof step))
        return 1;
    return system(step) != -1;
}

int boardwork_release(const struct board_card *c)
{
    if (!c || !c->worktree[0] || boardwork_tab(c->id) >= 0)
        return 0;
    if (!drop_worktree(c))
        return 0;

    struct board_card edited = *c;
    edited.worktree[0] = '\0';
    edited.base[0] = '\0';
    edited.stuck[0] = '\0';
    return board_update(&edited);
}

static int base_tab(const char *cwd)
{
    int any = -1;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        if (boardwork_card_of(s))
            continue;
        const char *at = session_cwd(s);
        if (cwd && *cwd && at && !strcmp(at, cwd))
            return i;
        if (any < 0)
            any = i;
    }
    return any;
}

void boardwork_leave(const struct board_card *c)
{
    struct worker *w = c ? slot_of(c->id) : NULL;
    if (!w || workspace_index_of(w->session) != workspace_index())
        return;

    int at = base_tab(c->cwd);
    if (at >= 0) {
        workspace_show(at);
        return;
    }

    char backend[32];
    snprintf(backend, sizeof backend, "%s", session_backend(w->session));
    workspace_spawn(backend, NULL, NULL, c->cwd[0] ? c->cwd : NULL, NULL);
}

void boardwork_let_go(const char *id)
{
    struct worker *w = slot_of(id);
    if (!w)
        return;
    int at = workspace_index_of(w->session);
    memset(w, 0, sizeof *w);
    if (at >= 0 && workspace_count() > 1)
        workspace_close(at);
}

static int at_role(const struct worker *w, const struct board_card *c)
{
    if (c->col != BOARD_STEP)
        return 0;
    if (w->runs != BOARD_RUNS_WORKER)
        return !strcmp(c->step, w->step);
    return !strcmp(c->step, w->step) ||
           boardflow_runs(c) == BOARD_RUNS_PERSON;
}

static int reconcile(struct worker *w, const struct board_card *c)
{
    if (w->runs != BOARD_RUNS_WORKER || strcmp(c->step, w->step))
        return 0;

    int at = workspace_index_of(w->session);
    if (at < 0 || session_busy(w->session) || workspace_queued(at)) {
        w->checked = 0;
        return 0;
    }
    if (w->checked)
        return 0;
    w->checked = 1;

    const char *reply = session_last_reply(w->session);
    if (!reply || !*reply)
        return 0;

    int         empty = 0;
    const char *next = landed(c, &empty);
    if (empty)
        return 0;

    return board_move_to(c->id, next, "board", "worker turn is over");
}

static void switched(struct worker *w)
{
    snprintf(w->backend, sizeof w->backend, "%s", session_backend(w->session));
    w->charged_usd = 0;
    w->charged_in = 0;
    w->charged_out = 0;
    if (w->runs == BOARD_RUNS_WORKER)
        card_backend(w->id, w->backend);
}

static struct session *tree_session(const char *tree)
{
    if (!tree || !*tree)
        return NULL;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        if (slot_by_session(s))
            continue;
        const char *at = session_cwd(s);
        if (at && !strcmp(at, tree))
            return s;
    }
    return NULL;
}

static int strayed(const struct worker *w, const struct board_card *c)
{
    if (!c->worktree[0])
        return 0;
    const char *at = session_cwd(w->session);
    return !at || strcmp(at, c->worktree) != 0;
}

static int rebind(struct worker *w, const struct board_card *c)
{
    struct session *s = tree_session(c->worktree);
    if (!s)
        return 0;

    w->session = s;
    w->done = 0;
    w->checked = 0;
    w->handover = 0;
    w->charged_usd = 0;
    w->charged_in = 0;
    w->charged_out = 0;
    switched(w);
    board_note(w->id, "board", "watching the session in the worktree");
    return 1;
}

int boardwork_poll(void)
{
    int changed = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!workers[i].session)
            continue;
        if (workers[i].done) {
            boardwork_let_go(workers[i].id);
            changed = 1;
            continue;
        }
        int tab = workspace_index_of(workers[i].session);
        if (tab >= 0) {
            if (strcmp(session_backend(workers[i].session), workers[i].backend)) {
                switched(&workers[i]);
                changed = 1;
            }
            if (workers[i].handover && !session_turn_running(workers[i].session) &&
                !workspace_queued(tab)) {
                if (follows(&workers[i]))
                    changed |= handover(&workers[i]);
                else
                    workers[i].handover = 0;
            }
            continue;
        }

        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && rebind(&workers[i], c)) {
            board_free(cards, n);
            changed = 1;
            continue;
        }
        if (c && board_at(c, workers[i].step)) {
            board_move(workers[i].id, BOARD_BACKLOG, NULL, "board",
                       "worker session ended");
            changed = 1;
        }
        board_free(cards, n);
        memset(&workers[i], 0, sizeof workers[i]);
    }

    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!workers[i].session)
            continue;
        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && strayed(&workers[i], c) && rebind(&workers[i], c))
            changed = 1;
        if (c && at_role(&workers[i], c)) {
            changed |= reconcile(&workers[i], c);
            continue;
        }
        boardwork_let_go(workers[i].id);
        changed = 1;
    }

    for (int i = 0; i < n; i++) {
        if (!cards[i].worktree[0])
            continue;
        if (cards[i].col != BOARD_DONE)
            continue;
        if (!boardwork_release(&cards[i]))
            continue;
        board_note(cards[i].id, "board", "worktree removed");
        changed = 1;
    }
    board_free(cards, n);
    return changed;
}

void boardwork_halt(const char *id)
{
    static const char *const STAGES[] = {"triage:", BOARDCMD_KEY};
    for (size_t i = 0; i < sizeof STAGES / sizeof *STAGES; i++) {
        char key[CHILD_KEY_MAX];
        snprintf(key, sizeof key, "%s%s", STAGES[i], id);
        child_stop(key);
    }

    boardwork_let_go(id);
}

void boardwork_discard(const struct board_card *c)
{
    if (!c)
        return;

    boardwork_halt(c->id);
    if (c->worktree[0])
        drop_worktree(c);
}

int boardwork_approve(const struct board_card *c, int force)
{
    const struct board_role *p = boardflow_role(c);
    if (!c || !p)
        return 0;

    boardwork_leave(c);
    boardwork_let_go(c->id);

    char said[64];
    snprintf(said, sizeof said, "%s%s", p->pass_label,
             force ? ", and the step it would pass over" : "");
    board_note(c->id, "you", said);

    return board_move_to(c->id, boardflow_next(c, c->step, force), "you", NULL);
}

int boardwork_send_back(const struct board_card *c, const char *why)
{
    if (!c || !why || !*why)
        return 0;

    struct board_card edited = *c;
    edited.merge_into[0] = '\0';
    edited.merge_from[0] = '\0';
    edited.merge_to[0] = '\0';
    board_update(&edited);

    boardwork_let_go(c->id);
    if (edited.col == BOARD_DONE && edited.worktree[0])
        boardwork_release(&edited);

    return boardwork_reject(c, why);
}

int boardwork_reject(const struct board_card *c, const char *why)
{
    if (!c)
        return 0;
    boardwork_let_go(c->id);

    return board_move_back(c->id, boardflow_fail(c), "you",
                           why && *why ? why : "rejected");
}

void boardwork_spoke_to(struct session *s)
{
    struct worker *w = slot_by_session(s);
    if (!w || w->runs != BOARD_RUNS_WORKER)
        return;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, w->id);
    if (c && boardflow_runs(c) == BOARD_RUNS_PERSON &&
        board_move(w->id, BOARD_STEP, w->step, "you", NULL))
        w->checked = 0;
    board_free(cards, n);
}

int boardwork_feedback(const struct board_card *c, const char *text)
{
    if (!c || !text || !*text)
        return 0;

    int at = boardwork_tab(c->id);
    if (at < 0)
        return 0;

    struct worker *w = slot_of(c->id);
    board_note(c->id, "you", text);
    int moved = board_move(c->id, BOARD_STEP,
                           w && w->step[0] ? w->step : boardflow_start(), "you",
                           NULL);
    workspace_send(at, text, NULL);
    return moved;
}
