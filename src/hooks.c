#include "hooks.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "text.h"
#include "vendor/cJSON.h"

static struct hook hooks[HOOKS_MAX];
static int         count;
static time_t      loaded_mtime;
static int         loaded;

static void clear(void)
{
    for (int i = 0; i < count; i++) {
        free(hooks[i].event);
        free(hooks[i].tool);
        for (int j = 0; j < hooks[i].match_count; j++)
            free(hooks[i].match[j]);
        free(hooks[i].context);
    }
    memset(hooks, 0, sizeof hooks);
    count = 0;
}

int hooks_path(char *out, size_t cap)
{
    return path_config_file(out, cap, "hooks.yaml");
}

static int indent_of(const char *line)
{
    int n = 0;
    while (line[n] == ' ')
        n++;
    return n;
}

static int blank_or_comment(const char *line)
{
    const char *p = line + indent_of(line);
    return *p == '\0' || *p == '\r' || *p == '#';
}

static char *rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                 s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

/* A plain or quoted scalar on one line. */
static char *scalar(const char *raw)
{
    while (*raw == ' ' || *raw == '\t')
        raw++;
    char *v = strdup(raw);
    if (!v)
        return NULL;
    rtrim(v);
    if (v[0] == '"' || v[0] == '\'') {
        char q = v[0];
        char *out = v, *in = v + 1;
        while (*in) {
            if (q == '"' && *in == '\\' && in[1]) {
                in++;
                *out++ = *in == 'n' ? '\n' : *in == 't' ? '\t' : *in;
                in++;
            } else if (q == '\'' && *in == '\'' && in[1] == '\'') {
                *out++ = '\'';
                in += 2;
            } else if (*in == q)
                break;
            else
                *out++ = *in++;
        }
        *out = '\0';
        return v;
    }
    char *hash = strstr(v, " #");
    if (hash)
        *hash = '\0';
    rtrim(v);
    return v;
}

/* Block scalar (| or >): the following lines indented past `indent`, with
   the first line's indent stripped. `>` folds single newlines into spaces.
   Advances *cursor past the block. */
static char *block(char **cursor, int indent, int fold)
{
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';
    int base = -1;
    while (**cursor) {
        char *line = *cursor;
        char *nl = strchr(line, '\n');
        size_t n = nl ? (size_t)(nl - line) : strlen(line);
        int ind = indent_of(line);
        int empty = (size_t)ind >= n || line[ind] == '\r';
        if (!empty && ind <= indent)
            break;
        if (base < 0 && !empty)
            base = ind;
        const char *body = empty ? "" : line + (ind < base ? ind : base);
        size_t bn = empty ? 0 : n - (size_t)(ind < base ? ind : base);
        if (len + bn + 2 > cap) {
            while (len + bn + 2 > cap)
                cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + len, body, bn);
        len += bn;
        out[len++] = empty ? '\n' : (fold ? ' ' : '\n');
        out[len] = '\0';
        *cursor = nl ? nl + 1 : line + n;
    }
    rtrim(out);
    return out;
}

static void add_match(struct hook *h, char *value)
{
    if (!value || !*value || h->match_count >= HOOKS_MATCH_MAX) {
        free(value);
        return;
    }
    h->match[h->match_count++] = value;
}

/* A flow list: [a, "b, c", d]. Consumes value. */
static void add_matches(struct hook *h, char *value)
{
    char *p = value + 1;
    while (*p) {
        while (*p == ' ')
            p++;
        char *end = p;
        if (*p == '"' || *p == '\'') {
            end = strchr(p + 1, *p);
            end = end ? end + 1 : p + strlen(p);
        }
        while (*end && *end != ',' && *end != ']')
            end++;
        char save = *end;
        *end = '\0';
        add_match(h, scalar(p));
        if (!save)
            break;
        p = end + 1;
        if (save == ']')
            break;
    }
    free(value);
}

/* A block list under `match:`: the following `- item` lines indented past
   `indent`. Advances *cursor past them. */
static void match_list(struct hook *h, char **cursor, int indent)
{
    while (**cursor) {
        char *line = *cursor;
        char *nl = strchr(line, '\n');
        size_t n = nl ? (size_t)(nl - line) : strlen(line);
        int ind = indent_of(line);
        if ((size_t)ind >= n || line[ind] == '\r' || line[ind] == '#') {
            *cursor = nl ? nl + 1 : line + n;
            continue;
        }
        if (ind <= indent || line[ind] != '-' ||
            (line[ind + 1] != ' ' && line[ind + 1] != '\n' && line[ind + 1] != '\0'))
            break;
        char save = line[n];
        line[n] = '\0';
        add_match(h, scalar(line + ind + 1));
        line[n] = save;
        *cursor = nl ? nl + 1 : line + n;
    }
}

