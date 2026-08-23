
#ifndef BOARDVIEW_H
#define BOARDVIEW_H

struct session;

// The board, and the detail screen behind it. Columns are group headings and
// cards are rows: mux draws on no alternate screen, and a board read top to
// bottom survives a narrow terminal in a way a column grid does not.
//
// `cwd` is what the list is filtered to when it opens, which is the directory
// the session works in; '*' widens it to every repo. NULL opens it wide.
void boardview_run(const char *cwd);

// Capture. The first line becomes the title, the whole of it the body, and
// nothing is classified: that is triage's job, later and elsewhere.
// Returns nonzero, with the new card's id in `id_out` when one is given.
int boardview_capture(const char *text, const char *cwd, char *id_out, int size);

#endif
