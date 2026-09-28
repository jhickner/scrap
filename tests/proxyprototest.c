#include <assert.h>
#include <string.h>

#include "proxyproto.h"

int main(void)
{
    unsigned char v4[] = "\r\n\r\n\0\r\nQUIT\n\x21\x11\x00\x0c"
                           "\x64\x65\x66\x67\x7f\x00\x00\x01\x12\x34\x22\x58";
    char ip[64];
    assert(proxyproto_len(v4) == 28);
    assert(proxyproto_source(v4, 28, ip, sizeof ip) && !strcmp(ip, "100.101.102.103"));
    assert(!proxyproto_source(v4, 27, ip, sizeof ip));

    unsigned char v6[] = "\r\n\r\n\0\r\nQUIT\n\x21\x21\x00\x24"
                           "\xfd\x7a\x11\x5c\xa1\xe0\0\0\0\0\0\0\0\0\0\x01";
    assert(proxyproto_len(v6) == 52);
    assert(proxyproto_source(v6, 52, ip, sizeof ip) && !strcmp(ip, "fd7a:115c:a1e0::1"));

    unsigned char local[] = "\r\n\r\n\0\r\nQUIT\n\x20\x00\x00\x00";
    assert(!proxyproto_len(local));
    unsigned char plain[] = "{\"ls\":true,\"x\":1}\n";
    assert(!proxyproto_len(plain));
    return 0;
}
