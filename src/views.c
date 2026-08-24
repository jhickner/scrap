#include "views.h"

#include "boardview.h"
#include "sessionswitch.h"
#include "workspace.h"

void views_board(const char *cwd)
{
    for (;;) {
        int tab = boardview_run(cwd);
        if (tab == BOARDVIEW_SESSIONS) {
            if (sessionswitch_run() == SESSIONSWITCH_BOARD)
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
    if (sessionswitch_run() == SESSIONSWITCH_BOARD)
        views_board(cwd);
}
