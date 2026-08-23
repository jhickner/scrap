#ifndef BOARDLOG_H
#define BOARDLOG_H

#include <stddef.h>

// The whole life of one card, kept apart from the card itself: every stage it
// went through, what that stage was asked, and what it answered.
//
// The card's own log says what changed -- moved here, said that. This says
// why, which is the half you need when a card came out wrong and the stage
// that did it has long since exited. Markdown, because the reader is a person
// with a question.

// The transcript file for a card, whether or not it exists yet.
int boardlog_path(const char *id, char *out, size_t size);

// One exchange with a stage. `prompt` or `reply` may be NULL, so a stage can
// write down what it asked before it knows the answer.
void boardlog_turn(const char *id, const char *stage, const char *prompt,
                   const char *reply);

// Something happened that was not an exchange: a move, a decision, a person
// saying yes. Keeps the timeline readable between the turns.
void boardlog_note(const char *id, const char *who, const char *text);

// The card is gone, and so is the record of it.
void boardlog_remove(const char *id);

#endif
