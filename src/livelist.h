#ifndef LIVELIST_H
#define LIVELIST_H

struct session;

struct live_session {
    long pid;
    int  slot;
    char backend[32];
    char model[160];
    char label[160];
    char effort[32];
    char cwd[4096];
    char id[128];
    char title[200];
    char status[16];
    char card[16];
    int  unseen;
    char pane[32];
    char window[32];
    char wname[64];
    char pane_index[8];
    long ts;
    int  mine;
};

void livelist_begin(void);

void livelist_on_card(const char *(*fn)(const struct session *s));

void livelist_publish(const struct session *s, const char *status);

void livelist_forget(const struct session *s);

int livelist_load(struct live_session **out);

int livelist_alive(long pid);

const char *livelist_tmux_window(void);
const char *livelist_tmux_window_name(void);

#endif
