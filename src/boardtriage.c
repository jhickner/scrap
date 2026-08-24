#include "boardtriage.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "board.h"
#include "child.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

#define TRIAGE_KEY "triage:"

#define TITLE_KEEP 100

static void triage_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, TRIAGE_KEY "%s", id);
}

static char *build_prompt(const struct board_card *c)
{
    const struct board_role *p = boardcfg_for_job("triage");
    const char                 *head = p && p->prompt ? p->prompt : "";
    const char                 *body = c->body && *c->body ? c->body : c->title;

    char kinds[4096];
    boardcfg_kinds_block(kinds, sizeof kinds);

    char projects[8192];
    boardcfg_projects_block(projects, sizeof projects);

    char where[8600] = {0};
    if (projects[0])
        snprintf(where, sizeof where,
                 "\n\ncwd is the directory of the project the card is about, "
                 "which need not be the one it was captured in. Match the "
                 "project the card names against this list and answer with its "
                 "path; if none of them is it, keep the capture directory.\n%s",
                 projects);

    const char *mark = strstr(head, "{kinds}");
    size_t      lead = mark ? (size_t)(mark - head) : strlen(head);
    const char *rest = mark ? mark + strlen("{kinds}") : "";

    size_t need = strlen(head) + strlen(kinds) + strlen(where) + strlen(body) +
                  strlen(c->cwd) + 128;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%.*s%s%s%s\n\nThe card was captured in: %s%s\n\ncard:\n%s\n",
             (int)lead, head, kinds, mark ? "" : "\n", rest,
             c->cwd[0] ? c->cwd : "(nowhere in particular)", where, body);
    return out;
}

static const char *str_of(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

static double num_of(const cJSON *o, const char *key, double fallback)
{
    const cJSON *j = cJSON_GetObjectItem((cJSON *)o, key);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : fallback;
}

int boardtriage_attempts(const struct board_card *c)
{
    int n = 0;
    for (int i = 0; i < c->log_n; i++) {
        if (!strcmp(c->log[i].who, "you"))
            n = 0;
        else if (!strcmp(c->log[i].who, "triage"))
            n++;
    }
    return n;
}

static int apply(const char *id, const cJSON *o)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    if (!c) {
        board_free(cards, n);
        return 0;
    }

    const char *kind = str_of(o, "kind");
    const char *title = str_of(o, "title");
    const char *spec = str_of(o, "spec");
    const char *cwd = str_of(o, "cwd");
    const char *question = str_of(o, "question");
    double      confidence = num_of(o, "confidence", 1.0);

    int unsure = confidence < 0.5 || *question || !boardcfg_kind(kind);

    struct board_card edited = *c;
    char             *spec_kept = NULL;

    if (*title && c->body && strlen(c->body) > TITLE_KEEP)
        snprintf(edited.title, sizeof edited.title, "%s", title);

    if (!unsure) {
        if (*spec) {
            spec_kept = strdup(spec);
            if (spec_kept)
                edited.body = spec_kept;
        }
        if (*cwd)
            snprintf(edited.cwd, sizeof edited.cwd, "%s", cwd);
        edited.priority = boardcfg_priority(kind);
    }

    char said[1024];
    if (unsure) {
        edited.col = BOARD_UNCLEAR;
        snprintf(said, sizeof said, "%s",
                 *question ? question : "could not classify it");
    } else {
        snprintf(edited.kind, sizeof edited.kind, "%s", kind);

        edited.col = BOARD_BACKLOG;
        snprintf(said, sizeof said, "%s · %s · priority %d", kind,
                 boardcfg_kind_takes(kind, BOARD_STEP_WORKTREE) ? "to build" : "to file",
                 edited.priority);
    }

    int ok = board_update(&edited);
    free(spec_kept);
    board_free(cards, n);
    if (ok)
        board_note(id, "triage", said);
    return ok;
}

static void failed(const char *id, const char *why)
{
    board_move(id, BOARD_UNCLEAR, "triage", why);
}

int boardtriage_start(const struct board_card *c)
{
    if (!c)
        return 0;

    char key[CHILD_KEY_MAX];
    triage_key(c->id, key, sizeof key);
    if (child_running(key))
        return 0;

    char *prompt = build_prompt(c);
    if (!prompt)
        return 0;

    char *argv[BOARDCFG_ARGV_MAX];
    if (!boardcfg_argv(boardcfg_for_job("triage"), prompt, argv, COUNT(argv))) {
        free(prompt);
        return 0;
    }

    int ok = child_start(key, argv, c->cwd[0] ? c->cwd : NULL);
    if (ok)
        boardlog_turn(c->id, "triage", prompt, NULL);
    free(prompt);
    return ok;
}

int boardtriage_running(const char *id)
{
    char key[CHILD_KEY_MAX];
    triage_key(id, key, sizeof key);
    return child_running(key);
}

int boardtriage_take(const char *key, const char *reply)
{
    {
        size_t mark = strlen(TRIAGE_KEY);
        if (key && !strncmp(key, TRIAGE_KEY, mark))
            boardlog_turn(key + mark, "triage", NULL, reply);
    }
    size_t mark = strlen(TRIAGE_KEY);
    if (!key || strncmp(key, TRIAGE_KEY, mark))
        return 0;

    const char *id = key + mark;
    cJSON      *o = replyjson_parse(reply);
    if (o) {
        apply(id, o);
        cJSON_Delete(o);
        return 1;
    }
    failed(id, reply && *reply ? "triage did not answer with a card"
                               : "triage did not answer");
    return 1;
}
