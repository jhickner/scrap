#include "muxcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "models.h"
#include "session.h"
#include "text.h"
#include "vendor/agents/backend.h"

#define MUX_FILE    "matrix.json"
#define MUX_DEFAULT "claude,codex,grok"

static struct mux_set sets[MUX_SETS];
static int            nsets;
static int            active;
static int            loaded;

static int known_backend(const char *name)
{
    for (const char *const *p = backend_names(); *p; p++)
        if (strcmp(name, *p) == 0)
            return 1;
    return 0;
}

static int store_path(char *out, size_t size)
{
    return path_config_file(out, size, MUX_FILE);
}

static void seed_default(struct mux_set *s)
{
    const char *p = MUX_DEFAULT;

    while (*p && s->n < MUX_MAX) {
        while (*p == ',' || *p == ' ')
            p++;
        if (!*p)
            break;

        struct mux_spec m = {0};
        size_t          run = strcspn(p, ",");
        int             bare = run == strcspn(p, ":,");

        for (int f = 0; f < 3; f++) {
            char  *out = f == 0 ? m.backend : f == 1 ? m.model : m.effort;
            size_t cap = f == 0 ? sizeof m.backend
                       : f == 1 ? sizeof m.model
                                : sizeof m.effort;
            size_t len = strcspn(p, ":,");
            if (len >= cap)
                len = cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            p += strcspn(p, ":,");
            if (*p != ':' || bare)
                break;
            p++;
        }
        p += strcspn(p, ",");

        if (!*m.backend || !known_backend(m.backend))
            continue;
        if (bare) {
            const char *model = session_saved_model(m.backend);
            const char *effort = session_saved_effort(m.backend);
            if (model)
                snprintf(m.model, sizeof m.model, "%s", model);
            if (effort)
                snprintf(m.effort, sizeof m.effort, "%s", effort);
        }
        s->row[s->n++] = m;
    }
}

void muxcfg_field(char *out, size_t cap, const cJSON *obj, const char *key)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    snprintf(out, cap, "%s", v ? v : "");
}

static void load_file(void)
{
    char path[4096];
    if (!store_path(path, sizeof path))
        return;

    char *text = text_slurp(path, 1 << 20, NULL);
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return;

    const char *want = cJSON_GetStringValue(cJSON_GetObjectItem(root, "active"));
    cJSON      *configs = cJSON_GetObjectItem(root, "configs");
    cJSON      *config;

    cJSON_ArrayForEach(config, configs) {
        if (nsets >= MUX_SETS || !config->string)
            break;
        struct mux_set *s = &sets[nsets++];
        snprintf(s->name, sizeof s->name, "%s", config->string);

        cJSON *row;
        cJSON_ArrayForEach(row, config) {
            if (s->n >= MUX_MAX)
                break;
            struct mux_spec m = {0};
            muxcfg_field(m.backend, sizeof m.backend, row, "backend");
            muxcfg_field(m.model, sizeof m.model, row, "model");
            muxcfg_field(m.effort, sizeof m.effort, row, "effort");
            muxcfg_field(m.prompt, sizeof m.prompt, row, "prompt");
            if (*m.backend && known_backend(m.backend))
                s->row[s->n++] = m;
        }
        if (want && !strcmp(want, s->name))
            active = nsets - 1;
    }
    cJSON_Delete(root);
}

static int write_text(FILE *f, void *ud)
{
    return fputs((const char *)ud, f) >= 0 && fputc('\n', f) != EOF;
}

static void save_file(void)
{
    char path[4096];
    if (!store_path(path, sizeof path))
        return;

    cJSON *root = cJSON_CreateObject();
    cJSON *configs = cJSON_AddObjectToObject(root, "configs");
    cJSON_AddStringToObject(root, "active", muxcfg_active());

    for (int i = 0; i < nsets; i++) {
        cJSON *rows = cJSON_AddArrayToObject(configs, sets[i].name);
        for (int r = 0; r < sets[i].n; r++) {
            const struct mux_spec *m = &sets[i].row[r];
            cJSON                 *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "backend", m->backend);
            cJSON_AddStringToObject(row, "model", m->model);
            cJSON_AddStringToObject(row, "effort", m->effort);
            cJSON_AddStringToObject(row, "prompt", m->prompt);
            cJSON_AddItemToArray(rows, row);
        }
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;

    text_spit(path, write_text, text);
    free(text);
}

