#ifndef ORCHCLI_H
#define ORCHCLI_H

/* `mux orch <verb>`: the orchestrator's state from any terminal, in or out of a
   mux session. Reads and plain state changes run here, against the same files
   and the same locks the subsystem uses, so they work with no mux running at
   all. The verbs that need a live mux -- dispatching a worker, typing at one,
   reconciling now -- go to the instance that owns the orchestrator over a
   request file, and say so plainly when there is none.

   argv[0] is "orch". Returns the process exit status. */
int orchcli_main(int argc, char **argv);

#endif
