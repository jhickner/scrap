#include "sidechannelcmd.h"

#include <string.h>

#include "session.h"
#include "sessionfork.h"

static int pair(char **out, int n, int max, const char *flag, const char *value)
{
    if (n + 2 >= max)
        return n;
    out[n++] = (char *)flag;
    out[n++] = (char *)value;
    return n;
}

int sidechannel_argv(const struct session *s, const char *prompt, char **out,
                     int max)
{
    if (max < 2) {
        if (max == 1)
            out[0] = NULL;
        return 0;
    }

    int n = 0;
    out[n++] = (char *)sessionfork_program();
    n = pair(out, n, max, "-b", session_backend(s));

    const char *cwd = session_cwd(s);
    if (cwd && *cwd)
        n = pair(out, n, max, "-C", cwd);

    const char *id = session_id(s);
    if (session_can_resume(s) && id && *id && n + 3 < max) {
        out[n++] = "--session";
        out[n++] = (char *)id;
        out[n++] = "--fork";
    }

    const char *model = session_model(s);
    if (model && strcmp(model, "default"))
        n = pair(out, n, max, "-m", model);

    const char *effort = session_effort(s);
    if (effort && strcmp(effort, "default"))
        n = pair(out, n, max, "-e", effort);

    if (n + 1 < max)
        out[n++] = (char *)prompt;
    out[n] = NULL;
    return n;
}
