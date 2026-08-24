#include "boardaudit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "boardflow.h"
#include "gitcmd.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

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

#define SIZE_CACHE_MAX 32
#define SIZE_CACHE_TTL 5

struct size_entry {
    char   id[BOARD_ID_MAX];
    char   base[24];
    time_t taken;
    int    files, lines, any;
};

static struct size_entry size_cache[SIZE_CACHE_MAX];
static int               size_cache_n;

int boardaudit_size_cached(const struct board_card *c, int *files, int *lines)
{
    if (files)
        *files = 0;
    if (lines)
        *lines = 0;
    if (!c || !c->worktree[0] || !c->base[0])
        return 0;

    time_t             now = time(NULL);
    struct size_entry *e = NULL;
    for (int i = 0; i < size_cache_n; i++)
        if (!strcmp(size_cache[i].id, c->id)) {
            e = &size_cache[i];
            break;
        }

    if (e && !strcmp(e->base, c->base) && now - e->taken < SIZE_CACHE_TTL) {
        if (files)
            *files = e->files;
        if (lines)
            *lines = e->lines;
        return e->any;
    }

    int got_files = 0, got_lines = 0;
    int any = boardaudit_size(c, &got_files, &got_lines);

    if (!e) {
        if (size_cache_n < SIZE_CACHE_MAX)
            e = &size_cache[size_cache_n++];
        else {
            e = &size_cache[0];
            for (int i = 1; i < size_cache_n; i++)
                if (size_cache[i].taken < e->taken)
                    e = &size_cache[i];
        }
        snprintf(e->id, sizeof e->id, "%s", c->id);
    }
    snprintf(e->base, sizeof e->base, "%s", c->base);
    e->taken = now;
    e->files = got_files;
    e->lines = got_lines;
    e->any = any;

    if (files)
        *files = got_files;
    if (lines)
        *lines = got_lines;
    return any;
}

int boardaudit_wanted(const struct board_card *c)
{
    const struct board_cfg *cfg = boardcfg();

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

char *boardaudit_prompt(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for_job("audit");
    const char                 *head = p && p->prompt ? p->prompt : "";

    size_t need = strlen(head) + strlen(c->title) + 256;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%s\n\nThe branch came off %s. The card it was built for:\n\n%s\n",
             head, c->base, c->title);
    return out;
}

static enum board_col next_after_audit(const char *id)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    enum board_col     col = boardflow_from(c ? c->kind : "", BOARD_STEP_MERGE, 0);
    board_free(cards, n);
    return col;
}

static int finding_text(cJSON *f, char *out, size_t size)
{
    const char *text = cJSON_GetStringValue(f);
    if (text && *text) {
        snprintf(out, size, "%s", text);
        return 1;
    }
    if (!cJSON_IsObject(f))
        return 0;

    static const char *const SAYS[] = {"finding", "text", "message", "detail",
                                       "description", "issue"};
    for (size_t i = 0; i < sizeof SAYS / sizeof *SAYS; i++) {
        text = cJSON_GetStringValue(cJSON_GetObjectItem(f, SAYS[i]));
        if (!text || !*text)
            continue;
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(f, "file"));
        if (file && *file && !strstr(text, file))
            snprintf(out, size, "%s: %s", file, text);
        else
            snprintf(out, size, "%s", text);
        return 1;
    }
    return 0;
}

int boardaudit_is_marker(const char *text)
{
    return text && (!strcmp(text, BOARDAUDIT_PASS) ||
                    !strcmp(text, BOARDAUDIT_NO_VERDICT));
}

int boardaudit_finished(const char *id, const char *reply)
{
    if (!id || !*id)
        return 0;

    boardlog_turn(id, "audit", NULL, reply);

    cJSON *o = replyjson_parse(reply);
    if (!o) {
        board_note(id, "audit", BOARDAUDIT_NO_VERDICT);
        board_move(id, next_after_audit(id), "audit", NULL);
        return 1;
    }

    const cJSON *clean = cJSON_GetObjectItem(o, "clean");
    const cJSON *found = cJSON_GetObjectItem(o, "findings");

    int nfound = 0;
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, found) {
        char said[1024];
        if (!finding_text((cJSON *)f, said, sizeof said))
            continue;
        board_note(id, "audit", said);
        nfound++;
    }

    int passed = nfound == 0 && (!clean || !cJSON_IsFalse(clean));
    cJSON_Delete(o);

    if (passed) {
        board_note(id, "audit", BOARDAUDIT_PASS);
        board_move(id, next_after_audit(id), "audit", NULL);
    } else {
        board_move(id, BOARD_DOING, "audit", NULL);
    }
    return 1;
}
