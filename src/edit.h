
#ifndef EDIT_H
#define EDIT_H

// $EDITOR on a scrap of text. For the things a one-line field cannot hold: a
// prompt of several paragraphs, a spec, anything with shape.
//
// Returns what came back, malloc'd, or NULL where the editor failed or was
// left without saving. `suffix` names the temp file's extension, so the editor
// highlights it sensibly; NULL for none.
char *edit_run(const char *initial, const char *suffix);

// $EDITOR on a file that is already there, for reading rather than writing.
// Zero when there is no such file.
int edit_open(const char *path);

#endif
