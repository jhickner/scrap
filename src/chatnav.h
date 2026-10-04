#ifndef CHATNAV_H
#define CHATNAV_H

#include <stddef.h>

struct session;

typedef void (*chatnav_bind_fn)(struct session *s, int active, void *ud);

struct chatnav_output {
    void *ud;
    void (*note)(void *ud, const char *text);
    void (*menu_begin)(void *ud, const char *kind);
    void (*menu_add)(void *ud, const char *label, const char *payload);
    void (*menu_send)(void *ud, const char *title, int per_row);
};

struct chatnav {
    struct session   *session;
    chatnav_bind_fn  bind;
    void             *bind_ud;
    struct chatnav_output output;
};

void chatnav_init(struct chatnav *b, struct session *s,
                   chatnav_bind_fn bind, void *ud);
void chatnav_set_output(struct chatnav *b, const struct chatnav_output *output);

struct session *chatnav_session(const struct chatnav *b);
void            chatnav_forget(struct chatnav *b, struct session *s);
void            chatnav_focus(struct chatnav *b, struct session *s);
void            chatnav_refocus(struct chatnav *b);

int chatnav_switch(struct chatnav *b, int index);

void chatnav_send_here(struct chatnav *b);
void chatnav_send_tabs(struct chatnav *b, int menu_max);
void chatnav_send_resume(struct chatnav *b, int menu_max);
void chatnav_cmd_open(struct chatnav *b, const char *cwd, const char *id);
void chatnav_cmd_close(struct chatnav *b, int index);
void chatnav_cmd_switch(struct chatnav *b, int index);

const char *chatnav_dir_name(const char *path);
void chatnav_tab_label(int index, char *out, size_t size);
void chatnav_tab_payload(int index, char *out, size_t size);
int  chatnav_tab_from_payload(const char *payload);

#endif
