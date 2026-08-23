
#ifndef BOARDAUDIT_H
#define BOARDAUDIT_H

struct board_card;

// A second pass over the diff before it lands, for the things a person
// reviewing their own agent's work is worst at noticing: a mechanism
// duplicated, structure fighting the code around it, memory, security.
//
// A gate a card may skip rather than a stage every card walks. A three-line
// change does not want an audit, and making every card wait for one is how a
// gate stops being kept.

// How big the change is: files touched and lines moved, against the commit the
// worktree branched from. Zero when that cannot be worked out.
int boardaudit_size(const struct board_card *c, int *files, int *lines);

// Whether that size is over what the configuration asks for.
int boardaudit_wanted(const struct board_card *c);

int boardaudit_start(const struct board_card *c);
int boardaudit_running(const char *id);

// Runs the next card sitting in the audit column, if none is already going.
int boardaudit_pump(void);

// Offered every child that has finished. Nonzero when it was ours.
int boardaudit_take(const char *key, const char *reply);

#endif
