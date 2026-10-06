
#include "tg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TELEGRAM_IMPLEMENTATION
#include "vendor/telegram.h"
#define WHISPER_IMPLEMENTATION
#include "vendor/whisper.h"
#include "vendor/httpd.h"
#include "vendor/mdv2.h"

#include "app.h"
#include "bash.h"
#include "chatnav.h"
#include "cmd.h"
#include "frontend.h"
#include "gitinfo.h"
#include "prompt.h"
#include "relay.h"
#include "reminders.h"
#include "handoff.h"
#include "im.h"
#include "session.h"
#include "settings.h"
#include "sessionlist.h"
#include "sessionview.h"
#include "status.h"
#include "tasks.h"
#include "text.h"
#include "tgartifacts.h"
#include "tgqueue.h"
#include "toolstyle.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define TG_LIMIT    4000
#define MAX_ATTACH  8
#define INBOX_MAX   32
#define MENU_MAX    16

enum { MIRROR_OFF = 0, MIRROR_REMOTE = 1, MIRROR_ALL = 2 };

struct inbox_item {
    char *text;
    int   quiet;
    int   tap;
};

struct tg_menu {
    long serial;
    long message_id;
    char kind[16];
    char payload[MENU_MAX][200];
    char label[MENU_MAX][80];
    int  count;
};

struct tg_receipt {
    long message_id;
    char label[80];
};

struct tg_runtime {
    struct chatnav     nav;
    tg_client          *receiver;
    tg_client          *sender;
    struct tgqueue     *sendq;
    struct tgartifacts *artifacts;
    long                chat_id;
    int                 mirror;
    int                 active;
    volatile int        stop_wanted;
    volatile int        poller_stop;
    pthread_t           poller;
    int                 poller_live;
    int                 wake_pipe[2];
    char                label[96];
    char                attach_dir[64];
    whisper_config      voice;
    int                 voice_set;
    int                 poll_seconds;
    char               *last_said;
    char                last_log[240];
    int                 log_repeats;
    pthread_mutex_t     log_lock;
    int                 from_chat;
    int                 repeat_task;
    time_t              typing_at;
    int                 dispatching_live;
    char                system_note[12288];

    struct inbox_item inbox[INBOX_MAX];
    int inbox_head;
    int inbox_count;
    pthread_mutex_t inbox_lock;

    struct tg_menu menu;
    long           menu_serial;
    struct tg_receipt receipt;
};

static struct tg_runtime runtime = {
    .mirror = MIRROR_ALL,
    .wake_pipe = {-1, -1},
    .poll_seconds = 30,
    .log_lock = PTHREAD_MUTEX_INITIALIZER,
    .inbox_lock = PTHREAD_MUTEX_INITIALIZER,
};

#define rx           runtime.receiver
#define tx           runtime.sender
#define sendq        runtime.sendq
#define artifacts    runtime.artifacts
#define mirror       runtime.mirror
#define running      runtime.active
#define stop_wanted  runtime.stop_wanted
#define poller_stop  runtime.poller_stop
#define poller       runtime.poller
#define poller_live  runtime.poller_live
#define wake         runtime.wake_pipe
#define attach_dir   runtime.attach_dir
#define voice        runtime.voice
#define voice_set    runtime.voice_set
#define poll_seconds runtime.poll_seconds
#define last_said    runtime.last_said
#define last_log     runtime.last_log
#define log_repeats  runtime.log_repeats
#define log_lock     runtime.log_lock
#define from_chat    runtime.from_chat
#define repeat_task  runtime.repeat_task
#define inbox        runtime.inbox
#define inbox_head   runtime.inbox_head
#define inbox_count  runtime.inbox_count
#define inbox_lock   runtime.inbox_lock
#define menu         runtime.menu
#define menu_serial  runtime.menu_serial
#define receipt      runtime.receipt

static struct session *current_session(void)
{
    return chatnav_session(&runtime.nav);
}

static void tg_cleanup(void);

static struct settings tgcfg;
static char            start_error[256];

const char *tg_start_error(void)
{
    return start_error[0] ? start_error : NULL;
}

static const char *cfg_get(const char *key, const char *dflt)
{
    const char *v = settings_get(&tgcfg, key, NULL);
    return (v && *v) ? v : dflt;
}

static long cfg_get_long(const char *key, long dflt)
{
    const char *v = cfg_get(key, NULL);
    return v ? strtol(v, NULL, 10) : dflt;
}

static int state_path(const char *name, char *out, size_t size)
{
    char leaf[64];
    snprintf(leaf, sizeof leaf, "tg-%s", name);
    return path_config_file(out, size, leaf);
}

static char *state_read(const char *name)
{
    char path[4200];
    if (!state_path(name, path, sizeof path))
        return NULL;
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char buf[512] = {0};
    char *r = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!r)
        return NULL;
    text_chomp(buf);
    return buf[0] ? strdup(buf) : NULL;
}

static int write_state(FILE *f, void *ud)
{
    return fprintf(f, "%s\n", ud ? (const char *)ud : "") > 0;
}

static void state_write(const char *name, const char *value)
{
    char path[4200];
    if (!state_path(name, path, sizeof path))
        return;
    text_spit(path, write_state, (void *)(value ? value : ""));
}

