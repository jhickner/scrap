/*
 * core.h — native agent loop: OpenAI-compatible chat completions over
 * libcurl, four tools (read, write, edit, bash), JSONL sessions, compaction,
 * context files, skills, and hooks.
 *
 * The declarations need nothing else. backend.h instantiates the
 * implementation (CORE_AGENT_IMPLEMENTATION) after its adapter scaffolding.
 *
 * Config and state live under $SCRAP_CONFIG_DIR/agent, else
 * ~/.config/scrap/agent: providers.json, models/<provider>.json (cached
 * GET /models), sessions/<encoded cwd>/<id>.jsonl.
 */
#ifndef SCRAP_AGENT_H
#define SCRAP_AGENT_H

#include <stddef.h>

int  core_agent_config_dir(char *out, size_t size);
int  core_agent_session_dir(const char *cwd, char *out, size_t size);
long core_agent_models_stamp(void);
void core_agent_models(void (*fn)(void *ud, const char *id, const char *name, long context),
                        void *ud);

#ifdef BACKEND_H
Backend *core_agent_open(const backend_opts *o);
#endif

#endif /* SCRAP_AGENT_H */

#ifdef CORE_AGENT_IMPLEMENTATION
#ifndef BACKEND_H
#error "Include backend.h before defining CORE_AGENT_IMPLEMENTATION."
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

extern char **environ;

#define SA_READ_LINES      2000
#define SA_READ_BYTES      (50 * 1024)
#define SA_LINE_CHARS      2000
#define SA_OUT_LINES       2000
#define SA_OUT_BYTES       (50 * 1024)
#define SA_KEEP_BYTES      (1024 * 1024)
#define SA_WINDOW_DEFAULT  131072
#define SA_MODELS_MAX_AGE  (4 * 60 * 60)
#define SA_HOOK_TIMEOUT_MS 60000
#define SA_RETRIES         3

static const char SA_DEFAULT_CONFIG[] =
    "{\n"
    "  \"default\": \"openrouter/openai/gpt-5.6-sol\",\n"
    "  \"max_steps\": 0,\n"
    "  \"providers\": {\n"
    "    \"openrouter\": { \"base_url\": \"https://openrouter.ai/api/v1\", \"key_env\": \"OPENROUTER_API_KEY\", \"effort\": \"openrouter\" },\n"
    "    \"cerebras\":   { \"base_url\": \"https://api.cerebras.ai/v1\", \"key_env\": \"CEREBRAS_API_KEY\", \"effort\": \"openai\" },\n"
    "    \"groq\":       { \"base_url\": \"https://api.groq.com/openai/v1\", \"key_env\": \"GROQ_API_KEY\", \"effort\": \"openai\" },\n"
    "    \"openai\":     { \"base_url\": \"https://api.openai.com/v1\", \"key_env\": \"OPENAI_API_KEY\", \"effort\": \"openai\" },\n"
    "    \"xai\":        { \"base_url\": \"https://api.x.ai/v1\", \"key_env\": \"XAI_API_KEY\", \"effort\": \"openai\" },\n"
    "    \"deepseek\":   { \"base_url\": \"https://api.deepseek.com/v1\", \"key_env\": \"DEEPSEEK_API_KEY\" },\n"
    "    \"fireworks\":  { \"base_url\": \"https://api.fireworks.ai/inference/v1\", \"key_env\": \"FIREWORKS_API_KEY\", \"effort\": \"openai\" },\n"
    "    \"together\":   { \"base_url\": \"https://api.together.xyz/v1\", \"key_env\": \"TOGETHER_API_KEY\" },\n"
    "    \"ollama\":     { \"base_url\": \"http://localhost:11434/v1\" }\n"
    "  }\n"
    "}\n";

static const char SA_TOOLS[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"read\",\"description\":"
    "\"Read a text file. Output is truncated to 2000 lines or 50KB; use offset and limit for larger files.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path, relative to the working directory or absolute\"},"
    "\"offset\":{\"type\":\"number\",\"description\":\"First line to read, 1-based\"},"
    "\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of lines to read\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write\",\"description\":"
    "\"Write a file, creating parent directories. Overwrites an existing file.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Full file content\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit\",\"description\":"
    "\"Replace text in a file. oldText must match exactly one location, including whitespace.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path\"},"
    "\"oldText\":{\"type\":\"string\",\"description\":\"Exact text to replace\"},"
    "\"newText\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
    "\"required\":[\"path\",\"oldText\",\"newText\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"bash\",\"description\":"
    "\"Run a bash command in the working directory. stdout and stderr are combined; output is truncated to the last 2000 lines or 50KB.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Command to run\"},"
    "\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds; none by default\"}},"
    "\"required\":[\"command\"]}}}"
    "]";

static const char SA_SYSTEM[] =
    "You are an expert coding assistant running inside scrap, a terminal coding harness. "
    "You help the user by reading files, running commands, editing code, and writing files.\n\n"
    "Tools:\n"
    "- read: read file contents\n"
    "- bash: run shell commands (ls, rg, find, git, builds, tests)\n"
    "- edit: replace exact text in a file\n"
    "- write: create or overwrite a file\n\n"
    "Guidelines:\n"
    "- Use bash for listing and searching files.\n"
    "- Read a file before editing it; use read, not cat or sed, to view files.\n"
    "- Use edit for targeted changes; oldText must match exactly once.\n"
    "- Use write only for new files or complete rewrites.\n"
    "- Be concise. Show file paths when referring to files.";

static const char SA_SUMMARY_PROMPT[] =
    "Summarize the conversation above so the work can continue from the summary alone. "
    "Include the user's goals and requests, decisions made, files read or changed with the "
    "details that matter, commands run and their results, the current state, and the "
    "remaining next steps. Output only the summary.";

/* ---------- buffers and small helpers ---------- */

typedef struct { char *p; size_t n, cap; } sa_buf;

static void sa_put(sa_buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *g = realloc(b->p, cap);
        if (!g) return;
        b->p = g;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void sa_puts(sa_buf *b, const char *s) { if (s) sa_put(b, s, strlen(s)); }
__attribute__((format(printf, 2, 3)))
static void sa_printf(sa_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { sa_put(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sa_put(b, big, (size_t)n);
    free(big);
}
static const char *sa_str(const sa_buf *b) { return b->p ? b->p : ""; }
static void sa_clear(sa_buf *b) { b->n = 0; if (b->p) b->p[0] = '\0'; }
static void sa_free(sa_buf *b) { free(b->p); memset(b, 0, sizeof *b); }

static long sa_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sa_mkdirs(const char *path, mode_t mode) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, mode);
        *p = '/';
    }
    mkdir(tmp, mode);
}

static char *sa_slurp(const char *path, size_t *len) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    sa_buf b = {0};
    char chunk[65536];
    size_t n;
    sa_put(&b, "", 0);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) sa_put(&b, chunk, n);
    fclose(f);
    if (len) *len = b.n;
    return b.p;
}

static int sa_write_file(const char *path, const char *data, size_t n) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok && rename(tmp, path) == 0) return 1;
    unlink(tmp);
    return 0;
}

static const char *sa_jstr(const cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static double sa_jnum(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int sa_unset(const char *s) { return !s || !*s || !strcmp(s, "default"); }

static void sa_new_id(char *out, size_t size) {
    unsigned char r[16] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, r, sizeof r) != (ssize_t)sizeof r) {
        for (size_t i = 0; i < sizeof r; i++) r[i] = (unsigned char)(rand() ^ (int)time(NULL) >> (i % 8));
    }
    if (fd >= 0) close(fd);
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    snprintf(out, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
             r[12], r[13], r[14], r[15]);
}

/* ---------- config ---------- */

int core_agent_config_dir(char *out, size_t size) {
    const char *env = getenv("SCRAP_CONFIG_DIR"), *home = getenv("HOME");
    int n;
    if (env && *env) n = snprintf(out, size, "%s/agent", env);
    else if (home && *home) n = snprintf(out, size, "%s/.config/scrap/agent", home);
    else return 0;
    if (n < 0 || (size_t)n >= size) return 0;
    sa_mkdirs(out, 0700);
    return 1;
}

static int sa_app_config_file(const char *leaf, char *out, size_t size) {
    const char *env = getenv("SCRAP_CONFIG_DIR"), *home = getenv("HOME");
    int n;
    if (env && *env) n = snprintf(out, size, "%s/%s", env, leaf);
    else if (home && *home) n = snprintf(out, size, "%s/.config/scrap/%s", home, leaf);
    else return 0;
    return n > 0 && (size_t)n < size;
}

int core_agent_session_dir(const char *cwd, char *out, size_t size) {
    char base[2048], enc[2048];
    size_t o = 0;
    if (!cwd || !core_agent_config_dir(base, sizeof base)) return 0;
    for (const char *p = cwd; *p && o + 1 < sizeof enc; p++)
        enc[o++] = (isalnum((unsigned char)*p) || *p == '-') ? *p : '-';
    enc[o] = '\0';
    int n = snprintf(out, size, "%s/sessions/%s", base, enc);
    if (n < 0 || (size_t)n >= size) return 0;
    sa_mkdirs(out, 0700);
    return 1;
}

static cJSON *sa_config_load(char *err, size_t errsize) {
    char dir[2048], path[2200];
    if (!core_agent_config_dir(dir, sizeof dir)) {
        snprintf(err, errsize, "no config directory (HOME unset)");
        return NULL;
    }
    snprintf(path, sizeof path, "%s/providers.json", dir);
    char *text = sa_slurp(path, NULL);
    if (!text) {
        sa_write_file(path, SA_DEFAULT_CONFIG, sizeof SA_DEFAULT_CONFIG - 1);
        text = strdup(SA_DEFAULT_CONFIG);
    }
    cJSON *c = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!c) snprintf(err, errsize, "%s is not valid JSON", path);
    return c;
}

static pthread_mutex_t sa_key_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char var[64]; char *value; } sa_keys[32];

