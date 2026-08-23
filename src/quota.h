
#ifndef QUOTA_H
#define QUOTA_H

// What is left of each backend's subscription, as its own protocol last
// reported it. Kept per backend and shared between every mux on the machine,
// so a window can ask about a backend it has no session on -- which is what
// the board needs before it decides who should take a card.
//
// Only some backends report this at all. A backend nothing is known about is
// not a backend that is exhausted, and the two must not be confused: the
// caller is told which it is.

// A session's backend has just said where it is. Kept, and published.
void quota_note(const char *backend, int percent, long resets_at,
                long window_minutes);

// What is known: nonzero when this backend reports at all and the reading is
// still worth having. `percent` is how much of the window is spent and
// `resets_at` when it starts again, either of which may be passed as NULL.
//
// A reading from before the window turned over reads as zero rather than as
// what it said: the window it described has been and gone.
int quota_get(const char *backend, int *percent, long *resets_at);

// Minutes until this backend's window turns over, or -1 where that is not
// known. Zero once it is due.
int quota_resets_in(const char *backend);

#endif
