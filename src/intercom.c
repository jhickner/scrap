#include "intercom.h"

#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "dispatch.h"
#include "files.h"
#include "tailnet.h"
#include "kvlog.h"
#include "livelist.h"
#include "sessionlist.h"
#include "sessionload.h"
#include "text.h"
#include "title.h"
#include "transcript.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define GREP_MAX    (4L * 1024 * 1024)
#define INLINE_MAX  4000
#define REPLY_WAIT  10
#define READ_TURNS  3
#define READ_BYTES  16000

static const char *const PARTS[] = {
    "anvil", "arc", "awl", "axle", "baffle", "ballast", "barrel", "bearing", "bellows", "bolt",
    "boom", "bore", "bracket", "breech", "burr", "bushing", "cable", "cam", "chassis", "chisel",
    "chrome", "cinder", "clamp", "claw", "clevis", "clinker", "clutch", "cog", "coil", "cotter",
    "cowl", "crank", "crowbar", "diode", "dross", "duct", "fender", "filament", "filings",
    "flange", "flechette", "flux", "flywheel", "fuse", "gantry", "gasket", "gear", "girder",
    "grommet", "gusset", "hacksaw", "hinge", "hook", "hose", "hull", "husk", "jack", "jib", "jig",
    "keel", "lead", "lug", "mallet", "manifold", "mast", "muzzle", "nozzle", "nut", "pawl",
    "pellet", "pinion", "piston", "pivot", "plate", "plug", "prong", "pulley", "pump", "rasp",
    "ratchet", "rebar", "rib", "rivet", "rocker", "rust", "scale", "screw", "seal", "servo",
    "shard", "shim", "shiv", "shrapnel", "slag", "sliver", "slug", "socket", "solder", "solenoid",
    "soot", "spar", "spark", "spike", "spindle", "sprocket", "staple", "strap", "strut", "stud",
    "swarf", "tack", "talon", "tappet", "tin", "tine", "tongs", "torch", "truss", "tube", "valve",
    "vent", "vise", "visor", "washer", "weld", "winch", "wire", "wrench", "yoke", "zinc",
};

#define COUNT(a) (sizeof(a) / sizeof(a)[0])

static int registry_path(char *out, size_t size)
{
    return path_config_file(out, size, "names");
}

static void field_at(const char *row, int index, char *out, size_t size)
{
    for (int i = 0; i < index && row; i++) {
        row = strchr(row, '\t');
        if (row)
            row++;
    }
    size_t n = row ? strcspn(row, "\t") : 0;
    snprintf(out, size, "%.*s", (int)n, row ? row : "");
}

static const char *route(const char *target, char *host, size_t size, const char **local)
{
    const char *name = tailnet_split(target, host, size);
    *local = target;
    if (name && dispatch_net_port() && tailnet_is_self(host)) {
        *local = name;
        return NULL;
    }
    return name;
}

int intercom_name_valid(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= INTERCOM_NAME_MAX)
        return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return 0;
    return 1;
}

int intercom_name_taken(const char *name, const char *id)
{
    char path[4200];
    if (registry_path(path, sizeof path)) {
        struct kvlog_map *m = kvlog_fresh(path);
        for (int i = 0; i < m->n; i++) {
            char have[INTERCOM_NAME_MAX];
            field_at(m->ents[i].val, 0, have, sizeof have);
            if (!strcmp(have, name) && (!id || strcmp(m->ents[i].key, id)))
                return 1;
        }
    }
    struct live_session *live = NULL;
    int                  n = livelist_load(&live);
    int                  taken = 0;
    for (int i = 0; i < n && !taken; i++)
        taken = !strcmp(live[i].name, name) && (!id || strcmp(live[i].id, id));
    free(live);
    return taken;
}

void intercom_name_new(char *out, size_t size)
{
    for (int tries = 0; tries < 256; tries++) {
        const char *part = PARTS[arc4random_uniform(COUNT(PARTS))];
        if (tries < 64)
            snprintf(out, size, "%s", part);
        else
            snprintf(out, size, "%s-%d", part, 2 + (tries - 64) / 16);
        if (!intercom_name_taken(out, NULL))
            return;
    }
    snprintf(out, size, "scrap-%u", arc4random_uniform(100000));
}

