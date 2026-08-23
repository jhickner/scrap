#include "boardtriage.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "board.h"
#include "child.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "sessionfork.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

// Triage runs as a child so the board stays live while it thinks. The pool
// and the reaping belong to child.c; what is left here is the question, and
// what to do with the answer.
#define TRIAGE_KEY "triage:"

// A card short enough to read as a row is its own best title. Rewriting one
// buys a few columns and stakes the meaning on a paraphrase -- and a title
// that inverts a card is worse than a title that runs off the edge, because
// the truncated one is still the words that were written.
#define TITLE_KEEP 100

static void triage_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, TRIAGE_KEY "%s", id);
}

/* ---- what the card is asked ------------------------------------------- */

// The prompt, the card, and where it was captured. The directory matters:
// most cards are thrown from the repo they belong to, so it is the strongest
// hint triage has, and it is still only a hint.
static char *build_prompt(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_TRIAGE);
    const char                 *head = p->prompt ? p->prompt : "";
    const char                 *body = c->body && *c->body ? c->body : c->title;

    // The kinds are configuration, so the prompt says where they go rather
    // than listing them. A prompt edited to drop the mark still gets them.
    char kinds[4096];
    boardcfg_kinds_block(kinds, sizeof kinds);

    const char *mark = strstr(head, "{kinds}");
    size_t      lead = mark ? (size_t)(mark - head) : strlen(head);
    const char *rest = mark ? mark + strlen("{kinds}") : "";

    size_t need = strlen(head) + strlen(kinds) + strlen(body) + strlen(c->cwd) + 128;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%.*s%s%s%s\n\nThe card was captured in: %s\n\ncard:\n%s\n",
             (int)lead, head, kinds, mark ? "" : "\n", rest,
             c->cwd[0] ? c->cwd : "(nowhere in particular)", body);
    return out;
}

/* ---- what comes back --------------------------------------------------- */


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


// Counted from the last thing a person said, not from the start of the card.
// Answering the question triage asked is new information, so the passes it
// made before the answer are not passes at this card.
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

// Writes what triage decided onto the card, and moves it to whichever column
// that decision implies.
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

    // A kind the configuration does not name is a kind nothing downstream
    // knows what to do with, so it counts as not having decided.
    int unsure = confidence < 0.5 || *question || !boardcfg_kind(kind);

    struct board_card edited = *c;
    char             *spec_kept = NULL;

    // A title is only how the card reads in a list, so a better one is worth
    // having either way -- but only where there is something to gain by it.
    if (*title && c->body && strlen(c->body) > TITLE_KEEP)
        snprintf(edited.title, sizeof edited.title, "%s", title);

    // The rest is what triage worked out, and it only worked anything out if
    // it understood the card. Writing its spec over one it could not read
    // would lose the words the card was captured with -- which are the exact
    // words the question is about.
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
        // The kind is not written either: a guess recorded as a fact is
        // exactly what the column exists to avoid.
        edited.col = BOARD_UNCLEAR;
        snprintf(said, sizeof said, "%s",
                 *question ? question : "could not tell what this card is");
    } else {
        snprintf(edited.kind, sizeof edited.kind, "%s", kind);
        // Every kind waits for a worker now: filing a note is work too, and
        // the wiki is not going to write itself.
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

/* ---- what has come back ------------------------------------------------- */

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

    const struct board_profile *p = boardcfg_for(BOARD_WHO_TRIAGE);

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

// Whether a finished child was triage's, and if so what it decided. Anything
// filed under another key is not ours to take.
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
