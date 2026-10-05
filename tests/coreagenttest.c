#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vendor/agents/backend.h"
#include "vendor/agents/core/core.h"
#include "vendor/cJSON.h"

static char dir[256];
static int  failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void send_all(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

static void chunk(int fd, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    send_all(fd, "data: ");
    send_all(fd, s);
    send_all(fd, "\n\n");
    free(s);
    cJSON_Delete(j);
}

static void delta(int fd, const char *key, const char *text)
{
    cJSON *j = cJSON_CreateObject(), *choices = cJSON_AddArrayToObject(j, "choices");
    cJSON *c = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(c, "delta");
    cJSON_AddStringToObject(d, key, text);
    cJSON_AddItemToArray(choices, c);
    chunk(fd, j);
}

static void tool_call(int fd, const char *name, const char *args)
{
    size_t half = strlen(args) / 2;
    for (int part = 0; part < 2; part++) {
        cJSON *j = cJSON_CreateObject(), *choices = cJSON_AddArrayToObject(j, "choices");
        cJSON *c = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(c, "delta");
        cJSON *calls = cJSON_AddArrayToObject(d, "tool_calls"), *tc = cJSON_CreateObject();
        cJSON_AddNumberToObject(tc, "index", 0);
        cJSON *fn = cJSON_AddObjectToObject(tc, "function");
        char piece[4096];
        if (!part) {
            cJSON_AddStringToObject(tc, "id", "call_1");
            cJSON_AddStringToObject(fn, "name", name);
            snprintf(piece, sizeof piece, "%.*s", (int)half, args);
        } else {
            snprintf(piece, sizeof piece, "%s", args + half);
        }
        cJSON_AddStringToObject(fn, "arguments", piece);
        cJSON_AddItemToArray(calls, tc);
        cJSON_AddItemToArray(choices, c);
        chunk(fd, j);
    }
}

static void split_args(const char *rest, const char *const *keys, int n, char *out, size_t size)
{
    cJSON *o = cJSON_CreateObject();
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", rest);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        char *bar = i + 1 < n ? strchr(p, '|') : NULL;
        if (bar)
            *bar = '\0';
        cJSON_AddStringToObject(o, keys[i], p);
        p = bar ? bar + 1 : p + strlen(p);
    }
    char *s = cJSON_PrintUnformatted(o);
    snprintf(out, size, "%s", s);
    free(s);
    cJSON_Delete(o);
}

static int mcp_epoch(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/mcp-epoch", dir);
    FILE *f = fopen(path, "r");
    int n = 1;
    if (f) {
        if (fscanf(f, "%d", &n) != 1)
            n = 1;
        fclose(f);
    }
    return n;
}

static void mcp_tool(cJSON *list, const char *name)
{
    cJSON *t = cJSON_CreateObject(), *schema = cJSON_AddObjectToObject(t, "inputSchema");
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", "test tool");
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToArray(list, t);
}

static void serve_mcp(int fd, const char *req, const char *body_text)
{
    cJSON *body = cJSON_Parse(body_text);
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(body, "method"));
    cJSON *id = cJSON_GetObjectItem(body, "id"), *params = cJSON_GetObjectItem(body, "params");
    char session[64], hdr[256];
    snprintf(session, sizeof session, "s%d", mcp_epoch());
    snprintf(hdr, sizeof hdr, "Mcp-Session-Id: %s", session);
    if (!method)
        method = "";
    if (strcmp(method, "initialize") && !strstr(req, hdr)) {
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        cJSON_Delete(body);
        return;
    }
    if (!id) {
        send_all(fd, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        cJSON_Delete(body);
        return;
    }
    cJSON *reply = cJSON_CreateObject(), *result = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, 1));
    int sse = 0;
    if (!strcmp(method, "initialize")) {
        cJSON_AddStringToObject(result, "protocolVersion", "2025-06-18");
    } else if (!strcmp(method, "tools/list")) {
        cJSON *list = cJSON_AddArrayToObject(result, "tools");
        if (!cJSON_GetObjectItem(params, "cursor")) {
            mcp_tool(list, "echo");
            cJSON_AddStringToObject(result, "nextCursor", "p2");
        } else {
            mcp_tool(list, "sse");
            mcp_tool(list, "fail");
        }
    } else if (!strcmp(method, "tools/call")) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
        const char *text = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(params, "arguments"), "text"));
        char out[512];
        snprintf(out, sizeof out, "%s %s", name ? name : "?", text ? text : "?");
        cJSON *content = cJSON_AddArrayToObject(result, "content"), *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "text");
        cJSON_AddStringToObject(b, "text", out);
        cJSON_AddItemToArray(content, b);
        if (name && !strcmp(name, "fail"))
            cJSON_AddTrueToObject(result, "isError");
        sse = name && !strcmp(name, "sse");
    }
    cJSON_AddItemToObject(reply, "result", result);
    char *s = cJSON_PrintUnformatted(reply);
    char head[512];
    if (sse) {
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n%s\r\n"
                                    "Connection: close\r\n\r\n", hdr);
        send_all(fd, head);
        send_all(fd, "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n");
        send_all(fd, "event: message\ndata: ");
        send_all(fd, s);
        send_all(fd, "\n\n");
    } else {
        snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n%s\r\n"
                                    "Content-Length: %zu\r\nConnection: close\r\n\r\n", hdr, strlen(s));
        send_all(fd, head);
        send_all(fd, s);
    }
    free(s);
    cJSON_Delete(reply);
    cJSON_Delete(body);
}

