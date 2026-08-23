#include "boardwork.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
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

// One line of a command's output, trimmed. Empty when it said nothing.
static int git_line(const char *dir, const char *args, char *out, size_t size)
{
    char quoted[4200];
    if (!text_shell_quote(dir, quoted, sizeof quoted))
        return 0;

    char cmd[8192];
    snprintf(cmd, sizeof cmd, "git -C %s %s 2>/dev/null", quoted, args);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    out[0] = '\0';
    if (!fgets(out, (int)size, f)) {
        pclose(f);
        return 0;
    }
    pclose(f);
    text_chomp(out);
    return out[0] != '\0';
}

// The repo proper, not whichever worktree of it we happen to be standing in.
// --show-toplevel answers with the worktree, so a board opened from inside one
// would put its workers underneath it, on branches cut from it: nested trees
// that go when that worktree is merged away, on bases the merge queue has no
// way to rebase. The first line of `worktree list` is always the main one.
static int repo_root(const char *cwd, char *out, size_t size)
{
    char line[4200];
    if (git_line(cwd, "worktree list --porcelain", line, sizeof line) &&
        !strncmp(line, "worktree ", 9) && line[9]) {
        snprintf(out, size, "%s", line + 9);
        return 1;
    }
    return git_line(cwd, "rev-parse --show-toplevel", out, size);
}

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
    if (!git_line(path, "rev-parse --path-format=absolute --git-common-dir",
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

/* ---- starting and finishing --------------------------------------------- */

int boardwork_start(const struct board_card *c, char *why, int size)
{
    snprintf(why, (size_t)size, "%s", "");

    if (!c || !c->cwd[0]) {
        snprintf(why, (size_t)size, "the card does not say which repo it is in");
        return 0;
    }
    if (slot_of(c->id)) {
        snprintf(why, (size_t)size, "a worker already has that card");
        return 0;
    }

    const struct board_cfg *cfg = boardcfg();
    if (boardwork_running() >= cfg->workers) {
        snprintf(why, (size_t)size, "all %d workers are busy", cfg->workers);
        return 0;
    }
    if (workspace_count() >= WORKSPACE_MAX) {
        snprintf(why, (size_t)size, "this window is full of tabs");
        return 0;
    }

    char root[4096];
    if (!repo_root(c->cwd, root, sizeof root)) {
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
    git_line(path, "rev-parse --short HEAD", base, sizeof base);

    ignore_card_file(path);
    write_card_file(path, c);

    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const char *backend = c->backend[0] ? c->backend : p->backend;
    const char *model = c->model[0] ? c->model : p->model;
    const char *effort = c->effort[0] ? c->effort : p->effort;

    // Opening a tab brings it to the front, and the window is not the
    // worker's -- it is parked in the board. Put back what was showing.
    int front = workspace_index();

    int at = workspace_spawn(backend[0] ? backend : "claude",
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
    board_update(&edited);
    board_note(c->id, "board", "a worker took it");
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
    edited.cost_usd = c->cost_usd;

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
    return board_move(c->id, BOARD_DONE, "you", NULL);
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
