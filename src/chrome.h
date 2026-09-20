#ifndef CHROME_H
#define CHROME_H

#include "tty.h"

struct prompt;

void chrome_bind(struct prompt *p);

void chrome_paint(void);
/* a one-line label painted live above the input, or NULL for none */
void chrome_live_label(const char *(*fn)(void));

int  chrome_paint_spin(void);

/* the session a click on the tab bar names, or -1 for a click anywhere else */
int  chrome_tab_at(int row, int col);

void chrome_clear(void);

void chrome_keep_above(void);

int  chrome_rows_left(void);

int  chrome_gap(void);

/* a modal covers the whole screen, leaving no session row painted behind it */
void chrome_full(int on);

/* the rows a modal has to paint over */
int  chrome_modal_rows(void);

/* the bar and the title a modal heads with */
void chrome_title_paint(const char *title);

/* the hint block, or the y/n question that stands in for it, under a modal */
int  chrome_foot_rows(const char *ask, const char *hint, int columns);
void chrome_foot_paint(const char *ask, const char *hint, int columns);

/* 1 yes, 0 no, -1 ignore */
int  chrome_read_yesno(const tty_event *ev);

typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);
void chrome_modal_keep(void);

int  chrome_modal_active(void);

void chrome_modal_interrupt(int (*fn)(void));
int  chrome_modal_interrupted(void);

#endif
