
#ifndef BOARDVIEW_H
#define BOARDVIEW_H

struct session;

int boardview_run(const char *cwd);

int boardview_capture(const char *text, const char *cwd, char *id_out, int size);

#endif
