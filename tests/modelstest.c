#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "models.h"
#include "text.h"

static int fail(const char *what)
{
    fprintf(stderr, "modelstest: %s\n", what);
    return 1;
}

int main(void)
{
    char path[4096];
    struct stat st;
    if (!path_config_file(path, sizeof path, "openrouter.json") ||
        stat(path, &st) || st.st_size < 1024) {
        puts("modelstest: skipped, no catalog");
        return 0;
    }

    struct model_rates bare = {0};
    if (!models_rates("codex", "gpt-5.6-sol", &bare))
        return fail("the catalog did not price a codex model");
    if (bare.input <= 0 || bare.output <= bare.input || bare.cache_read >= bare.input)
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

    puts("modelstest: ok");
    return 0;
}
