/*
 * core.h — native agent loop: OpenAI-compatible chat completions over
 * libcurl, four tools (read, write, edit, bash), MCP tools from running
 * Streamable HTTP servers, JSONL sessions, compaction, context files, skills,
 * and hooks.
 *
 * The declarations need nothing else. backend.h instantiates the
 * implementation (CORE_AGENT_IMPLEMENTATION) after its adapter scaffolding.
 *
 * Config and state live under $SCRAP_CONFIG_DIR/agent, else
 * ~/.config/scrap/agent: providers.json, mcp.json, models/<provider>.json
 * (cached GET /models), sessions/<encoded cwd>/<id>.jsonl.
 */
#ifndef SCRAP_AGENT_H
#define SCRAP_AGENT_H

#include <stddef.h>

int  core_agent_config_dir(char *out, size_t size);
int  core_agent_session_dir(const char *cwd, char *out, size_t size);
long core_agent_models_stamp(void);
void core_agent_models(void (*fn)(void *ud, const char *id, const char *name, long context),
                        void *ud);
/* Blocking: the OpenRouter account's remaining credit in US dollars, from
 * GET /credits with the key providers.json names. Returns nonzero on success. */
int  core_agent_openrouter_balance(double *usd);

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
#include <stdatomic.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"
#define OPTCHAT_IMPLEMENTATION
#include "optchat.h"
#include "../spawnfd.h"

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
#define SA_ECHO_CAP        30000

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
    "    \"ollama\":     { \"base_url\": \"http://localhost:11434/v1\" },\n"
    "    \"mtplx\":      { \"base_url\": \"http://127.0.0.1:8000/v1\" }\n"
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