static const char *sa_key(const char *var) {
    if (!var || !*var) return NULL;
    const char *env = getenv(var);
    if (env && *env) return env;
    for (const char *p = var; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return NULL;
    pthread_mutex_lock(&sa_key_lock);
    const char *found = NULL;
    size_t i;
    for (i = 0; i < sizeof sa_keys / sizeof *sa_keys && sa_keys[i].var[0]; i++)
        if (!strcmp(sa_keys[i].var, var)) { found = sa_keys[i].value; break; }
    if (i < sizeof sa_keys / sizeof *sa_keys && !sa_keys[i].var[0]) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "bash -lc 'printf %%s \"$%s\"' 2>/dev/null </dev/null", var);
        FILE *f = popen(cmd, "r");
        char buf[1024] = "";
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) pclose(f);
        buf[n] = '\0';
        snprintf(sa_keys[i].var, sizeof sa_keys[i].var, "%s", var);
        sa_keys[i].value = n ? strdup(buf) : NULL;
        found = sa_keys[i].value;
    }
    pthread_mutex_unlock(&sa_key_lock);
    return found;
}

static int sa_provider_key(const cJSON *prov, const char **key) {
    const char *var = sa_jstr(prov, "key_env");
    *key = var ? sa_key(var) : NULL;
    return !var || *key;
}

/* ---------- model catalogue ---------- */

static int sa_models_path(const char *provider, char *out, size_t size) {
    char dir[2048];
    if (!core_agent_config_dir(dir, sizeof dir)) return 0;
    int n = snprintf(out, size, "%s/models", dir);
    if (n < 0 || (size_t)n >= size) return 0;
    sa_mkdirs(out, 0700);
    n = snprintf(out, size, "%s/models/%s.json", dir, provider);
    return n > 0 && (size_t)n < size;
}

typedef struct { char *url, *key, *path; } sa_fetch;

static size_t sa_collect(char *p, size_t sz, size_t nm, void *ud) {
    sa_put(ud, p, sz * nm);
    return sz * nm;
}

static void *sa_fetch_thread(void *arg) {
    sa_fetch *f = arg;
    CURL *c = curl_easy_init();
    sa_buf body = {0};
    struct curl_slist *h = NULL;
    if (c) {
        if (f->key) {
            char auth[1200];
            snprintf(auth, sizeof auth, "Authorization: Bearer %s", f->key);
            h = curl_slist_append(h, auth);
        }
        curl_easy_setopt(c, CURLOPT_URL, f->url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sa_collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        long code = 0;
        if (curl_easy_perform(c) == CURLE_OK &&
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code == 200) {
            cJSON *j = cJSON_Parse(sa_str(&body));
            if (cJSON_IsArray(cJSON_GetObjectItem(j, "data")))
                sa_write_file(f->path, body.p, body.n);
            cJSON_Delete(j);
        }
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
    }
    sa_free(&body);
    free(f->url); free(f->key); free(f->path); free(f);
    return NULL;
}

static void sa_models_refresh(const cJSON *config) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static long last_ms;
    pthread_mutex_lock(&lock);
    long now = sa_now_ms();
    int due = !last_ms || now - last_ms > 60000;
    if (due) last_ms = now;
    pthread_mutex_unlock(&lock);
    if (!due) return;
    const cJSON *prov;
    cJSON_ArrayForEach(prov, cJSON_GetObjectItem(config, "providers")) {
        const char *base = sa_jstr(prov, "base_url"), *key = NULL;
        char path[2400];
        struct stat st;
        if (!base || cJSON_IsFalse(cJSON_GetObjectItem(prov, "list"))) continue;
        if (!sa_provider_key(prov, &key) || !sa_models_path(prov->string, path, sizeof path)) continue;
        if (stat(path, &st) == 0 && time(NULL) - st.st_mtime < SA_MODELS_MAX_AGE) continue;
        sa_fetch *f = calloc(1, sizeof *f);
        if (!f) continue;
        size_t n = strlen(base) + 16;
        f->url = malloc(n);
        if (f->url) snprintf(f->url, n, "%s/models", base);
        f->key = key ? strdup(key) : NULL;
        f->path = strdup(path);
        pthread_t t;
        if (!f->url || !f->path || pthread_create(&t, NULL, sa_fetch_thread, f) != 0) {
            free(f->url); free(f->key); free(f->path); free(f);
            continue;
        }
        pthread_detach(t);
    }
}

static cJSON *sa_models_cache(const char *provider) {
    char path[2400];
    if (!sa_models_path(provider, path, sizeof path)) return NULL;
    char *text = sa_slurp(path, NULL);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    return j;
}

static long sa_model_context(const cJSON *m) {
    long v = (long)sa_jnum(m, "context");
    if (!v) v = (long)sa_jnum(m, "context_length");
    if (!v) v = (long)sa_jnum(m, "context_window");
    if (!v) v = (long)sa_jnum(cJSON_GetObjectItem(m, "top_provider"), "context_length");
    return v;
}

long core_agent_models_stamp(void) {
    char dir[2048], path[2200];
    struct stat st;
    long t = 0;
    if (!core_agent_config_dir(dir, sizeof dir)) return 0;
    snprintf(path, sizeof path, "%s/providers.json", dir);
    if (stat(path, &st) == 0) t += (long)st.st_mtime;
    snprintf(path, sizeof path, "%s/models", dir);
    if (stat(path, &st) == 0) t += (long)st.st_mtime;
    return t;
}

void core_agent_models(void (*fn)(void *ud, const char *id, const char *name, long context),
                        void *ud) {
    char err[256];
    cJSON *config = sa_config_load(err, sizeof err);
    if (!config) return;
    sa_models_refresh(config);
    const cJSON *prov;
    cJSON_ArrayForEach(prov, cJSON_GetObjectItem(config, "providers")) {
        const char *key = NULL;
        if (!sa_provider_key(prov, &key)) continue;
        char id[512];
        const cJSON *mine = cJSON_GetObjectItem(prov, "models"), *m;
        cJSON_ArrayForEach(m, mine) {
            snprintf(id, sizeof id, "%s/%s", prov->string, m->string);
            fn(ud, id, sa_jstr(m, "name"), sa_model_context(m));
        }
        cJSON *cache = sa_models_cache(prov->string);
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(cache, "data")) {
            const char *mid = sa_jstr(m, "id");
            if (!mid || cJSON_GetObjectItemCaseSensitive(mine, mid)) continue;
            snprintf(id, sizeof id, "%s/%s", prov->string, mid);
            fn(ud, id, sa_jstr(m, "name"), sa_model_context(m));
        }
        cJSON_Delete(cache);
    }
    cJSON_Delete(config);
}

/* ---------- agent state ---------- */

typedef struct {
    char *full, *provider, *model, *base_url, *key, *effort_style;
    cJSON *headers, *extra;
    long window;
} sa_route;

typedef struct {
    backend_state st;
    int started;
    cJSON *config, *hooks;
    sa_route route;
    char id[64];
    char path[4096];
    FILE *fp;
    cJSON *msgs;
    char *system;
    sa_buf hook_context;
    long ctx_tokens, window;
    double cost;
    char err[1024];
} sa_agent;

static void sa_warn(sa_agent *x, const char *text) {
    backend_flush(&x->st);
    backend_event ev = { .kind = BACKEND_EV_WARNING, .text = text };
    backend_emit(&x->st, &ev);
}

static void sa_route_free(sa_route *r) {
    free(r->full); free(r->provider); free(r->model); free(r->base_url); free(r->key);
    free(r->effort_style);
    memset(r, 0, sizeof *r);
}

static int sa_route_resolve(sa_agent *x) {
    const char *want = sa_unset(x->st.model) ? sa_jstr(x->config, "default") : x->st.model;
    if (!want || !*want) {
        snprintf(x->err, sizeof x->err, "no model given and providers.json has no default");
        return 0;
    }
    if (x->route.full && !strcmp(x->route.full, want)) return 1;
    const char *slash = strchr(want, '/');
    if (!slash || slash == want || !slash[1]) {
        snprintf(x->err, sizeof x->err, "model '%s' is not provider/model", want);
        return 0;
    }
    char provider[128];
    snprintf(provider, sizeof provider, "%.*s", (int)(slash - want), want);
    const cJSON *prov = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItem(x->config, "providers"),
                                                         provider);
    const char *base = sa_jstr(prov, "base_url"), *key = NULL;
    if (!base) {
        snprintf(x->err, sizeof x->err, "provider '%s' is not in providers.json", provider);
        return 0;
    }
    if (!sa_provider_key(prov, &key)) {
        snprintf(x->err, sizeof x->err, "%s is not set", sa_jstr(prov, "key_env"));
        return 0;
    }
    sa_route_free(&x->route);
    x->route.full = strdup(want);
    x->route.provider = strdup(provider);
    x->route.model = strdup(slash + 1);
    x->route.base_url = strdup(base);
    x->route.key = key ? strdup(key) : NULL;
    x->route.effort_style = backend_dup(sa_jstr(prov, "effort"));
    x->route.headers = cJSON_GetObjectItem(prov, "headers");
    x->route.extra = cJSON_GetObjectItem(prov, "extra");
    long window = sa_model_context(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItem(prov, "models"),
                                                                    x->route.model));
    if (!window) {
        cJSON *cache = sa_models_cache(provider), *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(cache, "data")) {
            const char *mid = sa_jstr(m, "id");
            if (mid && !strcmp(mid, x->route.model)) { window = sa_model_context(m); break; }
        }
        cJSON_Delete(cache);
    }
    x->route.window = window ? window : SA_WINDOW_DEFAULT;
    x->window = x->route.window;
    backend_flush(&x->st);
    backend_event ev = { .kind = BACKEND_EV_INIT, .name = x->route.full };
    backend_emit(&x->st, &ev);
    return 1;
}

/* ---------- processes ---------- */

typedef struct {
    const char *cmd;
    const char *input;
    long timeout_ms;
    int merge;
} sa_proc;

enum { SA_RAN, SA_TIMEOUT, SA_ABORTED, SA_SPAWN_FAILED };

static void sa_keep(sa_buf *b, const char *s, size_t n, int *dropped) {
    sa_put(b, s, n);
    if (b->n > 2 * SA_KEEP_BYTES) {
        size_t cut = b->n - SA_KEEP_BYTES;
        memmove(b->p, b->p + cut, b->n - cut + 1);
        b->n -= cut;
        *dropped = 1;
    }
}

