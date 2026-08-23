
#ifndef CHILD_H
#define CHILD_H

#include <stddef.h>

// A few background children with their output kept. The board runs everything
// that takes its time this way -- an agent asked one question, a rebase, a
// build -- so the list stays live while they run and answers land on cards
// rather than on the screen.
//
// Each is filed under a key the caller chooses, which is how it asks later
// whether that one is still going.

#define CHILD_KEY_MAX 32

// Runs `argv` in `cwd`, with its output captured and stdin closed. Nonzero
// once it is going. Fails when the pool is full or that key is already in it.
int child_start(const char *key, char *const argv[], const char *cwd);

// The same through a shell, for a sequence of steps rather than one program.
int child_shell(const char *key, const char *command, const char *cwd);

int child_running(const char *key);
int child_busy(void);

// Takes one child that has finished, filling `key` with whose it was and
// `out` with everything it wrote -- the caller frees that. `ok` is whether it
// exited cleanly. Returns zero when none has finished, so call it until it
// does.
int child_reap(char *key, size_t keysize, char **out, int *ok);

void child_close_all(void);

#endif