static int suffix_of(const char *name, const char *base, int n)
{
    if (strncmp(name, base, (size_t)n))
        return -1;
    const char *p = name + n;
    if (!*p)
        return 1;
    for (const char *q = p; *q; q++)
        if (!isdigit((unsigned char)*q))
            return -1;
    return atoi(p);
}

void intercom_name_next(const char *base, char *out, size_t size)
{
    int n = (int)strlen(base);
    while (n > 0 && isdigit((unsigned char)base[n - 1]))
        n--;

    int  top = 1;
    char path[4200], have[INTERCOM_NAME_MAX];
    if (registry_path(path, sizeof path)) {
        struct kvlog_map *m = kvlog_fresh(path);
        for (int i = 0; i < m->n; i++) {
            field_at(m->ents[i].val, 0, have, sizeof have);
            int k = suffix_of(have, base, n);
            if (k > top)
                top = k;
        }
    }
    struct live_session *live = NULL;
    int                  count = livelist_load(&live);
    for (int i = 0; i < count; i++) {
        int k = suffix_of(live[i].name, base, n);
        if (k > top)
            top = k;
    }
    free(live);

    for (int i = top + 1; i < top + 100; i++) {
        snprintf(out, size, "%.*s%d", n, base, i);
        if (intercom_name_valid(out) && !intercom_name_taken(out, NULL))
            return;
    }
    intercom_name_new(out, size);
}

int intercom_name_of(const char *id, char *out, size_t size)
{
    char path[4200], row[8400];
    if (!id || !*id || !registry_path(path, sizeof path) ||
        !kvlog_lookup(path, id, row, sizeof row))
        return 0;
    field_at(row, 0, out, size);
    return out[0] != '\0';
}

void intercom_register(const char *id, const char *name, const char *backend,
                       const char *cwd)
{
    char path[4200], row[8400];
    if (!id || !*id || !intercom_name_valid(name) || !registry_path(path, sizeof path))
        return;
    snprintf(row, sizeof row, "%s\t%s\t%s", name, backend ? backend : "", cwd ? cwd : "");
    char have[8400];
    if (kvlog_lookup(path, id, have, sizeof have) && !strcmp(have, row))
        return;
    kvlog_append(path, id, row);
}

char *intercom_note(const char *name)
{
    if (!name || !*name)
        return NULL;
    return text_dsprintf(
        "This session is @%s in scrap. Other scrap sessions are reachable with the `scrap` "
        "command, run through Bash:\n"
        "- `scrap ls [--live] [--cwd DIR] [QUERY]` lists sessions, newest first; `--cwd .` "
        "means this directory; QUERY also searches transcript text.\n"
        "- `scrap read TARGET [-n TURNS] [--bytes N]` prints a session's recent turns.\n"
        "- `scrap send TARGET TEXT` sends a message to a live session.\n"
        "- `scrap open TARGET` resumes a past session in a new tab.\n"
        "- `scrap ls --net` lists live sessions on every machine on the tailnet.\n"
        "TARGET is @name, a session id prefix, or a title; machine:@name reaches a session "
        "on another tailnet machine through `send` and `read`. Messages from other "
        "sessions arrive prefixed `[from @name]` or `[from machine:@name]`; answer them "
        "with `scrap send` to that exact address only when an answer is needed.\n"
        "To coordinate with a live session: `scrap ls --live --cwd .`, then `scrap read "
        "@name`, then `scrap send @name ...`. To recover old context: `scrap ls QUERY`, then "
        "`scrap read TARGET`.",
        name);
}

int intercom_complete(void *ctx, const char *token, ReplCandidate *out, int max)
{
    int n = 0;
    if (!strpbrk(token, "/.")) {
        struct live_session *live = NULL;
        int                  count = livelist_load(&live);
        for (int i = 0; i < count && n < max; i++) {
            if (!live[i].name[0] || text_fuzzy_score(live[i].name, token) < 0)
                continue;
            snprintf(out[n].text, REPL_CAND_TEXT, "%s", live[i].name);
            snprintf(out[n].desc, REPL_CAND_DESC, "%s", live[i].title);
            n++;
        }
        free(live);
    }
    int files = n < max ? files_complete(ctx, token, out + n, max - n) : 0;
    return n + (files > 0 ? files : 0);
}

