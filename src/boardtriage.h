
#ifndef BOARDTRIAGE_H
#define BOARDTRIAGE_H

struct board_card;

// Working out what a card meant. One short turn per card, run as a child so
// the board stays live while it happens: the rows spin, and the answer lands
// on the card rather than on the screen.
//
// Triage may fail, and saying so is a result rather than an error -- a card it
// cannot place goes to `unclear` carrying the question it would have asked.

// Starts triage for a card if a slot is free and one is not already running
// for it. Nonzero once the child is going.
int boardtriage_start(const struct board_card *c);

int boardtriage_running(const char *id);

// Offered every child that has finished. Nonzero when it was triage's, and the
// card has been written; zero leaves it for whoever else was waiting on it.
int boardtriage_take(const char *key, const char *reply);

// How many times triage has had a go at this card since the last time a
// person said anything about it. Two failed passes is enough: a card that will
// not parse waits for a person rather than for another turn -- and once that
// person has answered, the count starts again.
int boardtriage_attempts(const struct board_card *c);

#define BOARDTRIAGE_TRIES 2

#endif
