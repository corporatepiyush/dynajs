#include "crawlkit.h"

int fe_scheme_is_https(const char* url)
{
    return !strncmp(url, "https://", 8);
}

int fe_split(const char* url, char* host, size_t cap, const char** path)
{
    const char *p = url, *h;
    size_t n;
    int https;
    if (!strncmp(p, "http://", 7)) {
        p += 7;
        https = 0;
    } else if (!strncmp(p, "https://", 8)) {
        p += 8;
        https = 1;
    } else
        return -1;
    h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && (unsigned char)*p > 0x20 && (unsigned char)*p != 0x7F)
        p++;
    if (*p && ((unsigned char)*p <= 0x20 || *p == 0x7F))
        return -1;
    n = (size_t)(p - h);
    if (n == 0 || n + 1 > cap)
        return -1;
    memcpy(host, h, n);
    host[n] = 0;
    *path = *p ? p : "/";
    return https;
}

static void fe_rm_dots(char* s)
{
    size_t seg[128], ns = 0, i = 0, o = 0, rooted = s[0] == '/';
    if (rooted)
        s[o++] = s[i++];
    while (s[i]) {
        if (s[i] == '/') {
            i++;
            continue;
        }
        if (s[i] == '.' && (s[i + 1] == '/' || !s[i + 1])) {
            i += s[i + 1] ? 2 : 1;
            continue;
        }
        if (s[i] == '.' && s[i + 1] == '.' && (s[i + 2] == '/' || !s[i + 2])) {
            i += s[i + 2] ? 3 : 2;
            o = ns ? seg[--ns] : (rooted ? 1u : 0u);
            continue;
        }
        if (ns < countof(seg))
            seg[ns++] = o;
        while (s[i] && s[i] != '/')
            s[o++] = s[i++];
        if (s[i])
            s[o++] = s[i++];
    }
    s[o] = 0;
}

long fe_resolve(const char* base, const char* ref, char* out, size_t cap)
{
    const char *p, *auth, *bp0, *bpend, *rf;
    size_t slen, authlen, rn, wr;
    int kind;

    for (p = ref; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f)
            return -1;

    if (!strncmp(base, "http://", 7)) {
        slen = 4;
    } else if (!strncmp(base, "https://", 8)) {
        slen = 5;
    } else
        return -1;

    auth = base + slen + 3;
    p = strchr(auth, '/');
    if (!p)
        p = strchr(auth, '?');
    authlen = p ? (size_t)(p - auth) : strlen(auth);
    bp0 = auth + authlen;
    bpend = bp0 + strcspn(bp0, "#");
    rf = strchr(ref, '#');
    rn = rf ? (size_t)(rf - ref) : strlen(ref);

    kind = 4;
    if (!strncmp(ref, "//", 2)) {
        kind = 1;
    } else if (rn > 0 && ref[0] == '/') {
        kind = 2;
    } else if (rn == 0 || ref[0] == '?') {
        kind = 3;
    } else {
        p = ref;
        if ((*p | 32) >= 'a' && (*p | 32) <= 'z') {
            const char* q = p + 1;
            while ((size_t)(q - ref) < rn && (((*q | 32) >= 'a' && (*q | 32) <= 'z') || (*q >= '0' && *q <= '9') || *q == '+' || *q == '-' || *q == '.'))
                q++;
            if ((size_t)(q - ref) < rn && *q == ':')
                kind = 0;
        }
    }

#define FE_PUT(src_, n_)                \
    do {                                \
        if ((n_) + 1 > cap - wr)        \
            return -1;                  \
        memcpy(out + wr, (src_), (n_)); \
        wr += (n_);                     \
        out[wr] = 0;                    \
    } while (0)

    if (kind == 1) {
        size_t k;
        memcpy(out, base, slen + 1);
        wr = slen + 1;
        out[wr] = 0;
        FE_PUT(ref, rn);
        k = slen + 3;
        while (out[k] && out[k] != '/')
            k++;
        if (out[k]) {
            fe_rm_dots(out + k);
            wr = k + strlen(out + k);
        } else {
            wr = strlen(out);
        }
        return (long)wr;
    }

    if (kind == 0) {
        if (rn >= cap)
            return -1;
        memcpy(out, ref, rn);
        out[rn] = 0;
        return (long)rn;
    }

    if (slen + 3 + authlen + 2 > cap)
        return -1;
    memcpy(out, base, slen + 3);
    wr = slen + 3;
    memcpy(out + wr, auth, authlen);
    wr += authlen;

    switch (kind) {
    case 2:
        FE_PUT("/", 1);
        FE_PUT(ref, rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        break;
    case 3: {
        const char* bq = memchr(bp0, '?', (size_t)(bpend - bp0));
        size_t pl = bq ? (size_t)(bq - bp0) : (size_t)(bpend - bp0);
        if (!pl)
            FE_PUT("/", 1);
        else
            FE_PUT(bp0, pl);
        FE_PUT(ref, rn);
        break;
    }
    case 4: {
        const char *be = bp0, *bs, *rq;
        while (be < bpend && *be != '?')
            be++;
        bs = be;
        while (bs > bp0 && *--bs != '/')
            ;
        if (*bs == '/')
            FE_PUT(bp0, (size_t)(bs - bp0) + 1);
        else
            FE_PUT("/", 1);
        rq = memchr(ref, '?', rn);
        FE_PUT(ref, rq ? (size_t)(rq - ref) : rn);
        fe_rm_dots(out + slen + 3 + authlen);
        wr = slen + 3 + authlen + strlen(out + slen + 3 + authlen);
        if (rq)
            FE_PUT(rq, rn - (size_t)(rq - ref));
        break;
    }
    }
    out[wr] = 0;
    return (long)wr;
#undef FE_PUT
}
