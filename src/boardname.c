#include "boardname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "board.h"
#include "boardcfg.h"
#include "boardlog.h"
#include "child.h"
#include "replyjson.h"
#include "vendor/cJSON.h"

#define NAME_KEY "name:"

/* A card short enough to read whole is already its own title, so naming only
   earns its turn on one long enough that the first line reads badly. */
#define TITLE_KEEP 100

static void name_key(const char *id, char *out, size_t size)
{
    snprintf(out, size, NAME_KEY "%s", id);
}

static char *build_prompt(const struct board_card *c)
{
    const struct board_action *p = boardcfg_action("name");
    const char                *head = p && p->prompt ? p->prompt : "";
    const char                *body = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(head) + strlen(body) + 32;
    char  *out = malloc(need);
    if (!out)
        return NULL;

    snprintf(out, need, "%s\n\ncard:\n%s\n", head, body);
    return out;
}

static int apply(const char *id, const cJSON *o)
{
    const char *title =
        cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, "title"));
    if (!title || !*title)
        return 0;

    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);

    int ok = 0;
    if (c) {
        struct board_card edited = *c;
        snprintf(edited.title, sizeof edited.title, "%s", title);
        ok = board_update(&edited);
    }

    board_free(cards, n);
    return ok;
}

int boardname_start(const struct board_card *c)
{
    if (!c || !c->body || strlen(c->body) <= TITLE_KEEP)
        return 0;

    char key[CHILD_KEY_MAX];
    name_key(c->id, key, sizeof key);
    if (child_running(key))
        return 0;

    char *prompt = build_prompt(c);
    if (!prompt)
        return 0;

    char *argv[BOARDCFG_ARGV_MAX];
    if (!boardcfg_argv(boardcfg_action("name"), prompt, argv, COUNT(argv))) {
        free(prompt);
        return 0;
    }

    int ok = child_start(key, argv, c->cwd[0] ? c->cwd : NULL);
    if (ok)
        boardlog_turn(c->id, "name", prompt, NULL);
    free(prompt);
    return ok;
}

int boardname_running(const char *id)
{
    char key[CHILD_KEY_MAX];
    name_key(id, key, sizeof key);
    return child_running(key);
}

/* A turn that answered with nothing usable leaves the title the card already
   had, which is why naming never asks you anything. */
int boardname_take(const char *key, const char *reply)
{
    size_t mark = strlen(NAME_KEY);
    if (!key || strncmp(key, NAME_KEY, mark))
        return 0;

    const char *id = key + mark;
    boardlog_turn(id, "name", NULL, reply);

    cJSON *o = replyjson_parse(reply);
    if (o) {
        apply(id, o);
        cJSON_Delete(o);
    }
    return 1;
}
