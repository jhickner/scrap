#ifndef AGENTTABS_H
#define AGENTTABS_H

void agenttabs_begin(void);

void agenttabs_publish(const void *key, const char *backend, const char *status,
                       const char *provider);

void agenttabs_usage(const void *key, int percent, long resets_at, long window_minutes);

void agenttabs_forget(const void *key);

void agenttabs_forget_hook(const char *id);

#endif
