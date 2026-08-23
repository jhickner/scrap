
#ifndef BOARDSWEEP_H
#define BOARDSWEEP_H

// Looking for what incremental work leaves behind. Cards land one at a time,
// each sensible on its own, and the duplication only shows up across them: two
// mechanisms doing one job, a thing done three ways.
//
// It counts cards rather than minutes. mux has no daemon and the board only
// exists while it is open, so a timer would come due when nothing was
// watching; a counter comes due exactly when the board has been used.
//
// What it finds becomes ordinary cards in `new`, which triage then sorts like
// anything else. The board feeds itself.

// A card has landed in this repo. Returns nonzero when that was the one that
// brings a sweep due.
int boardsweep_landed(const char *cwd);

int boardsweep_running(const char *cwd);

// Starts a sweep of `cwd` if one is due and none is going. Nonzero once it is.
int boardsweep_start(const char *cwd);

// Runs a sweep for any repo that has one owing.
int boardsweep_pump(void);

// Offered every child that has finished. Nonzero when it was ours.
int boardsweep_take(const char *key, const char *reply);

#endif
