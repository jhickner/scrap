
#ifndef SETTINGS_H
#define SETTINGS_H

#define SETTING_THINKING   "thinking"
#define SETTING_PERMISSION "permission"
#define SETTING_COMPACT    "compact"
#define SETTING_STICKY     "sticky"
#define SETTING_IMAGE_ROWS "image_rows"
#define SETTING_ECHO_ROWS  "echo_rows"

#define ECHO_ROWS_DEFAULT 10

#define SETTING_TASK_STALL "task_stall_seconds"

#define TASK_STALL_DEFAULT 45

#define SETTING_STATUS_INTERVAL "status_seconds"

#define SETTING_JUMP_LENGTH "jump_length"

#define JUMP_LENGTH_DEFAULT 10
#define JUMP_LENGTH_MAX     100

#define STATUS_INTERVAL_DEFAULT 300

#define SETTING_BACKEND      "backend"

#define SETTING_FOLDER_SORT  "folder_sort"

#define SETTING_SESSIONS_REMOTE "sessions_remote"

#define SETTING_CHROME       "chrome"

#define SETTING_SHUNT        "shunt"

#define SETTING_VOICE          "voice"
#define SETTING_VOICE_SPEAK    "voice_speak"

#define SETTING_VOICE_MODE     "voice_mode"
#define SETTING_VOICE_WAKE     "voice_wake"
#define SETTING_VOICE_VOLUME   "voice_volume"
#define SETTING_VOICE_HELPER   "voice_helper"
#define SETTING_VOICE_NAME     "voice_name"
#define SETTING_VOICE_RATE     "voice_rate"
#define SETTING_VOICE_SILENCE  "voice_silence"
#define SETTING_VOICE_TRACE    "voice_trace"
#define SETTING_VOICE_INPUT    "voice_input"

#define VOICE_VOLUME_DEFAULT 100
#define VOICE_RATE_DEFAULT   100

#define VOICE_SILENCE_DEFAULT 2.0
#define VOICE_SILENCE_MIN     0.4
#define VOICE_SILENCE_MAX     10.0

#define VOICE_RATE_MIN        20
#define VOICE_RATE_MAX       200

#define SETTING_VOICE_JEV_BACKEND   "voice_jev_backend"
#define SETTING_VOICE_JEV_THRESHOLD "voice_jev_threshold"
#define SETTING_VOICE_JEV_HOLD      "voice_jev_hold_threshold"
#define SETTING_VOICE_JEV_DELAY     "voice_jev_delay"
#define SETTING_VOICE_JEV_SILENCE   "voice_jev_silence"

#define VOICE_JEV_BACKEND_DEFAULT   "typesafe"
#define VOICE_JEV_THRESHOLD_DEFAULT 0.8
#define VOICE_JEV_HOLD_DEFAULT      0.5
#define VOICE_JEV_CANCEL            0.8
#define VOICE_JEV_DELAY_DEFAULT     100
#define VOICE_JEV_DELAY_MAX         2000
#define VOICE_JEV_SILENCE_DEFAULT   400
#define VOICE_JEV_SILENCE_MAX       3000

#define SETTING_VOICE_COMPLETE "voice_complete"

#define SETTING_THEME "theme"

#define SETTING_NAME_BADGE "name_badge"

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
