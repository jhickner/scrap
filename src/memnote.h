#ifndef MEMNOTE_H
#define MEMNOTE_H

#include <stddef.h>

/* Runs argv with stdin from /dev/null and stdout captured, in its own process group,
 * killed after timeout seconds. stderr is captured too unless quiet. Returns the output,
 * NUL-terminated and malloc'd, or NULL if it could not run. *status is the wait status,
 * or -1 on a timeout. */
char *memnote_capture(char *const argv[], int quiet, size_t cap, int timeout,
                      size_t *len, int *status);

/* `scrap note TEXT`: an LLM tidies a dictated note and picks its store, title and tags,
 * then `mem create` saves it. Prints mem's JSON. */
int memnote_main(int argc, char **argv);

#endif