static void wake_up(void)
{
    if (wake[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(wake[1], &b, 1);
        (void)ignored;
    }
}

static void wake_drain(void)
{
    char buf[64];
    while (wake[0] >= 0 && read(wake[0], buf, sizeof buf) > 0)
        ;
}

static int inbox_push_kind(char *text, int quiet, int tap)
{
    if (!text)
        return 0;
    pthread_mutex_lock(&inbox_lock);
    int ok = inbox_count < INBOX_MAX;
    if (ok) {
        inbox[(inbox_head + inbox_count) % INBOX_MAX].text = text;
        inbox[(inbox_head + inbox_count) % INBOX_MAX].quiet = quiet;
        inbox[(inbox_head + inbox_count) % INBOX_MAX].tap = tap;
        inbox_count++;
    }
    pthread_mutex_unlock(&inbox_lock);
    if (ok)
        wake_up();
    else
        free(text);
    return ok;
}

static int inbox_push(char *text, int quiet)
{
    return inbox_push_kind(text, quiet, 0);
}

static int inbox_room(void)
{
    pthread_mutex_lock(&inbox_lock);
    int room = inbox_count < INBOX_MAX;
    pthread_mutex_unlock(&inbox_lock);
    return room;
}

static char *inbox_take(int *quiet, int *tap)
{
    pthread_mutex_lock(&inbox_lock);
    char *text = NULL;
    if (inbox_count > 0) {
        text = inbox[inbox_head].text;
        if (quiet)
            *quiet = inbox[inbox_head].quiet;
        if (tap)
            *tap = inbox[inbox_head].tap;
        inbox_head = (inbox_head + 1) % INBOX_MAX;
        inbox_count--;
    }
    int remaining = inbox_count;
    pthread_mutex_unlock(&inbox_lock);
    if (text) {
        wake_drain();
        if (remaining)
            wake_up();
    }
    return text;
}

static char *inbox_take_live(void)
{
    pthread_mutex_lock(&inbox_lock);
    char *text = NULL;
    for (int k = 0; k < inbox_count; k++) {
        int at = (inbox_head + k) % INBOX_MAX;
        if (inbox[at].tap || !cmd_runs_live(inbox[at].text))
            continue;
        text = inbox[at].text;
        for (int j = k; j + 1 < inbox_count; j++)
            inbox[(inbox_head + j) % INBOX_MAX] =
                inbox[(inbox_head + j + 1) % INBOX_MAX];
        inbox_count--;
        break;
    }
    int remaining = inbox_count;
    pthread_mutex_unlock(&inbox_lock);
    if (text) {
        wake_drain();
        if (remaining)
            wake_up();
    }
    return text;
}

int tg_pending(void)
{
    pthread_mutex_lock(&inbox_lock);
    int n = inbox_count;
    pthread_mutex_unlock(&inbox_lock);
    return n;
}

int tg_fds(int *out, int max)
{
    if (!running || max < 1 || wake[0] < 0)
        return 0;
    out[0] = wake[0];
    return 1;
}

static void one_line(char *dst, size_t size, const char *src)
{
    text_trunc(dst, size, src);
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t')
            dst[i] = ' ';
}

static void send_markdown(const char *text)
{
    if (!tx || !runtime.chat_id)
        return;
    if (!text || !*text) {
        tgqueue_push(sendq, TGQUEUE_TEXT, "(done)");
        return;
    }
    size_t n = 0;
    char **msgs = mdv2_messages(text, TG_LIMIT, &n);
    if (!msgs) {
        tgqueue_push(sendq, TGQUEUE_TEXT, text);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (msgs[i] && *msgs[i])
            tgqueue_push(sendq, TGQUEUE_MARKDOWN, msgs[i]);
        free(msgs[i]);
    }
    free(msgs);
}

static void send_pre(const char *text)
{
    if (!tx || !runtime.chat_id || !text)
        return;
    while (*text == '\n')
        text++;
    if (!*text)
        return;
    mdv2_buf b = {0};
    mdv2_puts(&b, "```\n");
    mdv2_esc_code(&b, text, strlen(text));
    mdv2_puts(&b, "\n```");
    if (b.p && b.n < TG_LIMIT)
        tgqueue_push(sendq, TGQUEUE_MARKDOWN, b.p);
    else
        send_markdown(text);
    free(b.p);
}

static void send_note(const char *text)
{
    if (!tx || !runtime.chat_id || !text || !*text)
        return;
    mdv2_buf b = {0};
    mdv2_putc(&b, '_');
    mdv2_esc(&b, text, strlen(text));
    mdv2_putc(&b, '_');
    if (b.p)
        tgqueue_push(sendq, TGQUEUE_MARKDOWN, b.p);
    free(b.p);
}

__attribute__((format(printf, 1, 2)))
static void send_notef(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    send_note(line);
}

static void menu_begin(const char *kind)
{
    menu.count = 0;
    menu.serial = 0;
    menu.message_id = 0;
    snprintf(menu.kind, sizeof menu.kind, "%s", kind);
}

static void menu_add(const char *label, const char *payload)
{
    if (menu.count >= MENU_MAX)
        return;
    snprintf(menu.label[menu.count], sizeof menu.label[0], "%s", label);
    snprintf(menu.payload[menu.count], sizeof menu.payload[0], "%s",
             payload ? payload : "");
    menu.count++;
}

static void menu_send(const char *title, int per_row)
{
    if (!tx || !runtime.chat_id || !menu.count) {
        menu.serial = 0;
        if (title && *title)
            send_note(title);
        return;
    }

    tg_button buttons[MENU_MAX];
    char data[MENU_MAX][32];
    long serial = ++menu_serial;
    for (int i = 0; i < menu.count; i++) {
        snprintf(data[i], sizeof data[0], "%ld:%d", serial, i);
        buttons[i].label = menu.label[i];
        buttons[i].data = data[i];
    }

    tgqueue_hold(sendq);
    long id = tg_send_keyboard(tx, runtime.chat_id, title, 0, buttons, menu.count,
                               per_row);
    tgqueue_release(sendq);
    if (id < 0) {
        menu.serial = 0;
        send_note("could not show the menu");
        return;
    }
    menu.serial = serial;
    menu.message_id = id;
}

static int menu_take(const char *tapped, char *kind, size_t kind_size,
                     char *payload, size_t payload_size)
{
    long serial = 0;
    int  row = -1;
    if (!tapped || sscanf(tapped, "%ld:%d", &serial, &row) != 2)
        return 0;
    if (!menu.serial || serial != menu.serial || row < 0 || row >= menu.count) {
        send_note("that menu is out of date");
        return 0;
    }
    snprintf(kind, kind_size, "%s", menu.kind);
    snprintf(payload, payload_size, "%s", menu.payload[row]);

    if (menu.message_id) {
        receipt.message_id = menu.message_id;
        snprintf(receipt.label, sizeof receipt.label, "%s", menu.label[row]);
    }
    menu.serial = 0;
    menu.count = 0;
    return 1;
}

static void menu_flush(void)
{
    if (!receipt.message_id || !tx)
        return;
    long id = receipt.message_id;
    receipt.message_id = 0;
    tgqueue_hold(sendq);
    tg_edit_message(tx, runtime.chat_id, id, receipt.label, 0, NULL, 0, 0);
    tgqueue_release(sendq);
}

static void send_images(const char *text)
{
    for (const char *p = text; (p = strstr(p, "![")) != NULL;) {
        const char *open = strstr(p, "](");
        if (!open)
            return;
        open += 2;
        const char *close = strchr(open, ')');
        p = open;
        if (!close || *open != '/' || close - open > 1023)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%.*s", (int)(close - open), open);
        if (access(path, R_OK) == 0)
            tgqueue_push(sendq, TGQUEUE_PHOTO, path);
        p = close;
    }
}

static const char *short_path(const char *p)
{
    const char *cwd = current_session() ? session_cwd(current_session()) : NULL;
    size_t n = cwd ? strlen(cwd) : 0;
    if (n && !strncmp(p, cwd, n) && p[n] == '/')
        return p + n + 1;
    return p;
}

static const char *tool_arg(cJSON *in)
{
    return view_tool_arg_value(in);
}

static void tool_line(char *label, size_t ln, char *arg, size_t an,
                      const backend_event *ev)
{
    toolstyle_label(label, ln, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? tool_arg(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    text_trunc(arg, an, v ? v : (ev->input_json ? ev->input_json : ""));
    cJSON_Delete(in);
}

#define DIFF_MAX_LINES 40

static void put_diff_lines(mdv2_buf *b, const char *s, char sign, int *left)
{
    while (s && *s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (*left <= 0) {
            mdv2_puts(b, "...\n");
            return;
        }
        char tmp[512], line[220];
        size_t k = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
        memcpy(tmp, s, k);
        tmp[k] = '\0';
        text_trunc(line, sizeof line, tmp);
        mdv2_putc(b, sign);
        mdv2_esc_code(b, line, strlen(line));
        mdv2_putc(b, '\n');
        (*left)--;
        if (!nl)
            return;
        s = nl + 1;
    }
}

static int send_edit_diff(const char *label, const char *path, cJSON *in)
{
    const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(in, "old_string"));
    const char *nw = strcmp(label, "write")
        ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "new_string"))
        : cJSON_GetStringValue(cJSON_GetObjectItem(in, "content"));
    if ((!old || !*old) && (!nw || !*nw))
        return 0;

    mdv2_buf b = {0};
    mdv2_putc(&b, '_');
    mdv2_esc(&b, label, strlen(label));
    mdv2_putc(&b, '_');
    if (path && *path) {
        mdv2_puts(&b, " `");
        mdv2_esc_code(&b, path, strlen(path));
        mdv2_putc(&b, '`');
    }
    mdv2_puts(&b, "\n```diff\n");
    int lo = (old && *old) ? DIFF_MAX_LINES / 2 : 0, ln = DIFF_MAX_LINES - lo;
    put_diff_lines(&b, old, '-', &lo);
    put_diff_lines(&b, nw, '+', &ln);
    mdv2_puts(&b, "```");
    int ok = b.p && b.n < TG_LIMIT;
    if (ok)
        tgqueue_push(sendq, TGQUEUE_MARKDOWN, b.p);
    free(b.p);
    return ok;
}

