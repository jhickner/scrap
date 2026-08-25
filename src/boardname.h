#ifndef BOARDNAME_H
#define BOARDNAME_H

struct board_card;

/* Naming a card is a headless turn on the low tier, fired when the card is
   captured. It cannot fail in a way you need to answer: a card carries its own
   first line as a title until a better one comes back, so nothing waits on it
   and nothing lands anywhere for you to sort out. */

int boardname_start(const struct board_card *c);

int boardname_running(const char *id);

int boardname_take(const char *key, const char *reply);

#endif
