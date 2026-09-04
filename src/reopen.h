#ifndef REOPEN_H
#define REOPEN_H

/* Brings the sessions of a window that is gone back as tabs of this one.
   Returns how many were queued, 0 when there was nothing to pick or the pick
   was cancelled. */
int reopen_run(void);

int reopen_available(void);

#endif