static void serve_one(int fd)
{
    char *req = NULL;
    size_t len = 0, cap = 0;
    long body_at = -1, want = 0;
    char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0)
            break;
        if (len + (size_t)r + 1 > cap) {
            cap = (len + (size_t)r + 1) * 2;
            req = realloc(req, cap);
        }
        memcpy(req + len, buf, (size_t)r);
        len += (size_t)r;
        req[len] = '\0';
        if (body_at < 0) {
            char *end = strstr(req, "\r\n\r\n");
            if (end) {
                body_at = end + 4 - req;
                char *cl = strcasestr(req, "Content-Length:");
                want = cl ? atol(cl + 15) : 0;
            }
        }
        if (body_at >= 0 && (long)len - body_at >= want)
            break;
    }
    if (body_at < 0) {
        free(req);
        return;
    }
    if (!strncmp(req, "POST /mcp ", 10)) {
        serve_mcp(fd, req, req + body_at);
        free(req);
        return;
    }
    cJSON *body = cJSON_Parse(req + body_at);
    cJSON *msgs = cJSON_GetObjectItem(body, "messages");
    const char *sys = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "content"));
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir,
             sys && !strncmp(sys, "You write the memory", 20) ? "compactor-request.json"
             : sys && strstr(sys, "You are a subagent of scrap")  ? "sub-request.json"
                                                                  : "last-request.json");
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(req + body_at, f);
        fclose(f);
    }

    cJSON *last = cJSON_GetArrayItem(msgs, cJSON_GetArraySize(msgs) - 1);
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(last, "role"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(last, "content"));
    char joined[8192] = "";
    const cJSON *part;
    cJSON_ArrayForEach(part, cJSON_GetObjectItem(last, "content")) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(part, "text"));
        if (t)
            strncat(joined, t, sizeof joined - strlen(joined) - 1);
        content = joined;
    }
    if (!content)
        content = "";
    if (!strncmp(content, "<chat>", 6) && strstr(content, "</chat>\n"))
        content = strstr(content, "</chat>\n") + 8;

    send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
    delta(fd, "reasoning", "hmm");
    long prompt = 100;
    char args[4096], text[4096];
    static const char *const EDIT[] = {"path", "oldText", "newText"};
    static const char *const WRITE[] = {"path", "content"};
    static const char *const ONE_PATH[] = {"path"};
    static const char *const ONE_CMD[] = {"command"};
    if (role && !strcmp(role, "tool")) {
        snprintf(text, sizeof text, "tool said: %.200s", content);
        delta(fd, "content", text);
    } else if (!strncmp(content, "run: ", 5)) {
        split_args(content + 5, ONE_CMD, 1, args, sizeof args);
        tool_call(fd, "bash", args);
    } else if (!strncmp(content, "read: ", 6)) {
        split_args(content + 6, ONE_PATH, 1, args, sizeof args);
        tool_call(fd, "read", args);
    } else if (!strncmp(content, "edit: ", 6)) {
        split_args(content + 6, EDIT, 3, args, sizeof args);
        tool_call(fd, "edit", args);
    } else if (!strncmp(content, "write: ", 7)) {
        split_args(content + 7, WRITE, 2, args, sizeof args);
        tool_call(fd, "write", args);
    } else if (!strncmp(content, "tool: ", 6) && strchr(content, '|')) {
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(strchr(content, '|') - content - 6), content + 6);
        tool_call(fd, name, strchr(content, '|') + 1);
    } else if (!strncmp(content, "mcp: ", 5)) {
        char name[128];
        const char *bar = strchr(content, '|');
        snprintf(name, sizeof name, "mcp__fake__%.*s", bar ? (int)(bar - content - 5) : 0, content + 5);
        static const char *const ONE_TEXT[] = {"text"};
        split_args(bar ? bar + 1 : "", ONE_TEXT, 1, args, sizeof args);
        tool_call(fd, name, args);
    } else if (!strncmp(content, "Summarize the conversation", 26)) {
        delta(fd, "content", "SUMMARY");
    } else if (strstr(content, "Compress this message") && strstr(content, "hang-compactor")) {
        sleep(8);
    } else if (!strcmp(content, "slow")) {
        delta(fd, "content", "partial");
        sleep(5);
        delta(fd, "content", " never");
    } else {
        if (!strcmp(content, "big"))
            prompt = 15000;
        snprintf(text, sizeof text, "echo: %.200s", content);
        delta(fd, "content", text);
    }
    cJSON *u = cJSON_CreateObject(), *usage = cJSON_AddObjectToObject(u, "usage");
    cJSON_AddArrayToObject(u, "choices");
    cJSON_AddNumberToObject(usage, "prompt_tokens", (double)prompt);
    cJSON_AddNumberToObject(usage, "completion_tokens", 5);
    cJSON_AddNumberToObject(usage, "cost", 0.001);
    cJSON *details = cJSON_AddObjectToObject(usage, "prompt_tokens_details");
    cJSON_AddNumberToObject(details, "cached_tokens", 7);
    cJSON_AddNumberToObject(details, "cache_write_tokens", 3);
    chunk(fd, u);
    send_all(fd, "data: [DONE]\n\n");
    cJSON_Delete(body);
    free(req);
}

