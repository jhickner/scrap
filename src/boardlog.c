#include "boardlog.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mdcfg.h"
#include "text.h"

#define LOG_DIR "board-log"

int boardlog_path(const char *id, char *out, size_t size)
{
    char dir[4096];
    if (!id || !*id || !mdcfg_dir(dir, sizeof dir, LOG_DIR))
        return 0;

    for (const char *p = id; *p; p++)
        if (*p == '/' || *p == '.')
            return 0;
    return (size_t)snprintf(out, size, "%s/%s.md", dir, id) < size;
}

static FILE *open_for(const char *id, const char **stamp_out)
{
    static char stamp[32];
    char        path[4300];
    if (!boardlog_path(id, path, sizeof path))
        return NULL;

    int fresh = access(path, F_OK) != 0;
    FILE *f = fopen(path, "ae");
    if (!f)
        return NULL;
    if (fresh)
        fprintf(f, "# card %s\n", id);

    time_t    now = time(NULL);
    struct tm when;
    localtime_r(&now, &when);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &when);
    if (stamp_out)
        *stamp_out = stamp;
    return f;
}

static void put_block(FILE *f, const char *title, const char *text)
{
    if (!text || !*text)
        return;
    size_t len = strlen(text);
    fprintf(f, "\n**%s**\n\n```\n%s%s```\n", title, text,
            text[len - 1] == '\n' ? "" : "\n");
}

void boardlog_turn(const char *id, const char *stage, const char *prompt,
                   const char *reply)
{
    const char *stamp = NULL;
    FILE       *f = open_for(id, &stamp);
    if (!f)
        return;

    fprintf(f, "\n## %s \xc2\xb7 %s\n", stamp, stage ? stage : "?");
    put_block(f, "prompt", prompt);
    put_block(f, "response", reply);
    fclose(f);
}

void boardlog_note(const char *id, const char *who, const char *text)
{
    const char *stamp = NULL;
    FILE       *f = open_for(id, &stamp);
    if (!f)
        return;

    const char *nl = text ? strchr(text, '\n') : NULL;
    if (nl && nl[1]) {
        fprintf(f, "\n## %s \xc2\xb7 %s\n", stamp, who ? who : "board");
        put_block(f, "note", text);
    } else {
        fprintf(f, "\n- `%s` **%s** %s\n", stamp, who ? who : "board",
                text ? text : "");
    }
    fclose(f);
}

void boardlog_remove(const char *id)
{
    char path[4300];
    if (boardlog_path(id, path, sizeof path))
        unlink(path);
}
