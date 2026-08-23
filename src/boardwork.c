#include "boardwork.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "gitcmd.h"
#include "quota.h"
#include "boardcfg.h"
#include "session.h"
#include "text.h"
#include "workspace.h"

struct worker {
    char            id[BOARD_ID_MAX];
    struct session *session;
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

/* ---- git ---------------------------------------------------------------- */

// The worktree of a card, and the branch that goes with it. The convention is
// the one the repo already uses by hand.
static void worktree_of(const char *root, const char *id, char *out, size_t size)
{
    snprintf(out, size, "%s/.claude/worktrees/%s", root, id);
}

static void branch_of(const char *id, char *out, size_t size)
{
    snprintf(out, size, "worktree-%s", id);
}

// Adds the worktree, or adopts one already there: a card whose worker was
// reaped and started again should land back where its work is.
static int worktree_make(const char *root, const char *id, const char *path,
                         char *why, int size)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;

    char branch[128];
    branch_of(id, branch, sizeof branch);

    char qroot[4200], qpath[4200];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(path, qpath, sizeof qpath)) {
        snprintf(why, (size_t)size, "the path does not fit");
        return 0;
    }

    char cmd[9000];
    snprintf(cmd, sizeof cmd,
             "git -C %s worktree add %s -b %s >/dev/null 2>&1", qroot, qpath, branch);
    if (system(cmd) == 0)
        return 1;

    // A branch of that name from a previous run: take it rather than refuse.
    snprintf(cmd, sizeof cmd,
             "git -C %s worktree add %s %s >/dev/null 2>&1", qroot, qpath, branch);
    if (system(cmd) == 0)
        return 1;

    snprintf(why, (size_t)size, "could not make a worktree at %s", path);
    return 0;
}

/* ---- what the worker is handed ------------------------------------------ */

// CARD.md belongs to the worker, not to the repo. Left to itself it turns up
// untracked in every status, and in every diff the work is reviewed by.
//
// The exclude has to go in the common directory: git reads info/exclude from
// there and not from the per-worktree git dir, so writing the obvious place
// does nothing. That makes it a repo-wide rule, which is why it is written
// once and never repeated -- and info/exclude is not tracked, so it stays a
// local matter.
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

// The spec, written where it survives what a long session does to its own
// first turn: compaction, a restart, a context that rolled over.
static void write_card_file(const char *path, const struct board_card *c)
{
    char file[4300];
    snprintf(file, sizeof file, "%s/CARD.md", path);
    FILE *f = fopen(file, "w");
    if (!f)
        return;
    fprintf(f, "# %s\n\n%s\n", c->title, c->body ? c->body : "");
    if (c->kind[0])
        fprintf(f, "\nkind: %s\n", c->kind);
    if (c->log_n) {
        fprintf(f, "\n## What has been said about it\n\n");
        for (int i = 0; i < c->log_n; i++)
            fprintf(f, "- %s: %s\n", c->log[i].who,
                    c->log[i].text ? c->log[i].text : "");
    }
    fclose(f);
}

static char *first_turn(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const char                 *head = p->prompt ? p->prompt : "";
    const char                 *body = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(head) + strlen(body) + strlen(c->title) + 256;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%s\n\nThe card is also written to CARD.md here, which is the copy "
             "to go back to rather than this message.\n\n# %s\n\n%s\n",
             head, c->title, body);
    return out;
}

/* ---- who can take it ---------------------------------------------------- */

// A backend has room when it says how much is left and that is under the
// ceiling. One that reports nothing is not one that is spent: only some
// backends have a protocol for this at all, and those are the escape hatch
// when everything measured is exhausted.
static int has_room(const char *backend, int ceiling)
{
    int percent = 0;
    if (!quota_get(backend, &percent, NULL))
        return 1;
    return percent < ceiling;
}