struct entry {
    char name[INTERCOM_NAME_MAX];
    char id[128];
    char backend[32];
    char cwd[1024];
    char title[200];
    char status[16];
    long ts;
    long pid;
    int  live;
};

struct entries {
    struct entry *e;
    int           n, cap;
};

static struct entry *find_id(struct entries *l, const char *id)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].id, id))
            return &l->e[i];
    return NULL;
}

static void fill(char *dst, size_t size, const char *src)
{
    if (!dst[0] && src && *src)
        snprintf(dst, size, "%s", src);
}

static void add(struct entries *l, const struct entry *e)
{
    if (!e->id[0] && !e->live)
        return;
    struct entry *have = e->id[0] ? find_id(l, e->id) : NULL;
    if (have) {
        fill(have->name, sizeof have->name, e->name);
        fill(have->title, sizeof have->title, e->title);
        fill(have->backend, sizeof have->backend, e->backend);
        fill(have->cwd, sizeof have->cwd, e->cwd);
        return;
    }
    if (l->n == l->cap) {
        int           cap = l->cap ? l->cap * 2 : 64;
        struct entry *grown = realloc(l->e, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        l->e = grown;
        l->cap = cap;
    }
    l->e[l->n++] = *e;
}

static void add_records(struct entries *l, const struct live_session *v, int n, int live)
{
    for (int i = 0; i < n; i++) {
        struct entry e = {0};
        snprintf(e.name, sizeof e.name, "%s", v[i].name);
        snprintf(e.id, sizeof e.id, "%s", v[i].id);
        snprintf(e.backend, sizeof e.backend, "%s", v[i].backend);
        snprintf(e.cwd, sizeof e.cwd, "%s", v[i].cwd);
        snprintf(e.title, sizeof e.title, "%s", v[i].title);
        snprintf(e.status, sizeof e.status, "%s", live ? v[i].status : "closed");
        e.ts = v[i].ts;
        e.pid = v[i].pid;
        e.live = live;
        add(l, &e);
    }
}

static long transcript_time(const struct entry *e)
{
    char        path[4096];
    struct stat st;
    if (sessionload_path(e->backend, e->cwd, e->id, path, sizeof path) &&
        stat(path, &st) == 0)
        return (long)st.st_mtime;
    return 0;
}

static int newest(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->live != y->live)
        return y->live - x->live;
    return x->ts < y->ts ? 1 : x->ts > y->ts ? -1 : 0;
}

static void add_past(struct entries *l, const char *cwd)
{
    for (const char *const *b = backend_names(); *b; b++) {
        if (!sessionlist_available(*b))
            continue;
        struct past_session *past = NULL;
        int                  count = sessionlist_load(*b, cwd, NULL, &past);
        for (int i = 0; i < count; i++) {
            struct entry e = {0};
            snprintf(e.id, sizeof e.id, "%s", past[i].id);
            snprintf(e.backend, sizeof e.backend, "%s", *b);
            snprintf(e.cwd, sizeof e.cwd, "%s", cwd);
            snprintf(e.title, sizeof e.title, "%s", past[i].label);
            snprintf(e.status, sizeof e.status, "closed");
            e.ts = (long)past[i].modified;
            add(l, &e);
        }
        free(past);
    }
}

