#ifndef GROKBOT_H
#define GROKBOT_H

#include <stddef.h>
#include "cJSON.h"

typedef struct grokbot grokbot;

typedef struct {
    const char *gateway_url;
    const char *gateway_token;
    const char *extra_headers_json;
} grokbot_opts;

typedef struct {
    char *id;
    char *name;
    char *title;
    char *description;
    int   is_group;
    cJSON *raw;
} grokbot_agent;

grokbot *grokbot_open(const grokbot_opts *opts);
void     grokbot_close(grokbot *g);
const char *grokbot_error(grokbot *g);
const char *grokbot_gateway_url(grokbot *g);

cJSON *grokbot_call(grokbot *g, const char *method, cJSON *body);

cJSON *grokbot_list_agents(grokbot *g);
int    grokbot_resolve(grokbot *g, const char *ref, grokbot_agent *out);
void   grokbot_agent_free(grokbot_agent *a);

cJSON *grokbot_create_agent(grokbot *g, const char *name, const char *description,
                            const char *title);
int    grokbot_update_agent(grokbot *g, const char *ref, const char *name,
                            const char *description, const char *title);
int    grokbot_set_notify(grokbot *g, const char *ref, int enabled);
int    grokbot_set_hidden(grokbot *g, const char *ref, int hidden);
int    grokbot_delete_agent(grokbot *g, const char *ref);

cJSON *grokbot_create_group(grokbot *g, const char *name, const char *description,
                            const char **member_refs, int n_members);
cJSON *grokbot_set_group_members(grokbot *g, const char *group_ref,
                                 const char **member_refs, int n_members);

char  *grokbot_send(grokbot *g, const char *ref, const char *prompt,
                    const char *reply_to_id);
cJSON *grokbot_transcript_tail(grokbot *g, const char *ref, int limit);
cJSON *grokbot_thread(grokbot *g, const char *ref, const char *root_id);

/* Re-reads the app's gateway descriptor (token rotation). 0 when the client
 * was opened from explicit url/token or the reload failed. */
int grokbot_reload(grokbot *g);

typedef struct {
    char  *agent_id;
    char  *state;     /* running | absent | hibernated | ... */
    char  *vnc_url;   /* box-local noVNC URL; NULL when there is no desktop */
    int    display;   /* X display number from token=N; -1 when unknown */
    int    handoff;   /* bot is waiting for a human on this display */
    cJSON *raw;
} grokbot_box;

/* getForeverBoxStatus. Read-only. */
int  grokbot_box_status(grokbot *g, const char *ref, grokbot_box *out);
/* ensureForeverBox. Starts the agent's desktop; call only on explicit request. */
int  grokbot_box_ensure(grokbot *g, const char *ref, grokbot_box *out);
void grokbot_box_free(grokbot_box *b);

typedef struct {
    char url[2048];     /* wss://.../websockify?... */
    char header[1024];  /* "name: value" auth header */
} grokbot_vnc_endpoint;

/* box NULL -> base desktop (:1, port 6080). Otherwise the box's display via
 * port 6081; fails when the box has no running desktop. */
int grokbot_vnc_endpoint_for(grokbot *g, const grokbot_box *box,
                             grokbot_vnc_endpoint *out);

#endif

#ifdef GROKBOT_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#endif

#define GB_TIMEOUT_S 30L
#define GB_MAX_BODY  (2 * 1024 * 1024)

struct grokbot {
    CURL *curl;
    char *url;
    char *token;
    cJSON *headers;
    char *vnc_primary;
    char *vnc_fork;
    char *vnc_network_token;
    char *extra_headers_json;
    int   from_app;
    char err[512];
};

typedef struct { char *p; size_t n; } gb_buf;

static size_t gb_write(void *d, size_t s, size_t n, void *ud) {
    gb_buf *b = ud;
    size_t add = s * n;
    if (b->n + add > GB_MAX_BODY) return 0;
    char *np = realloc(b->p, b->n + add + 1);
    if (!np) return 0;
    b->p = np;
    memcpy(b->p + b->n, d, add);
    b->n += add;
    b->p[b->n] = 0;
    return add;
}

static void gb_seterr(grokbot *g, const char *fmt, const char *a, const char *b) {
    snprintf(g->err, sizeof g->err, fmt, a ? a : "", b ? b : "");
}

