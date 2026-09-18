#include "sessionaddr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"

static int dir_path(char *out, size_t size)
{
    const char *env = getenv("MUX_ADDR_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    return path_config_subdir(out, size, "addr");
}

int sessionaddr_alloc(char *out, size_t size)
{
    static int seq;
    char dir[4200];
    if (!dir_path(dir, sizeof dir))
        return 0;
    if ((size_t)snprintf(out, size, "%s/%ld-%d", dir, (long)getpid(), seq++) >= size)
        return 0;
    /* an empty file, so a child that looks before the backend has reported an
       id reads nothing rather than failing to open */
    FILE *f = fopen(out, "w");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

struct write_id {
    const char *id;
};

static int write_id(FILE *f, void *ud)
{
    const struct write_id *w = ud;
    return fprintf(f, "%s\n", w->id) > 0;
}

void sessionaddr_write(const char *path, const char *id)
{
    if (!path || !*path || !id || !*id)
        return;

    char *have = text_slurp(path, 4096, NULL);
    if (have) {
        char *nl = strchr(have, '\n');
        if (nl)
            *nl = '\0';
        int same = !strcmp(have, id);
        free(have);
        if (same)
            return;
    }

    struct write_id w = {id};
    text_spit(path, write_id, &w);
}

void sessionaddr_forget(const char *path)
{
    if (path && *path)
        unlink(path);
}
