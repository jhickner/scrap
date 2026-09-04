#ifndef TGBRIDGE_H
#define TGBRIDGE_H

#include <stddef.h>

struct session;

typedef void (*tgbridge_bind_fn)(struct session *s, int active, void *ud);

struct tgbridge_output {
    void *ud;
    void (*note)(void *ud, const char *text);
    void (*menu_begin)(void *ud, const char *kind);
    void (*menu_add)(void *ud, const char *label, const char *payload);
    void (*menu_send)(void *ud, const char *title, int per_row);
};

struct tgbridge {
    struct session   *session;
    tgbridge_bind_fn  bind;
    void             *bind_ud;
    struct tgbridge_output output;
};

void tgbridge_init(struct tgbridge *b, struct session *s,
                   tgbridge_bind_fn bind, void *ud);
void tgbridge_set_output(struct tgbridge *b, const struct tgbridge_output *output);

struct session *tgbridge_session(const struct tgbridge *b);
void            tgbridge_forget(struct tgbridge *b, struct session *s);
void            tgbridge_focus(struct tgbridge *b, struct session *s);
void            tgbridge_refocus(struct tgbridge *b);

int tgbridge_switch(struct tgbridge *b, int index);
int tgbridge_open(const char *cwd, const char *id);

void tgbridge_send_here(struct tgbridge *b);
void tgbridge_send_tabs(struct tgbridge *b, int menu_max);
void tgbridge_send_resume(struct tgbridge *b, int menu_max);
void tgbridge_open_tab(struct tgbridge *b, const char *cwd, const char *id);
void tgbridge_close_tab(struct tgbridge *b, int index);
void tgbridge_switch_tab(struct tgbridge *b, int index);

const char *tgbridge_dir_name(const char *path);
void tgbridge_tab_label(int index, char *out, size_t size);
void tgbridge_tab_payload(int index, char *out, size_t size);
int  tgbridge_tab_from_payload(const char *payload);

int  tgbridge_workspace_fds(void *ud, int *out, int max);
void tgbridge_workspace_ready(void *ud);

#endif