static void collect(struct entries *l, const char *cwd, int live_only, int up)
{
    struct live_session *v = NULL;
    int                  n = livelist_load(&v);
    add_records(l, v, n, 1);
    free(v);
    if (live_only)
        return;

    n = livelist_closed_load(&v);
    add_records(l, v, n, 0);
    free(v);

    char path[4200];
    if (registry_path(path, sizeof path)) {
        struct kvlog_map *m = kvlog_fresh(path);
        for (int i = m->n - 1; i >= 0; i--) {
            struct entry e = {0};
            snprintf(e.id, sizeof e.id, "%s", m->ents[i].key);
            field_at(m->ents[i].val, 0, e.name, sizeof e.name);
            field_at(m->ents[i].val, 1, e.backend, sizeof e.backend);
            field_at(m->ents[i].val, 2, e.cwd, sizeof e.cwd);
            snprintf(e.status, sizeof e.status, "closed");
            e.ts = transcript_time(&e);
            add(l, &e);
        }
    }

    char dir[4096];
    snprintf(dir, sizeof dir, "%s", cwd ? cwd : "");
    while (dir[0]) {
        add_past(l, dir);
        char *slash = strrchr(dir, '/');
        if (!up || !slash || slash == dir)
            break;
        *slash = '\0';
    }

    for (int i = 0; i < l->n; i++)
        if (!l->e[i].title[0] && l->e[i].id[0])
            title_lookup(l->e[i].id, l->e[i].title, sizeof l->e[i].title);
    qsort(l->e, (size_t)l->n, sizeof *l->e, newest);
}

static int contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return 1;
    return !n;
}

static int transcript_has(const struct entry *e, const char *query)
{
    char path[4096];
    if (!e->id[0] || !sessionload_path(e->backend, e->cwd, e->id, path, sizeof path))
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char *buf = malloc(GREP_MAX + 1);
    size_t got = buf ? fread(buf, 1, GREP_MAX, f) : 0;
    fclose(f);
    if (!buf)
        return 0;
    buf[got] = '\0';
    int found = contains(buf, query);
    free(buf);
    return found;
}

static int under(const char *cwd, const char *dir)
{
    size_t n = strlen(dir);
    if (n > 1 && dir[n - 1] == '/')
        n--;
    return !strncmp(cwd, dir, n) && (cwd[n] == '\0' || cwd[n] == '/');
}

static const struct entry *resolve(struct entries *l, const char *target)
{
    const char *name = target[0] == '@' ? target + 1 : target;
    for (int i = 0; i < l->n; i++)
        if (l->e[i].name[0] && !strcmp(l->e[i].name, name))
            return &l->e[i];
    if (target[0] == '@')
        return NULL;
    size_t n = strlen(target);
    for (int i = 0; i < l->n; i++)
        if (!strncmp(l->e[i].id, target, n))
            return &l->e[i];
    for (int i = 0; i < l->n; i++)
        if (contains(l->e[i].title, target))
            return &l->e[i];
    return NULL;
}

static void print_entry(FILE *out, const struct entry *e)
{
    char who[INTERCOM_NAME_MAX + 2], where[1024];
    if (e->name[0])
        snprintf(who, sizeof who, "@%s", e->name);
    else
        snprintf(who, sizeof who, "%.8s", e->id);
    path_home_relative(e->cwd, where, sizeof where);
    fprintf(out, "%-20s %-4s %-8s %s  %s\n", who, e->live ? "live" : "past",
           e->status[0] ? e->status : "-", where, e->title[0] ? e->title : "untitled");
}

static int here(char *out, size_t size)
{
    return getcwd(out, size) != NULL;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

char *intercom_net_list(const char *query)
{
    cJSON *machines = tailnet_survey();
    if (!machines)
        return NULL;
    char  *text = NULL;
    size_t len = 0;
    FILE  *out = open_memstream(&text, &len);
    cJSON *m;
    cJSON_ArrayForEach(m, machines)
    {
        const char *error = cJSON_GetStringValue(cJSON_GetObjectItem(m, "error"));
        if (error) {
            if (!query && out)
                fprintf(out, "%s: %s\n", jstr(m, "machine"), error);
            continue;
        }
        int    shown = 0;
        cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(m, "sessions"))
        {
            struct entry e = {.live = 1};
            snprintf(e.name, sizeof e.name, "%s", jstr(o, "name"));
            snprintf(e.id, sizeof e.id, "%s", jstr(o, "id"));
            snprintf(e.cwd, sizeof e.cwd, "%s", jstr(o, "cwd"));
            snprintf(e.title, sizeof e.title, "%s", jstr(o, "title"));
            snprintf(e.status, sizeof e.status, "%s", jstr(o, "status"));
            if (!out || (query && !contains(e.name, query) && !contains(e.title, query) &&
                         !contains(e.cwd, query)))
                continue;
            if (!shown++)
                fprintf(out, "%s\n", jstr(m, "machine"));
            fprintf(out, "  ");
            print_entry(out, &e);
        }
        if (!shown && !query && out)
            fprintf(out, "%s: no live sessions\n", jstr(m, "machine"));
    }
    if (out)
        fclose(out);
    cJSON_Delete(machines);
    return text ? text : strdup("");
}

