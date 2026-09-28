#include "proxyproto.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

static const unsigned char SIG[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};

size_t proxyproto_len(const unsigned char *head)
{
    if (memcmp(head, SIG, sizeof SIG) || head[12] != 0x21)
        return 0;
    return PROXYPROTO_HEAD + ((size_t)head[14] << 8 | head[15]);
}

int proxyproto_source(const unsigned char *hdr, size_t n, char *ip, size_t size)
{
    if (n < PROXYPROTO_HEAD || proxyproto_len(hdr) != n)
        return 0;
    const unsigned char *a = hdr + PROXYPROTO_HEAD;
    if (hdr[13] == 0x11 && n >= PROXYPROTO_HEAD + 12)
        return inet_ntop(AF_INET, a, ip, (socklen_t)size) != NULL;
    if (hdr[13] == 0x21 && n >= PROXYPROTO_HEAD + 36)
        return inet_ntop(AF_INET6, a, ip, (socklen_t)size) != NULL;
    return 0;
}
