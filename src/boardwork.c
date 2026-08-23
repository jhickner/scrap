#include "boardwork.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "gitcmd.h"
#include "md.h"
#include "boardcfg.h"
#include "boardaudit.h"
#include "child.h"
#include "boardflow.h"
#include "boardlog.h"
#include "boardmerge.h"
#include "boardsweep.h"
#include "session.h"
#include "text.h"
#include "workspace.h"

struct worker {
    char             id[BOARD_ID_MAX];
    struct session  *session;
    enum board_role  role;
    int              done;
    int              checked;
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

int boardwork_auditing(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w && w->role == BOARD_ROLE_AUDIT;
}

int boardwork_sweeping(const char *id)
{
    struct worker *w = id ? slot_of(id) : NULL;
    return w && w->role == BOARD_ROLE_SWEEP;
}

static int side_start(const struct board_card *c, enum board_who who,
                      enum board_role role, const char *cwd, char *prompt,
                      const char *label)
{
    if (!prompt)
        return 0;

    const struct board_profile *p = boardcfg_for(who);

    int front = workspace_index();
    int at = workspace_spawn(p->backend[0] ? p->backend : "claude",
                             p->model[0] ? p->model : NULL,
                             p->effort[0] ? p->effort : NULL, cwd, NULL);
    if (at < 0) {
        free(prompt);
        return 0;
    }
    workspace_show(front);

    if (!boardwork_hold(c->id, workspace_at(at), role)) {
        workspace_close(at);
        free(prompt);
        return 0;
    }

    workspace_send(at, prompt, c->title);
    boardlog_turn(c->id, label, prompt, NULL);
    free(prompt);
    return 1;
}

static int audit_start(const struct board_card *c)
{
    if (!c || !c->worktree[0] || boardwork_auditing(c->id))
        return 0;
    return side_start(c, BOARD_WHO_AUDIT, BOARD_ROLE_AUDIT, c->worktree,
                      boardaudit_prompt(c), "audit");
}

int boardwork_audit_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++) {
        if (cards[i].col != BOARD_AUDIT || boardwork_auditing(cards[i].id))
            continue;
        started = audit_start(&cards[i]);

        if (!started && !cards[i].worktree[0]) {
            board_move(cards[i].id,
                       boardflow_from(cards[i].kind, BOARD_STEP_MERGE, 0),
                       "board", "no diff to audit");
            started = 1;
        }
    }
    board_free(cards, n);
    return started;
}

static int sweeping_any(void)
{
    for (int i = 0; i < WORKSPACE_MAX; i++)
        if (workers[i].session && workers[i].role == BOARD_ROLE_SWEEP)
            return 1;
    return 0;
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

    char id[BOARD_ID_MAX];
    if (!boardsweep_open(cwd, id, sizeof id)) {
        free(prompt);
        return 0;
    }

    cards = NULL;
    n = board_load(&cards);
    const struct board_card *c = board_find(cards, n, id);
    int started = 0;
    if (c)
        started = side_start(c, BOARD_WHO_SWEEP, BOARD_ROLE_SWEEP, c->cwd,
                             prompt, "sweep");
    else
        free(prompt);
    board_free(cards, n);

    if (!started)
        board_remove(id);
    return started;
}

int boardwork_hold(const char *id, struct session *s, enum board_role role)
{
    if (!id || !*id || !s || slot_of(id))
        return 0;

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (workers[i].session)
            continue;
        memset(&workers[i], 0, sizeof workers[i]);
        snprintf(workers[i].id, sizeof workers[i].id, "%s", id);
        workers[i].session = s;
        workers[i].role = role;
        return 1;
    }
    return 0;
}

static void worktree_of(const char *root, const char *id, char *out, size_t size)
{
    snprintf(out, size, "%s/.claude/worktrees/%s", root, id);
}

static void branch_of(const char *id, char *out, size_t size)
{
    snprintf(out, size, "worktree-%s", id);
}

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

    snprintf(cmd, sizeof cmd,
             "git -C %s worktree add %s %s >/dev/null 2>&1", qroot, qpath, branch);
    if (system(cmd) == 0)
        return 1;

    snprintf(why, (size_t)size, "could not make a worktree at %s", path);
    return 0;
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
        fprintf(f, "\n## What has been said about it\n\n");
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

