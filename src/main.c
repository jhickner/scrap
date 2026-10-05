#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "app.h"
#include "askblock.h"
#include "askform.h"
#include "bash.h"
#include "chrome.h"
#include "newsession.h"
#include "cmd.h"
#include "frontend.h"
#include "pick.h"
#include "confirm.h"
#include "gitinfo.h"
#include "hud.h"
#include "stamp.h"
#include "tabbar.h"
#include "image.h"
#include "docview.h"
#include "stream.h"
#include "tailnet.h"
#include "intercom.h"
#include "imageview.h"
#include "livelist.h"
#include "md.h"
#include "prompt.h"
#include "restart.h"
#include "handoff.h"
#include "scrollback.h"
#include "session.h"
#include "sessionprefs.h"
#include "sessionfork.h"
#include "sessionload.h"
#include "hub.h"
#include "instance.h"
#include "job.h"
#include "memnote.h"
#include "sessionpresent.h"
#include "sessionswitch.h"
#include "sessionview.h"
#include "settings.h"
#include "version.h"
#include "dispatch.h"
#include "sidechannel.h"
#include "tabs.h"
#include "status.h"
#include "tg.h"
#include "im.h"
#include "agentsync.h"
#include "relay.h"
#include "bridges.h"
#include "api.h"
#include "voice.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"
#include "vendor/agents/backend.h"
#include "vendor/repl.h"
#include "text.h"
#include "grokbottail.h"
#include "vncinset.h"
#include "vncsource.h"

const char app_version[] = SCRAP_VERSION;

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

static void start_failed(const char *backend)
{
    const char *why = session_start_error();
    if (why)
        fprintf(stderr, APP_NAME ": could not start %s: %s\n", backend, why);
    else
        fprintf(stderr, APP_NAME ": could not start the %s CLI — is it on PATH?\n", backend);
}

static void restore_terminal(void)
{
    viewport_end();
    ui_term_colors_restore();
    tty_raw_end();
}

static int pick_startup_bot(struct session *s)
{
    if (strcmp(session_backend(s), "grokbot") || strcmp(session_model(s), "default"))
        return 1;
    int                     count = 0;
    const struct pick_item *choices = cmd_model_choices("grokbot", &count);
    if (!count || !frontend_has_keyboard())
        return 0;
    int index = pick_run_filter("select bot", choices, count, 0);
    viewport_flush();
    ui_flush();
    return index >= 0 && session_preset_model(s, choices[index].label);
}

static int state_enter(const char *arg)
{
    char root[4096];
    if (mkdir(arg, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, APP_NAME ": cannot create %s: %s\n", arg, strerror(errno));
        return 0;
    }
    if (!realpath(arg, root)) {
        fprintf(stderr, APP_NAME ": no such directory: %s\n", arg);
        return 0;
    }
    static const char *const LEAVES[][2] = {
        {"SCRAP_CONFIG_DIR", "config"},
        {"AGENT_TABS_STATE_DIR", "tabs"},
        {"TMPDIR", "tmp"},
    };
    for (size_t i = 0; i < sizeof LEAVES / sizeof LEAVES[0]; i++) {
        char path[4200];
        snprintf(path, sizeof path, "%s/%s", root, LEAVES[i][1]);
        mkdir(path, 0700);
        setenv(LEAVES[i][0], path, 1);
    }
    unsetenv("SCRAP_LIVE_DIR");
    unsetenv("SCRAP_ADDR_DIR");
    unsetenv("SCRAP_DISPATCH_DIR");
    return 1;
}

