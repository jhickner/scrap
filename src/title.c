#include "title.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvlog.h"
#include "text.h"

#define TITLE_MAX   80

int title_lookup(const char *id, char *out, size_t size)
{
    char path[1200];
    if (!id || !*id || !path_config_file(path, sizeof path, "titles"))
        return 0;
    return kvlog_lookup(path, id, out, size);
}

static int tidy(char *text)
{
    text[strcspn(text, "\n")] = '\0';
    char *p = text;
    while (*p == ' ' || *p == '"' || *p == '\'')
        p++;
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '.' || p[n - 1] == '"' || p[n - 1] == '\''))
        p[--n] = '\0';
    if (n == 0 || n > TITLE_MAX)
        return 0;
    memmove(text, p, n + 1);
    return 1;
}

static void write_cache(const char *id, const char *title)
{
    char path[1200];
    if (!path_config_file(path, sizeof path, "titles"))
        return;
    (void)kvlog_append(path, id, title);
}

int title_clean(const char *name, char *out, size_t size)
{
    if (!name || !out || size == 0)
        return 0;
    char text[256];
    snprintf(text, sizeof text, "%s", name);
    for (char *p = text; *p; p++)
        if (*p == '\t' || *p == '\n')
            *p = ' ';
    if (!tidy(text))
        return 0;
    snprintf(out, size, "%s", text);
    return 1;
}

int title_set(const char *id, const char *name)
{
    char text[256];
    if (!id || !*id || !title_clean(name, text, sizeof text))
        return 0;
    write_cache(id, text);
    return 1;
}
