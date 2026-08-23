#ifndef REMINDERS_H
#define REMINDERS_H

#include <time.h>
#include <stddef.h>

const char *reminders_path(void);

int reminders_pop_due(time_t now, char *out, size_t n);

int reminders_scheduled_count(void);

#endif
