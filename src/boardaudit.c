#include "boardaudit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "child.h"
#include "gitcmd.h"
#include "sessionfork.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

#define AUDIT_KEY "audit:"

static void audit_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, AUDIT_KEY "%s", id);
}

int boardaudit_running(const char *id)
{
    char key[CHILD_KEY_MAX];
    audit_key(id, key, sizeof key);
    return child_running(key);
}

/* ---- how big the change is ---------------------------------------------- */

// What will land, which is what is committed: work left dirty in the worktree
// is not part of the diff being asked about.
int boardaudit_size(const struct board_card *c, int *files, int *lines)
{
    if (files)
        *files = 0;
    if (lines)
        *lines = 0;
    if (!c || !c->worktree[0] || !c->base[0])
        return 0;

    char args[256];
    snprintf(args, sizeof args, "diff --shortstat %s..HEAD", c->base);

    char out[512];
    if (!gitcmd_line(c->worktree, args, out, sizeof out))
        return 0;

    // " 3 files changed, 47 insertions(+), 6 deletions(-)"
    int changed = 0, added = 0, removed = 0;
    const char *p = out;
    while (*p) {
        while (*p == ' ')
            p++;
        int         value = atoi(p);
        const char *word = p;
        while (*word && *word != ' ')
            word++;
        while (*word == ' ')
            word++;
        if (!strncmp(word, "file", 4))
            changed = value;
        else if (!strncmp(word, "insertion", 9))
            added = value;
        else if (!strncmp(word, "deletion", 8))
            removed = value;
        const char *comma = strchr(p, ',');
        if (!comma)
            break;
        p = comma + 1;
    }

    if (files)
        *files = changed;
    if (lines)
        *lines = added + removed;
    return changed > 0 || added > 0 || removed > 0;
}

int boardaudit_wanted(const struct board_card *c)
{
    const struct board_cfg *cfg = boardcfg();
    // Either threshold at zero turns that half of the test off; both off means
    // nothing is ever audited unless it is asked for by hand.
    if (!cfg->audit_files && !cfg->audit_lines)
        return 0;

    int files = 0, lines = 0;
    if (!boardaudit_size(c, &files, &lines))
        return 0;

    if (cfg->audit_files && files > cfg->audit_files)
        return 1;
    if (cfg->audit_lines && lines > cfg->audit_lines)
        return 1;
    return 0;
}

/* ---- running one -------------------------------------------------------- */

static char *build_prompt(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_AUDIT);
    const char                 *head = p->prompt ? p->prompt : "";

    size_t need = strlen(head) + strlen(c->title) + 256;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%s\n\nThe branch came off %s. The card it was built for:\n\n%s\n",
             head, c->base, c->title);
    return out;
}

int boardaudit_start(const struct board_card *c)
{
    if (!c || !c->worktree[0])
        return 0;

    char key[CHILD_KEY_MAX];
    audit_key(c->id, key, sizeof key);
    if (child_running(key))
        return 0;

    char *prompt = build_prompt(c);
    if (!prompt)
        return 0;

    const struct board_profile *p = boardcfg_for(BOARD_WHO_AUDIT);

    char *argv[16];
    int   n = 0;
    argv[n++] = (char *)sessionfork_program();
    argv[n++] = (char *)"-b";
    argv[n++] = (char *)(p->backend[0] ? p->backend : "claude");
    if (p->model[0] && strcmp(p->model, "default")) {
        argv[n++] = (char *)"-m";
        argv[n++] = (char *)p->model;
    }
    if (p->effort[0] && strcmp(p->effort, "default")) {
        argv[n++] = (char *)"-e";
        argv[n++] = (char *)p->effort;
    }
    argv[n++] = prompt;
    argv[n] = NULL;

    int ok = child_start(key, argv, c->worktree);
    free(prompt);
    return ok;
}

int boardaudit_pump(void)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);

    int started = 0;
    for (int i = 0; i < n && !started; i++) {
        if (cards[i].col != BOARD_AUDIT || boardaudit_running(cards[i].id))
            continue;
        started = boardaudit_start(&cards[i]);
        // A card that cannot be audited must not sit in the column forever.
        if (!started && !cards[i].worktree[0]) {
            board_move(cards[i].id,
                       boardflow_from(cards[i].kind, BOARD_STEP_MERGE, 0),
                       "board", "nothing to audit");
            started = 1;
        }
    }
    board_free(cards, n);
    return started;
}

/* ---- what it found ------------------------------------------------------ */


// The audit is behind it either way; what is left is whatever the kind still
// asks for.
static enum board_col next_after_audit(const char *id)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    enum board_col     col = boardflow_from(c ? c->kind : "", BOARD_STEP_MERGE, 0);
    board_free(cards, n);
    return col;
}

int boardaudit_take(const char *key, const char *reply)
{
    size_t mark = strlen(AUDIT_KEY);
    if (!key || strncmp(key, AUDIT_KEY, mark))
        return 0;

    const char *id = key + mark;
    cJSON      *o = replyjson_parse(reply);
    if (!o) {
        // An audit that did not answer is not an audit that passed, but it is
        // not a reason to hold the card either: it goes on, and says so.
        board_note(id, "audit", "the audit did not answer; going on without it");
        board_move(id, next_after_audit(id), "audit", NULL);
        return 1;
    }

    const cJSON *clean = cJSON_GetObjectItem(o, "clean");
    const cJSON *found = cJSON_GetObjectItem(o, "findings");

    int nfound = 0;
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, found) {
        const char *text = cJSON_GetStringValue((cJSON *)f);
        if (!text || !*text)
            continue;
        board_note(id, "audit", text);
        nfound++;
    }

    int passed = nfound == 0 && (!clean || !cJSON_IsFalse(clean));
    cJSON_Delete(o);

    if (passed) {
        board_note(id, "audit", "nothing worth stopping for");
        board_move(id, next_after_audit(id), "audit", NULL);
    } else {
        board_move(id, BOARD_DOING, "audit", NULL);
    }
    return 1;
}