static pid_t start_server(int *port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16) ||
        getsockname(s, (struct sockaddr *)&a, &al))
        return -1;
    *port = ntohs(a.sin_port);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        for (;;) {
            int c = accept(s, NULL, NULL);
            if (c < 0)
                continue;
            if (fork() == 0) {
                serve_one(c);
                close(c);
                _exit(0);
            }
            close(c);
            while (waitpid(-1, NULL, WNOHANG) > 0) {
            }
        }
    }
    close(s);
    return pid;
}

static void put_file(const char *leaf, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static char *get_file(const char *leaf)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int  tools_seen, results_seen, last_failed, thinking_seen, warnings_seen;
static char last_result[4096], last_tool[64];

static void on_event(void *ud, const backend_event *ev)
{
    (void)ud;
    if (ev->kind == BACKEND_EV_TOOL) {
        tools_seen++;
        snprintf(last_tool, sizeof last_tool, "%s", ev->name);
    } else if (ev->kind == BACKEND_EV_TOOL_RESULT) {
        results_seen++;
        last_failed = ev->failed;
        snprintf(last_result, sizeof last_result, "%s", ev->text ? ev->text : "");
    } else if (ev->kind == BACKEND_EV_THINKING) {
        thinking_seen++;
    } else if (ev->kind == BACKEND_EV_WARNING) {
        warnings_seen++;
    }
}

static long abort_after_ms;
static struct timespec turn_start;

static int should_abort(void)
{
    if (!abort_after_ms)
        return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - turn_start.tv_sec) * 1000 + (now.tv_nsec - turn_start.tv_nsec) / 1000000;
    return ms > abort_after_ms;
}

static Backend *open_agent(const char *resume, int fork_session)
{
    backend_opts o = {.name = "core", .model = "fake/echo", .cwd = dir,
                      .fork_session = fork_session};
    Backend *b = backend_open_ex(&o);
    b->set_event_cb(b, on_event, NULL);
    b->set_abort_check(b, should_abort);
    if (!b->start(b, resume)) {
        fprintf(stderr, "start failed: %s\n", b->last_error(b) ? b->last_error(b) : "?");
        exit(1);
    }
    return b;
}

static char *ask(Backend *b, const char *text, backend_result *meta)
{
    clock_gettime(CLOCK_MONOTONIC, &turn_start);
    return b->ask_ex(b, text, meta);
}

static cJSON *request_messages(cJSON **req)
{
    *req = cJSON_Parse(get_file("last-request.json"));
    return cJSON_GetObjectItem(*req, "messages");
}

