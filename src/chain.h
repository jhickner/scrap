#ifndef CHAIN_H
#define CHAIN_H

#include <stddef.h>

#define CHAIN_ID_MAX   40
#define CHAIN_NAME_MAX 40

struct chain_segment {
    char backend[32];
    char cwd[1024];
    char id[128];
};

struct chain_record {
    char                  name[CHAIN_NAME_MAX];
    int                   cut;
    int                   n;
    struct chain_segment *seg;
};

typedef void (*chain_scan_fn)(const char *chain, const struct chain_record *r, void *ctx);

void chain_new(char *out, size_t size);

int  chain_read(const char *chain, struct chain_record *r);
void chain_free(struct chain_record *r);
void chain_scan(chain_scan_fn fn, void *ctx);

int chain_find(const char *id, char *out, size_t size);
int chain_named(const char *name, char *out, size_t size);

void chain_add(const char *chain, const char *name, const char *backend, const char *cwd,
               const char *id);
void chain_set_name(const char *chain, const char *name);
void chain_cut(const char *chain, const char *id);

int chain_before(const char *chain, const char *id, struct chain_segment **out);

void chain_copy(const char *from, const char *until, const char *to, const char *name);

#endif
