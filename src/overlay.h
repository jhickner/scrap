#ifndef OVERLAY_H
#define OVERLAY_H

/* a block of rows drawn over one that is already painted: the cells the box
   does not cover keep what was under them */
struct overlay {
    int   row, col, w, rows;
    void (*paint_row)(void *ud, int at, int width);
    void *ud;
};

/* writes the painted block with the overlay composited onto it */
void overlay_put(const char *under, const struct overlay *o);

#endif