static char **sa_envp(sa_agent *x) {
    size_t n = 0, add = 1;
    while (environ[n]) n++;
    for (char **e = x->st.env; e && *e; e++) add++;
    char **out = calloc(n + add + 1, sizeof *out);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(environ[i], '=');
        size_t klen = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
        int shadowed = x->st.session_file && klen == 16 && !strncmp(environ[i], "MUX_SESSION_FILE", 16);
        for (char **e = x->st.env; !shadowed && e && *e; e++)
            shadowed = !strncmp(*e, environ[i], klen) && (*e)[klen] == '=';
        if (!shadowed) out[o++] = strdup(environ[i]);
    }
    for (char **e = x->st.env; e && *e; e++) out[o++] = strdup(*e);
    if (x->st.session_file) {
        size_t len = strlen(x->st.session_file) + 18;
        char *v = malloc(len);
        if (v) snprintf(v, len, "MUX_SESSION_FILE=%s", x->st.session_file);
        out[o++] = v;
    }
    return out;
}

static void sa_envp_free(char **envp) {
    for (char **e = envp; e && *e; e++) free(*e);
    free(envp);
}

static int sa_aborted(sa_agent *x) { return x->st.abort && x->st.abort(); }

static void sa_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static int sa_run(sa_agent *x, const sa_proc *p, sa_buf *out, sa_buf *err, int *status,
                  int *dropped) {
    int in[2] = {-1, -1}, o[2], e[2] = {-1, -1};
    *status = -1;
    *dropped = 0;
    if (pipe(o) != 0) return SA_SPAWN_FAILED;
    if (!p->merge && pipe(e) != 0) { close(o[0]); close(o[1]); return SA_SPAWN_FAILED; }
    if (p->input && pipe(in) != 0) {
        close(o[0]); close(o[1]);
        if (e[0] >= 0) { close(e[0]); close(e[1]); }
        return SA_SPAWN_FAILED;
    }
    int devnull = p->input ? -1 : open("/dev/null", O_RDONLY);
    char **envp = sa_envp(x);
    const char *cwd = x->st.cwd;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (cwd && chdir(cwd) != 0) _exit(126);
        dup2(p->input ? in[0] : devnull, 0);
        dup2(o[1], 1);
        dup2(p->merge ? o[1] : e[1], 2);
        for (int fd = 3; fd < 256; fd++) close(fd);
        char *argv[] = { "bash", "-c", (char *)p->cmd, NULL };
        execve("/bin/bash", argv, envp ? envp : environ);
        _exit(127);
    }
    sa_envp_free(envp);
    if (devnull >= 0) close(devnull);
    close(o[1]);
    if (e[1] >= 0) close(e[1]);
    if (in[0] >= 0) close(in[0]);
    if (pid < 0) {
        close(o[0]);
        if (e[0] >= 0) close(e[0]);
        if (in[1] >= 0) close(in[1]);
        return SA_SPAWN_FAILED;
    }
    setpgid(pid, pid);
    sa_nonblock(o[0]);
    if (e[0] >= 0) sa_nonblock(e[0]);
    if (in[1] >= 0) { sa_nonblock(in[1]); signal(SIGPIPE, SIG_IGN); }

    size_t written = 0, input_len = p->input ? strlen(p->input) : 0;
    long start = sa_now_ms();
    int result = SA_RAN, exited = 0;
    char chunk[16384];
    while (o[0] >= 0 || e[0] >= 0) {
        struct pollfd fds[3];
        int nf = 0;
        if (o[0] >= 0) fds[nf++] = (struct pollfd){ .fd = o[0], .events = POLLIN };
        if (e[0] >= 0) fds[nf++] = (struct pollfd){ .fd = e[0], .events = POLLIN };
        if (in[1] >= 0) fds[nf++] = (struct pollfd){ .fd = in[1], .events = POLLOUT };
        poll(fds, (nfds_t)nf, 20);
        for (int i = 0; i < nf; i++) {
            int fd = fds[i].fd;
            if (fd == in[1]) {
                if (!(fds[i].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
                ssize_t w = write(fd, p->input + written, input_len - written);
                if (w > 0) written += (size_t)w;
                if (w < 0 && errno != EAGAIN) written = input_len;
                if (written >= input_len) { close(in[1]); in[1] = -1; }
                continue;
            }
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t r = read(fd, chunk, sizeof chunk);
            if (r > 0) {
                sa_keep(fd == o[0] ? out : err, chunk, (size_t)r, dropped);
            } else if (r == 0 || errno != EAGAIN) {
                close(fd);
                if (fd == o[0]) o[0] = -1; else e[0] = -1;
            }
        }
        if (in[1] >= 0 && input_len == 0) { close(in[1]); in[1] = -1; }
        if (!exited && waitpid(pid, status, WNOHANG) == pid) exited = 1;
        if (exited) {
            for (int k = 0; k < 2; k++) {
                int fd = k ? e[0] : o[0];
                ssize_t r;
                while (fd >= 0 && (r = read(fd, chunk, sizeof chunk)) > 0)
                    sa_keep(k ? err : out, chunk, (size_t)r, dropped);
            }
            break;
        }
        if (sa_aborted(x)) { result = SA_ABORTED; break; }
        if (p->timeout_ms > 0 && sa_now_ms() - start > p->timeout_ms) { result = SA_TIMEOUT; break; }
    }
    if (o[0] >= 0) close(o[0]);
    if (e[0] >= 0) close(e[0]);
    if (in[1] >= 0) close(in[1]);
    if (!exited) {
        kill(-pid, SIGTERM);
        for (int i = 0; i < 50 && waitpid(pid, status, WNOHANG) != pid; i++) usleep(20000);
        kill(-pid, SIGKILL);
        waitpid(pid, status, 0);
    }
    return result;
}

/* ---------- session store ---------- */

static void sa_close_file(sa_agent *x) {
    if (x->fp) fclose(x->fp);
    x->fp = NULL;
}

static void sa_write_line(sa_agent *x, const cJSON *line) {
    if (x->st.ephemeral) return;
    if (!x->fp) {
        char dir[4096];
        if (!core_agent_session_dir(x->st.cwd ? x->st.cwd : ".", dir, sizeof dir)) return;
        snprintf(x->path, sizeof x->path, "%s/%s.jsonl", dir, x->id);
        struct stat st;
        int fresh = stat(x->path, &st) != 0;
        x->fp = fopen(x->path, "a");
        if (!x->fp) return;
        if (fresh) {
            cJSON *h = cJSON_CreateObject();
            cJSON_AddStringToObject(h, "type", "session");
            cJSON_AddStringToObject(h, "id", x->id);
            cJSON_AddStringToObject(h, "cwd", x->st.cwd ? x->st.cwd : "");
            cJSON_AddNumberToObject(h, "created", (double)time(NULL));
            char *s = cJSON_PrintUnformatted(h);
            if (s) { fputs(s, x->fp); fputc('\n', x->fp); }
            free(s);
            cJSON_Delete(h);
        }
    }
    char *s = cJSON_PrintUnformatted(line);
    if (s) { fputs(s, x->fp); fputc('\n', x->fp); fflush(x->fp); }
    free(s);
}

static void sa_store(sa_agent *x, cJSON *line) {
    sa_write_line(x, line);
    cJSON_AddItemToArray(x->msgs, line);
}

static cJSON *sa_line(const char *type, int meta, cJSON *message) {
    cJSON *l = cJSON_CreateObject();
    cJSON_AddStringToObject(l, "type", type);
    if (meta) cJSON_AddTrueToObject(l, "isMeta");
    cJSON_AddItemToObject(l, "message", message);
    return l;
}

static cJSON *sa_user_line(const char *text, int meta) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text ? text : "");
    return sa_line("user", meta, m);
}

static cJSON *sa_tool_line(const char *id, const char *name, const char *text, int failed) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "tool");
    cJSON_AddStringToObject(m, "tool_use_id", id);
    cJSON_AddStringToObject(m, "name", name ? name : "");
    cJSON_AddStringToObject(m, "content", text ? text : "");
    if (failed) cJSON_AddTrueToObject(m, "is_error");
    return sa_line("tool", 0, m);
}

static const char *sa_role(const cJSON *line) {
    return sa_jstr(cJSON_GetObjectItem(line, "message"), "role");
}

/* Tool calls of the last assistant message that have no result after it. */
static void sa_repair(sa_agent *x) {
    int n = cJSON_GetArraySize(x->msgs), last = -1;
    for (int i = n - 1; i >= 0; i--) {
        const char *role = sa_role(cJSON_GetArrayItem(x->msgs, i));
        if (role && !strcmp(role, "assistant")) { last = i; break; }
    }
    if (last < 0) return;
    const cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(x->msgs, last),
                                                                   "message"), "content");
    const cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *kind = sa_jstr(block, "type"), *id = sa_jstr(block, "id");
        if (!kind || strcmp(kind, "tool_use") || !id) continue;
        int answered = 0;
        for (int i = last + 1; i < n && !answered; i++) {
            const cJSON *m = cJSON_GetObjectItem(cJSON_GetArrayItem(x->msgs, i), "message");
            const char *tid = sa_jstr(m, "tool_use_id");
            answered = tid && !strcmp(tid, id);
        }
        if (!answered) sa_store(x, sa_tool_line(id, sa_jstr(block, "name"), "interrupted", 1));
    }
}

static cJSON *sa_summary_line(const char *summary) {
    sa_buf b = {0};
    sa_puts(&b, "The earlier conversation was compacted. Summary:\n\n");
    sa_puts(&b, summary);
    cJSON *l = sa_user_line(sa_str(&b), 1);
    sa_free(&b);
    return l;
}

