#include "netipk.h"

static NetAddr net_unmap(const NetAddr* a)
{
    NetAddr u = *a;
    if (netipk_is_4in6(a)) {
        memset(u.b, 0, sizeof(u.b));
        u.is4 = 1;
        u.b[0] = a->b[12];
        u.b[1] = a->b[13];
        u.b[2] = a->b[14];
        u.b[3] = a->b[15];
    }
    return u;
}

static int net_is_loopback(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    int i;
    if (a.is4)
        return a.b[0] == 127;
    for (i = 0; i < 15; i++)
        if (a.b[i])
            return 0;
    return a.b[15] == 1;
}

static int net_is_private(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 10 || (a.b[0] == 172 && (a.b[1] & 0xf0) == 16) || (a.b[0] == 192 && a.b[1] == 168);
    return (a.b[0] & 0xfe) == 0xfc;
}

static int net_is_multicast(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return (a.b[0] & 0xf0) == 0xe0;
    return a.b[0] == 0xff;
}

static int net_is_ll_unicast(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 169 && a.b[1] == 254;
    return a.b[0] == 0xfe && (a.b[1] & 0xc0) == 0x80;
}

static int net_is_ll_multicast(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 224 && a.b[1] == 0 && a.b[2] == 0;
    return a.b[0] == 0xff && (a.b[1] & 0x0f) == 0x02;
}

static int net_is_unspecified(const NetAddr* a0)
{
    int i;
    if (a0->is4)
        return a0->b[0] == 0 && a0->b[1] == 0 && a0->b[2] == 0 && a0->b[3] == 0;
    for (i = 0; i < 16; i++)
        if (a0->b[i])
            return 0;
    return 1;
}

static int net_is_global_unicast(const NetAddr* a0)
{
    NetAddr a = net_unmap(a0);
    int i;
    if (a.is4) {
        if (a.b[0] == 0 && a.b[1] == 0 && a.b[2] == 0 && a.b[3] == 0)
            return 0;
        if (a.b[0] == 255 && a.b[1] == 255 && a.b[2] == 255 && a.b[3] == 255)
            return 0;
    } else {
        for (i = 0; i < 16; i++)
            if (a.b[i])
                break;
        if (i == 16)
            return 0;
    }
    return !net_is_loopback(&a) && !net_is_multicast(&a) && !net_is_ll_unicast(&a);
}

int netipk_prefix_contains(const NetAddr* p, int bits, const NetAddr* ip)
{
    int full = bits / 8, rem = bits % 8, i;
    if (p->is4 != ip->is4)
        return 0;
    for (i = 0; i < full; i++)
        if (p->b[i] != ip->b[i])
            return 0;
    if (rem) {
        uint8_t m = (uint8_t)(0xff << (8 - rem));
        if ((p->b[full] & m) != (ip->b[full] & m))
            return 0;
    }
    return 1;
}

int netipk_compare(const NetAddr* x, const NetAddr* y)
{
    int c, nbytes;
    if (x->is4 != y->is4)
        return x->is4 ? -1 : 1;
    nbytes = x->is4 ? 4 : 16;
    c = memcmp(x->b, y->b, (size_t)nbytes);
    return (c > 0) - (c < 0);
}

int netipk_class(const NetAddr* a, int cls)
{
    int r;

    switch (cls) {
    case CLS_LOOPBACK:
        r = net_is_loopback(a);
        break;
    case CLS_PRIVATE:
        r = net_is_private(a);
        break;
    case CLS_MULTICAST:
        r = net_is_multicast(a);
        break;
    case CLS_UNSPECIFIED:
        r = net_is_unspecified(a);
        break;
    case CLS_LL_UNICAST:
        r = net_is_ll_unicast(a);
        break;
    case CLS_GLOBAL_UNICAST:
        r = net_is_global_unicast(a);
        break;
    case CLS_LL_MULTICAST:
        r = net_is_ll_multicast(a);
        break;
    default:
        r = 0;
        break;
    }
    return r;
}

void netipk_mask(NetAddr* a, int bits)
{
    int nbytes = a->is4 ? 4 : 16;
    int full = bits / 8, rem = bits % 8, i;
    if (rem) {
        a->b[full] &= (uint8_t)(0xff << (8 - rem));
        full++;
    }
    for (i = full; i < nbytes; i++)
        a->b[i] = 0;
}
