#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "models.h"
#include "pick.h"
#include "text.h"

static int fail(const char *what)
{
    fprintf(stderr, "modelstest: %s\n", what);
    return 1;
}

static int has_label(const struct pick_item *items, int n, const char *label)
{
    for (int i = 0; i < n; i++)
        if (items[i].label && !strcmp(items[i].label, label))
            return 1;
    return 0;
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

static void clear_pi_keys(void)
{
    unsetenv("CEREBRAS_API_KEY");
    unsetenv("GROQ_API_KEY");
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("OPENAI_API_KEY");
    unsetenv("GEMINI_API_KEY");
    unsetenv("XAI_API_KEY");
    unsetenv("DEEPSEEK_API_KEY");
    unsetenv("HF_TOKEN");
}

static const char STORE[] =
    "{\n"
    "  \"cerebras\": {\"models\": [\n"
    "    {\"id\": \"qwen-3.8-27b\", \"name\": \"Qwen3.8 27B\",\n"
    "     \"contextWindow\": 65536,\n"
    "     \"cost\": {\"input\": 0.99, \"output\": 1.49, \"cacheRead\": 0}}\n"
    "  ]},\n"
    "  \"groq\": {\"models\": [\n"
    "    {\"id\": \"llama-3.3-70b\", \"name\": \"Llama\",\n"
    "     \"contextWindow\": 131072,\n"
    "     \"cost\": {\"input\": 0.1, \"output\": 0.2, \"cacheRead\": 0}}\n"
    "  ]}\n"
    "}\n";

static int native_store(void)
{
    char home[] = "/tmp/mux-modelstest-XXXXXX";
    if (!mkdtemp(home))
        return fail("could not make a temp home");

    char dir[256], store[280], auth[280];
    snprintf(dir, sizeof dir, "%s/.pi/agent", home);
    snprintf(store, sizeof store, "%s/models-store.json", dir);
    snprintf(auth, sizeof auth, "%s/auth.json", dir);
    char pi[256];
    snprintf(pi, sizeof pi, "%s/.pi", home);
    if (mkdir(pi, 0700) || mkdir(dir, 0700) || !write_file(store, STORE))
        return fail("could not write a models-store");

    setenv("HOME", home, 1);
    clear_pi_keys();

    const struct pick_item *items = NULL;
    int n = models_for("pi", &items);
    if (has_label(items, n, "cerebras/qwen-3.8-27b") ||
        has_label(items, n, "groq/llama-3.3-70b"))
        return fail("native store models appeared without a key");

    setenv("CEREBRAS_API_KEY", "test-key", 1);
    items = NULL;
    n = models_for("pi", &items);
    if (!has_label(items, n, "cerebras/qwen-3.8-27b"))
        return fail("cerebras was missing with CEREBRAS_API_KEY set");
    if (has_label(items, n, "groq/llama-3.3-70b"))
        return fail("groq appeared without GROQ_API_KEY");

    struct model_rates rates = {0};
    if (!models_rates("pi", "cerebras/qwen-3.8-27b", &rates) ||
        rates.input != 0.99 || rates.output != 1.49)
        return fail("cerebras was not priced from the store");

    unsetenv("CEREBRAS_API_KEY");
    if (!write_file(auth, "{\"cerebras\":{\"type\":\"api_key\",\"key\":\"from-auth\"}}\n"))
        return fail("could not write auth.json");
    items = NULL;
    n = models_for("pi", &items);
    if (!has_label(items, n, "cerebras/qwen-3.8-27b"))
        return fail("cerebras was missing with an auth.json key");

    char rm[300];
    snprintf(rm, sizeof rm, "rm -rf %s", home);
    (void)system(rm);
    return 0;
}

int main(void)
{
    char path[4096];
    struct stat st;
    if (path_config_file(path, sizeof path, "openrouter.json") &&
        !stat(path, &st) && st.st_size >= 1024) {
        struct model_rates bare = {0};
        if (!models_rates("codex", "gpt-5.6-sol", &bare))
            return fail("the catalog did not price a codex model");
        if (bare.input <= 0 || bare.output <= bare.input ||
            bare.cache_read >= bare.input)
            return fail("rates were not input < output with cheaper cached reads");

        struct model_rates filed = {0};
        if (!models_rates("pi", "openrouter/openai/gpt-5.6-sol", &filed) ||
            filed.input != bare.input || filed.output != bare.output)
            return fail("a catalog id priced differently from the backend's own name");

        struct model_rates routed = {0};
        if (!models_rates("pi", "openrouter/openai/gpt-5.6-sol:floor", &routed) ||
            routed.input != bare.input)
            return fail("a routing variant did not price as the model it routes to");

        struct model_rates none = {0};
        if (models_rates("codex", "gpt-9-imaginary", &none) ||
            none.input || none.output || none.cache_read)
            return fail("an unknown model was priced");
    }

    if (native_store())
        return 1;

    puts("modelstest: ok");
    return 0;
}
