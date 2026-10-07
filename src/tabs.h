#ifndef TABS_H
#define TABS_H

#include <stdio.h>

struct session;

struct tab_args {
    const char *screen, *backend, *cwd, *model, *effort, *id, *remote, *name;
    int         memory;
    char        latest[128];
};

int tabs_parse(char *line, struct tab_args *t);

void tabs_write(FILE *f, const struct session *s, const char *screen);

void tabs_prepare(const char *path);

int tabs_queue(struct session *s, const char *screen);

int tabs_pending(void);

int tabs_start(struct session *front);

void tabs_admit(int all);

void tabs_drop_all(void);

#endif
