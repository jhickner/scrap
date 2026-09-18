#ifndef ORCH_H
#define ORCH_H

struct session;

/* The orchestrator: the runtime half of delegated work. It watches workers,
   owns ~/.config/orchestrator, and speaks to one session -- the orchestrator
   session -- which does the judgement. Started like relay and telegram, by
   --orchestrator or /orchestrator, and limited to one instance by the same
   owner lock. The instance that claims it keeps it: when that instance exits
   the orchestrator stops, and no other instance takes over. */

int  orch_start(struct session *s);
void orch_stop(void);
const char *orch_start_error(void);

/* non-NULL while this instance owns the orchestrator */
const char *orch_label(void);

struct session *orch_session(void);
void orch_forget_session(struct session *s);

/* from the idle loop: serve requests, deliver completions, reconcile on time */
void orch_poll(void);

/* Reconcile now: fold in results that landed, notice workers that died without
   one, dispatch what became ready. Returns the number of tasks it changed. */
int orch_reconcile(void);

#endif