static void usage(void)
{
    char choices[128];
    backend_choices(choices, sizeof choices);
    fprintf(stderr,
            "usage: " APP_NAME " [-b backend] [-m model] [-e effort] [-C dir] [-s] [-r] [-p prompt]\n"
            "\n"
            "  -b name    agent CLI to drive: %s (default: the last /default pick, else claude)\n"
            "  -m model   model to run (default: the last /model pick, else the CLI's own)\n"
            "  -e effort  reasoning/thinking effort (default: the last /effort pick, else the CLI's own)\n"
            "  -C dir     working directory for the agent's tools\n"
            "  -p text    --prompt: answer text and exit\n"
            "  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks\n"
            "  --name x   session name (default: a generated one)\n"
            "  --telegram turn on the Telegram bridge; one open window hosts it\n"
            "  --imessage also answer over iMessage, in the same session (config: ~/.config/scrap/imessage)\n"
            "  --connect telegram   the same thing, spelled out\n"
            "  --api      turn on the worker API; one open window hosts it (config: ~/.config/scrap/api)\n"
            "  --state dir  keep config and state under dir instead of ~/.config/scrap\n"
            "  -r         --resume: pick a past conversation to continue\n"
            "  --session id  resume a specific conversation (used by the fork commands)\n"
            "  --fork     with --session: branch off it instead of writing back to it\n"
            "  --restore f  take over the screen from a restarting scrap (used by /restart)\n"
            "  --tabs f   reopen the sessions a restarting scrap was holding (used by /restart)\n"
            "  --relay id  serve that session over the relay once it is open (used by /restart)\n"
            "  --instance x  reopen the tabs saved with /save x\n"
            "  --attach machine:@name   open a live session from another window or machine\n"
            "  -h         this help\n"
            "  -V, --version  print the version and exit\n"
            "\n"
            "  " APP_NAME " version   the same, as a subcommand\n"
            "  " APP_NAME " ls [--live] [--net] [--cwd DIR] [QUERY]   list sessions, newest first\n"
            "  " APP_NAME " read TARGET [-n TURNS] [--bytes N]   print a session's last turns\n"
            "  " APP_NAME " send TARGET TEXT   message a live session\n"
            "  " APP_NAME " open TARGET   resume a past session in a new tab, or here outside scrap\n"
            "  " APP_NAME " attach TARGET   stream a live session as JSON lines; stdin lines are prompts\n"
            "  " APP_NAME " job ls|check NAME   scheduled jobs; " APP_NAME " job prints the file format\n"
            "  " APP_NAME " hub   the per-machine process for the network broker and jobs (started on demand)\n"
            ,
            choices);
}

static int idle_fds(void *ud, int *out, int max)
{
    (void)ud;
    int n = workspace_fds(out, max);
    if (tabs_pending() && n < max) {
        int fd = session_start_fd();
        if (fd >= 0)
            out[n++] = fd;
    }
    n += sidechannel_fds(out + n, max - n);
    n += relay_fds(out + n, max - n);
    n += api_fds(out + n, max - n);
    n += dispatch_fds(out + n, max - n);
    n += stream_fds(out + n, max - n);
    n += im_fds(out + n, max - n);
    return n + tg_fds(out + n, max - n);
}

static void offer_project_trust(struct session *s)
{
    if (!session_take_trust_request(s))
        return;
    if (!confirm_run("trust this folder in codex?"))
        return;
    workspace_settle(s);
    if (!session_trust_project(s)) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_error("could not trust this folder or reload Codex");
        viewport_item_end();
        ui_flush();
    }
}

static int idle_render(void *ud)
{
    (void)ud;
    tabs_admit(0);
    relay_resume_poll();
    sidechannel_poll();
    sidechannel_tick();
    dispatch_poll();
    stream_poll();

    if (tg_pending() || relay_pending() || im_pending())
        tty_wake();
    voice_pending();
    relay_poll(NULL);
    api_poll();
    bridges_tick();
    struct session *drew = session_set_drawing(workspace_current());
    image_poll();
    session_set_drawing(drew);
    int busy = workspace_pump();
    status_tick();
    return busy;
}

static int im_took;

static char *chat_line(void *ud)
{
    (void)ud;
    char *line = im_take_line();
    im_took = line != NULL;
    if (line)
        return line;
    return tg_take_line();
}

static struct vncinset *inset_live(void)
{
    struct session *s = workspace_current();
    if (!s || chrome_modal_active() || strcmp(session_backend(s), "grokbot"))
        return NULL;
    struct vncinset *v = session_inset(s, 0);
    if (!vncinset_shown(v))
        return NULL;
    vncinset_set_bot(v, session_model(s));
    return v;
}

