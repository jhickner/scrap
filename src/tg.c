
#include "tg.h"

#include <ctype.h>
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
#define HTTPD_IMPLEMENTATION
#include "vendor/httpd.h"
#include "vendor/mdv2.h"

#include "app.h"
#include "bash.h"
#include "cmd.h"
#include "frontend.h"
#include "gitinfo.h"
#include "reminders.h"
#include "handoff.h"
#include "restart.h"
#include "session.h"
#include "sessionlist.h"
#include "status.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define TG_LIMIT    4000
#define MAX_ATTACH  8
#define INBOX_MAX   32

enum { MIRROR_OFF = 0, MIRROR_REMOTE = 1, MIRROR_ALL = 2 };

static struct session *sess;
static tg_client      *rx;
static tg_client      *tx;
static long            chat_id;
static int             mirror = MIRROR_ALL;
static int             running;
static volatile int    stop_wanted;
static volatile int    poller_stop;
static pthread_t       poller;
static int             wake[2] = {-1, -1};
static char            label[96];
static whisper_config  voice;
static int             voice_set;
static int             poll_seconds = 30;
static char           *last_said;
static char            last_log[240];
static int             log_repeats;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int             from_chat;

#define CFG_MAX 32
static struct { char key[64]; char val[512]; } cfg[CFG_MAX];
static int cfg_count = -1;

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void cfg_load(void)
{
    cfg_count = 0;
    char path[4200];
    if (!path_config_file(path, sizeof path, "telegram"))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[700];
    while (fgets(line, sizeof line, f) && cfg_count < CFG_MAX) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!*k)
            continue;
        snprintf(cfg[cfg_count].key, sizeof cfg[0].key, "%s", k);
        snprintf(cfg[cfg_count].val, sizeof cfg[0].val, "%s", v);
        cfg_count++;
    }
    fclose(f);
}

static const char *cfg_get(const char *key, const char *dflt)
{
    if (cfg_count < 0)
        cfg_load();
    for (int i = 0; i < cfg_count; i++)
        if (!strcmp(cfg[i].key, key) && cfg[i].val[0])
            return cfg[i].val;
    return dflt;
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

static void state_write(const char *name, const char *value)
{
    char path[4200];
    if (!state_path(name, path, sizeof path))
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s\n", value ? value : "");
    fclose(f);
}

struct inbox_item {
    char *text;
    int   quiet;

    int   tap;
};

static struct inbox_item inbox[INBOX_MAX];
static int inbox_head, inbox_count;
static pthread_mutex_t inbox_lock = PTHREAD_MUTEX_INITIALIZER;

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

static void copy_trunc(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i < size - 1; i++)
        dst[i] = src[i];
    if (src && src[i]) {
        for (size_t j = i; j > 0; j--) {
            unsigned char c = (unsigned char)dst[j - 1];
            if ((c & 0xC0) == 0x80)
                continue;
            size_t need = (c & 0x80) == 0    ? 1 : (c & 0xE0) == 0xC0 ? 2
                        : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (j - 1 + need > i)
                i = j - 1;
            break;
        }
        if (i + 4 <= size) {
            memcpy(dst + i, "...", 3);
            i += 3;
        }
    }
    dst[i] = '\0';
}

static void one_line(char *dst, size_t size, const char *src)
{
    copy_trunc(dst, size, src);
    for (size_t i = 0; dst[i]; i++)
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t')
            dst[i] = ' ';
}

static char *strip_ansi(char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s), w = 0;
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t end = ui_esc_span(s, n, i, &kind);
        if (kind == UI_ESC_TEXT)
            for (size_t k = i; k < end; k++)
                if (s[k] != '\r')
                    s[w++] = s[k];
        i = end;
    }
    s[w] = '\0';
    return s;
}

