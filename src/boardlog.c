#include "boardlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "text.h"

#define LOG_DIR "board-log"
#define LOG_MAX (1u << 20)

int boardlog_path(const char *id, char *out, size_t size)
{
    return board_md_path(LOG_DIR, id, out, size);
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

struct log_head {
    const char *id;
    const char *worktree;
    const char *rest;
};

static int spit_head(FILE *f, void *ud)
{
    const struct log_head *h = ud;
    if (fprintf(f, "# card %s\n", h->id) < 0)
        return 0;
    if (h->worktree && *h->worktree &&
        fprintf(f, "worktree %s\n", h->worktree) < 0)
        return 0;
    return !h->rest || fputs(h->rest, f) >= 0;
}

void boardlog_worktree(const char *id, const char *path)
{
    char file[4300];
    if (!path || !*path || !boardlog_path(id, file, sizeof file))
        return;

    char *text = text_slurp(file, LOG_MAX, NULL);
    const char *rest = text ? text : "";

    char title[64];
    int  n = snprintf(title, sizeof title, "# card %s\n", id);
    if (text && n > 0 && !strncmp(text, title, (size_t)n)) {
        rest = text + n;
        if (!strncmp(rest, "worktree ", 9)) {
            const char *nl = strchr(rest, '\n');
            rest = nl ? nl + 1 : rest + strlen(rest);
        }
    }

    struct log_head h = {.id = id, .worktree = path, .rest = rest};
    text_spit(file, spit_head, &h);
    free(text);
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