static char *landing_turn(const struct board_card *c)
{
    const char *standing = boardcfg_for(BOARD_WHO_WORKER)->prompt;
    const char *head = boardcfg_for(BOARD_WHO_MERGE)->prompt;
    const char *body = c->body && *c->body ? c->body : c->title;

    if (!standing)
        standing = "";
    if (!head)
        head = "";

    char onto[128] = "";
    if (!boardmerge_base(c, onto, sizeof onto) || !onto[0])
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

    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const struct board_kind    *k = boardcfg_kind(c->kind);
    int                         lands = boardcfg_kind_takes(c->kind, BOARD_STEP_WORKTREE);

    const char *head = lands && p->prompt ? p->prompt : "";
    const char *mine = k && k->prompt ? k->prompt : "";
    const char *body = c->body && *c->body ? c->body : c->title;
    const char *card = lands
        ? "The card is also written to CARD.md here, which is the copy to go "
          "back to rather than this message."
        : "";

    size_t need = strlen(head) + strlen(mine) + strlen(body) +
                  strlen(c->title) + strlen(card) + 256;
    char  *out = malloc(need);
    if (!out)
        return NULL;

    int at = snprintf(out, need, "%s", head);
    if (*mine)
        at += snprintf(out + at, need - (size_t)at, "%s%s", *head ? "\n\n" : "", mine);
    if (*card)
        at += snprintf(out + at, need - (size_t)at, "\n\n%s", card);
    snprintf(out + at, need - (size_t)at, "\n\n# %s\n\n%s\n", c->title, body);
    return out;
}

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

    if (boardsweep_is(c)) {
        snprintf(why, (size_t)size, "a sweep starts itself");
        return 1;
    }

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

    return 0;
}

int boardwork_start(const struct board_card *c, char *why, int size)
{
    if (boardwork_blocked(c, why, size))
        return 0;

    int  lands = boardcfg_kind_takes(c->kind, BOARD_STEP_WORKTREE);
    char path[4200];
    char base[64] = {0};

    if (lands) {
        char root[4096];
        if (!gitcmd_root(c->cwd, root, sizeof root)) {
            snprintf(why, (size_t)size, "%s is not in a git repo", c->cwd);
            return 0;
        }
        worktree_of(root, c->id, path, sizeof path);
        if (!worktree_make(root, c->id, path, why, size))
            return 0;

        gitcmd_line(path, "rev-parse --short HEAD", base, sizeof base);

        ignore_card_file(path);
        write_card_file(path, c);
    } else {
        snprintf(path, sizeof path, "%s", c->cwd);
    }

    const struct board_profile *p = boardcfg_for(BOARD_WHO_WORKER);
    const char *wanted = wanted_backend(c);
    const char *model = c->model[0] ? c->model : p->model;
    const char *effort = c->effort[0] ? c->effort : p->effort;

    const char *backend = wanted;

    int front = workspace_index();

    int at = workspace_spawn(backend,
                             model[0] ? model : NULL,
                             effort[0] ? effort : NULL, path, NULL);
    if (at < 0) {
        snprintf(why, (size_t)size, "could not start a %s session", backend);
        return 0;
    }
    workspace_show(front);

    show_card(at, c);

    struct session *s = workspace_at(at);
    char           *turn = first_turn(c);
    if (turn) {
        workspace_send(at, turn, c->title);
        boardlog_turn(c->id, c->stuck[0] ? "merge worker" : "worker", turn, NULL);
        free(turn);
    }

    boardwork_hold(c->id, s, BOARD_ROLE_WORKER);

    struct board_card edited = *c;
    edited.col = BOARD_DOING;

    edited.stuck[0] = '\0';
    snprintf(edited.worktree, sizeof edited.worktree, "%s", lands ? path : "");
    snprintf(edited.base, sizeof edited.base, "%s", base);
    snprintf(edited.backend, sizeof edited.backend, "%s", backend);
    board_update(&edited);

    board_note(c->id, "board", "started");
    return 1;
}

static enum board_col landed(const struct board_card *c, int *empty_out)
{
    int files = 0, lines = 0;
    int empty = boardcfg_kind_takes(c->kind, BOARD_STEP_WORKTREE) &&
                c->worktree[0] && c->base[0] &&
                !boardaudit_size(c, &files, &lines);
    if (empty_out)
        *empty_out = empty;
    if (empty)
        return BOARD_DOING;
    return boardflow_from(c->kind, BOARD_STEP_REVIEW, boardaudit_wanted(c));
}

void boardwork_finished(struct session *s)
{
    struct worker *w = slot_by_session(s);
    if (!w)
        return;

    const char *sid = session_id(s);
    const char *reply = session_last_reply(s);
    const char *failed = session_last_error(s);

    if (w->role == BOARD_ROLE_AUDIT) {
        w->done = 1;
        if (failed && *failed)
            board_note(w->id, "audit", failed);
        boardaudit_finished(w->id, failed && *failed ? NULL : reply);
        return;
    }

    if (w->role == BOARD_ROLE_SWEEP) {
        if (failed && *failed)
            board_note(w->id, "sweep", failed);
        boardsweep_finished(w->id, failed && *failed ? NULL : reply);
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

    edited.cost_usd = session_cost(s);

    boardlog_turn(w->id, "worker", NULL,
                  failed && *failed ? failed : reply);

    int            empty = 0;
    enum board_col next = landed(c, &empty);

    if ((!failed || !*failed) && !empty)
        edited.col = next;
    int stored = board_update(&edited);
    board_free(cards, n);

    if (!stored)
        board_note(w->id, "board", "the store did not take the update");

    if (failed && *failed) {
        size_t need = strlen(failed) + 32;
        char  *said = malloc(need);
        if (said) {
            snprintf(said, need, "the turn failed: %s", failed);
            board_note(w->id, "worker", said);
            free(said);
        } else {
            board_note(w->id, "worker", failed);
        }
    } else {
        board_note(w->id, "worker",
                   reply && *reply ? reply : "finished without saying anything");
    }
    if (empty)
        board_note(w->id, "board", "no commit on the branch");
}

static char pull_failed[BOARD_ID_MAX];

static int pull_priority(const struct board_card *c)
{
    return c->priority ? c->priority : boardcfg_priority(c->kind);
}

static const struct board_card *pull_next(const struct board_card *cards, int n)
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
        if (boardwork_blocked(c, why, sizeof why))
            continue;

        int p = pull_priority(c);
        if (best && p <= best_p && (p != best_p || c->created >= best->created))
            continue;
        best = c;
        best_p = p;
    }
    return best;
}

