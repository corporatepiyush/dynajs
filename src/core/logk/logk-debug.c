#include "logk.h"

int dyn_debug_match(const char* env, const char* ns)
{
    size_t nsn = strlen(ns);
    int on = 0;
    const char* pat = env;
    while (*pat) {
        const char* e = pat;
        const char* p;
        size_t len;
        int neg = 0, m;
        const char* star;
        while (*e && *e != ',')
            e++;
        len = (size_t)(e - pat);
        p = pat;
        if (len && *p == '-') {
            neg = 1;
            p++;
            len--;
        }
        while (len && *p == ' ') {
            p++;
            len--;
        }
        while (len && p[len - 1] == ' ')
            len--;
        if (len) {
            star = memchr(p, '*', len);
            if (star) {
                size_t pre = (size_t)(star - p);
                size_t suf = len - pre - 1;
                m = nsn >= pre + suf && memcmp(p, ns, pre) == 0 && (suf == 0 || memcmp(p + pre + 1, ns + nsn - suf, suf) == 0);
            } else {
                m = nsn == len && memcmp(p, ns, len) == 0;
            }
            if (m)
                on = !neg;
        }
        if (!*e)
            break;
        pat = e + 1;
    }
    return on;
}

