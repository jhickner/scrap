#ifndef INSTANCE_H
#define INSTANCE_H

#include <stddef.h>

struct tab_args;

int instance_save(const char *name);

int instance_names(char *out, size_t size);

int instance_open(const char *name, char *front, size_t front_size, struct tab_args *t,
                  char *tabs, size_t tabs_size, char *held, size_t held_size);

#endif
