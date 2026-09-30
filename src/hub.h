#ifndef HUB_H
#define HUB_H

#include <stddef.h>

void hub_ensure(void);
int  hub_self_path(char *out, size_t size);

int hub_main(int argc, char **argv);

#endif
