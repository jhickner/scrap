#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "app.h"
#include "board.h"
#include "boardcfg.h"
#include "boardname.h"
#include "boardview.h"
#include "child.h"
#include "boardwork.h"
#include "bash.h"
#include "chrome.h"
#include "cmd.h"
#include "confirm.h"
#include "gitinfo.h"
#include "hud.h"
#include "image.h"
#include "imageview.h"
#include "livelist.h"
#include "prompt.h"
#include "restart.h"
#include "handoff.h"
#include "scrollback.h"
#include "session.h"
#include "sessionfork.h"
#include "sessionload.h"
#include "sessionswitch.h"
#include "sessionview.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tg.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "views.h"
#include "workspace.h"
#include "vendor/agents/backend.h"
#include "vendor/repl.h"
#include "text.h"

static void backend_choices(char *out, size_t size)
{
    size_t n = 0;
    for (const char *const *p = backend_names(); *p && n + 1 < size; p++) {
        int w = snprintf(out + n, size - n, "%s%s", n ? ", " : "", *p);
        if (w > 0)
            n += (size_t)w < size - n ? (size_t)w : size - n - 1;
    }
}

static int backend_known(const char *name)
{
    for (const char *const *p = backend_names(); *p; p++)
        if (strcmp(*p, name) == 0)
            return 1;
    return 0;
}

static void restore_terminal(void)
{
    viewport_end();
    ui_cursor_restore();
    tty_raw_end();
}

static void replay_tab(struct session *s, void *ud)
{
    const char *screen = ud;
    struct stat st;

    if (screen && *screen && stat(screen, &st) == 0 && st.st_size > 0 &&
        scrollback_restore(screen))
        return;
    hud_print(s);
    sessionload_into(s);
}

static void restore_tabs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    int front = workspace_index();

    char line[6144];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        char       *rest = line;
        const char *screen = strsep(&rest, "\t");
        const char *backend = NULL, *cwd = NULL, *model = NULL, *effort = NULL;
        const char *id = NULL;
        for (char *arg; (arg = strsep(&rest, "\t"));) {
            if (arg[0] != '-' || !strcmp(arg, "-s"))
                continue;
            char *value = strsep(&rest, "\t");
            if (!value)
                break;
            if (!strcmp(arg, "-b"))
                backend = value;
            else if (!strcmp(arg, "-C"))
                cwd = value;
            else if (!strcmp(arg, "-m"))
                model = value;
            else if (!strcmp(arg, "-e"))
                effort = value;
            else if (!strcmp(arg, "--session"))
                id = value;
        }
        if (!backend || !*backend || !id || !*id)
            continue;

        int at = workspace_spawn(backend, model, effort, cwd, id);
        if (at < 0)
            continue;
        workspace_show(front);
        workspace_render(at, replay_tab, (void *)screen);
        if (screen && *screen)
            unlink(screen);
    }
    fclose(f);
}

static void usage(void)
{
    char choices[128];
    backend_choices(choices, sizeof choices);
    fprintf(stderr,
            "usage: " APP_NAME " [-b backend] [-m model] [-e effort] [-C dir] [-s] [-r] [prompt...]\n"
            "\n"
            "  -b name    agent CLI to drive: %s (default: the last /default pick, else claude)\n"
            "  -m model   model to run (default: the last /model pick, else the CLI's own)\n"
            "  -e effort  reasoning/thinking effort (default: the last /effort pick, else the CLI's own)\n"
            "  -C dir     working directory for the agent's tools\n"
            "  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks\n"
            "  --card     put the rest of the line on the board and leave\n"
            "             -b and --tier pin the worker; a card id retargets it\n"
            "  --tier     with --card: low, med or high\n"
            "  --telegram also answer over Telegram, in the same session\n"
            "  --connect telegram   the same thing, spelled out\n"
            "  -r         --resume: pick a past conversation to continue\n"
            "  --session id  resume a specific conversation (used by the fork commands)\n"
            "  --fork     with --session: branch off it instead of writing back to it\n"
            "  --restore f  take over the screen from a restarting mux (used by /restart)\n"
            "  --tabs f   reopen the sessions a restarting mux was holding (used by /restart)\n"
            "  -h         this help\n"
            "\n"
            "With a prompt on the command line, answer it and exit.\n",
            choices);
}

static int idle_fds(void *ud, int *out, int max)
{
    (void)ud;
    int n = workspace_fds(out, max);
    n += sidechannel_fds(out + n, max - n);
    return n + tg_fds(out + n, max - n);
}

