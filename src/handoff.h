#ifndef HANDOFF_H
#define HANDOFF_H

#include <stddef.h>

int handoff_wanted(void);

int handoff_take_request(char *id, size_t size, int *kill);

void handoff_refuse(const char *id);

int handoff_screen_path(const char *id, char *out, size_t size);
int handoff_publish(const char *id);

int handoff_ask(long pid, const char *id, char *screen, size_t size,
                void (*tick)(int waited_ms, void *ud), void *ud);

int handoff_kill(long pid, const char *id, char *screen, size_t size,
                 void (*tick)(int waited_ms, void *ud), void *ud);

int handoff_close(long pid, const char *id, void (*tick)(int waited_ms, void *ud), void *ud);

#endif