static char *gb_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *gb_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    gb_buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (gb_write(tmp, 1, n, &b) != n) { free(b.p); fclose(f); return NULL; }
    }
    fclose(f);
    return b.p ? b.p : gb_strdup("");
}

static char *gb_run(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    gb_buf b = {0};
    char tmp[1024];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, p)) > 0) gb_write(tmp, 1, n, &b);
    int rc = pclose(p);
    if (rc != 0 || !b.p) { free(b.p); return NULL; }
    while (b.n && (b.p[b.n - 1] == '\n' || b.p[b.n - 1] == '\r')) b.p[--b.n] = 0;
    return b.p;
}

static unsigned char *gb_b64dec(const char *s, size_t *out_n) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(s);
    unsigned char *out = malloc(len * 3 / 4 + 4);
    if (!out) return NULL;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        const char *q = strchr(tbl, s[i]);
        if (s[i] == '=' ) break;
        if (!q || !s[i]) continue;
        acc = (acc << 6) | (unsigned)(q - tbl);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *out_n = n;
    return out;
}

#ifdef __APPLE__
static char *gb_decrypt_safe_storage(const char *enc_b64, const char *password,
                                     grokbot *g) {
    size_t n;
    unsigned char *enc = gb_b64dec(enc_b64, &n);
    if (!enc || n < 3 || memcmp(enc, "v10", 3) != 0) {
        free(enc);
        gb_seterr(g, "unsupported Safe Storage format", NULL, NULL);
        return NULL;
    }
    unsigned char key[16];
    CCKeyDerivationPBKDF(kCCPBKDF2, password, strlen(password),
                         (const uint8_t *)"saltysalt", 9,
                         kCCPRFHmacAlgSHA1, 1003, key, sizeof key);
    unsigned char iv[16];
    memset(iv, ' ', sizeof iv);
    size_t clen = n - 3;
    char *out = malloc(clen + 16 + 1);
    size_t moved = 0;
    CCCryptorStatus st = CCCrypt(kCCDecrypt, kCCAlgorithmAES128, kCCOptionPKCS7Padding,
                                 key, sizeof key, iv, enc + 3, clen, out, clen + 16, &moved);
    free(enc);
    if (st != kCCSuccess) {
        free(out);
        gb_seterr(g, "Safe Storage decrypt failed", NULL, NULL);
        return NULL;
    }
    out[moved] = 0;
    return out;
}

static int gb_load_app_session(grokbot *g) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    char path[1024];
    snprintf(path, sizeof path,
             "%s/Library/Application Support/Grok Bot/gateway-descriptor.json", home);
    char *txt = gb_read_file(path);
    if (!txt) { gb_seterr(g, "no Grok Bot session at %s", path, NULL); return 0; }
    cJSON *wrapped = cJSON_Parse(txt);
    free(txt);
    if (!wrapped) { gb_seterr(g, "bad gateway descriptor JSON", NULL, NULL); return 0; }

    const char *enc = NULL;
    cJSON *ver = cJSON_GetObjectItem(wrapped, "version");
    if (ver && cJSON_IsNumber(ver) && ver->valueint == 2) {
        cJSON *entries = cJSON_GetObjectItem(wrapped, "entries");
        int cnt = entries ? cJSON_GetArraySize(entries) : 0;
        if (cnt != 1) {
            gb_seterr(g, cnt ? "descriptor has multiple gateway entries"
                             : "descriptor has no gateway entries", NULL, NULL);
            cJSON_Delete(wrapped);
            return 0;
        }
        enc = cJSON_GetStringValue(cJSON_GetObjectItem(entries->child, "encrypted"));
    } else {
        enc = cJSON_GetStringValue(cJSON_GetObjectItem(wrapped, "encrypted"));
    }
    if (!enc) { gb_seterr(g, "descriptor missing encrypted payload", NULL, NULL);
                cJSON_Delete(wrapped); return 0; }

    char *pw = gb_run("/usr/bin/security find-generic-password -w -s 'Grok Bot Safe Storage' 2>/dev/null");
    if (!pw) { gb_seterr(g, "keychain lookup failed", NULL, NULL);
               cJSON_Delete(wrapped); return 0; }
    char *clear = gb_decrypt_safe_storage(enc, pw, g);
    memset(pw, 0, strlen(pw));
    free(pw);
    cJSON_Delete(wrapped);
    if (!clear) return 0;

    cJSON *d = cJSON_Parse(clear);
    memset(clear, 0, strlen(clear));
    free(clear);
    if (!d) { gb_seterr(g, "bad decrypted descriptor", NULL, NULL); return 0; }
    const char *base = cJSON_GetStringValue(cJSON_GetObjectItem(d, "baseUrl"));
    const char *tok  = cJSON_GetStringValue(cJSON_GetObjectItem(d, "token"));
    if (!base || !tok) { gb_seterr(g, "descriptor incomplete", NULL, NULL);
                         cJSON_Delete(d); return 0; }
    g->url = gb_strdup(base);
    g->token = gb_strdup(tok);
    cJSON *h = cJSON_GetObjectItem(d, "headers");
    g->headers = h ? cJSON_Duplicate(h, 1) : NULL;
    cJSON *vp = cJSON_GetObjectItem(d, "vncProxy");
    g->vnc_primary = gb_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(vp, "primaryUrl")));
    g->vnc_fork = gb_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(vp, "forkBaseUrl")));
    g->vnc_network_token =
        gb_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(vp, "networkToken")));
    cJSON_Delete(d);
    size_t L = strlen(g->url);
    if (L && g->url[L - 1] == '/') g->url[L - 1] = 0;
    return 1;
}
#else
static int gb_load_app_session(grokbot *g) {
    gb_seterr(g, "app session loading only implemented on macOS", NULL, NULL);
    return 0;
}
#endif

