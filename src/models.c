#include "models.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pick.h"
#include "text.h"
#include "vendor/cJSON.h"

#define LABEL_BYTES  96
#define DETAIL_BYTES 96

struct list {
    char              backend[32];
    time_t            stamp;
    int               n, cap;
    struct pick_item *items;
    char            (*label)[LABEL_BYTES];
    char            (*detail)[DETAIL_BYTES];
};

static struct list cache[6];

static int grow(struct list *l)
{
    int cap = l->cap ? l->cap * 2 : 32;

    struct pick_item *items = realloc(l->items, (size_t)cap * sizeof *items);
    if (items)
        l->items = items;
    char (*label)[LABEL_BYTES] = realloc(l->label, (size_t)cap * sizeof *l->label);
    if (label)
        l->label = label;
    char (*detail)[DETAIL_BYTES] = realloc(l->detail, (size_t)cap * sizeof *l->detail);
    if (detail)
        l->detail = detail;
    if (!items || !label || !detail)
        return 0;

    l->cap = cap;
    for (int i = 0; i < l->n; i++)
        l->items[i] = (struct pick_item){l->label[i], l->detail[i]};
    return 1;
}

static void push(struct list *l, const char *label, const char *detail)
{
    if (!label || !*label)
        return;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->label[i], label))
            return;
    if (l->n == l->cap && !grow(l))
        return;

    int at = l->n++;
    snprintf(l->label[at], LABEL_BYTES, "%s", label);
    snprintf(l->detail[at], DETAIL_BYTES, "%s", detail ? detail : "");
    l->items[at] = (struct pick_item){l->label[at], l->detail[at]};
}

static char *home_slurp(const char *rest)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return NULL;

    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home, rest);
    return text_slurp(path, 1 << 22, NULL);
}

static time_t home_stamp(const char *rest)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;

    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home, rest);
    struct stat st;
    return stat(path, &st) ? 0 : st.st_mtime;
}

static int env_set(const char *name)
{
    const char *v = getenv(name);
    return v && *v;
}

/* google and huggingface use names that are not PROVIDER_API_KEY. */
static int pi_env_name(const char *provider, char *out, size_t cap)
{
    if (!provider || !*provider || !out || cap < 12)
        return 0;
    if (!strcmp(provider, "google"))
        return snprintf(out, cap, "GEMINI_API_KEY") < (int)cap;
    if (!strcmp(provider, "huggingface"))
        return snprintf(out, cap, "HF_TOKEN") < (int)cap;

    size_t n = 0;
    for (const char *p = provider; *p && n + 9 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        out[n++] = c == '-' ? '_' : (char)toupper(c);
    }
    out[n] = '\0';
    return snprintf(out + n, cap - n, "_API_KEY") < (int)(cap - n);
}

static int pi_key_from_auth(cJSON *auth, const char *provider)
{
    cJSON *entry = auth ? cJSON_GetObjectItem(auth, provider) : NULL;
    if (!entry || !cJSON_IsObject(entry))
        return 0;
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "key"));
    if (key && *key)
        return 1;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "type"));
    return type && *type && strcmp(type, "api_key") != 0;
}

static int pi_has_key(cJSON *auth, const char *provider)
{
    char var[80];
    if (pi_env_name(provider, var, sizeof var) && env_set(var))
        return 1;
    return pi_key_from_auth(auth, provider);
}

static time_t pi_env_stamp(void)
{
    static const char *const vars[] = {
        "ANTHROPIC_API_KEY", "OPENAI_API_KEY", "GEMINI_API_KEY", "XAI_API_KEY",
        "CEREBRAS_API_KEY", "GROQ_API_KEY", "DEEPSEEK_API_KEY", "HF_TOKEN",
        "MISTRAL_API_KEY", "FIREWORKS_API_KEY", "TOGETHER_API_KEY",
        "NVIDIA_API_KEY", NULL
    };
    time_t t = 0;
    for (int i = 0; vars[i]; i++)
        if (env_set(vars[i]))
            t |= (time_t)1 << i;
    return t;
}

