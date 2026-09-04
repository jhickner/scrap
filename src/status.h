
#ifndef STATUS_H
#define STATUS_H

#define SPIN_FRAME_MS 90

int    spin_advance(int *frame, double *at);

const char *spin_glyph(int frame);

void   status_begin(void);
void   status_begin_at(double elapsed);
void   status_end(void);

void   status_paint_spin(void);
void   status_paint_sticky(void);
int    status_sticky_measure(void);

int    status_spinning(void);

void   status_set_word(const char *text);

void   status_set_alert(const char *text);

void   status_set_note(const char *text);

void   status_tick(void);

void   status_touch(void);

void   status_pause(void);
void   status_resume(void);

#define STICKY_LINES 3

void   status_sticky_set(int on);
int    status_sticky_enabled(void);

void   status_sticky_prompt(const char *text);

int    status_sticky_rows(void);

const char *status_sticky_offscreen(void);

void   status_sticky_busy(int on);

void   status_sticky_erased(void);

double status_elapsed(void);

#endif
