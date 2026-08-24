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

/* the bar and the title a modal heads with */
void chrome_title_paint(const char *title);

/* the hint block, or the y/n question that stands in for it, under a modal */
int  chrome_foot_rows(const char *ask, const char *hint, int columns);
void chrome_foot_paint(const char *ask, const char *hint, int columns);

typedef void (*chrome_modal_fn)(void *ud);
void chrome_modal(chrome_modal_fn fn, void *ud);
void chrome_modal_keep(void);

int  chrome_modal_active(void);

void chrome_modal_interrupt(int (*fn)(void));
int  chrome_modal_interrupted(void);

#endif