static int sa_load(sa_agent *x, const char *id, int fork) {
    char dir[4096], path[4200];
    if (!core_agent_session_dir(x->st.cwd ? x->st.cwd : ".", dir, sizeof dir)) return 0;
    snprintf(path, sizeof path, "%s/%s.jsonl", dir, id);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(x->err, sizeof x->err, "no core session %s in %s", id, dir);
        return 0;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    sa_buf copy = {0};
    while ((n = getline(&line, &cap, f)) > 0) {
        cJSON *l = cJSON_ParseWithLength(line, (size_t)n);
        const char *type = sa_jstr(l, "type");
        if (!type || !strcmp(type, "session")) { cJSON_Delete(l); continue; }
        if (fork) sa_put(&copy, line, (size_t)n);
        if (!strcmp(type, "compaction")) {
            cJSON_Delete(x->msgs);
            x->msgs = cJSON_CreateArray();
            const char *summary = sa_jstr(l, "summary");
            cJSON_AddItemToArray(x->msgs, sa_summary_line(summary ? summary : ""));
            cJSON_Delete(l);
            continue;
        }
        if (!cJSON_GetObjectItem(l, "message")) { cJSON_Delete(l); continue; }
        const cJSON *usage = cJSON_GetObjectItem(cJSON_GetObjectItem(l, "message"), "usage");
        if (usage) {
            x->cost += sa_jnum(usage, "cost");
            if (sa_jnum(usage, "totalTokens") > 0) x->ctx_tokens = (long)sa_jnum(usage, "totalTokens");
        }
        cJSON_AddItemToArray(x->msgs, l);
    }
    free(line);
    fclose(f);
    if (fork) {
        sa_new_id(x->id, sizeof x->id);
        snprintf(x->path, sizeof x->path, "%s/%s.jsonl", dir, x->id);
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "type", "session");
        cJSON_AddStringToObject(h, "id", x->id);
        cJSON_AddStringToObject(h, "cwd", x->st.cwd ? x->st.cwd : "");
        cJSON_AddStringToObject(h, "parent", id);
        cJSON_AddNumberToObject(h, "created", (double)time(NULL));
        char *hs = cJSON_PrintUnformatted(h);
        cJSON_Delete(h);
        sa_buf all = {0};
        sa_puts(&all, hs);
        sa_puts(&all, "\n");
        sa_put(&all, sa_str(&copy), copy.n);
        free(hs);
        if (!x->st.ephemeral) sa_write_file(x->path, sa_str(&all), all.n);
        sa_free(&all);
    } else {
        snprintf(x->id, sizeof x->id, "%s", id);
        snprintf(x->path, sizeof x->path, "%s", path);
    }
    sa_free(&copy);
    sa_repair(x);
    return 1;
}

/* ---------- hooks ---------- */

static cJSON *sa_hooks_load(void) {
    char path[4096];
    if (!sa_app_config_file("hooks.json", path, sizeof path)) return NULL;
    char *text = sa_slurp(path, NULL);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    return j;
}

static int sa_matches(const char *pattern, const char *subject) {
    if (!pattern || !*pattern || !strcmp(pattern, "*") || !subject) return 1;
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) return !strcmp(pattern, subject);
    int ok = regexec(&re, subject, 0, NULL, 0) == 0;
    regfree(&re);
    return ok;
}

/* Runs the hooks for one event. Returns 1 when a hook blocks; its reason goes
 * to *reason, and context a hook adds goes to *context. */
static int sa_hook(sa_agent *x, const char *event, const char *match, cJSON *payload,
                   sa_buf *context, sa_buf *reason) {
    const cJSON *groups = cJSON_GetObjectItem(cJSON_GetObjectItem(x->hooks, "hooks"), event);
    if (!cJSON_GetArraySize(groups)) { cJSON_Delete(payload); return 0; }
    cJSON_AddStringToObject(payload, "session_id", x->id);
    cJSON_AddStringToObject(payload, "transcript_path", x->path);
    cJSON_AddStringToObject(payload, "cwd", x->st.cwd ? x->st.cwd : "");
    cJSON_AddStringToObject(payload, "hook_event_name", event);
    char *input = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    int blocked = 0;
    const cJSON *group;
    cJSON_ArrayForEach(group, groups) {
        if (!sa_matches(sa_jstr(group, "matcher"), match)) continue;
        const cJSON *h;
        cJSON_ArrayForEach(h, cJSON_GetObjectItem(group, "hooks")) {
            const char *type = sa_jstr(h, "type");
            const cJSON *mine = cJSON_GetObjectItem(h, "scrap");
            if ((type && strcmp(type, "command")) || cJSON_IsFalse(mine)) continue;
            const char *cmd = cJSON_IsString(mine) ? mine->valuestring : sa_jstr(h, "command");
            if (!cmd || !*cmd) continue;
            long timeout = (long)(sa_jnum(h, "timeout") * 1000);
            sa_proc p = { .cmd = cmd, .input = input ? input : "{}",
                          .timeout_ms = timeout > 0 ? timeout : SA_HOOK_TIMEOUT_MS };
            sa_buf out = {0}, err = {0};
            int status, dropped;
            int why = sa_run(x, &p, &out, &err, &status, &dropped);
            int code = (why == SA_RAN && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
            if (code == 2) {
                blocked = 1;
                sa_puts(reason, sa_str(&err));
            } else if (code == 0) {
                const char *text = sa_str(&out);
                while (isspace((unsigned char)*text)) text++;
                cJSON *j = *text == '{' ? cJSON_Parse(text) : NULL;
                if (j) {
                    const cJSON *hso = cJSON_GetObjectItem(j, "hookSpecificOutput");
                    const char *add = sa_jstr(hso, "additionalContext");
                    const char *decision = sa_jstr(hso, "permissionDecision");
                    if (add && context) { sa_puts(context, add); sa_puts(context, "\n"); }
                    if (decision && !strcmp(decision, "deny")) {
                        blocked = 1;
                        sa_puts(reason, sa_jstr(hso, "permissionDecisionReason"));
                    }
                    decision = sa_jstr(j, "decision");
                    if (decision && !strcmp(decision, "block")) {
                        blocked = 1;
                        sa_puts(reason, sa_jstr(j, "reason"));
                    }
                    if (cJSON_IsFalse(cJSON_GetObjectItem(j, "continue"))) {
                        blocked = 1;
                        sa_puts(reason, sa_jstr(j, "stopReason"));
                    }
                    cJSON_Delete(j);
                } else if (context && *text) {
                    sa_puts(context, text);
                    sa_puts(context, "\n");
                }
            }
            sa_free(&out);
            sa_free(&err);
            if (why == SA_ABORTED) { free(input); return blocked; }
        }
    }
    free(input);
    return blocked;
}

static cJSON *sa_payload(void) { return cJSON_CreateObject(); }

static const char *sa_hook_tool_name(const char *name) {
    if (!strcmp(name, "read")) return "Read";
    if (!strcmp(name, "write")) return "Write";
    if (!strcmp(name, "edit")) return "Edit";
    if (!strcmp(name, "bash")) return "Bash";
    return name;
}

static cJSON *sa_hook_tool_input(const cJSON *input) {
    cJSON *in = cJSON_Duplicate(input, 1);
    if (!cJSON_IsObject(in)) { cJSON_Delete(in); return cJSON_CreateObject(); }
    static const char *const ALIASES[][2] = {
        {"path", "file_path"}, {"oldText", "old_string"}, {"newText", "new_string"},
    };
    for (size_t i = 0; i < sizeof ALIASES / sizeof *ALIASES; i++) {
        const cJSON *v = cJSON_GetObjectItem(in, ALIASES[i][0]);
        if (v && !cJSON_GetObjectItem(in, ALIASES[i][1]))
            cJSON_AddItemToObject(in, ALIASES[i][1], cJSON_Duplicate(v, 1));
    }
    return in;
}

/* ---------- system prompt ---------- */

static void sa_add_file(sa_buf *out, const char *path, int *any) {
    char *text = sa_slurp(path, NULL);
    if (!text) return;
    if (!*any) sa_puts(out, "\n\n# Project context\n");
    *any = 1;
    sa_printf(out, "\n## %s\n\n%s\n", path, text);
    free(text);
}

static void sa_context_files(sa_agent *x, sa_buf *out) {
    const char *home = getenv("HOME");
    char path[4200], cwd[4096], dir[4096];
    int any = 0;
    if (home) {
        snprintf(path, sizeof path, "%s/.agents/AGENTS.md", home);
        sa_add_file(out, path, &any);
    }
    if (!x->st.cwd || !realpath(x->st.cwd, cwd)) return;
    size_t len = strlen(cwd);
    for (size_t i = 0; i <= len; i++) {
        if (i < len && cwd[i] != '/') continue;
        if (i == len && len == 1) continue;
        snprintf(dir, sizeof dir, "%.*s", (int)i, cwd);
        struct stat a, c;
        snprintf(path, sizeof path, "%s/AGENTS.md", dir);
        int have = stat(path, &a) == 0;
        sa_add_file(out, path, &any);
        snprintf(path, sizeof path, "%s/CLAUDE.md", dir);
        if (!have || stat(path, &c) != 0 || a.st_ino != c.st_ino || a.st_dev != c.st_dev)
            sa_add_file(out, path, &any);
        if (home && !strcmp(dir, home)) continue;
        snprintf(path, sizeof path, "%s/.agents/AGENTS.md", dir);
        sa_add_file(out, path, &any);
    }
}

static void sa_frontmatter(const char *text, char *name, size_t nsize, sa_buf *desc) {
    if (strncmp(text, "---", 3)) return;
    const char *p = strchr(text, '\n');
    int folding = 0;
    while (p && *++p && strncmp(p, "---", 3)) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (folding && len && isspace((unsigned char)*p)) {
            while (len && isspace((unsigned char)*p)) { p++; len--; }
            if (desc->n) sa_puts(desc, " ");
            sa_put(desc, p, len);
        } else {
            folding = 0;
            if (!strncmp(p, "name:", 5)) {
                const char *v = p + 5;
                while (*v == ' ') v++;
                snprintf(name, nsize, "%.*s", (int)(len - (size_t)(v - p)), v);
            } else if (!strncmp(p, "description:", 12)) {
                const char *v = p + 12;
                while (*v == ' ') v++;
                size_t vlen = len - (size_t)(v - p);
                if (vlen && (*v == '>' || *v == '|')) folding = 1;
                else sa_put(desc, v, vlen);
            }
        }
        p = eol;
    }
}

static void sa_skills_in(const char *dir, sa_buf *out, int *any) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4200];
        snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, e->d_name);
        char *text = sa_slurp(path, NULL);
        if (!text) continue;
        char name[256];
        snprintf(name, sizeof name, "%s", e->d_name);
        sa_buf desc = {0};
        sa_frontmatter(text, name, sizeof name, &desc);
        if (!*any)
            sa_puts(out, "\n\n# Skills\n\nA skill holds instructions for one kind of task. When a "
                         "task matches a skill's description, read its SKILL.md with the read tool "
                         "before acting.\n\n");
        *any = 1;
        sa_printf(out, "- %s: %s (%s)\n", name, sa_str(&desc), path);
        sa_free(&desc);
        free(text);
    }
    closedir(d);
}