static const char *msg_text(cJSON *msgs, int i)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, i), "content"));
}


static const char *arg_after(int argc, char **argv, const char *option)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], option))
            return argv[i + 1];
    return NULL;
}

static void cli_log(const char *tag, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/cli.log", getenv("SCRAP_CONFIG_DIR"));
    FILE *f = fopen(path, "a");
    cJSON *j = cJSON_CreateString(text);
    char *s = cJSON_PrintUnformatted(j);
    fprintf(f, "%s %s\n", tag, s);
    free(s);
    cJSON_Delete(j);
    fclose(f);
}

/* Calls zoom(0, 1) on the memory socket named in --mcp-config. */
static char *cli_zoom(const char *mcp_config)
{
    cJSON *cfg = cJSON_Parse(mcp_config);
    cJSON *args = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cfg, "mcpServers"), "optchat"), "args");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(args, 1)));
    cJSON_Delete(cfg);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof a))
        return strdup("no socket");
    send_all(fd, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n"
                 "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                 "{\"name\":\"zoom\",\"arguments\":{\"id\":0,\"n\":1}}}\n");
    char buf[16384];
    size_t n = 0;
    int lines = 0;
    while (lines < 2 && n < sizeof buf - 1) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++)
            lines += buf[n + i] == '\n';
        n += (size_t)r;
    }
    buf[n] = '\0';
    close(fd);
    char *second = strchr(buf, '\n');
    cJSON *reply = cJSON_Parse(second ? second + 1 : "");
    cJSON *block = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "content"), 0);
    char *out = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(block, "text")) ?: "no reply");
    cJSON_Delete(reply);
    if (!strstr(buf, "\"name\":\"agent\"")) {
        free(out);
        out = strdup("agent tool missing");
    }
    return out;
}

/* Sends a slow agent call, then a zoom, and returns the ids in reply order. */
static char *cli_parallel(const char *mcp_config)
{
    cJSON *cfg = cJSON_Parse(mcp_config);
    cJSON *args = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cfg, "mcpServers"), "optchat"), "args");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", cJSON_GetStringValue(cJSON_GetArrayItem(args, 1)));
    cJSON_Delete(cfg);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof a))
        return strdup("no socket");
    send_all(fd, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                 "{\"name\":\"agent\",\"arguments\":{\"task\":\"SLOW\"}}}\n"
                 "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                 "{\"name\":\"zoom\",\"arguments\":{\"id\":0,\"n\":1}}}\n");
    char buf[16384], out[64] = "order:";
    size_t n = 0;
    int lines = 0;
    while (lines < 2 && n < sizeof buf - 1) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++)
            lines += buf[n + i] == '\n';
        n += (size_t)r;
    }
    buf[n] = '\0';
    close(fd);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        cJSON *j = cJSON_Parse(line);
        snprintf(out + strlen(out), sizeof out - strlen(out), " %d", cJSON_GetObjectItem(j, "id")->valueint);
        cJSON_Delete(j);
    }
    return strdup(out);
}

static void cli_result(const char *text)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "result");
    cJSON_AddStringToObject(j, "subtype", "success");
    cJSON_AddBoolToObject(j, "is_error", 0);
    cJSON_AddStringToObject(j, "result", text);
    char *s = cJSON_PrintUnformatted(j);
    printf("%s\n", s);
    fflush(stdout);
    free(s);
    cJSON_Delete(j);
}