static _Thread_local char gb_open_err[512];

static void gb_merge_extra_headers(grokbot *g) {
    cJSON *h = g->extra_headers_json ? cJSON_Parse(g->extra_headers_json) : NULL;
    if (!h) return;
    if (!g->headers) g->headers = cJSON_CreateObject();
    for (cJSON *it = h->child; it; it = it->next) {
        cJSON_DeleteItemFromObject(g->headers, it->string);
        cJSON_AddItemToObject(g->headers, it->string,
                              cJSON_CreateString(cJSON_GetStringValue(it)));
    }
    cJSON_Delete(h);
}

grokbot *grokbot_open(const grokbot_opts *opts) {
    gb_open_err[0] = 0;
    grokbot *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    const char *url = opts ? opts->gateway_url : NULL;
    const char *tok = opts ? opts->gateway_token : NULL;
    if (!url) url = getenv("GROK_BOT_GATEWAY_URL");
    if (!tok) tok = getenv("GROK_BOT_GATEWAY_TOKEN");
    if (url && tok) {
        g->url = gb_strdup(url);
        g->token = gb_strdup(tok);
        size_t L = strlen(g->url);
        if (L && g->url[L - 1] == '/') g->url[L - 1] = 0;
    } else if (!gb_load_app_session(g)) {
        snprintf(gb_open_err, sizeof gb_open_err, "%s", g->err);
        grokbot_close(g);
        return NULL;
    } else {
        g->from_app = 1;
    }
    if (opts && opts->extra_headers_json) {
        g->extra_headers_json = gb_strdup(opts->extra_headers_json);
        gb_merge_extra_headers(g);
    }
    g->curl = curl_easy_init();
    if (!g->curl) { grokbot_close(g); return NULL; }
    return g;
}

static void gb_free_secret(char **s) {
    if (*s) memset(*s, 0, strlen(*s));
    free(*s);
    *s = NULL;
}

static void gb_clear_session(grokbot *g) {
    gb_free_secret(&g->token);
    gb_free_secret(&g->vnc_primary);
    gb_free_secret(&g->vnc_network_token);
    free(g->url);
    free(g->vnc_fork);
    g->url = g->vnc_fork = NULL;
    cJSON_Delete(g->headers);
    g->headers = NULL;
}

void grokbot_close(grokbot *g) {
    if (!g) return;
    if (g->curl) curl_easy_cleanup(g->curl);
    gb_clear_session(g);
    free(g->extra_headers_json);
    free(g);
}

int grokbot_reload(grokbot *g) {
    if (!g || !g->from_app) return 0;
    char err[sizeof g->err];
    grokbot fresh = {0};
    if (!gb_load_app_session(&fresh)) {
        memcpy(err, fresh.err, sizeof err);
        gb_clear_session(&fresh);
        memcpy(g->err, err, sizeof err);
        return 0;
    }
    gb_clear_session(g);
    g->url = fresh.url;
    g->token = fresh.token;
    g->headers = fresh.headers;
    g->vnc_primary = fresh.vnc_primary;
    g->vnc_fork = fresh.vnc_fork;
    g->vnc_network_token = fresh.vnc_network_token;
    gb_merge_extra_headers(g);
    return 1;
}

