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

static void check_tokenomics(void)
{
    const struct sessionpresent_tokens turns[] = {
        {.backend = "claude", .model = "m", .prompt = "first\tprompt",
         .fresh = 1000, .cache_write = 9000, .cache_read = 0, .output = 500,
         .context = 10500, .cost = 0.5,
         .rate_input = 10, .rate_cache_write = 12.5, .rate_cache_read = 1, .rate_output = 50},
        {.backend = "claude", .model = "m", .prompt = "second",
         .fresh = 0, .cache_write = 1000, .cache_read = 9000, .output = 500,
         .context = 11000, .cost = 0.25,
         .rate_input = 10, .rate_cache_write = 12.5, .rate_cache_read = 1, .rate_output = 50},
    };

    ui_capture_begin(100);
    sessionpresent_tokenomics(turns, 2, 200000);
    char *drawn = ui_capture_end();
    expect("tokenomics snapshot",
           "  tokenomics \xc2\xb7 claude m \xc2\xb7 2 turns\n"
           "\n"
           "                 tokens   share  list cost\n"
           "  fresh input      1.0k    5.0%    $0.0100\n"
           "  cache write     10.0k   50.0%    $0.1250\n"
           "  cache read       9.0k   45.0%    $0.0090\n"
           "  output           1.0k            $0.0500\n"
           "  total           21.0k            $0.1940\n"
           "\n"
           "  cache hit    45.0% of input\n"
           "  cache reuse  0.9 reads per written token\n"
           "  peak context 11.0k / 200.0k (5%)\n"
           "  charged      $0.7500\n"
           "\n"
           "  turn   fresh   write    read     out context      cost  prompt\n"
           "     1    1.0k    9.0k       0     500   10.5k   $0.5000  first prompt\n"
           "     2       0    1.0k    9.0k     500   11.0k   $0.2500  second",
           drawn);
    free(drawn);

    struct sessionpresent_tokens wide = turns[1];
    snprintf(wide.prompt, sizeof wide.prompt, "%s",
             "caf\xc3\xa9 \xe6\xbc\xa2\xe5\xad\x97 a prompt far longer than the column has room for");
    ui_capture_begin(80);
    sessionpresent_tokenomics(&wide, 1, 0);
    drawn = ui_capture_end();
    char *plain = ui_plain(drawn, 1);
    const char *row = plain ? strstr(plain, "     1 ") : NULL;
    if (!row || !strstr(row, "caf\xc3\xa9 \xe6\xbc\xa2\xe5\xad\x97 a prompt\xe2\x80\xa6") ||
        ui_cells(row) > 80) {
        fprintf(stderr, "FAIL long prompt is cut to the column with an ellipsis\ngot:\n%s\n",
                row ? row : "(none)");
        failures++;
    }
    free(plain);
    free(drawn);

    ui_capture_begin(100);
    sessionpresent_tokenomics(NULL, 0, 0);
    drawn = ui_capture_end();
    expect("tokenomics with no turns", "  no completed turns yet", drawn);
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
    if (present.streamed || present.call_open ||
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
    check_tokenomics();
    check_turn_state();

    if (failures)
        return 1;
    puts("sessionpresenttest: all checks passed");
    return 0;
}
