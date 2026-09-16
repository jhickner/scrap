#ifndef VNCSOURCE_H
#define VNCSOURCE_H

#include "vncinset.h"

/* Frame source for a Grok Bot agent desktop: reads the box status and connects
   off the UI thread, then pumps the connection on each frame call. Never starts
   a stopped desktop. A vncinset_open_fn. */
struct vncinset_source *vncsource_open(const char *bot);

#endif
