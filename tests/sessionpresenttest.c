#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sessionpresent.h"
#include "restart.h"
#include "sidechannel.h"
#include "ui.h"

static int failures;

void restart_shield_thread(void) {}
int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }

static void expect(const char *what, const char *want, const char *drawn)
{
    char *plain = ui_plain(drawn, 1);
    if (!plain || strcmp(plain, want)) {
        fprintf(stderr, "FAIL %s\nwant:\n%s\ngot:\n%s\n", what, want,
                plain ? plain : "(null)");
        failures++;
    }
    free(plain);
}

static void check_footer(void)
{
    ui_capture_begin(80);
    sessionpresent_footer(12.0, 1500, 8000, 0.125, "A useful title");
    char *drawn = ui_capture_end();
    expect("footer snapshot",
           "12s \xc2\xb7 1.5k / 8.0k (18%) \xc2\xb7 $0.1250 \xc2\xb7 A useful title",
           drawn);
    free(drawn);

    ui_capture_begin(28);
    sessionpresent_footer(12.0, 1500, 8000, 0.125, "A useful title");
    drawn = ui_capture_end();
    expect("narrow footer keeps the title on a second line",
           "12s \xc2\xb7 1.5k / 8.0k (18%) \xc2\xb7 $0.1250\nA useful title", drawn);
    free(drawn);
}

static void check_report(void)
{
    const struct sessionpresent_report report = {
        .backend = "codex",
        .model = "gpt-test",
        .effort = "high",
        .auth = "subscription login",
        .chat = "team-chat",
        .id = "abc123",
        .parent = "parent turn",
        .cwd = "/tmp/project",
        .compact = 1,
        .turns = 7,
        .context_tokens = 1500,
        .context_window = 8000,
        .cost = 0.125,
    };

    ui_capture_begin(80);
    sessionpresent_report(&report);
    char *drawn = ui_capture_end();
    expect("session report snapshot",
           "  backend  codex\n"
           "  model    gpt-test\n"
           "  effort   high\n"
           "  auth     subscription login\n"
           "  calls    compact (one row each)\n"
           "  chat     team-chat\n"
           "  session  abc123\n"
           "  parent   parent turn\n"
           "  cwd      /tmp/project\n"
           "  turns    7\n"
           "  context  1.5k / 8.0k\n"
           "  cost     $0.1250  (list price; the subscription is not billed per token)",
           drawn);
    free(drawn);
}

static void check_turn_state(void)
{
    struct sessionpresent present = {0};
    present.streamed = strdup("old streamed answer");
    present.streamed_len = strlen(present.streamed);
    present.streamed_cap = present.streamed_len + 1;
    present.call_open = 1;
    present.view.after_collapse = 1;

    sessionpresent_turn_begin(&present);
    if (sessionpresent_streamed(&present) || present.call_open ||
        present.view.after_collapse) {
        fprintf(stderr, "FAIL a new turn resets presentation-only state\n");
        failures++;
    }
    sessionpresent_free(&present);
}

int main(void)
{
    ui_init();
    check_footer();
    check_report();
    check_turn_state();

    if (failures)
        return 1;
    puts("sessionpresenttest: all checks passed");
    return 0;
}