static void send_tool_line(const backend_event *ev)
{
    char label[48], raw[600];
    tool_line(label, sizeof label, raw, sizeof raw, ev);
    mdv2_buf b = {0};
    if (*raw && !strcmp(label, "bash")) {
        mdv2_puts(&b, "```bash\n");
        mdv2_esc_code(&b, raw, strlen(raw));
        mdv2_puts(&b, "\n```");
        if (b.p)
            tgqueue_push(sendq, TGQUEUE_MARKDOWN, b.p);
        free(b.p);
        return;
    }
    if (!strcmp(label, "edit") || !strcmp(label, "write") ||
        !strcmp(label, "notebookedit")) {
        cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
        int sent = in ? send_edit_diff(label, raw, in) : 0;
        cJSON_Delete(in);
        if (sent) {
            free(b.p);
            return;
        }
    }
    mdv2_putc(&b, '_');
    mdv2_esc(&b, label, strlen(label));
    mdv2_putc(&b, '_');
    if (*raw) {
        char one[180];
        one_line(one, sizeof one, raw);
        mdv2_puts(&b, " `");
        mdv2_esc_code(&b, one, strlen(one));
        mdv2_putc(&b, '`');
    }
    if (b.p)
        tgqueue_push(sendq, TGQUEUE_MARKDOWN, b.p);
    free(b.p);
}

static void send_task_line(const struct task *a)
{
    char line[240];
    tasks_line(a, line, sizeof line, NULL, NULL);
    send_note(line);
}

__attribute__((format(printf, 4, 5)))
static void appendf(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    *n = (size_t)w < size - *n ? *n + (size_t)w : size - 1;
}

static void send_agents(void)
{
    const struct tasktab *t = session_tasks(current_session());

    if (!tasks_count(t)) {
        send_note("no background agents this session");
        return;
    }
    char msg[2000];
    size_t n = 0;
    time_t now = time(NULL);
    for (int i = 0; i < tasks_count(t); i++) {
        const struct task *a = tasks_at(t, i);
        char took[32], desc[160];
        tasks_duration(took, sizeof took, (long)((a->ended ? a->ended : now) - a->started));
        one_line(desc, sizeof desc, a->desc[0] ? a->desc : a->id);
        appendf(msg, sizeof msg, &n, "%-9s %-6s %s%s%s\n", a->status, took, desc,
                a->type[0] ? "  @" : "", a->type);
        if (a->latest[0]) {
            char one[160];
            one_line(one, sizeof one, a->latest);
            appendf(msg, sizeof msg, &n, "          %s\n", one);
        }
    }
    appendf(msg, sizeof msg, &n, "\n%d running", tasks_running(t));
    if (!t->lifecycle)
        appendf(msg, sizeof msg, &n,
                "\n%s reports no subagent life cycle, so these are the spawn\n"
                "calls seen: what ran, not how it ended.",
                current_session() ? session_backend(current_session()) : "this backend");
    send_pre(msg);
}

