#include "boardwork.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "gitcmd.h"
#include "boardcfg.h"
#include "boarddiff.h"
#include "boardstep.h"
#include "child.h"
#include "boardflow.h"
#include "boardlog.h"
#include "session.h"
#include "sessionload.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

struct worker {
    char            id[BOARD_ID_MAX];
    struct session *session;
    char            job[32];
    char            repo[4096];
    char            backend[32];
    int             done;
    int             attached;
    int             checked;
    int             handover;
    double          began;
    double          charged_usd;
    long            charged_in, charged_out;
};

#define TIDY_MAX 16384

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

const char *boardwork_step_job(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w && w->job[0] ? w->job : NULL;
}

double boardwork_elapsed(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w && w->began ? now_seconds() - w->began : 0;
}

int boardwork_tab(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w ? workspace_index_of(w->session) : -1;
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

static int worker_action(const struct worker *w, const struct board_card *c,
                         struct board_action *out)
{
    return boardcfg_for_backend(w->job, c ? c->backend_pin : "",
                                c ? c->tier_pin : "", out);
}

static int handover(struct worker *w, const struct board_card *c)
{
    struct board_action p;
    int                 have = worker_action(w, c, &p);

    w->handover = 0;
    if (!have || !session_switch_backend(w->session, p.backend))
        return 0;
    if (p.model[0])
        session_set_model(w->session, p.model);
    if (p.effort[0])
        session_set_effort(w->session, p.effort);

    card_backend(w->id, p.backend);

    char said[64];
    snprintf(said, sizeof said, "handed to %s", p.backend);
    board_note(w->id, "board", said);
    return 1;
}

static int follows(const struct worker *w, const struct board_card *c)
{
    struct board_action p;
    return worker_action(w, c, &p) &&
           strcmp(session_backend(w->session), p.backend) != 0;
}

int boardwork_serve(int *waiting)
{
    int moved = 0, later = 0;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        struct worker           *w = &workers[i];
        const struct board_card *c = w->session ? board_find(cards, n, w->id) : NULL;
        if (!w->session || !follows(w, c))
            continue;

        int at = workspace_index_of(w->session);
        if (session_turn_running(w->session) || workspace_queued(at)) {
            w->handover = 1;
            later++;
            continue;
        }
        moved += handover(w, c);
    }

    board_free(cards, n);

    if (waiting)
        *waiting = later;
    return moved;
}

static int hold(const char *id, const char *repo, struct session *s,
                const char *job)
{
    if (!id || !*id || !s || slot_of(id))
        return 0;

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (workers[i].session)
            continue;
        memset(&workers[i], 0, sizeof workers[i]);
        snprintf(workers[i].id, sizeof workers[i].id, "%s", id);
        snprintf(workers[i].job, sizeof workers[i].job, "%s", job);
        snprintf(workers[i].repo, sizeof workers[i].repo, "%s", repo ? repo : "");
        workers[i].session = s;
        workers[i].began = now_seconds();
        snprintf(workers[i].backend, sizeof workers[i].backend, "%s",
                 session_backend(s));
        return 1;
    }
    return 0;
}

static void worktree_of(const char *root, const char *id, char *out,
                        size_t size)
{
    snprintf(out, size, "%s/.claude/worktrees/%s", root, id);
}

static int tidy_step(const char *root, const char *tree, const char *branch,
                     char *out, size_t size)
{
    char qroot[4200], qtree[4200], qbranch[520];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(tree, qtree, sizeof qtree) ||
        !text_shell_quote(branch, qbranch, sizeof qbranch))
        return 0;

    int at = snprintf(out, size,
                      "git -C %s worktree remove --force %s >/dev/null 2>&1\n"
                      "git -C %s branch -D %s >/dev/null 2>&1\n",
                      qroot, qtree, qroot, qbranch);
    return at > 0 && (size_t)at < size;
}