static void sa_build_system(sa_agent *x) {
    sa_buf b = {0};
    if (!x->st.disable_tools) {
        sa_puts(&b, SA_SYSTEM);
        char date[64];
        time_t now = time(NULL);
        strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
        sa_printf(&b, "\n\nDate: %s\nWorking directory: %s", date, x->st.cwd ? x->st.cwd : ".");
        if (!x->st.ephemeral) {
            sa_context_files(x, &b);
            int any = 0;
            char dir[4200], home_real[PATH_MAX] = "", real[PATH_MAX];
            const char *home = getenv("HOME");
            if (home) {
                snprintf(dir, sizeof dir, "%s/.agents/skills", home);
                if (realpath(dir, home_real)) sa_skills_in(home_real, &b, &any);
            }
            if (x->st.cwd) {
                snprintf(dir, sizeof dir, "%s/.agents/skills", x->st.cwd);
                if (realpath(dir, real) && strcmp(real, home_real)) sa_skills_in(real, &b, &any);
            }
        }
        if (x->hook_context.n) {
            sa_puts(&b, "\n\n");
            sa_puts(&b, sa_str(&x->hook_context));
        }
    }
    if (x->st.system) {
        if (b.n) sa_puts(&b, "\n\n");
        sa_puts(&b, x->st.system);
    }
    free(x->system);
    x->system = b.p;
}

static void sa_session_start(sa_agent *x, const char *source) {
    sa_clear(&x->hook_context);
    if (!x->st.ephemeral && !x->st.disable_tools) {
        cJSON *p = sa_payload();
        cJSON_AddStringToObject(p, "source", source);
        sa_buf reason = {0};
        sa_hook(x, "SessionStart", source, p, &x->hook_context, &reason);
        sa_free(&reason);
    }
    sa_build_system(x);
}

/* ---------- request ---------- */

static cJSON *sa_wire(const cJSON *line) {
    const cJSON *m = cJSON_GetObjectItem(line, "message");
    const char *role = sa_jstr(m, "role");
    cJSON *w = cJSON_CreateObject();
    if (!role) role = "user";
    if (!strcmp(role, "tool")) {
        cJSON_AddStringToObject(w, "role", "tool");
        cJSON_AddStringToObject(w, "tool_call_id", sa_jstr(m, "tool_use_id") ? sa_jstr(m, "tool_use_id") : "");
        cJSON_AddStringToObject(w, "content", sa_jstr(m, "content") ? sa_jstr(m, "content") : "");
        return w;
    }
    if (strcmp(role, "assistant")) {
        cJSON_AddStringToObject(w, "role", "user");
        cJSON_AddItemToObject(w, "content", cJSON_Duplicate(cJSON_GetObjectItem(m, "content"), 1));
        return w;
    }
    cJSON_AddStringToObject(w, "role", "assistant");
    sa_buf text = {0};
    cJSON *calls = cJSON_CreateArray();
    const cJSON *block;
    cJSON_ArrayForEach(block, cJSON_GetObjectItem(m, "content")) {
        const char *kind = sa_jstr(block, "type");
        if (!kind) continue;
        if (!strcmp(kind, "text")) {
            sa_puts(&text, sa_jstr(block, "text"));
        } else if (!strcmp(kind, "tool_use")) {
            cJSON *c = cJSON_CreateObject(), *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "id", sa_jstr(block, "id") ? sa_jstr(block, "id") : "");
            cJSON_AddStringToObject(c, "type", "function");
            cJSON_AddStringToObject(fn, "name", sa_jstr(block, "name") ? sa_jstr(block, "name") : "");
            char *args = cJSON_PrintUnformatted(cJSON_GetObjectItem(block, "input"));
            cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
            free(args);
            cJSON_AddItemToObject(c, "function", fn);
            cJSON_AddItemToArray(calls, c);
        }
    }
    if (text.n || !cJSON_GetArraySize(calls)) cJSON_AddStringToObject(w, "content", sa_str(&text));
    else cJSON_AddNullToObject(w, "content");
    sa_free(&text);
    if (cJSON_GetArraySize(calls)) cJSON_AddItemToObject(w, "tool_calls", calls);
    else cJSON_Delete(calls);
    const cJSON *details = cJSON_GetObjectItem(m, "reasoning_details");
    if (cJSON_GetArraySize(details)) cJSON_AddItemToObject(w, "reasoning_details", cJSON_Duplicate(details, 1));
    return w;
}

static void sa_effort(sa_agent *x, cJSON *root) {
    const char *e = x->st.effort, *style = x->route.effort_style;
    if (sa_unset(e) || !style) return;
    int off = !strcmp(e, "off") || !strcmp(e, "none");
    if (!strcmp(style, "openrouter")) {
        cJSON *r = cJSON_CreateObject();
        if (off) cJSON_AddFalseToObject(r, "enabled");
        else cJSON_AddStringToObject(r, "effort", e);
        cJSON_AddItemToObject(root, "reasoning", r);
    } else if (!strcmp(style, "openai")) {
        cJSON_AddStringToObject(root, "reasoning_effort", off ? "none" : e);
    }
}

static char *sa_body(sa_agent *x, int upto, const char *extra_user, int tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", x->route.model);
    cJSON_AddTrueToObject(root, "stream");
    cJSON *so = cJSON_CreateObject();
    cJSON_AddTrueToObject(so, "include_usage");
    cJSON_AddItemToObject(root, "stream_options", so);
    cJSON *msgs = cJSON_CreateArray();
    if (x->system && *x->system) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "role", "system");
        cJSON_AddStringToObject(s, "content", x->system);
        cJSON_AddItemToArray(msgs, s);
    }
    for (int i = 0; i < upto; i++) cJSON_AddItemToArray(msgs, sa_wire(cJSON_GetArrayItem(x->msgs, i)));
    if (extra_user) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", extra_user);
        cJSON_AddItemToArray(msgs, u);
    }
    cJSON_AddItemToObject(root, "messages", msgs);
    if (!x->st.disable_tools) {
        cJSON_AddItemToObject(root, "tools", cJSON_Parse(SA_TOOLS));
        if (!tools) cJSON_AddStringToObject(root, "tool_choice", "none");
    }
    sa_effort(x, root);
    if (!strcmp(x->route.provider, "openrouter")) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddTrueToObject(u, "include");
        cJSON_AddItemToObject(root, "usage", u);
    }
    const cJSON *kv;
    cJSON_ArrayForEach(kv, x->route.extra) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, kv->string);
        cJSON_AddItemToObject(root, kv->string, cJSON_Duplicate(kv, 1));
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

typedef struct { char *id, *name; sa_buf args; } sa_call;

typedef struct {
    sa_agent *x;
    int emit;
    sa_buf line, raw, text, reasoning;
    cJSON *details;
    sa_call *calls;
    int ncalls;
    long in, out, cached;
    double cost;
    char err[512];
} sa_resp;

static void sa_resp_free(sa_resp *r) {
    sa_free(&r->line); sa_free(&r->raw); sa_free(&r->text); sa_free(&r->reasoning);
    cJSON_Delete(r->details);
    for (int i = 0; i < r->ncalls; i++) { free(r->calls[i].id); free(r->calls[i].name); sa_free(&r->calls[i].args); }
    free(r->calls);
    sa_agent *x = r->x;
    int emit = r->emit;
    memset(r, 0, sizeof *r);
    r->x = x;
    r->emit = emit;
}

static void sa_details_merge(sa_resp *r, const cJSON *arr) {
    if (!r->details) r->details = cJSON_CreateArray();
    const cJSON *d;
    cJSON_ArrayForEach(d, arr) {
        const cJSON *idx = cJSON_GetObjectItem(d, "index");
        cJSON *have = NULL, *e;
        if (cJSON_IsNumber(idx)) {
            cJSON_ArrayForEach(e, r->details) {
                const cJSON *ei = cJSON_GetObjectItem(e, "index");
                if (cJSON_IsNumber(ei) && ei->valueint == idx->valueint) { have = e; break; }
            }
        }
        if (!have) { cJSON_AddItemToArray(r->details, cJSON_Duplicate(d, 1)); continue; }
        const cJSON *f;
        cJSON_ArrayForEach(f, d) {
            cJSON *old = cJSON_GetObjectItemCaseSensitive(have, f->string);
            int text = !strcmp(f->string, "text") || !strcmp(f->string, "summary") ||
                       !strcmp(f->string, "data");
            if (text && cJSON_IsString(f) && cJSON_IsString(old)) {
                sa_buf b = {0};
                sa_puts(&b, old->valuestring);
                sa_puts(&b, f->valuestring);
                cJSON_ReplaceItemInObjectCaseSensitive(have, f->string, cJSON_CreateString(sa_str(&b)));
                sa_free(&b);
            } else if (!old) {
                cJSON_AddItemToObject(have, f->string, cJSON_Duplicate(f, 1));
            } else if (!cJSON_IsNull(f)) {
                cJSON_ReplaceItemInObjectCaseSensitive(have, f->string, cJSON_Duplicate(f, 1));
            }
        }
    }
}

