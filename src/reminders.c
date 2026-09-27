#include "reminders.h"
#include "text.h"
#include "vendor/cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "filelock.h"

#define REMINDERS_MAX_BYTES (1u << 24)
const char *reminders_path(void)
{
    static char path[4200];
    static int  inited;
    if (!inited) {
        inited = 1;
        if (!path_config_file(path, sizeof path, "reminders"))
            path[0] = '\0';
    }
    return path;
}

static int store_lock(int op)
{
    const char *path = reminders_path();
    if (!path || !path[0])
        return -1;
    return filelock_acquire(path, op);
}

static void store_unlock(int fd)
{
    filelock_release(fd);
}

typedef struct {
    cJSON *o;
    time_t at;
} Ent;

static void free_ents(Ent *ents, int nobj)
{
    for (int i = 0; i < nobj; i++)
        if (ents[i].o)
            cJSON_Delete(ents[i].o);
    free(ents);
}

static int parse_ents(char *buf, Ent **ents_out, int *nobj_out)
{
    Ent *ents = NULL;
    int  nobj = 0, cap = 0;
    for (char *p = buf; *p;) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (*p) {
            cJSON *o = cJSON_Parse(p);
            if (o) {
                if (nobj == cap) {
                    int  ncap = cap ? cap * 2 : 64;
                    Ent *ne = realloc(ents, (size_t)ncap * sizeof *ne);
                    if (!ne) {
                        cJSON_Delete(o);
                        free_ents(ents, nobj);
                        return 0;
                    }
                    ents = ne;
                    cap = ncap;
                }
                ents[nobj].o = o;
                ents[nobj].at = (time_t)-1;
                nobj++;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    *ents_out = ents;
    *nobj_out = nobj;
    return 1;
}

int reminders_scheduled_count(void)
{
    if (!reminders_path()[0])
        return 0;
    int lock = store_lock(LOCK_SH);

    size_t len = 0;
    char  *buf = text_slurp(reminders_path(), REMINDERS_MAX_BYTES, &len);
    if (!buf) {
        store_unlock(lock);
        return 0;
    }

    Ent *ents = NULL;
    int  nobj = 0;
    int  n = parse_ents(buf, &ents, &nobj) ? nobj : 0;
    free(buf);
    free_ents(ents, nobj);
    store_unlock(lock);
    return n;
}

static time_t parse_at(const char *s)
{
    struct tm   tm;
    const char *fmts[] = { "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
                           "%Y-%m-%d %H:%M",    "%Y-%m-%dT%H:%M", NULL };
    for (int i = 0; fmts[i]; i++) {
        memset(&tm, 0, sizeof tm);
        tm.tm_isdst = -1;
        if (strptime(s, fmts[i], &tm))
            return mktime(&tm);
    }
    return (time_t)-1;
}

static void fmt_at(time_t t, char *out, size_t n)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, n, "%Y-%m-%d %H:%M", &lt);
}

static void parse_hhmm(const char *s, int *h, int *m)
{
    *h = 8;
    *m = 0;
    if (s)
        sscanf(s, "%d:%d", h, m);
}

static int dow_num(const char *s)
{
    if (!s)
        return -1;
    static const char *nm[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    char b[4] = {0};
    for (int i = 0; i < 3 && s[i]; i++)
        b[i] = (char)tolower((unsigned char)s[i]);
    for (int i = 0; i < 7; i++)
        if (!strcmp(b, nm[i]))
            return i;
    if (s[0] >= '0' && s[0] <= '6' && s[1] == '\0')
        return s[0] - '0';
    return -1;
}

static int day_matches(cJSON *rule, const struct tm *t)
{
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(rule, "kind"));
    if (!kind)
        return 0;
    if (!strcmp(kind, "daily"))
        return 1;
    if (!strcmp(kind, "weekdays"))
        return t->tm_wday >= 1 && t->tm_wday <= 5;
    if (!strcmp(kind, "weekly")) {
        cJSON *days = cJSON_GetObjectItem(rule, "days"), *d;
        cJSON_ArrayForEach(d, days)
            if (dow_num(cJSON_GetStringValue(d)) == t->tm_wday)
                return 1;
        return 0;
    }
    if (!strcmp(kind, "nth_weekday")) {
        int    dow = dow_num(cJSON_GetStringValue(cJSON_GetObjectItem(rule, "dow")));
        cJSON *nj = cJSON_GetObjectItem(rule, "n");
        int    nth = nj ? (int)nj->valuedouble : 1;
        if (t->tm_wday != dow)
            return 0;
        if (nth == -1) {
            struct tm x = *t;
            x.tm_mday += 7;
            x.tm_isdst = -1;
            time_t    tt = mktime(&x);
            struct tm y;
            localtime_r(&tt, &y);
            return y.tm_mon != t->tm_mon;
        }
        return (t->tm_mday - 1) / 7 + 1 == nth;
    }
    if (!strcmp(kind, "monthly")) {
        cJSON *dj = cJSON_GetObjectItem(rule, "dom");
        return t->tm_mday == (dj ? (int)dj->valuedouble : 1);
    }
    return 0;
}

static time_t next_occurrence(cJSON *rule, time_t after)
{
    int hh, mm;
    parse_hhmm(cJSON_GetStringValue(cJSON_GetObjectItem(rule, "time")), &hh, &mm);
    struct tm base;
    localtime_r(&after, &base);
    base.tm_hour = hh;
    base.tm_min = mm;
    base.tm_sec = 0;
    for (int d = 0; d < 420; d++) {
        struct tm day = base;
        day.tm_mday = base.tm_mday + d;
        day.tm_isdst = -1;
        time_t cand = mktime(&day);
        if (cand < after)
            continue;
        if (day_matches(rule, &day))
            return cand;
    }
    return (time_t)-1;
}

