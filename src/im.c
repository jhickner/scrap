#include "im.h"

#ifdef __APPLE__

#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "bash.h"
#include "chatnav.h"
#include "cmd.h"
#include "filelock.h"
#include "frontend.h"
#include "gitinfo.h"
#include "relay.h"
#include "restart.h"
#include "session.h"
#include "settings.h"
#include "status.h"
#include "text.h"
#include "tg.h"
#include "workspace.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define PREFIX     "agent: "
#define INBOX_MAX  32
#define OUTBOX_MAX 64
#define MAX_ATTACH 8
#define SENT_NAMES 16

extern char **environ;

struct outgoing {
    int   file;
    char *payload;
};

static struct {
    struct chatnav nav;
    int             active;
    char            handle[128];
    char            label[160];
    char            db_path[1024];
    char            attach_dir[64];
    int             wake[2];
    int             kq;
    sqlite3        *db;
    sqlite3_int64   last_row;
    volatile int    stop_wanted;
    volatile int    watcher_stop;
    pthread_t       watcher;
    int             watcher_live;
    int             from_chat;
    char           *last_said;
    char            system_note[2048];

    char           *inbox[INBOX_MAX];
    int             inbox_head, inbox_count;
    pthread_mutex_t inbox_lock;

    struct outgoing outbox[OUTBOX_MAX];
    int             outbox_head, outbox_count;
    int             sender_stop;
    pthread_mutex_t outbox_lock;
    pthread_cond_t  outbox_cond;
    pthread_t       sender;
    int             sender_live;

    char            sent[SENT_NAMES][256];
    int             sent_at;
    pthread_mutex_t sent_lock;
} rt = {
    .wake = {-1, -1},
    .kq = -1,
    .inbox_lock = PTHREAD_MUTEX_INITIALIZER,
    .outbox_lock = PTHREAD_MUTEX_INITIALIZER,
    .outbox_cond = PTHREAD_COND_INITIALIZER,
    .sent_lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct settings imcfg;
static int             owner_lock = -1;
static char            start_error[256];

static const char SEND_SCRIPT[] =
    "on run argv\n"
    "  set theHandle to item 1 of argv\n"
    "  set theKind to item 2 of argv\n"
    "  set thePayload to item 3 of argv\n"
    "  tell application \"Messages\"\n"
    "    set lastErr to \"no iMessage service\"\n"
    "    repeat with s in (every service whose service type = iMessage)\n"
    "      try\n"
    "        set b to participant theHandle of s\n"
    "        if theKind is \"file\" then\n"
    "          send (POSIX file thePayload) to b\n"
    "        else\n"
    "          send thePayload to b\n"
    "        end if\n"
    "        return\n"
    "      on error errm\n"
    "        set lastErr to errm\n"
    "      end try\n"
    "    end repeat\n"
    "    error lastErr\n"
    "  end tell\n"
    "end run\n";

static struct session *current_session(void)
{
    return chatnav_session(&rt.nav);
}

__attribute__((format(printf, 1, 2)))
static void fail_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(start_error, sizeof start_error, fmt, ap);
    va_end(ap);
    fprintf(stderr, APP_NAME ": %s\n", start_error);
}

static void wake_up(void)
{
    if (rt.wake[1] >= 0) {
        char b = 1;
        ssize_t ignored = write(rt.wake[1], &b, 1);
        (void)ignored;
    }
}

static void wake_drain(void)
{
    char buf[64];
    while (rt.wake[0] >= 0 && read(rt.wake[0], buf, sizeof buf) > 0)
        ;
}

static void inbox_push(char *text)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int ok = rt.inbox_count < INBOX_MAX;
    if (ok)
        rt.inbox[(rt.inbox_head + rt.inbox_count++) % INBOX_MAX] = text;
    pthread_mutex_unlock(&rt.inbox_lock);
    if (ok)
        wake_up();
    else
        free(text);
}