static int mirroring(void)
{
    if (!running || !runtime.chat_id)
        return 0;
    if (mirror == MIRROR_OFF)
        return 0;
    return mirror == MIRROR_ALL || from_chat;
}

static void typing(void)
{
    time_t now = time(NULL);
    if (now - runtime.typing_at < 4)
        return;
    runtime.typing_at = now;
    tgqueue_push(sendq, TGQUEUE_ACTION, "typing");
}

static void on_event(void *ud, const backend_event *ev)
{
    (void)ud;

    const struct task *changed = session_task_change(current_session());

    if (ev->kind == BACKEND_EV_TASK) {
        if (session_task_repeat(current_session()))
            repeat_task = 1;
        if (changed && mirroring())
            send_task_line(changed);
        return;
    }

    if (!mirroring())
        return;

    if (repeat_task && !from_chat) {
        if (ev->kind == BACKEND_EV_ASSISTANT)
            repeat_task = 0;
        return;
    }

    switch (ev->kind) {
    case BACKEND_EV_TOOL:
        if (changed) {
            send_task_line(changed);
            break;
        }
        send_tool_line(ev);
        break;

    case BACKEND_EV_ASSISTANT:
        if (!ev->text || !*ev->text)
            break;
        send_markdown(ev->text);
        send_images(ev->text);
        free(last_said);
        last_said = strdup(ev->text);
        break;

    case BACKEND_EV_WARNING:
        if (ev->text && *ev->text)
            send_note(ev->text);
        break;

    default:
        break;
    }
    typing();
}

static void run_live_lines(void)
{
    if (runtime.dispatching_live || !current_session())
        return;
    runtime.dispatching_live = 1;

    char *line;
    while ((line = inbox_take_live()) != NULL) {
        int was = from_chat;
        from_chat = 1;
        frontend_push(0);
        status_pause();
        if (!cmd_self_echoes(line))
            prompt_echo_message(line);

        ui_sink_begin_tee();
        cmd_dispatch_live(current_session(), line);
        char *raw = ui_sink_end();

        status_resume();
        frontend_pop();
        from_chat = was;

        char *shown = ui_plain(raw, 1);
        free(raw);
        if (shown && *shown)
            send_pre(shown);
        else
            send_note("ok");
        free(shown);
        free(line);
    }
    runtime.dispatching_live = 0;
}

static int on_abort(void *ud)
{
    const struct session *s = ud;
    if (mirroring())
        typing();

    if (s == current_session())
        run_live_lines();

    if (stop_wanted && (!s || s == current_session())) {
        stop_wanted = 0;
        return 1;
    }
    return 0;
}

static void on_log(const char *msg)
{
    pthread_mutex_lock(&log_lock);
    if (!strcmp(last_log, msg)) {
        log_repeats++;
    } else {
        snprintf(last_log, sizeof last_log, "%s", msg);
        log_repeats = 0;
    }
    pthread_mutex_unlock(&log_lock);
}

__attribute__((format(printf, 1, 2)))
static void note_up(const char *fmt, ...)
{
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", line);
    viewport_item_end();
    ui_flush();
}

static void artifacts_token(char *out, size_t size)
{
    char *saved = state_read("artifacts_token");
    if (saved && strlen(saved) >= 16) {
        snprintf(out, size, "%s", saved);
        free(saved);
        return;
    }
    free(saved);

    unsigned char raw[8];
    arc4random_buf(raw, sizeof raw);
    char hex[2 * sizeof raw + 1];
    for (size_t i = 0; i < sizeof raw; i++)
        snprintf(hex + i * 2, 3, "%02x", raw[i]);
    snprintf(out, size, "%s", hex);
    state_write("artifacts_token", hex);
}

static void artifacts_init(void)
{
    char art_dir[4096];
    const char *dir = cfg_get("artifacts_dir", NULL);
    if (dir && *dir) {
        snprintf(art_dir, sizeof art_dir, "%s", dir);
    } else {
        if (!path_config_subdir(art_dir, sizeof art_dir, "artifacts"))
            return;
    }
    mkdir(art_dir, 0700);

    int port = (int)cfg_get_long("artifacts_port", 8787);

    const char *bind = cfg_get("artifacts_bind", NULL);
    if (!bind || !*bind)
        bind = httpd_tailscale_ip();
    if (!bind || !*bind)
        bind = "127.0.0.1";

    char token[64];
    artifacts_token(token, sizeof token);

    const char *url = cfg_get("artifacts_url", NULL);
    artifacts = tgartifacts_start(art_dir, bind, port, url, token);
    if (!artifacts) {
        note_up("no artifact server on %s:%d (port taken?)", bind, port);
    }
}

static void send_artifacts(void)
{
    if (!artifacts) {
        send_note("no artifact server running — see the log for why");
        return;
    }
    char msg[1200];
    tgartifacts_list(artifacts, msg, sizeof msg);
    send_pre(msg);
}