// The first backend in the chain with room, starting from the one the card
// would otherwise use. Returns NULL when every one of them is spent.
static const char *with_room(const char *first, const char *chain, int ceiling,
                             char *out, size_t size)
{
    if (has_room(first, ceiling)) {
        snprintf(out, size, "%s", first);
        return out;
    }

    char rest[256];
    snprintf(rest, sizeof rest, "%s", chain ? chain : "");
    for (char *save = rest, *name; (name = strsep(&save, ","));) {
        while (*name == ' ')
            name++;
        if (!*name || !strcmp(name, first))
            continue;
        if (has_room(name, ceiling)) {
            snprintf(out, size, "%s", name);
            return out;
        }
    }
    return NULL;
}

/* ---- starting and finishing --------------------------------------------- */

// Which backend a card would run on, before anything is asked of it.
static const char *wanted_backend(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const char                 *b = c->backend[0] ? c->backend : p->backend;
    return b[0] ? b : "claude";
}

int boardwork_blocked(const struct board_card *c, char *why, int size)
{
    snprintf(why, (size_t)size, "%s", "");
    if (!c)
        return 1;

    if (!c->cwd[0]) {
        snprintf(why, (size_t)size, "no repo on the card");
        return 1;
    }
    if (slot_of(c->id)) {
        snprintf(why, (size_t)size, "a worker already has it");
        return 1;
    }

    const struct board_cfg *cfg = boardcfg();
    if (boardwork_running() >= cfg->workers) {
        snprintf(why, (size_t)size, "all %d workers busy", cfg->workers);
        return 1;
    }
    if (workspace_count() >= WORKSPACE_MAX) {
        snprintf(why, (size_t)size, "no room for another tab");
        return 1;
    }

    const char *wanted = wanted_backend(c);
    if (has_room(wanted, cfg->usage_ceiling))
        return 0;

    // Over the ceiling: either the window turns over soon enough to be worth
    // waiting for, or somebody else in the chain takes it.
    int soon = quota_resets_in(wanted);
    if (soon >= 0 && soon <= cfg->reset_hold) {
        snprintf(why, (size_t)size, "%s resets in %d min", wanted, soon);
        return 1;
    }

    char took[32];
    if (!with_room(wanted, cfg->delegation, cfg->usage_ceiling, took, sizeof took)) {
        snprintf(why, (size_t)size, "every backend over %d%%", cfg->usage_ceiling);
        return 1;
    }
    return 0;
}

int boardwork_start(const struct board_card *c, char *why, int size)
{
    if (boardwork_blocked(c, why, size))
        return 0;

    const struct board_cfg *cfg = boardcfg();

    char root[4096];
    if (!gitcmd_root(c->cwd, root, sizeof root)) {
        snprintf(why, (size_t)size, "%s is not in a git repo", c->cwd);
        return 0;
    }

    char path[4200];
    worktree_of(root, c->id, path, sizeof path);
    if (!worktree_make(root, c->id, path, why, size))
        return 0;

    // Asked of the worktree rather than of the repo: what the work is actually
    // sitting on is the thing the diff and the rebase are against.
    char base[64] = {0};
    gitcmd_line(path, "rev-parse --short HEAD", base, sizeof base);

    ignore_card_file(path);
    write_card_file(path, c);

    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const char *wanted = wanted_backend(c);
    const char *model = c->model[0] ? c->model : p->model;
    const char *effort = c->effort[0] ? c->effort : p->effort;

    char        took[32];
    const char *backend = with_room(wanted, cfg->delegation, cfg->usage_ceiling,
                                    took, sizeof took);
    if (!backend)
        backend = wanted;
    // A card that ran somewhere other than where it was meant to should say so
    // later, when the question is why it came out the way it did.
    int handed_on = strcmp(backend, wanted) != 0;

    // Opening a tab brings it to the front, and the window is not the
    // worker's -- it is parked in the board. Put back what was showing.
    int front = workspace_index();

    int at = workspace_spawn(backend,
                             model[0] ? model : NULL,
                             effort[0] ? effort : NULL, path, NULL);
    if (at < 0) {
        snprintf(why, (size_t)size, "could not start a %s session", backend);
        return 0;
    }
    workspace_show(front);

    struct session *s = workspace_at(at);
    char           *turn = first_turn(c);
    if (turn) {
        workspace_send(at, turn, c->title);
        free(turn);
    }

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (workers[i].session)
            continue;
        snprintf(workers[i].id, sizeof workers[i].id, "%s", c->id);
        workers[i].session = s;
        break;
    }

    struct board_card edited = *c;
    edited.col = BOARD_DOING;
    snprintf(edited.worktree, sizeof edited.worktree, "%s", path);
    snprintf(edited.base, sizeof edited.base, "%s", base);
    snprintf(edited.backend, sizeof edited.backend, "%s", backend);
    board_update(&edited);

    if (handed_on) {
        int spent = 0;
        quota_get(wanted, &spent, NULL);
        char said[256];
        snprintf(said, sizeof said, "%s was at %d%%, so %s took it", wanted,
                 spent, backend);
        board_note(c->id, "board", said);
    } else {
        board_note(c->id, "board", "a worker took it");
    }
    return 1;
}