static char *inbox_take(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    char *text = NULL;
    if (rt.inbox_count > 0) {
        text = rt.inbox[rt.inbox_head];
        rt.inbox_head = (rt.inbox_head + 1) % INBOX_MAX;
        rt.inbox_count--;
    }
    int remaining = rt.inbox_count;
    pthread_mutex_unlock(&rt.inbox_lock);
    if (text) {
        wake_drain();
        if (remaining)
            wake_up();
    }
    return text;
}

static void inbox_clear(void)
{
    char *t;
    while ((t = inbox_take()) != NULL)
        free(t);
    rt.inbox_head = 0;
}

int im_pending(void)
{
    pthread_mutex_lock(&rt.inbox_lock);
    int n = rt.inbox_count;
    pthread_mutex_unlock(&rt.inbox_lock);
    return n;
}

int im_fds(int *out, int max)
{
    if (!rt.active || max < 1 || rt.wake[0] < 0)
        return 0;
    out[0] = rt.wake[0];
    return 1;
}

static void remember_sent(const char *path)
{
    const char *base = strrchr(path, '/');
    pthread_mutex_lock(&rt.sent_lock);
    snprintf(rt.sent[rt.sent_at], sizeof rt.sent[0], "%s", base ? base + 1 : path);
    rt.sent_at = (rt.sent_at + 1) % SENT_NAMES;
    pthread_mutex_unlock(&rt.sent_lock);
}

static int take_sent(const char *name)
{
    int hit = 0;
    pthread_mutex_lock(&rt.sent_lock);
    for (int i = 0; i < SENT_NAMES && !hit; i++)
        if (rt.sent[i][0] && !strcmp(rt.sent[i], name)) {
            rt.sent[i][0] = '\0';
            hit = 1;
        }
    pthread_mutex_unlock(&rt.sent_lock);
    return hit;
}