static void send_markdown(const char *text)
{
    if (!tx || !chat_id)
        return;
    if (!text || !*text) {
        tg_send_message(tx, chat_id, "(done)");
        return;
    }
    size_t n = 0;
    char **msgs = mdv2_messages(text, TG_LIMIT, &n);
    if (!msgs) {
        tg_send_message(tx, chat_id, text);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (msgs[i] && *msgs[i])
            tg_send_message_md(tx, chat_id, msgs[i]);
        free(msgs[i]);
    }
    free(msgs);
}

static void send_pre(const char *text)
{
    if (!tx || !chat_id || !text)
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
        tg_send_message_md(tx, chat_id, b.p);
    else
        send_markdown(text);
    free(b.p);
}

static void send_note(const char *text)
{
    if (!tx || !chat_id || !text || !*text)
        return;
    mdv2_buf b = {0};
    mdv2_putc(&b, '_');
    mdv2_esc(&b, text, strlen(text));
    mdv2_putc(&b, '_');
    if (b.p)
        tg_send_message_md(tx, chat_id, b.p);
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

#define MENU_MAX 16

static struct {
    long serial;
    long message_id;
    char kind[16];
    char payload[MENU_MAX][200];
    char label[MENU_MAX][80];
    int  count;
} menu;
static long menu_serial;

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
    if (!tx || !chat_id || !menu.count) {
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

    long id = tg_send_keyboard(tx, chat_id, title, 0, buttons, menu.count, per_row);
    if (id < 0) {
        menu.serial = 0;
        send_note("could not put the menu up");
        return;
    }
    menu.serial = serial;
    menu.message_id = id;
}

static struct {
    long message_id;
    char label[80];
} receipt;

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
    tg_edit_message(tx, chat_id, id, receipt.label, 0, NULL, 0, 0);
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
            tg_send_photo(tx, chat_id, path, NULL);
        p = close;
    }
}

static const char *short_path(const char *p)
{
    const char *cwd = sess ? session_cwd(sess) : NULL;
    size_t n = cwd ? strlen(cwd) : 0;
    if (n && !strncmp(p, cwd, n) && p[n] == '/')
        return p + n + 1;
    return p;
}

static void tool_label(char *dst, size_t size, const char *name)
{
    if (!strncmp(name, "mcp__", 5)) {
        const char *sep = name, *last = name + 5;
        while ((sep = strstr(sep, "__")) != NULL) {
            last = sep + 2;
            sep += 2;
        }
        name = last;
    }
    size_t i = 0;
    for (; name[i] && i < size - 1; i++)
        dst[i] = (char)tolower((unsigned char)name[i]);
    dst[i] = '\0';
}

static const char *tool_arg(cJSON *in)
{
    static const char *const keys[] = {
        "file_path", "notebook_path", "command", "pattern", "query", "url",
        "skill", "description", "prompt", "path", "message", NULL
    };
    for (int i = 0; keys[i]; i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(in, keys[i]));
        if (v && *v)
            return v;
    }
    return NULL;
}

static void tool_line(char *label, size_t ln, char *arg, size_t an,
                      const backend_event *ev)
{
    tool_label(label, ln, ev->name ? ev->name : "tool");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *v = in ? tool_arg(in) : ev->arg;
    if (v && (!strcmp(label, "read") || !strcmp(label, "edit") ||
              !strcmp(label, "write") || !strcmp(label, "notebookedit")))
        v = short_path(v);
    copy_trunc(arg, an, v ? v : (ev->input_json ? ev->input_json : ""));
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
        copy_trunc(line, sizeof line, tmp);
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
        tg_send_message_md(tx, chat_id, b.p);
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
            tg_send_message_md(tx, chat_id, b.p);
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
        tg_send_message_md(tx, chat_id, b.p);
    free(b.p);
}

#define SUBAGENT_MAX 16

struct subagent {
    char   id[40];
    char   desc[140];
    char   type[40];
    char   status[24];
    char   latest[240];
    time_t started, ended;
    int    repeats;
};