static int bash_fds(void *ud, int *out, int max)
{
    (void)ud;
    return workspace_fds(out, max);
}

static void bash_ready(void *ud)
{
    (void)ud;
    workspace_drain();
}

static void reap_children(void)
{
    char key[CHILD_KEY_MAX];
    for (;;) {
        char *out = NULL;
        int   ok = 0;
        if (!child_reap(key, sizeof key, &out, &ok))
            break;
        (void)ok;
        boardname_take(key, out);
        free(out);
    }
}

static void offer_project_trust(struct session *s)
{
    if (!session_take_trust_request(s))
        return;
    if (confirm_run("trust this folder in codex?") && !session_trust_project(s)) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("could not trust this folder or reload Codex");
        viewport_item_end();
        ui_flush();
    }
}

static int idle_render(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
    reap_children();

    if (tg_pending())
        tty_wake();
    struct session *drew = session_set_drawing(workspace_current());
    image_poll();
    session_set_drawing(drew);
    int busy = workspace_pump();
    status_tick();
    return busy;
}

static char *chat_line(void *ud)
{
    (void)ud;
    return tg_take_line();
}

static int side_busy(void *ud)  { (void)ud; return sidechannel_busy() || workspace_busy(); }

static void side_tick(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
    reap_children();
    image_poll();
    workspace_pump();
    status_tick();
}
static int idle_poll(void *ud)   { (void)ud; return workspace_polling(); }
static void replay(void *ud)      { (void)ud; session_replay(workspace_current()); }
static void blank_line(void *ud)  { (void)ud; hud_print(workspace_current()); }
static int clicked(void *ud, int row, int col)
{
    (void)ud;
    int tab = chrome_tab_at(row, col);
    if (tab >= 0) {
        if (tab != workspace_index())
            workspace_show(tab);
        return 1;
    }
    return imageview_click(row, col);
}

static void switcher(void *ud)
{
    struct session *here = workspace_current();
    views_last(here ? session_cwd(here) : NULL);
    if (!workspace_count())
        prompt_stop(ud);
}

static void board(void *ud)
{
    struct session *here = workspace_current();
    if (!here)
        return;
    views_board(session_cwd(here));
    if (!workspace_count())
        prompt_stop(ud);
}

static void cycle_session(void *ud, int delta)
{
    (void)ud;
    workspace_cycle(delta);
}

static void another(void *ud)
{
    (void)ud;
    int n = workspace_count();
    for (int i = 1; i < n; i++) {
        int at = (workspace_index() + i) % n;
        if (session_turn_running(workspace_at(at)) || workspace_queued(at))
            continue;
        workspace_show(at);
        return;
    }

    struct session *here = workspace_current();
    if (!here)
        return;
    if (n >= WORKSPACE_MAX) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_note("this window is already holding as many sessions as it can");
        viewport_item_end();
        ui_flush();
        return;
    }
    if (workspace_spawn(session_backend(here), session_model(here), session_effort(here),
                        session_cwd(here), NULL) < 0) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("could not start another %s CLI", session_backend(here));
        viewport_item_end();
        ui_flush();
        return;
    }
    hud_print(workspace_current());
    ui_flush();
}

static void collapse_tools(void *ud)
{
    (void)ud;
    struct session *s = workspace_current();
    int on = !(s ? session_compact(s) : view_collapsed());

    if (s)
        session_set_compact(s, on);
    settings_set_int(SETTING_COMPACT, on);
    view_collapse(on);
}

static void splitter(void *ud, int quiet)
{
    (void)ud;
    sessionfork_shell(workspace_current(), FORK_SPLIT_H, quiet);
}

static int takeover_pending(void *ud)
{
    (void)ud;
    if (tty_quit_requested())
        return 1;
    return restart_wanted() && handoff_wanted();
}

static void takeover_run(void *ud)
{
    if (tty_quit_requested()) {
        prompt_stop(ud);
        return;
    }
    sessionswitch_serve_request();
    restart_clear();
    if (sessionswitch_gave_last())
        prompt_stop(ud);
}

static int window_working(void)
{
    if (workspace_busy())
        return 1;
    for (int i = 0; i < workspace_count(); i++)
        if (workspace_queued(i))
            return 1;
    return 0;
}

static int restart_pending(void *ud)
{
    (void)ud;
    return restart_wanted() && !handoff_wanted() && !window_working();
}

static int idle_restart(void *ud)
{
    (void)ud;

    sidechannel_close_all();
    child_close_all();

    if (!restart_exec(workspace_current())) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("could not restart: no runnable %s at %s or on PATH — "
                 "staying on this build",
                 APP_NAME, sessionfork_program());
        viewport_item_end();
        ui_flush();
    }
    return 0;
}

