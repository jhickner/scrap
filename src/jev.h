#ifndef JEV_H
#define JEV_H

/* Jev endpointing: one request asks whether the spoken turn so far is
   complete, whether it is being cancelled, and whether the speaker asked to
   keep listening. The request runs on the curl multi handle, so the poll loop
   is never blocked by it. */

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

/* experiential or typesafe; 0 when the name is neither */
int         jev_set_backend(const char *name);
const char *jev_backend(void);
/* the key for the current backend, "" when there is none */
const char *jev_key(void);

/* start a request about transcript; 0 when one is already running, the key is
   missing, or there is nothing to ask about */
int  jev_ask(const char *transcript, const char *recent_turns, long ms_since_change);
int  jev_busy(void);
/* 1 when an answer landed, copied to out */
int  jev_pump(struct jev_result *out);
int  jev_fds(int *out, int max);
const struct jev_result *jev_last(void);
/* drop the request in flight and the partials remembered for the next body */
void jev_reset(void);

char *jev_body(const char *transcript, const char *recent_turns,
               const char *previous_partials, long ms_since_change);
int   jev_parse(const char *json, struct jev_result *out);
/* the text past the words of submitted, when both start with the same words;
   text itself when they do not */
const char *jev_strip_prefix(const char *submitted, const char *text);

#endif