const char *tg_system_note(void)
{
    char *note = runtime.system_note;
    size_t n = 0;

    appendf(note, sizeof runtime.system_note, &n,
        "## This conversation is also on Telegram\n\n"
        "The user is at a terminal, but the same session is reachable from "
        "their phone and what you say may be relayed there as chat messages. "
        "Markdown renders; wide tables and long code listings do not. Keep "
        "answers short and say the answer first. To show them a picture — a "
        "render, a screenshot, a photo — write it as a markdown image with an "
        "absolute local path, ![alt](/abs/path.png), and it is sent as a photo; "
        "the file has to be on this machine. That only sends the image to them; "
        "it does not show it to you. To look at an image yourself, read it with "
        "the Read tool first, then write the markdown.\n");

    const char *art_base = tgartifacts_base(artifacts);
    if (art_base)
        appendf(note, sizeof runtime.system_note, &n,
            "\n## Artifacts\n\n"
            "Anything you put in %s is served at %s/<name>, a link the user can "
            "open on their phone. When a turn produces something better seen "
            "than pasted — a PDF, an image, a rendered page, a long report — put "
            "it there and reply with its link. Copying or symlinking the file in "
            "both work. Say what the link is before you send it; a bare URL is "
            "not an answer.\n", tgartifacts_dir(artifacts), art_base);

    appendf(note, sizeof runtime.system_note, &n,
        "\n## Reminders\n\n"
        "You can set reminders for the user. Reminders are JSON lines in the file "
        "%s. Each has `text` plus a schedule -- either a one-shot/interval or a "
        "recurrence rule:\n"
        "  one-shot / interval: {\"at\":\"YYYY-MM-DD HH:MM\",\"text\":\"...\","
        "\"repeat_secs\":0}  (`at` is 24h LOCAL time; repeat_secs>0 re-fires every N "
        "seconds, 0 = one-shot).\n"
        "  recurrence (OMIT `at` -- the daemon computes the next occurrence): "
        "{\"text\":\"...\",\"rule\":{...}} where rule is one of: "
        "{\"kind\":\"daily\",\"time\":\"13:00\"}; "
        "{\"kind\":\"weekdays\",\"time\":\"13:00\"} (Mon-Fri); "
        "{\"kind\":\"weekly\",\"days\":[\"tue\",\"thu\"],\"time\":\"09:00\"}; "
        "{\"kind\":\"nth_weekday\",\"n\":2,\"dow\":\"tue\",\"time\":\"09:00\"} (2nd "
        "Tuesday each month; n 1-5, or -1 for last); "
        "{\"kind\":\"monthly\",\"dom\":15,\"time\":\"09:00\"} (the 15th). `time` is "
        "24h HH:MM. So \"every weekday at 1pm\" -> weekdays/13:00; \"every second "
        "Tuesday\" -> nth_weekday n=2 dow=tue.\n"
        "When the user gives a DAY but no time (\"tomorrow\", \"on Tuesday\"), "
        "default the time to 08:00. To ADD a reminder, append one line to that file "
        "with your tools. To LIST or CANCEL, read/edit that file. The daemon "
        "delivers due reminders and reschedules recurring ones automatically -- do "
        "not deliver them yourself.\n"
        "A reminder's `text` may be an INSTRUCTION to YOU, not just a message: when "
        "it comes due the daemon hands it back to you to act on. Use this for "
        "DYNAMIC reminders that should reflect current data at delivery time rather "
        "than a frozen snapshot -- e.g. store text like \"List the current land "
        "todos: grep the wiki for todos on the land page\" so the list is re-queried "
        "fresh when it fires. IMPORTANT: a fired reminder's reply is delivered "
        "straight to the user over the chat, so DO NOT phrase reminders as "
        "texting/emailing/\"sending\" them anywhere, and never message or email "
        "another PERSON when a reminder fires unless the user's original request "
        "explicitly said to send it to a named person. The default is simply: "
        "surface the info to the user.\n"
        "PROACTIVITY: when he mentions an upcoming trip or event that maps to stored "
        "todos or info (\"I'm going to the land tomorrow\", \"Costco run Saturday\"), "
        "OFFER to schedule a morning reminder that surfaces the relevant items -- ask "
        "first, do NOT auto-create it and do NOT just acknowledge the statement.\n",
        reminders_path());

    return note;
}

static int fire_one_reminder(const char *text, void *ud)
{
    (void)ud;
    if (!inbox_room())
        return 0;

    static const char fmt[] =
        "A scheduled reminder just came due: \"%s\". Deliver it to the user "
        "now -- your reply goes straight to THEM over the chat, so just tell "
        "them (if it's an instruction to gather info, do that and reply with "
        "the result). Do NOT text or email it to anyone, and do NOT "
        "message/email another person unless this reminder explicitly names "
        "sending it to a specific person.";
    int need = snprintf(NULL, 0, fmt, text);
    if (need < 0)
        return 0;
    char *p = malloc((size_t)need + 1);
    if (!p)
        return 0;
    snprintf(p, (size_t)need + 1, fmt, text);
    return inbox_push(p, 0);
}

static void fire_due_reminders(void)
{
    reminders_drain_due(time(NULL), fire_one_reminder, NULL);
}

static int poller_aborting(void)
{
    if (tgqueue_on_sender(sendq))
        return tgqueue_aborted(sendq);
    return poller_stop;
}

static const char *ext_for(const tg_update *u)
{
    const char *dot = u->file_name ? strrchr(u->file_name, '.') : NULL;
    if (dot && strlen(dot) < 8)
        return dot + 1;
    if (u->mime_type) {
        if (!strcmp(u->mime_type, "image/jpeg"))
            return "jpg";
        if (!strcmp(u->mime_type, "image/png"))
            return "png";
        if (!strcmp(u->mime_type, "image/webp"))
            return "webp";
        if (!strcmp(u->mime_type, "application/pdf"))
            return "pdf";
        if (!strncmp(u->mime_type, "audio/", 6))
            return "ogg";
    }
    return "bin";
}

struct incoming {
    char *text;
    char *files[MAX_ATTACH];
    int   nfiles;
};

static void take_file(const tg_update *u, struct incoming *in)
{
    if (in->nfiles >= MAX_ATTACH || !attach_dir[0])
        return;
    char path[600];
    snprintf(path, sizeof path, "%s/%ld.%s", attach_dir, u->update_id, ext_for(u));
    if (tg_download_file(rx, u->file_id, path) != 0)
        return;

    if (u->mime_type && !strncmp(u->mime_type, "audio/", 6)) {
        char *said = whisper_transcribe(path, voice_set ? &voice : NULL);
        remove(path);
        if (said) {
            free(in->text);
            in->text = said;
        }
        return;
    }
    in->files[in->nfiles++] = strdup(path);
}

static char *compose(struct incoming *in)
{
    if (!in->nfiles)
        return in->text ? in->text : strdup("");
    size_t n = 256;
    for (int i = 0; i < in->nfiles; i++)
        n += strlen(in->files[i]) + 8;
    if (in->text)
        n += strlen(in->text);
    char *out = malloc(n);
    if (!out)
        return in->text ? in->text : strdup("");
    size_t at = (size_t)snprintf(out, n, "I sent %s:\n",
                                 in->nfiles == 1 ? "a file" : "some files");
    for (int i = 0; i < in->nfiles; i++) {
        at += (size_t)snprintf(out + at, n - at, "- %s\n", in->files[i]);
        free(in->files[i]);
    }
    if (in->text && *in->text)
        snprintf(out + at, n - at, "\n%s", in->text);
    free(in->text);
    return out;
}

