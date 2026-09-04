#ifndef DIRPICK_H
#define DIRPICK_H

/* a fuzzy pick over the directories under $HOME. The query starts on seed,
   which backspace can clear. Returns a malloc'd absolute path, or NULL. */
char *dirpick_run(const char *title, const char *seed);

#endif