static int worktree_make(const char *root, const char *id, const char *path,
                         char *why, int size)
{
    char branch[128];
    board_branch(id, branch, sizeof branch);
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

/* the board used to add CARD.md to the shared git dir's info/exclude, which is
   repo-wide and permanent and blocks the first `git add CARD.md` */
static void unignore_card_file(const char *path)
{
    char common[4200];
    if (!gitcmd_line(path, "rev-parse --path-format=absolute --git-common-dir",
                  common, sizeof common))
        return;

    char exclude[4400];
    snprintf(exclude, sizeof exclude, "%s/info/exclude", common);

    FILE *f = fopen(exclude, "r");
    if (!f)
        return;

    char  *kept = NULL;
    size_t len = 0;
    FILE  *out = open_memstream(&kept, &len);
    if (!out) {
        fclose(f);
        return;
    }

    int dropped = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char bare[512];
        snprintf(bare, sizeof bare, "%s", line);
        text_chomp(bare);
        if (!strcmp(bare, "CARD.md")) {
            dropped = 1;
            continue;
        }
        fputs(line, out);
    }
    fclose(f);
    fclose(out);

    if (dropped) {
        f = fopen(exclude, "w");
        if (f) {
            fwrite(kept, 1, len, f);
            fclose(f);
        }
    }
    free(kept);
}

/* a short card is its own title, and writing both prints the same line twice */
static int same_text(const char *a, const char *b)
{
    while (*a && isspace((unsigned char)*a))
        a++;
    while (*b && isspace((unsigned char)*b))
        b++;

    size_t na = strlen(a), nb = strlen(b);
    while (na && isspace((unsigned char)a[na - 1]))
        na--;
    while (nb && isspace((unsigned char)b[nb - 1]))
        nb--;

    return na == nb && !strncmp(a, b, na);
}

static void card_write(FILE *f, const struct board_card *c)
{
    const char *body = c->body ? c->body : "";
    if (*body && !same_text(body, c->title))
        fprintf(f, "# %s\n\n%s\n", c->title, body);
    else
        fprintf(f, "# %s\n", c->title);
    if (c->worktree[0])
        fprintf(f, "\nworktree %s\n", c->worktree);
    if (c->log_n) {
        fprintf(f, "\n## Notes\n\n");
        for (int i = 0; i < c->log_n; i++)
            fprintf(f, "- %s: %s\n", c->log[i].who,
                    c->log[i].text ? c->log[i].text : "");
    }
}

/* CARD.md lives on the card's branch, so the plan has a history and travels
   with a clone. The board writes it once and commits it; the worker owns it
   from then on. */
static int commit_card_file(const char *tree, const char *why)
{
    char quoted[256];
    if (!text_shell_quote(why, quoted, sizeof quoted))
        return 0;

    char args[512];
    snprintf(args, sizeof args, "commit --quiet -m %s -- CARD.md", quoted);
    return gitcmd_run(tree, "add -- CARD.md") && gitcmd_run(tree, args);
}

static int write_card_file(const char *path, const struct board_card *c)
{
    char file[4300];
    snprintf(file, sizeof file, "%s/CARD.md", path);
    FILE *f = fopen(file, "w");
    if (!f)
        return 0;
    card_write(f, c);
    fclose(f);

    return commit_card_file(path, "card: the card as the board wrote it");
}

struct card_show {
    char *text;
    char *notes;
};

static void card_show_free(void *ud)
{
    struct card_show *k = ud;
    if (!k)
        return;
    free(k->text);
    free(k->notes);
    free(k);
}

static struct card_show *card_show_new(const struct board_card *c)
{
    struct card_show *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;

    const char *body = c->body ? c->body : "";
    if (*body && !same_text(body, c->title))
        k->text = c->worktree[0]
            ? text_dsprintf("%s\n\n%s\n\nworktree %s", c->title, body,
                            c->worktree)
            : text_dsprintf("%s\n\n%s", c->title, body);
    else
        k->text = c->worktree[0]
            ? text_dsprintf("%s\n\nworktree %s", c->title, c->worktree)
            : text_dsprintf("%s", c->title);
    if (!k->text) {
        card_show_free(k);
        return NULL;
    }

    if (c->log_n) {
        char  *buf = NULL;
        size_t len = 0;
        FILE  *f = open_memstream(&buf, &len);
        if (f) {
            for (int i = 0; i < c->log_n; i++)
                fprintf(f, "%s%s: %s", i ? "\n" : "", c->log[i].who,
                        c->log[i].text ? c->log[i].text : "");
            fclose(f);
            k->notes = buf;
        }
    }
    return k;
}

static void card_render(void *ud, int cols)
{
    (void)cols;
    const struct card_show *k = ud;

    ui_bar(ui_style(UI_HEADING), "Card");
    ui_wrapped(k->text, 2, UI_DIM);
    if (k->notes) {
        ui_put("\n");
        ui_bar(ui_style(UI_HEADING), "Notes");
        ui_wrapped(k->notes, 2, UI_DIM);
    }
    ui_put("\n");
    ui_bar(ui_style(UI_HEADING), "Worker");
}