static void sa_chunk(sa_resp *r, const cJSON *j) {
    const cJSON *err = cJSON_GetObjectItem(j, "error");
    if (err) {
        const char *msg = cJSON_IsString(err) ? err->valuestring : sa_jstr(err, "message");
        const char *raw = sa_jstr(cJSON_GetObjectItem(err, "metadata"), "raw");
        snprintf(r->err, sizeof r->err, "%s%s%s", msg ? msg : "stream error", raw ? ": " : "",
                 raw ? raw : "");
        return;
    }
    const cJSON *usage = cJSON_GetObjectItem(j, "usage");
    if (cJSON_IsObject(usage)) {
        r->in = (long)sa_jnum(usage, "prompt_tokens");
        r->out = (long)sa_jnum(usage, "completion_tokens");
        r->cached = (long)sa_jnum(cJSON_GetObjectItem(usage, "prompt_tokens_details"), "cached_tokens");
        r->cost = sa_jnum(usage, "cost");
    }
    const cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0), "delta");
    if (!delta) return;
    const char *reason = sa_jstr(delta, "reasoning");
    if (!reason) reason = sa_jstr(delta, "reasoning_content");
    if (reason && *reason) {
        sa_puts(&r->reasoning, reason);
        if (r->emit) backend_delta(&r->x->st, BACKEND_EV_THINKING, reason);
    }
    const cJSON *details = cJSON_GetObjectItem(delta, "reasoning_details");
    if (cJSON_GetArraySize(details)) sa_details_merge(r, details);
    const char *content = sa_jstr(delta, "content");
    if (content && !r->text.n) content += strspn(content, " \t\r\n");
    if (content && *content) {
        sa_puts(&r->text, content);
        if (r->emit) backend_delta(&r->x->st, BACKEND_EV_ASSISTANT, content);
    }
    const cJSON *tc;
    int pos = 0;
    cJSON_ArrayForEach(tc, cJSON_GetObjectItem(delta, "tool_calls")) {
        const cJSON *idx = cJSON_GetObjectItem(tc, "index");
        int i = cJSON_IsNumber(idx) ? idx->valueint : pos;
        pos++;
        if (i < 0 || i > 255) continue;
        if (i >= r->ncalls) {
            sa_call *g = realloc(r->calls, (size_t)(i + 1) * sizeof *g);
            if (!g) continue;
            memset(g + r->ncalls, 0, (size_t)(i + 1 - r->ncalls) * sizeof *g);
            r->calls = g;
            r->ncalls = i + 1;
        }
        sa_call *c = &r->calls[i];
        const char *id = sa_jstr(tc, "id");
        const cJSON *fn = cJSON_GetObjectItem(tc, "function");
        const char *name = sa_jstr(fn, "name");
        if (id && *id && !c->id) c->id = strdup(id);
        if (name && *name && !c->name) c->name = strdup(name);
        sa_puts(&c->args, sa_jstr(fn, "arguments"));
    }
}

static void sa_sse_line(sa_resp *r, char *line) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[--n] = '\0';
    if (strncmp(line, "data:", 5)) return;
    const char *p = line + 5;
    while (*p == ' ') p++;
    if (!*p || !strcmp(p, "[DONE]")) return;
    cJSON *j = cJSON_Parse(p);
    if (j) sa_chunk(r, j);
    cJSON_Delete(j);
}

static size_t sa_on_data(char *p, size_t sz, size_t nm, void *ud) {
    sa_resp *r = ud;
    size_t n = sz * nm;
    if (r->raw.n < 65536) sa_put(&r->raw, p, n);
    sa_put(&r->line, p, n);
    char *start = r->line.p, *nl;
    while (start && (nl = memchr(start, '\n', r->line.n - (size_t)(start - r->line.p)))) {
        *nl = '\0';
        sa_sse_line(r, start);
        start = nl + 1;
    }
    if (start && start != r->line.p) {
        size_t rest = r->line.n - (size_t)(start - r->line.p);
        memmove(r->line.p, start, rest + 1);
        r->line.n = rest;
    }
    return n;
}

/* One HTTP request. Returns the status code, 0 on a transport failure. */
static long sa_http(sa_agent *x, const char *body, sa_resp *r, int *interrupted) {
    CURL *c = curl_easy_init();
    if (!c) { snprintf(r->err, sizeof r->err, "curl init failed"); return 0; }
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: text/event-stream");
    char buf[2048];
    if (x->route.key) {
        snprintf(buf, sizeof buf, "Authorization: Bearer %s", x->route.key);
        h = curl_slist_append(h, buf);
    }
    if (!strcmp(x->route.provider, "openrouter")) {
        h = curl_slist_append(h, "HTTP-Referer: https://github.com/jhickner/scrap");
        h = curl_slist_append(h, "X-Title: scrap");
    }
    const cJSON *kv;
    cJSON_ArrayForEach(kv, x->route.headers) {
        if (!cJSON_IsString(kv)) continue;
        snprintf(buf, sizeof buf, "%s: %s", kv->string, kv->valuestring);
        h = curl_slist_append(h, buf);
    }
    char url[2048];
    snprintf(url, sizeof url, "%s/chat/completions", x->route.base_url);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sa_on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 600L);
    CURLM *m = curl_multi_init();
    curl_multi_add_handle(m, c);
    int running = 1;
    CURLcode result = CURLE_OK;
    while (running) {
        curl_multi_perform(m, &running);
        if (!running) break;
        if (sa_aborted(x)) { *interrupted = 1; break; }
        curl_multi_poll(m, NULL, 0, 20, NULL);
    }
    CURLMsg *msg;
    int left;
    while ((msg = curl_multi_info_read(m, &left)))
        if (msg->msg == CURLMSG_DONE) result = msg->data.result;
    long code = 0;
    if (!*interrupted) {
        if (result != CURLE_OK) snprintf(r->err, sizeof r->err, "%s", curl_easy_strerror(result));
        else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_multi_remove_handle(m, c);
    curl_multi_cleanup(m);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    if (r->line.n) sa_sse_line(r, r->line.p);
    if (code && code != 200 && !r->err[0]) {
        cJSON *j = cJSON_Parse(sa_str(&r->raw));
        if (j) sa_chunk(r, j);
        cJSON_Delete(j);
        if (!r->err[0]) snprintf(r->err, sizeof r->err, "%.400s", sa_str(&r->raw));
    }
    return code;
}

static int sa_wait(sa_agent *x, long ms) {
    for (long t = 0; t < ms; t += 20) {
        if (sa_aborted(x)) return 1;
        usleep(20000);
    }
    return 0;
}

/* A model request with retries for transport errors, 429 and 5xx. Returns 1
 * on success. */
static int sa_request(sa_agent *x, const char *body, sa_resp *r, int *interrupted) {
    for (int attempt = 0;; attempt++) {
        long code = sa_http(x, body, r, interrupted);
        if (*interrupted) return 0;
        if (code == 200 && !r->err[0]) return 1;
        int retry = (code == 0 || code == 429 || code >= 500 || (code == 200 && r->err[0])) &&
                    !r->text.n && !r->ncalls && attempt < SA_RETRIES;
        char why[700];
        if (code && code != 200) snprintf(why, sizeof why, "HTTP %ld: %s", code, r->err);
        else snprintf(why, sizeof why, "%s", r->err[0] ? r->err : "request failed");
        if (!retry) {
            snprintf(x->err, sizeof x->err, "%s: %s", x->route.full, why);
            return 0;
        }
        char note[800];
        snprintf(note, sizeof note, "%s; retrying in %d s", why, 2 << attempt);
        sa_warn(x, note);
        sa_resp_free(r);
        if (sa_wait(x, 2000L << attempt)) { *interrupted = 1; return 0; }
    }
}

/* ---------- compaction ---------- */

static int sa_compact(sa_agent *x) {
    int n = cJSON_GetArraySize(x->msgs), cut = -1;
    if (n < 2) return 0;
    for (int i = n - 1; i > 0; i--) {
        const cJSON *l = cJSON_GetArrayItem(x->msgs, i);
        const char *role = sa_role(l);
        if (role && !strcmp(role, "user") && !cJSON_IsTrue(cJSON_GetObjectItem(l, "isMeta"))) { cut = i; break; }
    }
    if (cut > 0) {
        size_t tail = 0;
        for (int i = cut; i < n; i++) {
            char *s = cJSON_PrintUnformatted(cJSON_GetArrayItem(x->msgs, i));
            tail += s ? strlen(s) : 0;
            free(s);
        }
        if ((long)(tail / 4) > x->window / 2) cut = -1;
    }
    if (cut <= 0) cut = n;
    sa_warn(x, "compacting the conversation");
    char *body = sa_body(x, cut, SA_SUMMARY_PROMPT, 0);
    sa_resp r = { .x = x, .emit = 0 };
    int interrupted = 0;
    int ok = body && sa_request(x, body, &r, &interrupted) && r.text.n;
    free(body);
    if (!ok) {
        sa_resp_free(&r);
        if (!interrupted) sa_warn(x, "compaction failed");
        return 0;
    }
    x->cost += r.cost;
    cJSON *rec = cJSON_CreateObject();
    cJSON_AddStringToObject(rec, "type", "compaction");
    cJSON_AddStringToObject(rec, "summary", sa_str(&r.text));
    sa_write_line(x, rec);
    cJSON_Delete(rec);
    cJSON *next = cJSON_CreateArray();
    cJSON_AddItemToArray(next, sa_summary_line(sa_str(&r.text)));
    if (cut == n) {
        cJSON_AddItemToArray(next, sa_user_line("Continue from the summary.", 1));
    }
    for (int i = cut; i < n; i++) {
        cJSON *l = cJSON_Duplicate(cJSON_GetArrayItem(x->msgs, i), 1);
        if (!cJSON_GetObjectItem(l, "isMeta")) cJSON_AddTrueToObject(l, "isMeta");
        cJSON_AddItemToArray(next, l);
    }
    cJSON *l;
    int i = 0;
    cJSON_ArrayForEach(l, next) if (i++ > 0) sa_write_line(x, l);
    cJSON_Delete(x->msgs);
    x->msgs = next;
    x->ctx_tokens = 0;
    sa_resp_free(&r);
    sa_session_start(x, "compact");
    return 1;
}

static void sa_maybe_compact(sa_agent *x) {
    long window = x->window, reserve = window / 8;
    if (reserve < 16384) reserve = 16384;
    if (reserve > window / 2) reserve = window / 2;
    if (x->ctx_tokens > 0 && window > 0 && x->ctx_tokens >= window - reserve) sa_compact(x);
}

/* ---------- tools ---------- */

static void sa_resolve(sa_agent *x, const char *path, char *out, size_t size) {
    const char *home = getenv("HOME");
    if (path[0] == '/') snprintf(out, size, "%s", path);
    else if (path[0] == '~' && (path[1] == '/' || !path[1]) && home) snprintf(out, size, "%s%s", home, path + 1);
    else snprintf(out, size, "%s/%s", x->st.cwd ? x->st.cwd : ".", path);
}

