#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

char* dyn_uri_resolve(JSContext* ctx, const char* base, const char* ref)
{
    dyn_uri_t b, r;
    size_t cap = strlen(base) + strlen(ref) + 8, n = 0, pstart, plen;
    char* out = js_malloc(ctx, cap);
    if (!out)
        return NULL;
    dyn_uri_parse(base, &b);
    dyn_uri_parse(ref, &r);
#define URI_PUT(p, l) (memcpy(out + n, (p), (l)), n += (l))
    if (r.has_scheme) {
        URI_PUT(ref, strlen(ref));
        out[n] = 0;
        pstart = r.sl + 1 + (r.has_auth ? 2 + r.al : 0);
        plen = dyn_uri_remove_dots(out + pstart, r.pl);
        memmove(out + pstart + plen, out + pstart + r.pl, n - pstart - r.pl + 1);
        return out;
    }
    if (b.has_scheme) {
        URI_PUT(b.scheme, b.sl);
        out[n++] = ':';
    }
    if (r.has_auth) {
        URI_PUT("//", 2);
        URI_PUT(r.auth, r.al);
        pstart = n;
        URI_PUT(r.path, r.pl);
        n = pstart + dyn_uri_remove_dots(out + pstart, r.pl);
        if (r.has_query) {
            out[n++] = '?';
            URI_PUT(r.query, r.ql);
        }
    } else {
        if (b.has_auth) {
            URI_PUT("//", 2);
            URI_PUT(b.auth, b.al);
        }
        pstart = n;
        if (r.pl == 0) {
            URI_PUT(b.path, b.pl);
            if (r.has_query) {
                out[n++] = '?';
                URI_PUT(r.query, r.ql);
            } else if (b.has_query) {
                out[n++] = '?';
                URI_PUT(b.query, b.ql);
            }
        } else {
            if (r.path[0] == '/') {
                URI_PUT(r.path, r.pl);
            } else {
                size_t keep = b.pl;
                if (b.has_auth && b.pl == 0) {
                    out[n++] = '/';
                } else {
                    while (keep > 0 && b.path[keep - 1] != '/')
                        keep--;
                    URI_PUT(b.path, keep);
                }
                URI_PUT(r.path, r.pl);
            }
            n = pstart + dyn_uri_remove_dots(out + pstart, n - pstart);
            if (r.has_query) {
                out[n++] = '?';
                URI_PUT(r.query, r.ql);
            }
        }
    }
    if (r.has_frag) {
        out[n++] = '#';
        URI_PUT(r.frag, r.fl);
    }
#undef URI_PUT
    out[n] = 0;
    return out;
}

#endif