static void draw_card(struct session *s, void *ud)
{
    (void)s;
    viewport_item_begin(&(struct viewport_entry){
        .render = card_render, .ud = ud, .free_ud = card_show_free,
        .reflow = 1, .pad_before = 1, .pad_after = 1});
    card_render(ud, ui_columns());
    viewport_item_end();
}

static void replay(struct session *s, void *ud)
{
    (void)ud;
    sessionload_into(s);
}

static void show_card(int at, const struct board_card *c)
{
    struct card_show *k = card_show_new(c);
    if (k)
        workspace_render(at, draw_card, k);
}

/* The turn that opens a card's session: the action it is on and the card in
   full, notes and all, so there is nothing left to go and read. */
static char *first_turn(const struct board_card *c)
{
    const struct board_action *p = boardflow_action(c);

    char *head = boardstep_fill(p && p->prompt ? p->prompt : "", c);
    if (!head)
        return NULL;

    /* an action in the repo has left the worktree behind, so the turn names the
       branch the work is on and where it came from */
    char tree[8600];
    boardstep_where(c, p, tree, sizeof tree);

    char  *buf = NULL;
    size_t len = 0;
    FILE  *out = open_memstream(&buf, &len);
    if (!out) {
        free(head);
        return NULL;
    }

    fputs(head, out);
    if (tree[0])
        fprintf(out, "\n\n%s", tree);
    if (boardflow_worktree(c))
        fputs("\n\nCARD.md here is the card on the branch. Read it first: it "
              "is tracked, it carries whatever earlier turns wrote back to it, "
              "and its `## Plan` section supersedes the card body below.",
              out);
    fputs("\n\n", out);
    card_write(out, c);
    fclose(out);
    free(head);
    return buf;
}

static const char *wanted_backend(const struct board_card *c)
{
    if (c->backend_pin[0])
        return c->backend_pin;
    const struct board_action *p = boardflow_action(c);
    const char                *b = c->backend[0] ? c->backend
                                                 : (p ? p->backend : "");
    return b[0] ? b : "claude";
}

static void pick_after(struct worker *w);

/* Two cards working the same checkout at once would step on each other, so an
   action that runs in the repo is taken by one card at a time. */
static int same_repo(const char *a, const char *b)
{
    if (!a || !b || !*a || !*b)
        return 0;
    if (!strcmp(a, b))
        return 1;

    char ra[4096], rb[4096];
    return gitcmd_root(a, ra, sizeof ra) && gitcmd_root(b, rb, sizeof rb) &&
           !strcmp(ra, rb);
}

static const char *checkout_held(const struct board_card *c, const char *job)
{
    if (!c || !job || !*job || !boardflow_exclusive(boardcfg_action(job)))
        return NULL;

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        const struct worker *w = &workers[i];
        if (!w->session || !strcmp(w->id, c->id))
            continue;
        if (!boardflow_exclusive(boardcfg_action(w->job)))
            continue;
        if (same_repo(w->repo, c->cwd))
            return w->id;
    }
    return NULL;
}

/* the card is asked for its turn on every tick, so it is told it is waiting
   once rather than on each of them */
static char waiting[WORKSPACE_MAX][BOARD_ID_MAX];

static void wait_note(const char *id, const char *holder)
{
    int at = -1;
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!strcmp(waiting[i], id))
            return;
        if (!waiting[i][0] && at < 0)
            at = i;
    }
    if (at < 0)
        return;

    snprintf(waiting[at], BOARD_ID_MAX, "%s", id);

    char said[96];
    snprintf(said, sizeof said, "waiting on card %s in the checkout", holder);
    board_note(id, "board", said);
}

static void wait_clear(const char *id)
{
    for (int i = 0; i < WORKSPACE_MAX; i++)
        if (!strcmp(waiting[i], id))
            waiting[i][0] = '\0';
}

static int queued_behind(const struct board_card *c, const char *job)
{
    const char *holder = checkout_held(c, job);
    if (!holder)
        return 0;
    wait_note(c->id, holder);
    return 1;
}

