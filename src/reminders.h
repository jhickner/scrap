#ifndef REMINDERS_H
#define REMINDERS_H

#include <time.h>
#include <stddef.h>

const char *reminders_path(void);

int reminders_pop_due(time_t now, char *out, size_t n);

/* `take` returning 0 leaves that reminder and later due ones in the store. */
int reminders_drain_due(time_t now, int (*take)(const char *text, void *ud),
                        void *ud);

int reminders_scheduled_count(void);

#endif
