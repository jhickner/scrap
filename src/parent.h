#ifndef PARENT_H
#define PARENT_H

#include <stddef.h>

/* Which session a session was opened from. The file is hand-editable, so
   readers cap their depth. */
int parent_of(const char *child_id, char *out, size_t size);

#endif
