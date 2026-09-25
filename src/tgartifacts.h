#ifndef TGARTIFACTS_H
#define TGARTIFACTS_H

#include <stddef.h>

struct tgartifacts;

struct tgartifacts *tgartifacts_start(const char *dir, const char *bind, int port,
                                      const char *url, const char *token);
void                tgartifacts_stop(struct tgartifacts *a);

const char *tgartifacts_base(const struct tgartifacts *a);
const char *tgartifacts_dir(const struct tgartifacts *a);

void tgartifacts_list(const struct tgartifacts *a, char *out, size_t size);

#endif
