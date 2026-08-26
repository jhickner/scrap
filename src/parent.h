#ifndef PARENT_H
#define PARENT_H

#include <stddef.h>

/* Which session a session was opened from. One parent, fixed at creation, so
   the graph is a forest and no reader has to guard against a cycle it made
   itself. The file is hand-editable, so readers still cap their depth. */
int parent_set(const char *child_id, const char *parent_id);

int parent_of(const char *child_id, char *out, size_t size);

#endif