static int cmd_ls_net(const char *query)
{
    char *text = intercom_net_list(query);
    if (!text) {
        fprintf(stderr, "scrap: tailscale status is unavailable\n");
        return 1;
    }
    fputs(text, stdout);
    free(text);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    int         live_only = 0, net = 0;
    const char *dir = NULL, *query = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--live"))
            live_only = 1;
        else if (!strcmp(argv[i], "--net"))
            net = 1;
        else if (!strcmp(argv[i], "--cwd") && i + 1 < argc)
            dir = argv[++i];
        else if (!query && argv[i][0] != '-')
            query = argv[i];
        else {
            fprintf(stderr, "usage: scrap ls [--live] [--net] [--cwd DIR] [QUERY]\n");
            return 2;
        }
    }
    if (net)
        return cmd_ls_net(query);

    char cwd[4096], real[4096];
    if (dir) {
        if (!realpath(dir, real)) {
            fprintf(stderr, "scrap: no such directory: %s\n", dir);
            return 1;
        }
        dir = real;
    }
    const char *hint = dir ? dir : here(cwd, sizeof cwd) ? cwd : NULL;

    struct entries l = {0};
    collect(&l, hint, live_only, 0);
    for (int i = 0; i < l.n; i++) {
        const struct entry *e = &l.e[i];
        if (dir && !under(e->cwd, dir))
            continue;
        if (query && !contains(e->name, query) && !contains(e->title, query) &&
            !contains(e->cwd, query) && strncmp(e->id, query, strlen(query)) &&
            !transcript_has(e, query))
            continue;
        print_entry(stdout, e);
    }
    free(l.e);
    return 0;
}

static const struct entry *lookup(struct entries *l, const char *target)
{
    char cwd[4096];
    collect(l, here(cwd, sizeof cwd) ? cwd : NULL, 0, 1);
    const struct entry *e = resolve(l, target);
    if (!e)
        fprintf(stderr, "scrap: no session matches %s\n", target);
    return e;
}

char *intercom_read(const char *target, long turns, long bytes, char *msg, size_t size)
{
    struct entries l = {0};
    char           cwd[4096];
    collect(&l, here(cwd, sizeof cwd) ? cwd : NULL, 0, 1);
    const struct entry *e = resolve(&l, target);
    struct transcript   t = {0};
    char               *out = NULL;
    size_t              len = 0;
    if (!e)
        snprintf(msg, size, "no session matches %s", target);
    else if (!sessionload_fill(&t, e->backend, e->cwd, e->id) || !t.count)
        snprintf(msg, size, "no transcript for %s", target);
    else {
        size_t            first = t.count > (size_t)turns ? t.count - (size_t)turns : 0;
        struct transcript tail = {t.turns + first, t.count - first, t.count - first};
        char             *text = transcript_handoff(&tail, (size_t)bytes, NULL);
        const char       *body = text ? strstr(text, "\n\n") : NULL;
        FILE             *f = open_memstream(&out, &len);
        if (f) {
            print_entry(f, e);
            fprintf(f, "id %s, last %zu of %zu turns\n\n%s", e->id, tail.count, t.count,
                    body ? body + 2 : "");
            fclose(f);
        }
        free(text);
        if (!out)
            snprintf(msg, size, "out of memory");
    }
    transcript_free(&t);
    free(l.e);
    return out;
}

