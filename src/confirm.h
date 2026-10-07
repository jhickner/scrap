#ifndef CONFIRM_H
#define CONFIRM_H

struct permission;

int confirm_run(const char *question);
int confirm_permission(const char *from, const struct permission *p);

#endif