/* A backend that reads its models off disk rebuilds its list when the file
   moves, or an edited config waits for the next mux. */
static time_t backend_stamp(const char *backend)
{
    if (!strcmp(backend, "pi"))
        return home_stamp(".pi/agent/models.json")
             + home_stamp(".pi/agent/models-store.json")
             + home_stamp(".pi/agent/auth.json")
             + pi_env_stamp();
    if (!strcmp(backend, "codex"))
        return home_stamp(".codex/models_cache.json");
    return 0;
}

static const struct pick_item CLAUDE[] = {
    {"claude-opus-5", "most capable"},
    {"claude-opus-5[1m]", "opus with a 1M-token context"},
    {"claude-sonnet-5", "balanced speed and capability"},
    {"claude-haiku-4-5", "fastest"},
    {"claude-fable-5", "compact"},
};

static const struct pick_item GROK[] = {
    {"grok-4.6", "latest frontier model"},
    {"grok-4.5", "the prior generation"},
};

static void fill_static(struct list *l, const struct pick_item *v, int n)
{
    for (int i = 0; i < n; i++)
        push(l, v[i].label, v[i].detail);
}

static void fill_codex(struct list *l)
{
    char *text = home_slurp(".codex/models_cache.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *models = root ? cJSON_GetObjectItem(root, "models") : NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *visibility = cJSON_GetStringValue(cJSON_GetObjectItem(m, "visibility"));
        if (visibility && strcmp(visibility, "list"))
            continue;
        push(l, cJSON_GetStringValue(cJSON_GetObjectItem(m, "slug")),
             cJSON_GetStringValue(cJSON_GetObjectItem(m, "description")));
    }
    cJSON_Delete(root);
    free(text);
}

static void describe(char *out, size_t cap, const char *name, double context)
{
    char ctx[32] = "";
    if (context >= 1000000)
        snprintf(ctx, sizeof ctx, "%gM context", (double)(long)(context / 100000) / 10);
    else if (context >= 1000)
        snprintf(ctx, sizeof ctx, "%ldK context", (long)(context / 1000));

    if (name && *name && ctx[0])
        snprintf(out, cap, "%s \xc2\xb7 %s", name, ctx);
    else
        snprintf(out, cap, "%s", name && *name ? name : ctx);
}

static void pi_model_id(char *out, size_t cap, const char *id)
{
    if (!strncmp(id, "openrouter/", 11))
        snprintf(out, cap, "%s", id);
    else
        snprintf(out, cap, "openrouter/%s", id);
}

/* pi takes provider/id. The id is already prefixed when the catalog listed it
   that way. */
static void push_pi_model(struct list *l, const char *provider, const char *id,
                          const char *name, double context)
{
    if (!id || !*id)
        return;

    char        label[LABEL_BYTES];
    const char *at = provider ? provider : "";
    size_t      len = strlen(at);
    if (len && !strncmp(id, at, len) && id[len] == '/')
        snprintf(label, sizeof label, "%s", id);
    else if (len)
        snprintf(label, sizeof label, "%s/%s", at, id);
    else
        snprintf(label, sizeof label, "%s", id);

    char detail[DETAIL_BYTES];
    describe(detail, sizeof detail, name, context);
    push(l, label, detail);
}

static void push_openrouter(struct list *l, const char *id, const char *name, double context)
{
    if (!id || !*id)
        return;

    char label[LABEL_BYTES];
    char detail[DETAIL_BYTES];
    pi_model_id(label, sizeof label, id);
    describe(detail, sizeof detail, name, context);
    push(l, label, detail);

    if (strncmp(id, "openrouter/", 11))
        return;
    static const char *const VARIANTS[] = {":nitro", ":floor"};
    static const char *const WHAT[] = {"routed for throughput", "routed for price"};
    for (size_t i = 0; i < sizeof VARIANTS / sizeof *VARIANTS; i++) {
        char variant[LABEL_BYTES];
        char why[DETAIL_BYTES];
        snprintf(variant, sizeof variant, "%s%s", label, VARIANTS[i]);
        snprintf(why, sizeof why, "%s \xc2\xb7 %s", name && *name ? name : id, WHAT[i]);
        push(l, variant, why);
    }
}

