#include "boardfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "text.h"

#define CARD_DIR  "board-cards"
#define CARD_FILE "CARD.md"
#define CARD_MAX  (1u << 20)

int boardfile_kept(const char *id, char *out, size_t size)
{
    return board_md_path(CARD_DIR, id, out, size);
}

static int in_tree(const char *tree, char *out, size_t size)
{
    if (!tree || !*tree)
        return 0;
    return (size_t)snprintf(out, size, "%s/" CARD_FILE, tree) < size;
}

static int spit(FILE *f, void *ud)
{
    return fputs(ud, f) >= 0;
}

void boardfile_keep(const struct board_card *c)
{
    char tree[4300], kept[4300];
    if (!c || !in_tree(c->worktree, tree, sizeof tree) ||
        !boardfile_kept(c->id, kept, sizeof kept))
        return;

    /* No file to read is not an empty file: a worker that deleted it, or a
       turn that died before the worktree was made, leaves the last copy. */
    char *text = text_slurp(tree, CARD_MAX, NULL);
    if (!text)
        return;

    text_spit(kept, spit, text);
    free(text);
}

int boardfile_put(const char *tree, const struct board_card *c)
{
    char path[4300], kept[4300];
    if (!c || !in_tree(tree, path, sizeof path) ||
        !boardfile_kept(c->id, kept, sizeof kept))
        return 0;

    char *text = text_slurp(kept, CARD_MAX, NULL);
    if (!text)
        return 0;

    int ok = text_spit(path, spit, text);
    free(text);
    return ok;
}

void boardfile_drop(const char *id)
{
    char kept[4300];
    if (boardfile_kept(id, kept, sizeof kept))
        unlink(kept);
}
