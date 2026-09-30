#include "chain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvlog.h"
#include "text.h"

static struct kvlog_map *store(void)
{
    char path[4200];
    return path_config_file(path, sizeof path, "chains") ? kvlog_fresh(path) : NULL;
}

static const char *field(const char *p, char *out, size_t size)
{
    size_t n = strcspn(p, "\t");
    snprintf(out, size, "%.*s", (int)n, p);
    return p[n] ? p + n + 1 : p + n;
}

static void parse(const char *val, struct chain_record *r)
{
    char cut[16];
    memset(r, 0, sizeof *r);
    const char *p = field(val, r->name, sizeof r->name);
    p = field(p, cut, sizeof cut);
    r->cut = atoi(cut);
    for (const char *q = p; *q;) {
        struct chain_segment s;
        q = field(q, s.backend, sizeof s.backend);
        q = field(q, s.cwd, sizeof s.cwd);
        q = field(q, s.id, sizeof s.id);
        if (!s.id[0])
            break;
        struct chain_segment *grown = realloc(r->seg, (size_t)(r->n + 1) * sizeof *grown);
        if (!grown)
            break;
        r->seg = grown;
        r->seg[r->n++] = s;
    }
    if (r->cut > r->n)
        r->cut = r->n;
}

static int position(const struct chain_record *r, const char *id)
{
    for (int k = 0; id && k < r->n; k++)
        if (!strcmp(r->seg[k].id, id))
            return k;
    return -1;
}

static void write_record(const char *chain, const struct chain_record *r)
{
    char path[4200];
    char *val = text_dsprintf("%s\t%d", r->name, r->cut);
    for (int k = 0; val && k < r->n; k++) {
        char *next = text_dsprintf("%s\t%s\t%s\t%s", val, r->seg[k].backend, r->seg[k].cwd,
                                   r->seg[k].id);
        free(val);
        val = next;
    }
    if (val && path_config_file(path, sizeof path, "chains"))
        kvlog_append(path, chain, val);
    free(val);
}

void chain_new(char *out, size_t size)
{
    snprintf(out, size, "%08x%08x", arc4random(), arc4random());
}

int chain_read(const char *chain, struct chain_record *r)
{
    memset(r, 0, sizeof *r);
    struct kvlog_map *m = chain && *chain ? store() : NULL;
    for (int i = 0; m && i < m->n; i++)
        if (!strcmp(m->ents[i].key, chain)) {
            parse(m->ents[i].val, r);
            return 1;
        }
    return 0;
}

void chain_free(struct chain_record *r)
{
    free(r->seg);
    r->seg = NULL;
    r->n = 0;
}

void chain_scan(chain_scan_fn fn, void *ctx)
{
    struct kvlog_map *m = store();
    for (int i = m ? m->n - 1 : -1; i >= 0; i--) {
        struct chain_record r;
        parse(m->ents[i].val, &r);
        fn(m->ents[i].key, &r, ctx);
        chain_free(&r);
    }
}

int chain_find(const char *id, char *out, size_t size)
{
    struct kvlog_map *m = id && *id ? store() : NULL;
    int               found = -1;
    for (int i = 0; m && i < m->n; i++) {
        if (!strstr(m->ents[i].val, id))
            continue;
        struct chain_record r;
        parse(m->ents[i].val, &r);
        int at = position(&r, id), last = at >= 0 && at == r.n - 1;
        chain_free(&r);
        if (at >= 0 && (last || found < 0))
            found = i;
        if (last)
            break;
    }
    if (found < 0)
        return 0;
    snprintf(out, size, "%s", m->ents[found].key);
    return 1;
}

int chain_named(const char *name, char *out, size_t size)
{
    struct kvlog_map *m = name && *name ? store() : NULL;
    for (int i = m ? m->n - 1 : -1; i >= 0; i--) {
        char have[CHAIN_NAME_MAX];
        field(m->ents[i].val, have, sizeof have);
        if (!strcmp(have, name)) {
            snprintf(out, size, "%s", m->ents[i].key);
            return 1;
        }
    }
    return 0;
}

void chain_add(const char *chain, const char *name, const char *backend, const char *cwd,
               const char *id)
{
    if (!chain || !*chain || !id || !*id)
        return;
    struct chain_record r;
    chain_read(chain, &r);
    int changed = name && strcmp(r.name, name);
    if (changed)
        snprintf(r.name, sizeof r.name, "%s", name);
    struct chain_segment *grown =
        position(&r, id) < 0 ? realloc(r.seg, (size_t)(r.n + 1) * sizeof *grown) : NULL;
    if (grown) {
        r.seg = grown;
        struct chain_segment *s = &r.seg[r.n++];
        snprintf(s->backend, sizeof s->backend, "%s", backend ? backend : "");
        snprintf(s->cwd, sizeof s->cwd, "%s", cwd ? cwd : "");
        snprintf(s->id, sizeof s->id, "%s", id);
        changed = 1;
    }
    if (changed)
        write_record(chain, &r);
    chain_free(&r);
}

void chain_set_name(const char *chain, const char *name)
{
    struct chain_record r;
    if (!name || !chain_read(chain, &r))
        return;
    if (strcmp(r.name, name)) {
        snprintf(r.name, sizeof r.name, "%s", name);
        write_record(chain, &r);
    }
    chain_free(&r);
}

void chain_cut(const char *chain, const char *id)
{
    struct chain_record r;
    if (!chain_read(chain, &r))
        return;
    int at = position(&r, id);
    r.cut = at >= 0 ? at : r.n;
    write_record(chain, &r);
    chain_free(&r);
}

int chain_before(const char *chain, const char *id, struct chain_segment **out)
{
    struct chain_record r;
    *out = NULL;
    chain_read(chain, &r);
    int end = id ? position(&r, id) : r.n;
    int n = end > r.cut ? end - r.cut : 0;
    if (n && (*out = malloc((size_t)n * sizeof **out)))
        memcpy(*out, r.seg + r.cut, (size_t)n * sizeof **out);
    else
        n = 0;
    chain_free(&r);
    return n;
}

void chain_copy(const char *from, const char *until, const char *to, const char *name)
{
    struct chain_record r = {0};
    snprintf(r.name, sizeof r.name, "%s", name ? name : "");
    r.n = chain_before(from, until, &r.seg);
    write_record(to, &r);
    chain_free(&r);
}
