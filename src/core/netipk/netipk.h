#ifndef DYN_NETIPK_NETIPK_H
#define DYN_NETIPK_NETIPK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int is4;
    uint8_t b[16];
} NetAddr;

enum {
    CLS_LOOPBACK,
    CLS_PRIVATE,
    CLS_MULTICAST,
    CLS_UNSPECIFIED,
    CLS_LL_UNICAST,
    CLS_GLOBAL_UNICAST,
    CLS_LL_MULTICAST
};

static inline int netipk_is_4in6(const NetAddr* a)
{
    int i;
    if (a->is4)
        return 0;
    for (i = 0; i < 10; i++)
        if (a->b[i])
            return 0;
    return a->b[10] == 0xff && a->b[11] == 0xff;
}

int netipk_parse_addr(const char* s, size_t n, NetAddr* a);
int netipk_parse_prefix(const char* s, size_t n, NetAddr* a, int* bits);
size_t netipk_format_addr(const NetAddr* a, char* dst);
int netipk_prefix_contains(const NetAddr* p, int bits, const NetAddr* ip);
int netipk_compare(const NetAddr* x, const NetAddr* y);
void netipk_mask(NetAddr* a, int bits);
int netipk_class(const NetAddr* a, int cls);

#endif