static void ensure_loaded(void)
{
    if (loaded)
        return;
    loaded = 1;

    load_file();
    if (!nsets) {
        struct mux_set *s = &sets[nsets++];
        snprintf(s->name, sizeof s->name, "default");
        seed_default(s);
    }
    if (active >= nsets)
        active = 0;
}

const char *muxcfg_active(void)
{
    ensure_loaded();
    return sets[active].name;
}

int muxcfg_load(struct mux_spec *out, int max)
{
    ensure_loaded();

    int n = sets[active].n < max ? sets[active].n : max;
    memcpy(out, sets[active].row, (size_t)n * sizeof *out);
    return n;
}

static void unique_name(const char *want, char *out, size_t cap)
{
    const char *base = want && *want ? want : "matrix";

    snprintf(out, cap, "%s", base);
    for (int i = 2; muxcfg_name_taken(out, -1) && i < 100; i++)
        snprintf(out, cap, "%.*s %d", (int)cap - 5, base, i);
}

int muxcfg_install(const char *name, const struct mux_spec *v, int n)
{
    ensure_loaded();

    struct mux_set *s;
    if (nsets < MUX_SETS) {
        s = &sets[nsets++];
    } else {
        s = &sets[active == 0 ? 1 : 0];
    }
    memset(s, 0, sizeof *s);

    char unique[MUX_NAME];
    unique_name(name, unique, sizeof unique);
    snprintf(s->name, sizeof s->name, "%s", unique);

    for (int i = 0; i < n && s->n < MUX_MAX; i++)
        if (*v[i].backend && known_backend(v[i].backend))
            s->row[s->n++] = v[i];

    active = (int)(s - sets);
    save_file();
    return s->n;
}

static const char *short_model(const struct mux_spec *m)
{
    if (!*m->model)
        return "default";
    return models_short_name(m->backend, m->model);
}

void muxcfg_label(const struct mux_spec *m, char *out, size_t cap)
{
    if (*m->effort && strcmp(m->effort, "default"))
        snprintf(out, cap, "%s \xc2\xb7 %s \xc2\xb7 %s", m->backend, short_model(m), m->effort);
    else
        snprintf(out, cap, "%s \xc2\xb7 %s", m->backend, short_model(m));
}

int muxcfg_count(void)
{
    ensure_loaded();
    return nsets;
}

int muxcfg_index(void)
{
    ensure_loaded();
    return active;
}

struct mux_set *muxcfg_at(int i)
{
    ensure_loaded();
    return i >= 0 && i < nsets ? &sets[i] : NULL;
}

void muxcfg_select(int i)
{
    ensure_loaded();
    if (i >= 0 && i < nsets)
        active = i;
}

int muxcfg_name_taken(const char *name, int except)
{
    ensure_loaded();
    for (int i = 0; i < nsets; i++)
        if (i != except && !strcmp(sets[i].name, name))
            return 1;
    return 0;
}

int muxcfg_new(const char *name)
{
    ensure_loaded();
    if (nsets >= MUX_SETS)
        return -1;

    struct mux_set *s = &sets[nsets];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    active = nsets++;
    return active;
}

int muxcfg_drop(int i)
{
    ensure_loaded();
    if (nsets < 2 || i < 0 || i >= nsets)
        return 0;

    memmove(&sets[i], &sets[i + 1], (size_t)(nsets - i - 1) * sizeof *sets);
    nsets--;
    if (active >= nsets)
        active = nsets - 1;
    else if (active > i)
        active--;
    return 1;
}

void muxcfg_save(void)
{
    ensure_loaded();
    save_file();
}
