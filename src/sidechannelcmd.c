#include "sidechannelcmd.h"

#include "session.h"
#include "sessionfork.h"

int sidechannel_argv(const struct session *s, const char *prompt, char **out,
                     int max)
{
    const char *id = session_id(s);
    unsigned what = SESSION_ARGV_CWD;
    if (session_can_resume(s) && id && *id)
        what |= SESSION_ARGV_RESUME | SESSION_ARGV_FORK;
    return scrap_argv(out, max, what, sessionfork_program(), session_backend(s),
                    session_cwd(s), session_model(s), session_effort(s), id, 0,
                    prompt);
}