static int sa_tool_read(sa_agent *x, const cJSON *in, sa_buf *out) {
    const char *arg = sa_jstr(in, "path");
    if (!arg) { sa_puts(out, "path is required"); return 1; }
    char path[4096];
    sa_resolve(x, arg, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) { sa_printf(out, "%s: %s", arg, strerror(errno)); return 1; }
    if (S_ISDIR(st.st_mode)) { sa_printf(out, "%s is a directory; use bash to list it", arg); return 1; }
    size_t len = 0;
    char *text = sa_slurp(path, &len);
    if (!text) { sa_printf(out, "%s: cannot read", arg); return 1; }
    if (memchr(text, '\0', len < 8192 ? len : 8192)) {
        sa_printf(out, "%s is a binary file (%zu bytes)", arg, len);
        free(text);
        return 1;
    }
    long offset = (long)sa_jnum(in, "offset"), limit = (long)sa_jnum(in, "limit");
    if (offset < 1) offset = 1;
    if (limit < 1 || limit > SA_READ_LINES) limit = SA_READ_LINES;
    long total = 0;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') total++;
    if (len && text[len - 1] != '\n') total++;
    if (!len) { sa_puts(out, "(empty file)"); free(text); return 0; }
    if (offset > total) {
        sa_printf(out, "offset %ld is past the end of %s (%ld lines)", offset, arg, total);
        free(text);
        return 1;
    }
    long line = 1, last = offset - 1;
    const char *p = text, *end = text + len;
    while (line < offset && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        p = nl ? nl + 1 : end;
        line++;
    }
    while (p < end && line < offset + limit && out->n < SA_READ_BYTES) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n > SA_LINE_CHARS) {
            sa_put(out, p, SA_LINE_CHARS);
            sa_puts(out, " [line truncated]");
        } else {
            sa_put(out, p, n);
        }
        sa_puts(out, "\n");
        p = nl ? nl + 1 : end;
        last = line++;
    }
    if (last < total)
        sa_printf(out, "\n[Showing lines %ld-%ld of %ld. Use offset=%ld to continue.]", offset, last,
                  total, last + 1);
    free(text);
    return 0;
}

static int sa_tool_write(sa_agent *x, const cJSON *in, sa_buf *out) {
    const char *arg = sa_jstr(in, "path"), *content = sa_jstr(in, "content");
    if (!arg || !content) { sa_puts(out, "path and content are required"); return 1; }
    char path[4096], dir[4096];
    sa_resolve(x, arg, path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; sa_mkdirs(dir, 0755); }
    FILE *f = fopen(path, "wb");
    size_t n = strlen(content);
    if (!f || fwrite(content, 1, n, f) != n) {
        sa_printf(out, "%s: %s", arg, strerror(errno));
        if (f) fclose(f);
        return 1;
    }
    if (fclose(f) != 0) { sa_printf(out, "%s: %s", arg, strerror(errno)); return 1; }
    sa_printf(out, "Wrote %zu bytes to %s", n, arg);
    return 0;
}

static int sa_tool_edit(sa_agent *x, const cJSON *in, sa_buf *out) {
    const char *arg = sa_jstr(in, "path");
    const char *old = sa_jstr(in, "oldText"), *new = sa_jstr(in, "newText");
    if (!old) old = sa_jstr(in, "old_string");
    if (!new) new = sa_jstr(in, "new_string");
    if (!arg || !old || !new) { sa_puts(out, "path, oldText and newText are required"); return 1; }
    if (!*old) { sa_puts(out, "oldText is empty; use write to create a file"); return 1; }
    char path[4096];
    sa_resolve(x, arg, path, sizeof path);
    size_t len = 0;
    char *text = sa_slurp(path, &len);
    if (!text) { sa_printf(out, "%s: cannot read", arg); return 1; }
    size_t olen = strlen(old);
    int count = 0;
    const char *at = NULL;
    for (const char *p = text; (p = strstr(p, old)); p += olen) {
        if (!count) at = p;
        count++;
    }
    if (count != 1) {
        if (!count) sa_printf(out, "oldText was not found in %s. It must match exactly, including whitespace.", arg);
        else sa_printf(out, "oldText matches %d places in %s; include more surrounding text to make it unique.", count, arg);
        free(text);
        return 1;
    }
    sa_buf next = {0};
    sa_put(&next, text, (size_t)(at - text));
    sa_puts(&next, new);
    sa_put(&next, at + olen, len - (size_t)(at - text) - olen);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(sa_str(&next), 1, next.n, f) == next.n;
    if (f) ok &= fclose(f) == 0;
    if (ok) sa_printf(out, "Edited %s", arg);
    else sa_printf(out, "%s: %s", arg, strerror(errno));
    sa_free(&next);
    free(text);
    return !ok;
}

static void sa_tail(sa_buf *out, const sa_buf *all, int dropped) {
    const char *s = sa_str(all);
    size_t n = all->n, start = n > SA_OUT_BYTES ? n - SA_OUT_BYTES : 0;
    int lines = 0;
    for (size_t i = n; i > start; i--) {
        if (s[i - 1] == '\n' && i != n && ++lines >= SA_OUT_LINES) { start = i; break; }
    }
    if (start || dropped) {
        if (start) {
            const char *nl = memchr(s + start, '\n', n - start);
            if (nl && s[start - 1] != '\n') start = (size_t)(nl - s) + 1;
        }
        sa_puts(out, "[output truncated; showing the end]\n");
    }
    sa_put(out, s + start, n - start);
}

static int sa_tool_bash(sa_agent *x, const cJSON *in, sa_buf *out, int *interrupted) {
    const char *cmd = sa_jstr(in, "command");
    if (!cmd) { sa_puts(out, "command is required"); return 1; }
    long timeout = (long)(sa_jnum(in, "timeout") * 1000);
    sa_proc p = { .cmd = cmd, .timeout_ms = timeout, .merge = 1 };
    sa_buf all = {0};
    int status, dropped;
    int why = sa_run(x, &p, &all, NULL, &status, &dropped);
    int failed = 0;
    if (why == SA_SPAWN_FAILED) {
        sa_printf(out, "could not run bash: %s", strerror(errno));
        sa_free(&all);
        return 1;
    }
    sa_tail(out, &all, dropped);
    if (!all.n) sa_puts(out, "(no output)");
    if (why == SA_ABORTED) {
        sa_puts(out, "\n\nCommand aborted");
        *interrupted = 1;
        failed = 1;
    } else if (why == SA_TIMEOUT) {
        sa_printf(out, "\n\nCommand timed out after %ld seconds", timeout / 1000);
        failed = 1;
    } else if (WIFEXITED(status) && WEXITSTATUS(status)) {
        sa_printf(out, "\n\nCommand exited with code %d", WEXITSTATUS(status));
        failed = 1;
    } else if (WIFSIGNALED(status)) {
        sa_printf(out, "\n\nCommand killed by signal %d", WTERMSIG(status));
        failed = 1;
    }
    sa_free(&all);
    return failed;
}

static void sa_run_call(sa_agent *x, sa_call *call, int *interrupted) {
    const char *name = call->name ? call->name : "";
    cJSON *input = cJSON_Parse(call->args.n ? sa_str(&call->args) : "{}");
    int bad = !cJSON_IsObject(input);
    if (bad) { cJSON_Delete(input); input = cJSON_CreateObject(); }
    char *json = cJSON_PrintUnformatted(input);

    backend_flush(&x->st);
    backend_event ev = { .kind = BACKEND_EV_TOOL, .name = name, .input_json = json, .id = call->id };
    backend_emit(&x->st, &ev);

    sa_buf out = {0}, reason = {0}, ctx = {0};
    int failed = 1;
    if (bad) {
        sa_printf(&out, "arguments are not a JSON object: %s", sa_str(&call->args));
    } else {
        cJSON *p = sa_payload();
        cJSON_AddStringToObject(p, "tool_name", sa_hook_tool_name(name));
        cJSON_AddItemToObject(p, "tool_input", sa_hook_tool_input(input));
        if (sa_hook(x, "PreToolUse", sa_hook_tool_name(name), p, NULL, &reason)) {
            sa_printf(&out, "blocked by hook: %s", reason.n ? sa_str(&reason) : "no reason given");
        } else if (!strcmp(name, "read")) {
            failed = sa_tool_read(x, input, &out);
        } else if (!strcmp(name, "write")) {
            failed = sa_tool_write(x, input, &out);
        } else if (!strcmp(name, "edit")) {
            failed = sa_tool_edit(x, input, &out);
        } else if (!strcmp(name, "bash")) {
            failed = sa_tool_bash(x, input, &out, interrupted);
        } else {
            sa_printf(&out, "unknown tool '%s'; the tools are read, write, edit, bash", name);
        }
        if (!*interrupted && !reason.n) {
            p = sa_payload();
            cJSON_AddStringToObject(p, "tool_name", sa_hook_tool_name(name));
            cJSON_AddItemToObject(p, "tool_input", sa_hook_tool_input(input));
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "output", sa_str(&out));
            cJSON_AddBoolToObject(resp, "success", !failed);
            cJSON_AddItemToObject(p, "tool_response", resp);
            sa_clear(&reason);
            if (sa_hook(x, "PostToolUse", sa_hook_tool_name(name), p, &ctx, &reason) || ctx.n) {
                sa_printf(&out, "\n\n[hook] %s%s", sa_str(&reason), sa_str(&ctx));
            }
        }
    }
    backend_event res = { .kind = BACKEND_EV_TOOL_RESULT, .text = sa_str(&out), .failed = failed,
                          .id = call->id };
    backend_emit(&x->st, &res);
    sa_store(x, sa_tool_line(call->id, name, sa_str(&out), failed));
    sa_free(&out);
    sa_free(&reason);
    sa_free(&ctx);
    free(json);
    cJSON_Delete(input);
}

/* ---------- turn ---------- */