static int card_blocked(const struct board_card *c, char *why, int size)
{
    snprintf(why, (size_t)size, "%s", "");
    if (!c)
        return 1;

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
    int lands = boardflow_worktree(c);
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
    worktree_of(root, c->id, path, path_size);
    if (!worktree_make(root, c->id, path, why, size))
        return 0;

    base_of(root, path, c->base, base, base_size);
    unignore_card_file(path);

    struct board_card file = *c;
    snprintf(file.worktree, sizeof file.worktree, "%s", path);
    if (base && base[0])
        snprintf(file.base, sizeof file.base, "%s", base);
    /* the board's own commit is where the card's work starts from, so a turn
       that commits nothing still reads as nothing landed */
    if (write_card_file(path, &file))
        gitcmd_line(path, "rev-parse --short HEAD", base, base_size);
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
    } else if (!switch_d) {
        session_clear(s);
    }

    return session_retarget(s, model, effort, cwd);
}

static void take_slot(struct worker *w, const struct board_card *c,
                      const char *job)
{
    if (strcmp(w->id, c->id))
        w->began = now_seconds();
    snprintf(w->id, sizeof w->id, "%s", c->id);
    snprintf(w->job, sizeof w->job, "%s", job);
    snprintf(w->repo, sizeof w->repo, "%s", c->cwd);
    w->done = 0;
    w->checked = 0;
    w->handover = 0;
    w->charged_usd = 0;
    w->charged_in = 0;
    w->charged_out = 0;
    snprintf(w->backend, sizeof w->backend, "%s", session_backend(w->session));
}

/* The action the card's next turn takes, and the model and effort it runs on:
   what that action asks for on this backend, unless the card pins its own. */
static const struct board_action *aimed_at(const struct board_card *c,
                                           const char *backend,
                                           struct board_action *tiered,
                                           const char **model,
                                           const char **effort)
{
    const struct board_action *mine = boardflow_action(c);
    int have = mine && boardcfg_for_backend(mine->name, backend, c->tier_pin,
                                            tiered);

    *model = c->model[0] ? c->model : (have ? tiered->model : "");
    *effort = c->effort[0] ? c->effort : (have ? tiered->effort : "");
    return mine;
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

    const char                *backend = wanted_backend(c);
    const char                *model, *effort;
    struct board_action        tiered;
    const struct board_action *mine =
        aimed_at(c, backend, &tiered, &model, &effort);
    if (!mine) {
        snprintf(why, (size_t)size, "nothing queued on the card");
        return 0;
    }
    const char *job = mine->name;

    const char *holder = checkout_held(c, job);
    if (holder) {
        wait_note(c->id, holder);
        snprintf(why, (size_t)size, "card %s is in the checkout", holder);
        return 0;
    }

    int  lands = 0;
    char path[4200];
    char base[64];
    if (!card_tree(c, path, sizeof path, base, sizeof base, &lands, why, size))
        return 0;

    /* an action that runs in the repo acts on the checkout, even on a card
       that has a worktree of its own */
    const char *cwd = mine->where == BOARD_IN_REPO ? c->cwd : path;

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
                      effort[0] ? effort : NULL, cwd)) {
            snprintf(why, (size_t)size, "could not start a %s session", backend);
            return 0;
        }
        take_slot(onto, c, job);
    } else {
        int front = workspace_index();
        at = workspace_spawn(backend, model[0] ? model : NULL,
                             effort[0] ? effort : NULL, cwd, NULL);
        if (at < 0) {
            snprintf(why, (size_t)size, "could not start a %s session", backend);
            return 0;
        }
        workspace_show(front);
        s = workspace_at(at);
        hold(c->id, c->cwd, s, job);
    }

    struct board_card edited = *c;
    snprintf(edited.worktree, sizeof edited.worktree, "%s", lands ? path : "");
    snprintf(edited.base, sizeof edited.base, "%s", base);
    snprintf(edited.backend, sizeof edited.backend, "%s", backend);

    show_card(at, &edited);
    if (edited.worktree[0])
        boardlog_worktree(edited.id, edited.worktree);

    char *turn = first_turn(&edited);
    if (turn) {
        workspace_send(at, turn, edited.title);
        boardlog_turn(edited.id, job, turn, NULL);
        free(turn);
    }
    board_update(&edited);

    wait_clear(c->id);
    board_note(c->id, "board", "started");
    return 1;
}

int boardwork_start(const struct board_card *c, char *why, int size)
{
    return start_on(c, NULL, why, size);
}

