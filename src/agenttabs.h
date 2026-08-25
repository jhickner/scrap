#ifndef AGENTTABS_H
#define AGENTTABS_H

/* Every session in this process gets its own record, keyed by an opaque
 * pointer, so a background codex tab is tracked alongside the foreground
 * claude one instead of overwriting it. */

void agenttabs_begin(void);

void agenttabs_publish(const void *key, const char *backend, const char *status);

void agenttabs_usage(const void *key, int percent, long resets_at, long window_minutes);

void agenttabs_forget(const void *key);

void agenttabs_forget_hook(const char *id);

#endif