static void inset_cover(char **rows, int n, int cols)
{
    vncinset_cover(inset_live(), rows, n, cols);
    tabbar_cover(rows, n, cols);
    stamp_cover(rows, n, cols);
}

static int side_busy(void *ud)
{
    (void)ud;
    return sidechannel_busy() || workspace_busy() || inset_live();
}

static void side_tick(void *ud)
{
    (void)ud;
    sidechannel_poll();
    sidechannel_tick();
    image_poll();
    workspace_pump();
    status_tick();
    if (vncinset_stale(inset_live()))
        viewport_touch();
}

static int idle_poll(void *ud)   { (void)ud; return 1; }
static void replay(void *ud)      { (void)ud; session_replay(workspace_current()); }
static void load_earlier(void)    { sessionload_earlier(workspace_current()); }
static void blank_line(void *ud)
{
    (void)ud;
    hud_print(workspace_current());
    relay_banner(workspace_current());
}
static int clicked(void *ud, int row, int col)
{
    int tab = tabbar_hit(row, col);
    if (tab == TABBAR_NEW) {
        if (!chrome_modal_active()) {
            newsession_run();
            viewport_flush();
            ui_flush();
        }
        return 1;
    }
    if (tab >= 0) {
        if (tab != workspace_index())
            workspace_show(tab);
        return 1;
    }
    if (imageview_click(row, col) || docview_click(row, col))
        return 1;
    char *cmd = md_command_at(row, col);
    if (cmd)
        prompt_set_line(ud, cmd);
    free(cmd);
    return cmd != NULL;
}

static char *reply_command(void *ud, int nth)
{
    (void)ud;
    struct session *s = workspace_current();
    if (!s)
        return NULL;
    const char *reply = session_last_reply(s);
    return md_command_nth(reply ? reply : session_last_block(s), nth);
}

static void switcher(void *ud)
{
    sessionswitch_run();
    if (!workspace_count())
        prompt_stop(ud);
}

static void focus_changed(int on)
{
    voice_arm(on);
    if (on)
        workspace_log_active();
}

static void step(void *ud, int dir)
{
    (void)ud;
    sessionswitch_step(dir);
}

static void cycle_session(void *ud, int delta)
{
    (void)ud;
    workspace_cycle(delta);
}

