#include "views.h"

#include "boardview.h"
#include "sessionswitch.h"
#include "workspace.h"

static int on_board;

static int sessions(void)
{
    on_board = 0;
    return sessionswitch_run();
}

static int board(const char *cwd)
{
    on_board = 1;
    int tab = boardview_run(cwd);
    if (tab == BOARDVIEW_NONE)
        on_board = 0;
    return tab;
}

void views_board(const char *cwd)
{
    for (;;) {
        int tab = board(cwd);
        if (tab == BOARDVIEW_SESSIONS) {
            if (sessions() == SESSIONSWITCH_BOARD)
                continue;
            return;
        }
        if (tab >= 0)
            workspace_show(tab);
        return;
    }
}

void views_sessions(const char *cwd)
{
    if (sessions() == SESSIONSWITCH_BOARD)
        views_board(cwd);
}

void views_last(const char *cwd)
{
    if (on_board)
        views_board(cwd);
    else
        views_sessions(cwd);
}
