#ifndef REPLKEYS_H
#define REPLKEYS_H

#include "tty.h"
#include "vendor/repl.h"

int replkeys_map(const tty_event *ev, ReplEvent *out);

int replkeys_paste(Repl *r);

#endif
