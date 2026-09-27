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
    char name[40];
    char parent[128];
    char title[200];
    char status[16];
    char channels[48];
    int  unseen;
    char pane[32];
    long ts;
    int  mine;
};

void livelist_begin(void);

void livelist_publish(const struct session *s, const char *status);

void livelist_forget(const struct session *s);

void livelist_channels(char *out, int size);

int livelist_load(struct live_session **out);

int livelist_closed_load(struct live_session **out);

int livelist_alive(long pid);

int livelist_jump(const struct live_session *v, char *why, int size);

#endif
