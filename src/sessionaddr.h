#ifndef SESSIONADDR_H
#define SESSIONADDR_H

#include <stddef.h>

/* A session's own name, for the child to read. The backend reports a session id
   long after the child is exec'd, so the id cannot go in the environment: the
   child gets the path of a file instead, as $MUX_SESSION_FILE, and mux writes
   the id into it once the backend reports one. Files live in
   ~/.config/mux/addr, or $MUX_ADDR_DIR when set. */

/* Reserve a file for one session and fill out with its path. Returns 0 when
   there is nowhere to put it. */
int sessionaddr_alloc(char *out, size_t size);

/* Publish id at path, atomically. Writing the same id twice is a no-op. */
void sessionaddr_write(const char *path, const char *id);

void sessionaddr_forget(const char *path);

#endif
