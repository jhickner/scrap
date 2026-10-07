#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vendor/agents/claude/claude.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

static const char *message_text(cJSON *msg)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(msg, "message");
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (cJSON_IsString(content))
        return content->valuestring;
    cJSON *block = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
    return block ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "text")) : NULL;
}

static void result(const char *text)
{
    printf("{\"type\":\"result\",\"subtype\":\"success\","
           "\"is_error\":false,\"result\":\"%s\"}\n", text);
    fflush(stdout);
}

static void stray_turn(const char *text)
{
    printf("{\"type\":\"system\",\"subtype\":\"task_notification\"}\n"
           "{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
    fflush(stdout);
    result(text);
}

static void turn(const char *text)
{
    printf("{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
    fflush(stdout);
    result(text);
}

static long milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int has_arg(int argc, char **argv, const char *value)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], value))
            return 1;
    return 0;
}

static int has_option(int argc, char **argv, const char *option, const char *value)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], option) && !strcmp(argv[i + 1], value))
            return 1;
    return 0;
}

static int mock_cli(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "auth") && !strcmp(argv[2], "login")) {
        const char *marker = getenv("CLAUDETEST_AUTH_MARKER");
        if (!has_arg(argc, argv, "--claudeai") || getenv("ANTHROPIC_API_KEY") || !marker)
            return 3;
        FILE *f = fopen(marker, "w");
        if (!f) return 4;
        fclose(f);
        return 0;
    }

    if (!has_arg(argc, argv, "--no-session-persistence") ||
        !has_option(argc, argv, "--name", "title helper") ||
        !has_option(argc, argv, "--tools", ""))
        return 2;

    char *line = NULL;
    size_t cap = 0;
    int effort_changes = 0;
    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        const char *type = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "type")) : NULL;
        cJSON *request = msg ? cJSON_GetObjectItemCaseSensitive(msg, "request") : NULL;
        const char *subtype = request ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(request, "subtype")) : NULL;
        if (type && !strcmp(type, "control_request") && subtype &&
            !strcmp(subtype, "initialize")) {
            usleep(400000);
            printf("{\"type\":\"control_response\",\"response\":{"
                   "\"subtype\":\"success\","
                   "\"request_id\":\"claude_h_initialize\","
                   "\"response\":{}},\"session_id\":\"session-1\"}\n");
            fflush(stdout);
            cJSON_Delete(msg);
            continue;
        }
        const char *text = msg ? message_text(msg) : NULL;
        cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(msg, "message"), "content") : NULL;
        if (cJSON_GetArraySize(content) > 1) {
            char shape[256] = "";
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "text"));
                size_t n = strlen(shape);
                snprintf(shape + n, sizeof shape - n, "%s%c%s", n ? "|" : "",
                         t[strlen(t) - 1] == '\n' ? 'L' : 'T',
                         cJSON_GetObjectItemCaseSensitive(block, "cache_control") ? "*" : "");
            }
            result(shape);
            cJSON_Delete(msg);
            continue;
        }
        if (text && !strcmp(text, "/effort low") && effort_changes++ == 0)
            result("Set effort level to low");
        else if (text && !strcmp(text, "/effort auto") && effort_changes++ == 1)
            result("Effort level set to auto");
        else if (text && !strcmp(text, "/effort bogus"))
            result("Invalid argument: bogus");
        else if (text && !strcmp(text, "continue")) {
            printf("{\"type\":\"rate_limit_event\",\"rate_limit_info\":{"
                   "\"status\":\"allowed\",\"resetsAt\":1789089600,"
                   "\"rateLimitType\":\"five_hour\",\"overageStatus\":\"rejected\","
                   "\"isUsingOverage\":false,\"unifiedWindows\":{"
                   "\"five_hour\":{\"utilization\":0.51,\"resetsAt\":1789089600},"
                   "\"seven_day\":{\"utilization\":0.46,\"resetsAt\":1789138800}}},"
                   "\"session_id\":\"session-1\"}\n");
            fflush(stdout);
            result("done");
        }

        else if (text && !strcmp(text, "env")) {
            const char *v = getenv("CLAUDETEST_ENV");
            result(v ? v : "unset");
        }

        else if (text && !strcmp(text, "reauth")) {
            const char *marker = getenv("CLAUDETEST_AUTH_MARKER");
            if (marker && access(marker, F_OK) == 0 &&
                has_option(argc, argv, "--resume", "session-1")) {
                result("recovered");
            } else {
                printf("{\"type\":\"result\",\"subtype\":\"error_during_execution\","
                       "\"is_error\":true,\"result\":\"Failed to authenticate: "
                       "OAuth session expired and could not be refreshed\","
                       "\"session_id\":\"session-1\"}\n");
                fflush(stdout);
            }
        }

        else if (text && !strcmp(text, "fanout")) {
            printf("{\"type\":\"system\",\"subtype\":\"background_tasks_changed\","
                   "\"tasks\":[{\"task_id\":\"a\"},{\"task_id\":\"b\"}]}\n");
            fflush(stdout);
            result("launched");

            printf("{\"type\":\"system\",\"subtype\":\"background_tasks_changed\","
                   "\"tasks\":[]}\n");
            fflush(stdout);
            stray_turn("workers done");
            printf("{\"type\":\"system\",\"subtype\":\"task_notification\","
                   "\"task_id\":\"b\",\"status\":\"stopped\"}\n");
            fflush(stdout);
        }

        else if (text && !strcmp(text, "asktwice")) {
            /* Two tool calls ask back to back, and more output lands while the
             * first prompt is still open. */
            printf("{\"type\":\"system\",\"subtype\":\"init\",\"model\":\"mock\"}\n");
            for (int i = 1; i <= 2; i++)
                printf("{\"type\":\"control_request\",\"request_id\":\"p%d\","
                       "\"request\":{\"subtype\":\"can_use_tool\","
                       "\"tool_name\":\"Bash\",\"input\":{\"command\":\"true\"}}}\n", i);
            fflush(stdout);
            usleep(50000);
            for (int i = 0; i < 400; i++)
                printf("{\"type\":\"system\",\"subtype\":\"padding\","
                       "\"n\":%d,\"pad\":\"................................\"}\n", i);
            fflush(stdout);
            int allowed = 0;
            for (int i = 0; i < 2 && getline(&line, &cap, stdin) >= 0; i++)
                if (strstr(line, "\"allow\""))
                    allowed++;
            result(allowed == 2 ? "allowed twice" : "not allowed");
        }

        else if (text && !strcmp(text, "race")) {
            printf("{\"type\":\"rate_limit_event\",\"rate_limit_info\":{"
                   "\"status\":\"allowed_warning\",\"resetsAt\":1789138800,"
                   "\"rateLimitType\":\"seven_day\",\"utilization\":0.905,"
                   "\"unifiedWindows\":{"
                   "\"five_hour\":{\"utilization\":0.6,\"resetsAt\":1789089600}}}}\n");
            fflush(stdout);
            stray_turn("background task finished");
            turn("answered");
        }
        else
            result("unexpected input");
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