int boardwork_rejoin(const struct board_card *c, char *why, int size)
{
    snprintf(why, (size_t)size, "%s", "");
    if (!c || !c->session[0]) {
        snprintf(why, (size_t)size, "no worker on that card");
        return -1;
    }
    if (c->closed) {
        snprintf(why, (size_t)size, "card %s is closed", c->id);
        return -1;
    }
    if (boardwork_blocked(c, why, size))
        return -1;

    /* rejoining is picking a conversation back up, so it is allowed on a card
       at rest: there is no action to run, only one to talk to. */
    const char                *backend = c->backend[0] ? c->backend
                                                       : wanted_backend(c);
    const char                *model, *effort;
    struct board_action        tiered;
    const struct board_action *mine =
        aimed_at(c, backend, &tiered, &model, &effort);
    const char                *job = mine && board_stands(c) == BOARD_WORKING
                                         ? mine->name
                                         : "";
    const char                *cwd = boardflow_cwd(c, mine);

    int at = workspace_spawn(backend, model[0] ? model : NULL,
                             effort[0] ? effort : NULL, cwd, c->session);
    if (at < 0) {
        snprintf(why, (size_t)size, "could not start a %s session", backend);
        return -1;
    }

    struct session *s = workspace_at(at);
    if (!session_can_resume(s)) {
        workspace_close(at);
        snprintf(why, (size_t)size, "%s cannot pick a conversation back up",
                 backend);
        return -1;
    }

    if (!hold(c->id, c->cwd, s, job)) {
        workspace_close(at);
        snprintf(why, (size_t)size, "no worker slot left");
        return -1;
    }

    boardwork_keep(c->id);

    board_note(c->id, "board", "worker rejoined the session");
    show_card(at, c);
    workspace_render(at, replay, NULL);
    return at;
}

/* An action asked to commit and committed nothing did not do what it was
   asked, so the card stays on it rather than counting it as run. An action
   that is not asked to commit -- plan writes a file and nothing else -- passes
   on an empty branch. */
/* The action that closed the card is done with the session it ran in, as a
   close from the board is: the worktree the card carried goes with it. */
static void close_after(struct worker *w)
{
    if (!board_close(w->id))
        board_note(w->id, "board", "the store did not take the close");
    boardwork_let_go(w->id);
}

static int nothing_landed(const struct board_card *c,
                          const struct board_action *p)
{
    int files = 0, lines = 0;
    return p && p->commits && p->where == BOARD_IN_WORKTREE && c->worktree[0] &&
           c->base[0] && !boarddiff_size(c, &files, &lines);
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

    /* an interrupt cancels the turn, not the action: the card keeps its queue
       and its session, and the action stands where it was */
    if (session_last_interrupted(s)) {
        if (!board_update(&edited))
            board_note(w->id, "board", "the store did not take the update");
        board_free(cards, n);
        board_note(w->id, "board", "the turn was interrupted");
        return;
    }

    const struct board_action *p = boardflow_action(c);
    const char *ran = p ? p->name : NULL;
    int broke = p && p->fail_marker[0] && reply && strstr(reply, p->fail_marker);
    int empty = !broke && nothing_landed(c, p);

    /* An action that errored, tripped its marker, or committed nothing did not
       pass, so it does not join the history and the rest of the pipeline is
       dropped: the card stands in review and waits to be told what to run
       next, whether or not anything passed before it. */
    int passed = ran && !broke && !empty && (!failed || !*failed);
    int closes = passed && boardflow_closes(c, p);

    int stored = board_update(&edited);
    board_free(cards, n);

    if (passed)
        stored &= board_took(w->id, ran);
    else if (ran)
        stored &= board_stopped(w->id);

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
    else if (closes && stored)
        close_after(w);
    else if ((!failed || !*failed) && stored)
        pick_after(w);

    /* the rest of the queue starts on the turn that finished the action before
       it, rather than waiting for the board view to be opened and tick */
    boardwork_pump();
}

static char pull_failed[BOARD_ID_MAX];

