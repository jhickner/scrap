
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTING_THINKING   "thinking"
#define SETTING_PERMISSION "permission"
#define SETTING_COMPACT    "compact"
#define SETTING_STICKY     "sticky"
#define SETTING_IMAGE_ROWS "image_rows"
#define SETTING_ECHO_ROWS  "echo_rows"

#define ECHO_ROWS_DEFAULT 10

/* Seconds to wait, after the last background task of a session ends without
   waking the agent, before the session is nudged on. 0 turns the nudge off. */
#define SETTING_TASK_STALL "task_stall_seconds"

#define TASK_STALL_DEFAULT 45

#define SETTING_BACKEND      "backend"

/* claude only: pass --chrome, so the agent gets the Claude in Chrome tools */
#define SETTING_CHROME       "chrome"

#define SETTING_COLOR_INPUT    "color_input"
#define SETTING_COLOR_EMPHASIS "color_emphasis"

#define MAX_SETTING_KEY    64
#define MAX_SETTING_VALUE  256
#define MAX_SETTINGS       512

struct settings {
    char path[4096];
    int  count;
    struct {
        char key[MAX_SETTING_KEY];
        char value[MAX_SETTING_VALUE];
    } entries[MAX_SETTINGS];
};

void settings_load(struct settings *s, const char *path);
const char *settings_get(const struct settings *s, const char *key,
                         const char *fallback);
void settings_put(struct settings *s, const char *key, const char *value);

void settings_open(const char *path);

int  settings_get_int(const char *key, int fallback);

const char *settings_get_str(const char *key, const char *fallback);

void settings_set_int(const char *key, int value);
void settings_set_str(const char *key, const char *value);

#endif
