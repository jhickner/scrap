#include "boardcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardlog.h"
#include "boardwork.h"
#include "child.h"
#include "gitcmd.h"
#include "text.h"

static void cmd_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, BOARDCMD_KEY "%s", id);
}

int boardcmd_running(const char *id)
{
    char key[CHILD_KEY_MAX];
    cmd_key(id, key, sizeof key);
    return child_running(key);
}

static int base_branch(const char *root, char *out, size_t size)
{
    return gitcmd_line(root, "rev-parse --abbrev-ref HEAD", out, size);
}

int boardcmd_base(const struct board_card *c, char *out, size_t size)
{
    char root[4096];
    if (!c || !gitcmd_root(c->cwd, root, sizeof root))
        return 0;
    return base_branch(root, out, size);
}

static char *expand(const char *script, const struct board_card *c,
                    const char *root, const char *base)
{
    char tree[4200], branch[128];
    boardwork_worktree_of(root, c->id, tree, sizeof tree);
    boardwork_branch_of(c->id, branch, sizeof branch);

    char qroot[4300], qtree[4300];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(c->worktree[0] ? c->worktree : tree, qtree,
                          sizeof qtree))
        return NULL;

    const struct {
        const char *mark;
        const char *is;
    } MARKS[] = {
        {"{id}", c->id},   {"{root}", qroot}, {"{tree}", qtree},
        {"{branch}", branch}, {"{base}", base},
    };

    char  *out = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&out, &len);
    if (!f)
        return NULL;

    for (const char *p = script; *p;) {
        size_t took = 0;
        for (size_t i = 0; i < sizeof MARKS / sizeof *MARKS && !took; i++) {
            size_t n = strlen(MARKS[i].mark);
            if (strncmp(p, MARKS[i].mark, n))
                continue;
            fputs(MARKS[i].is, f);
            took = n;
        }
        if (took) {
            p += took;
            continue;
        }
        fputc(*p++, f);
    }
    fclose(f);
    return out;
}

static int holds_lock(const struct board_card *c, const struct board_role *p,
                      const struct board_card *cards, int n)
{
    if (p->lock == BOARD_LOCK_NONE)
        return 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(cards[i].id, c->id) || !boardcmd_running(cards[i].id))
            continue;
        if (p->lock == BOARD_LOCK_MACHINE)
            return 1;
        if (!strcmp(cards[i].cwd, c->cwd))
            return 1;
    }
    return 0;
}

static int start(struct board_card *c, const struct board_role *p)
{
    char root[4096], base[128] = "";
    if (!gitcmd_root(c->cwd, root, sizeof root)) {
        board_move_back(c->id, boardflow_fail(c), "board", "not in a git repo");
        return 1;
    }
    base_branch(root, base, sizeof base);

    char *script = expand(p->prompt ? p->prompt : "", c, root, base);
    if (!script)
        return 0;

    char key[CHILD_KEY_MAX];
    cmd_key(c->id, key, sizeof key);
    int ok = child_shell(key, script, c->worktree[0] ? c->worktree : c->cwd);
    if (ok) {
        struct board_card edited = *c;
        snprintf(edited.merge_into, sizeof edited.merge_into, "%s", base);
        gitcmd_line(root, "rev-parse HEAD", edited.merge_from,
                    sizeof edited.merge_from);
        edited.merge_to[0] = '\0';
        board_update(&edited);
        boardlog_turn(c->id, p->job, script, NULL);
    }
    free(script);
    return ok;
}

int boardcmd_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    struct board_card       *next = NULL;
    const struct board_role *role = NULL;
    for (int i = 0; i < n; i++) {
        const struct board_role *p = boardflow_role(&cards[i]);
        if (!p || p->runs != BOARD_RUNS_COMMAND)
            continue;
        if (boardcmd_running(cards[i].id) || holds_lock(&cards[i], p, cards, n))
            continue;
        if (!next || cards[i].created < next->created) {
            next = &cards[i];
            role = p;
        }
    }

    int ok = next ? start(next, role) : 0;
    board_free(cards, n);
    return ok;
}

static void tail_of(const char *text, char *out, size_t size)
{
    if (!text || !*text) {
        snprintf(out, size, "it said nothing");
        return;
    }
    size_t      len = strlen(text);
    size_t      want = size > 1 ? size - 1 : 1;
    const char *from = len > want ? text + len - want : text;
    snprintf(out, size, "%s", from);
    text_chomp(out);
}

static const char *last_line(const char *text)
{
    const char *at = text;
    for (const char *p = text; *p; p++)
        if (*p == '\n' && p[1])
            at = p + 1;
    return at;
}

static void passed(const char *id)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    if (!c) {
        board_free(cards, n);
        return;
    }

    if (boardflow_runs(c) != BOARD_RUNS_COMMAND) {
        board_free(cards, n);
        return;
    }

    struct board_card edited = *c;
    char              root[4096];
    if (gitcmd_root(c->cwd, root, sizeof root))
        gitcmd_line(root, "rev-parse HEAD", edited.merge_to,
                    sizeof edited.merge_to);
    if (!strcmp(edited.merge_to, edited.merge_from)) {
        edited.merge_to[0] = '\0';
        edited.merge_from[0] = '\0';
        edited.merge_into[0] = '\0';
    }
    board_update(&edited);

    const char *step = c->step;
    char        said[256] = "";
    if (edited.merge_into[0])
        snprintf(said, sizeof said, "%s into %s", step, edited.merge_into);
    board_note(id, "board", said[0] ? said : step);

    const char *next = boardflow_next(c, c->step, 0);
    board_free(cards, n);
    board_move_to(id, next, "board", NULL);
}

static void failed(const char *id, const char *said)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    if (c && boardflow_runs(c) != BOARD_RUNS_COMMAND)
        c = NULL;

    const struct board_role *p = c ? boardflow_role(c) : NULL;
    const char              *back = c ? boardflow_fail(c) : NULL;
    if (c) {
        struct board_card edited = *c;
        snprintf(edited.stuck, sizeof edited.stuck, "%s", last_line(said));
        edited.merge_to[0] = '\0';
        board_update(&edited);
    }
    board_free(cards, n);

    board_note(id, p ? p->job : "board", said);
    board_move_back(id, back, p ? p->job : "board", NULL);
}

int boardcmd_take(const char *key, const char *out, int ok)
{
    size_t mark = strlen(BOARDCMD_KEY);
    if (!key || strncmp(key, BOARDCMD_KEY, mark))
        return 0;

    const char *id = key + mark;
    char        said[600];
    tail_of(out, said, sizeof said);

    boardlog_turn(id, "step", NULL, out);
    if (ok)
        passed(id);
    else
        failed(id, said);
    return 1;
}
