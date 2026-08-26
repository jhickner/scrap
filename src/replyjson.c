#include "replyjson.h"

#include <stdlib.h>
#include <string.h>

static const char *opener(const char *text, const char *close)
{
    int depth = 0;
    for (size_t i = (size_t)(close - text) + 1; i-- > 0;) {
        if (text[i] == '"') {
            int escaped;
            do {
                if (i == 0)
                    break;
                i--;
                escaped = 0;
                for (size_t b = i; b > 0 && text[b - 1] == '\\'; b--)
                    escaped = !escaped;
            } while (i > 0 && (text[i] != '"' || escaped));
            continue;
        }
        if (text[i] == '}')
            depth++;
        else if (text[i] == '{' && --depth == 0)
            return text + i;
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