static int is_bare_stop(const char *s)
{
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (strncasecmp(s, "stop", 4) != 0)
        return 0;
    s += 4;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '.' ||
           *s == '!' || *s == '?')
        s++;
    return *s == '\0';
}

static void *poller_thread(void *ud)
{
    (void)ud;
    char *saved = state_read("offset");
    long offset = saved ? strtol(saved, NULL, 10) : 0;
    free(saved);

    while (!poller_stop) {
        fire_due_reminders();

        tg_update *u = NULL;
        int n = tg_get_updates(rx, offset, poll_seconds, &u);
        if (n < 0) {
            for (int i = 0; i < 20 && !poller_stop; i++) {
                struct timespec nap = {0, 100 * 1000 * 1000};
                nanosleep(&nap, NULL);
            }
            continue;
        }

        for (int i = 0; i < n && !poller_stop; i++) {
            offset = u[i].update_id + 1;

            if (u[i].chat_id != runtime.chat_id)
                continue;

            if (u[i].callback_data) {
                tg_answer_callback(rx, u[i].callback_id, NULL);
                char *tapped = strdup(u[i].callback_data);
                if (tapped && !inbox_push_kind(tapped, 0, 1))
                    tg_send_message(rx, runtime.chat_id,
                                    "queue is full; try again shortly");
                continue;
            }

            if (u[i].text && (!strcmp(u[i].text, "/stop") || is_bare_stop(u[i].text))) {
                stop_wanted = 1;
                wake_up();
                continue;
            }

            struct incoming in = {0};
            if (u[i].text)
                in.text = strdup(u[i].text);
            if (u[i].file_id)
                take_file(&u[i], &in);

            while (u[i].media_group_id && i + 1 < n && u[i + 1].media_group_id &&
                   !strcmp(u[i].media_group_id, u[i + 1].media_group_id)) {
                i++;
                offset = u[i].update_id + 1;
                if (u[i].file_id)
                    take_file(&u[i], &in);
                if (!in.text && u[i].text)
                    in.text = strdup(u[i].text);
            }

            char *line = compose(&in);
            if (line && *line && !inbox_push(line, 0))
                tg_send_message(rx, runtime.chat_id,
                                "queue is full; try again shortly");
            else if (line && !*line)
                free(line);
        }
        tg_updates_free(u, n);

        char buf[32];
        snprintf(buf, sizeof buf, "%ld", offset);
        state_write("offset", buf);
    }
    return NULL;
}

static void bind_session(struct session *s, int active, void *ud)
{
    (void)ud;
    session_set_observer(s, active ? on_event : NULL, NULL);
    if (active)
        session_set_abort_hook(s, on_abort, s);
}

static void nav_note(void *ud, const char *text)
{
    (void)ud;
    send_note(text);
}

static void nav_menu_begin(void *ud, const char *kind)
{
    (void)ud;
    menu_begin(kind);
}

static void nav_menu_add(void *ud, const char *label, const char *payload)
{
    (void)ud;
    menu_add(label, payload);
}

static void nav_menu_send(void *ud, const char *title, int per_row)
{
    (void)ud;
    menu_send(title, per_row);
}

void tg_refocus(void)
{
    if (!running)
        return;
    chatnav_refocus(&runtime.nav);
}

static char *menu_line(const char *tapped)
{
    char kind[16], payload[200], line[256];
    if (!menu_take(tapped, kind, sizeof kind, payload, sizeof payload))
        return NULL;

    if (!strcmp(kind, "tab")) {
        if (!strcmp(payload, "new")) {
            snprintf(line, sizeof line, "/open");
        } else if (!strcmp(payload, "resume")) {
            snprintf(line, sizeof line, "/resume");
        } else if (!strcmp(payload, "cancel")) {
            goto nothing;
        } else {
            int at = chatnav_tab_from_payload(payload);
            if (at < 0) {
                send_note("that conversation is gone");
                goto nothing;
            }
            snprintf(line, sizeof line, "/tab %d", at + 1);
        }
    } else if (!strcmp(kind, "resume")) {
        if (!strcmp(payload, "back"))
            snprintf(line, sizeof line, "/tabs");
        else
            snprintf(line, sizeof line, "/resume %s", payload);
    } else {
        goto nothing;
    }
    return strdup(line);

nothing:

    menu_flush();
    return NULL;
}

static void send_bridge_status(void)
{
    char msg[1400];
    size_t n = 0;
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "bridge", runtime.label);
    appendf(msg, sizeof msg, &n, "%-10s %ld\n", "chat", runtime.chat_id);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "mirror",
            mirror == MIRROR_OFF ? "off" : mirror == MIRROR_REMOTE ? "remote" : "all");
    if (workspace_count() > 1)
        appendf(msg, sizeof msg, &n, "%-10s %d of %d\n", "tab",
                workspace_index() + 1, workspace_count());
    appendf(msg, sizeof msg, &n, "%-10s %ds\n", "poll", poll_seconds);
    const char *art_base = tgartifacts_base(artifacts);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "artifacts",
            art_base ? art_base : "not running");
    appendf(msg, sizeof msg, &n, "%-10s %d scheduled\n", "reminders",
            reminders_scheduled_count());
    appendf(msg, sizeof msg, &n, "%-10s %d running, %d this session\n", "agents",
            tasks_running(session_tasks(current_session())), tasks_count(session_tasks(current_session())));
    pthread_mutex_lock(&log_lock);
    if (last_log[0]) {
        char extra[32] = "";
        if (log_repeats)
            snprintf(extra, sizeof extra, " (x%d)", log_repeats + 1);
        appendf(msg, sizeof msg, &n, "%-10s %s%s\n", "last error", last_log, extra);
    }
    pthread_mutex_unlock(&log_lock);
    send_pre(msg);
}

