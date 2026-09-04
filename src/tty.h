
#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>

struct termios;

typedef enum {
    TK_CHAR,
    TK_TEXT,
    TK_ENTER,
    TK_NEWLINE,
    TK_BACKSPACE,
    TK_DELETE,
    TK_TAB,
    TK_NEXT_TAB,
    TK_PREV_TAB,
    TK_ESCAPE,
    TK_LEFT,
    TK_RIGHT,
    TK_UP,
    TK_DOWN,
    TK_WORD_LEFT,
    TK_WORD_RIGHT,
    TK_HOME,
    TK_END,
    TK_PAGE_UP,
    TK_PAGE_DOWN,
    TK_RESIZE,
    TK_EOF,
    TK_SCROLL_UP,
    TK_SCROLL_DOWN,
    TK_MOUSE_DOWN,

    TK_NONE,
} tty_key;

typedef struct {
    tty_key   key;
    uint32_t  cp;
    char     *text;
    int       row;
    int       col;
} tty_event;

int  tty_raw_begin(void);

void tty_raw_end(void);

/* Restore cooked input for an exec handoff without leaving the alternate
   screen that the replacement process inherits. */
void tty_raw_handoff(void);

void tty_keyboard_on(void);

int  tty_read(tty_event *ev, int timeout_ms);

#define TTY_WATCH_MAX 32

void tty_wake(void);

void tty_watch(int (*fds)(void *ud, int *out, int max), void (*ready)(void *ud),
               void *ud);

int  tty_watch_fds(int *out, int max);

void tty_watch_ready(void);

int  tty_is_raw(void);

int  tty_quit_requested(void);

int tty_cooked_termios(struct termios *out);

size_t tty_take_pending(void *buf, size_t max);

int tty_input_waiting(void);

unsigned tty_resize_epoch(void);

#define TTY_RESIZE_SETTLE_MS 100

#define TTY_MIN_COLUMNS 20

int  tty_columns(void);

int  tty_screen_columns(void);

int  tty_rows(void);

#endif
