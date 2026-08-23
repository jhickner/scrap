#include "replyjson.h"

#include <stdlib.h>
#include <string.h>

static const char *opener(const char *text, const char *close)
{
    int depth = 0;
    for (const char *p = close; p >= text; p--) {
        if (*p == '"') {
            int escaped;
            do {
                p--;
                escaped = 0;
                for (const char *b = p - 1; b >= text && *b == '\\'; b--)
                    escaped = !escaped;
            } while (p > text && (*p != '"' || escaped));
            continue;
        }
        if (*p == '}')
            depth++;
        else if (*p == '{' && --depth == 0)
            return p;
    }
    return NULL;
}

cJSON *replyjson_parse(const char *text)
{
    if (!text)
        return NULL;

    const char *close = text + strlen(text);
    while (close-- > text) {
        if (*close != '}')
            continue;

        const char *open = opener(text, close);
        if (!open)
            continue;

        size_t n = (size_t)(close - open) + 1;
        char  *slice = malloc(n + 1);
        if (!slice)
            return NULL;
        memcpy(slice, open, n);
        slice[n] = '\0';
        cJSON *o = cJSON_Parse(slice);
        free(slice);
        if (o)
            return o;
        close = open;
    }
    return NULL;
}
