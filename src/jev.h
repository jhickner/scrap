#ifndef JEV_H
#define JEV_H

#define JEV_TEXT_MAX 4096

struct jev_result {
    double ready;
    double cancel;
    double hold;
    double release;
    long   rtt_ms;
    long   in_tok;
    long   out_tok;
    int    error;
    char   text[JEV_TEXT_MAX];
};

int         jev_set_backend(const char *name);
const char *jev_backend(void);

const char *jev_key(void);

int  jev_ask(const char *transcript, const char *recent_turns, long ms_since_change);
int  jev_busy(void);

int  jev_pump(struct jev_result *out);
int  jev_fds(int *out, int max);

void jev_reset(void);

char *jev_body(const char *transcript, const char *recent_turns,
               const char *previous_partials, long ms_since_change);
int   jev_parse(const char *json, struct jev_result *out);

const char *jev_strip_prefix(const char *submitted, const char *text);

#endif
