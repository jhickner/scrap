#ifndef PROXYPROTO_H
#define PROXYPROTO_H

#include <stddef.h>

#define PROXYPROTO_HEAD 16

size_t proxyproto_len(const unsigned char *head);

int proxyproto_source(const unsigned char *hdr, size_t n, char *ip, size_t size);

#endif