static int cmd_read(int argc, char **argv)
{
    const char *target = NULL;
    long        turns = READ_TURNS, bytes = READ_BYTES;
    int         bad = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            turns = atol(argv[++i]);
        else if (!strcmp(argv[i], "--bytes") && i + 1 < argc)
            bytes = atol(argv[++i]);
        else if (!target)
            target = argv[i];
        else
            bad = 1;
    }
    if (bad || !target || turns < 1 || bytes < 1) {
        fprintf(stderr, "usage: scrap read TARGET [-n TURNS] [--bytes N]\n");
        return 2;
    }

    char        host[TAILNET_HOST_MAX], msg[1200];
    const char *local, *name = route(target, host, sizeof host, &local);
    char       *text = name ? tailnet_read(host, name, turns, bytes, msg, sizeof msg)
                            : intercom_read(local, turns, bytes, msg, sizeof msg);
    if (!text) {
        fprintf(stderr, "scrap: %s\n", msg);
        return 1;
    }
    fputs(text, stdout);
    free(text);
    return 0;
}

char *intercom_serve(const cJSON *o)
{
    cJSON *r = NULL;
    char   msg[1200];
    if (cJSON_GetObjectItem((cJSON *)o, "ls")) {
        r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "machine", tailnet_self_name());
        cJSON_AddItemToObject(r, "sessions", intercom_live_json());
    } else if (cJSON_IsString(cJSON_GetObjectItem((cJSON *)o, "read"))) {
        cJSON *n = cJSON_GetObjectItem((cJSON *)o, "n"), *b = cJSON_GetObjectItem((cJSON *)o, "bytes");
        long   turns = cJSON_IsNumber(n) ? (long)n->valuedouble : READ_TURNS;
        long   bytes = cJSON_IsNumber(b) ? (long)b->valuedouble : READ_BYTES;
        char  *text = intercom_read(cJSON_GetObjectItem((cJSON *)o, "read")->valuestring,
                                    turns < 1 ? 1 : turns, bytes < 1 ? 1 : bytes, msg, sizeof msg);
        r = cJSON_CreateObject();
        if (text)
            cJSON_AddStringToObject(r, "text", text);
        else
            cJSON_AddStringToObject(r, "error", msg);
        free(text);
    }
    char *out = r ? cJSON_PrintUnformatted(r) : NULL;
    cJSON_Delete(r);
    return out;
}

cJSON *intercom_live_json(void)
{
    struct live_session *v = NULL;
    int                  n = livelist_load(&v);
    cJSON               *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", v[i].name);
        cJSON_AddStringToObject(o, "id", v[i].id);
        cJSON_AddStringToObject(o, "backend", v[i].backend);
        cJSON_AddStringToObject(o, "cwd", v[i].cwd);
        cJSON_AddStringToObject(o, "title", v[i].title);
        cJSON_AddStringToObject(o, "status", v[i].status);
        cJSON_AddNumberToObject(o, "ts", (double)v[i].ts);
        cJSON_AddNumberToObject(o, "port", v[i].port);
        cJSON_AddItemToArray(a, o);
    }
    free(v);
    return a;
}

static int request(long pid, cJSON *body, char *reply, size_t size)
{
    char *json = cJSON_PrintUnformatted(body);
    if (!json)
        return 0;
    dispatch_request(pid, json, reply, size, REPLY_WAIT);
    free(json);
    return 1;
}

static int answered(const char *reply, const char *done)
{
    cJSON      *o = cJSON_Parse(reply);
    const char *error = cJSON_GetStringValue(cJSON_GetObjectItem(o, "error"));
    if (error)
        fprintf(stderr, "scrap: %s\n", error);
    else
        printf("%s\n", done);
    cJSON_Delete(o);
    return error ? 1 : 0;
}

static void self_name(char *out, size_t size)
{
    out[0] = '\0';
    const char *file = getenv("MUX_SESSION_FILE");
    char       *id = file && *file ? text_slurp(file, 4096, NULL) : NULL;
    if (id) {
        text_chomp(id);
        intercom_name_of(id, out, size);
    }
    free(id);
}

