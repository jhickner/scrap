#ifndef QUOTA_H
#define QUOTA_H

#include "vendor/agents/backend.h"

#define QUOTA_EVERY 300

const char *quota_provider(const char *backend, const char *model);
int         quota_fresh(const char *provider);
int         quota_read(const char *provider, backend_rate_limit *out);
void        quota_note(const char *provider, const backend_rate_limit *r);
void        quota_want(const char *provider);
void        quota_tick(void);

#endif
