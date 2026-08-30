
#ifndef TOOLSTYLE_H
#define TOOLSTYLE_H

#include <stddef.h>

/* the plain lowercase name of a tool call, with any mcp__server__ prefix
   dropped */
void toolstyle_label(char *out, size_t size, const char *name);

int toolstyle_collapses(const char *name, const char *input_json, const char *arg);

int toolstyle_shell_reads(const char *command);

int toolstyle_is_shell(const char *name);

#endif