static const struct board_card *pull_next(const struct board_card *cards, int n,
                                         int reuse)
{
    const struct board_card *best = NULL;
    int                      best_p = 0;

    for (int i = 0; i < n; i++) {
        const struct board_card *c = &cards[i];
        /* a card with a queue and no session on it is one to pick up */
        if (board_stands(c) != BOARD_WORKING || slot_of(c->id))
            continue;
        if (!strcmp(c->id, pull_failed))
            continue;

        const struct board_action *head = boardflow_action(c);
        if (queued_behind(c, head ? head->name : NULL))
            continue;

        char why[256];
        if (reuse) {
            if (card_blocked(c, why, sizeof why))
                continue;
        } else if (boardwork_blocked(c, why, sizeof why)) {
            continue;
        }

        if (best && c->priority <= best_p &&
            (c->priority != best_p || c->created >= best->created))
            continue;
        best = c;
        best_p = c->priority;
    }
    return best;
}

static void pick_after(struct worker *w)
{
    if (!w || !boardcfg()->auto_pick)
        return;

    int at = workspace_index_of(w->session);
    if (at < 0 || session_turn_running(w->session) || workspace_queued(at))
        return;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    const struct board_card *held = board_find(cards, n, w->id);
    /* the session stays with the card while the card has anything left to run,
       and a person answers a review by talking to the session that built it */
    enum board_stand stand = board_stands(held);
    if (held && (stand == BOARD_WORKING || stand == BOARD_REVIEW)) {
        board_free(cards, n);
        return;
    }
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

/* The card's own session takes the action, whichever action it is: set it to
 * the model that action asks for and send it that action's prompt. */
static int step_send(const struct board_card *c, const struct board_action *p,
                     struct worker *w)
{
    int at = workspace_index_of(w->session);
    if (at < 0 || session_turn_running(w->session) || workspace_queued(at))
        return 0;
    if (queued_behind(c, p->name))
        return 0;

    char *turn = boardstep_prompt(c, p);
    if (!turn)
        return 0;

    struct board_action tiered;
    int                 levelled = boardcfg_for_backend(
        p->name, session_backend(w->session), c->tier_pin, &tiered);

    /* an action that runs in the repo acts on the checkout, so the session
       walks out of the worktree to take it and back in afterwards. */
    const char *want = boardflow_cwd(c, p);
    if (!session_retarget(w->session, levelled ? tiered.model : NULL,
                          levelled ? tiered.effort : NULL, want)) {
        free(turn);
        return 0;
    }

    snprintf(w->job, sizeof w->job, "%s", p->name);
    w->checked = 0;
    wait_clear(c->id);

    workspace_send(at, turn, c->title);
    boardlog_turn(c->id, p->name, turn, NULL);
    free(turn);
    return 1;
}

static int step_start(const struct board_card *c)
{
    const struct board_action *p = boardflow_action(c);
    if (!p)
        return 0;

    struct worker *w = slot_of(c->id);
    if (!w) {
        char why[256];
        return boardwork_start(c, why, sizeof why);
    }
    if (!strcmp(w->job, p->name))
        return 0;
    return step_send(c, p, w);
}

/* step_start's "the worker is already on this action" guard is what keeps the
   board's tick from sending the same action twice; a trigger has just asked
   for this one, so it sends whatever the card is now on. */
void boardwork_step(const char *id)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    const struct board_action *p = boardflow_action(c);
    struct worker             *w = p ? slot_of(id) : NULL;
    if (p && w) {
        step_send(c, p, w);
    } else if (p) {
        char why[256];
        boardwork_start(c, why, sizeof why);
    }
    board_free(cards, n);
}

int boardwork_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++)
        if (board_stands(&cards[i]) == BOARD_WORKING)
            started = step_start(&cards[i]);
    if (!started)
        started = pull_pump(cards, n);
    board_free(cards, n);
    return started;
}

static void *tidy_thread(void *ud)
{
    char *step = ud;
    (void)system(step);
    free(step);
    return NULL;
}

/* Removing the worktree deletes a checkout, which the poll that asks for it
   would stand still for. The card has already let go of the path, so the git
   commands go on a thread of their own. */
static int drop_worktree(const struct board_card *c)
{
    /* tidy_step is remove --force then branch -D, so an uncommitted CARD.md
       would go with no way back */
    commit_card_file(c->worktree, "card: CARD.md as the worktree goes");

    char root[4096], branch[128], step[TIDY_MAX];
    if (!gitcmd_root(c->cwd, root, sizeof root))
        return 1;

    board_branch(c->id, branch, sizeof branch);
    if (!tidy_step(root, c->worktree, branch, step, sizeof step))
        return 1;

    char *command = strdup(step);
    if (!command)
        return system(step) != -1;

    pthread_t t;
    if (pthread_create(&t, NULL, tidy_thread, command) != 0) {
        int ok = system(command) != -1;
        free(command);
        return ok;
    }
    pthread_detach(t);
    return 1;
}