/* Stands in for `claude --print` in memory mode. */
static int mock_claude(int argc, char **argv)
{
    cli_log("SYSTEM", arg_after(argc, argv, "--append-system-prompt") ?: "");
    cli_log("MCP", arg_after(argc, argv, "--mcp-config") ?: "");
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        cJSON *req = cJSON_GetObjectItem(msg, "request");
        if (req && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(req, "subtype")) ?: "", "initialize")) {
            printf("{\"type\":\"control_response\",\"response\":{\"subtype\":\"success\","
                   "\"request_id\":\"claude_h_initialize\",\"response\":{}},\"session_id\":\"s1\"}\n");
            fflush(stdout);
            cJSON_Delete(msg);
            continue;
        }
        cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "message"), "content");
        const char *text = cJSON_IsString(content) ? content->valuestring
                         : cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "text"));
        printf("{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
        if (text && !strcmp(text, "/clear")) {
            cli_log("CLEAR", "");
            cli_result("");
        } else {
            cli_log("PROMPT", text ?: "");
            printf("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":["
                   "{\"type\":\"text\",\"text\":\"cli says hi\"},"
                   "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"echo x\"}}]}}\n"
                   "{\"type\":\"assistant\",\"parent_tool_use_id\":\"t1\",\"message\":{\"role\":\"assistant\","
                   "\"content\":[{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Read\",\"input\":{\"file_path\":\"/sub\"}}]}}\n"
                   "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":["
                   "{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"x-out\"}]}}\n");
            fflush(stdout);
            if (text && strstr(text, "SLOW"))
                usleep(300000);
            const char *mcp = arg_after(argc, argv, "--mcp-config") ?: "{}";
            char *zoom = text && strstr(text, "PAR") ? cli_parallel(mcp) : text && strstr(text, "ZOOM") ? cli_zoom(mcp) : NULL;
            cli_result(zoom ? zoom : "done");
            free(zoom);
        }
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

static void drive_test(void)
{
    char bin[512], self[1024], link[600];
    snprintf(bin, sizeof bin, "%s/bin", dir);
    mkdir(bin, 0700);
    snprintf(link, sizeof link, "%s/claude", bin);
    CHECK(realpath("/proc/self/exe", self) || realpath(getenv("CORETEST_SELF"), self));
    CHECK(symlink(self, link) == 0);
    char path[8192];
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    char home[512];
    snprintf(home, sizeof home, "%s/drive", dir);
    mkdir(home, 0700);
    mkdir(strcat(home, "/agent"), 0700);
    put_file("drive/agent/providers.json", get_file("agent/providers.json"));
    snprintf(home, sizeof home, "%s/drive", dir);
    setenv("SCRAP_CONFIG_DIR", home, 1);

    backend_opts o = {.name = "claude", .cwd = dir, .memory = 1, .memory_relay = "relay"};
    Backend *b = backend_open_ex(&o);
    b->set_event_cb(b, on_event, NULL);
    b->set_abort_check(b, should_abort);
    CHECK(b->start(b, NULL));
    CHECK(!b->session_id(b));
    backend_result meta;
    char *reply = ask(b, "drive one", &meta);
    CHECK(reply && !strcmp(reply, "done"));
    free(reply);
    reply = ask(b, "drive ZOOM", &meta);
    CHECK(reply && !strcmp(reply, "0+0|user: drive one"));
    free(reply);
    reply = ask(b, "drive PAR", &meta);
    CHECK(reply && !strcmp(reply, "order: 3 2"));
    free(reply);
    b->close(b);

    char *log = get_file("drive/cli.log");
    CHECK(strstr(log, "SYSTEM \"You are scrap") && strstr(log, "tools of the optchat MCP server"));
    CHECK(strstr(log, "MCP \"{\\\"mcpServers\\\":{\\\"optchat\\\":{\\\"command\\\":\\\"relay\\\",\\\"args\\\":[\\\"mcp-memory\\\",\\\"/tmp/optchat-"));
    char *first = strstr(log, "PROMPT "), *clear = strstr(log, "CLEAR"), *second = first ? strstr(first + 1, "PROMPT ") : NULL;
    CHECK(first && clear && second && first < clear && clear < second);
    CHECK(first && strstr(first, "\\n</chat>\\ndrive one\""));
    CHECK(second && strstr(second, "|user: drive one\\n") && strstr(second, "|talk: cli says hi\\n") &&
          strstr(second, "|tool: Bash {\\\"command\\\":\\\"echo x\\\"}\\n") && strstr(second, "|echo: x-out\\n"));
    CHECK(!strstr(log, "/sub"));
}

static void memory_test(void)
{
    backend_opts o = {.name = "core", .model = "fake/echo", .cwd = dir, .memory = 1};
    Backend *b = backend_open_ex(&o);
    b->set_event_cb(b, on_event, NULL);
    b->set_abort_check(b, should_abort);
    CHECK(b->start(b, NULL));
    CHECK(!b->session_id(b));
    backend_result meta;
    cJSON *req;

    char *reply = ask(b, "first", &meta);
    CHECK(reply && !strcmp(reply, "echo: first"));
    free(reply);
    cJSON *msgs = request_messages(&req);
    CHECK(cJSON_GetArraySize(msgs) == 2);
    char *sys = strdup(msg_text(msgs, 0) ? msg_text(msgs, 0) : "");
    CHECK(!strstr(sys, "Date:") && strstr(sys, "zoom(id, n)") && strstr(sys, "project rule 42"));
    CHECK(strstr(sys, "to a subagent with agent"));
    CHECK(msg_text(msgs, 1) && !strcmp(msg_text(msgs, 1), "<chat>\n</chat>\nfirst"));
    CHECK(strstr(get_file("last-request.json"), "\"name\":\"zoom\"") &&
          strstr(get_file("last-request.json"), "\"name\":\"date\""));
    cJSON_Delete(req);

    reply = ask(b, "second", &meta);
    free(reply);
    msgs = request_messages(&req);
    CHECK(cJSON_GetArraySize(msgs) == 2);
    CHECK(msg_text(msgs, 0) && !strcmp(msg_text(msgs, 0), sys));
    CHECK(msg_text(msgs, 1) &&
          !strcmp(msg_text(msgs, 1), "<chat>\n0+1|user: first\n1+1|talk: echo: first\n</chat>\nsecond"));
    cJSON_Delete(req);
    free(sys);

    reply = ask(b, "run: head -c 40000 /dev/zero | tr '\\0' x", &meta);
    free(reply);
    msgs = request_messages(&req);
    CHECK(cJSON_GetArraySize(msgs) == 4);
    CHECK(strlen(last_result) > 4000);
    cJSON_Delete(req);

    reply = ask(b, "tool: zoom|{\"id\":0,\"n\":1}", &meta);
    CHECK(!strcmp(last_tool, "zoom") && !last_failed && !strcmp(last_result, "0+0|user: first"));
    free(reply);
    msgs = request_messages(&req);
    CHECK(strstr(msg_text(msgs, 1), "6+1|echo: ") && !strstr(msg_text(msgs, 1), "not summarized"));
    cJSON_Delete(req);
    CHECK(strstr(get_file("compactor-request.json"), "You write the memory of scrap"));

    reply = ask(b, "tool: date|{\"id\":0}", &meta);
    CHECK(!strcmp(last_tool, "date") && !last_failed && !strncmp(last_result, "20", 2) &&
          strchr(last_result, 'T'));
    free(reply);
    reply = ask(b, "tool: zoom|{\"id\":1,\"n\":2}", &meta);
    CHECK(last_failed && !strcmp(last_result, "No line 1+2."));
    free(reply);

    reply = ask(b, "tool: agent|{\"task\":\"run: echo sub\"}", &meta);
    CHECK(!strcmp(last_tool, "agent") && !last_failed && !strcmp(last_result, "tool said: sub\n"));
    CHECK(reply && !strcmp(reply, "tool said: tool said: sub\n"));
    free(reply);
    CHECK(strstr(get_file("last-request.json"), "\"name\":\"agent\""));
    char *sub = get_file("sub-request.json");
    CHECK(strstr(sub, "|tool: agent {") && strstr(sub, "</chat>\\nrun: echo sub") && strstr(sub, "zoom(id, n)"));
    CHECK(!strstr(sub, "\"name\":\"agent\"") && strstr(sub, "\"name\":\"zoom\""));
    CHECK(!strstr(sub, "to a subagent with agent"));

    b->set_model(b, "fake/anthropic/echo");
    reply = ask(b, "blocks", &meta);
    CHECK(reply && !strcmp(reply, "echo: blocks"));
    free(reply);
    CHECK(strstr(get_file("last-request.json"),
                 "{\"type\":\"text\",\"text\":\"blocks\",\"cache_control\":{\"type\":\"ephemeral\"}}"));
    char hang[700];
    snprintf(hang, sizeof hang, "hang-compactor %0600d", 0);
    reply = ask(b, hang, &meta);
    free(reply);
    time_t t0 = time(NULL);
    b->close(b);
    CHECK(time(NULL) - t0 < 5);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "cat %s/memory/main/*.jsonl > %s/log.jsonl", dir, dir);
    CHECK(system(cmd) == 0);
    char *log = strdup(get_file("log.jsonl")), kinds[512] = "";
    CHECK(!strstr(log, "hmm"));
    for (char *line = strtok(log, "\n"); line; line = strtok(NULL, "\n")) {
        cJSON *j = cJSON_Parse(line);
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
        strncat(kinds, kind ? kind : "?", sizeof kinds - strlen(kinds) - 2);
        strcat(kinds, " ");
        if (kind && !strcmp(kind, "echo") && text && strlen(text) > 1000)
            CHECK(strlen(text) < 30100 && strstr(text, "bytes cut]"));
        if (kind && !strcmp(kind, "tool") && text && strstr(text, "head -c"))
            CHECK(strstr(text, "bash {\"command\":\"head") == text);
        cJSON_Delete(j);
    }
    CHECK(!strcmp(kinds, "user talk user talk user tool echo talk user tool echo talk user tool echo talk "
                         "user tool echo talk user tool echo talk user talk user talk "));
    if (strcmp(kinds, "user talk user talk user tool echo talk user tool echo talk user tool echo talk "
                      "user tool echo talk user tool echo talk user talk user talk "))
        fprintf(stderr, "kinds: %s\n", kinds);
    free(log);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--print"))
        return mock_claude(argc, argv);
    setenv("CORETEST_SELF", argv[0], 1);
    snprintf(dir, sizeof dir, "/tmp/coreagenttest.%d", (int)getpid());
    mkdir(dir, 0700);
    setenv("SCRAP_CONFIG_DIR", dir, 1);
    int port;
    pid_t server = start_server(&port);
    CHECK(server > 0);

    char config[1024];
    mkdir(strcat(strcpy(config, dir), "/agent"), 0700);
    snprintf(config, sizeof config,
             "{\"default\":\"fake/echo\",\"providers\":{\"fake\":{\"base_url\":"
             "\"http://127.0.0.1:%d/v1\",\"effort\":\"openai\",\"list\":false,"
             "\"models\":{\"echo\":{\"context\":20000}}}},\"compactor_backend\":\"core\",\"compactor_model\":\"fake/echo\"}",
             port);
    put_file("agent/providers.json", config);
    put_file("hooks.json",
             "{\"hooks\":{\"PreToolUse\":[{\"matcher\":\"Bash\",\"hooks\":[{\"type\":\"command\","
             "\"command\":\"grep -q forbidden && { echo nope >&2; exit 2; }; exit 0\"}]}]}}");
    put_file("AGENTS.md", "project rule 42");

    Backend *b = open_agent(NULL, 0);
    backend_result meta;
    char *reply = ask(b, "hello", &meta);
    CHECK(reply && !strcmp(reply, "echo: hello"));
    CHECK(thinking_seen > 0);
    CHECK(meta.context_tokens == 105 && meta.context_window == 20000);
    CHECK(meta.cost_usd > 0.0009);
    CHECK(strstr(get_file("last-request.json"), "project rule 42") != NULL);
    free(reply);

    char id[128];
    snprintf(id, sizeof id, "%s", b->session_id(b));
    char sessdir[1024], sessfile[1200];
    core_agent_session_dir(dir, sessdir, sizeof sessdir);
    snprintf(sessfile, sizeof sessfile, "%s/%s.jsonl", sessdir, id);
    struct stat st;
    CHECK(stat(sessfile, &st) == 0);

    reply = ask(b, "run: echo hi", &meta);
    CHECK(tools_seen == 1 && !strcmp(last_tool, "bash") && !strcmp(last_result, "hi\n"));
    CHECK(reply && !strcmp(reply, "tool said: hi\n"));
    free(reply);

    reply = ask(b, "run: echo forbidden", &meta);
    CHECK(last_failed && strstr(last_result, "blocked by hook: nope"));
    free(reply);

    reply = ask(b, "write: sub/f.txt|alpha beta alpha", &meta);
    CHECK(!last_failed && !strcmp(get_file("sub/f.txt"), "alpha beta alpha"));
    free(reply);
    reply = ask(b, "edit: sub/f.txt|alpha|gamma", &meta);
    CHECK(last_failed && strstr(last_result, "matches 2 places"));
    free(reply);
    reply = ask(b, "edit: sub/f.txt|beta|delta", &meta);
    CHECK(!last_failed && !strcmp(get_file("sub/f.txt"), "alpha delta alpha"));
    free(reply);
    reply = ask(b, "read: sub/f.txt", &meta);
    CHECK(!last_failed && !strcmp(last_result, "alpha delta alpha\n"));
    free(reply);

    reply = ask(b, "plain" BACKEND_CACHE_MARK "route", &meta);
    CHECK(reply && !strcmp(reply, "echo: plainroute"));
    CHECK(!strstr(get_file("last-request.json"), "cache_control"));
    CHECK(meta.cache_read_tokens == 7 && meta.cache_creation_tokens == 3);
    free(reply);
    b->set_model(b, "fake/anthropic/echo");
    reply = ask(b, "view" BACKEND_CACHE_MARK "step", &meta);
    char *creq = get_file("last-request.json");
    CHECK(strstr(creq, "{\"type\":\"text\",\"text\":\"view\",\"cache_control\":{\"type\":\"ephemeral\"}}"));
    CHECK(strstr(creq, "{\"type\":\"text\",\"text\":\"step\",\"cache_control\":{\"type\":\"ephemeral\"}}"));
    CHECK(!strstr(creq, BACKEND_CACHE_MARK));
    free(reply);
    reply = ask(b, "run: echo cached", &meta);
    creq = get_file("last-request.json");
    CHECK(strstr(creq, "\"role\":\"tool\""));
    CHECK(strstr(creq, "{\"type\":\"text\",\"text\":\"cached\\n\",\"cache_control\":{\"type\":\"ephemeral\"}}"));
    free(reply);
    b->set_model(b, "fake/echo");

    b->set_effort(b, "high");
    reply = ask(b, "effort", &meta);
    CHECK(strstr(get_file("last-request.json"), "\"reasoning_effort\":\"high\"") != NULL);
    free(reply);

    abort_after_ms = 300;
    reply = ask(b, "run: sleep 10", &meta);
    CHECK(meta.interrupted);
    free(reply);
    reply = ask(b, "slow", &meta);
    CHECK(meta.interrupted && reply && !strcmp(reply, "partial"));
    free(reply);
    abort_after_ms = 0;

    reply = ask(b, "after abort", &meta);
    CHECK(reply && !strcmp(reply, "echo: after abort"));
    free(reply);
    b->close(b);

    b = open_agent(id, 0);
    CHECK(!strcmp(b->session_id(b), id));
    reply = ask(b, "resumed", &meta);
    char *req = get_file("last-request.json");
    CHECK(strstr(req, "run: echo hi") && strstr(req, "\"tool_call_id\":\"call_1\""));
    free(reply);
    b->close(b);

    b = open_agent(id, 1);
    CHECK(strcmp(b->session_id(b), id) != 0);
    reply = ask(b, "big", &meta);
    free(reply);
    warnings_seen = 0;
    reply = ask(b, "after big", &meta);
    CHECK(warnings_seen > 0);
    req = get_file("last-request.json");
    CHECK(strstr(req, "SUMMARY") && !strstr(req, "run: echo hi"));
    CHECK(reply && !strcmp(reply, "echo: after big"));
    free(reply);
    b->close(b);

    b = open_agent(NULL, 0);
    CHECK(b->reset(b));
    b->close(b);

    snprintf(config, sizeof config,
             "{\"servers\":{\"fake\":{\"url\":\"http://127.0.0.1:%d/mcp\"},"
             "\"down\":{\"url\":\"http://127.0.0.1:1/mcp\"}}}",
             port);
    put_file("agent/mcp.json", config);
    warnings_seen = 0;
    b = open_agent(NULL, 0);
    CHECK(warnings_seen == 1);
    reply = ask(b, "tools", &meta);
    req = get_file("last-request.json");
    CHECK(strstr(req, "\"mcp__fake__echo\"") && strstr(req, "\"mcp__fake__sse\"") &&
          strstr(req, "\"mcp__fake__fail\""));
    free(reply);
    reply = ask(b, "mcp: echo|hi", &meta);
    CHECK(!strcmp(last_tool, "mcp__fake__echo") && !last_failed && !strcmp(last_result, "echo hi"));
    CHECK(reply && !strcmp(reply, "tool said: echo hi"));
    free(reply);
    reply = ask(b, "mcp: sse|there", &meta);
    CHECK(!last_failed && !strcmp(last_result, "sse there"));
    free(reply);
    reply = ask(b, "mcp: fail|x", &meta);
    CHECK(last_failed && !strcmp(last_result, "fail x"));
    free(reply);
    put_file("mcp-epoch", "2");
    reply = ask(b, "mcp: echo|again", &meta);
    CHECK(!last_failed && !strcmp(last_result, "echo again"));
    free(reply);
    reply = ask(b, "mcp: nope|x", &meta);
    CHECK(last_failed && strstr(last_result, "mcp__fake__fail"));
    free(reply);
    b->close(b);

    memory_test();
    drive_test();

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (!failures)
        (void)!system(cmd);
    if (failures)
        fprintf(stderr, "coreagenttest: %d failures (state in %s)\n", failures, dir);
    else
        printf("coreagenttest: ok\n");
    return failures != 0;
}
