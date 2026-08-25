
#ifndef BOARDVIEW_H
#define BOARDVIEW_H

struct session;

#define BOARDVIEW_SESSIONS (-2)
#define BOARDVIEW_NONE     (-3)

int boardview_run(const char *cwd);

int boardview_capture(const char *text, const char *cwd, char *id_out, int size);

int boardview_close(const char *id, char *why, int size);

/* queue "<action>[, <action>]" on a card; no spec says what it will take */
int boardview_trigger(const char *id, const char *spec, char *why, int size);


#endif