static int echo_filter(void *ud, const char *line)
{
    (void)ud;
    if (cmd_self_echoes(line))
        return 0;

    if (session_turn_running(workspace_current()) && !cmd_is_command(line) &&
        !bash_is_command(line))
        return 0;
    return 1;
}

static int cancel_turn(void *ud)
{
    (void)ud;
    struct session *s = workspace_current();
    if (!session_turn_running(s))
        return 0;
    session_interrupt(s);
    return 1;
}

static void turn_done(struct session *s)
{
    cmd_run_deferred(s);
}

static int tab_queued(void *ud)
{
    (void)ud;
    return workspace_queued(workspace_index());
}

static const char *tab_queued_at(void *ud, int i)
{
    (void)ud;
    return workspace_pending_at(workspace_index(), i);
}

static char *tab_unqueue(void *ud)
{
    (void)ud;
    return workspace_unqueue(workspace_index());
}

static int live_command(void *ud, const char *line)
{
    (void)ud;
    if (!cmd_runs_mid_turn(line))
        return 0;
    status_pause();
    if (!cmd_self_echoes(line))
        prompt_echo_message(line);
    cmd_dispatch_live(workspace_current(), line);
    status_resume();
    return 1;
}

int main(int argc, char **argv)
{
    static const struct option LONG_OPTS[] = {
        {"backend", required_argument, NULL, 'b'},
        {"model",   required_argument, NULL, 'm'},
        {"effort",  required_argument, NULL, 'e'},
        {"dir",     required_argument, NULL, 'C'},
        {"safe",    no_argument,       NULL, 's'},
        {"resume",  no_argument,       NULL, 'r'},
        {"session", required_argument, NULL, 'S'},
        {"fork",    no_argument,       NULL, 'F'},
        {"restore", required_argument, NULL, 'R'},
        {"tabs",    required_argument, NULL, 'B'},
        {"card",    no_argument,       NULL, 'K'},
        {"tier",    required_argument, NULL, 1},
        {"telegram", no_argument,      NULL, 'T'},
        {"connect", required_argument, NULL, 'N'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL,      0,                 NULL, 0},
    };

    const char *backend = "claude";
    const char *model = NULL;
    const char *effort = NULL;
    const char *tier = NULL;
    const char *dir = NULL;
    const char *session_arg = NULL;
    const char *restore_arg = NULL;
    const char *tabs_arg = NULL;
    int telegram = 0;
    int card = 0;
    int pin_backend = 0;
    int fork_session = 0;
    int safe_mode = 0;
    int resume = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "b:m:e:C:srh", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'b': backend = optarg; pin_backend = 1; break;
        case 'm': model = optarg; break;
        case 'e': effort = optarg; break;
        case 1:   tier = optarg; break;
        case 'C': dir = optarg; break;
        case 's': safe_mode = 1; break;
        case 'r': resume = 1; break;
        case 'S': session_arg = optarg; break;
        case 'F': fork_session = 1; break;
        case 'R': restore_arg = optarg; break;
        case 'B': tabs_arg = optarg; break;
        case 'K': card = 1; break;
        case 'T': telegram = 1; break;
        case 'N':
            if (strcmp(optarg, "telegram")) {
                fprintf(stderr, APP_NAME ": --connect takes 'telegram'\n");
                return 2;
            }
            telegram = 1;
            break;
        default:  usage(); return opt == 'h' ? 0 : 2;
        }
    }

    if (!backend_known(backend)) {
        char choices[128];
        backend_choices(choices, sizeof choices);
        fprintf(stderr, APP_NAME ": unknown backend '%s' — pick one of %s\n", backend, choices);
        return 2;
    }
    if (effort && !strcmp(effort, "default"))
        effort = NULL;
    if (tier && !card) {
        fprintf(stderr, APP_NAME ": --tier is for --card\n");
        return 2;
    }
    if (tier && boardcfg_tier_from_name(tier) >= BOARD_TIERS) {
        fprintf(stderr, APP_NAME ": --tier takes low, med or high\n");
        return 2;
    }

    sessionfork_set_program(argv[0]);

    if (resume && optind < argc) {
        fprintf(stderr, APP_NAME ": --resume takes no prompt\n");
        return 2;
    }

    char cwd[4096];
    if (dir) {
        if (!realpath(dir, cwd)) {
            fprintf(stderr, APP_NAME ": no such directory: %s\n", dir);
            return 1;
        }
    } else if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, APP_NAME ": cannot determine the working directory\n");
        return 1;
    }

    if (card) {
        const char *card_backend = pin_backend ? backend : NULL;
        const char *card_tier = tier;
        int         rest = argc - optind;

        if (rest == 1 && (card_backend || card_tier) &&
            board_pin(argv[optind], card_backend, card_tier)) {
            printf("%s\n", argv[optind]);
            return 0;
        }
        if (rest <= 0) {
            fprintf(stderr, APP_NAME ": --card takes a task description\n");
            return 2;
        }
        size_t need = 1;
        for (int i = optind; i < argc; i++)
            need += strlen(argv[i]) + 1;
        char *text = malloc(need);
        if (!text) {
            fprintf(stderr, APP_NAME ": out of memory\n");
            return 1;
        }
        text[0] = '\0';
        for (int i = optind; i < argc; i++) {
            if (i > optind)
                strcat(text, " ");
            strcat(text, argv[i]);
        }
        char id[16] = {0};
        int  ok = boardview_capture(text, cwd, id, sizeof id);
        if (ok && (card_backend || card_tier))
            ok = board_pin(id, card_backend, card_tier);
        free(text);
        if (!ok) {
            fprintf(stderr, APP_NAME ": could not write to the board\n");
            return 1;
        }
        printf("%s\n", id);
        return 0;
    }

    char config[4096];
    int have_config = path_config_dir(config, sizeof config);
    if (have_config) {
        char path[4200];
        snprintf(path, sizeof path, "%s/settings", config);
        settings_open(path);
    }

    if (!pin_backend)
        backend = cmd_default_backend();

    if (!model)
        model = session_saved_model(backend);
    if (!effort)
        effort = session_saved_effort(backend);

    ui_init();

    image_init();
    image_set_rows(settings_get_int(SETTING_IMAGE_ROWS, IMAGE_ROWS_DEFAULT));

    int interactive = optind >= argc;

    if (telegram && !interactive) {
        fprintf(stderr, APP_NAME ": --telegram takes no prompt\n");
        return 2;
    }

    if (interactive) {
        restart_arm();
        if (tty_raw_begin() != 0) {
            fprintf(stderr, APP_NAME ": not a terminal — pass a prompt as arguments instead\n");
            return 1;
        }
        atexit(restore_terminal);
        ui_raw(1);
        ui_cursor_plain();

        if (restore_arg) {
            view_collapse(settings_get_int(SETTING_COMPACT, 0));
            viewport_inherit();
            scrollback_restore(restore_arg);
            unlink(restore_arg);
        } else {
            viewport_begin();
        }

        if (tty_input_waiting()) {
            ui_esc("\r");
            ui_esc(UI_ERASE_BELOW);
            ui_flush();
        }
    }

    agenttabs_begin();
    if (interactive)
        livelist_begin();
    struct session *session = session_new(backend, cwd, model, effort);
    if (session) {
        session_set_customizations(session, !safe_mode);
        session_set_browser_login(session, interactive);
        session_set_fork(session, fork_session && session_arg);
        session_set_thinking(session, settings_get_int(SETTING_THINKING, 1));
        session_set_compact(session, settings_get_int(SETTING_COMPACT, 0));
        session_set_permission(session,
            session_permission_name(settings_get_int(SETTING_PERMISSION,
                                                     session_permission_default())));

        session_adopt_id(session, session_arg);
    }

    if (telegram && session && !tg_start(session))
        telegram = 0;

    if (!session || !session_start(session)) {
        tty_raw_end();
        fprintf(stderr, APP_NAME ": could not start the %s CLI — is it on PATH?\n", backend);
        session_free(session);
        return 1;
    }

    if (!interactive) {
        size_t need = 1;
        for (int i = optind; i < argc; i++)
            need += strlen(argv[i]) + 1;
        char *text = calloc(need, 1);
        if (!text) {
            session_free(session);
            return 1;
        }
        size_t at = 0;
        for (int i = optind; i < argc; i++) {
            if (i > optind)
                text[at++] = ' ';
            size_t n = strlen(argv[i]);
            memcpy(text + at, argv[i], n);
            at += n;
        }
        text[at] = '\0';
        session_set_quiet(session, !isatty(STDOUT_FILENO));
        session_set_naming(session, 0);
        int ok = session_turn(session, text);
        free(text);
        session_free(session);
        return ok ? 0 : 1;
    }

    status_sticky_set(settings_get_int(SETTING_STICKY, 0));

    if (!workspace_begin(session, safe_mode)) {
        session_free(session);
        return 1;
    }

    int                cmd_count = 0;
    const ReplCommand *cmds = cmd_completions(&cmd_count);
    struct prompt     *prompt = prompt_new(cmds, cmd_count);
    if (!prompt) {
        workspace_end();
        return 1;
    }
    prompt_file_completion(prompt, cwd);
    if (have_config) {
        char history[4200];
        snprintf(history, sizeof history, "%s/history", config);
        prompt_history_open(prompt, history);
    }

    session_set_typeahead(prompt_live_key, prompt);
    chrome_bind(prompt);
    chrome_modal_interrupt(handoff_wanted);
    prompt_set_live_command(prompt, live_command, NULL);
    prompt_set_queued_source(prompt, tab_queued, tab_queued_at, tab_unqueue, NULL);
    prompt_set_echo_filter(prompt, echo_filter, NULL);
    prompt_set_idle(prompt, idle_fds, idle_render, idle_poll, NULL);
    prompt_set_restart(prompt, restart_pending, idle_restart, NULL);
    prompt_set_takeover(prompt, takeover_pending, takeover_run, prompt);
    prompt_set_switcher(prompt, switcher, prompt);
    prompt_set_click(prompt, clicked, NULL);
    prompt_set_split(prompt, splitter, NULL);
    prompt_set_another(prompt, another, NULL);
    prompt_set_cycle(prompt, cycle_session, NULL);
    prompt_set_collapse(prompt, collapse_tools, NULL);
    view_collapse(session_compact(session));
    prompt_set_board(prompt, board, prompt);
    prompt_set_cancel(prompt, cancel_turn, NULL);
    workspace_on_finish(turn_done);

    workspace_on_settled(boardwork_finished);
    workspace_on_turn(boardwork_spoke_to);
    livelist_on_card(boardwork_card_of);
    prompt_set_replay(prompt, replay, NULL);
    prompt_set_blank(prompt, blank_line, NULL);
    prompt_set_animate(prompt, side_busy, side_tick, NULL);
    if (telegram)
        prompt_set_external(prompt, chat_line, NULL);

    if (!resume || !cmd_resume(session))
        hud_print(session);

    if (tabs_arg) {
        restore_tabs(tabs_arg);
        unlink(tabs_arg);
    }

    boardwork_reattach();

    if (!resume && session_arg && !restore_arg)
        sessionload_into(session);

    for (;;) {
        session = workspace_current();
        if (!session)
            break;

        offer_project_trust(session);

        char *line = prompt_take_queued(prompt);
        if (line) {
            if (!cmd_self_echoes(line))
                prompt_echo_message(line);
        } else {
            line = prompt_read(prompt);

            if (tty_quit_requested()) {
                free(line);
                break;
            }
            if (!line && workspace_count() > 1) {
                workspace_close(workspace_index());
                continue;
            }
        }
        if (!line)
            break;

        session = workspace_current();
        if (!session) {
            free(line);
            break;
        }

        if (bash_is_command(line)) {
            tty_watch(bash_fds, bash_ready, NULL);
            bash_run(line);
            tty_watch(NULL, NULL, NULL);
            gitinfo_forget();
            char *text = bash_take_context();
            if (text) {
                workspace_send(workspace_index(), text, line);
                free(text);
            }
            free(line);
            prompt_restart_check(prompt);
            continue;
        }

        if (prompt_line_was_external(prompt)) {
            workspace_settle(tg_session());
            tg_run_line(line);
            prompt_restart_check(prompt);
            continue;
        }

        enum cmd_result r = cmd_submit(session, line);
        if (r == CMD_QUIT) {
            free(line);
            break;
        }
        free(line);
        prompt_restart_check(prompt);
    }

    sidechannel_close_all();
    child_close_all();
    tg_stop();
    session_set_typeahead(NULL, NULL);
    chrome_bind(NULL);
    prompt_free(prompt);

    char *notes[WORKSPACE_MAX];
    int   nnotes = 0;
    for (int i = 0; i < workspace_count(); i++) {
        char *note = sessionfork_exit_note(workspace_at(i));
        if (note && nnotes < WORKSPACE_MAX)
            notes[nnotes++] = note;
        else
            free(note);
    }
    workspace_end();
    viewport_end();
    ui_raw(0);
    tty_raw_end();
    for (int i = 0; i < nnotes; i++) {
        fputs(notes[i], stdout);
        free(notes[i]);
    }
    if (nnotes)
        fflush(stdout);
    return 0;
}
