#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apicore.h"
#include "cmd.h"
#include "dispatch.h"
#include "session.h"
#include "workspace.h"
#include "stubs/apistubs.h"

static int failures;
static void fail(const char *what)
{
    fprintf(stderr, "apitest: %s\n", what);
    failures++;
}

/* captured replies and events */
static struct apicall *last;
static int             replies;
static char            events[512][40];
static char           *event_data[512];
static int             nevents;

static void on_reply(struct apicall *c)
{
    last = c;
    replies++;
}

static void on_emit(const char *event, cJSON *data)
{
    if (nevents < 512) {
        snprintf(events[nevents], sizeof events[nevents], "%s", event);
        event_data[nevents++] = cJSON_PrintUnformatted(data);
    }
    cJSON_Delete(data);
}

static int saw(const char *event, const char *needle)
{
    for (int i = 0; i < nevents; i++)
        if (!strcmp(events[i], event) && (!needle || strstr(event_data[i], needle)))
            return 1;
    return 0;
}

static void clear_events(void)
{
    for (int i = 0; i < nevents; i++)
        free(event_data[i]);
    nevents = 0;
}

static struct apicall call;
static cJSON         *body;

static int request(const char *method, const char *path, const char *query, const char *json)
{
    cJSON_Delete(call.out);
    cJSON_Delete(body);
    body = json ? cJSON_Parse(json) : NULL;
    memset(&call, 0, sizeof call);
    call.method = method;
    call.path = path;
    call.query = query;
    call.body = body;
    last = NULL;
    apicore_handle(&call);
    return last == &call ? call.status : 0;
}

static const char *out_str(const char *a, const char *b)
{
    cJSON *o = cJSON_GetObjectItemCaseSensitive(call.out, a);
    if (b)
        o = cJSON_GetObjectItemCaseSensitive(o, b);
    return cJSON_GetStringValue(o);
}

static const char *err_code(void)
{
    return out_str("error", "code");
}

