#ifndef VIEWPORT_H
#define VIEWPORT_H

#include <stddef.h>

int  viewport_active(void);

void viewport_begin(void);
void viewport_end(void);

void viewport_handoff(void);
void viewport_inherit(void);
int  viewport_dump(const char *path);

void viewport_write(const char *s, size_t n);

typedef void (*viewport_render_fn)(void *ud, int cols);

struct viewport_entry {
    viewport_render_fn render;
    void              *ud;
    void             (*free_ud)(void *);
    int                reflow;
    int                pad_before;
    int                pad_after;
};

#define VIEWPORT_ROWS(before, after) \
    (&(struct viewport_entry){.pad_before = (before), .pad_after = (after)})

unsigned viewport_item_begin(const struct viewport_entry *e);
void viewport_item_end(void);

void viewport_suspend(void);
void viewport_resume(void);

void viewport_paint(void);

void viewport_forget(void);
void viewport_touch(void);

void viewport_chrome(char **rows, int n, int caret_row, int caret_col);
void viewport_chrome_clear(void);

int viewport_chrome_top(void);

void viewport_chrome_row(int at, const char *s);

void viewport_chrome_keep(int keep);

int viewport_ends_blank(void);

void viewport_clear(void);

unsigned viewport_mark(void);
int      viewport_visible(unsigned mark);

void *viewport_item_data(unsigned mark);

void  viewport_item_update(unsigned mark);

typedef char *(*viewport_encode_fn)(void *ud);
void viewport_item_persist(unsigned mark, const char *kind, viewport_encode_fn encode);

struct viewport_state;

struct viewport_state *viewport_state_new(void);
void viewport_state_free(struct viewport_state *st);

void viewport_hold(int on);

void viewport_stash(struct viewport_state *st);
void viewport_adopt(struct viewport_state *st);

void viewport_scroll(int delta);
void viewport_scroll_end(void);
int  viewport_scrolled(void);

#endif
