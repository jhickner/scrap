#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <stddef.h>

void highlight_shell(const char *text, size_t len, unsigned char *roles);
int  highlight_code(const char *info, const char *text, size_t len, unsigned char *roles);

#endif
