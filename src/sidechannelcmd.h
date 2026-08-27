#ifndef SIDECHANNELCMD_H
#define SIDECHANNELCMD_H

struct session;

int sidechannel_argv(const struct session *s, const char *prompt, char **out,
                     int max);

#endif