static void assign(struct hook *h, const char *key, char *value)
{
    if (!strcmp(key, "match")) {
        if (value && value[0] == '[')
            add_matches(h, value);
        else
            add_match(h, value);
        return;
    }
    char **slot = !strcmp(key, "event")   ? &h->event
                : !strcmp(key, "tool")    ? &h->tool
                : !strcmp(key, "context") ? &h->context
                : NULL;
    if (!slot) {
        free(value);
        return;
    }
    free(*slot);
    *slot = value;
}

int hooks_parse(const char *text)
{
    clear();
    loaded = 1;
    char *copy = strdup(text ? text : "");
    if (!copy)
        return 0;
    char *cursor = copy;
    struct hook *cur = NULL;
    while (*cursor) {
        char *line = cursor;
        char *nl = strchr(line, '\n');
        cursor = nl ? nl + 1 : line + strlen(line);
        if (nl)
            *nl = '\0';
        if (blank_or_comment(line))
            continue;
        int indent = indent_of(line);
        char *p = line + indent;
        if (p[0] == '-' && (p[1] == ' ' || p[1] == '\0' || p[1] == '\r')) {
            if (count >= HOOKS_MAX)
                break;
            cur = &hooks[count++];
            p += 1;
            while (*p == ' ')
                p++;
            indent = (int)(p - line);
            if (*p == '\0' || *p == '\r')
                continue;
        }
        if (!cur)
            continue;
        char *colon = strchr(p, ':');
        if (!colon || (colon[1] != ' ' && colon[1] != '\0' && colon[1] != '\r'))
            continue;
        *colon = '\0';
        const char *key = rtrim(p);
        const char *rest = colon + 1;
        while (*rest == ' ')
            rest++;
        if (rest[0] == '|' || rest[0] == '>') {
            if (nl)
                *nl = '\n';
            assign(cur, key, block(&cursor, indent, rest[0] == '>'));
        } else if (!strcmp(key, "match") && (*rest == '\0' || *rest == '\r' || *rest == '#')) {
            if (nl)
                *nl = '\n';
            match_list(cur, &cursor, indent);
        } else
            assign(cur, key, scalar(rest));
    }
    free(copy);
    for (int i = 0; i < count; i++)
        if (!hooks[i].event || !*hooks[i].event) {
            free(hooks[i].event);
            hooks[i].event = strdup("PreToolUse");
        }
    return count;
}

int hooks_load(void)
{
    char path[4096];
    if (!hooks_path(path, sizeof path))
        return count;
    struct stat st;
    if (stat(path, &st) != 0) {
        if (loaded)
            clear();
        loaded = 1;
        loaded_mtime = 0;
        return 0;
    }
    if (loaded && st.st_mtime == loaded_mtime)
        return count;
    char *text = text_slurp(path, 1u << 20, NULL);
    hooks_parse(text ? text : "");
    free(text);
    loaded_mtime = st.st_mtime;
    return count;
}

int hooks_count(void)
{
    return count;
}

const struct hook *hooks_at(int i)
{
    return i >= 0 && i < count ? &hooks[i] : NULL;
}

int hooks_backend(backend_hook *out, int max)
{
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        out[i].event = hooks[i].event;
        out[i].tool = hooks[i].tool ? hooks[i].tool : "";
    }
    return n;
}

static int any_match(const struct hook *h, const char *text)
{
    for (int i = 0; i < h->match_count; i++)
        if (strstr(text, h->match[i]))
            return 1;
    return 0;
}

/* Each `match` is tested against the tool's command or file path when the
   input has one, else against the whole input; one hit is enough. */
static int matches(const struct hook *h, const char *input_json)
{
    if (!h->match_count)
        return 1;
    if (!input_json)
        return 0;
    cJSON *input = cJSON_Parse(input_json);
    int hit = 0, scoped = 0;
    if (input) {
        static const char *const fields[] = {"command", "file_path", "path", "notebook_path"};
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(input, fields[i]));
            if (!v)
                continue;
            scoped = 1;
            if (any_match(h, v))
                hit = 1;
        }
        cJSON_Delete(input);
    }
    if (!scoped)
        hit = any_match(h, input_json);
    return hit;
}

char *hooks_context(int i, const char *tool, const char *input_json)
{
    (void)tool;
    const struct hook *h = hooks_at(i);
    if (!h || !h->context || !*h->context || !matches(h, input_json))
        return NULL;
    return strdup(h->context);
}