static time_t effective_at(cJSON *o, time_t now, int *normalized)
{
    const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(o, "at"));
    time_t      ts = at ? parse_at(at) : (time_t)-1;
    if (ts != (time_t)-1)
        return ts;
    cJSON *rule = cJSON_GetObjectItem(o, "rule");
    if (rule && cJSON_IsObject(rule)) {
        ts = next_occurrence(rule, now);
        if (ts != (time_t)-1) {
            char b[32];
            fmt_at(ts, b, sizeof b);
            cJSON_DeleteItemFromObject(o, "at");
            cJSON_AddStringToObject(o, "at", b);
            *normalized = 1;
        }
    }
    return ts;
}

static int reschedule(cJSON *o, time_t fired_ts, time_t now)
{
    cJSON *rule = cJSON_GetObjectItem(o, "rule");
    if (rule && cJSON_IsObject(rule)) {
        time_t next = next_occurrence(rule, fired_ts + 60);
        if (next == (time_t)-1)
            return 0;
        char b[32];
        fmt_at(next, b, sizeof b);
        cJSON_DeleteItemFromObject(o, "at");
        cJSON_AddStringToObject(o, "at", b);
        return 1;
    }
    cJSON *rep = cJSON_GetObjectItem(o, "repeat_secs");
    long   repeat = (rep && cJSON_IsNumber(rep)) ? (long)rep->valuedouble : 0;
    if (repeat > 0) {
        time_t next = fired_ts + repeat;
        while (next <= now)
            next += repeat;
        char b[32];
        fmt_at(next, b, sizeof b);
        cJSON_DeleteItemFromObject(o, "at");
        cJSON_AddStringToObject(o, "at", b);
        return 1;
    }
    return 0;
}

struct rewrite {
    const Ent  *ents;
    int         nent;
    const char *tail;
    size_t      tail_len;
};

static int write_entries(FILE *f, void *ud)
{
    const struct rewrite *out = ud;

    for (int i = 0; i < out->nent; i++) {
        if (!out->ents[i].o)
            continue;
        char *s = cJSON_PrintUnformatted(out->ents[i].o);
        if (!s)
            return 0;
        fputs(s, f);
        fputc('\n', f);
        free(s);
    }
    if (out->tail_len)
        fwrite(out->tail, 1, out->tail_len, f);
    return 1;
}

static void store_rewrite(const Ent *ents, int nent, const char *orig, size_t orig_len)
{
    const char *path = reminders_path();
    if (!path || !path[0])
        return;
    size_t      cur_len = 0;
    char       *cur = text_slurp(path, REMINDERS_MAX_BYTES, &cur_len);
    if (!cur)
        return;
    if (cur_len < orig_len || memcmp(cur, orig, orig_len) != 0) {
        free(cur);
        return;
    }

    struct rewrite out = {ents, nent, cur + orig_len, cur_len - orig_len};
    text_spit(path, write_entries, &out);
    free(cur);
}

int reminders_drain_due(time_t now, int (*take)(const char *text, void *ud),
                        void *ud)
{
    if (!take || !reminders_path()[0])
        return 0;

    int lock = store_lock(LOCK_EX);

    size_t len = 0;
    char  *buf = text_slurp(reminders_path(), REMINDERS_MAX_BYTES, &len);
    if (!buf) {
        store_unlock(lock);
        return 0;
    }
    char *orig = malloc(len + 1);
    if (!orig) {
        free(buf);
        store_unlock(lock);
        return 0;
    }
    memcpy(orig, buf, len);
    orig[len] = '\0';

    Ent *ents = NULL;
    int  nobj = 0;
    if (!parse_ents(buf, &ents, &nobj)) {
        free(orig);
        free(buf);
        store_unlock(lock);
        return 0;
    }
    free(buf);

    int dirty = 0;
    for (int i = 0; i < nobj; i++)
        ents[i].at = effective_at(ents[i].o, now, &dirty);

    int *due = NULL;
    int  ndue = 0;
    if (nobj > 0) {
        due = malloc((size_t)nobj * sizeof *due);
        if (!due) {
            free(orig);
            free_ents(ents, nobj);
            store_unlock(lock);
            return 0;
        }
        for (int i = 0; i < nobj; i++)
            if (ents[i].at != (time_t)-1 && ents[i].at <= now)
                due[ndue++] = i;
        for (int a = 0; a < ndue; a++)
            for (int b = a + 1; b < ndue; b++)
                if (ents[due[b]].at < ents[due[a]].at) {
                    int t = due[a];
                    due[a] = due[b];
                    due[b] = t;
                }
    }

    int taken = 0;
    for (int d = 0; d < ndue; d++) {
        int         i = due[d];
        const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(ents[i].o, "text"));
        if (!take(txt ? txt : "(reminder)", ud))
            break;
        if (!reschedule(ents[i].o, ents[i].at, now)) {
            cJSON_Delete(ents[i].o);
            ents[i].o = NULL;
        }
        dirty = 1;
        taken++;
    }

    if (dirty)
        store_rewrite(ents, nobj, orig, len);

    free(due);
    free(orig);
    free_ents(ents, nobj);
    store_unlock(lock);
    return taken;
}
