#include <stdio.h>
#include <string.h>

#include "boardview.h"
#include "sessionswitch.h"
#include "views.h"
#include "workspace.h"

static int  session_result;
static int  board_result;
static int  shown = -1;
static char calls[16];
static int  ncalls;
static int  failures;

int sessionswitch_run(void)
{
    calls[ncalls++] = 's';
    return session_result;
}

int boardview_run(const char *cwd)
{
    (void)cwd;
    calls[ncalls++] = 'b';
    return board_result;
}

void workspace_show(int index)
{
    calls[ncalls++] = 'w';
    shown = index;
}

static void expect(const char *name, const char *want)
{
    calls[ncalls] = '\0';
    if (strcmp(calls, want)) {
        printf("FAIL %s: got %s, want %s\n", name, calls, want);
        failures++;
    }
    ncalls = 0;
}

int main(void)
{
    session_result = SESSIONSWITCH_BOARD;
    board_result = BOARDVIEW_SESSIONS;
    views_last(".");
    expect("sessions first", "sb");

    views_last(".");
    expect("board first", "bs");

    views_sessions(".");
    expect("sessions entry", "sb");

    views_board(".");
    expect("board entry", "bs");

    board_result = 4;
    views_board(".");
    expect("board selection", "bw");
    if (shown != 4) {
        printf("FAIL board selection: showed %d, want 4\n", shown);
        failures++;
    }

    if (failures)
        return 1;
    puts("viewstest: ok");
    return 0;
}
