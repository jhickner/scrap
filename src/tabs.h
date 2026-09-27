#ifndef TABS_H
#define TABS_H

struct session;

void tabs_prepare(const char *path);

int tabs_queue(struct session *s, const char *screen);

int tabs_pending(void);

int tabs_start(struct session *front);

void tabs_admit(int all);

void tabs_drop_all(void);

#endif
