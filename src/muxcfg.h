
#ifndef MUXCFG_H
#define MUXCFG_H

#include <stddef.h>

#include "vendor/cJSON.h"

#define MUX_MAX      12
#define MUX_SETS     8
#define MUX_NAME     48
#define MUX_PROMPT   512

struct mux_spec {
    char backend[32];
    char model[128];
    char effort[32];
    char prompt[MUX_PROMPT];
};

struct mux_set {
    char            name[MUX_NAME];
    int             n;
    struct mux_spec row[MUX_MAX];
};

int         muxcfg_load(struct mux_spec *out, int max);
const char *muxcfg_active(void);

void muxcfg_label(const struct mux_spec *m, char *out, size_t cap);

void muxcfg_run(void);

int muxcfg_install(const char *name, const struct mux_spec *v, int n);

void muxcfg_field(char *out, size_t cap, const cJSON *obj, const char *key);

/* the stored configs, for the editor in muxcfgui.c */
int             muxcfg_count(void);
int             muxcfg_index(void);
struct mux_set *muxcfg_at(int i);
void            muxcfg_select(int i);
int             muxcfg_name_taken(const char *name, int except);
int             muxcfg_new(const char *name);
int             muxcfg_drop(int i);
void            muxcfg_save(void);

#endif
