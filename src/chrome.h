#ifndef CHROME_H
#define CHROME_H

struct prompt;

void chrome_bind(struct prompt *p);

void chrome_paint(void);

int  chrome_paint_spin(void);

void chrome_clear(void);

void chrome_keep_above(void);

int  chrome_rows_left(void);

int  chrome_gap(void);

typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);

int  chrome_modal_active(void);

void chrome_modal_interrupt(int (*fn)(void));
int  chrome_modal_interrupted(void);

#endif
