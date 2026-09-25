#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vendor/agents/codex/codex.h"
#include "vendor/cJSON.h"

static int abort_turn;
static codex_client *test_client;
static long live_tokens, live_window;
static int shell_starts, edit_starts, tool_results;
static char shell_input[2][1024], shell_output[512], exec_output[1024];
static char edit_input[1024], edit_diff[1024];
static int warning_events;
static char warning_text[1024];
static int trust_events;
static char trust_path[1024];
static int task_events;
static int task_running, task_completed;
static char task_id[64], task_status[32], task_path[128], task_parent[64];

static long milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int should_abort(void)
{
    codex_usage(test_client, &live_tokens, &live_window);
    return abort_turn && live_tokens > 0;
}

static void capture_event(void *ud, const codex_event *ev)
{
    (void)ud;
    if (ev->kind == CODEX_EV_TOOL && ev->name && !strcmp(ev->name, "Shell")) {
        int index = shell_starts < 2 ? shell_starts : 1;
        shell_starts++;
        snprintf(shell_input[index], sizeof shell_input[index], "%s",
                 ev->input_json ? ev->input_json : "");
    } else if (ev->kind == CODEX_EV_TOOL && ev->name && !strcmp(ev->name, "Edit")) {
        edit_starts++;
        snprintf(edit_input, sizeof edit_input, "%s",
                 ev->input_json ? ev->input_json : "");
    } else if (ev->kind == CODEX_EV_TOOL_RESULT) {
        tool_results++;
        if (ev->diff) {
            snprintf(edit_diff, sizeof edit_diff, "%s", ev->diff);
        } else if (tool_results == 1) {
            snprintf(shell_output, sizeof shell_output, "%s", ev->text ? ev->text : "");
        } else {
            snprintf(exec_output, sizeof exec_output, "%s", ev->text ? ev->text : "");
        }
    } else if (ev->kind == CODEX_EV_TRUST) {
        trust_events++;
        snprintf(trust_path, sizeof trust_path, "%s", ev->text ? ev->text : "");
    } else if (ev->kind == CODEX_EV_TASK) {
        task_events++;
        if (ev->name && !strcmp(ev->name, "running")) task_running++;
        if (ev->name && !strcmp(ev->name, "completed")) task_completed++;
        snprintf(task_id, sizeof task_id, "%s", ev->id ? ev->id : "");
        snprintf(task_status, sizeof task_status, "%s", ev->name ? ev->name : "");
        snprintf(task_path, sizeof task_path, "%s", ev->text ? ev->text : "");
        snprintf(task_parent, sizeof task_parent, "%s", ev->parent ? ev->parent : "");
    } else if (ev->kind == CODEX_EV_WARNING) {
        warning_events++;
        snprintf(warning_text, sizeof warning_text, "%s", ev->text ? ev->text : "");
    }
}

static void respond(int id, const char *result)
{
    printf("{\"id\":%d,\"result\":%s}\n", id, result);
    fflush(stdout);
}

static void respond_error(int id)
{
    printf("{\"id\":%d,\"error\":{\"code\":-32602,\"message\":\"bad test params\"}}\n", id);
    fflush(stdout);
}