/* NULL -> why the last grokbot_open on this thread failed */
const char *grokbot_error(grokbot *g) {
    return g ? g->err : gb_open_err[0] ? gb_open_err : "no client";
}
const char *grokbot_gateway_url(grokbot *g) { return g ? g->url : NULL; }

static cJSON *gb_post(grokbot *g, const char *method, const char *payload, long *status_out) {

    char url[2048];
    snprintf(url, sizeof url, "%s/api/%s", g->url, method);
    char auth[4096];
    snprintf(auth, sizeof auth, "authorization: Bearer %s", g->token);

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "content-type: application/json");
    hdr = curl_slist_append(hdr, auth);
    if (g->headers) {
        for (cJSON *it = g->headers->child; it; it = it->next) {
            const char *v = cJSON_GetStringValue(it);
            if (!v || !*v) continue;
            char line[2048];
            snprintf(line, sizeof line, "%s: %s", it->string, v);
            hdr = curl_slist_append(hdr, line);
        }
    }

    gb_buf out = {0};
    curl_easy_reset(g->curl);
    curl_easy_setopt(g->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g->curl, CURLOPT_URL, url);
    curl_easy_setopt(g->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(g->curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(g->curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(g->curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(g->curl, CURLOPT_TIMEOUT, GB_TIMEOUT_S);
    curl_easy_setopt(g->curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(g->curl, CURLOPT_WRITEFUNCTION, gb_write);
    curl_easy_setopt(g->curl, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(g->curl);
    long status = 0;
    curl_easy_getinfo(g->curl, CURLINFO_RESPONSE_CODE, &status);
    *status_out = status;
    curl_slist_free_all(hdr);
    memset(auth, 0, sizeof auth);

    if (rc != CURLE_OK) {
        gb_seterr(g, "%s: %s", method, curl_easy_strerror(rc));
        free(out.p);
        return NULL;
    }
    cJSON *data = out.p ? cJSON_Parse(out.p) : NULL;
    if (status < 200 || status >= 300) {
        const char *detail = NULL;
        if (data) {
            detail = cJSON_GetStringValue(cJSON_GetObjectItem(data, "message"));
            if (!detail) detail = cJSON_GetStringValue(cJSON_GetObjectItem(data, "error"));
        }
        char code[32];
        snprintf(code, sizeof code, "%ld", status);
        gb_seterr(g, "%s failed: %s", method, code);
        if (detail || out.p) {
            size_t L = strlen(g->err);
            snprintf(g->err + L, sizeof g->err - L, " %.200s", detail ? detail : out.p);
        }
        cJSON_Delete(data);
        free(out.p);
        return NULL;
    }
    free(out.p);
    if (!data) data = cJSON_CreateObject();
    g->err[0] = 0;
    return data;
}

cJSON *grokbot_call(grokbot *g, const char *method, cJSON *body) {
    if (!body) body = cJSON_CreateObject();
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) return NULL;
    long status = 0;
    cJSON *data = gb_post(g, method, payload, &status);
    if (!data && (status == 401 || status == 404) && grokbot_reload(g))
        data = gb_post(g, method, payload, &status);
    free(payload);
    return data;
}

static cJSON *gb_unwrap_list(cJSON *data) {
    if (cJSON_IsArray(data)) return data;
    cJSON *a = cJSON_GetObjectItem(data, "agents");
    if (cJSON_IsArray(a)) return a;
    return NULL;
}

cJSON *grokbot_list_agents(grokbot *g) {
    cJSON *data = grokbot_call(g, "listAgents", NULL);
    if (!data) return NULL;
    cJSON *list = gb_unwrap_list(data);
    cJSON *out = list ? cJSON_Duplicate(list, 1) : cJSON_CreateArray();
    cJSON_Delete(data);
    return out;
}

static const char *gb_str(cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

static int gb_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int gb_is_group(cJSON *a) {
    cJSON *ig = cJSON_GetObjectItem(a, "isGroup");
    if (cJSON_IsBool(ig)) return cJSON_IsTrue(ig);
    cJSON *m = cJSON_GetObjectItem(a, "memberIds");
    if (!m) m = cJSON_GetObjectItem(a, "memberAgentIds");
    return cJSON_IsArray(m) && cJSON_GetArraySize(m) > 0;
}

static const char *gb_id(cJSON *a) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(a, "id"));
    if (!id) id = cJSON_GetStringValue(cJSON_GetObjectItem(a, "agentId"));
    return id;
}

static cJSON *gb_find(grokbot *g, cJSON *list, const char *ref) {
    cJSON *it, *match = NULL;
    int n = 0;
    cJSON_ArrayForEach(it, list) {
        const char *id = gb_id(it);
        if (id && gb_ieq(id, ref)) return it;
    }
    cJSON_ArrayForEach(it, list) {
        if (gb_ieq(gb_str(it, "name"), ref) || gb_ieq(gb_str(it, "title"), ref)) {
            match = it;
            n++;
        }
    }
    if (n == 1) return match;
    gb_seterr(g, n ? "ambiguous name \"%s\"" : "no bot or group named \"%s\"", ref, NULL);
    return NULL;
}

static void gb_fill(grokbot_agent *out, cJSON *a) {
    memset(out, 0, sizeof *out);
    out->id = gb_strdup(gb_id(a));
    out->name = gb_strdup(gb_str(a, "name"));
    out->title = gb_strdup(gb_str(a, "title"));
    out->description = gb_strdup(gb_str(a, "description"));
    out->is_group = gb_is_group(a);
    out->raw = cJSON_Duplicate(a, 1);
}

int grokbot_resolve(grokbot *g, const char *ref, grokbot_agent *out) {
    cJSON *list = grokbot_list_agents(g);
    if (!list) return 0;
    cJSON *a = gb_find(g, list, ref);
    if (a) gb_fill(out, a);
    cJSON_Delete(list);
    return a != NULL;
}

void grokbot_agent_free(grokbot_agent *a) {
    if (!a) return;
    free(a->id); free(a->name); free(a->title); free(a->description);
    cJSON_Delete(a->raw);
    memset(a, 0, sizeof *a);
}

static cJSON *gb_unwrap_one(cJSON *data) {
    cJSON *a = cJSON_GetObjectItem(data, "agent");
    cJSON *out = cJSON_Duplicate(a ? a : data, 1);
    cJSON_Delete(data);
    return out;
}

cJSON *grokbot_create_agent(grokbot *g, const char *name, const char *description,
                            const char *title) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "name", name);
    cJSON_AddStringToObject(b, "description", description ? description : "");
    cJSON_AddStringToObject(b, "title", title ? title : "");
    cJSON_AddStringToObject(b, "avatarShape", "");
    cJSON_AddStringToObject(b, "avatarColor", "");
    cJSON_AddStringToObject(b, "origin", "user");
    cJSON *data = grokbot_call(g, "createAgent", b);
    return data ? gb_unwrap_one(data) : NULL;
}

int grokbot_update_agent(grokbot *g, const char *ref, const char *name,
                         const char *description, const char *title) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return 0;
    cJSON *profile = cJSON_CreateObject();
    cJSON_AddStringToObject(profile, "name", name ? name : a.name);
    cJSON_AddStringToObject(profile, "description", description ? description : a.description);
    if (title) cJSON_AddStringToObject(profile, "title", title);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    cJSON_AddItemToObject(b, "profile", profile);
    grokbot_agent_free(&a);
    cJSON *data = grokbot_call(g, "updateAgent", b);
    cJSON_Delete(data);
    return data != NULL;
}

