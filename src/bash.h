
#ifndef BASH_H
#define BASH_H

#include "vendor/cJSON.h"

int bash_is_command(const char *line);

/* Returns the wait status, or -1 if nothing ran; *out, if given, takes the output. */
int bash_run(const char *line, char **out);

char *bash_take_context(void);

char *bash_take_held(void);
void bash_drop_held(void);
const char *bash_held_label(void);

#define BASH_RAN_KIND "bash"
void bash_ran_load(const cJSON *st);

#endif
