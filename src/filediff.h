
#ifndef FILEDIFF_H
#define FILEDIFF_H

#include <stddef.h>

struct filediff_snapshot {
    int    have;
    char   path[4096];
    char  *before;
    size_t before_len;
};

void filediff_snapshot(struct filediff_snapshot *snap, const char *path);

char *filediff_take_patch(struct filediff_snapshot *snap);

int filediff_patch_draws(const char *patch);

int filediff_render_patch(const char *patch);

void filediff_clear(struct filediff_snapshot *snap);

#endif