/* Commits on the card's branch that the checkout's branch does not have. A card
   closed with any of these loses them: the board deletes the branch. */
int boardwork_unmerged(const struct board_card *c)
{
    if (!c || !c->worktree[0])
        return 0;

    char root[4096];
    if (!gitcmd_root(c->cwd, root, sizeof root))
        return 0;

    char branch[128], onto[128];
    board_branch(c->id, branch, sizeof branch);
    if (!gitcmd_line(root, "rev-parse --abbrev-ref HEAD", onto, sizeof onto))
        return 0;

    char args[320], count[32];
    snprintf(args, sizeof args, "rev-list --count %s..%s", onto, branch);
    if (!gitcmd_line(root, args, count, sizeof count))
        return 0;
    return atoi(count);
}

static int release(const struct board_card *c)
{
    if (!c || !c->worktree[0] || boardwork_tab(c->id) >= 0)
        return 0;
    if (!drop_worktree(c))
        return 0;

    struct board_card edited = *c;
    edited.worktree[0] = '\0';
    edited.base[0] = '\0';
    return board_update(&edited);
}

void boardwork_let_go(const char *id)
{
    wait_clear(id);

    struct worker *w = slot_of(id);
    if (!w)
        return;
    int at = workspace_index_of(w->session);
    memset(w, 0, sizeof *w);
    if (at >= 0 && workspace_count() > 1)
        workspace_close_to_base(at);
}

void boardwork_keep(const char *id)
{
    struct worker *w = slot_of(id);
    if (w)
        w->attached = 1;
}

/* The session stays with the card for every action in its queue, so it is held
 * until the queue empties and the card comes to rest. */
static int holds(const struct board_card *c)
{
    return board_stands(c) == BOARD_WORKING;
}

/* The turn ended without boardwork_finished seeing it — the session answered
   while the board was not watching. Count the action the worker is on, so long
   as it is still the one the card is on. */
static int reconcile(struct worker *w, const struct board_card *c)
{
    const struct board_action *p = boardflow_action(c);
    if (!p || strcmp(p->name, w->job))
        return 0;

    int at = workspace_index_of(w->session);
    if (at < 0 || session_busy(w->session) || workspace_queued(at)) {
        w->checked = 0;
        return 0;
    }
    if (w->checked)
        return 0;
    w->checked = 1;

    /* the turn was cancelled rather than answered, so what it left behind is
       not the action running to a finish */
    if (session_last_interrupted(w->session))
        return 0;

    const char *reply = session_last_reply(w->session);
    if (!reply || !*reply)
        return 0;

    /* the same test boardwork_finished applies: an action that tripped its
       marker or landed nothing did not pass, and takes the rest of the
       pipeline down with it */
    if ((p->fail_marker[0] && strstr(reply, p->fail_marker)) ||
        nothing_landed(c, p))
        return board_stopped(c->id);

    return board_took(c->id, p->name);
}

static void switched(struct worker *w)
{
    snprintf(w->backend, sizeof w->backend, "%s", session_backend(w->session));
    w->charged_usd = 0;
    w->charged_in = 0;
    w->charged_out = 0;
    card_backend(w->id, w->backend);
}

static struct session *tree_session(const char *tree)
{
    if (!tree || !*tree)
        return NULL;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        if (s == workspace_base() || slot_by_session(s))
            continue;
        const char *at = session_cwd(s);
        if (at && !strcmp(at, tree))
            return s;
    }
    return NULL;
}

static struct session *id_session(const char *id)
{
    if (!id || !*id)
        return NULL;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        if (s == workspace_base() || slot_by_session(s))
            continue;
        const char *sid = session_id(s);
        if (sid && !strcmp(sid, id))
            return s;
    }
    return NULL;
}

/* the run's start is not kept on the card, so it is read back off the note the
   board wrote when it started the run rather than restarting the clock */
static double run_began(const struct board_card *c)
{
    for (int i = c->log_n - 1; i >= 0; i--)
        if (c->log[i].text && !strcmp(c->log[i].text, "started"))
            return (double)c->log[i].ts;
    return now_seconds();
}