static void openrouter_json(struct list *l, const char *text, const char *key)
{
    cJSON *root = cJSON_Parse(text);
    cJSON *models = root ? cJSON_GetObjectItem(root, key) : NULL;

    for (int routers = 1; routers >= 0; routers--) {
        cJSON *m;
        cJSON_ArrayForEach(m, models) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
            if (!id || (strncmp(id, "openrouter/", 11) == 0) != routers)
                continue;
            push_openrouter(l, id, cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                            cJSON_GetNumberValue(cJSON_GetObjectItem(m, "context_length")));
        }
    }
    cJSON_Delete(root);
}

#define CATALOG_MAX_AGE (4 * 60 * 60)

static void refresh_openrouter(const char *path)
{
    char dest[4200], tmp[4300], quoted[4400];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp)
        return;
    if (!text_shell_quote(path, dest, sizeof dest) ||
        !text_shell_quote(tmp, quoted, sizeof quoted))
        return;

    char cmd[26000];
    if (snprintf(cmd, sizeof cmd,
                 "(if curl -fsS --max-time 20 -o %s "
                 "https://openrouter.ai/api/v1/models >/dev/null 2>&1 && [ -s %s ]; "
                 "then mv %s %s; else rm -f %s; fi) >/dev/null 2>&1 &",
                 quoted, quoted, quoted, dest, quoted) >= (int)sizeof cmd)
        return;

    (void)system(cmd);
}

static char *catalog_text(void)
{
    char path[4096];
    if (!path_config_file(path, sizeof path, "openrouter.json"))
        return NULL;

    struct stat st;
    if (stat(path, &st) || st.st_size < 1024 ||
        now_seconds() - (double)st.st_mtime > CATALOG_MAX_AGE)
        refresh_openrouter(path);

    return text_slurp(path, 1 << 23, NULL);
}

static int fill_openrouter_catalog(struct list *l)
{
    char *text = catalog_text();
    if (!text)
        return 0;

    int before = l->n;
    openrouter_json(l, text, "data");
    free(text);
    return l->n > before;
}

/* The catalog quotes dollars per token as a string. */
static double per_million(cJSON *pricing, const char *key)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(pricing, key));
    return v ? strtod(v, NULL) * 1e6 : 0;
}

static int rates_of(cJSON *models, const char *id, struct model_rates *out)
{
    cJSON *m;
    cJSON_ArrayForEach(m, models) {
        const char *have = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (!have || strcmp(have, id))
            continue;
        cJSON *pricing = cJSON_GetObjectItem(m, "pricing");
        out->input = per_million(pricing, "prompt");
        out->output = per_million(pricing, "completion");
        out->cache_read = per_million(pricing, "input_cache_read");
        return out->input > 0 || out->output > 0;
    }
    return 0;
}

/* A backend names its model without the vendor the catalog files it under. */
static const char *catalog_vendor(const char *backend)
{
    if (!backend)
        return "";
    if (!strcmp(backend, "codex"))
        return "openai/";
    if (!strcmp(backend, "claude"))
        return "anthropic/";
    if (!strcmp(backend, "grok"))
        return "x-ai/";
    return "";
}

/* 0 when the catalog is not there yet */
static time_t catalog_stamp(void)
{
    char        path[4096];
    struct stat st;
    if (!path_config_file(path, sizeof path, "openrouter.json") || stat(path, &st))
        return 0;
    return st.st_mtime;
}

