#include "schedule.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HORIZON_DAYS (366 * 8)
#define FIELDS_MAX   10

static const char *const MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static const char *const DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

__attribute__((format(printf, 3, 4))) static int fail(char *err, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && size)
        vsnprintf(err, size, fmt, ap);
    va_end(ap);
    return 0;
}

static int days_in(int y, int m)
{
    static const int D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return D[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
}

static long civil(int y, int m, int d)
{
    y -= m <= 2;
    long     era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static int value(const char **p, int lo, int hi, const char *const *names, int *out)
{
    if (names)
        for (int i = 0; i <= hi - lo; i++)
            if (!strncasecmp(*p, names[i], 3) && !isalpha((unsigned char)(*p)[3])) {
                *out = lo + i;
                *p += 3;
                return 1;
            }
    if (!isdigit((unsigned char)**p))
        return 0;
    char *end;
    long  v = strtol(*p, &end, 10);
    if (v < lo || v > hi)
        return 0;
    *out = (int)v;
    *p = end;
    return 1;
}

static int span(const char **p, int lo, int hi, const char *const *names, unsigned char *set, int base)
{
    int a, b, step = 1;
    if (**p == '*') {
        a = lo;
        b = hi;
        (*p)++;
    } else {
        if (!value(p, lo, hi, names, &a))
            return 0;
        b = a;
        if (**p == '-') {
            (*p)++;
            if (!value(p, lo, hi, names, &b) || b < a)
                return 0;
        } else if (**p == '/')
            b = hi;
    }
    if (**p == '/') {
        char *end;
        long  st = strtol(*p + 1, &end, 10);
        if (end == *p + 1 || st < 1)
            return 0;
        step = (int)st;
        *p = end;
    }
    for (int v = a; v <= b; v += step)
        set[v - base] = 1;
    return 1;
}

static int field(const char *s, int lo, int hi, const char *const *names, unsigned char *set, int base, int *last)
{
    const char *p = s;
    for (;;) {
        if (last && *p == 'L' && (p[1] == ',' || !p[1])) {
            *last = 1;
            p++;
        } else if (!span(&p, lo, hi, names, set, base))
            return 0;
        if (*p != ',')
            return !*p;
        p++;
    }
}

static int dow_field(const char *s, struct schedule *sc)
{
    const char *p = s;
    for (;;) {
        const char *q = p;
        int         d, one = value(&q, 1, 7, DAYS, &d);
        if (one && *q == '#') {
            char *end;
            long  k = strtol(q + 1, &end, 10);
            if (end == q + 1 || k < 1 || k > 5)
                return 0;
            sc->dow_nth[k] |= (unsigned char)(1 << d);
            p = end;
        } else if (one && *q == 'L') {
            sc->dow_last |= (unsigned char)(1 << d);
            p = q + 1;
        } else if (!span(&p, 1, 7, DAYS, sc->dow, 0))
            return 0;
        if (*p != ',')
            return !*p;
        p++;
    }
}

static int clock_min(const char **p, int *out)
{
    char *end;
    long  h = strtol(*p, &end, 10);
    if (end == *p || *end != ':')
        return 0;
    const char *m = end + 1;
    long        mm = strtol(m, &end, 10);
    if (end - m != 2 || mm > 59 || h < 0 || h > 24 || (h == 24 && mm))
        return 0;
    *out = (int)(h * 60 + mm);
    *p = end;
    return 1;
}

static int random_token(const char *s, struct schedule *sc)
{
    const char *p = s + 1;
    char       *end;
    sc->random = 1;
    sc->count = 1;
    if (isdigit((unsigned char)*p)) {
        long n = strtol(p, &end, 10);
        if (n < 1 || n > SCHEDULE_RANDOM_MAX)
            return 0;
        sc->count = (int)n;
        p = end;
    }
    if (*p == '/') {
        long g = strtol(p + 1, &end, 10);
        if (end == p + 1 || g < 0)
            return 0;
        p = end;
        if (*p == 'h') {
            g *= 60;
            p++;
        } else if (*p == 'm')
            p++;
        sc->gap = (int)g;
    }
    if (*p++ != '[' || !clock_min(&p, &sc->from) || *p++ != '-' || !clock_min(&p, &sc->to) ||
        *p++ != ']' || *p)
        return 0;
    return sc->to > sc->from && (long)(sc->count - 1) * sc->gap < sc->to - sc->from;
}

static int week_token(const char *s, struct schedule *sc)
{
    char *end;
    long  n = strtol(s + 1, &end, 10);
    int   y, m, d, used = 0;
    if (end == s + 1 || n < 1 || n > 520 || end[0] != 'w' || end[1] != '@' ||
        sscanf(end + 2, "%4d-%2d-%2d%n", &y, &m, &d, &used) != 3 || end[2 + used] || m < 1 ||
        m > 12 || d < 1 || d > days_in(y, m))
        return 0;
    sc->weeks = (int)n;
    sc->anchor = civil(y, m, d);
    return 1;
}

int schedule_parse(const char *spec, struct schedule *s, char *err, size_t size)
{
    memset(s, 0, sizeof *s);
    char buf[512];
    if (strlen(spec) >= sizeof buf)
        return fail(err, size, "schedule too long");
    snprintf(buf, sizeof buf, "%s", spec);
    char *tok[FIELDS_MAX];
    int   n = 0;
    for (char *t = strtok(buf, " \t"); t; t = strtok(NULL, " \t")) {
        if (n == FIELDS_MAX)
            return fail(err, size, "too many fields");
        tok[n++] = t;
    }
    if (n && tok[n - 1][0] == '~') {
        if (!week_token(tok[n - 1], s))
            return fail(err, size, "bad week interval '%s': expected ~Nw@YYYY-MM-DD", tok[n - 1]);
        n--;
    }
    int r = n >= 2 && tok[1][0] == 'R';
    int want = r ? 5 : 6;
    if (n != want && n != want + 1)
        return fail(err, size, "expected %d or %d fields, got %d", want, want + 1, n);
    if (!field(tok[0], 0, 59, NULL, s->sec, 0, NULL))
        return fail(err, size, "bad seconds '%s'", tok[0]);
    int i = 1;
    if (r) {
        if (!random_token(tok[1], s))
            return fail(err, size,
                        "bad random window '%s': expected R[count][/gap][HH:MM-HH:MM], window longer "
                        "than (count-1)*gap",
                        tok[1]);
        i = 2;
    } else {
        if (!field(tok[1], 0, 59, NULL, s->min, 0, NULL))
            return fail(err, size, "bad minutes '%s'", tok[1]);
        if (!field(tok[2], 0, 23, NULL, s->hour, 0, NULL))
            return fail(err, size, "bad hours '%s'", tok[2]);
        i = 3;
    }
    if (!strcmp(tok[i], "?"))
        s->dom_skip = 1;
    else if (!field(tok[i], 1, 31, NULL, s->dom, 0, &s->dom_last))
        return fail(err, size, "bad day-of-month '%s'", tok[i]);
    if (!field(tok[i + 1], 1, 12, MONTHS, s->mon, 0, NULL))
        return fail(err, size, "bad month '%s'", tok[i + 1]);
    if (!strcmp(tok[i + 2], "?"))
        s->dow_skip = 1;
    else if (!dow_field(tok[i + 2], s))
        return fail(err, size, "bad day-of-week '%s'", tok[i + 2]);
    if (s->dom_skip == s->dow_skip)
        return fail(err, size, "exactly one of day-of-month and day-of-week must be ?");
    if (n > i + 3) {
        if (!field(tok[i + 3], SCHEDULE_YEAR_MIN, SCHEDULE_YEAR_MIN + SCHEDULE_YEARS - 1, NULL,
                   s->year, SCHEDULE_YEAR_MIN, NULL))
            return fail(err, size, "bad year '%s'", tok[i + 3]);
    } else
        memset(s->year, 1, sizeof s->year);
    return 1;
}

static int day_match(const struct schedule *s, const struct tm *d)
{
    int y = d->tm_year + 1900, m = d->tm_mon + 1, md = d->tm_mday, wd = d->tm_wday + 1;
    int dim = days_in(y, m);
    if (!s->mon[m] || y < SCHEDULE_YEAR_MIN || y >= SCHEDULE_YEAR_MIN + SCHEDULE_YEARS ||
        !s->year[y - SCHEDULE_YEAR_MIN])
        return 0;
    if (!s->dom_skip && !s->dom[md] && !(s->dom_last && md == dim))
        return 0;
    if (!s->dow_skip && !s->dow[wd] && !(s->dow_nth[(md - 1) / 7 + 1] & (1 << wd)) &&
        !((s->dow_last & (1 << wd)) && md + 7 > dim))
        return 0;
    if (s->weeks) {
        long k = civil(y, m, md) - s->anchor;
        if (k < 0 || (k / 7) % s->weeks)
            return 0;
    }
    return 1;
}

static uint64_t mix(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int by_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int random_minutes(const struct schedule *s, const char *seed, long day, int *out)
{
    uint64_t x = 1469598103934665603ULL;
    for (const char *c = seed; *c; c++)
        x = (x ^ (unsigned char)*c) * 1099511628211ULL;
    x ^= (uint64_t)day;
    int room = s->to - s->from - (s->count - 1) * s->gap;
    for (int i = 0; i < s->count; i++)
        out[i] = (int)(mix(&x) % (uint64_t)room);
    qsort(out, (size_t)s->count, sizeof *out, by_int);
    for (int i = 0; i < s->count; i++)
        out[i] += s->from + i * s->gap;
    return s->count;
}

static time_t at(const struct tm *day, int h, int m, int sec)
{
    struct tm t = {.tm_year = day->tm_year, .tm_mon = day->tm_mon, .tm_mday = day->tm_mday,
                   .tm_hour = h, .tm_min = m, .tm_sec = sec, .tm_isdst = -1};
    return mktime(&t);
}

time_t schedule_next(const struct schedule *s, const char *seed, time_t after)
{
    struct tm a;
    localtime_r(&after, &a);
    for (int i = 0; i < HORIZON_DAYS; i++) {
        struct tm day = {.tm_year = a.tm_year, .tm_mon = a.tm_mon, .tm_mday = a.tm_mday + i,
                         .tm_hour = 12, .tm_isdst = -1};
        if (mktime(&day) == -1 || !day_match(s, &day))
            continue;
        if (s->random) {
            int m[SCHEDULE_RANDOM_MAX], sec = 0;
            while (!s->sec[sec])
                sec++;
            int n = random_minutes(s, seed, civil(day.tm_year + 1900, day.tm_mon + 1, day.tm_mday), m);
            for (int k = 0; k < n; k++) {
                time_t t = at(&day, m[k] / 60, m[k] % 60, sec);
                if (t > after)
                    return t;
            }
            continue;
        }
        for (int h = 0; h < 24; h++) {
            if (!s->hour[h] || (i == 0 && h < a.tm_hour))
                continue;
            for (int mi = 0; mi < 60; mi++) {
                if (!s->min[mi] || (i == 0 && h == a.tm_hour && mi < a.tm_min))
                    continue;
                for (int se = 0; se < 60; se++) {
                    if (!s->sec[se])
                        continue;
                    time_t t = at(&day, h, mi, se);
                    if (t > after)
                        return t;
                }
            }
        }
    }
    return 0;
}
