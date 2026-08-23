
#ifndef BOARDWORK_H
#define BOARDWORK_H

struct board_card;
struct session;

enum board_role {
    BOARD_ROLE_WORKER,
    BOARD_ROLE_AUDIT,
    BOARD_ROLE_SWEEP,
};

void boardwork_begin(void);

void boardwork_finished(struct session *s);

int boardwork_start(const struct board_card *c, char *why, int size);

int boardwork_blocked(const struct board_card *c, char *why, int size);

int boardwork_running(void);

/* Move every worker whose role follows the serving backend onto it now;
 * one mid-turn waits for that turn. *waiting counts those. */
int boardwork_serve(int *waiting);

const char *boardwork_card_of(const struct session *s);

int boardwork_tab(const char *id);

int boardwork_auditing(const char *id);

int boardwork_sweeping(const char *id);

int boardwork_audit_pump(void);

int boardwork_sweep_pump(void);

int boardwork_hold(const char *id, struct session *s, enum board_role role);

void boardwork_let_go(const char *id);

int boardwork_poll(void);

int boardwork_release(const struct board_card *c);

void boardwork_discard(const struct board_card *c);

int boardwork_pump(void);

int boardwork_approve(const struct board_card *c, int audit);

int boardwork_reject(const struct board_card *c, const char *why);

int boardwork_feedback(const struct board_card *c, const char *text);

void boardwork_close_all(void);

#endif
