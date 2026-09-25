#ifndef OVERLAY_H
#define OVERLAY_H

struct overlay {
    int   row, col, w, rows;
    void (*paint_row)(void *ud, int at, int width);
    void *ud;
};

void overlay_put(const char *under, const struct overlay *o);

#endif