static char *spill(const char *text)
{
    char dir[4200], path[4400];
    if (!path_config_subdir(dir, sizeof dir, "intercom"))
        return NULL;
    snprintf(path, sizeof path, "%s/%ld-%08x.txt", dir, (long)time(NULL), arc4random());
    FILE *f = fopen(path, "w");
    if (!f)
        return NULL;
    fputs(text, f);
    if (fclose(f) != 0)
        return NULL;
    return text_dsprintf("The message is %zu bytes, stored in %s; read that file.",
                         strlen(text), path);
}

int intercom_deliver(const char *host, const char *from, const char *target, const char *text,
                     char *msg, size_t size)
{
    struct entries l = {0};
    char           cwd[4096];
    collect(&l, here(cwd, sizeof cwd) ? cwd : NULL, 0, 1);
    const struct entry *e = resolve(&l, target);
    int                 rc = 1;
    if (!e)
        snprintf(msg, size, "no session matches %s", target);
    else if (!e->live)
        snprintf(msg, size, "%s is not live; resume it with `scrap open %s`", target, target);
    else {
        char  *long_text = strlen(text) > INLINE_MAX ? spill(text) : NULL;
        char   reply[1024] = "";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "send", long_text ? long_text : text);
        cJSON_AddStringToObject(o, "session", e->id);
        if (e->name[0])
            cJSON_AddStringToObject(o, "name", e->name);
        if (from && *from)
            cJSON_AddStringToObject(o, "from", from);
        if (host && *host)
            cJSON_AddStringToObject(o, "host", host);
        if (request(e->pid, o, reply, sizeof reply)) {
            cJSON      *r = cJSON_Parse(reply);
            const char *error = cJSON_GetStringValue(cJSON_GetObjectItem(r, "error"));
            if (error)
                snprintf(msg, size, "%s", error);
            else
                snprintf(msg, size, "sent to @%s", e->name[0] ? e->name : e->id);
            rc = error ? 1 : 0;
            cJSON_Delete(r);
        } else
            snprintf(msg, size, "could not write the request");
        cJSON_Delete(o);
        free(long_text);
    }
    free(l.e);
    return rc;
}

int intercom_send(const char *from, const char *target, const char *text, char *msg,
                  size_t size)
{
    char        host[TAILNET_HOST_MAX];
    const char *local, *name = route(target, host, sizeof host, &local);
    if (name)
        return tailnet_send(host, from, name, text, msg, size);
    return intercom_deliver(NULL, from, local, text, msg, size);
}

static int cmd_send(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: scrap send TARGET TEXT\n");
        return 2;
    }
    size_t len = 0;
    for (int i = 2; i < argc; i++)
        len += strlen(argv[i]) + 1;
    char *text = calloc(1, len + 1);
    if (!text)
        return 1;
    for (int i = 2; i < argc; i++) {
        if (i > 2)
            strcat(text, " ");
        strcat(text, argv[i]);
    }

    char me[INTERCOM_NAME_MAX], msg[1200];
    self_name(me, sizeof me);
    int rc = intercom_send(me, argv[1], text, msg, sizeof msg);
    if (rc)
        fprintf(stderr, "scrap: %s\n", msg);
    else
        printf("%s\n", msg);
    free(text);
    return rc;
}

static int cmd_open(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: scrap open TARGET\n");
        return 2;
    }
    const char *owner = getenv("SCRAP_PID");
    long        pid = owner ? atol(owner) : 0;
    if (pid <= 0 || !livelist_alive(pid)) {
        fprintf(stderr, "scrap: scrap open runs inside a scrap session\n");
        return 1;
    }

    struct entries      l = {0};
    const struct entry *e = lookup(&l, argv[1]);
    int                 rc = 1;
    if (e && e->live)
        fprintf(stderr, "scrap: %s is already live\n", argv[1]);
    else if (e) {
        char   reply[1024] = "";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "backend", e->backend);
        cJSON_AddStringToObject(o, "cwd", e->cwd);
        cJSON_AddStringToObject(o, "resume", e->id);
        char done[200];
        snprintf(done, sizeof done, "opened %s in a new tab", argv[1]);
        rc = request(pid, o, reply, sizeof reply) ? answered(reply, done) : 1;
        cJSON_Delete(o);
    }
    free(l.e);
    return rc;
}

