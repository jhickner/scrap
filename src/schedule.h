#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stddef.h>
#include <time.h>

#define SCHEDULE_YEAR_MIN 2000
#define SCHEDULE_YEARS    200
#define SCHEDULE_RANDOM_MAX 100

struct schedule {
    unsigned char sec[60], min[60], hour[24], dom[32], mon[13], dow[8];
    unsigned char year[SCHEDULE_YEARS];
    unsigned char dow_nth[6], dow_last;
    int           dom_skip, dom_last, dow_skip;
    int           random, count, gap, from, to;
    int           weeks;
    long          anchor;
};

int schedule_parse(const char *spec, struct schedule *s, char *err, size_t size);

time_t schedule_next(const struct schedule *s, const char *seed, time_t after);

#endif
