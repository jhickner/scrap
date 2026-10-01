#ifndef SIDEROUTE_H
#define SIDEROUTE_H

enum sideroute { SIDEROUTE_QUEUE, SIDEROUTE_SIDE, SIDEROUTE_REDIRECT };

enum sideroute sideroute_classify(const char *running, const char *queued);

#endif
