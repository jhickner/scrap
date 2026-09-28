#ifndef CHROME_H
#define CHROME_H

#include "tty.h"

struct prompt;

void chrome_bind(struct prompt *p);

void chrome_paint(void);

void chrome_live_label(const char *(*fn)(void));

int  chrome_paint_spin(void);


void chrome_clear(void);

void chrome_keep_above(void);

int  chrome_rows_left(void);

int  chrome_gap(void);

void chrome_full(int on);

int  chrome_modal_rows(void);

void chrome_title_paint(const char *title);

int  chrome_foot_rows(const char *ask, const char *hint, int columns);
void chrome_foot_paint(const char *ask, const char *hint, int columns);

int  chrome_read_yesno(const tty_event *ev);

typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);
void chrome_modal_keep(void);

int  chrome_modal_active(void);

void chrome_modal_interrupt(int (*fn)(void));
int  chrome_modal_interrupted(void);

#endif
