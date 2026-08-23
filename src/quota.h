
#ifndef QUOTA_H
#define QUOTA_H

void quota_note(const char *backend, int percent, long resets_at,
                long window_minutes);

int quota_get(const char *backend, int *percent, long *resets_at);

int quota_resets_in(const char *backend);

#endif