static void another(void *ud)
{
    (void)ud;
    struct session *here = workspace_current();
    if (!here)
        return;
    if (workspace_count() >= WORKSPACE_MAX) {
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

static struct askblock *asked;
static struct session  *asked_by;

static void drop_asked(void)
{
    askblock_free(asked);
    asked = NULL;
    asked_by = NULL;
}

static int ask_ready(void)
{
    struct session *s = workspace_current();
    if (!session_ask_open(s))
        return 0;
    if (session_turn_running(s) || workspace_queued(workspace_index())) {
        session_set_ask_open(s, 0);
        return 0;
    }
    if (s != asked_by) {
        drop_asked();
        asked = askblock_parse(session_last_block(s));
        asked_by = asked ? s : NULL;
        if (!asked)
            session_set_ask_open(s, 0);
    }
    return asked != NULL;
}

static int ask_interrupted(void)
{
    return handoff_wanted() || relay_pending();
}

static void ask_run_form(void)
{
    enum askform_exit how;
    chrome_modal_interrupt(ask_interrupted);
    char *answer = askform_run(asked, &how);
    chrome_modal_interrupt(handoff_wanted);
    if (how == ASKFORM_NEW_TAB) {
        another(NULL);
        return;
    }
    if (how != ASKFORM_DONE) {
        workspace_cycle(how == ASKFORM_NEXT_TAB ? 1 : -1);
        return;
    }
    session_set_ask_open(asked_by, 0);
    drop_asked();
    if (answer && *answer) {
        prompt_echo_message(answer);
        workspace_send(workspace_index(), answer, answer);
    }
    free(answer);
}

static int ask_permission(struct session *s, const char *question)
{
    if (s == workspace_current())
        return confirm_run(question);
    char buf[700];
    snprintf(buf, sizeof buf, "@%s: %s", session_name(s), question);
    return confirm_run(buf);
}

static int takeover_pending(void *ud)
{
    (void)ud;
    if (tty_quit_requested())
        return 1;
    if (ask_ready())
        return 1;
    if (session_permission_pending(workspace_current()))
        return 1;
    return restart_wanted() && handoff_wanted();
}

static void takeover_run(void *ud)
{
    if (tty_quit_requested()) {
        prompt_stop(ud);
        return;
    }
    struct session *s = workspace_current();
    const char *question = session_permission_pending(s);
    if (question) {
        session_permission_answer(s, confirm_run(question));
        return;
    }
    if (ask_ready()) {
        ask_run_form();
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
    tabs_admit(1);

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
    if (voice_speaking()) {
        voice_mute();
        return 1;
    }
    struct session *s = workspace_current();
    if (!session_turn_running(s))
        return 0;
    voice_turn_cancel(s);
    session_interrupt(s);
    return 1;
}

static int turn_running(void *ud)
{
    (void)ud;
    return session_turn_running(workspace_current());
}

static char *serve_extra(const cJSON *o, int fd, int *kept)
{
    *kept = stream_serve(o, fd);
    return *kept ? NULL : intercom_serve(o);
}

static void turn_done(struct session *s)
{
    if (s == asked_by)
        drop_asked();
    session_set_ask_open(s, !session_last_result(s)->interrupted);
    api_turn_done(s);
    relay_turn_done(s);
    stream_turn_done(s);
    voice_turn_done(s);
    cmd_run_deferred(s);
    cmd_turn_done(s);
}

static void turn_begin(struct session *s)
{
    api_turn_begin(s);
    stream_turn_begin(s);
    voice_turn_begin(s);
    if (s == asked_by)
        drop_asked();
    session_set_ask_open(s, 0);
    bash_drop_held();
    prompt_drop_held();
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
    if (argc > 1 && !strcmp(argv[1], "sync"))
        return agentsync_main(argc - 1, argv + 1);
    if (argc > 1 && (!strcmp(argv[1], "ls") || !strcmp(argv[1], "read") ||
                     !strcmp(argv[1], "send") || !strcmp(argv[1], "open") ||
                     !strcmp(argv[1], "attach") || !strcmp(argv[1], "yank") || !strcmp(argv[1], "close")))
        return intercom_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "hub"))
        return hub_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "job"))
        return job_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "note"))
        return memnote_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "version")) {
        printf(APP_NAME " %s\n", SCRAP_VERSION);
        return 0;
    }

    static const struct option LONG_OPTS[] = {
        {"backend", required_argument, NULL, 'b'},
        {"model",   required_argument, NULL, 'm'},
        {"effort",  required_argument, NULL, 'e'},
        {"dir",     required_argument, NULL, 'C'},
        {"name",    required_argument, NULL, 'n'},
        {"safe",    no_argument,       NULL, 's'},
        {"resume",  no_argument,       NULL, 'r'},
        {"session", required_argument, NULL, 'S'},
        {"fork",    no_argument,       NULL, 'F'},
        {"restore", required_argument, NULL, 'R'},
        {"tabs",    required_argument, NULL, 'B'},
        {"relay",   required_argument, NULL, 'L'},
        {"instance", required_argument, NULL, 'O'},
        {"attach",  required_argument, NULL, 'Y'},
        {"telegram", no_argument,      NULL, 'T'},
        {"imessage", no_argument,      NULL, 'I'},
        {"api",      no_argument,      NULL, 'A'},
        {"connect", required_argument, NULL, 'N'},
        {"state",   required_argument, NULL, 'X'},
        {"prompt",  required_argument, NULL, 'p'},
        {"help",    no_argument,       NULL, 'h'},
        {"version", no_argument,       NULL, 'V'},
        {NULL,      0,                 NULL, 0},
    };

    const char *backend = "claude";
    const char *model = NULL;
    const char *effort = NULL;
    const char *dir = NULL;
    const char *name = NULL;
    const char *session_arg = NULL;
    const char *restore_arg = NULL;
    const char *tabs_arg = NULL;
    const char *instance_arg = NULL;
    const char *attach_arg = NULL;
    const char *state_arg = NULL;
    const char *prompt_arg = NULL;
    int telegram = 0;
    int imessage = 0;
    int api_on = 0;
    int pin_backend = 0;
    int fork_session = 0;
    int safe_mode = 0;
    int resume = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "b:m:e:C:p:srhV", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'b': backend = optarg; pin_backend = 1; break;
        case 'm': model = optarg; break;
        case 'e': effort = optarg; break;
        case 'C': dir = optarg; break;
        case 'p': prompt_arg = optarg; break;
        case 'n': name = optarg + (optarg[0] == '@'); break;
        case 's': safe_mode = 1; break;
        case 'r': resume = 1; break;
        case 'S': session_arg = optarg; break;
        case 'F': fork_session = 1; break;
        case 'R': restore_arg = optarg; break;
        case 'B': tabs_arg = optarg; break;
        case 'L': relay_resume(optarg); break;
        case 'O': instance_arg = optarg; break;
        case 'Y': attach_arg = optarg; break;
        case 'T': telegram = 1; break;
        case 'I': imessage = 1; break;
        case 'A': api_on = 1; break;
        case 'X': state_arg = optarg; break;
        case 'V': printf(APP_NAME " %s\n", SCRAP_VERSION); return 0;
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
    if (optind < argc) {
        fprintf(stderr, APP_NAME ": unknown command '%s'\n", argv[optind]);
        return 2;
    }

    if (state_arg) {
        if (!state_enter(state_arg))
            return 1;
    }

    char instance_front[6144], instance_tabs[4200], instance_held[1024];
    const char *front_screen = NULL;
    if (instance_arg) {
        if (session_arg || resume || name || tabs_arg || restore_arg || attach_arg ||
            prompt_arg) {
            fprintf(stderr, APP_NAME ": --instance takes no prompt and does not combine with "
                            "--session, --resume, --name, --attach\n");
            return 2;
        }
        struct tab_args t;
        if (!instance_open(instance_arg, instance_front, sizeof instance_front, &t,
                           instance_tabs, sizeof instance_tabs, instance_held,
                           sizeof instance_held)) {
            fprintf(stderr, APP_NAME ": no saved instance '%s'%s%s\n", instance_arg,
                    instance_held[0] ? "; held by another window: " : "", instance_held);
            return 1;
        }
        if (t.remote) {
            attach_arg = t.remote;
        } else {
            backend = t.backend;
            pin_backend = 1;
            model = t.model;
            effort = t.effort;
            session_arg = t.id;
        }
        dir = t.cwd;
        front_screen = t.screen && *t.screen ? t.screen : NULL;
        if (instance_tabs[0])
            tabs_arg = instance_tabs;
    }

    if (!backend_known(backend)) {
        char choices[128];
        backend_choices(choices, sizeof choices);
        fprintf(stderr, APP_NAME ": unknown backend '%s' — pick one of %s\n", backend, choices);
        return 2;
    }
    if (effort && !strcmp(effort, "default"))
        effort = NULL;
    sessionfork_set_program(argv[0]);

    if (name && (resume || session_arg)) {
        fprintf(stderr, APP_NAME ": --name does not combine with --resume or --session\n");
        return 2;
    }
    if (name && !intercom_name_valid(name)) {
        fprintf(stderr, APP_NAME ": bad name '%s': letters, digits, - and _ only\n", name);
        return 2;
    }

    if (resume && prompt_arg) {
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

    char config[4096];
    int have_config = path_config_dir(config, sizeof config);
    if (have_config) {
        char path[4200];
        snprintf(path, sizeof path, "%s/settings", config);
        settings_open(path);
    }

    if (name && intercom_name_taken(name, NULL)) {
        if (prompt_arg || dir || instance_arg || attach_arg) {
            fprintf(stderr, APP_NAME ": the name '%s' is taken\n", name);
            return 1;
        }
        char *open_argv[] = { "open", (char *)name, NULL };
        return intercom_main(2, open_argv);
    }

    if (!pin_backend)
        backend = cmd_default_backend();

    if (!model)
        model = session_saved_model(backend);
    if (!effort)
        effort = session_saved_effort(backend);

    char pid_env[24];
    snprintf(pid_env, sizeof pid_env, "%ld", (long)getpid());
    setenv("SCRAP_PID", pid_env, 1);

    ui_init();

    image_init();
    image_set_rows(settings_get_int(SETTING_IMAGE_ROWS, IMAGE_ROWS_DEFAULT));

    int interactive = !prompt_arg;

    if ((telegram || imessage || api_on) && !interactive) {
        fprintf(stderr, APP_NAME ": --%s takes no prompt\n",
                telegram ? "telegram" : imessage ? "imessage" : "api");
        return 2;
    }

    if (interactive) {
        restart_arm();
        if (tty_raw_begin() != 0) {
            fprintf(stderr, APP_NAME ": not a terminal — pass a prompt with -p\n");
            return 1;
        }
        atexit(restore_terminal);
        ui_raw(1);
        ui_term_colors();

        if (!restore_arg && tty_input_waiting()) {
            ui_esc("\r");
            ui_esc(UI_ERASE_BELOW);
            ui_flush();
        }

        if (restore_arg) {
            view_collapse(settings_get_int(SETTING_COMPACT, 0));
            viewport_inherit();
            char row[64];
            snprintf(row, sizeof row, "%s\xe2\x9d\xaf%s ", ui_style(UI_ACCENT),
                     ui_style(UI_RESET));
            char *rows[] = { row };
            viewport_chrome(rows, 1, 0, 2);
            scrollback_restore(restore_arg);
            unlink(restore_arg);
        } else {
            viewport_begin();
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
        if (name)
            session_set_name(session, name);
        if (attach_arg)
            session_set_remote(session, attach_arg);
    }

    const char *wanted[] = {telegram ? "telegram" : NULL, api_on ? "api" : NULL};
    for (int i = 0; i < 2 && session; i++) {
        char msg[300];
        if (wanted[i] && !bridges_set(wanted[i], 1, msg, sizeof msg))
            fprintf(stderr, APP_NAME ": %s\n", msg);
    }
    if (imessage && session && im_start(session))
        workspace_republish();

    if (!session) {
        if (interactive)
            restore_terminal();
        fprintf(stderr, APP_NAME ": could not start the %s CLI — is it on PATH?\n", backend);
        return 1;
    }

    if (!interactive) {
        if (!session_start(session)) {
            start_failed(backend);
            session_free(session);
            return 1;
        }
        session_set_quiet(session, 1);
        session_set_naming(session, 0);
        int ok = session_turn(session, prompt_arg);
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
    prompt_set_name_completer(intercom_complete);
    dispatch_serve_with(serve_extra);
    intercom_set_window();
    hub_ensure();
    session_add_listener(stream_event, NULL);
    prompt_file_completion(prompt, cwd);

    session_set_typeahead(prompt_live_key, prompt);
    chrome_bind(prompt);
    chrome_modal_interrupt(handoff_wanted);
    prompt_set_live_command(prompt, live_command, NULL);
    prompt_set_queued_source(prompt, tab_queued, tab_queued_at, tab_unqueue, NULL);
    prompt_set_echo_filter(prompt, echo_filter, NULL);
    prompt_set_idle(prompt, idle_fds, idle_render, idle_poll, NULL);
    prompt_set_restart(prompt, restart_pending, idle_restart, NULL);
    prompt_set_takeover(prompt, takeover_pending, takeover_run, prompt);
    session_on_permission(ask_permission);
    prompt_set_switcher(prompt, switcher, prompt);
    prompt_set_click(prompt, clicked, prompt);
    prompt_set_command(prompt, reply_command, NULL);
    prompt_set_split(prompt, splitter, NULL);
    prompt_set_another(prompt, another, NULL);
    prompt_set_step(prompt, step, NULL);
    prompt_set_busy(prompt, turn_running, NULL);
    prompt_set_cycle(prompt, cycle_session, NULL);
    prompt_set_history_follow(prompt, workspace_history_follow);
    prompt_set_collapse(prompt, collapse_tools, NULL);
    view_collapse(session_compact(session));
    prompt_set_cancel(prompt, cancel_turn, NULL);
    workspace_on_finish(turn_done);

    workspace_on_turn(turn_begin);
    prompt_set_replay(prompt, replay, NULL);
    viewport_on_top(load_earlier);
    prompt_set_blank(prompt, blank_line, NULL);
    vncinset_set_opener(vncsource_open);
    prompt_set_animate(prompt, side_busy, side_tick, NULL);
    viewport_on_cover(inset_cover);
    prompt_set_external(prompt, chat_line, prompt);
    tty_on_focus(focus_changed);
    tty_on_key(stamp_clear);

    voice_init();

    chrome_paint();

    if (tabs_arg)
        tabs_prepare(tabs_arg);

    int picked_bot = pick_startup_bot(session);

    if (!tabs_start(session)) {
        tabs_drop_all();
        chrome_bind(NULL);
        prompt_free(prompt);
        workspace_end();
        restore_terminal();
        start_failed(backend);
        return 1;
    }

    if (restore_arg)
        hud_refresh(session);

    if (front_screen) {
        int shown = scrollback_restore(front_screen);
        unlink(front_screen);
        if (!shown)
            front_screen = NULL;
    }
    if (front_screen)
        hud_refresh(session);
    else if (!resume || !cmd_resume(session))
        hud_print_launch(session);

    if (instance_arg && instance_held[0]) {
        char alert[1100];
        snprintf(alert, sizeof alert, "held by another window, not reopened: %s", instance_held);
        status_set_alert(alert);
    }

    if (tabs_arg) {
        unlink(tabs_arg);
        tabs_admit(0);
    }

    if (!resume && !restore_arg && !front_screen && (session_arg || grokbottail_applies(session)))
        sessionload_into(session);
    if (attach_arg && !restore_arg)
        sessionpresent_replay(session_transcript(session));

    if (api_on && interactive) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        char *conn = api_connect_json();
        ui_note("worker API %s", api_url());
        for (char *line = conn, *nl; line && *line; line = nl ? nl + 1 : NULL) {
            nl = strchr(line, '\n');
            ui_note("%.*s", nl ? (int)(nl - line) : (int)strlen(line), line);
        }
        char reg[512];
        int registered = api_register_dlv(reg, sizeof reg);
        if (registered)
            ui_note("%s", reg);
        if (registered != 1 && conn && copy_to_clipboard(conn))
            ui_note("copied \xc2\xb7 paste it into dlv \xe2\x86\x92 Hosts");
        free(conn);
        viewport_item_end();
        ui_flush();
    }

    if (!picked_bot) {
        prefs_remember_choice("model", "grokbot", session_model_label(session));
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_note("no bot selected, using %s \xc2\xb7 /model to change",
                session_model_label(session));
        viewport_item_end();
        ui_flush();
    }

    for (;;) {
        session = workspace_current();
        if (!session)
            break;

        offer_project_trust(session);
        if (tty_quit_requested())
            break;

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
            tty_watch(workspace_watch_fds, workspace_watch_ready, NULL);
            bash_run(line);
            tty_watch(NULL, NULL, NULL);
            gitinfo_forget();
            free(line);
            prompt_restart_check(prompt);
            continue;
        }

        if (prompt_line_was_external(prompt)) {
            if (im_took) {
                workspace_settle(im_session());
                im_run_line(line);
            } else {
                workspace_settle(tg_session());
                tg_run_line(line);
            }
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
    tabs_admit(1);
    voice_stop();
    tg_stop();
    relay_stop();
    im_stop();
    api_stop();
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