static struct subagent agents[SUBAGENT_MAX];
static int agent_count;
static int task_events;
static int repeat_task;

static int subagent_done(const struct subagent *a)
{
    return strcmp(a->status, "running") != 0 && strcmp(a->status, "pending") != 0;
}

static struct subagent *subagent_find(const char *id)
{
    for (int i = 0; i < agent_count; i++)
        if (!strcmp(agents[i].id, id))
            return &agents[i];
    return NULL;
}

static struct subagent *subagent_add(const char *id)
{
    if (agent_count < SUBAGENT_MAX) {
        struct subagent *a = &agents[agent_count++];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    for (int i = 0; i < agent_count; i++) {
        if (!subagent_done(&agents[i]))
            continue;
        memmove(&agents[i], &agents[i + 1],
                (size_t)(agent_count - i - 1) * sizeof agents[0]);
        struct subagent *a = &agents[agent_count - 1];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", id);
        return a;
    }
    return NULL;
}

static int subagents_running(void)
{
    int n = 0;
    for (int i = 0; i < agent_count; i++)
        if (!subagent_done(&agents[i]))
            n++;
    return n;
}

static void human_secs(char *out, size_t size, long secs)
{
    if (secs < 60)
        snprintf(out, size, "%lds", secs);
    else if (secs < 3600)
        snprintf(out, size, "%ldm%02lds", secs / 60, secs % 60);
    else
        snprintf(out, size, "%ldh%02ldm", secs / 3600, (secs % 3600) / 60);
}

static struct subagent *subagent_note(const backend_event *ev)
{
    if (!ev->id || !*ev->id)
        return NULL;
    struct subagent *a = subagent_find(ev->id);
    int fresh = 0;
    if (!a) {
        if (!(a = subagent_add(ev->id)))
            return NULL;
        a->started = time(NULL);
        snprintf(a->status, sizeof a->status, "running");
        fresh = 1;
    }
    if (ev->arg && *ev->arg)
        snprintf(a->type, sizeof a->type, "%s", ev->arg);
    if (ev->text && *ev->text) {
        if (!a->desc[0])
            copy_trunc(a->desc, sizeof a->desc, ev->text);
        else
            copy_trunc(a->latest, sizeof a->latest, ev->text);
    }
    int changed = fresh;