static int store_rates(const char *model, struct model_rates *out)
{
    if (!model || !strncmp(model, "openrouter/", 11))
        return 0;
    const char *slash = strchr(model, '/');
    if (!slash || slash == model)
        return 0;

    char provider[64];
    size_t n = (size_t)(slash - model);
    if (n >= sizeof provider)
        return 0;
    memcpy(provider, model, n);
    provider[n] = '\0';

    char *text = home_slurp(".pi/agent/models-store.json");
    if (!text)
        return 0;

    cJSON *root = cJSON_Parse(text);
    cJSON *block = root ? cJSON_GetObjectItem(root, provider) : NULL;
    int    ok = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(block, "models")) {
        const char *have = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (!have || strcmp(have, slash + 1))
            continue;
        cJSON *cost = cJSON_GetObjectItem(m, "cost");
        out->input = cJSON_GetNumberValue(cJSON_GetObjectItem(cost, "input"));
        out->output = cJSON_GetNumberValue(cJSON_GetObjectItem(cost, "output"));
        out->cache_read = cJSON_GetNumberValue(cJSON_GetObjectItem(cost, "cacheRead"));
        ok = out->input > 0 || out->output > 0;
        break;
    }
    cJSON_Delete(root);
    free(text);
    return ok;
}

int models_rates(const char *backend, const char *model, struct model_rates *out)
{
    if (!out || !model || !*model)
        return 0;
    memset(out, 0, sizeof *out);

    char key[256];
    snprintf(key, sizeof key, "%s/%s", backend ? backend : "", model);

    /* Misses are remembered too, or a model the catalog does not carry reparses
       the whole file every turn. A catalog that lands, or is refreshed, moves
       the mtime and drops the table. */
    static struct {
        char               key[256];
        struct model_rates rates;
        int                hit;
    } known[16];
    static int    n, at;
    static time_t stamp;

    time_t now = catalog_stamp() + home_stamp(".pi/agent/models-store.json");
    if (now != stamp) {
        stamp = now;
        n = at = 0;
    }

    for (int i = 0; i < n; i++)
        if (!strcmp(known[i].key, key)) {
            if (!known[i].hit)
                return 0;
            *out = known[i].rates;
            return 1;
        }

    char id[160];
    snprintf(id, sizeof id, "%s", model);
    if (!strncmp(id, "openrouter/", 11))
        memmove(id, id + 11, strlen(id + 11) + 1);
    char *variant = strchr(id, ':');
    if (variant)
        *variant = '\0';

    char filed[192];
    if (strchr(id, '/'))
        snprintf(filed, sizeof filed, "%s", id);
    else
        snprintf(filed, sizeof filed, "%s%s", catalog_vendor(backend), id);

    int ok = 0;
    char *text = catalog_text();
    if (text) {
        cJSON *root = cJSON_Parse(text);
        cJSON *models = root ? cJSON_GetObjectItem(root, "data") : NULL;
        ok = models && (rates_of(models, filed, out) || rates_of(models, id, out));
        cJSON_Delete(root);
        free(text);
    }
    if (!ok)
        ok = store_rates(model, out);

    if (!ok)
        memset(out, 0, sizeof *out);

    snprintf(known[at].key, sizeof known[at].key, "%s", key);
    known[at].rates = *out;
    known[at].hit = ok;
    at = (at + 1) % (int)(sizeof known / sizeof *known);
    if (n < (int)(sizeof known / sizeof *known))
        n++;

    return ok;
}

static void fill_pi_store(struct list *l)
{
    char *text = home_slurp(".pi/agent/models-store.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *openrouter = root ? cJSON_GetObjectItem(root, "openrouter") : NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(openrouter, "models"))
        push_openrouter(l, cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                        cJSON_GetNumberValue(cJSON_GetObjectItem(m, "contextWindow")));
    cJSON_Delete(root);
    free(text);
}

static void fill_pi_configured(struct list *l)
{
    char *text = home_slurp(".pi/agent/models.json");
    if (!text)
        return;

    cJSON *root = cJSON_Parse(text);
    cJSON *providers = root ? cJSON_GetObjectItem(root, "providers") : NULL;
    cJSON *provider;
    cJSON_ArrayForEach(provider, providers) {
        cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(provider, "models"))
            push_pi_model(l, provider->string,
                          cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")),
                          cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                          cJSON_GetNumberValue(cJSON_GetObjectItem(m, "contextWindow")));
    }
    cJSON_Delete(root);
    free(text);
}