static claude_client *asking;
static int asks, ask_depth, ask_nested;

/* Stands in for the permission dialog, whose key loop runs the idle pump. */
static int ask_and_pump(void *ud, const claude_permission *req)
{
    (void)ud; (void)req;
    asks++;
    if (++ask_depth > 1)
        ask_nested = 1;
    for (int i = 0; i < 10; i++) {
        claude_idle_pump(asking);
        usleep(10000);
    }
    ask_depth--;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--print") || !strcmp(argv[1], "auth")))
        return mock_cli(argc, argv);

    static const char *const child_env[] = {"CLAUDETEST_ENV=worker-7", NULL};
    claude_opts opts = {
        .cli_path = argv[0],
        .env = child_env,
        .session_name = "title helper",
        .tools = "",
        .no_session_persistence = 1,
    };
    long started = milliseconds();
    claude_client *client = claude_start(&opts);
    if (!client) {
        fputs("claudetest: could not start mock CLI\n", stderr);
        return 1;
    }
    if (milliseconds() - started >= 250 || claude_session_id(client)) {
        fputs("claudetest: startup did not return during background prewarm\n", stderr);
        claude_stop(client);
        return 1;
    }
    if (!claude_set_effort(client, "low") ||
        !claude_set_effort(client, NULL) ||
        claude_set_effort(client, "bogus")) {
        fputs("claudetest: live effort command failed\n", stderr);
        claude_stop(client);
        return 1;
    }
    claude_rate_limit rate = {0};
    claude_get_rate_limit(client, &rate);
    if (rate.available) {
        fputs("claudetest: rate limit reported before any rate_limit_event\n", stderr);
        claude_stop(client);
        return 1;
    }
    char *reply = claude_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fputs("claudetest: process was not reusable after effort changes\n", stderr);
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);
    claude_get_rate_limit(client, &rate);
    if (!rate.available || rate.used_percent != 51 ||
        rate.resets_at != 1789089600L || rate.window_minutes != 300) {
        fprintf(stderr, "claudetest: five-hour rate limit was not retained (%d, %ld, %ld)\n",
                rate.used_percent, rate.resets_at, rate.window_minutes);
        claude_stop(client);
        return 1;
    }

    reply = claude_send(client, "head\n\x1e" "a\nb\nc\n\x1e" "tail");
    if (!reply || strcmp(reply, "L|L|L|L*|T")) {
        fprintf(stderr, "claudetest: cache marks became %s\n", reply ? reply : "none");
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    /* A long marked piece keeps its start (where a caller last ended) within
       the API's 20-block lookback: 18 line blocks plus the rest. */
    char longer[512] = "head\n\x1e";
    for (int i = 0; i < 25; i++)
        strcat(longer, "x\n");
    strcat(longer, "\x1e" "tail");
    reply = claude_send(client, longer);
    if (!reply || strcmp(reply, "L|L|L|L|L|L|L|L|L|L|L|L|L|L|L|L|L|L|L|L*|T")) {
        fprintf(stderr, "claudetest: long marked piece became %s\n", reply ? reply : "none");
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    reply = claude_send(client, "env");
    if (!reply || strcmp(reply, "worker-7") || getenv("CLAUDETEST_ENV")) {
        fprintf(stderr, "claudetest: env did not reach only the child (%s)\n", reply ? reply : "none");
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    asking = client;
    claude_set_permission_cb(client, ask_and_pump, NULL);
    reply = claude_send(client, "asktwice");
    claude_set_permission_cb(client, NULL, NULL);
    if (!reply || strcmp(reply, "allowed twice") || asks != 2 || ask_nested) {
        fprintf(stderr, "claudetest: pumping during a permission prompt broke the scan "
                "(%s, %d asks, nested %d)\n", reply ? reply : "none", asks, ask_nested);
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    claude_result meta = {0};
    reply = claude_send_ex(client, "race", &meta);
    if (!reply || strcmp(reply, "answered")) {
        fprintf(stderr, "claudetest: a stray turn was taken as the reply (%s)\n",
                reply ? reply : "none");
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);
    claude_get_rate_limit(client, &rate);
    if (!rate.available || rate.used_percent != 91 ||
        rate.resets_at != 1789138800L || rate.window_minutes != 10080) {
        fprintf(stderr, "claudetest: named rate-limit window was not preferred (%d, %ld, %ld)\n",
                rate.used_percent, rate.resets_at, rate.window_minutes);
        claude_stop(client);
        return 1;
    }

    char auth_root[] = "/tmp/claudetest.XXXXXX";
    if (!mkdtemp(auth_root)) {
        perror("claudetest: mkdtemp");
        claude_stop(client);
        return 1;
    }
    char marker[4096], cli_link[4096];
    snprintf(marker, sizeof marker, "%s/authenticated", auth_root);
    snprintf(cli_link, sizeof cli_link, "%s/claude", auth_root);
    char self[4096];
    if (!realpath(argv[0], self) || symlink(self, cli_link) != 0) {
        perror("claudetest: mock cli link");
        rmdir(auth_root);
        claude_stop(client);
        return 1;
    }
    const char *old_path = getenv("PATH");
    size_t path_size = strlen(auth_root) + 2 + (old_path ? strlen(old_path) : 0);
    char *test_path = malloc(path_size);
    if (!test_path) {
        unlink(cli_link);
        rmdir(auth_root);
        claude_stop(client);
        return 1;
    }
    snprintf(test_path, path_size, "%s:%s", auth_root, old_path ? old_path : "");
    setenv("PATH", test_path, 1);
    setenv("CLAUDETEST_AUTH_MARKER", marker, 1);
    setenv("ANTHROPIC_API_KEY", "must not reach auth login", 1);

    backend_opts unattended_options = {
        .name = "claude",
        .session_name = "title helper",
        .ephemeral = 1,
        .disable_tools = 1,
        .no_browser_login = 1,
    };
    Backend *unattended = backend_open_ex(&unattended_options);
    backend_result unattended_meta = {0};
    char *refused = unattended ? unattended->ask_ex(unattended, "reauth", &unattended_meta)
                               : NULL;
    if (!refused || !unattended_meta.is_error || access(marker, F_OK) == 0) {
        fputs("claudetest: an unattended run opened the browser login\n", stderr);
        free(refused);
        if (unattended) unattended->close(unattended);
        free(test_path);
        unlink(marker); unlink(cli_link); rmdir(auth_root);
        claude_stop(client);
        return 1;
    }
    free(refused);
    unattended->close(unattended);

    backend_opts backend_options = {
        .name = "claude",
        .session_name = "title helper",
        .ephemeral = 1,
        .disable_tools = 1,
    };
    Backend *backend = backend_open_ex(&backend_options);
    backend_result backend_meta = {0};
    char *recovered = backend ? backend->ask_ex(backend, "reauth", &backend_meta) : NULL;
    if (!recovered || strcmp(recovered, "recovered") || backend_meta.is_error) {
        fprintf(stderr, "claudetest: OAuth login did not recover the turn (%s)\n",
                recovered ? recovered : "none");
        free(recovered);
        if (backend) backend->close(backend);
        free(test_path);
        unlink(marker); unlink(cli_link); rmdir(auth_root);
        claude_stop(client);
        return 1;
    }
    free(recovered);
    backend->close(backend);
    free(test_path);
    unlink(marker);
    unlink(cli_link);
    rmdir(auth_root);

    reply = claude_send(client, "fanout");
    if (!reply || claude_background_tasks(client) != 2) {
        fprintf(stderr, "claudetest: background tasks not counted (%d)\n",
                claude_background_tasks(client));
        free(reply);
        claude_stop(client);
        return 1;
    }
    free(reply);

    int busy = 1;
    started = milliseconds();
    while (milliseconds() - started < 300) {
        busy = claude_idle_pump(client);
        usleep(2000);
    }
    if (busy || claude_background_tasks(client)) {
        fputs("claudetest: idle work never cleared once the workers finished\n", stderr);
        claude_stop(client);
        return 1;
    }
    const char *owed = claude_wake_owed(client);
    if (!owed || !strstr(owed, "<task-id>b</task-id>") ||
        !strstr(owed, "<status>stopped</status>") || strstr(owed, "</task-notification>\n<task")) {
        fprintf(stderr, "claudetest: unanswered notification not kept (%s)\n",
                owed ? owed : "none");
        claude_stop(client);
        return 1;
    }

    claude_stop(client);
    puts("claudetest: ok");
    return 0;
}