void boardwork_finished(struct session *s)
{
    struct worker *w = slot_by_session(s);
    if (!w)
        return;

    // The id only exists once the backend has answered at least once, and it
    // is what reopens the conversation later.
    const char *sid = session_id(s);
    const char *reply = session_last_reply(s);
    const char *failed = session_last_error(s);

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
    // A worker session is one card, so what the session has cost is what the
    // card has cost.
    edited.cost_usd = session_cost(s);

    // An errored turn is not a finished one: the card stays where it is,
    // marked, because what is needed to fix it is the tab and not a field.
    if (!failed || !*failed)
        edited.col = BOARD_REVIEW;
    board_update(&edited);
    board_free(cards, n);

    char said[1024];
    if (failed && *failed)
        snprintf(said, sizeof said, "the turn failed: %s", failed);
    else
        snprintf(said, sizeof said, "%s",
                 reply && *reply ? reply : "finished without saying anything");
    board_note(w->id, "worker", said);
}

// A worker whose tab has gone -- closed by hand, or lost with a restart --
// leaves a card in `doing` that nothing is working on.
int boardwork_poll(void)
{
    int changed = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!workers[i].session)
            continue;
        if (workspace_index_of(workers[i].session) >= 0)
            continue;

        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && c->col == BOARD_DOING) {
            board_move(workers[i].id, BOARD_BACKLOG, "board",
                       "the worker went away before it finished");
            changed = 1;
        }
        board_free(cards, n);
        memset(&workers[i], 0, sizeof workers[i]);
    }
    return changed;
}

/* ---- what happens to the work ------------------------------------------- */

static void let_go(const char *id)
{
    struct worker *w = slot_of(id);
    if (!w)
        return;
    int at = workspace_index_of(w->session);
    memset(w, 0, sizeof *w);
    if (at >= 0 && workspace_count() > 1)
        workspace_close(at);
}

int boardwork_approve(const struct board_card *c)
{
    if (!c)
        return 0;
    let_go(c->id);
    board_note(c->id, "you", "approved");

    // Approving is not finishing: the work still has to land, and a card with
    // no worktree has nothing to land.
    return board_move(c->id, c->worktree[0] ? BOARD_MERGING : BOARD_DONE,
                      "you", NULL);
}

int boardwork_reject(const struct board_card *c, const char *why)
{
    if (!c)
        return 0;
    let_go(c->id);
    return board_move(c->id, BOARD_BACKLOG, "you",
                      why && *why ? why : "rejected");
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
    return board_move(c->id, BOARD_DOING, "you", NULL);
}

void boardwork_begin(void)
{
    memset(workers, 0, sizeof workers);
}

void boardwork_close_all(void)
{
    memset(workers, 0, sizeof workers);
}
