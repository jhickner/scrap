#ifndef AGENTSYNC_H
#define AGENTSYNC_H

#include <stdio.h>

int agentsync_run(const char *home, int prune, FILE *log);

int agentsync_main(int argc, char **argv);

#endif
