
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

/* claude only: load ~/.config/mux/plugins/shunt, which blocks whole-file reads
   over SHUNT_MIN_LINES and routes them to a cheap worker model instead */
#define SETTING_SHUNT        "shunt"

/* hands-free voice: on/off, whether replies are spoken, output volume,
   the helper app, the voice name, the speaking rate as a percent of normal
   speed, the end-of-turn silence in seconds, and the input device name to
   prefer */
#define SETTING_VOICE          "voice"
#define SETTING_VOICE_SPEAK    "voice_speak"
#define SETTING_VOICE_VOLUME   "voice_volume"
#define SETTING_VOICE_HELPER   "voice_helper"
#define SETTING_VOICE_NAME     "voice_name"
#define SETTING_VOICE_RATE     "voice_rate"
#define SETTING_VOICE_SILENCE  "voice_silence"
#define SETTING_VOICE_TRACE    "voice_trace" /* opt-in voice-events.log */
#define SETTING_VOICE_INPUT    "voice_input"

#define VOICE_VOLUME_DEFAULT 100
#define VOICE_RATE_DEFAULT   100
/* seconds of quiet that end a spoken turn */
#define VOICE_SILENCE_DEFAULT 2.0
#define VOICE_SILENCE_MIN     0.4
#define VOICE_SILENCE_MAX     10.0

#define VOICE_RATE_MIN        20
#define VOICE_RATE_MAX       200
/* say the session name when a turn ends while this pane is unfocused */
#define SETTING_VOICE_COMPLETE "voice_complete"

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
