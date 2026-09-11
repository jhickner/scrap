# Intake: plan → tasks

For "here's a plan for X" or a spoken braindump.

1. Resolve the project; read the plan (file, URL, or the transcript).
2. Decompose into tasks that are independently dispatchable: one clear
   deliverable each, small enough that a worker finishes in one session,
   with deps for real ordering constraints only.
3. Classify each: design/schema/architecture/review → `planning`; bug
   hunting → `diagnosis`; everything else → `impl`. Mark natural user-review
   points as checkpoint tasks.
4. If the plan is large or ambiguous, run the decomposition on a
   planning-class model (dispatch it like any task, with the plan text in
   the prompt, asking for task records back as JSON) instead of doing it
   inline.
5. Append the records, then read back a compact summary: counts by class,
   the first thing that would dispatch, and any checkpoint. Ask nothing
   unless the plan genuinely forks.
