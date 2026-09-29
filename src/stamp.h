#ifndef STAMP_H
#define STAMP_H

void stamp_prepare(const char *name);

void stamp_show(const char *name);

void stamp_clear(void);

void stamp_cover(char **rows, int n, int cols);

#endif