static cJSON *sa_assistant_line(sa_agent *x, sa_resp *r, int with_calls) {
    cJSON *m = cJSON_CreateObject(), *content = cJSON_CreateArray();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "model", x->route.full);
    if (r->reasoning.n) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "thinking");
        cJSON_AddStringToObject(b, "text", sa_str(&r->reasoning));
        cJSON_AddItemToArray(content, b);
    }
    if (r->text.n) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", sa_str(&r->text));
        cJSON_AddItemToArray(content, b);
    }
    for (int i = 0; with_calls && i < r->ncalls; i++) {
        sa_call *c = &r->calls[i];
        if (!c->id) {
            char id[32];
            snprintf(id, sizeof id, "call_%d_%ld", i, sa_now_ms());
            c->id = strdup(id);
        }
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "tool_use");
        cJSON_AddStringToObject(b, "id", c->id);
        cJSON_AddStringToObject(b, "name", c->name ? c->name : "");
        cJSON *input = cJSON_Parse(c->args.n ? sa_str(&c->args) : "{}");
        cJSON_AddItemToObject(b, "input", cJSON_IsObject(input) ? input : cJSON_CreateObject());
        if (!cJSON_IsObject(input)) cJSON_Delete(input);
        cJSON_AddItemToArray(content, b);
    }
    cJSON_AddItemToObject(m, "content", content);
    if (cJSON_GetArraySize(r->details)) cJSON_AddItemToObject(m, "reasoning_details", cJSON_Duplicate(r->details, 1));
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "input_tokens", (double)r->in);
    cJSON_AddNumberToObject(u, "output_tokens", (double)r->out);
    cJSON_AddNumberToObject(u, "cache_read_input_tokens", (double)r->cached);
    cJSON_AddNumberToObject(u, "totalTokens", (double)(r->in + r->out));
    if (r->cost) cJSON_AddNumberToObject(u, "cost", r->cost);
    cJSON_AddItemToObject(m, "usage", u);
    return sa_line("assistant", 0, m);
}

static int sa_start(Backend *b, const char *resume);

static char *sa_ask_ex(Backend *b, const char *user, backend_result *meta) {
    sa_agent *x = b->ctx;
    backend_result res = {0};
    snprintf(res.subtype, sizeof res.subtype, "success");
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->started && !sa_start(b, x->st.resume)) {
        if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
        return NULL;
    }
    x->err[0] = '\0';
    if (!sa_route_resolve(x)) {
        if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
        return NULL;
    }

    if (!x->st.ephemeral && !x->st.disable_tools) {
        sa_buf ctx = {0}, reason = {0};
        cJSON *p = sa_payload();
        cJSON_AddStringToObject(p, "prompt", user ? user : "");
        int blocked = sa_hook(x, "UserPromptSubmit", NULL, p, &ctx, &reason);
        if (blocked) {
            snprintf(x->err, sizeof x->err, "prompt blocked by hook: %s", sa_str(&reason));
            sa_free(&ctx);
            sa_free(&reason);
            if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
            return NULL;
        }
        if (ctx.n) {
            sa_buf wrapped = {0};
            sa_printf(&wrapped, "<system-reminder>\n%s</system-reminder>", sa_str(&ctx));
            sa_store(x, sa_user_line(sa_str(&wrapped), 1));
            sa_free(&wrapped);
        }
        sa_free(&ctx);
        sa_free(&reason);
    }
    sa_store(x, sa_user_line(user ? user : "", 0));

    long max_steps = (long)sa_jnum(x->config, "max_steps");
    char *reply = NULL;
    int stop_hook_active = 0;
    for (long step = 1;; step++) {
        if (max_steps > 0 && step > max_steps) {
            snprintf(res.subtype, sizeof res.subtype, "error_max_turns");
            res.is_error = 1;
            snprintf(x->err, sizeof x->err, "stopped after %ld model requests (max_steps)", max_steps);
            break;
        }
        sa_maybe_compact(x);
        char *body = sa_body(x, cJSON_GetArraySize(x->msgs), NULL, 1);
        sa_resp r = { .x = x, .emit = 1 };
        int interrupted = 0;
        int ok = body && sa_request(x, body, &r, &interrupted);
        free(body);
        backend_flush(&x->st);
        res.input_tokens += r.in;
        res.output_tokens += r.out;
        res.cache_read_tokens += r.cached;
        x->cost += r.cost;
        if (r.in + r.out > 0) x->ctx_tokens = r.in + r.out;
        if (interrupted || !ok) {
            if (r.text.n || r.reasoning.n) sa_store(x, sa_assistant_line(x, &r, 0));
            if (r.text.n) { free(reply); reply = strdup(sa_str(&r.text)); }
            if (interrupted) {
                res.interrupted = 1;
                snprintf(res.subtype, sizeof res.subtype, "interrupted");
            } else {
                res.is_error = 1;
                snprintf(res.subtype, sizeof res.subtype, "error");
            }
            sa_resp_free(&r);
            break;
        }
        sa_store(x, sa_assistant_line(x, &r, 1));
        if (r.text.n) { free(reply); reply = strdup(sa_str(&r.text)); }
        if (!r.ncalls) {
            sa_resp_free(&r);
            sa_buf reason = {0};
            cJSON *p = sa_payload();
            cJSON_AddBoolToObject(p, "stop_hook_active", stop_hook_active);
            int again = !x->st.ephemeral && !x->st.disable_tools &&
                        sa_hook(x, "Stop", NULL, p, NULL, &reason) && !stop_hook_active;
            if (x->st.ephemeral || x->st.disable_tools) cJSON_Delete(p);
            if (again) {
                stop_hook_active = 1;
                sa_store(x, sa_user_line(reason.n ? sa_str(&reason) : "Continue.", 1));
                sa_free(&reason);
                continue;
            }
            sa_free(&reason);
            break;
        }
        for (int i = 0; i < r.ncalls; i++) {
            if (interrupted) {
                sa_store(x, sa_tool_line(r.calls[i].id, r.calls[i].name, "skipped: interrupted", 1));
                continue;
            }
            sa_run_call(x, &r.calls[i], &interrupted);
        }
        sa_resp_free(&r);
        if (interrupted) {
            res.interrupted = 1;
            snprintf(res.subtype, sizeof res.subtype, "interrupted");
            break;
        }
    }
    backend_flush(&x->st);
    res.cost_usd = x->cost;
    res.context_tokens = x->ctx_tokens;
    res.context_window = x->window;
    if (meta) *meta = res;
    if (res.is_error && !reply) return NULL;
    return reply ? reply : strdup("");
}

static char *sa_ask(Backend *b, const char *user) { return sa_ask_ex(b, user, NULL); }

/* ---------- lifecycle ---------- */

static void sa_teardown(sa_agent *x) {
    sa_close_file(x);
    cJSON_Delete(x->msgs);
    x->msgs = cJSON_CreateArray();
    x->path[0] = '\0';
    x->id[0] = '\0';
    x->ctx_tokens = 0;
    x->cost = 0;
}

static void sa_curl_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static int sa_start(Backend *b, const char *resume) {
    sa_agent *x = b->ctx;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sa_curl_init);
    sa_teardown(x);
    x->started = 0;
    x->err[0] = '\0';
    cJSON_Delete(x->config);
    x->config = sa_config_load(x->err, sizeof x->err);
    if (!x->config) return 0;
    cJSON_Delete(x->hooks);
    x->hooks = (x->st.ephemeral || x->st.disable_tools) ? NULL : sa_hooks_load();
    sa_route_free(&x->route);
    if (!sa_route_resolve(x)) return 0;
    if (resume && *resume) {
        if (!sa_load(x, resume, x->st.fork_session)) return 0;
    } else {
        sa_new_id(x->id, sizeof x->id);
    }
    sa_session_start(x, resume && *resume ? "resume" : "startup");
    sa_models_refresh(x->config);
    backend_set(&x->st.resume, resume);
    x->started = 1;
    return 1;
}

static int sa_reset(Backend *b) {
    sa_agent *x = b->ctx;
    if (!x->started) return sa_start(b, NULL);
    sa_teardown(x);
    sa_new_id(x->id, sizeof x->id);
    backend_set(&x->st.resume, NULL);
    sa_session_start(x, "clear");
    return 1;
}

static void sa_set_event_cb(Backend *b, void (*cb)(void *ud, const backend_event *ev), void *ud) {
    sa_agent *x = b->ctx;
    x->st.on_event = cb;
    x->st.event_ud = ud;
}

static void sa_set_abort(Backend *b, int (*cb)(void)) { ((sa_agent *)b->ctx)->st.abort = cb; }

static int sa_set_effort(Backend *b, const char *effort) {
    backend_set(&((sa_agent *)b->ctx)->st.effort, effort);
    return 1;
}

static void sa_usage(Backend *b, long *tokens, long *window) {
    sa_agent *x = b->ctx;
    *tokens = x->ctx_tokens;
    *window = x->window;
}

static const char *sa_session_id(Backend *b) {
    sa_agent *x = b->ctx;
    return x->st.ephemeral || !x->id[0] ? NULL : x->id;
}

static const char *sa_model(Backend *b) {
    sa_agent *x = b->ctx;
    return x->route.full ? x->route.full : x->st.model;
}

static const char *sa_error(Backend *b) {
    sa_agent *x = b->ctx;
    return x->err[0] ? x->err : NULL;
}

static void sa_close(Backend *b) {
    sa_agent *x = b->ctx;
    sa_close_file(x);
    cJSON_Delete(x->msgs);
    cJSON_Delete(x->config);
    cJSON_Delete(x->hooks);
    sa_route_free(&x->route);
    sa_free(&x->hook_context);
    free(x->system);
    backend_state_free(&x->st);
    free(x);
    free(b);
}

Backend *core_agent_open(const backend_opts *o) {
    sa_agent *x = calloc(1, sizeof *x);
    Backend *b = calloc(1, sizeof *b);
    if (!x || !b) { free(x); free(b); return NULL; }
    backend_state_init(&x->st, o);
    x->msgs = cJSON_CreateArray();
    b->ctx = x;
    b->caps = BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT;
    b->ask = sa_ask;
    b->reset = sa_reset;
    b->close = sa_close;
    b->start = sa_start;
    b->ask_ex = sa_ask_ex;
    b->usage = sa_usage;
    b->set_model = backend_set_model_generic;
    b->set_effort = sa_set_effort;
    b->set_permission = backend_set_permission_none;
    b->set_event_cb = sa_set_event_cb;
    b->set_abort_check = sa_set_abort;
    b->session_id = sa_session_id;
    b->model = sa_model;
    b->effort = backend_stored_effort;
    b->auth_source = backend_none;
    b->last_error = sa_error;
    return b;
}

#endif /* CORE_AGENT_IMPLEMENTATION */