static int connect_unix(long pid)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (!dispatch_socket_path(pid, sa.sun_path, sizeof sa.sun_path))
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

int intercom_attach(const char *target, char *msg, size_t size)
{
    char        host[TAILNET_HOST_MAX], id[128] = "", name[INTERCOM_NAME_MAX] = "";
    const char *local, *remote = route(target, host, sizeof host, &local);
    int         fd = -1;
    if (remote) {
        char ip[256], err[512];
        int  port;
        if (!tailnet_locate(host, remote, ip, sizeof ip, &port, id, sizeof id, msg, size))
            return -1;
        fd = tailnet_connect(ip, port, 3, err, sizeof err);
        if (fd < 0)
            snprintf(msg, size, "%s: %s", host, err);
        if (remote[0] == '@')
            snprintf(name, sizeof name, "%s", remote + 1);
    } else {
        struct entries      l = {0};
        char                cwd[4096];
        collect(&l, here(cwd, sizeof cwd) ? cwd : NULL, 1, 0);
        const struct entry *e = resolve(&l, local);
        if (!e)
            snprintf(msg, size, "no live session matches %s", local);
        else if (e->pid == (long)getpid())
            snprintf(msg, size, "%s is a tab in this window", local);
        else {
            snprintf(id, sizeof id, "%s", e->id);
            snprintf(name, sizeof name, "%s", e->name);
            fd = connect_unix(e->pid);
            if (fd < 0)
                snprintf(msg, size, "scrap %ld is not running", e->pid);
        }
        free(l.e);
    }
    if (fd < 0)
        return -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "attach", id);
    if (name[0])
        cJSON_AddStringToObject(o, "name", name);
    char  *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    size_t len = json ? strlen(json) : 0;
    int    ok = json && send(fd, json, len, 0) == (ssize_t)len && send(fd, "\n", 1, 0) == 1;
    free(json);
    if (!ok) {
        snprintf(msg, size, "could not write the attach request");
        close(fd);
        return -1;
    }
    return fd;
}

static int cmd_attach(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: scrap attach TARGET\n");
        return 2;
    }
    char msg[1200];
    int  fd = intercom_attach(argv[1], msg, sizeof msg);
    if (fd < 0) {
        fprintf(stderr, "scrap: %s\n", msg);
        return 1;
    }
    char  *typed = NULL;
    size_t tcap = 0;
    int    stdin_open = 1, started = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = 0, .events = POLLIN}};
        if (poll(p, stdin_open && started ? 2 : 1, -1) < 0)
            break;
        if (p[0].revents) {
            char    chunk[8192];
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0 || fwrite(chunk, 1, (size_t)n, stdout) != (size_t)n)
                break;
            fflush(stdout);
            started = started || memchr(chunk, '\n', (size_t)n);
            continue;
        }
        if (stdin_open && p[1].revents) {
            ssize_t n = getline(&typed, &tcap, stdin);
            if (n <= 0) {
                stdin_open = 0;
                continue;
            }
            text_chomp(typed);
            cJSON *o = cJSON_CreateObject();
            if (!strcmp(typed, "!interrupt"))
                cJSON_AddBoolToObject(o, "interrupt", 1);
            else
                cJSON_AddStringToObject(o, "prompt", typed);
            char *json = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (json && (write(fd, json, strlen(json)) < 0 || write(fd, "\n", 1) < 0)) {
                free(json);
                break;
            }
            free(json);
        }
    }
    free(typed);
    close(fd);
    return 0;
}

int intercom_main(int argc, char **argv)
{
    if (!strcmp(argv[0], "ls"))
        return cmd_ls(argc, argv);
    if (!strcmp(argv[0], "read"))
        return cmd_read(argc, argv);
    if (!strcmp(argv[0], "send"))
        return cmd_send(argc, argv);
    if (!strcmp(argv[0], "open"))
        return cmd_open(argc, argv);
    if (!strcmp(argv[0], "attach"))
        return cmd_attach(argc, argv);
    return -1;
}
