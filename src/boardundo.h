#ifndef BOARDUNDO_H
#define BOARDUNDO_H

#include <stddef.h>

struct board_card;

int boardundo_can(const struct board_card *c);

int boardundo_run(const struct board_card *c, char *said, size_t size);

#endif