static int run_quiet(char *const argv[], const char *input)
{
    int in[2];
    if (pipe(in) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    if (rc != 0) {
        close(in[1]);
        return -1;
    }
    if (input) {
        ssize_t ignored = write(in[1], input, strlen(input));
        (void)ignored;
    }
    close(in[1]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void osa_send(int file, const char *payload)
{
    char staged[1200];
    if (file) {
        const char *home = getenv("HOME");
        const char *base = strrchr(payload, '/');
        snprintf(staged, sizeof staged, "%s/Library/Messages/Attachments/scrap", home ? home : "");
        mkdir(staged, 0700);
        size_t n = strlen(staged);
        snprintf(staged + n, sizeof staged - n, "/%s", base ? base + 1 : payload);
        if (copyfile(payload, staged, NULL, COPYFILE_DATA) == 0)
            payload = staged;
        remember_sent(payload);
    }
    char *argv[] = {"osascript", "-", rt.handle, file ? "file" : "text",
                    (char *)payload, NULL};
    run_quiet(argv, SEND_SCRIPT);
}

static void *sender_thread(void *ud)
{
    (void)ud;
    for (;;) {
        pthread_mutex_lock(&rt.outbox_lock);
        while (!rt.outbox_count && !rt.sender_stop)
            pthread_cond_wait(&rt.outbox_cond, &rt.outbox_lock);
        if (!rt.outbox_count) {
            pthread_mutex_unlock(&rt.outbox_lock);
            return NULL;
        }
        struct outgoing o = rt.outbox[rt.outbox_head];
        rt.outbox_head = (rt.outbox_head + 1) % OUTBOX_MAX;
        rt.outbox_count--;
        pthread_mutex_unlock(&rt.outbox_lock);
        osa_send(o.file, o.payload);
        free(o.payload);
    }
}

static void outbox_push(int file, const char *payload)
{
    char *copy = strdup(payload);
    if (!copy)
        return;
    pthread_mutex_lock(&rt.outbox_lock);
    if (rt.outbox_count < OUTBOX_MAX) {
        rt.outbox[(rt.outbox_head + rt.outbox_count++) % OUTBOX_MAX] =
            (struct outgoing){file, copy};
        copy = NULL;
        pthread_cond_signal(&rt.outbox_cond);
    }
    pthread_mutex_unlock(&rt.outbox_lock);
    free(copy);
}

static void send_text(const char *text)
{
    if (!rt.active || !text)
        return;
    while (*text == '\n' || *text == ' ')
        text++;
    if (!*text)
        return;
    size_t n = strlen(PREFIX) + strlen(text) + 1;
    char *msg = malloc(n);
    if (!msg)
        return;
    snprintf(msg, n, PREFIX "%s", text);
    outbox_push(0, msg);
    free(msg);
}

static void send_reply(const char *text)
{
    char *out = strdup(text);
    if (!out)
        return;
    char *files[MAX_ATTACH];
    int nfiles = 0;
    for (char *p = out; (p = strstr(p, "![")) != NULL;) {
        char *open = strstr(p, "](");
        if (!open)
            break;
        char *close = strchr(open + 2, ')');
        if (!close || open[2] != '/' || close - open > 1024) {
            p = open + 2;
            continue;
        }
        *close = '\0';
        if (nfiles >= MAX_ATTACH || access(open + 2, R_OK) != 0) {
            *close = ')';
            p = close;
            continue;
        }
        files[nfiles++] = strdup(open + 2);
        memmove(p, close + 1, strlen(close + 1) + 1);
    }
    send_text(out);
    for (int i = 0; i < nfiles; i++) {
        if (files[i])
            outbox_push(1, files[i]);
        free(files[i]);
    }
    free(out);
}

static char *attributed_text(const unsigned char *b, int n)
{
    static const char tag[] = "NSString";
    const unsigned char *end = b + n;
    const unsigned char *p = memmem(b, (size_t)n, tag, sizeof tag - 1);
    if (!p)
        return NULL;
    p += sizeof tag - 1;
    const unsigned char *plus = memchr(p, '+', (size_t)(end - p) < 8 ? (size_t)(end - p) : 8);
    if (!plus || plus + 1 >= end)
        return NULL;
    p = plus + 1;
    size_t len = *p++;
    if (len == 0x81) {
        if (p + 2 > end)
            return NULL;
        len = (size_t)p[0] | (size_t)p[1] << 8;
        p += 2;
    } else if (len == 0x82) {
        if (p + 4 > end)
            return NULL;
        len = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16 | (size_t)p[3] << 24;
        p += 4;
    }
    if (len > (size_t)(end - p))
        return NULL;
    return strndup((const char *)p, len);
}

static int is_agent(const char *s)
{
    while (*s == ' ' || *s == '\n')
        s++;
    return !strncasecmp(s, PREFIX, strlen(PREFIX) - 1);
}

static int is_bare_stop(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (strncasecmp(s, "stop", 4))
        return 0;
    for (s += 4; *s; s++)
        if (!strchr(" \t\r\n.!?", *s))
            return 0;
    return 1;
}

static int wait_for_file(const char *path)
{
    for (int i = 0; i < 60 && !rt.watcher_stop; i++) {
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0)
            return 1;
        struct timespec nap = {0, 500 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
    return 0;
}

static char *local_copy(const char *src, sqlite3_int64 row, int k)
{
    const char *dot = strrchr(src, '.');
    if (dot && !strcasecmp(dot, ".heic") && rt.attach_dir[0]) {
        char out[600];
        snprintf(out, sizeof out, "%s/%lld-%d.jpg", rt.attach_dir, (long long)row, k);
        char *argv[] = {"sips", "-s", "format", "jpeg", (char *)src, "--out", out, NULL};
        if (run_quiet(argv, NULL) == 0 && access(out, R_OK) == 0)
            return strdup(out);
    }
    return strdup(src);
}

static int take_attachments(sqlite3_int64 row, char **files)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(rt.db,
            "SELECT a.filename, a.transfer_name FROM attachment a "
            "JOIN message_attachment_join x ON x.attachment_id = a.ROWID "
            "WHERE x.message_id = ?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, row);
    int n = 0;
    while (n < MAX_ATTACH && sqlite3_step(st) == SQLITE_ROW) {
        const char *fn = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        if (!fn || (name && take_sent(name)))
            continue;
        char *path = path_expand_home(fn);
        if (path && wait_for_file(path) && (files[n] = local_copy(path, row, n)) != NULL)
            n++;
        free(path);
    }
    sqlite3_finalize(st);
    return n;
}

static char *compose(char *text, char **files, int nfiles)
{
    if (!nfiles)
        return text;
    size_t n = 64 + (text ? strlen(text) : 0);
    for (int i = 0; i < nfiles; i++)
        n += strlen(files[i]) + 4;
    char *out = malloc(n);
    if (out) {
        size_t at = (size_t)snprintf(out, n, "I sent %s:\n",
                                     nfiles == 1 ? "a file" : "some files");
        for (int i = 0; i < nfiles; i++)
            at += (size_t)snprintf(out + at, n - at, "- %s\n", files[i]);
        if (text && *text)
            snprintf(out + at, n - at, "\n%s", text);
    }
    for (int i = 0; i < nfiles; i++)
        free(files[i]);
    free(text);
    return out;
}

static sqlite3_int64 max_row(void)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 v = 0;
    if (sqlite3_prepare_v2(rt.db, "SELECT MAX(ROWID) FROM message", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void read_new(void)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(rt.db,
            "SELECT m.ROWID, m.text, m.attributedBody, m.cache_has_attachments "
            "FROM message m "
            "JOIN chat_message_join j ON j.message_id = m.ROWID "
            "JOIN chat c ON c.ROWID = j.chat_id "
            "WHERE c.chat_identifier = ? AND m.is_from_me = 0 AND m.ROWID > ? "
            "AND m.item_type = 0 AND m.associated_message_type = 0 "
            "ORDER BY m.ROWID", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, rt.handle, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, rt.last_row);
    sqlite3_int64 seen = rt.last_row;
    while (!rt.watcher_stop && sqlite3_step(st) == SQLITE_ROW) {
        seen = sqlite3_column_int64(st, 0);
        const char *t = (const char *)sqlite3_column_text(st, 1);
        char *text = t && *t ? strdup(t)
                             : attributed_text(sqlite3_column_blob(st, 2),
                                               sqlite3_column_bytes(st, 2));
        if (text) {
            char *obj;
            while ((obj = strstr(text, "\xef\xbf\xbc")) != NULL)
                memmove(obj, obj + 3, strlen(obj + 3) + 1);
        }
        if (text && is_agent(text)) {
            free(text);
            continue;
        }
        if (text && is_bare_stop(text)) {
            free(text);
            rt.stop_wanted = 1;
            wake_up();
            continue;
        }
        char *files[MAX_ATTACH];
        int nfiles = sqlite3_column_int(st, 3) ? take_attachments(seen, files) : 0;
        char *line = compose(text, files, nfiles);
        if (line && *line)
            inbox_push(line);
        else
            free(line);
    }
    sqlite3_finalize(st);
    if (seen > rt.last_row) {
        rt.last_row = seen;
        char path[4200];
        if (path_config_file(path, sizeof path, "imessage-row")) {
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "%lld\n", (long long)seen);
                fclose(f);
            }
        }
    }
}

static int watch_file(const char *suffix, int *fd)
{
    char path[1100];
    snprintf(path, sizeof path, "%s%s", rt.db_path, suffix);
    if (*fd >= 0)
        close(*fd);
    *fd = open(path, O_EVTONLY);
    if (*fd < 0)
        return 0;
    struct kevent ev;
    EV_SET(&ev, *fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    return kevent(rt.kq, &ev, 1, NULL, 0, NULL) == 0;
}

static void *watcher_thread(void *ud)
{
    (void)ud;
    int db_fd = -1, wal_fd = -1;
    watch_file("", &db_fd);
    watch_file("-wal", &wal_fd);
    read_new();
    while (!rt.watcher_stop) {
        struct kevent got[4];
        struct timespec tick = {5, 0};
        int n = kevent(rt.kq, NULL, 0, got, 4, &tick);
        int reopen = 0;
        for (int i = 0; i < n; i++)
            if (got[i].filter == EVFILT_VNODE && got[i].fflags & (NOTE_DELETE | NOTE_RENAME))
                reopen = 1;
        if (reopen || wal_fd < 0) {
            watch_file("", &db_fd);
            watch_file("-wal", &wal_fd);
        }
        if (!rt.watcher_stop)
            read_new();
    }
    if (db_fd >= 0)
        close(db_fd);
    if (wal_fd >= 0)
        close(wal_fd);
    return NULL;
}

static void on_event(void *ud, struct session *s, const backend_event *ev)
{
    (void)ud;
    if (!rt.active || !rt.from_chat || s != current_session())
        return;
    if (ev->kind == BACKEND_EV_ASSISTANT && ev->text && *ev->text) {
        send_reply(ev->text);
        free(rt.last_said);
        rt.last_said = strdup(ev->text);
    } else if (ev->kind == BACKEND_EV_WARNING && ev->text && *ev->text) {
        send_text(ev->text);
    }
}

static int on_abort(void *ud)
{
    const struct session *s = ud;
    if (rt.stop_wanted && (!s || s == current_session())) {
        rt.stop_wanted = 0;
        return 1;
    }
    return 0;
}

const char *im_system_note(void)
{
    snprintf(rt.system_note, sizeof rt.system_note,
        "## This conversation is also on iMessage\n\n"
        "The user is at a terminal, but the same session is reachable from "
        "their phone over iMessage, and your replies to turns they send from "
        "there are texted back to them. iMessage shows plain text: markdown "
        "does not render, so avoid tables and heavy formatting. To send them a "
        "file — a picture, a PDF, anything on this machine — write it as a "
        "markdown image with an absolute local path, ![name](/abs/path), and it "
        "is sent as an attachment. That only sends the file to them; it does "
        "not show it to you. Files they send arrive as local paths; read them "
        "with your tools.\n");
    return rt.system_note;
}

static void bind_session(struct session *s, int active, void *ud)
{
    (void)ud;
    if (active) {
        session_set_abort_hook(s, on_abort, s);
        session_set_system_extra(s, im_system_note());
    } else {
        session_set_abort_hook(s, NULL, NULL);
        session_set_system_extra(s, relay_label() && relay_session() == s
                                      ? relay_system_note() : NULL);
    }
}

static void nav_note(void *ud, const char *text)
{
    (void)ud;
    send_text(text);
}

static void send_turn_reply(int ok)
{
    if (!ok) {
        const char *why = session_last_error(current_session());
        char msg[700];
        snprintf(msg, sizeof msg, "the turn failed%s%s", why ? ": " : "", why ? why : "");
        send_text(msg);
        return;
    }
    const char *reply = session_last_reply(current_session());
    if (session_last_interrupted(current_session()) && (!reply || !*reply)) {
        send_text("(stopped)");
        return;
    }
    if (reply && *reply && (!rt.last_said || strcmp(rt.last_said, reply))) {
        send_reply(reply);
    }
}

void im_run_line(char *line)
{
    free(rt.last_said);
    rt.last_said = NULL;
    rt.from_chat = 1;
    rt.stop_wanted = 0;
    frontend_push(0);

    if (!current_session())
        chatnav_refocus(&rt.nav);
    if (!current_session()) {
        send_text("that session is gone; start a new one at the terminal");
        goto done;
    }

    if (bash_is_command(line)) {
        tty_watch(workspace_watch_fds, workspace_watch_ready, NULL);
        bash_run(line);
        tty_watch(NULL, NULL, NULL);
        gitinfo_forget();
        char *context = bash_take_context();
        if (context) {
            send_text(context);
            send_turn_reply(session_turn(current_session(), context));
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
    if (r != CMD_NOT_A_COMMAND)
        send_text(shown && *shown ? shown : "ok");
    free(shown);

    if (r == CMD_QUIT) {
        send_text("the terminal owns this session; /quit there");
    } else if (r == CMD_NOT_A_COMMAND) {
        status_sticky_prompt(line);
        send_turn_reply(session_turn(current_session(), line));
        cmd_run_deferred(current_session());
    }

done:
    rt.from_chat = 0;
    frontend_pop();
    free(line);
}

static int own_handle(char *out, size_t size)
{
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(rt.db,
            "SELECT substr(account, 3) FROM message WHERE is_from_me = 1 "
            "AND account LIKE 'P:+%' GROUP BY account ORDER BY COUNT(*) DESC LIMIT 1",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out, size, "%s", (const char *)sqlite3_column_text(st, 0));
        ok = 1;
    }
    sqlite3_finalize(st);
    return ok;
}

static sqlite3_int64 saved_row(void)
{
    char path[4200];
    if (!path_config_file(path, sizeof path, "imessage-row"))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    long long v = 0;
    if (fscanf(f, "%lld", &v) != 1)
        v = 0;
    fclose(f);
    return v;
}

static void im_cleanup(void);

int im_start(struct session *s)
{
    start_error[0] = '\0';
    if (rt.active)
        return 0;
    if (tg_label()) {
        fail_note("--imessage and --telegram do not combine");
        return 0;
    }

    char cfgpath[4200];
    int  have = path_config_file(cfgpath, sizeof cfgpath, "imessage");
    settings_load(&imcfg, have ? cfgpath : "");

    owner_lock = have ? filelock_acquire(cfgpath, LOCK_EX | LOCK_NB) : -1;
    if (owner_lock < 0) {
        fail_note("imessage is already enabled by another scrap instance");
        return 0;
    }

    const char *home = getenv("HOME");
    snprintf(rt.db_path, sizeof rt.db_path, "%s/Library/Messages/chat.db", home ? home : "");
    char uri[1100];
    snprintf(uri, sizeof uri, "file:%s?mode=ro", rt.db_path);
    if (sqlite3_open_v2(uri, &rt.db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI |
                        SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK || max_row() <= 0) {
        fail_note("imessage: can't read %s; grant the terminal Full Disk Access", rt.db_path);
        goto fail;
    }
    sqlite3_busy_timeout(rt.db, 2000);

    const char *h = settings_get(&imcfg, "handle", NULL);
    if (h && *h)
        snprintf(rt.handle, sizeof rt.handle, "%s", h);
    else if (!own_handle(rt.handle, sizeof rt.handle)) {
        fail_note("imessage: no phone number found for this account; set `handle` in %s",
                  cfgpath);
        goto fail;
    }

    sqlite3_int64 top = max_row();
    rt.last_row = saved_row();
    if (rt.last_row <= 0 || rt.last_row > top)
        rt.last_row = top;

    if (pipe(rt.wake) != 0 || (rt.kq = kqueue()) < 0) {
        fail_note("imessage: %s", strerror(errno));
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(rt.wake[i], F_SETFL, O_NONBLOCK);
        fcntl(rt.wake[i], F_SETFD, FD_CLOEXEC);
    }
    fcntl(rt.kq, F_SETFD, FD_CLOEXEC);
    struct kevent user;
    EV_SET(&user, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(rt.kq, &user, 1, NULL, 0, NULL);

    snprintf(rt.attach_dir, sizeof rt.attach_dir, "/tmp/" APP_NAME "_im_XXXXXX");
    if (!mkdtemp(rt.attach_dir))
        rt.attach_dir[0] = '\0';

    snprintf(rt.label, sizeof rt.label, "imessage %s", rt.handle);
    chatnav_init(&rt.nav, s, bind_session, NULL);
    const struct chatnav_output out = {.note = nav_note};
    chatnav_set_output(&rt.nav, &out);
    session_set_system_extra(s, im_system_note());
    session_set_abort_hook(s, on_abort, s);
    session_add_listener(on_event, NULL);

    signal(SIGPIPE, SIG_IGN);
    rt.active = 1;
    rt.sender_stop = 0;
    rt.watcher_stop = 0;
    if (pthread_create(&rt.sender, NULL, sender_thread, NULL) != 0)
        goto fail;
    rt.sender_live = 1;
    if (pthread_create(&rt.watcher, NULL, watcher_thread, NULL) != 0)
        goto fail;
    rt.watcher_live = 1;
    restart_flag("--imessage");

    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("%s", rt.label);
    viewport_item_end();
    ui_flush();
    return 1;

fail:
    im_cleanup();
    return 0;
}

static void im_cleanup(void)
{
    rt.active = 0;
    rt.watcher_stop = 1;
    if (rt.watcher_live) {
        struct kevent user;
        EV_SET(&user, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(rt.kq, &user, 1, NULL, 0, NULL);
        pthread_join(rt.watcher, NULL);
        rt.watcher_live = 0;
    }
    if (rt.sender_live) {
        pthread_mutex_lock(&rt.outbox_lock);
        rt.sender_stop = 1;
        pthread_cond_signal(&rt.outbox_cond);
        pthread_mutex_unlock(&rt.outbox_lock);
        pthread_join(rt.sender, NULL);
        rt.sender_live = 0;
    }
    session_remove_listener(on_event, NULL);
    struct session *s = current_session();
    if (s)
        bind_session(s, 0, NULL);
    chatnav_forget(&rt.nav, s);
    if (rt.kq >= 0)
        close(rt.kq);
    rt.kq = -1;
    for (int i = 0; i < 2; i++) {
        if (rt.wake[i] >= 0)
            close(rt.wake[i]);
        rt.wake[i] = -1;
    }
    sqlite3_close(rt.db);
    rt.db = NULL;
    inbox_clear();
    free(rt.last_said);
    rt.last_said = NULL;
    if (rt.attach_dir[0]) {
        rmdir(rt.attach_dir);
        rt.attach_dir[0] = '\0';
    }
    rt.label[0] = rt.handle[0] = '\0';
    rt.from_chat = rt.stop_wanted = 0;
    restart_unflag("--imessage");
    filelock_release(owner_lock);
    owner_lock = -1;
}

void im_stop(void)
{
    if (rt.active || owner_lock >= 0)
        im_cleanup();
}

const char *im_label(void)
{
    return rt.active ? rt.label : NULL;
}

char *im_take_line(void)
{
    return rt.active ? inbox_take() : NULL;
}

struct session *im_session(void)
{
    return current_session();
}

void im_refocus(void)
{
    if (rt.active)
        chatnav_refocus(&rt.nav);
}

void im_forget_session(struct session *s)
{
    chatnav_forget(&rt.nav, s);
}

#else

#include <stdio.h>

#include "app.h"

int im_start(struct session *s)
{
    (void)s;
    fprintf(stderr, APP_NAME ": imessage requires macOS\n");
    return 0;
}

void im_stop(void) {}
const char *im_label(void) { return NULL; }
const char *im_system_note(void) { return NULL; }
int im_fds(int *out, int max) { (void)out; (void)max; return 0; }
int im_pending(void) { return 0; }
char *im_take_line(void) { return NULL; }
void im_run_line(char *line) { (void)line; }
struct session *im_session(void) { return NULL; }
void im_refocus(void) {}
void im_forget_session(struct session *s) { (void)s; }

#endif
