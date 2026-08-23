#ifndef MDCFG_H
#define MDCFG_H

#include <stddef.h>

#define MDCFG_KEYS 32
#define MDCFG_NAME 64

struct mdcfg {
    char *keys[MDCFG_KEYS];
    char *values[MDCFG_KEYS];
    int   n;
    char *body;
    char *text;
};

int  mdcfg_load(struct mdcfg *m, const char *path);
void mdcfg_free(struct mdcfg *m);

const char *mdcfg_get(const struct mdcfg *m, const char *key);
int         mdcfg_has(const struct mdcfg *m, const char *key);
int         mdcfg_int(const struct mdcfg *m, const char *key, int fallback);

int mdcfg_write(const char *path, const char *const *keys,
                const char *const *values, int n, const char *body);

int mdcfg_list(const char *dir, char names[][MDCFG_NAME], int max);

int mdcfg_dir(char *out, size_t size, const char *leaf);

#endif
