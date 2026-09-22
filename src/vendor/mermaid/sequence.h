#ifndef SEQUENCE_H
#define SEQUENCE_H

#include "internal.h"

typedef enum { SEQ_MESSAGE, SEQ_NOTE, SEQ_DIVIDER } SeqItemKind;
typedef enum { SEQHEAD_ARROW, SEQHEAD_CROSS } SeqHead;
typedef enum { ANCHOR_OVER, ANCHOR_LEFT, ANCHOR_RIGHT } NoteAnchorKind;

typedef struct {
    SeqItemKind kind;
    int from, to;
    char *text;
    bool dashed;
    SeqHead head;
    NoteAnchorKind anchor_kind;
    int anchor_a, anchor_b;
} SeqItem;

struct Sequence {
    char **ids;
    char **labels;
    size_t n_labels, cap_labels;
    SeqItem *items;
    size_t n_items, cap_items;
};

int sequence_participant(Sequence *s, const char *id, const char *label);

#endif