static const char SA_MEMORY_TOOLS[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"zoom\",\"description\":"
    "\"Open the line id+n of the view into the two lines of n/2 under it; n = 1 gives the message whole.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"First message of the line\"},"
    "\"n\":{\"type\":\"integer\",\"description\":\"Messages the line covers\"}},"
    "\"required\":[\"id\",\"n\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"date\",\"description\":"
    "\"The date and time of message id.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"Message id\"}},"
    "\"required\":[\"id\"]}}}"
    "]";

static const char SA_AGENT_TOOL[] =
    "{\"type\":\"function\",\"function\":{\"name\":\"agent\",\"description\":"
    "\"Start a subagent on a task in its own tab. The call returns at once; the subagent "
    "keeps running even if this turn ends or is interrupted. It does not report back by itself, "
    "so end the task by telling it to send its report with scrap send @<your session name>. The "
    "subagent starts from this view, including this turn so far, and has the same tools, so give "
    "it the task, not the background. Its steps stay out of the chat; only its report enters it. "
    "It works in this session's directory unless cwd names another, on this session's backend and "
    "model unless backend or model name others, at the backend's default effort unless effort "
    "names one; this session's effort is not passed on.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"What the subagent is to do, ending with how to report back (scrap send @<your session name>)\"},"
    "\"cwd\":{\"type\":\"string\",\"description\":\"Directory to work in: absolute, ~/..., or relative to this session's; defaults to this session's\"},"
    "\"backend\":{\"type\":\"string\",\"enum\":[\"claude\",\"codex\",\"grok\",\"core\"],\"description\":\"Backend to run on; defaults to this session's\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"Model to use; defaults to this session's on the same backend, else that backend's default\"},"
    "\"effort\":{\"type\":\"string\",\"description\":\"Reasoning effort such as low, medium, high or xhigh; defaults to the backend's default, not this session's\"}},"
    "\"required\":[\"task\"]}}}";

static const char SA_DELEGATE[] =
    "Give work that takes many tool calls, such as searching, reading several files, or running "
    "and fixing builds and tests, to a subagent with agent: its steps stay out of the chat and "
    "only its report enters the view. agent returns at once, and the subagent sends its report "
    "with scrap send only if its task says to, so always end the task with that instruction; "
    "then end the turn or go on with other work rather than wait for it. Do work of one or two "
    "tool calls yourself.\n\n";

static const char SA_SUBAGENT[] =
    "You are a subagent of " OC_AGENT ". The view is " OC_AGENT "'s chat; the first message after "
    "it is your task from " OC_AGENT ", and later ones come from the user. Do not stop to ask: do "
    "the task, then reply with a report for " OC_AGENT ": what you did, what you found and what is left. "
    "Your final reply goes nowhere by itself: as your last step, send the report with scrap "
    "send to the address your task gives. Your steps are not kept, so the report must hold everything that matters.\n\n";

static const char SA_DRIVE_TOOLS[] =
    "zoom and date are tools of the optchat MCP server.\n\n";

static const char SA_DRIVE_AGENT[] =
    "zoom, date and agent are tools of the optchat MCP server. Use its agent rather than a "
    "built-in subagent tool: only its subagent starts from the view.\n\n";

static const char SA_ROLE[] =
    "You are an expert coding assistant running inside scrap, a terminal coding harness. "
    "You help the user by reading files, running commands, editing code, and writing files.\n\n";

static const char SA_GUIDE[] =
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

int core_agent_openrouter_balance(double *usd) {
    char err[256];
    cJSON *config = sa_config_load(err, sizeof err);
    const cJSON *prov = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItem(config, "providers"),
                                                         "openrouter");
    const char *base = sa_jstr(prov, "base_url"), *key = NULL;
    int ok = 0;
    CURL *c = base && sa_provider_key(prov, &key) && key ? curl_easy_init() : NULL;
    if (c) {
        char url[1024], auth[1200];
        snprintf(url, sizeof url, "%s/credits", base);
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        struct curl_slist *h = curl_slist_append(NULL, auth);
        sa_buf body = {0};
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sa_collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        long code = 0;
        if (curl_easy_perform(c) == CURLE_OK &&
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code == 200) {
            cJSON *j = cJSON_Parse(sa_str(&body));
            cJSON *d = cJSON_GetObjectItem(j, "data");
            cJSON *total = cJSON_GetObjectItem(d, "total_credits");
            cJSON *used = cJSON_GetObjectItem(d, "total_usage");
            if (cJSON_IsNumber(total) && cJSON_IsNumber(used)) {
                *usd = total->valuedouble - used->valuedouble;
                ok = 1;
            }
            cJSON_Delete(j);
        }
        sa_free(&body);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
    }
    cJSON_Delete(config);
    return ok;
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
    struct sa_mcp *mcp;
    int nmcp;
    long mcp_seq;
    oc_mem *mem;
    oc_compactor *compactor;
    char compactor_backend[32], compactor_model[256], compactor_log[4200];
    _Atomic int halting;
    const _Atomic int *halt;
    long timeout;
    int sub;
    int (*agent_host)(void *ud, Backend *child, const char *task, const char *cwd,
                      const char *backend, const char *model, const char *effort, char **report);
    void *agent_ud;
    char drive[16];
    long view_seen;
    Backend *inner;
    int inner_used;
    char *relay;
    int lfd, nconn;
    _Atomic int relay_stop;
    pthread_t accept_thread;
    pthread_mutex_t conn_mu;
    pthread_cond_t conn_cond;
    char sock[104];
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

static int sa_aborted(sa_agent *x) { return (x->halt && *x->halt) || (x->st.abort && x->st.abort()); }

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
        agents_close_inherited();
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
    if (x->st.ephemeral || x->mem) return;
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

static char *sa_unboiler(const char *text) {
    static const char state[] = "(file state is current in your context \xe2\x80\x94 no need to Read it back)";
    sa_buf b = {0};
    for (const char *p = text; *p;) {
        size_t len = strcspn(p, "\n");
        int drop = !strncmp(p, "Shell cwd was reset to ", 23) ||
                   (len == 31 && !strncmp(p, "(Bash completed with no output)", 31));
        if (!drop) sa_put(&b, p, len + (p[len] == '\n'));
        p += len + (p[len] == '\n');
    }
    char *s = strdup(sa_str(&b));
    sa_free(&b);
    for (char *at; (at = strstr(s, state));) {
        char *from = at > s && at[-1] == ' ' ? at - 1 : at;
        memmove(from, at + sizeof state - 1, strlen(at + sizeof state - 1) + 1);
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
    s[n] = '\0';
    if (!n && *text) {
        free(s);
        s = strdup("(no output)");
    }
    return s;
}

static void sa_echo(sa_agent *x, const char *raw) {
    char *text = sa_unboiler(raw);
    size_t n = strlen(text), head = SA_ECHO_CAP / 2, tail = n - SA_ECHO_CAP / 2;
    if (n <= SA_ECHO_CAP) { oc_append(x->mem, "echo", text); free(text); return; }
    while (head && ((unsigned char)text[head] & 0xC0) == 0x80) head--;
    while (tail < n && ((unsigned char)text[tail] & 0xC0) == 0x80) tail++;
    sa_buf b = {0};
    sa_put(&b, text, head);
    sa_printf(&b, "\n[%zu bytes cut]\n", tail - head);
    sa_put(&b, text + tail, n - tail);
    oc_append(x->mem, "echo", sa_str(&b));
    sa_free(&b);
    free(text);
}

static void sa_tool_log(sa_buf *b, const char *name, const cJSON *in, const char *raw) {
    sa_printf(b, "%s", name ? name : "");
    const char *cmd = sa_jstr(in, "command"), *desc = sa_jstr(in, "description");
    if (name && !strcasecmp(name, "bash") && cmd) {
        sa_printf(b, " `%s`", cmd);
        if (desc && *desc && (!*cmd || strlen(cmd) > OC_NODE)) sa_printf(b, " (%s)", desc);
        return;
    }
    if (!cJSON_IsObject(in)) {
        sa_printf(b, " %s", raw ? raw : "{}");
        return;
    }
    const cJSON *f;
    int first = 1;
    cJSON_ArrayForEach(f, in) {
        sa_printf(b, "%s%s=", first ? " " : ", ", f->string ? f->string : "");
        first = 0;
        if (cJSON_IsString(f)) {
            sa_printf(b, "%s", f->valuestring);
        } else {
            char *v = cJSON_PrintUnformatted(f);
            sa_printf(b, "%s", v ? v : "");
            free(v);
        }
    }
}

static void sa_remember(sa_agent *x, const cJSON *line) {
    const cJSON *m = cJSON_GetObjectItem(line, "message"), *block;
    const char *role = sa_jstr(m, "role");
    if (!role) return;
    if (!strcmp(role, "tool")) { sa_echo(x, sa_jstr(m, "content") ? sa_jstr(m, "content") : ""); return; }
    if (strcmp(role, "assistant")) return;
    cJSON_ArrayForEach(block, cJSON_GetObjectItem(m, "content")) {
        const char *kind = sa_jstr(block, "type");
        if (kind && !strcmp(kind, "text")) {
            oc_append(x->mem, "talk", sa_jstr(block, "text") ? sa_jstr(block, "text") : "");
        } else if (kind && !strcmp(kind, "tool_use")) {
            const cJSON *input = cJSON_GetObjectItem(block, "input");
            char *in = cJSON_PrintUnformatted(input);
            sa_buf b = {0};
            sa_tool_log(&b, sa_jstr(block, "name"), input, in);
            oc_append(x->mem, "tool", sa_str(&b));
            sa_free(&b);
            free(in);
        }
    }
}

static void sa_store(sa_agent *x, cJSON *line) {
    sa_write_line(x, line);
    if (x->mem && !x->sub) sa_remember(x, line);
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
    if (x->drive[0]) {
        sa_printf(&b, "%s\n\n%s\n\n%s%s", OC_MASTER, OC_VIEW_DOC, x->sub ? SA_SUBAGENT : SA_DELEGATE,
                  x->sub ? SA_DRIVE_TOOLS : SA_DRIVE_AGENT);
    } else if (!x->st.disable_tools) {
        if (x->st.memory) sa_printf(&b, "%s\n\n%s\n\n%s", OC_MASTER, OC_VIEW_DOC, x->sub ? SA_SUBAGENT : SA_DELEGATE);
        else sa_puts(&b, SA_ROLE);
        sa_puts(&b, SA_GUIDE);
        sa_puts(&b, "\n\n");
        if (!x->st.memory) {
            char date[64];
            time_t now = time(NULL);
            strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
            sa_printf(&b, "Date: %s\n", date);
        }
        sa_printf(&b, "Working directory: %s", x->st.cwd ? x->st.cwd : ".");
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

/* ---------- mcp ---------- */

#define SA_MCP_VERSION "2025-06-18"

typedef struct sa_mcp { char *name, *url, *session, *version; cJSON *headers, *tools; } sa_mcp;

typedef struct { sa_buf body; char session[256], type[128]; } sa_mcp_resp;

static void sa_mcp_hdr_value(const char *p, size_t n, char *out, size_t size) {
    while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
    while (n && (p[n - 1] == '\r' || p[n - 1] == '\n' || p[n - 1] == ' ')) n--;
    snprintf(out, size, "%.*s", (int)n, p);
}

static size_t sa_mcp_on_header(char *p, size_t sz, size_t nm, void *ud) {
    sa_mcp_resp *r = ud;
    size_t n = sz * nm;
    if (n > 15 && !strncasecmp(p, "Mcp-Session-Id:", 15))
        sa_mcp_hdr_value(p + 15, n - 15, r->session, sizeof r->session);
    else if (n > 13 && !strncasecmp(p, "Content-Type:", 13))
        sa_mcp_hdr_value(p + 13, n - 13, r->type, sizeof r->type);
    return n;
}

/* Finds the JSON-RPC message with the given id in an SSE body. */
static cJSON *sa_mcp_sse_find(const char *body, long id) {
    sa_buf data = {0};
    cJSON *found = NULL;
    const char *p = body;
    while (*p && !found) {
        const char *eol = strchr(p, '\n');
        if (!eol) break;
        size_t n = (size_t)(eol - p);
        if (n && p[n - 1] == '\r') n--;
        if (!n) {
            cJSON *j = data.n ? cJSON_Parse(sa_str(&data)) : NULL;
            const cJSON *jid = cJSON_GetObjectItem(j, "id");
            if (cJSON_IsNumber(jid) && (long)jid->valuedouble == id) found = j;
            else cJSON_Delete(j);
            sa_clear(&data);
        } else if (n >= 5 && !strncmp(p, "data:", 5)) {
            const char *v = p + 5;
            if (*v == ' ') v++;
            if (data.n) sa_put(&data, "\n", 1);
            sa_put(&data, v, (size_t)(p + n - v));
        }
        p = eol + 1;
    }
    sa_free(&data);
    return found;
}

/* POSTs one JSON-RPC message. id 0 is a notification. Returns the HTTP status, 0 on failure. */
static long sa_mcp_post(sa_agent *x, sa_mcp *s, cJSON *msg, long id, cJSON **reply, char *err,
                        size_t errsize, int *interrupted) {
    *reply = NULL;
    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errsize, "curl init failed"); return 0; }
    struct curl_slist *h = NULL;
    char buf[2048];
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: application/json, text/event-stream");
    if (s->session) {
        snprintf(buf, sizeof buf, "Mcp-Session-Id: %s", s->session);
        h = curl_slist_append(h, buf);
    }
    if (s->version) {
        snprintf(buf, sizeof buf, "MCP-Protocol-Version: %s", s->version);
        h = curl_slist_append(h, buf);
    }
    const cJSON *kv;
    cJSON_ArrayForEach(kv, s->headers) {
        if (!cJSON_IsString(kv)) continue;
        snprintf(buf, sizeof buf, "%s: %s", kv->string, kv->valuestring);
        h = curl_slist_append(h, buf);
    }
    char *body = cJSON_PrintUnformatted(msg);
    sa_mcp_resp r = {0};
    curl_easy_setopt(c, CURLOPT_URL, s->url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sa_collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, sa_mcp_on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLM *m = curl_multi_init();
    curl_multi_add_handle(m, c);
    int running = 1, sse = 0;
    CURLcode result = CURLE_OK;
    while (running) {
        curl_multi_perform(m, &running);
        if (!sse) sse = strstr(r.type, "text/event-stream") != NULL;
        if (sse && id && (*reply = sa_mcp_sse_find(sa_str(&r.body), id))) break;
        if (!running) break;
        if (sa_aborted(x)) { *interrupted = 1; break; }
        curl_multi_poll(m, NULL, 0, 20, NULL);
    }
    CURLMsg *cm;
    int left;
    while ((cm = curl_multi_info_read(m, &left)))
        if (cm->msg == CURLMSG_DONE) result = cm->data.result;
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (*interrupted) {
        code = 0;
    } else if (!*reply && result != CURLE_OK) {
        snprintf(err, errsize, "%s", curl_easy_strerror(result));
        code = 0;
    } else if (code >= 200 && code < 300 && id && !*reply) {
        *reply = sse ? sa_mcp_sse_find(sa_str(&r.body), id) : cJSON_Parse(sa_str(&r.body));
        if (!*reply) snprintf(err, errsize, "no response to request %ld", id);
    } else if (code >= 300) {
        snprintf(err, errsize, "HTTP %ld: %.200s", code, sa_str(&r.body));
    }
    if (r.session[0] && code) {
        free(s->session);
        s->session = strdup(r.session);
    }
    curl_multi_remove_handle(m, c);
    curl_multi_cleanup(m);
    curl_easy_cleanup(c);
    curl_slist_free_all(h);
    free(body);
    sa_free(&r.body);
    return code;
}

static cJSON *sa_mcp_request(sa_agent *x, sa_mcp *s, const char *method, cJSON *params, char *err,
                             size_t errsize, int *interrupted);

static int sa_mcp_init(sa_agent *x, sa_mcp *s, char *err, size_t errsize, int *interrupted) {
    free(s->session); s->session = NULL;
    free(s->version); s->version = NULL;
    cJSON *params = cJSON_CreateObject(), *info = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", SA_MCP_VERSION);
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON_AddStringToObject(info, "name", "scrap");
    cJSON_AddStringToObject(info, "version", "1");
    cJSON_AddItemToObject(params, "clientInfo", info);
    cJSON *res = sa_mcp_request(x, s, "initialize", params, err, errsize, interrupted);
    if (!res) return 0;
    const char *v = sa_jstr(res, "protocolVersion");
    s->version = strdup(v ? v : SA_MCP_VERSION);
    cJSON_Delete(res);
    cJSON *note = cJSON_CreateObject(), *reply;
    cJSON_AddStringToObject(note, "jsonrpc", "2.0");
    cJSON_AddStringToObject(note, "method", "notifications/initialized");
    char ignored[64];
    sa_mcp_post(x, s, note, 0, &reply, ignored, sizeof ignored, interrupted);
    cJSON_Delete(note);
    return !*interrupted;
}

/* Sends a request and returns its result. Takes ownership of params. */
static cJSON *sa_mcp_request(sa_agent *x, sa_mcp *s, const char *method, cJSON *params, char *err,
                             size_t errsize, int *interrupted) {
    cJSON *msg = cJSON_CreateObject(), *reply = NULL, *result = NULL;
    long id = ++x->mcp_seq;
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", (double)id);
    cJSON_AddStringToObject(msg, "method", method);
    cJSON_AddItemToObject(msg, "params", params ? params : cJSON_CreateObject());
    long code = sa_mcp_post(x, s, msg, id, &reply, err, errsize, interrupted);
    if (code == 404 && s->session && strcmp(method, "initialize") &&
        sa_mcp_init(x, s, err, errsize, interrupted)) {
        cJSON_ReplaceItemInObject(msg, "id", cJSON_CreateNumber((double)(id = ++x->mcp_seq)));
        code = sa_mcp_post(x, s, msg, id, &reply, err, errsize, interrupted);
    }
    cJSON_Delete(msg);
    if (reply) {
        const cJSON *e = cJSON_GetObjectItem(reply, "error");
        if (e) snprintf(err, errsize, "%s", sa_jstr(e, "message") ? sa_jstr(e, "message") : "error");
        else result = cJSON_DetachItemFromObject(reply, "result");
        if (!e && !result) snprintf(err, errsize, "response has no result");
    }
    cJSON_Delete(reply);
    return result;
}

static void sa_mcp_free(sa_agent *x) {
    for (int i = 0; i < x->nmcp; i++) {
        sa_mcp *s = &x->mcp[i];
        free(s->name); free(s->url); free(s->session); free(s->version);
        cJSON_Delete(s->headers);
        cJSON_Delete(s->tools);
    }
    free(x->mcp);
    x->mcp = NULL;
    x->nmcp = 0;
}

static void sa_mcp_connect(sa_agent *x, sa_mcp *s) {
    char err[512] = "", msg[800];
    int interrupted = 0;
    s->tools = cJSON_CreateArray();
    if (!sa_mcp_init(x, s, err, sizeof err, &interrupted)) {
        snprintf(msg, sizeof msg, "mcp server %s: %s", s->name, err);
        sa_warn(x, msg);
        return;
    }
    char *cursor = NULL;
    do {
        cJSON *params = cJSON_CreateObject();
        if (cursor) cJSON_AddStringToObject(params, "cursor", cursor);
        free(cursor);
        cursor = NULL;
        cJSON *res = sa_mcp_request(x, s, "tools/list", params, err, sizeof err, &interrupted);
        if (!res) {
            snprintf(msg, sizeof msg, "mcp server %s: tools/list: %s", s->name, err);
            sa_warn(x, msg);
            break;
        }
        cJSON *tools = cJSON_GetObjectItem(res, "tools"), *t;
        while (cJSON_GetArraySize(tools) && (t = cJSON_DetachItemFromArray(tools, 0)))
            cJSON_AddItemToArray(s->tools, t);
        const char *next = sa_jstr(res, "nextCursor");
        if (next && *next) cursor = strdup(next);
        cJSON_Delete(res);
    } while (cursor);
}

static void sa_mcp_load(sa_agent *x) {
    char dir[2048], path[2200];
    sa_mcp_free(x);
    if (x->st.ephemeral || x->st.disable_tools || !core_agent_config_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/mcp.json", dir);
    char *text = sa_slurp(path, NULL);
    if (!text) return;
    cJSON *c = cJSON_Parse(text), *srv;
    free(text);
    if (!c) {
        char msg[2400];
        snprintf(msg, sizeof msg, "%s is not valid JSON", path);
        sa_warn(x, msg);
        return;
    }
    cJSON *servers = cJSON_GetObjectItem(c, "servers");
    x->mcp = calloc((size_t)cJSON_GetArraySize(servers) + 1, sizeof *x->mcp);
    cJSON_ArrayForEach(srv, servers) {
        const char *url = sa_jstr(srv, "url");
        if (!x->mcp || !url) continue;
        sa_mcp *s = &x->mcp[x->nmcp++];
        s->name = strdup(srv->string);
        s->url = strdup(url);
        s->headers = cJSON_Duplicate(cJSON_GetObjectItem(srv, "headers"), 1);
        sa_mcp_connect(x, s);
    }
    cJSON_Delete(c);
}

static void sa_mcp_tool_name(const sa_mcp *s, const char *tool, char *out, size_t size) {
    snprintf(out, size, "mcp__%s__%s", s->name, tool ? tool : "");
    for (char *p = out; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') *p = '_';
    if (strlen(out) > 64) out[64] = '\0';
}

static const cJSON *sa_mcp_find(sa_agent *x, const char *name, sa_mcp **server) {
    char full[512];
    if (strncmp(name, "mcp__", 5)) return NULL;
    for (int i = 0; i < x->nmcp; i++) {
        const cJSON *t;
        cJSON_ArrayForEach(t, x->mcp[i].tools) {
            sa_mcp_tool_name(&x->mcp[i], sa_jstr(t, "name"), full, sizeof full);
            if (!strcmp(full, name)) { *server = &x->mcp[i]; return t; }
        }
    }
    return NULL;
}

static void sa_mcp_tools(sa_agent *x, cJSON *tools) {
    char full[512];
    for (int i = 0; i < x->nmcp; i++) {
        const cJSON *t;
        cJSON_ArrayForEach(t, x->mcp[i].tools) {
            const cJSON *schema = cJSON_GetObjectItem(t, "inputSchema");
            cJSON *w = cJSON_CreateObject(), *fn = cJSON_CreateObject();
            sa_mcp_tool_name(&x->mcp[i], sa_jstr(t, "name"), full, sizeof full);
            cJSON_AddStringToObject(w, "type", "function");
            cJSON_AddStringToObject(fn, "name", full);
            cJSON_AddStringToObject(fn, "description", sa_jstr(t, "description") ? sa_jstr(t, "description") : "");
            cJSON_AddItemToObject(fn, "parameters", cJSON_IsObject(schema) ? cJSON_Duplicate(schema, 1)
                                                                            : cJSON_Parse("{\"type\":\"object\"}"));
            cJSON_AddItemToObject(w, "function", fn);
            cJSON_AddItemToArray(tools, w);
        }
    }
}

static int sa_mcp_call(sa_agent *x, sa_mcp *s, const cJSON *tool, const cJSON *input, sa_buf *out,
                       int *interrupted) {
    char err[512] = "";
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", sa_jstr(tool, "name"));
    cJSON_AddItemToObject(params, "arguments", cJSON_Duplicate(input, 1));
    cJSON *res = sa_mcp_request(x, s, "tools/call", params, err, sizeof err, interrupted);
    if (!res) {
        if (!*interrupted) sa_printf(out, "mcp server %s: %s", s->name, err);
        return 1;
    }
    const cJSON *block;
    cJSON_ArrayForEach(block, cJSON_GetObjectItem(res, "content")) {
        const char *kind = sa_jstr(block, "type");
        if (out->n) sa_puts(out, "\n");
        if (kind && !strcmp(kind, "text")) sa_puts(out, sa_jstr(block, "text"));
        else sa_printf(out, "[%s omitted]", kind ? kind : "block");
    }
    if (out->n > SA_OUT_BYTES) {
        out->n = SA_OUT_BYTES;
        out->p[out->n] = '\0';
        sa_puts(out, "\n[output truncated]");
    }
    int failed = cJSON_IsTrue(cJSON_GetObjectItem(res, "isError"));
    cJSON_Delete(res);
    return failed;
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

static int sa_cache_control(const sa_agent *x) {
    const char *m = x->route.model;
    return m && (!strncmp(m, "anthropic/", 10) || !strncmp(m, "google/gemini", 13) || strstr(m, "claude"));
}

static cJSON *sa_text_part(const char *s, size_t n, int cache) {
    cJSON *p = cJSON_CreateObject();
    char *t = strndup(s, n);
    cJSON_AddStringToObject(p, "type", "text");
    cJSON_AddStringToObject(p, "text", t);
    free(t);
    if (cache) cJSON_AddItemToObject(p, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
    return p;
}

static void sa_cache_marks(cJSON *msgs, int cache) {
    int budget = 3;
    cJSON *msg;
    cJSON_ArrayForEach(msg, msgs) {
        const char *c = sa_jstr(msg, "content");
        if (!c || !strpbrk(c, BACKEND_CACHE_MARK BACKEND_BLOCK_MARK)) continue;
        cJSON *parts = cJSON_CreateArray();
        sa_buf flat = {0};
        for (const char *s = c, *e;; s = e + 1) {
            e = strpbrk(s, BACKEND_CACHE_MARK BACKEND_BLOCK_MARK);
            size_t n = e ? (size_t)(e - s) : strlen(s);
            int mark = e && *e == *BACKEND_CACHE_MARK;
            sa_put(&flat, s, n);
            if (n) cJSON_AddItemToArray(parts, sa_text_part(s, n, mark && budget > 0));
            if (mark && n && budget > 0) budget--;
            if (!e) break;
        }
        if (cache) {
            cJSON_ReplaceItemInObject(msg, "content", parts);
        } else {
            cJSON_Delete(parts);
            cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(sa_str(&flat)));
        }
        sa_free(&flat);
    }
    if (!cache) return;
    cJSON *last = cJSON_GetArrayItem(msgs, cJSON_GetArraySize(msgs) - 1);
    cJSON *content = cJSON_GetObjectItem(last, "content");
    if (cJSON_IsString(content) && *content->valuestring) {
        cJSON *parts = cJSON_CreateArray();
        cJSON_AddItemToArray(parts, sa_text_part(content->valuestring, strlen(content->valuestring), 1));
        cJSON_ReplaceItemInObject(last, "content", parts);
    } else if (cJSON_IsArray(content)) {
        cJSON *tail = cJSON_GetArrayItem(content, cJSON_GetArraySize(content) - 1);
        if (tail && !cJSON_GetObjectItem(tail, "cache_control"))
            cJSON_AddItemToObject(tail, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
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
    sa_cache_marks(msgs, sa_cache_control(x));
    cJSON_AddItemToObject(root, "messages", msgs);
    if (!x->st.disable_tools) {
        cJSON *defs = cJSON_Parse(SA_TOOLS);
        if (x->mem) {
            cJSON *more = cJSON_Parse(SA_MEMORY_TOOLS), *t;
            while ((t = cJSON_DetachItemFromArray(more, 0))) cJSON_AddItemToArray(defs, t);
            cJSON_Delete(more);
            if (!x->sub) cJSON_AddItemToArray(defs, cJSON_Parse(SA_AGENT_TOOL));
        }
        sa_mcp_tools(x, defs);
        cJSON_AddItemToObject(root, "tools", defs);
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
    long in, out, cached, written;
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
        r->written = (long)sa_jnum(cJSON_GetObjectItem(usage, "prompt_tokens_details"), "cache_write_tokens");
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
    if (x->timeout) curl_easy_setopt(c, CURLOPT_TIMEOUT, x->timeout);
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

static char *sa_ask_ex(Backend *b, const char *user, backend_result *meta);

/* The agent tool's cwd as an absolute directory in out: ~ is $HOME, a
 * relative path is under the parent's cwd. 0 with why in err otherwise. */
static int sa_agent_cwd(sa_agent *x, const char *want, char *out, size_t size, sa_buf *err) {
    char path[PATH_MAX];
    const char *home = getenv("HOME");
    if (want[0] == '~' && (want[1] == '/' || !want[1]) && home)
        snprintf(path, sizeof path, "%s%s", home, want + 1);
    else if (want[0] == '/')
        snprintf(path, sizeof path, "%s", want);
    else {
        char here[PATH_MAX];
        const char *base = x->st.cwd ? x->st.cwd : getcwd(here, sizeof here);
        snprintf(path, sizeof path, "%s/%s", base ? base : ".", want);
    }
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(path, real) || stat(real, &st) || !S_ISDIR(st.st_mode)) {
        sa_printf(err, "cwd %s is not a directory", path);
        return 0;
    }
    snprintf(out, size, "%s", real);
    return 1;
}

static int sa_tool_agent(sa_agent *x, const cJSON *input, sa_buf *out, int *interrupted) {
    const char *task = sa_jstr(input, "task");
    if (!task || !*task) {
        sa_puts(out, "task is required");
        return 1;
    }
    const char *want = sa_jstr(input, "cwd");
    char dir[PATH_MAX];
    const char *cwd = x->st.cwd;
    if (want && *want) {
        if (!sa_agent_cwd(x, want, dir, sizeof dir, out)) return 1;
        cwd = dir;
    }
    const char *mine = x->drive[0] ? x->drive : "core";
    const char *backend = sa_jstr(input, "backend");
    if (!backend || !*backend) backend = mine;
    else if (strcmp(backend, "claude") && strcmp(backend, "codex") && strcmp(backend, "grok") &&
             strcmp(backend, "core")) {
        sa_printf(out, "backend %s cannot run a subagent: pick claude, codex, grok or core", backend);
        return 1;
    }
    const char *model = sa_jstr(input, "model");
    if (!model || !*model || !strcmp(model, "default"))
        model = !strcmp(backend, mine) && !(model && *model) ? x->st.model : NULL;
    const char *effort = sa_jstr(input, "effort");
    if (effort && (!*effort || !strcmp(effort, "default"))) effort = NULL;
    backend_opts o = { .name = backend, .model = model, .effort = effort,
                       .cwd = cwd, .system = x->st.system, .memory = 1, .memory_relay = x->relay,
                       .permission_mode = x->st.permission, .env = (const char *const *)x->st.env,
                       /* the parent's settings files, so its permission rules too */
                       .allow_customizations = x->st.allow_customizations,
                       .no_browser_login = x->st.no_browser_login };
    Backend *b = core_agent_open(&o);
    if (!b) {
        sa_puts(out, "could not open a subagent");
        return 1;
    }
    sa_agent *c = b->ctx;
    c->sub = 1;
    c->mem = x->mem;
    oc_retain(x->mem);
    c->st.abort = x->st.abort;
    char *report = NULL;
    int status = x->agent_host ? x->agent_host(x->agent_ud, b, task, cwd, backend, model, effort, &report) : BACKEND_AGENT_DECLINED;
    if (status == BACKEND_AGENT_STARTED) {
        sa_puts(out, report ? report : "subagent started in its own tab");
        free(report);
        return 0;
    }
    if (status != BACKEND_AGENT_DECLINED) {
        if (status == BACKEND_AGENT_INTERRUPTED) *interrupted = 1;
        if (report && *report) sa_puts(out, report);
        else sa_puts(out, status == BACKEND_AGENT_INTERRUPTED ? "subagent interrupted" : "subagent failed: no report");
        free(report);
        return status != BACKEND_AGENT_DONE;
    }
    backend_result res;
    char *reply = sa_ask_ex(b, task, &res);
    int failed = !reply || res.is_error || res.interrupted;
    if (res.interrupted) *interrupted = 1;
    if (reply && *reply) sa_puts(out, reply);
    else sa_printf(out, "subagent failed: %s", c->err[0] ? c->err : "no report");
    free(reply);
    b->close(b);
    return failed;
}

/* zoom, date and agent; -1 when name is none of them. */
static int sa_memory_tool(sa_agent *x, const char *name, const cJSON *input, sa_buf *out, int *interrupted) {
    if (!x->mem) return -1;
    if (!strcmp(name, "zoom")) {
        char *z = oc_zoom(x->mem, (long)sa_jnum(input, "id"), (long)sa_jnum(input, "n"));
        sa_puts(out, z);
        int failed = !strncmp(z, "No line", 7);
        free(z);
        return failed;
    }
    if (!strcmp(name, "date")) {
        const char *d = oc_date(x->mem, (long)sa_jnum(input, "id"));
        if (d) sa_puts(out, d);
        else sa_printf(out, "No message %ld.", (long)sa_jnum(input, "id"));
        return !d;
    }
    if (!x->sub && !strcmp(name, "agent")) return sa_tool_agent(x, input, out, interrupted);
    return -1;
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
    int failed = 1, mem_failed;
    sa_mcp *server = NULL;
    const cJSON *tool;
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
        } else if ((mem_failed = sa_memory_tool(x, name, input, &out, interrupted)) >= 0) {
            failed = mem_failed;
        } else if ((tool = sa_mcp_find(x, name, &server))) {
            failed = sa_mcp_call(x, server, tool, input, &out, interrupted);
        } else {
            sa_printf(&out, "unknown tool '%s'; the tools are read, write, edit, bash%s", name,
                      !x->mem ? "" : x->sub ? ", zoom, date" : ", zoom, date, agent");
            for (int i = 0; i < x->nmcp; i++) {
                char full[512];
                const cJSON *t;
                cJSON_ArrayForEach(t, x->mcp[i].tools) {
                    sa_mcp_tool_name(&x->mcp[i], sa_jstr(t, "name"), full, sizeof full);
                    sa_printf(&out, ", %s", full);
                }
            }
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
    cJSON_AddNumberToObject(u, "cache_creation_input_tokens", (double)r->written);
    cJSON_AddNumberToObject(u, "totalTokens", (double)(r->in + r->out));
    if (r->cost) cJSON_AddNumberToObject(u, "cost", r->cost);
    cJSON_AddItemToObject(m, "usage", u);
    return sa_line("assistant", 0, m);
}

static int sa_start(Backend *b, const char *resume);

static int sa_settle_abort(void *ud) { return sa_aborted(ud); }

static const char *sa_user_kind(const char *user) {
    return strncmp(user, "[from ", 6) ? "user" : "work";
}

static int sa_memory_turn(sa_agent *x, const char *user, const sa_buf *reminder, sa_buf *out) {
    /* A subagent starts mid-turn, while the parent's latest lines (at least
       its own agent call) are still being summarized; its task carries that
       context and it can zoom, so it renders the view as it is. */
    if (!x->sub && !oc_settled(x->mem)) sa_warn(x, "waiting for memory summaries");
    if (!x->sub && !oc_settle(x->mem, sa_settle_abort, x)) {
        char why[512];
        if (x->compactor && oc_compactor_error(x->compactor, why, sizeof why)) sa_warn(x, why);
        oc_append(x->mem, sa_user_kind(user), user);
        return 0;
    }
    if (!x->sub && !oc_settled(x->mem)) {
        char why[512];
        if (x->compactor && oc_compactor_error(x->compactor, why, sizeof why)) sa_warn(x, why);
    }
    /* Claude turns each start after /clear, so the view goes as one user
       message; claude.h turns its marks into blocks with one breakpoint on
       the last marked piece. That mark sits before </chat> so the next
       turn's view (which only adds lines) starts with this turn's cached
       prefix, and a mark where the last view ended keeps that prefix within
       the API's lookback when many lines came since. Other drives would see
       the marks as raw text. */
    int marks = !out || !strcmp(x->drive, "claude");
    char *view = out && marks && !x->sub ? oc_render_turn(x->mem)
               : oc_render_from(x->mem, marks, out && marks ? &x->view_seen : NULL);
    if (!x->sub) oc_append(x->mem, sa_user_kind(user), user);
    if (out) {
        sa_printf(out, "%s\n%s", view, user);
        free(view);
        return 1;
    }
    sa_buf b = {0};
    sa_printf(&b, "%s\n" BACKEND_CACHE_MARK "%s", view, user);
    if (reminder->n) sa_printf(&b, "\n\n%s", sa_str(reminder));
    cJSON_AddItemToArray(x->msgs, sa_user_line(sa_str(&b), 0));
    sa_free(&b);
    free(view);
    return 1;
}

static char *sa_drive_ask(sa_agent *x, const char *user, backend_result *meta);

static char *sa_ask_ex(Backend *b, const char *user, backend_result *meta) {
    sa_agent *x = b->ctx;
    if (x->drive[0]) return sa_drive_ask(x, user, meta);
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

    sa_buf reminder = {0};
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
        if (ctx.n) sa_printf(&reminder, "<system-reminder>\n%s</system-reminder>", sa_str(&ctx));
        sa_free(&ctx);
        sa_free(&reason);
    }
    if (x->mem) {
        if (!sa_memory_turn(x, user ? user : "", &reminder, NULL)) {
            res.interrupted = 1;
            snprintf(res.subtype, sizeof res.subtype, "interrupted");
        }
    } else {
        if (reminder.n) sa_store(x, sa_user_line(sa_str(&reminder), 1));
        sa_store(x, sa_user_line(user ? user : "", 0));
    }
    sa_free(&reminder);

    long max_steps = (long)sa_jnum(x->config, "max_steps");
    char *reply = NULL;
    int stop_hook_active = 0;
    for (long step = 1; !res.interrupted; step++) {
        if (max_steps > 0 && step > max_steps) {
            snprintf(res.subtype, sizeof res.subtype, "error_max_turns");
            res.is_error = 1;
            snprintf(x->err, sizeof x->err, "stopped after %ld model requests (max_steps)", max_steps);
            break;
        }
        if (!x->mem) sa_maybe_compact(x);
        char *body = sa_body(x, cJSON_GetArraySize(x->msgs), NULL, 1);
        sa_resp r = { .x = x, .emit = 1 };
        int interrupted = 0;
        int ok = body && sa_request(x, body, &r, &interrupted);
        free(body);
        backend_flush(&x->st);
        /* prompt_tokens counts the cached reads and writes too */
        long fresh = r.in - r.cached - r.written;
        res.input_tokens += fresh > 0 ? fresh : 0;
        res.output_tokens += r.out;
        res.cache_read_tokens += r.cached;
        res.cache_creation_tokens += r.written;
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
    if (x->mem) {
        cJSON_Delete(x->msgs);
        x->msgs = cJSON_CreateArray();
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

static _Thread_local _Atomic int *sa_compactor_halt;
static _Thread_local long sa_compactor_deadline;
static _Thread_local char *(*sa_compactor_ask)(Backend *b, const char *user, backend_result *meta);
static _Thread_local const char *sa_compactor_log, *sa_compactor_model;
static _Thread_local const char *(*sa_compactor_last_error)(Backend *b);
static _Thread_local char sa_compactor_why[300];

static int sa_compactor_aborted(void) {
    return (sa_compactor_halt && *sa_compactor_halt) || (sa_compactor_deadline && sa_now_ms() > sa_compactor_deadline);
}

static char *sa_compactor_timed_ask(Backend *b, const char *user) {
    sa_compactor_deadline = sa_now_ms() + OC_TIMEOUT_S * 1000;
    backend_result u = {0};
    long started = sa_now_ms();
    char *r = sa_compactor_ask(b, user, &u);
    sa_compactor_deadline = 0;
    FILE *f = fopen(sa_compactor_log, "a");
    if (f) {
        fprintf(f, "{\"t\":%ld,\"model\":\"%s\",\"ms\":%ld,\"input\":%ld,\"write\":%ld,\"read\":%ld,\"output\":%ld,\"ok\":%d}\n",
                (long)time(NULL), sa_compactor_model, sa_now_ms() - started, u.input_tokens, u.cache_creation_tokens,
                u.cache_read_tokens, u.output_tokens, r && !u.is_error);
        fclose(f);
    }
    /* An error's text is no summary; returning none makes it a failed try,
       and the text is kept to say why. */
    snprintf(sa_compactor_why, sizeof sa_compactor_why, "%s", u.is_error && r ? r : "");
    if (u.is_error) { free(r); return NULL; }
    return r;
}

static const char *sa_compactor_error(Backend *b) {
    if (sa_compactor_why[0]) return sa_compactor_why;
    return sa_compactor_last_error ? sa_compactor_last_error(b) : NULL;
}

static char *sa_compactor_core_ask(Backend *b, const char *user) {
    backend_result u = {0};
    char *r = sa_ask_ex(b, user, &u);
    if (u.is_error) { free(r); return NULL; }
    return r;
}

static Backend *sa_compactor_open(void *ud, const char *system) {
    sa_agent *x = ud;
    backend_opts o = { .name = x->compactor_backend, .model = x->compactor_model, .effort = OC_COMPACT_EFFORT,
                       .system = system, .ephemeral = 1, .disable_tools = 1 };
    if (strcmp(o.name, "core")) {
        Backend *b = backend_open_ex(&o);
        sa_compactor_halt = &x->halting;
        sa_compactor_log = x->compactor_log;
        sa_compactor_model = x->compactor_model;
        if (b) {
            b->set_abort_check(b, sa_compactor_aborted);
            sa_compactor_ask = b->ask_ex;
            b->ask = sa_compactor_timed_ask;
            sa_compactor_last_error = b->last_error;
            b->last_error = sa_compactor_error;
        }
        return b;
    }
    Backend *b = core_agent_open(&o);
    if (b) {
        sa_agent *h = b->ctx;
        h->halt = &x->halting;
        h->timeout = OC_TIMEOUT_S;
        b->ask = sa_compactor_core_ask;
    }
    return b;
}

static int sa_memory_open(sa_agent *x);

/* ---------- memory mode on a CLI ----------
 *
 * The CLI runs each turn in a fresh context holding the view and the user's
 * message. Its events are written to memory, and its zoom, date and agent
 * tools come from an MCP server on a unix socket, reached through
 * "<relay> mcp-memory <socket>". */

static cJSON *sa_relay_tools(sa_agent *x) {
    cJSON *defs = cJSON_Parse(SA_MEMORY_TOOLS), *out = cJSON_CreateArray(), *t;
    if (!x->sub) cJSON_AddItemToArray(defs, cJSON_Parse(SA_AGENT_TOOL));
    cJSON_ArrayForEach(t, defs) {
        cJSON *f = cJSON_GetObjectItem(t, "function"), *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", sa_jstr(f, "name"));
        cJSON_AddStringToObject(tool, "description", sa_jstr(f, "description"));
        cJSON_AddItemToObject(tool, "inputSchema", cJSON_Duplicate(cJSON_GetObjectItem(f, "parameters"), 1));
        if (strcmp(sa_jstr(f, "name"), "agent"))
            cJSON_AddBoolToObject(cJSON_AddObjectToObject(tool, "annotations"), "readOnlyHint", 1);
        cJSON_AddItemToArray(out, tool);
    }
    cJSON_Delete(defs);
    return out;
}

typedef struct {
    sa_agent *x;
    int fd, inflight;
    pthread_mutex_t mu;
    pthread_cond_t cond;
} sa_relay_conn;

static int sa_relay_send(sa_relay_conn *c, cJSON *msg) {
    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!s) return 0;
    size_t n = strlen(s), off = 0;
    s[n++] = '\n';
    pthread_mutex_lock(&c->mu);
    while (off < n) {
        ssize_t w = write(c->fd, s + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    pthread_mutex_unlock(&c->mu);
    free(s);
    return off == n;
}

static int sa_relay_handle(sa_relay_conn *c, const cJSON *m) {
    sa_agent *x = c->x;
    const cJSON *id = cJSON_GetObjectItem(m, "id"), *params = cJSON_GetObjectItem(m, "params");
    const char *method = sa_jstr(m, "method");
    if (!id || !method) return 1;
    cJSON *reply = cJSON_CreateObject(), *r = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, 1));
    if (!strcmp(method, "initialize")) {
        const char *v = sa_jstr(params, "protocolVersion");
        cJSON_AddStringToObject(r, "protocolVersion", v ? v : "2025-06-18");
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(r, "capabilities"), "tools");
        cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
        cJSON_AddStringToObject(info, "name", "optchat");
        cJSON_AddStringToObject(info, "version", "1");
    } else if (!strcmp(method, "tools/list")) {
        cJSON_AddItemToObject(r, "tools", sa_relay_tools(x));
    } else if (!strcmp(method, "tools/call")) {
        const char *name = sa_jstr(params, "name");
        const cJSON *args = cJSON_GetObjectItem(params, "arguments");
        cJSON *empty = cJSON_CreateObject();
        sa_buf out = {0};
        int interrupted = 0;
        int failed = sa_memory_tool(x, name ? name : "", cJSON_IsObject(args) ? args : empty, &out, &interrupted);
        if (failed < 0) sa_printf(&out, "unknown tool '%s'", name ? name : "");
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "type", "text");
        cJSON_AddStringToObject(block, "text", sa_str(&out));
        cJSON_AddItemToArray(cJSON_AddArrayToObject(r, "content"), block);
        cJSON_AddBoolToObject(r, "isError", failed != 0);
        sa_free(&out);
        cJSON_Delete(empty);
    } else if (strcmp(method, "ping")) {
        cJSON_Delete(r);
        r = NULL;
        cJSON *e = cJSON_AddObjectToObject(reply, "error");
        cJSON_AddNumberToObject(e, "code", -32601);
        cJSON_AddStringToObject(e, "message", "method not found");
    }
    if (r) cJSON_AddItemToObject(reply, "result", r);
    return sa_relay_send(c, reply);
}

typedef struct { sa_relay_conn *c; cJSON *m; } sa_relay_job;

static void *sa_relay_call(void *arg) {
    sa_relay_job *j = arg;
    sa_relay_handle(j->c, j->m);
    cJSON_Delete(j->m);
    pthread_mutex_lock(&j->c->mu);
    j->c->inflight--;
    pthread_cond_broadcast(&j->c->cond);
    pthread_mutex_unlock(&j->c->mu);
    free(j);
    return NULL;
}

/* tools/call runs on its own thread, so a subagent does not hold up the calls
 * made alongside it; the rest is answered in order. */
static int sa_relay_dispatch(sa_relay_conn *c, cJSON *m) {
    const char *method = sa_jstr(m, "method");
    if (!method || strcmp(method, "tools/call")) {
        int ok = sa_relay_handle(c, m);
        cJSON_Delete(m);
        return ok;
    }
    sa_relay_job *j = malloc(sizeof *j);
    pthread_t t;
    if (!j) { cJSON_Delete(m); return 0; }
    *j = (sa_relay_job){ c, m };
    pthread_mutex_lock(&c->mu);
    c->inflight++;
    pthread_mutex_unlock(&c->mu);
    if (pthread_create(&t, NULL, sa_relay_call, j)) {
        pthread_mutex_lock(&c->mu);
        c->inflight--;
        pthread_mutex_unlock(&c->mu);
        free(j);
        int ok = sa_relay_handle(c, m);
        cJSON_Delete(m);
        return ok;
    }
    pthread_detach(t);
    return 1;
}

static void *sa_relay_serve(void *arg) {
    sa_relay_conn *c = arg;
    sa_agent *x = c->x;
    sa_buf in = {0};
    char chunk[8192];
    while (!x->relay_stop) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        int ready = poll(&p, 1, 200);
        if (ready < 0) break;
        if (!ready) continue;
        ssize_t r = read(c->fd, chunk, sizeof chunk);
        if (r <= 0) break;
        sa_put(&in, chunk, (size_t)r);
        char *nl;
        int ok = 1;
        while (ok && in.n && (nl = memchr(in.p, '\n', in.n))) {
            *nl = '\0';
            cJSON *m = cJSON_Parse(in.p);
            if (m) ok = sa_relay_dispatch(c, m);
            size_t used = (size_t)(nl + 1 - in.p);
            memmove(in.p, nl + 1, in.n - used);
            in.n -= used;
        }
        if (!ok) break;
    }
    sa_free(&in);
    pthread_mutex_lock(&c->mu);
    while (c->inflight) pthread_cond_wait(&c->cond, &c->mu);
    pthread_mutex_unlock(&c->mu);
    close(c->fd);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cond);
    free(c);
    pthread_mutex_lock(&x->conn_mu);
    x->nconn--;
    pthread_cond_broadcast(&x->conn_cond);
    pthread_mutex_unlock(&x->conn_mu);
    return NULL;
}

static void *sa_relay_accept(void *arg) {
    sa_agent *x = arg;
    while (!x->relay_stop) {
        struct pollfd p = { .fd = x->lfd, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(x->lfd, NULL, NULL);
        if (fd < 0) continue;
        sa_relay_conn *c = malloc(sizeof *c);
        pthread_t t;
        if (!c) { close(fd); continue; }
        *c = (sa_relay_conn){ .x = x, .fd = fd };
        pthread_mutex_init(&c->mu, NULL);
        pthread_cond_init(&c->cond, NULL);
        pthread_mutex_lock(&x->conn_mu);
        x->nconn++;
        pthread_mutex_unlock(&x->conn_mu);
        if (pthread_create(&t, NULL, sa_relay_serve, c)) {
            close(fd);
            pthread_mutex_destroy(&c->mu);
            pthread_cond_destroy(&c->cond);
            free(c);
            pthread_mutex_lock(&x->conn_mu);
            x->nconn--;
            pthread_mutex_unlock(&x->conn_mu);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

static int sa_relay_listen(sa_agent *x) {
    static _Atomic long seq;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(x->sock, sizeof x->sock, "/tmp/optchat-%d-%ld.sock", (int)getpid(), ++seq);
    snprintf(a.sun_path, sizeof a.sun_path, "%s", x->sock);
    unlink(x->sock);
    x->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (x->lfd < 0) return 0;
    fcntl(x->lfd, F_SETFD, FD_CLOEXEC);
    mode_t old = umask(077);
    int bound = !bind(x->lfd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (!bound || listen(x->lfd, 8) || pthread_create(&x->accept_thread, NULL, sa_relay_accept, x)) {
        snprintf(x->err, sizeof x->err, "could not listen on %s", x->sock);
        close(x->lfd);
        x->lfd = -1;
        unlink(x->sock);
        return 0;
    }
    return 1;
}

static void sa_relay_close(sa_agent *x) {
    if (x->lfd < 0) return;
    x->relay_stop = 1;
    pthread_join(x->accept_thread, NULL);
    close(x->lfd);
    x->lfd = -1;
    unlink(x->sock);
    pthread_mutex_lock(&x->conn_mu);
    while (x->nconn) pthread_cond_wait(&x->conn_cond, &x->conn_mu);
    pthread_mutex_unlock(&x->conn_mu);
}

static void sa_drive_event(void *ud, const backend_event *ev) {
    sa_agent *x = ud;
    backend_emit(&x->st, ev);
    if (!x->mem || x->sub || ev->parent) return;
    if (ev->kind == BACKEND_EV_ASSISTANT && ev->text) {
        oc_append(x->mem, "talk", ev->text);
    } else if (ev->kind == BACKEND_EV_TOOL) {
        sa_buf b = {0};
        const char *raw = ev->input_json ? ev->input_json : ev->arg;
        cJSON *input = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
        sa_tool_log(&b, ev->name, input, raw);
        oc_append(x->mem, "tool", sa_str(&b));
        sa_free(&b);
        cJSON_Delete(input);
    } else if (ev->kind == BACKEND_EV_TOOL_RESULT) {
        sa_echo(x, ev->text ? ev->text : "");
    }
}

static int sa_drive_start(sa_agent *x) {
    cJSON_Delete(x->config);
    x->config = sa_config_load(x->err, sizeof x->err);
    if (!x->config) return 0;
    if (!x->mem && !sa_memory_open(x)) return 0;
    sa_build_system(x);
    if (x->relay && x->lfd < 0 && !sa_relay_listen(x)) return 0;
    const char *argv[] = { x->relay, "mcp-memory", x->sock, NULL };
    backend_opts o = { .name = x->drive, .model = x->st.model, .effort = x->st.effort, .system = x->system,
                       .cwd = x->st.cwd, .session_file = x->st.session_file,
                       .env = (const char *const *)x->st.env, .permission_mode = x->st.permission,
                       .allow_customizations = x->st.allow_customizations,
                       .no_browser_login = x->st.no_browser_login, .chrome = x->st.chrome,
                       .plugin_dir = x->st.plugin_dir, .ephemeral = 1, .mcp = x->relay ? argv : NULL,
                       /* subagents go through optchat's agent tool, which starts
                          them from this view in their own tab */
                       .no_subagents = 1 };
    if (x->inner) x->inner->close(x->inner);
    x->inner_used = 0;
    x->inner = backend_open_ex(&o);
    if (!x->inner) {
        snprintf(x->err, sizeof x->err, "could not open %s", x->drive);
        return 0;
    }
    x->inner->set_event_cb(x->inner, sa_drive_event, x);
    x->inner->set_abort_check(x->inner, x->st.abort);
    if (x->st.on_permission && x->inner->set_permission_cb)
        x->inner->set_permission_cb(x->inner, x->st.on_permission, x->st.permission_ud);
    if (!x->inner->start(x->inner, NULL)) {
        const char *e = x->inner->last_error ? x->inner->last_error(x->inner) : NULL;
        snprintf(x->err, sizeof x->err, "could not start %s%s%s", x->drive, e ? ": " : "", e ? e : "");
        return 0;
    }
    x->started = 1;
    return 1;
}

static char *sa_drive_ask(sa_agent *x, const char *user, backend_result *meta) {
    backend_result res = {0};
    if (meta) memset(meta, 0, sizeof *meta);
    if (!x->started && !sa_drive_start(x)) {
        if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
        return NULL;
    }
    x->err[0] = '\0';
    sa_buf text = {0}, none = {0};
    if (!sa_memory_turn(x, user ? user : "", &none, &text)) {
        if (meta) { meta->interrupted = 1; snprintf(meta->subtype, sizeof meta->subtype, "interrupted"); }
        return strdup("");
    }
    /* A claude drive is cleared before its first turn too: /clear leaves
       blocks of its own at the head of the next message, so the first turn
       would otherwise start a prefix that no later turn shares. */
    if ((x->inner_used || !strcmp(x->drive, "claude")) && !x->inner->reset(x->inner)) {
        snprintf(x->err, sizeof x->err, "could not clear the %s context", x->drive);
        sa_free(&text);
        if (meta) { meta->is_error = 1; snprintf(meta->subtype, sizeof meta->subtype, "error"); }
        return NULL;
    }
    x->inner_used = 1;
    char *reply = x->inner->ask_ex(x->inner, sa_str(&text), &res);
    sa_free(&text);
    if (!reply && !res.interrupted) {
        const char *e = x->inner->last_error ? x->inner->last_error(x->inner) : NULL;
        snprintf(x->err, sizeof x->err, "%s", e && *e ? e : "no reply");
        res.is_error = 1;
    }
    if (meta) *meta = res;
    return reply;
}

static int sa_memory_open(sa_agent *x) {
    char dir[4096], *slash;
    if (!core_agent_config_dir(dir, sizeof dir) || !(slash = strrchr(dir, '/'))) {
        snprintf(x->err, sizeof x->err, "no config directory (HOME unset)");
        return 0;
    }
    snprintf(slash, sizeof dir - (size_t)(slash - dir), "/memory");
    snprintf(x->compactor_log, sizeof x->compactor_log, "%s/usage.jsonl", dir);
    x->mem = oc_open(dir, 0, 0, x->err, sizeof x->err);
    if (!x->mem) return 0;
    const char *backend = sa_jstr(x->config, "compactor_backend"), *model = sa_jstr(x->config, "compactor_model");
    snprintf(x->compactor_backend, sizeof x->compactor_backend, "%s", backend ? backend : OC_COMPACT_BACKEND);
    snprintf(x->compactor_model, sizeof x->compactor_model, "%s", model ? model : OC_COMPACT_MODEL);
    x->compactor = oc_compactor_start(x->mem, 0, sa_compactor_open, x);
    return 1;
}

static int sa_start(Backend *b, const char *resume) {
    sa_agent *x = b->ctx;
    if (x->drive[0]) return sa_drive_start(x);
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
    if (x->st.memory && !x->mem && !sa_memory_open(x)) return 0;
    sa_mcp_load(x);
    if (resume && *resume && !x->mem) {
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
    if (x->drive[0]) return 1;
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

static void sa_set_abort(Backend *b, int (*cb)(void)) {
    sa_agent *x = b->ctx;
    x->st.abort = cb;
    if (x->inner) x->inner->set_abort_check(x->inner, cb);
}

static void sa_set_permission_cb(Backend *b, int (*cb)(void *ud, const backend_permission *req), void *ud) {
    sa_agent *x = b->ctx;
    x->st.on_permission = cb;
    x->st.permission_ud = ud;
    if (x->inner && x->inner->set_permission_cb) x->inner->set_permission_cb(x->inner, cb, ud);
}

static void sa_rate_limit(Backend *b, backend_rate_limit *out) {
    sa_agent *x = b->ctx;
    memset(out, 0, sizeof *out);
    if (x->inner && x->inner->rate_limit) x->inner->rate_limit(x->inner, out);
}

static void sa_set_agent_host(Backend *b,
                              int (*host)(void *ud, Backend *child, const char *task, const char *cwd,
                                          const char *backend, const char *model, const char *effort,
                                          char **report),
                              void *ud) {
    sa_agent *x = b->ctx;
    x->agent_host = host;
    x->agent_ud = ud;
}

static char *sa_memory_view(Backend *b) {
    sa_agent *x = b->ctx;
    return x->mem ? oc_render(x->mem, 0) : NULL;
}

static long sa_memory_pending(Backend *b) {
    sa_agent *x = b->ctx;
    return x->mem ? oc_pending(x->mem) : -1;
}

static int sa_set_effort(Backend *b, const char *effort) {
    sa_agent *x = b->ctx;
    if (x->inner && !x->inner->set_effort(x->inner, effort)) return 0;
    backend_set(&x->st.effort, effort);
    return 1;
}

static void sa_usage(Backend *b, long *tokens, long *window) {
    sa_agent *x = b->ctx;
    if (x->inner && x->inner->usage) { x->inner->usage(x->inner, tokens, window); return; }
    *tokens = x->ctx_tokens;
    *window = x->window;
}

static const char *sa_session_id(Backend *b) {
    sa_agent *x = b->ctx;
    return x->st.ephemeral || x->mem || !x->id[0] ? NULL : x->id;
}

static const char *sa_model(Backend *b) {
    sa_agent *x = b->ctx;
    const char *m = x->inner ? x->inner->model(x->inner) : NULL;
    if (m) return m;
    return x->route.full ? x->route.full : x->st.model;
}

static const char *sa_error(Backend *b) {
    sa_agent *x = b->ctx;
    if (!x->err[0] && x->inner && x->inner->last_error) return x->inner->last_error(x->inner);
    return x->err[0] ? x->err : NULL;
}

static void sa_close(Backend *b) {
    sa_agent *x = b->ctx;
    x->halting = 1;
    if (x->inner) x->inner->close(x->inner);
    sa_relay_close(x);
    free(x->relay);
    pthread_mutex_destroy(&x->conn_mu);
    pthread_cond_destroy(&x->conn_cond);
    oc_compactor_stop(x->compactor);
    if (x->sub) oc_release(x->mem);
    else oc_close(x->mem);
    sa_close_file(x);
    cJSON_Delete(x->msgs);
    cJSON_Delete(x->config);
    cJSON_Delete(x->hooks);
    sa_mcp_free(x);
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
    x->lfd = -1;
    pthread_mutex_init(&x->conn_mu, NULL);
    pthread_cond_init(&x->conn_cond, NULL);
    if (o->memory && o->name && strcmp(o->name, "core")) {
        snprintf(x->drive, sizeof x->drive, "%s", o->name);
        x->relay = o->memory_relay ? strdup(o->memory_relay) : NULL;
    }
    b->ctx = x;
    b->caps = x->drive[0] ? BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT
                          : BACKEND_CAP_RESUME | BACKEND_CAP_EFFORT | BACKEND_CAP_LIVE_EFFORT;
    b->ask = sa_ask;
    b->reset = sa_reset;
    b->close = sa_close;
    b->start = sa_start;
    b->ask_ex = sa_ask_ex;
    b->usage = sa_usage;
    b->set_model = backend_set_model_generic;
    b->set_effort = sa_set_effort;
    b->set_permission = x->drive[0] ? backend_set_permission_generic : backend_set_permission_none;
    if (x->drive[0]) {
        b->set_permission_cb = sa_set_permission_cb;
        b->rate_limit = sa_rate_limit;
    }
    b->set_event_cb = sa_set_event_cb;
    b->set_abort_check = sa_set_abort;
    b->set_agent_host = sa_set_agent_host;
    b->session_id = sa_session_id;
    b->memory_view = sa_memory_view;
    b->memory_pending = sa_memory_pending;
    b->model = sa_model;
    b->effort = backend_stored_effort;
    b->auth_source = backend_none;
    b->last_error = sa_error;
    return b;
}

#endif /* CORE_AGENT_IMPLEMENTATION */
