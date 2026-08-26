#ifndef EDIT_H
#define EDIT_H

/* the ceiling on an $EDITOR round trip */
#define EDIT_MAX_BYTES (1u << 22)

int edit_open(const char *path);

#endif