    if (ev->name && *ev->name && strcmp(a->status, ev->name) &&
        !(subagent_done(a) && !strcmp(ev->name, "running"))) {
        snprintf(a->status, sizeof a->status, "%s", ev->name);
        changed = 1;
    }
    if (subagent_done(a) && !a->ended)
        a->ended = time(NULL);
    return changed ? a : NULL;
}

static int is_spawn_tool(const char *name)
{
    static const char *const spawn[] = {
        "task", "agent", "spawn_subagent", "spawn_agent", "workflow", NULL
    };
    char lower[64];
    tool_label(lower, sizeof lower, name);
    for (int i = 0; spawn[i]; i++)
        if (!strcmp(lower, spawn[i]))
            return 1;
    return 0;
}

static struct subagent *subagent_note_launch(const backend_event *ev)
{
    static int seq;
    if (!ev->name || !is_spawn_tool(ev->name))
        return NULL;
    char id[40];
    snprintf(id, sizeof id, "%s-%d", sess ? session_backend(sess) : "agent", ++seq);
    struct subagent *a = subagent_add(id);
    if (!a)
        return NULL;
    a->started = time(NULL);
    snprintf(a->status, sizeof a->status, "launched");
    cJSON *in = ev->input_json ? cJSON_Parse(ev->input_json) : NULL;
    const char *d = in ? tool_arg(in) : NULL;
    copy_trunc(a->desc, sizeof a->desc, d ? d : ev->name);
    cJSON_Delete(in);
    return a;
}

static void send_subagent_line(const struct subagent *a)
{
    char took[32] = "";
    if (subagent_done(a))
        human_secs(took, sizeof took, (long)(a->ended - a->started));
    send_notef("agent %s: %s%s%s", a->status, a->desc[0] ? a->desc : a->id,
               took[0] ? " in " : "", took);
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
    if (!agent_count) {
        send_note("no background agents this session");
        return;
    }
    char msg[2000];
    size_t n = 0;
    time_t now = time(NULL);
    for (int i = 0; i < agent_count; i++) {
        struct subagent *a = &agents[i];
        char took[32], desc[160];
        human_secs(took, sizeof took, (long)((a->ended ? a->ended : now) - a->started));
        one_line(desc, sizeof desc, a->desc[0] ? a->desc : a->id);
        appendf(msg, sizeof msg, &n, "%-9s %-6s %s%s%s\n", a->status, took, desc,
                a->type[0] ? "  @" : "", a->type);
        if (a->latest[0]) {
            char one[160];
            one_line(one, sizeof one, a->latest);
            appendf(msg, sizeof msg, &n, "          %s\n", one);
        }
    }
    appendf(msg, sizeof msg, &n, "\n%d running", subagents_running());
    if (!task_events)
        appendf(msg, sizeof msg, &n,
                "\n%s reports no subagent life cycle, so these are the spawn\n"
                "calls seen: what ran, not how it ended.",
                sess ? session_backend(sess) : "this backend");
    send_pre(msg);
}

static int mirroring(void)
{
    if (!running || !chat_id)
        return 0;
    if (mirror == MIRROR_OFF)
        return 0;
    return mirror == MIRROR_ALL || from_chat;
}

static void typing(void)
{
    static time_t last;
    time_t now = time(NULL);
    if (now - last < 4)
        return;
    last = now;
    tg_send_chat_action(tx, chat_id, "typing");
}

static void on_event(void *ud, const backend_event *ev)
{
    (void)ud;

    if (ev->kind == BACKEND_EV_TASK) {
        task_events = 1;

        struct subagent *prev = subagent_find(ev->id ? ev->id : "");
        int was_done = prev && subagent_done(prev);
        struct subagent *a = subagent_note(ev);
        if (was_done && !a && prev->repeats++ >= 1)
            repeat_task = 1;
        if (a && mirroring())
            send_subagent_line(a);
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
        if (!task_events) {
            struct subagent *a = subagent_note_launch(ev);
            if (a) {
                send_subagent_line(a);
                break;
            }
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

static int on_abort(void *ud)
{
    const struct session *s = ud;
    if (mirroring())
        typing();

    if (stop_wanted && (!s || s == sess)) {
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

static httpd *server;
static char   art_dir[4096];
static char   art_base[300];

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
    const char *dir = cfg_get("artifacts_dir", NULL);
    if (dir && *dir) {
        snprintf(art_dir, sizeof art_dir, "%s", dir);
    } else {
        char base[4096];
        if (!path_config_dir(base, sizeof base))
            return;
        snprintf(art_dir, sizeof art_dir, "%s/artifacts", base);
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

    server = httpd_start(art_dir, bind, port, token);
    if (!server) {
        note_up("no artifact server on %s:%d (port taken?)", bind, port);
        return;
    }

    const char *url = cfg_get("artifacts_url", NULL);
    if (url && *url) {
        char trimmed[256];
        snprintf(trimmed, sizeof trimmed, "%s", url);
        size_t l = strlen(trimmed);
        while (l && trimmed[l - 1] == '/')
            trimmed[--l] = '\0';
        snprintf(art_base, sizeof art_base, "%s/%s", trimmed, token);
    } else {
        snprintf(art_base, sizeof art_base, "http://%s:%d/%s", bind, port, token);
    }
}

struct artifact { char name[256]; time_t mtime; };

static int artifact_newer(const void *a, const void *b)
{
    const struct artifact *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;
}

static void send_artifacts(void)
{
    if (!server) {
        send_note("no artifact server running — see the log for why");
        return;
    }
    char msg[1200];
    size_t n = 0;
    appendf(msg, sizeof msg, &n, "%s/\n", art_base);

    struct artifact ents[64];
    int count = 0;
    DIR *d = opendir(art_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && count < (int)(sizeof ents / sizeof *ents)) {
            if (de->d_name[0] == '.')
                continue;
            char full[4400];
            snprintf(full, sizeof full, "%s/%s", art_dir, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0)
                continue;
            snprintf(ents[count].name, sizeof ents[count].name, "%s", de->d_name);
            ents[count].mtime = st.st_mtime;
            count++;
        }
        closedir(d);
    }
    qsort(ents, (size_t)count, sizeof *ents, artifact_newer);

    if (!count)
        appendf(msg, sizeof msg, &n, "\nnothing published yet");
    for (int i = 0; i < count && i < 10; i++)
        appendf(msg, sizeof msg, &n, "\n%s/%s", art_base, ents[i].name);
    if (count > 10)
        appendf(msg, sizeof msg, &n, "\n… and %d more", count - 10);
    send_pre(msg);
}

const char *tg_system_note(void)
{
    static char note[12288];
    size_t n = 0;

    appendf(note, sizeof note, &n,
        "## This conversation is also on Telegram\n\n"
        "The user is at a terminal, but the same session is reachable from "
        "their phone and what you say may be relayed there as chat messages. "
        "Markdown renders; wide tables and long code listings do not. Keep "
        "answers short and say the answer first. To show them a picture — a "
        "render, a screenshot, a photo — write it as a markdown image with an "
        "absolute local path, ![alt](/abs/path.png), and it is sent as a photo; "
        "the file has to be on this machine.\n");

    if (server && art_base[0])
        appendf(note, sizeof note, &n,
            "\n## Artifacts\n\n"
            "Anything you put in %s is served at %s/<name>, a link the user can "
            "open on their phone. When a turn produces something better seen "
            "than pasted — a PDF, an image, a rendered page, a long report — put "
            "it there and reply with its link. Copying or symlinking the file in "
            "both work. Say what the link is before you send it; a bare URL is "
            "not an answer.\n", art_dir, art_base);

    appendf(note, sizeof note, &n,
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

static void fire_due_reminders(void)
{
    for (int guard = 0; guard < 64; guard++) {
        char rem[2048];
        if (!reminders_pop_due(time(NULL), rem, sizeof rem))
            break;

        static const char fmt[] =
            "A scheduled reminder just came due: \"%s\". Deliver it to the user "
            "now -- your reply goes straight to THEM over the chat, so just tell "
            "them (if it's an instruction to gather info, do that and reply with "
            "the result). Do NOT text or email it to anyone, and do NOT "
            "message/email another person unless this reminder explicitly names "
            "sending it to a specific person.";
        int need = snprintf(NULL, 0, fmt, rem);
        if (need < 0)
            break;
        char *p = malloc((size_t)need + 1);
        if (!p)
            break;
        snprintf(p, (size_t)need + 1, fmt, rem);
        if (!inbox_push(p, 0))
            break;
    }
}

static int poller_aborting(void) { return poller_stop; }

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
    if (in->nfiles >= MAX_ATTACH)
        return;
    char path[600];
    snprintf(path, sizeof path, "/tmp/" APP_NAME "_tg_%ld.%s", u->update_id, ext_for(u));
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

            if (u[i].chat_id != chat_id)
                continue;

            if (u[i].callback_data) {
                tg_answer_callback(rx, u[i].callback_id, NULL);
                char *tapped = strdup(u[i].callback_data);
                if (tapped && !inbox_push_kind(tapped, 0, 1))
                    tg_send_message(rx, chat_id, "queue is full; try again shortly");
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
                tg_send_message(rx, chat_id, "queue is full; try again shortly");
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

static void focus(struct session *s)
{
    if (sess == s)
        return;

    if (sess)
        session_set_observer(sess, NULL, NULL);
    sess = s;
    if (!sess)
        return;
    session_set_observer(sess, on_event, NULL);
    session_set_abort_hook(sess, on_abort, sess);
}

static int tab_switch(int i)
{
    if (i < 0 || i >= workspace_count())
        return 0;
    workspace_show(i);
    focus(workspace_at(i));
    return 1;
}

static int tab_open(const char *cwd, const char *id)
{
    struct session *from = workspace_current();
    if (!from)
        return -1;

    return workspace_spawn(session_backend(from), session_model(from),
                           session_effort(from),
                           cwd && *cwd ? cwd : session_cwd(from), id);
}

static const char *dir_name(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void tab_label(int i, char *out, size_t size)
{
    struct session *s = workspace_at(i);
    const char *title = session_title(s);
    if (!title || !*title)
        title = dir_name(session_cwd(s));

    const char *mark = i == workspace_index()  ? "> "
                     : session_unseen(s) ? "* "
                                         : "  ";
    const char *what = session_turn_running(s) ? "  (working)" : "";
    snprintf(out, size, "%s%d  %s%s", mark, i + 1, title, what);
}

static void tab_payload(int i, char *out, size_t size)
{
    const char *id = session_id(workspace_at(i));
    if (id && *id)
        snprintf(out, size, "%s", id);
    else
        snprintf(out, size, "#%d", i);
}

static int tab_from_payload(const char *payload)
{
    if (!payload || !*payload)
        return -1;
    if (*payload == '#') {
        int at = atoi(payload + 1);
        return at >= 0 && at < workspace_count() ? at : -1;
    }
    return workspace_find_id(payload);
}

static void send_here(void)
{
    struct session *s = sess;
    if (!s)
        return;
    const char *title = session_title(s);
    send_notef("%d/%d  %s  in %s", workspace_index() + 1, workspace_count(),
               title && *title ? title : "untitled", dir_name(session_cwd(s)));
}

static void send_tabs(void)
{
    int n = workspace_count();
    if (n <= 0) {
        send_note("no conversations here");
        return;
    }
    menu_begin("tab");
    for (int i = 0; i < n; i++) {
        char label[80], payload[200];
        tab_label(i, label, sizeof label);
        tab_payload(i, payload, sizeof payload);
        menu_add(label, payload);
    }
    if (n < WORKSPACE_MAX)
        menu_add("+ new conversation", "new");
    menu_add("resume past session", "resume");
    menu_add("cancel", "cancel");
    menu_send("conversations", 1);
}

static void send_resume(void)
{
    if (!sess) {
        send_note("no conversation to resume alongside");
        return;
    }
    const char *backend = session_backend(sess);
    if (!sessionlist_available(backend)) {
        send_notef("%s keeps no list of past conversations", backend);
        return;
    }

    struct past_session *past = NULL;
    int n = sessionlist_load(backend, session_cwd(sess), session_id(sess), &past);
    if (n <= 0) {
        free(past);
        send_note("nothing to resume here");
        return;
    }

    int room = MENU_MAX - 1;
    menu_begin("resume");
    for (int i = 0; i < n && i < room; i++) {
        char label[80];
        snprintf(label, sizeof label, "%s  %s", past[i].when, past[i].label);
        menu_add(label, past[i].id);
    }
    free(past);
    menu_add("< back", "back");
    char title[80];
    snprintf(title, sizeof title, "past conversations here%s",
             n > room ? " (the newest few)" : "");
    menu_send(title, 1);
}

static void open_tab(const char *cwd, const char *id)
{
    if (workspace_count() >= WORKSPACE_MAX) {
        send_notef("that is all %d conversations; close one first",
                   WORKSPACE_MAX);
        return;
    }
    char *expanded = cwd && *cwd ? path_expand_home(cwd) : NULL;
    char  resolved[4096];
    const char *where = NULL;
    if (cwd && *cwd) {
        if (!realpath(expanded ? expanded : cwd, resolved)) {
            send_notef("no such directory: %s", cwd);
            free(expanded);
            return;
        }
        where = resolved;
    }
    free(expanded);

    int at = tab_open(where, id);
    if (at < 0) {
        send_note("could not start another conversation");
        return;
    }
    focus(workspace_at(at));
    send_here();
}

static void close_tab(int at)
{
    if (at < 0 || at >= workspace_count()) {
        send_note("no such conversation");
        return;
    }
    if (workspace_count() == 1) {
        send_note("that is the only conversation; /quit to stop instead");
        return;
    }
    workspace_close(at);

    send_here();
}

static void switch_tab(int at)
{
    if (!tab_switch(at)) {
        send_note("no such conversation");
        return;
    }
    send_here();
}

void tg_refocus(void)
{
    if (!running)
        return;
    focus(workspace_current());
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
            int at = tab_from_payload(payload);
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
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "bridge", label);
    appendf(msg, sizeof msg, &n, "%-10s %ld\n", "chat", chat_id);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "mirror",
            mirror == MIRROR_OFF ? "off" : mirror == MIRROR_REMOTE ? "remote" : "all");
    if (workspace_count() > 1)
        appendf(msg, sizeof msg, &n, "%-10s %d of %d\n", "tab",
                workspace_index() + 1, workspace_count());
    appendf(msg, sizeof msg, &n, "%-10s %ds\n", "poll", poll_seconds);
    appendf(msg, sizeof msg, &n, "%-10s %s\n", "artifacts",
            server ? art_base : "not running");
    appendf(msg, sizeof msg, &n, "%-10s %d scheduled\n", "reminders",
            reminders_scheduled_count());
    appendf(msg, sizeof msg, &n, "%-10s %d running, %d this session\n", "agents",
            subagents_running(), agent_count);
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

static const char *HELP =
    "Anything you type is one turn in a live mux session, including the CLI's "
    "own slash commands (/w, /email, /code-review, ...) and mux's (/model, "
    "/new, /cd, /backend, /session, ...).\n\n"
    "With a terminal attached these are that window's own tabs: switching "
    "here switches what the terminal shows, and the other way round.\n\n"
    "/tabs        the conversations open here, to switch between\n"
    "             (/sessions is the same list)\n"
    "/open [dir]  another conversation, here or somewhere else\n"
    "/close [n]   drop one\n"
    "/resume      reopen a past conversation from this directory\n"
    "/agents      background agents: status, runtime, latest\n"
    "/artifacts   published files and their links\n"
    "/stop        abandon the turn in flight (or just say \"stop\")\n"
    "/tg          the bridge's own settings, and this\n\n"
    "Settings live in ~/.config/mux/telegram.";

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
        send_tabs();
        return 1;
    }
    const char *arg = arg_of(line, "/tab");
    if (arg && *arg) {
        switch_tab(atoi(arg) - 1);
        return 1;
    }
    if ((arg = arg_of(line, "/open")) != NULL) {
        open_tab(*arg ? arg : NULL, NULL);
        return 1;
    }
    if ((arg = arg_of(line, "/close")) != NULL) {
        close_tab(*arg ? atoi(arg) - 1 : workspace_index());
        return 1;
    }
    if ((arg = arg_of(line, "/resume")) != NULL) {
        if (*arg) {
            int at = workspace_find_id(arg);
            if (at >= 0)
                switch_tab(at);
            else
                open_tab(NULL, arg);
        } else {
            send_resume();
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
        const char *why = session_last_error(sess);
        char msg[700];
        snprintf(msg, sizeof msg, "the turn failed%s%s", why ? ":\n" : "",
                 why ? why : "");
        send_markdown(msg);
        return;
    }
    const char *reply = session_last_reply(sess);
    int nothing_said = (!reply || !*reply) && (!last_said || !*last_said);
    if (quiet && nothing_said)
        return;
    if (session_last_interrupted(sess) && (!reply || !*reply)) {
        send_note("(stopped)");
        return;
    }
    if (reply && *reply && (!last_said || strcmp(last_said, reply))) {
        send_markdown(reply);
        send_images(reply);
    }
    int pct = session_context_percent(sess);
    long window = session_context_window(sess);
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

    if (!sess)
        focus(workspace_current());
    if (!sess) {
        send_note("that session is gone — start a new one at the terminal");
        goto done;
    }

    if (bash_is_command(line)) {
        bash_run(line);
        gitinfo_forget();
        char *context = bash_take_context();
        if (context) {
            send_pre(context);
            send_turn_reply(session_turn(sess, context), 0);
            cmd_run_deferred(sess);
            free(context);
        }
        goto done;
    }

    ui_sink_begin_tee();
    enum cmd_result r = cmd_dispatch(sess, line);
    char *shown = ui_sink_end();
    if (r != CMD_NOT_A_COMMAND) {
        strip_ansi(shown);
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
        send_turn_reply(session_turn(sess, line), quiet);
        cmd_run_deferred(sess);
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
    const char *token = bot_token();
    if (!token || !*token) {
        char path[4200];
        path_config_file(path, sizeof path, "telegram");
        fprintf(stderr, APP_NAME ": --telegram needs a bot token — $%s, or `token` in %s\n",
                cfg_get("token_env", "TELEGRAM_TOKEN"), path);
        return 0;
    }
    chat_id = cfg_get_long("chat_id", 0);
    if (!chat_id) {
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
    snprintf(label, sizeof label, "telegram%s%s", bot ? " " : "", bot ? bot : "");

    rx = tg_new(token);
    tx = tg_new(token);
    if (!rx || !tx) {
        fprintf(stderr, APP_NAME ": telegram init failed\n");
        return 0;
    }
    if (pipe(wake) != 0) {
        fprintf(stderr, APP_NAME ": telegram wake pipe: %s\n", strerror(errno));
        return 0;
    }
    fcntl(wake[0], F_SETFL, O_NONBLOCK);
    fcntl(wake[1], F_SETFL, O_NONBLOCK);

    sess = s;
    artifacts_init();
    session_set_system_extra(s, tg_system_note());
    session_set_observer(s, on_event, NULL);
    session_set_abort_hook(s, on_abort, s);

    tg_set_abort_check(poller_aborting);
    tg_set_log(on_log);

    restart_flag("--telegram");
    signal(SIGPIPE, SIG_IGN);
    running = 1;
    if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
        fprintf(stderr, APP_NAME ": can't start the telegram poller\n");
        running = 0;
        return 0;
    }

    char *note = state_read("restarted");
    if (note && *note) {
        send_note("restarted");
        state_write("restarted", "");
    } else if (cfg_get_long("announce", 0)) {
        char scratch[600];
        snprintf(scratch, sizeof scratch, "%s is live in %s", label, session_cwd(s));
        send_note(scratch);
    }
    free(note);

    note_up("%s, chat %ld%s%s", label, chat_id,
            server ? ", artifacts on " : "", server ? art_base : "");
    return 1;
}

const char *tg_label(void)
{
    return running ? label : NULL;
}

void tg_stop(void)
{
    if (!running)
        return;
    running = 0;
    poller_stop = 1;
    wake_up();
    pthread_detach(poller);
    httpd_close(server);
    server = NULL;
    rx = tx = NULL;
    free(last_said);
    last_said = NULL;
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
    return sess;
}

void tg_forget_session(struct session *s)
{
    if (sess == s)
        sess = NULL;
}

void tg_run_line(char *line)
{
    run_line(line, 0);
}