static int pull_pump(const struct board_card *cards, int n)
{
    const struct board_cfg *cfg = boardcfg();
    if (!cfg->auto_pull || boardwork_running() >= cfg->workers)
        return 0;

    const struct board_card *c = pull_next(cards, n);
    if (!c)
        return 0;

    char why[256];
    if (boardwork_start(c, why, sizeof why))
        return 1;

    snprintf(pull_failed, sizeof pull_failed, "%s", c->id);
    board_note(c->id, "board", why[0] ? why : "could not start it");
    return 1;
}

int boardwork_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++) {
        if (cards[i].col != BOARD_DOING || !cards[i].stuck[0])
            continue;
        if (boardwork_tab(cards[i].id) >= 0)
            continue;

        char why[256];

        started = boardwork_start(&cards[i], why, sizeof why);
    }
    if (!started)
        started = pull_pump(cards, n);
    board_free(cards, n);
    return started;
}

static int drop_worktree(const struct board_card *c)
{
    char root[4096], branch[128];
    branch_of(c->id, branch, sizeof branch);

    if (!gitcmd_root(c->cwd, root, sizeof root))
        return 1;

    char qroot[4200], qtree[4200];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(c->worktree, qtree, sizeof qtree))
        return 1;

    char cmd[9000];
    snprintf(cmd, sizeof cmd,
             "git -C %s worktree remove --force %s >/dev/null 2>&1; "
             "git -C %s branch -D %s >/dev/null 2>&1",
             qroot, qtree, qroot, branch);
    return system(cmd) != -1;
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

static int at_role(enum board_role role, enum board_col col)
{
    switch (role) {
    case BOARD_ROLE_AUDIT:
        return col == BOARD_AUDIT;
    default:
        return col == BOARD_DOING || col == BOARD_REVIEW;
    }
}

static int reconcile(struct worker *w, const struct board_card *c)
{
    if (w->role != BOARD_ROLE_WORKER || c->col != BOARD_DOING)
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

    int            empty = 0;
    enum board_col next = landed(c, &empty);
    if (empty)
        return 0;

    return board_move(c->id, next, "board", "worker turn is over");
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
        if (workspace_index_of(workers[i].session) >= 0)
            continue;

        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, workers[i].id);
        if (c && c->col == BOARD_DOING) {
            board_move(workers[i].id, BOARD_BACKLOG, "board",
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
        if (c && at_role(workers[i].role, c->col)) {
            changed |= reconcile(&workers[i], c);
            continue;
        }
        boardwork_let_go(workers[i].id);
        changed = 1;
    }

    for (int i = 0; i < n; i++) {
        if (!cards[i].worktree[0])
            continue;
        if (cards[i].col != BOARD_BACKLOG && cards[i].col != BOARD_DONE)
            continue;
        if (!boardwork_release(&cards[i]))
            continue;
        board_note(cards[i].id, "board", "worktree removed");
        changed = 1;
    }
    board_free(cards, n);
    return changed;
}

void boardwork_discard(const struct board_card *c)
{
    if (!c)
        return;

    static const char *const STAGES[] = {"triage:", "merge:"};
    for (size_t i = 0; i < sizeof STAGES / sizeof *STAGES; i++) {
        char key[CHILD_KEY_MAX];
        snprintf(key, sizeof key, "%s%s", STAGES[i], c->id);
        child_stop(key);
    }

    boardwork_let_go(c->id);
    if (c->worktree[0])
        drop_worktree(c);
}

int boardwork_approve(const struct board_card *c, int audit)
{
    if (!c)
        return 0;
    boardwork_let_go(c->id);
    board_note(c->id, "you", audit ? "approved, for audit" : "approved");

    if (!c->worktree[0])
        return board_move(c->id, BOARD_DONE, "you", NULL);
    return board_move(c->id, boardflow_from(c->kind, BOARD_STEP_AUDIT, audit),
                      "you", NULL);
}

int boardwork_reject(const struct board_card *c, const char *why)
{
    if (!c)
        return 0;
    boardwork_let_go(c->id);
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
    pull_failed[0] = '\0';
}

void boardwork_close_all(void)
{
    memset(workers, 0, sizeof workers);
}
