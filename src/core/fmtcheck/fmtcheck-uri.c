#include "fmtcheck.h"

void dyn_uri_parse(const char* u, dyn_uri_t* o)
{
    const char* p = u;
    const char* q;
    memset(o, 0, sizeof(*o));
    for (q = p; *q && *q != ':' && *q != '/' && *q != '?' && *q != '#'; q++)
        ;
    if (*q == ':' && q > p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
        o->scheme = p;
        o->sl = (size_t)(q - p);
        o->has_scheme = 1;
        p = q + 1;
    }
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        for (q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++)
            ;
        o->auth = p;
        o->al = (size_t)(q - p);
        o->has_auth = 1;
        p = q;
    }
    for (q = p; *q && *q != '?' && *q != '#'; q++)
        ;
    o->path = p;
    o->pl = (size_t)(q - p);
    p = q;
    if (*p == '?') {
        for (q = p + 1; *q && *q != '#'; q++)
            ;
        o->query = p + 1;
        o->ql = (size_t)(q - p - 1);
        o->has_query = 1;
        p = q;
    }
    if (*p == '#') {
        o->frag = p + 1;
        o->fl = strlen(p + 1);
        o->has_frag = 1;
    }
}

size_t dyn_uri_remove_dots(char* in, size_t n)
{
    char* out = in;
    size_t i = 0, o = 0;
    while (i < n) {
        if (n - i >= 3 && !memcmp(in + i, "../", 3)) {
            i += 3;
        } else if (n - i >= 2 && !memcmp(in + i, "./", 2)) {
            i += 2;
        } else if (n - i >= 3 && !memcmp(in + i, "/./", 3)) {
            i += 2;
        } else if (n - i == 2 && !memcmp(in + i, "/.", 2)) {
            in[i + 1] = '/';
            i += 1;
        } else if ((n - i >= 4 && !memcmp(in + i, "/../", 4)) || (n - i == 3 && !memcmp(in + i, "/..", 3))) {
            if (n - i == 3) {
                in[i + 2] = '/';
                i += 2;
            } else {
                i += 3;
            }
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
        } else if ((n - i == 1 && in[i] == '.') || (n - i == 2 && !memcmp(in + i, "..", 2))) {
            i = n;
        } else {
            size_t j = i + (in[i] == '/' ? 1 : 0);
            while (j < n && in[j] != '/')
                j++;
            memmove(out + o, in + i, j - i);
            o += j - i;
            i = j;
        }
    }
    return o;
}