static int gb_set_flag(grokbot *g, const char *ref, const char *method,
                       const char *key, int val) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    cJSON_AddBoolToObject(b, key, val);
    grokbot_agent_free(&a);
    cJSON *data = grokbot_call(g, method, b);
    cJSON_Delete(data);
    return data != NULL;
}

int grokbot_set_notify(grokbot *g, const char *ref, int enabled) {
    return gb_set_flag(g, ref, "setAgentNotifyOnUpdates", "isEnabled", enabled);
}

int grokbot_set_hidden(grokbot *g, const char *ref, int hidden) {
    return gb_set_flag(g, ref, "setAgentHiddenFromSidebar", "isHidden", hidden);
}

int grokbot_delete_agent(grokbot *g, const char *ref) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    grokbot_agent_free(&a);
    cJSON *data = grokbot_call(g, "deleteAgent", b);
    cJSON_Delete(data);
    return data != NULL;
}

static cJSON *gb_member_ids(grokbot *g, cJSON *list, const char **refs, int n) {
    cJSON *ids = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *a = gb_find(g, list, refs[i]);
        if (!a) { cJSON_Delete(ids); return NULL; }
        if (gb_is_group(a)) {
            gb_seterr(g, "cannot add group \"%s\" as a member", refs[i], NULL);
            cJSON_Delete(ids);
            return NULL;
        }
        const char *id = gb_id(a);
        int dup = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, ids) if (!strcmp(cJSON_GetStringValue(e), id)) dup = 1;
        if (!dup) cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    }
    if (cJSON_GetArraySize(ids) == 0) {
        gb_seterr(g, "a group needs at least one member", NULL, NULL);
        cJSON_Delete(ids);
        return NULL;
    }
    return ids;
}