static const char HELP[] =
    "Anything you type is one turn in a live scrap session, including the CLI's "
    "own slash commands (/w, /email, /code-review, ...) and scrap's (/model, "
    "/new, /cd, /backend, /session, ...).\n\n"
    "With a terminal attached these are that window's own tabs: switching "
    "here switches what the terminal shows, and the other way round.\n\n"
    "/tabs        the conversations open here, to switch between\n"
    "             (/sessions is the same list)\n"
    "/open [dir]  another conversation, here or somewhere else\n"
    "/close [n]   close one\n"
    "/resume      reopen a past conversation from this directory\n"
    "/agents      background agents: status, runtime, latest\n"
    "/artifacts   published files and their links\n"
    "/stop        abandon the turn in flight (or just say \"stop\")\n"
    "/tg          the bridge's own settings, and this\n\n"
    "Settings live in ~/.config/scrap/telegram.";

static const char *arg_of(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) || (line[n] && line[n] != ' '))
        return NULL;
    const char *arg = line + n;
    while (*arg == ' ')
        arg++;
    return arg;
}

static int bridge_command(const char *line)
{
    if (!strcmp(line, "/tabs") || !strcmp(line, "/tab") ||
        !strcmp(line, "/sessions")) {
        chatnav_send_tabs(&runtime.nav, MENU_MAX);
        return 1;
    }
    const char *arg = arg_of(line, "/tab");
    if (arg && *arg) {
        chatnav_cmd_switch(&runtime.nav, atoi(arg) - 1);
        return 1;
    }
    if ((arg = arg_of(line, "/open")) != NULL) {
        chatnav_cmd_open(&runtime.nav, *arg ? arg : NULL, NULL);
        return 1;
    }
    if ((arg = arg_of(line, "/close")) != NULL) {
        chatnav_cmd_close(&runtime.nav,
                           *arg ? atoi(arg) - 1 : workspace_index());
        return 1;
    }
    if ((arg = arg_of(line, "/resume")) != NULL) {
        if (*arg) {
            int at = workspace_find_id(arg);
            if (at >= 0)
                chatnav_cmd_switch(&runtime.nav, at);
            else
                chatnav_cmd_open(&runtime.nav, NULL, arg);
        } else {
            chatnav_send_resume(&runtime.nav, MENU_MAX);
        }
        return 1;
    }
    if (!strcmp(line, "/agents")) {
        send_agents();
        return 1;
    }
    if (!strcmp(line, "/artifacts")) {
        send_artifacts();
        return 1;
    }
    if (!strcmp(line, "/tg")) {
        send_bridge_status();
        send_markdown(HELP);
        return 1;
    }
    if (!strcmp(line, "/stop"))
        return 1;
    return 0;
}

static void send_turn_reply(int ok, int quiet)
{
    if (!ok) {
        const char *why = session_last_error(current_session());
        char msg[700];
        snprintf(msg, sizeof msg, "the turn failed%s%s", why ? ":\n" : "",
                 why ? why : "");
        send_markdown(msg);
        return;
    }
    const char *reply = session_last_reply(current_session());
    int nothing_said = (!reply || !*reply) && (!last_said || !*last_said);
    if (quiet && nothing_said)
        return;
    if (session_last_interrupted(current_session()) && (!reply || !*reply)) {
        send_note("(stopped)");
        return;
    }
    if (reply && *reply && (!last_said || strcmp(last_said, reply))) {
        send_markdown(reply);
        send_images(reply);
    }
    int pct = session_context_percent(current_session());
    long window = session_context_window(current_session());
    if (pct > 0 && window > 0)
        send_notef("context %d%% of %ldk", pct, window / 1000);
}

static void run_line(char *line, int quiet)
{
    menu_flush();
    free(last_said);
    last_said = NULL;
    from_chat = 1;
    frontend_push(0);
    repeat_task = 0;
    stop_wanted = 0;

    if (bridge_command(line))
        goto done;

    if (!current_session())
        chatnav_refocus(&runtime.nav);
    if (!current_session()) {
        send_note("that session is gone — start a new one at the terminal");
        goto done;
    }

    if (bash_is_command(line)) {
        tty_watch(workspace_watch_fds, workspace_watch_ready, NULL);
        bash_run(line, NULL);
        tty_watch(NULL, NULL, NULL);
        gitinfo_forget();
        char *context = bash_take_context();
        if (context) {
            send_pre(context);
            send_turn_reply(session_turn(current_session(), context), 0);
            cmd_run_deferred(current_session());
            free(context);
        }
        goto done;
    }

    ui_sink_begin_tee();
    enum cmd_result r = cmd_dispatch(current_session(), line);
    char *raw = ui_sink_end();
    char *shown = ui_plain(raw, 1);
    free(raw);
    if (r != CMD_NOT_A_COMMAND) {
        if (shown && *shown)
            send_pre(shown);
        else
            send_note("ok");
    }
    free(shown);

    if (r == CMD_QUIT) {
        send_note("the terminal owns this session; /quit there");
        goto done;
    }
    if (r == CMD_NOT_A_COMMAND) {
        status_sticky_prompt(line);
        send_turn_reply(session_turn(current_session(), line), quiet);
        cmd_run_deferred(current_session());
    }

done:
    from_chat = 0;
    frontend_pop();
    free(line);
}

static const char *bot_token(void)
{
    const char *var = cfg_get("token_env", "TELEGRAM_TOKEN");
    const char *token = getenv(var);
    if (token && *token)
        return token;
    return cfg_get("token", NULL);
}

