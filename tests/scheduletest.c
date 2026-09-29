#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "schedule.h"

static time_t utc(int y, int mo, int d, int h, int mi)
{
    struct tm t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi};
    return timegm(&t);
}

static time_t next(const char *spec, time_t after)
{
    struct schedule s;
    char            err[256];
    assert(schedule_parse(spec, &s, err, sizeof err));
    return schedule_next(&s, "seed", after);
}

static int bad(const char *spec)
{
    struct schedule s;
    char            err[256] = "";
    return !schedule_parse(spec, &s, err, sizeof err) && err[0];
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    tzset();

    const char *biweekly = "0 0 15 ? * TUE ~2w@2026-09-29";
    assert(next(biweekly, utc(2026, 9, 28, 0, 0)) == utc(2026, 9, 29, 15, 0));
    assert(next(biweekly, utc(2026, 9, 29, 15, 0)) == utc(2026, 10, 13, 15, 0));

    assert(next("0 0 9 ? * SUN#1", utc(2026, 10, 1, 0, 0)) == utc(2026, 10, 4, 9, 0));
    assert(next("0 0 9 ? * SUN#1", utc(2026, 10, 4, 9, 0)) == utc(2026, 11, 1, 9, 0));
    assert(next("0 0 9 ? * 6L", utc(2026, 10, 1, 0, 0)) == utc(2026, 10, 30, 9, 0));
    assert(next("0 0 0 L * ?", utc(2028, 2, 1, 0, 0)) == utc(2028, 2, 29, 0, 0));
    assert(next("0 30 8-10/2 ? * MON-FRI", utc(2026, 10, 3, 0, 0)) == utc(2026, 10, 5, 8, 30));
    assert(next("0 0 12 1 JAN ? 2027", utc(2026, 10, 1, 0, 0)) == utc(2027, 1, 1, 12, 0));
    assert(next("0 0 12 1 JAN ? 2025", utc(2026, 10, 1, 0, 0)) == 0);

    time_t day = utc(2026, 10, 1, 0, 0), t = next("0 R[08:00-21:00] * * ?", day);
    assert(t >= utc(2026, 10, 1, 8, 0) && t < utc(2026, 10, 1, 21, 0));
    assert(next("0 R[08:00-21:00] * * ?", day) == t);
    assert(next("0 R[08:00-21:00] * * ?", t) >= utc(2026, 10, 2, 8, 0));

    time_t prev = 0, at = day;
    for (int i = 0; i < 5; i++) {
        at = next("0 R5/30m[08:00-21:00] * * ?", at);
        assert(at >= utc(2026, 10, 1, 8, 0) && at < utc(2026, 10, 1, 21, 0));
        assert(!prev || at - prev >= 30 * 60);
        prev = at;
    }
    assert(next("0 R5/30m[08:00-21:00] * * ?", at) >= utc(2026, 10, 2, 8, 0));

    assert(bad("0 0 15 * * TUE"));
    assert(bad("0 0 15 ? * ?"));
    assert(bad("0 R5/4h[08:00-21:00] * * ?"));
    assert(bad("0 R[09:00-08:00] * * ?"));
    assert(bad("0 0 15 ? * TUE ~2w@2026-02-30"));
    assert(bad("0 0 25 ? * TUE"));
    assert(bad("0 0 15 ? *"));
    return 0;
}
