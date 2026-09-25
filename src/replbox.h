#ifndef REPLBOX_H
#define REPLBOX_H

#include "replframe.h"
#include "tty.h"
#include "vendor/repl.h"

struct replbox {
    Repl             repl;
    struct replframe frame;
    int              cols;
    int              rows;
    int              top;
    int              fresh;
};

void replbox_init(struct replbox *b, const ReplCommand *cmds, int n);
void replbox_free(struct replbox *b);

void replbox_width(struct replbox *b, int cols);
int  replbox_wants(const struct replbox *b);
int  replbox_render(struct replbox *b, int rows);
void replbox_scroll(struct replbox *b, int room);
int  replbox_top(const struct replbox *b);
void replbox_paint_row(const struct replbox *b, int y, int gutter, int focused);

int replbox_key(struct replbox *b, const tty_event *ev);

const char *replbox_line(const struct replbox *b);
void        replbox_set_text(struct replbox *b, const char *text);

#endif
