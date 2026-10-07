#ifndef HANDOFF_H
#define HANDOFF_H

#include <stddef.h>

int handoff_wanted(void);

int handoff_take_request(char *id, size_t size, long *asker);

int handoff_withdrawn(const char *id, long asker);
void handoff_forget(const char *id);

void handoff_refuse(const char *id);

int handoff_screen_path(const char *id, char *out, size_t size);
int handoff_publish(const char *id);

int handoff_ask(long pid, const char *id, char *screen, size_t size,
                int (*tick)(int waited_ms, void *ud), void *ud);

#endif