static int tree_gone(const struct board_card *c)
{
    struct stat st;
    return c->worktree[0] &&
           (stat(c->worktree, &st) != 0 || !S_ISDIR(st.st_mode));
}

void boardwork_reattach(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    for (int i = 0; i < n; i++) {
        struct board_card *c = &cards[i];
        if (c->closed || slot_of(c->id) || tree_gone(c))
            continue;

        struct session *s = id_session(c->session);
        if (!s)
            s = tree_session(c->worktree);
        if (!s)
            continue;

        const struct board_action *p =
            board_stands(c) == BOARD_WORKING ? boardflow_action(c) : NULL;
        if (!hold(c->id, c->cwd, s, p ? p->name : ""))
            continue;

        struct worker *w = slot_of(c->id);
        w->began = run_began(c);
        /* the tab was restored with the rest of them, so it is a person's to
           close, not the board's */
        w->attached = 1;
    }

    board_free(cards, n);
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

    struct board_card *cards = NULL;
    int                n = board_load(&cards);

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
                const struct board_card *c = board_find(cards, n, workers[i].id);
                if (follows(&workers[i], c))
                    changed |= handover(&workers[i], c);
                else
                    workers[i].handover = 0;
            }
            continue;
        }

        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && rebind(&workers[i], c)) {
            changed = 1;
            continue;
        }
        /* the session went away mid-queue, so nothing is running any more and
           the card comes back to rest rather than looking busy */
        if (c && board_stands(c) == BOARD_WORKING) {
            board_stopped(workers[i].id);
            board_note(workers[i].id, "board", "worker session ended");
            changed = 1;
        }
        memset(&workers[i], 0, sizeof workers[i]);
    }

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!workers[i].session)
            continue;
        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && strayed(&workers[i], c) && rebind(&workers[i], c))
            changed = 1;

        /* the card learns its session as soon as the backend reports one, so
           a tab that goes away mid-turn can still be picked back up */
        const char *sid = session_id(workers[i].session);
        if (c && sid && strcmp(c->session, sid)) {
            if (board_set_session(workers[i].id, sid))
                changed = 1;
            snprintf(c->session, sizeof c->session, "%s", sid);
        }
        /* a closed card is done with its session, even one a person rejoined:
           there is nothing left for the worker to be on */
        if (c && c->closed) {
            boardwork_let_go(workers[i].id);
            changed = 1;
            continue;
        }
        if (c && holds(c)) {
            changed |= reconcile(&workers[i], c);
            continue;
        }
        /* a tab a person is in is theirs: it is let go when they close it,
           not when the card comes to rest. The board opens over that tab,
           so a tick here would close the session they are in. The action
           it was on is not still running, though, so the slot drops the
           step. */
        if (workers[i].attached ||
            workspace_index_of(workers[i].session) == workspace_index()) {
            if (workers[i].job[0]) {
                workers[i].job[0] = '\0';
                changed = 1;
            }
            continue;
        }
        boardwork_let_go(workers[i].id);
        changed = 1;
    }

    for (int i = 0; i < n; i++) {
        if (!cards[i].worktree[0])
            continue;
        if (!cards[i].closed)
            continue;
        if (!release(&cards[i]))
            continue;
        board_note(cards[i].id, "board", "worktree removed");
        changed = 1;
    }
    board_free(cards, n);
    return changed;
}

static void halt(const char *id)
{
    char key[CHILD_KEY_MAX];
    snprintf(key, sizeof key, "name:%s", id);
    child_stop(key);

    boardwork_let_go(id);
}

void boardwork_discard(const struct board_card *c)
{
    if (!c)
        return;

    halt(c->id);
    if (c->worktree[0])
        drop_worktree(c);
}

int boardwork_stop(const struct board_card *c, const char *why)
{
    if (!c || !why || !*why)
        return 0;

    boardwork_let_go(c->id);
    board_note(c->id, "you", why);
    return board_stopped(c->id);
}

void boardwork_spoke_to(struct session *s)
{
    struct worker *w = slot_by_session(s);
    if (!w)
        return;

    /* a turn the board started is sent to a tab that is not this one */
    if (workspace_index_of(s) == workspace_index())
        w->attached = 1;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, w->id);
    if (c)
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

    board_note(c->id, "you", text);
    workspace_send(at, text, NULL);
    return 1;
}
