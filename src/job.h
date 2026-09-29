#ifndef JOB_H
#define JOB_H

#include <time.h>

void job_tick(const char *exe, time_t now);

int job_main(int argc, char **argv);

#endif
