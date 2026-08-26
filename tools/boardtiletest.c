#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "boarddiff.h"
#include "boardflow.h"
#include "boardname.h"
#include "boardtile.h"
#include "boardwork.h"
#include "session.h"
#include "ui.h"
#include "workspace.h"

enum board_stand board_stands(const struct board_card *c)
{
    if (c->closed)
        return BOARD_CLOSED;
    if (c->queue_n)
        return BOARD_WORKING;
    if (c->done_n || c->stopped)
        return BOARD_REVIEW;
    return BOARD_OPEN;
}

int boardflow_waits_on_you(const struct board_card *c)
{
    return board_stands(c) == BOARD_REVIEW;
}

int boardname_running(const char *id)
{
    (void)id;
    return 0;
}

const char *boardwork_step_job(const char *id)
{
    (void)id;
    return NULL;
}

int boardwork_tab(const char *id)
{
    (void)id;
    return -1;
}

double boardwork_elapsed(const char *id)
{
    (void)id;
    return 0;
}

int boardwork_blocked(const struct board_card *c, char *why, int size)
{
    (void)c;
    if (size > 0)
        why[0] = '\0';
    return 0;
}

int boarddiff_size_cached(const struct board_card *c, int *files, int *lines)
{
    (void)c;
    *files = 0;
    *lines = 0;
    return 0;
}

struct session *workspace_at(int index)
{
    (void)index;
    return NULL;
}

double session_quiet(const struct session *s)
{
    (void)s;
    return 0;
}

int session_busy(const struct session *s)
{
    (void)s;
    return 0;
}

int session_recent(const struct session *s, const char **out, int max)
{
    (void)s;
    (void)out;
    (void)max;
    return 0;
}

size_t ui_cells(const char *s)
{
    return strlen(s);
}

static void expect_status(const struct board_card *c, const char *want,
                          const char *absent)
{
    struct board_tile tile;
    if (!boardtile_of(c, 0, &tile) || !strstr(tile.status, want) ||
        (absent && strstr(tile.status, absent))) {
        fprintf(stderr, "boardtiletest: expected %s without %s, got %s\n",
                want, absent ? absent : "anything", tile.status);
        exit(1);
    }
}

int main(void)
{
    struct board_card c = {.closed = 1,
                           .tokens_in = 12345,
                           .tokens_out = 678,
                           .created = time(NULL),
                           .updated = time(NULL)};

    expect_status(&c, "12.3k in · 678 out · just now", "$");

    c.cost_usd = 0.42;
    expect_status(&c, "$0.42 · just now", "12.3k in");

    c.closed = 0;
    c.done_n = 1;
    c.cost_usd = 0;
    expect_status(&c, "just now", "12.3k in");

    return 0;
}
