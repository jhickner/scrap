#ifndef MATRIX_H
#define MATRIX_H

struct session;

int matrix_run(struct session *s, const char *prompt);

/* the rows of the last run, reopened against their saved ids; 0 if there is
   nothing to reopen */
int matrix_reopen(struct session *s);

#endif
