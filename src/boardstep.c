#include "boardstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boardlog.h"

char *boardstep_since(const struct board_card *c, const char *job)
{
    if (!c || !job || !*job)
        return NULL;

    int from = -1;
    for (int i = c->log_n - 1; i >= 0 && from < 0; i--)
        if (!strcmp(c->log[i].who, job))
            from = i;
    if (from < 0)
        return NULL;

    char  *out = NULL;
    size_t len = 0;
    FILE  *f = open_memstream(&out, &len);
    if (!f)
        return NULL;

    for (int i = from; i < c->log_n; i++) {
        const char *who = c->log[i].who, *text = c->log[i].text;
        if (!text || !*text)
            continue;
        if (!strcmp(who, "board"))
            continue;
        fprintf(f, "%s%s said:\n\n%s\n", len ? "\n" : "", who, text);
    }
    fclose(f);

    if (len)
        return out;
    free(out);
    return NULL;
}

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