int main(void)
{
    apicore_init(on_reply, on_emit);

    /* validation */
    if (request("POST", "/v1/agents", NULL, "{\"backend\":\"nope\",\"prompt\":{\"text\":\"x\"}}") != 400 ||
        strcmp(err_code(), "unknown_backend"))
        fail("unknown backend is 400");
    if (request("POST", "/v1/agents", NULL, "{\"backend\":\"claude\"}") != 400)
        fail("missing prompt is 400");
    if (request("POST", "/v1/agents", NULL, "{\"prompt\":{\"text\":\"x\"},\"env\":{\"1BAD\":\"v\"}}") != 400)
        fail("bad env name is 400");
    if (request("POST", "/v1/agents", NULL, "{\"prompt\":{\"text\":\"x\"},\"cwd\":\"relative\"}") != 400)
        fail("relative cwd is 400");
    spawn_fails = 1;
    if (request("POST", "/v1/agents", NULL, "{\"prompt\":{\"text\":\"x\"}}") != 502)
        fail("a CLI that does not start is 502");
    spawn_fails = 0;

    /* create: first turn starts at once; env reaches the spawn */
    if (request("POST", "/v1/agents", NULL,
                "{\"backend\":\"claude\",\"model\":\"opus\",\"title\":\"t\",\"cwd\":\"/tmp\","
                "\"prompt\":{\"text\":\"first\"},\"env\":{\"DLV_KEY\":\"k1\"}}") != 201)
        fail("create is 201");
    if (strcmp(out_str("agent", "id"), "ag_1") || strcmp(out_str("agent", "session_id"), "sess-1") ||
        strcmp(out_str("run", "id"), "run_1") || strcmp(out_str("run", "status"), "running") ||
        strcmp(out_str("agent", "status"), "busy"))
        fail("create returns the agent and its running first run");
    if (strcmp(tabs[0]->env[0], "DLV_KEY=k1"))
        fail("env reaches the spawned session");
    if (!saw("status", "\"run_id\":\"run_1\",\"status\":\"running\"") || !saw("agent", "\"busy\""))
        fail("create emits run and agent status");

    /* events route to the running run */
    backend_event ev = {.kind = BACKEND_EV_ASSISTANT, .text = "working on it"};
    apicore_event(NULL, tabs[0], &ev);
    backend_event tool = {.kind = BACKEND_EV_TOOL, .name = "Bash", .input_json = "{\"command\":\"ls\"}"};
    apicore_event(NULL, tabs[0], &tool);
    if (!saw("assistant", "\"run_id\":\"run_1\",\"text\":\"working on it\"") || !saw("tool_call", "\"name\":\"Bash\""))
        fail("assistant and tool events carry the run");

    /* the model the CLI resolves is reported as the agent's model */
    backend_event init = {.kind = BACKEND_EV_INIT, .name = "claude-opus-5-5"};
    apicore_event(NULL, tabs[0], &init);
    if (!saw("agent", "\"model\":\"claude-opus-5-5\""))
        fail("the resolved model is an agent event");
    snprintf(tabs[0]->model, sizeof tabs[0]->model, "claude-opus-5-5");
    if (request("GET", "/v1/agents/ag_1", NULL, NULL) != 200 || strcmp(out_str("model", NULL), "claude-opus-5-5"))
        fail("the agent reads back its resolved model");

    /* a second run queues behind the first */
    if (request("POST", "/v1/agents/ag_1/runs", NULL, "{\"prompt\":{\"text\":\"second\"}}") != 201 ||
        strcmp(out_str("status", NULL), "queued"))
        fail("a run sent mid-turn is queued");

    clear_events();
    end_turn(tabs[0], "all done", 0);
    if (!saw("result", "\"run_id\":\"run_1\",\"result\":\"all done\"") || !saw("result", "\"output_tokens\":42") ||
        !saw("done", "run_1") || !saw("status", "\"run_id\":\"run_2\",\"status\":\"running\""))
        fail("turn end finishes run_1 and starts the queued run_2");
    request("GET", "/v1/agents/ag_1/runs/run_1", NULL, NULL);
    if (strcmp(out_str("status", NULL), "finished") || strcmp(out_str("result", NULL), "all done"))
        fail("run_1 reads back finished with its result");

    /* cancel: queued is dropped from the tab queue, running is interrupted */
    request("POST", "/v1/agents/ag_1/runs", NULL, "{\"prompt\":{\"text\":\"third\"}}");
    if (tabs[0]->nqueue != 1)
        fail("third is queued in the tab");
    if (request("POST", "/v1/agents/ag_1/runs/run_3/cancel", NULL, NULL) != 200 ||
        strcmp(out_str("status", NULL), "cancelled") || tabs[0]->nqueue != 0)
        fail("cancelling a queued run removes it from the tab queue");
    if (request("POST", "/v1/agents/ag_1/runs/run_2/cancel", NULL, NULL) != 202 || !tabs[0]->interrupted)
        fail("cancelling the running run interrupts the turn");
    end_turn(tabs[0], NULL, 1);
    request("GET", "/v1/agents/ag_1/runs/run_2", NULL, NULL);
    if (strcmp(out_str("status", NULL), "cancelled"))
        fail("the interrupted run ends cancelled");
    if (request("POST", "/v1/agents/ag_1/runs/run_2/cancel", NULL, NULL) != 409)
        fail("cancelling a finished run is 409");

    /* a failed turn */
    request("POST", "/v1/agents/ag_1/runs", NULL, "{\"prompt\":{\"text\":\"fail\"}}");
    clear_events();
    end_turn(tabs[0], NULL, 0);
    if (!saw("error", "\"message\":\"boom\"") || !saw("status", "\"status\":\"error\""))
        fail("a failed turn emits error");

    /* someone types in the tab */
    tabs[0]->running = 1;
    apicore_turn_begin(tabs[0]);
    request("GET", "/v1/agents/ag_1", NULL, NULL);
    char latest[32];
    snprintf(latest, sizeof latest, "%s", out_str("latest_run_id", NULL));
    request("GET", "/v1/agents/ag_1/runs/run_5", NULL, NULL);
    if (strcmp(latest, "run_5") || strcmp(out_str("source", NULL), "tab"))
        fail("a typed turn is recorded as a tab run");
    end_turn(tabs[0], "typed reply", 0);

    /* create held until the backend reports an id */
    hide_ids = 1;
    if (request("POST", "/v1/agents", NULL, "{\"prompt\":{\"text\":\"slow\"}}") != 0)
        fail("create is held while the session has no id");
    hide_ids = 0;
    snprintf(tabs[1]->id, sizeof tabs[1]->id, "late-id");
    apicore_tick();
    if (!last || last->status != 201 || strcmp(out_str("agent", "session_id"), "late-id"))
        fail("the held create replies once the id arrives");

    /* delete rules */
    in_view = 1;
    end_turn(tabs[1], "ok", 0);
    if (request("DELETE", "/v1/agents/ag_2", NULL, NULL) != 409 || strcmp(err_code(), "agent_in_view"))
        fail("deleting the tab in view is refused");
    in_view = 0;
    request("POST", "/v1/agents/ag_2/runs", NULL, "{\"prompt\":{\"text\":\"busy\"}}");
    if (request("DELETE", "/v1/agents/ag_2", NULL, NULL) != 409 || strcmp(err_code(), "agent_busy"))
        fail("deleting a busy tab is refused");
    clear_events();
    if (request("DELETE", "/v1/agents/ag_2", "force=1", NULL) != 200 || strcmp(out_str("status", NULL), "exited") ||
        ntabs != 1 || !saw("status", "\"status\":\"cancelled\"") || !saw("agent", "\"exited\""))
        fail("force delete closes the tab and cancels its run");
    if (request("POST", "/v1/agents/ag_2/runs", NULL, "{\"prompt\":{\"text\":\"x\"}}") != 409)
        fail("an exited agent takes no runs");

    /* a tab closed elsewhere */
    request("POST", "/v1/agents/ag_1/runs", NULL, "{\"prompt\":{\"text\":\"orphan\"}}");
    workspace_close(0);
    clear_events();
    apicore_tick();
    if (!saw("agent", "\"ag_1\",\"status\":\"exited\"") || !saw("status", "\"status\":\"error\""))
        fail("a closed tab exits the agent and fails its run");

    /* listing, capacity, routes */
    request("GET", "/v1/agents", NULL, NULL);
    if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(call.out, "agents")) != 0)
        fail("exited agents are hidden by default");
    request("GET", "/v1/agents", "include_exited=1", NULL);
    if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(call.out, "agents")) != 2)
        fail("include_exited lists them");
    while (ntabs < WORKSPACE_MAX)
        dispatch_spawn("claude", NULL, NULL, NULL, NULL, NULL, NULL);
    if (request("POST", "/v1/agents", NULL, "{\"prompt\":{\"text\":\"x\"}}") != 429)
        fail("a full instance is 429");
    request("GET", "/v1/capacity", NULL, NULL);
    if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(call.out, "free")) != 0)
        fail("capacity reports no free slots");
    if (request("GET", "/v1/agents/ag_99", NULL, NULL) != 404 || request("GET", "/v1/nope", NULL, NULL) != 404 ||
        request("PUT", "/v1/agents", NULL, NULL) != 405 || request("GET", "/v1/agents/ag_1/runs/run_6", NULL, NULL) != 404)
        fail("unknown routes, ids, methods, and another agent's run");

    cJSON_Delete(call.out);
    cJSON_Delete(body);
    clear_events();
    apicore_reset();
    if (failures)
        return 1;
    puts("apitest: ok");
    return 0;
}