static int mock_server(void)
{
    char *line = NULL;
    size_t cap = 0;
    int turns = 0;

    while (getline(&line, &cap, stdin) >= 0) {
        cJSON *msg = cJSON_Parse(line);
        cJSON *idj = msg ? cJSON_GetObjectItemCaseSensitive(msg, "id") : NULL;
        const char *method = msg ? cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(msg, "method")) : NULL;
        int id = cJSON_IsNumber(idj) ? idj->valueint : 0;

        if (method && !strcmp(method, "initialize")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            cJSON *caps = params ? cJSON_GetObjectItemCaseSensitive(
                params, "capabilities") : NULL;
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caps, "experimentalApi"))) {
                respond_error(id);
                cJSON_Delete(msg);
                continue;
            }
            usleep(400000);
            respond(id, "{}");
            fputs("timestamp ERROR codex_app_server: Project-local config, hooks, and "
                  "exec policies are disabled until the project is trusted.\n"
                  "    To load them, add /project as trusted in /tmp/config.toml.\n",
                  stderr);
            fflush(stderr);
            printf("{\"method\":\"configWarning\",\"params\":{"
                   "\"summary\":\"Project-local config, hooks, and exec policies "
                   "are disabled until the project is trusted.\","
                   "\"details\":null}}\n");
            fflush(stdout);
        } else if (method && !strcmp(method, "account/rateLimits/read")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            if (!cJSON_IsNull(params)) {
                respond_error(id);
                cJSON_Delete(msg);
                continue;
            }
            respond(id, "{\"rateLimits\":{\"primary\":{\"usedPercent\":17,"
                        "\"resetsAt\":2000000000,\"windowDurationMins\":10080}}}");
        } else if (method && !strcmp(method, "config/value/write")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            const char *key = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "keyPath")) : NULL;
            const char *strategy = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "mergeStrategy")) : NULL;
            cJSON *value = params ? cJSON_GetObjectItemCaseSensitive(params, "value") : NULL;
            cJSON *project = cJSON_IsObject(value) ?
                cJSON_GetObjectItemCaseSensitive(value, "/project.with.dot") : NULL;
            const char *trust = project ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(project, "trust_level")) : NULL;
            if (!key || strcmp(key, "projects") || !strategy || strcmp(strategy, "upsert") ||
                !trust || strcmp(trust, "trusted"))
                respond_error(id);
            else
                respond(id, "{\"status\":\"ok\"}");
        } else if (method && !strcmp(method, "thread/start")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                    params, "experimentalRawEvents")) ||
                !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "ephemeral"))) {
                respond(id, "{}");
                cJSON_Delete(msg);
                continue;
            }
            respond(id, "{\"thread\":{\"id\":\"thread-1\"}}");
        } else if (method && !strcmp(method, "thread/fork")) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            const char *thread = params ? cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(params, "threadId")) : NULL;
            if (!thread || strcmp(thread, "thread-parent"))
                respond_error(id);
            else
                respond(id, "{\"thread\":{\"id\":\"thread-fork\"}}");
        } else if (method && !strcmp(method, "thread/resume")) {
            respond_error(id);
        } else if (method && !strcmp(method, "turn/start")) {
            turns++;
            cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
            cJSON *effort = params ? cJSON_GetObjectItemCaseSensitive(params, "effort") : NULL;
            cJSON *input = params ? cJSON_GetObjectItemCaseSensitive(params, "input") : NULL;
            int valid_effort =
                (turns == 1 && cJSON_IsString(effort) && !strcmp(effort->valuestring, "high")) ||
                (turns == 2 && cJSON_IsString(effort) && !strcmp(effort->valuestring, "low")) ||
                (turns == 3 && cJSON_IsString(effort) && !strcmp(effort->valuestring, "low")) ||
                (turns == 4 && cJSON_IsNull(effort));
            int valid_input = cJSON_IsArray(input) &&
                cJSON_GetArraySize(input) == (turns == 3 ? 0 : 1);
            if (!valid_effort || !valid_input) {
                respond(id, "{}");
                cJSON_Delete(msg);
                continue;
            }

            if (turns == 2) {
                printf("{\"method\":\"account/rateLimits/updated\",\"params\":{"
                       "\"rateLimits\":{\"primary\":{\"usedPercent\":29,"
                       "\"resetsAt\":2000000100}}}}\n");
                fflush(stdout);
            }
            char started[64];
            snprintf(started, sizeof started, "{\"turn\":{\"id\":\"turn-%d\"}}", turns);
            respond(id, started);
            printf("{\"method\":\"thread/tokenUsage/updated\",\"params\":{"
                   "\"threadId\":\"thread-1\",\"turnId\":\"turn-%d\","
                   "\"tokenUsage\":{\"last\":{\"totalTokens\":%d},"
                   "\"total\":{\"totalTokens\":9999,\"inputTokens\":%d,"
                   "\"cachedInputTokens\":%d,\"cacheWriteInputTokens\":%d,"
                   "\"outputTokens\":%d},"
                   "\"modelContextWindow\":1000}}}\n",
                   turns, turns == 1 ? 120 : 240,
                   turns * 100, turns * 40, turns * 5, turns * 10);
            printf("{\"method\":\"item/agentMessage/delta\","
                   "\"params\":{\"delta\":\"%s\"}}\n",
                   turns == 1 ? "partial" : turns == 2 ? "done" :
                   turns == 3 ? "resumed" : "reset");
            if (turns > 1) {
                if (turns == 2) {
                    printf("{\"method\":\"rawResponseItem/completed\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"custom_tool_call\",\"id\":\"raw-1\","
                           "\"call_id\":\"call-1\",\"name\":\"exec\","
                           "\"status\":\"completed\","
                           "\"input\":\"const r = await tools.exec_command({cmd:\\\"pwd\\\","
                           "workdir:\\\"/project\\\",yield_time_ms:10000}); "
                           "text(r.output);\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{\"item\":{"
                           "\"type\":\"commandExecution\",\"id\":\"cmd-1\","
                           "\"command\":\"sed -n '1,2p' src/session.c\","
                           "\"cwd\":\"/project\",\"status\":\"inProgress\","
                           "\"commandActions\":[]}}}\n");
                    printf("{\"method\":\"item/completed\",\"params\":{\"item\":{"
                           "\"type\":\"commandExecution\",\"id\":\"cmd-1\","
                           "\"command\":\"sed -n '1,2p' src/session.c\","
                           "\"cwd\":\"/project\",\"status\":\"completed\","
                           "\"commandActions\":[],"
                           "\"aggregatedOutput\":\"line one\\nline two\\n\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{\"item\":{"
                           "\"type\":\"fileChange\",\"id\":\"edit-1\","
                           "\"status\":\"inProgress\",\"changes\":[{"
                           "\"path\":\"src/session.c\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n\"},{"
                           "\"path\":\"src/session.h\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -2 +2 @@\\n-before\\n+after\\n\"}]}}}\n");
                    printf("{\"method\":\"item/completed\",\"params\":{\"item\":{"
                           "\"type\":\"fileChange\",\"id\":\"edit-1\","
                           "\"status\":\"completed\",\"changes\":[{"
                           "\"path\":\"src/session.c\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n\"},{"
                           "\"path\":\"src/session.h\",\"kind\":{\"type\":\"update\"},"
                           "\"diff\":\"@@ -2 +2 @@\\n-before\\n+after\\n\"}]}}}\n");
                    printf("{\"method\":\"rawResponseItem/completed\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"custom_tool_call_output\",\"id\":\"raw-out-1\","
                           "\"call_id\":\"call-1\",\"output\":["
                           "{\"type\":\"input_text\",\"text\":\"Script completed\\n\"},"
                           "{\"type\":\"input_text\",\"text\":\"Output:\\n/project\\n\"}]}}}\n");

                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"call-agent-1\","
                           "\"kind\":\"started\",\"agentThreadId\":\"thread-2\","
                           "\"agentPath\":\"/root/review_diff\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-2\","
                           "\"status\":{\"type\":\"active\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"call-agent-2\","
                           "\"kind\":\"started\",\"agentThreadId\":\"thread-3\","
                           "\"agentPath\":\"/root/check_tests\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-3\","
                           "\"status\":{\"type\":\"active\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"call-agent-4\","
                           "\"kind\":\"started\",\"agentThreadId\":\"thread-5\","
                           "\"agentPath\":\"/root/status_only\"}}}\n");

                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"call-agent-3\","
                           "\"kind\":\"started\",\"agentThreadId\":\"thread-4\","
                           "\"agentPath\":\"/root/quick_check\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"done-agent-3\","
                           "\"kind\":\"completed\",\"agentThreadId\":\"thread-4\","
                           "\"agentPath\":\"/root/quick_check\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-4\","
                           "\"status\":{\"type\":\"idle\"}}}\n");
                    printf("{\"method\":\"item/agentMessage/delta\",\"params\":{"
                           "\"threadId\":\"thread-2\",\"delta\":\"sub\"}}\n");
                    printf("{\"method\":\"turn/completed\",\"params\":{"
                           "\"threadId\":\"thread-2\","
                           "\"turn\":{\"status\":\"completed\"}}}\n");
                }
                printf("{\"method\":\"turn/completed\",\"params\":{"
                       "\"turn\":{\"status\":\"completed\"}}}\n");
                if (turns == 2) {
                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"done-agent-1\","
                           "\"kind\":\"completed\",\"agentThreadId\":\"thread-2\","
                           "\"agentPath\":\"/root/review_diff\"}}}\n");
                    printf("{\"method\":\"item/started\",\"params\":{"
                           "\"threadId\":\"thread-1\",\"turnId\":\"turn-2\",\"item\":{"
                           "\"type\":\"subAgentActivity\",\"id\":\"done-agent-2\","
                           "\"kind\":\"completed\",\"agentThreadId\":\"thread-3\","
                           "\"agentPath\":\"/root/check_tests\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-2\","
                           "\"status\":{\"type\":\"idle\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-3\","
                           "\"status\":{\"type\":\"idle\"}}}\n");
                    printf("{\"method\":\"thread/status/changed\",\"params\":{"
                           "\"threadId\":\"thread-5\","
                           "\"status\":{\"type\":\"idle\"}}}\n");
                }
            }
            fflush(stdout);
        } else if (method && !strcmp(method, "turn/interrupt")) {
            respond(id, "{}");
            printf("{\"method\":\"turn/completed\",\"params\":{"
                   "\"turn\":{\"status\":\"interrupted\"}}}\n");
            fflush(stdout);
        }
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "app-server"))
        return mock_server();

    codex_opts opts = { .cli_path = argv[0], .effort = "high", .ephemeral = 1 };
    long started = milliseconds();
    codex_client *client = codex_start(&opts);
    if (!client) {
        fprintf(stderr, "codextest: could not start mock app-server\n");
        return 1;
    }
    if (milliseconds() - started >= 250 || codex_session_id(client)) {
        fprintf(stderr, "codextest: startup did not return during background prewarm\n");
        codex_stop(client);
        return 1;
    }

    test_client = client;
    codex_set_abort_check(client, should_abort);
    codex_set_event_cb(client, capture_event, NULL);
    if (!codex_trust_project(client, "/project.with.dot")) {
        fprintf(stderr, "codextest: config/value/write could not trust a project\n");
        codex_stop(client);
        return 1;
    }
    if (codex_idle_fd(client) < 0) {
        fprintf(stderr, "codextest: config warning did not expose an idle fd\n");
        codex_stop(client);
        return 1;
    }
    codex_idle_pump(client);
    if (trust_events != 1 || strcmp(trust_path, "this folder") || warning_events != 0 ||
        codex_last_error(client)) {
        fprintf(stderr, "codextest: trust request was not cleaned up (%d, %s, %d, %s)\n",
                trust_events, trust_path, warning_events,
                codex_last_error(client) ? codex_last_error(client) : "no stderr");
        codex_stop(client);
        return 1;
    }
    abort_turn = 1;
    codex_result meta = {0};
    char *reply = codex_send_ex(client, "interrupt me", &meta);
    if (!reply || strcmp(reply, "partial") || !meta.interrupted ||
        meta.context_tokens != 120 || meta.context_window != 1000 ||
        live_tokens != 120 || live_window != 1000 ||
        meta.input_tokens != 60 || meta.cache_read_tokens != 40 ||
        meta.cache_creation_tokens != 5 || meta.output_tokens != 10) {
        fprintf(stderr, "codextest: interrupted turn was reported as a failure\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    codex_rate_limit rate = {0};
    codex_get_rate_limit(client, &rate);
    if (!rate.available || rate.used_percent != 17 ||
        rate.resets_at != 2000000000L || rate.window_minutes != 10080) {
        fprintf(stderr, "codextest: initial account rate limit was not retained\n");
        codex_stop(client);
        return 1;
    }

    abort_turn = 0;
    if (!codex_set_effort(client, "low")) {
        fprintf(stderr, "codextest: could not change effort\n");
        codex_stop(client);
        return 1;
    }
    reply = codex_send(client, "continue");
    if (!reply || strcmp(reply, "done")) {
        fprintf(stderr, "codextest: process was not reusable after interrupt\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    if (task_events != 5 || task_running != 4 || task_completed != 1 ||
        strcmp(task_id, "thread-4") || strcmp(task_status, "completed") ||
        codex_background_tasks(client) != 3 || codex_take_continuation(client)) {
        fprintf(stderr, "codextest: active-turn sub-agents were not reported safely "
                        "(%d, %d, %d, %s, %s, %d)\n",
                task_events, task_running, task_completed, task_id, task_status,
                codex_background_tasks(client));
        codex_stop(client);
        return 1;
    }
    if (codex_idle_pump(client) || codex_background_tasks(client) ||
        task_events != 8 || task_completed != 4 || strcmp(task_status, "completed") ||
        !codex_take_continuation(client) || codex_take_continuation(client)) {
        fprintf(stderr, "codextest: idle completions did not coalesce (%d, %d, %s, %d)\n",
                task_events, task_completed, task_status,
                codex_background_tasks(client));
        codex_stop(client);
        return 1;
    }
    reply = codex_continue(client);
    if (!reply || strcmp(reply, "resumed")) {
        fprintf(stderr, "codextest: queued completion did not run as empty input\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    codex_opts fork_opts = { .cli_path = argv[0],
                             .resume_session = "thread-parent",
                             .fork_session = 1 };
    codex_client *forked = codex_start(&fork_opts);
    if (forked)
        codex_trust_project(forked, "/project.with.dot");
    const char *fork_id = codex_session_id(forked);
    if (!forked || !fork_id || strcmp(fork_id, "thread-fork")) {
        fprintf(stderr, "codextest: resumed thread was not forked (%s)\n",
                forked && codex_last_error(forked)
                    ? codex_last_error(forked) : "no backend error");
        codex_stop(forked);
        codex_stop(client);
        return 1;
    }
    codex_stop(forked);

    codex_get_rate_limit(client, &rate);
    if (!rate.available || rate.used_percent != 29 ||
        rate.resets_at != 2000000100L || rate.window_minutes != 10080) {
        fprintf(stderr, "codextest: rolling account rate limit was not retained\n");
        codex_stop(client);
        return 1;
    }

    cJSON *nested_shell = cJSON_Parse(shell_input[0]);
    const char *nested_command = nested_shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(nested_shell, "command")) : NULL;
    const char *nested_cwd = nested_shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(nested_shell, "cwd")) : NULL;
    cJSON *shell = cJSON_Parse(shell_input[1]);
    const char *command = shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(shell, "command")) : NULL;
    const char *cwd = shell ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(shell, "cwd")) : NULL;
    cJSON *edit = cJSON_Parse(edit_input);
    const char *file_path = edit ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(edit, "file_path")) : NULL;
    cJSON *changes = edit ? cJSON_GetObjectItemCaseSensitive(edit, "changes") : NULL;
    if (shell_starts != 2 || edit_starts != 1 || tool_results != 3 ||
        !nested_command || strcmp(nested_command, "pwd") ||
        !nested_cwd || strcmp(nested_cwd, "/project") ||
        !command || strcmp(command, "sed -n '1,2p' src/session.c") ||
        !cwd || strcmp(cwd, "/project") ||
        strcmp(exec_output, "/project\n") ||
        strcmp(shell_output, "line one\nline two\n") ||
        !file_path || strcmp(file_path, "src/session.c") ||
        !cJSON_IsArray(changes) || cJSON_GetArraySize(changes) != 2 ||
        strcmp(edit_diff,
               "@@file src/session.c\n@@ -1 +1 @@\n-old\n+new\n"
               "@@file src/session.h\n@@ -2 +2 @@\n-before\n+after\n")) {
        fprintf(stderr, "codextest: structured tool events were not preserved\n");
        fprintf(stderr, "  starts shell=%d edit=%d results=%d\n"
                        "  nested=%s cwd=%s legacy=%s cwd=%s\n"
                        "  legacy-out=%s raw-out=%s\n",
                shell_starts, edit_starts, tool_results,
                nested_command ? nested_command : "(null)",
                nested_cwd ? nested_cwd : "(null)",
                command ? command : "(null)", cwd ? cwd : "(null)",
                shell_output, exec_output);
        cJSON_Delete(nested_shell);
        cJSON_Delete(shell);
        cJSON_Delete(edit);
        codex_stop(client);
        return 1;
    }
    cJSON_Delete(nested_shell);
    cJSON_Delete(shell);
    cJSON_Delete(edit);

    codex_usage(client, &live_tokens, &live_window);
    if (live_tokens != 240 || live_window != 1000) {
        fprintf(stderr, "codextest: latest context usage was not retained\n");
        codex_stop(client);
        return 1;
    }

    if (!codex_set_effort(client, NULL)) {
        fprintf(stderr, "codextest: could not reset effort\n");
        codex_stop(client);
        return 1;
    }
    reply = codex_send(client, "use the default");
    if (!reply || strcmp(reply, "reset")) {
        fprintf(stderr, "codextest: default effort was not sent as null\n");
        free(reply);
        codex_stop(client);
        return 1;
    }
    free(reply);

    codex_reset(client);
    codex_usage(client, &live_tokens, &live_window);
    if (live_tokens || live_window) {
        fprintf(stderr, "codextest: reset retained stale context usage\n");
        codex_stop(client);
        return 1;
    }
    codex_stop(client);
    puts("codextest: ok");
    return 0;
}
