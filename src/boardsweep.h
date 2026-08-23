
#ifndef BOARDSWEEP_H
#define BOARDSWEEP_H

int boardsweep_landed(const char *cwd);

int boardsweep_running(const char *cwd);

int boardsweep_start(const char *cwd);

int boardsweep_pump(void);

int boardsweep_take(const char *key, const char *reply);

#endif