int tg_start(struct session *s)
{
    start_error[0] = '\0';
    if (running)
        return 0;
    if (im_label()) {
        snprintf(start_error, sizeof start_error, "--telegram and --imessage do not combine");
        fprintf(stderr, APP_NAME ": %s\n", start_error);
        return 0;
    }

    poller_stop = 0;
    stop_wanted = 0;
    from_chat = 0;
    repeat_task = 0;
    char cfgpath[4200];
    if (path_config_file(cfgpath, sizeof cfgpath, "telegram"))
        settings_load(&tgcfg, cfgpath);
    else
        settings_load(&tgcfg, "");
    const char *token = bot_token();
    if (!token || !*token) {
        char path[4200];
        path_config_file(path, sizeof path, "telegram");
        fprintf(stderr, APP_NAME ": --telegram needs a bot token — $%s, or `token` in %s\n",
                cfg_get("token_env", "TELEGRAM_TOKEN"), path);
        return 0;
    }
    runtime.chat_id = cfg_get_long("chat_id", 0);
    if (!runtime.chat_id) {
        char path[4200];
        path_config_file(path, sizeof path, "telegram");
        fprintf(stderr, APP_NAME ": set chat_id in %s — refusing to serve every chat\n",
                path);
        return 0;
    }


    const char *m = cfg_get("mirror", "remote");
    mirror = !strcmp(m, "off") ? MIRROR_OFF : !strcmp(m, "remote") ? MIRROR_REMOTE
                                                                  : MIRROR_ALL;

    voice.model_path = cfg_get("whisper_model", NULL);
    voice.whisper_bin = cfg_get("whisper_bin", NULL);
    voice.ffmpeg_bin = cfg_get("ffmpeg_bin", NULL);
    voice_set = voice.model_path || voice.whisper_bin || voice.ffmpeg_bin;
    poll_seconds = (int)cfg_get_long("poll_seconds", 30);
    if (poll_seconds < 1 || poll_seconds > 60)
        poll_seconds = 30;

    const char *bot = cfg_get("bot", NULL);
    snprintf(runtime.label, sizeof runtime.label, "telegram%s%s",
             bot ? " " : "", bot ? bot : "");

    rx = tg_new(token);
    tx = tg_new(token);
    if (!rx || !tx) {
        fprintf(stderr, APP_NAME ": telegram init failed\n");
        goto fail;
    }
    if (pipe(wake) != 0) {
        fprintf(stderr, APP_NAME ": telegram wake pipe: %s\n", strerror(errno));
        goto fail;
    }
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    fcntl(wake[1], F_SETFL, O_NONBLOCK);
    fcntl(wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake[1], F_SETFD, FD_CLOEXEC);

    snprintf(attach_dir, sizeof attach_dir, "/tmp/" APP_NAME "_tg_XXXXXX");
    if (!mkdtemp(attach_dir))
        attach_dir[0] = '\0';

    chatnav_init(&runtime.nav, s, bind_session, NULL);
    const struct chatnav_output bridge_output = {
        .note = nav_note,
        .menu_begin = nav_menu_begin,
        .menu_add = nav_menu_add,
        .menu_send = nav_menu_send,
    };
    chatnav_set_output(&runtime.nav, &bridge_output);
    artifacts_init();
    session_set_system_extra(s, tg_system_note());
    session_set_observer(s, on_event, NULL);
    session_set_abort_hook(s, on_abort, s);

    tg_set_abort_check(poller_aborting);
    tg_set_log(on_log);

    signal(SIGPIPE, SIG_IGN);
    running = 1;
    sendq = tgqueue_new(tx, runtime.chat_id);
    if (!sendq) {
        fprintf(stderr, APP_NAME ": can't start the telegram sender\n");
        goto fail;
    }
    if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
        fprintf(stderr, APP_NAME ": can't start the telegram poller\n");
        goto fail;
    }
    poller_live = 1;

    char *note = state_read("restarted");
    if (note && *note) {
        send_note("restarted");
        state_write("restarted", "");
    } else if (cfg_get_long("announce", 0)) {
        char scratch[600];
        snprintf(scratch, sizeof scratch, "%s is live in %s", runtime.label,
                 session_cwd(s));
        send_note(scratch);
    }
    free(note);

    const char *art_base = tgartifacts_base(artifacts);
    note_up("%s, chat %ld%s%s", runtime.label, runtime.chat_id,
            art_base ? ", artifacts on " : "", art_base ? art_base : "");
    return 1;

fail:
    tg_cleanup();
    return 0;
}

const char *tg_label(void)
{
    return running ? runtime.label : NULL;
}

static void inbox_clear(void)
{
    pthread_mutex_lock(&inbox_lock);
    while (inbox_count > 0) {
        free(inbox[inbox_head].text);
        inbox[inbox_head] = (struct inbox_item){0};
        inbox_head = (inbox_head + 1) % INBOX_MAX;
        inbox_count--;
    }
    inbox_head = 0;
    pthread_mutex_unlock(&inbox_lock);
}

static void tg_cleanup(void)
{
    running = 0;
    poller_stop = 1;
    wake_up();
    if (poller_live) {
        pthread_join(poller, NULL);
        poller_live = 0;
    }
    tgqueue_free(sendq);
    sendq = NULL;
    tg_set_abort_check(NULL);
    tg_set_log(NULL);

    tgartifacts_stop(artifacts);
    artifacts = NULL;
    if (wake[0] >= 0)
        close(wake[0]);
    if (wake[1] >= 0)
        close(wake[1]);
    wake[0] = wake[1] = -1;
    tg_free(rx);
    tg_free(tx);
    rx = tx = NULL;
    inbox_clear();
    free(last_said);
    last_said = NULL;
    if (attach_dir[0]) {
        rmdir(attach_dir);
        attach_dir[0] = '\0';
    }
    struct session *s = current_session();
    if (s) {
        session_set_observer(s, NULL, NULL);
        session_set_abort_hook(s, NULL, NULL);
        session_set_system_extra(s, relay_label() && relay_session() == s
                                      ? relay_system_note() : NULL);
    }
    chatnav_forget(&runtime.nav, s);
    runtime.chat_id = 0;
    runtime.label[0] = '\0';
    voice = (whisper_config){0};
    voice_set = 0;
    repeat_task = 0;
    from_chat = 0;
    stop_wanted = 0;
    runtime.typing_at = 0;
    runtime.dispatching_live = 0;
    menu = (struct tg_menu){0};
    receipt = (struct tg_receipt){0};
}

void tg_stop(void)
{
    if (!running && !rx && !tx && wake[0] < 0 && wake[1] < 0 && !sendq &&
        !poller_live && !artifacts)
        return;
    tg_cleanup();
}

char *tg_take_line(void)
{
    if (!running)
        return NULL;
    for (;;) {
        int tap = 0;
        char *line = inbox_take(NULL, &tap);
        if (!line || !tap)
            return line;
        char *cmd = menu_line(line);
        free(line);
        if (cmd)
            return cmd;
    }
}

struct session *tg_session(void)
{
    return current_session();
}

void tg_forget_session(struct session *s)
{
    chatnav_forget(&runtime.nav, s);
}

void tg_run_line(char *line)
{
    run_line(line, 0);
}