static void fill_pi_native(struct list *l)
{
    char *text = home_slurp(".pi/agent/models-store.json");
    if (!text)
        return;

    char *auth_text = home_slurp(".pi/agent/auth.json");
    cJSON *root = cJSON_Parse(text);
    cJSON *auth = auth_text ? cJSON_Parse(auth_text) : NULL;
    cJSON *provider;
    cJSON_ArrayForEach(provider, root) {
        const char *at = provider->string;
        if (!at || !strcmp(at, "openrouter"))
            continue;
        if (!pi_has_key(auth, at))
            continue;
        cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(provider, "models"))
            push_pi_model(l, at,
                          cJSON_GetStringValue(cJSON_GetObjectItem(m, "id")),
                          cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")),
                          cJSON_GetNumberValue(cJSON_GetObjectItem(m, "contextWindow")));
    }
    cJSON_Delete(auth);
    cJSON_Delete(root);
    free(auth_text);
    free(text);
}

static void fill_pi(struct list *l)
{
    fill_pi_configured(l);
    fill_pi_native(l);
    if (!fill_openrouter_catalog(l))
        fill_pi_store(l);
}

int models_for(const char *backend, const struct pick_item **out)
{
    time_t       stamp = backend_stamp(backend);
    struct list *l = NULL;
    for (size_t i = 0; i < sizeof cache / sizeof *cache; i++) {
        if (!strcmp(cache[i].backend, backend)) {
            if (cache[i].stamp == stamp) {
                *out = cache[i].items;
                return cache[i].n;
            }
            l = &cache[i];
            l->n = 0;
            break;
        }
        if (!l && !cache[i].backend[0])
            l = &cache[i];
    }
    if (!l) {
        *out = NULL;
        return 0;
    }
    snprintf(l->backend, sizeof l->backend, "%s", backend);
    l->stamp = stamp;

    char detail[DETAIL_BYTES];
    snprintf(detail, sizeof detail, "whatever the %s CLI is configured to use", backend);
    push(l, "default", detail);

    if (!strcmp(backend, "claude")) {
        fill_static(l, CLAUDE, (int)(sizeof CLAUDE / sizeof *CLAUDE));
    } else if (!strcmp(backend, "codex")) {
        fill_codex(l);
    } else if (!strcmp(backend, "pi")) {
        fill_pi(l);
    } else if (!strcmp(backend, "grok")) {
        fill_static(l, GROK, (int)(sizeof GROK / sizeof *GROK));
    }

    *out = l->items;
    return l->n;
}

const char *models_short_name(const char *backend, const char *model)
{
    const size_t plen = sizeof "claude-" - 1;

    if (backend && model && !strcmp(backend, "claude") &&
        !strncmp(model, "claude-", plen) && model[plen])
        return model + plen;
    return model;
}

/* Codex takes only the full slug: `sol` reaches the API and comes back a 400.
 * Expand a bare family name against the cached catalogue when exactly one slug
 * ends in it. */
int models_codex_slug(const char *model, char *out, size_t size)
{
    if (!model || !*model)
        return 0;

    const struct pick_item *items = NULL;
    int                     n = models_for("codex", &items);
    size_t                  len = strlen(model);
    const char             *hit = NULL;

    for (int i = 0; i < n; i++) {
        const char *slug = items[i].label;
        size_t      at = strlen(slug);

        if (!strcmp(slug, model))
            return 0;
        if (at <= len || slug[at - len - 1] != '-' || strcmp(slug + at - len, model))
            continue;
        if (hit)
            return 0;
        hit = slug;
    }
    if (!hit)
        return 0;

    snprintf(out, size, "%s", hit);
    return 1;
}
