#include "boardstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "gitcmd.h"

struct fills {
    char branch[160];
    char worktree[4096];
    char repo[4096];
    char onto[128];
    char base[24];
};

static void fills_of(const struct board_card *c, struct fills *f)
{
    memset(f, 0, sizeof *f);
    board_branch(c->id, f->branch, sizeof f->branch);
    snprintf(f->worktree, sizeof f->worktree, "%s", c->worktree);
    snprintf(f->repo, sizeof f->repo, "%s", c->cwd);
    snprintf(f->base, sizeof f->base, "%s", c->base);
    gitcmd_line(c->cwd, "rev-parse --abbrev-ref HEAD", f->onto, sizeof f->onto);
}

static const char *fill_of(const struct fills *f, const char *key, size_t n)
{
    static const char *const names[] = {"branch", "worktree", "repo", "onto",
                                        "base"};
    const char *const values[] = {f->branch, f->worktree, f->repo, f->onto,
                                  f->base};

    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (strlen(names[i]) == n && !strncmp(names[i], key, n))
            return values[i];
    return NULL;
}

char *boardstep_fill(const char *text, const struct board_card *c)
{
    if (!text)
        return NULL;
    if (!c || !strchr(text, '{'))
        return strdup(text);

    struct fills f;
    fills_of(c, &f);

    char  *buf = NULL;
    size_t len = 0;
    FILE  *out = open_memstream(&buf, &len);
    if (!out)
        return strdup(text);

    for (const char *p = text; *p;) {
        const char *end = *p == '{' ? strchr(p + 1, '}') : NULL;
        const char *v = end ? fill_of(&f, p + 1, (size_t)(end - p - 1)) : NULL;
        if (!v) {
            fputc(*p++, out);
            continue;
        }
        fputs(v, out);
        p = end + 1;
    }
    fclose(out);
    return buf;
}

int boardstep_where(const struct board_card *c, const struct board_action *p,
                    char *out, size_t size)
{
    out[0] = '\0';
    if (!c || !p || p->where != BOARD_IN_REPO || !c->worktree[0])
        return 0;

    struct fills f;
    fills_of(c, &f);

    int at = snprintf(out, size,
                      "The work is committed on branch %s, in the worktree at "
                      "%s, off %s. You are in the checkout at %s, on %s.",
                      f.branch, f.worktree, f.base, f.repo, f.onto);
    if (at > 0 && (size_t)at < size)
        return 1;
    out[0] = '\0';
    return 0;
}

/* The session took every step before this one, so it is not told again what
 * was said: it is only told what this step is. */
char *boardstep_prompt(const struct board_card *c, const struct board_action *p)
{
    if (!c || !p)
        return NULL;

    char *head = boardstep_fill(p->prompt ? p->prompt : "", c);
    if (!head)
        return NULL;

    char where[8600];
    boardstep_where(c, p, where, sizeof where);

    const char *body = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(head) + strlen(where) + strlen(body) +
                  strlen(c->title) + 128;
    char *out = malloc(need);
    if (!out) {
        free(head);
        return NULL;
    }

    int at = snprintf(out, need, "%s", head);
    if (where[0])
        at += snprintf(out + at, need - (size_t)at, "\n\n%s", where);
    snprintf(out + at, need - (size_t)at,
             "\n\nThe card this is for:\n\n# %s\n\n%s\n", c->title, body);
    free(head);
    return out;
}
