#ifndef ASKFORM_H
#define ASKFORM_H

#include "askblock.h"

enum askform_exit { ASKFORM_DONE, ASKFORM_PREV_TAB, ASKFORM_NEXT_TAB, ASKFORM_NEW_TAB };

char *askform_run(const struct askblock *b, enum askform_exit *how);

#endif
