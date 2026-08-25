#include "boardstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

/* The session took every step before this one, so it is not told again what
 * was said: it is only told what this step is. */
char *boardstep_prompt(const struct board_card *c, const struct board_action *p)
{
    if (!c || !p)
        return NULL;

    const char *head = p->prompt ? p->prompt : "";
    const char *body = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(head) + strlen(body) + strlen(c->title) + 128;
    char  *out = malloc(need);
    if (!out)
        return NULL;

    snprintf(out, need, "%s\n\nThe card this is for:\n\n# %s\n\n%s\n", head,
             c->title, body);
    return out;
}