cJSON *grokbot_create_group(grokbot *g, const char *name, const char *description,
                            const char **member_refs, int n_members) {
    cJSON *list = grokbot_list_agents(g);
    if (!list) return NULL;
    cJSON *ids = gb_member_ids(g, list, member_refs, n_members);
    cJSON_Delete(list);
    if (!ids) return NULL;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "name", name);
    cJSON_AddStringToObject(b, "description", description ? description : "");
    cJSON_AddItemToObject(b, "memberAgentIds", ids);
    cJSON *data = grokbot_call(g, "createGroup", b);
    return data ? gb_unwrap_one(data) : NULL;
}

cJSON *grokbot_set_group_members(grokbot *g, const char *group_ref,
                                 const char **member_refs, int n_members) {
    cJSON *list = grokbot_list_agents(g);
    if (!list) return NULL;
    cJSON *grp = gb_find(g, list, group_ref);
    if (!grp || !gb_is_group(grp)) {
        if (grp) gb_seterr(g, "\"%s\" is a bot, not a group", group_ref, NULL);
        cJSON_Delete(list);
        return NULL;
    }
    char *gid = gb_strdup(gb_id(grp));
    cJSON *ids = gb_member_ids(g, list, member_refs, n_members);
    cJSON_Delete(list);
    if (!ids) { free(gid); return NULL; }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", gid);
    cJSON_AddItemToObject(b, "memberAgentIds", ids);
    free(gid);
    cJSON *data = grokbot_call(g, "setGroupMembers", b);
    return data ? gb_unwrap_one(data) : NULL;
}

static void gb_uuid(char out[37]) {
    unsigned char r[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, 16, f) != 16) for (int i = 0; i < 16; i++) r[i] = rand() & 0xff;
    if (f) fclose(f);
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

char *grokbot_send(grokbot *g, const char *ref, const char *prompt,
                   const char *reply_to_id) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return NULL;
    char nonce[37];
    gb_uuid(nonce);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "agentId", a.id);
    cJSON_AddStringToObject(b, "prompt", prompt);
    cJSON_AddStringToObject(b, "clientNonce", nonce);
    if (reply_to_id) cJSON_AddStringToObject(b, "replyToId", reply_to_id);
    grokbot_agent_free(&a);
    cJSON *data = grokbot_call(g, "sendPrompt", b);
    if (!data) return NULL;
    char *mid = gb_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(data, "messageId")));
    cJSON_Delete(data);
    if (!mid) mid = gb_strdup("");
    return mid;
}

cJSON *grokbot_transcript_tail(grokbot *g, const char *ref, int limit) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return NULL;
    if (limit < 1) limit = 40;
    if (limit > 200) limit = 200;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    cJSON_AddNumberToObject(b, "limit", limit);
    grokbot_agent_free(&a);
    return grokbot_call(g, "getAgentTranscriptTail", b);
}

cJSON *grokbot_thread(grokbot *g, const char *ref, const char *root_id) {
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return NULL;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    if (root_id) cJSON_AddStringToObject(b, "rootId", root_id);
    grokbot_agent_free(&a);
    return grokbot_call(g, "getAgentThread", b);
}

static int gb_box_call(grokbot *g, const char *method, const char *ref, grokbot_box *out) {
    memset(out, 0, sizeof *out);
    out->display = -1;
    grokbot_agent a;
    if (!grokbot_resolve(g, ref, &a)) return 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", a.id);
    out->agent_id = a.id;
    a.id = NULL;
    grokbot_agent_free(&a);
    cJSON *data = grokbot_call(g, method, b);
    if (!data) { grokbot_box_free(out); out->display = -1; return 0; }
    out->raw = data;
    out->state = gb_strdup(gb_str(data, "state"));
    out->vnc_url = gb_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(data, "vncUrl")));
    cJSON *h = cJSON_GetObjectItem(data, "handoff");
    out->handoff = h && !cJSON_IsNull(h);
    const char *t = out->vnc_url ? strstr(out->vnc_url, "token%3D") : NULL;
    if (t) out->display = atoi(t + 8);
    else if (out->vnc_url && (t = strstr(out->vnc_url, "token="))) out->display = atoi(t + 6);
    return 1;
}

