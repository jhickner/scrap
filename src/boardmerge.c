#include "boardmerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "child.h"
#include "gitcmd.h"
#include "text.h"

#define MERGE_KEY "merge:"

static void merge_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, MERGE_KEY "%s", id);
}

int boardmerge_running(const char *id)
{
    char key[CHILD_KEY_MAX];
    merge_key(id, key, sizeof key);
    return child_running(key);
}

static int something_landing(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    int                yes = 0;
    for (int i = 0; i < n && !yes; i++)
        yes = boardmerge_running(cards[i].id);
    board_free(cards, n);
    return yes;
}

static int base_branch(const char *root, char *out, size_t size)
{
    return gitcmd_line(root, "rev-parse --abbrev-ref HEAD", out, size);
}

int boardmerge_base(const struct board_card *c, char *out, size_t size)
{
    char root[4096];
    if (!c || !gitcmd_root(c->cwd, root, sizeof root))
        return 0;
    return base_branch(root, out, size);
}

static char *script_for(const struct board_card *c, const char *root,
                        const char *base)
{
    const struct board_cfg *cfg = boardcfg();

    char qroot[4200], qtree[4200];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(c->worktree, qtree, sizeof qtree))
        return NULL;

    char branch[128];
    snprintf(branch, sizeof branch, "worktree-%s", c->id);

    const char *verify = cfg->verify[0] ? cfg->verify : NULL;

    size_t need = 4096 + (verify ? strlen(verify) : 0);
    char  *out = malloc(need);
    if (!out)
        return NULL;

    int at = snprintf(out, need,
        "echo '== rebase onto %s'\n"
        "git fetch . %s:%s >/dev/null 2>&1 || true\n"
        "git rebase %s || { git rebase --abort >/dev/null 2>&1; "
        "echo 'rebase failed'; exit 1; }\n",
        base, base, base, base);

    if (verify)
        at += snprintf(out + at, need - (size_t)at,
            "echo '== check'\n"
            "%s || { echo 'check failed'; exit 1; }\n", verify);

    at += snprintf(out + at, need - (size_t)at,
        "echo '== merge'\n"
        "git -C %s merge --ff-only %s || { echo 'merge failed'; exit 1; }\n"
        "echo '== tidy'\n"
        "git -C %s worktree remove --force %s >/dev/null 2>&1\n"
        "git -C %s branch -d %s >/dev/null 2>&1\n"
        "echo landed\n",
        qroot, branch, qroot, qtree, qroot, branch);

    return out;
}

int boardmerge_pump(void)
{
    if (something_landing())
        return 0;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    struct board_card *next = NULL;
    for (int i = 0; i < n; i++) {
        if (cards[i].col != BOARD_MERGING || !cards[i].worktree[0])
            continue;
        if (!next || cards[i].created < next->created)
            next = &cards[i];
    }
    if (!next) {
        board_free(cards, n);
        return 0;
    }

    char root[4096], base[128];
    if (!gitcmd_root(next->cwd, root, sizeof root) ||
        !base_branch(root, base, sizeof base)) {
        board_move(next->id, BOARD_DOING, "board",
                   "no base branch");
        board_free(cards, n);
        return 1;
    }

    char *script = script_for(next, root, base);
    if (!script) {
        board_free(cards, n);
        return 0;
    }

    char key[CHILD_KEY_MAX];
    merge_key(next->id, key, sizeof key);
    int ok = child_shell(key, script, next->worktree);
    if (ok)
        boardlog_turn(next->id, "merge", script, NULL);
    free(script);
    board_free(cards, n);
    return ok;
}

static void tail_of(const char *text, char *out, size_t size)
{
    if (!text || !*text) {
        snprintf(out, size, "it said nothing");
        return;
    }
    size_t len = strlen(text);
    size_t want = size > 1 ? size - 1 : 1;
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

int boardmerge_take(const char *key, const char *out, int ok)
{
    {
        size_t mark = strlen(MERGE_KEY);
        if (key && !strncmp(key, MERGE_KEY, mark))
            boardlog_turn(key + mark, "merge", NULL, out);
    }
    size_t mark = strlen(MERGE_KEY);
    if (!key || strncmp(key, MERGE_KEY, mark))
        return 0;

    const char *id = key + mark;
    char        said[600];
    tail_of(out, said, sizeof said);

    if (ok) {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, id);
        if (c) {
            struct board_card edited = *c;
            edited.col = BOARD_DONE;
            edited.worktree[0] = '\0';
            board_update(&edited);
        }
        board_free(cards, n);
        board_note(id, "board", "landed");
    } else {
        struct board_card *cards = NULL;
        int                n = board_load(&cards);
        struct board_card *c = board_find(cards, n, id);
        if (c) {
            struct board_card edited = *c;
            edited.col = BOARD_DOING;

            snprintf(edited.stuck, sizeof edited.stuck, "%s", last_line(said));
            board_update(&edited);
        }
        board_free(cards, n);

        board_note(id, "board", last_line(said));
    }
    return 1;
}
