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
    char parent[128];
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

/* The sessions of windows that are gone, newest first. A dead window's records
   are kept until the session turns up live again, so a reopen that fails
   leaves the window there to try again. */
int livelist_closed_load(struct live_session **out);

int livelist_alive(long pid);

/* Move the terminal to the tmux pane a session runs in. Returns 0 and fills
   why when there is no pane to go to. */
int livelist_jump(const struct live_session *v, char *why, int size);

const char *livelist_tmux_window(void);

#endif