int grokbot_box_status(grokbot *g, const char *ref, grokbot_box *out) {
    return gb_box_call(g, "getForeverBoxStatus", ref, out);
}

int grokbot_box_ensure(grokbot *g, const char *ref, grokbot_box *out) {
    return gb_box_call(g, "ensureForeverBox", ref, out);
}

void grokbot_box_free(grokbot_box *b) {
    if (!b) return;
    free(b->agent_id); free(b->state); free(b->vnc_url);
    cJSON_Delete(b->raw);
    memset(b, 0, sizeof *b);
}

static int gb_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Copies the percent-decoded value of query parameter `key` from url. */
static int gb_query_param(const char *url, const char *key, char *out, size_t cap) {
    const char *q = strchr(url, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t n = 0;
            for (; *v && *v != '&'; v++) {
                int c = (unsigned char)*v;
                if (c == '%' && gb_hexval(v[1]) >= 0 && gb_hexval(v[2]) >= 0) {
                    c = gb_hexval(v[1]) * 16 + gb_hexval(v[2]);
                    v += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (n + 1 >= cap) return 0;
                out[n++] = (char)c;
            }
            out[n] = 0;
            return 1;
        }
        q = strchr(q, '&');
    }
    return 0;
}

static int gb_url_host(const char *url, char *out, size_t cap) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/?#");
    if (!n || n >= cap) return 0;
    memcpy(out, h, n);
    out[n] = 0;
    return 1;
}

int grokbot_vnc_endpoint_for(grokbot *g, const grokbot_box *box,
                             grokbot_vnc_endpoint *out) {
    memset(out, 0, sizeof *out);
    char host[512];
    int n;
    int base = !box || (box->vnc_url && strstr(box->vnc_url, ":6080/"));
    if (box && (!box->vnc_url || !box->state || strcmp(box->state, "running"))) {
        gb_seterr(g, "no running desktop (state %s)", box->state ? box->state : "unknown", NULL);
        return 0;
    }
    if (base) {
        char path[1536], port_token[1024];
        if (!g->vnc_primary || !gb_url_host(g->vnc_primary, host, sizeof host) ||
            !gb_query_param(g->vnc_primary, "path", path, sizeof path) ||
            !gb_query_param(g->vnc_primary, "port_token", port_token, sizeof port_token)) {
            gb_seterr(g, "descriptor has no usable vncProxy.primaryUrl", NULL, NULL);
            return 0;
        }
        n = snprintf(out->url, sizeof out->url, "wss://%s/%s", host, path);
        if (n < 0 || (size_t)n >= sizeof out->url) goto toolong;
        n = snprintf(out->header, sizeof out->header, "x-anyrun-port-token: %s", port_token);
        memset(port_token, 0, sizeof port_token);
        if (n < 0 || (size_t)n >= sizeof out->header) goto toolong;
        return 1;
    }
    if (box->display < 0) {
        gb_seterr(g, "no display number in vncUrl", NULL, NULL);
        return 0;
    }
    if (!g->vnc_fork || !g->vnc_network_token || !gb_url_host(g->vnc_fork, host, sizeof host)) {
        gb_seterr(g, "descriptor has no usable vncProxy.forkBaseUrl", NULL, NULL);
        return 0;
    }
    n = snprintf(out->url, sizeof out->url,
                 "wss://%s/websockify?token=%d&network_token=%s"
                 "&resume_lower_s=900&resume_upper_s=18000",
                 host, box->display, g->vnc_network_token);
    if (n < 0 || (size_t)n >= sizeof out->url) goto toolong;
    n = snprintf(out->header, sizeof out->header, "x-anyrun-network-token: %s",
                 g->vnc_network_token);
    if (n < 0 || (size_t)n >= sizeof out->header) goto toolong;
    return 1;
toolong:
    memset(out, 0, sizeof *out);
    gb_seterr(g, "vnc endpoint exceeds buffer", NULL, NULL);
    return 0;
}

#endif
