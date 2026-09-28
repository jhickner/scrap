#ifndef NEWSESSION_H
#define NEWSESSION_H

int newsession_spawn(const char *backend, const char *model, const char *effort,
                     const char *cwd, const char *name);

int newsession_run(void);

#endif
