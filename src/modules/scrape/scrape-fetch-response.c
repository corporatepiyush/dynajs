#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)

static int fe_cc_no_store(const char* cc)
{
    const char* p = cc;
    if (!p)
        return 0;
    while (*p && (size_t)(p - cc) < 512) {
        if (!strncasecmp(p, "no-store", 8)) {
            const char* q = p + 8;
            if (!*q || *q == ',' || *q == ';' || *q == ' ' || *q == '\t')
                return 1;
        }
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    return 0;
}

const char* fe_header(JSContext* ctx, JSValueConst res, const char* want,
    JSValue* hold)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    const char* out = NULL;
    *hold = JS_UNDEFINED;
    if (JS_IsException(h))
        return NULL;
    if (!JS_IsObject(h)) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) == 0) {
        for (i = 0; i < n; i++) {
            const char* k = JS_AtomToCString(ctx, tab[i].atom);
            int hit = k && !strcasecmp(k, want);
            if (k)
                JS_FreeCString(ctx, k);
            if (hit) {
                *hold = JS_GetProperty(ctx, h, tab[i].atom);
                out = JS_ToCString(ctx, *hold);
                break;
            }
        }
        for (i = 0; i < n; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, h);
    return out;
}

char* fe_headers_all(JSContext* ctx, JSValueConst res, const char* want)
{
    JSValue h = JS_GetPropertyStr(ctx, res, "headers");
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    size_t cap = 0, len = 0;
    char* out = NULL;
    if (JS_IsException(h))
        return NULL;
    if (!JS_IsObject(h)) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, h, JS_GPN_STRING_MASK) != 0) {
        JS_FreeValue(ctx, h);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int hit = k && !strcasecmp(k, want);
        if (k)
            JS_FreeCString(ctx, k);
        if (!hit)
            continue;
        {
            JSValue v = JS_GetProperty(ctx, h, tab[i].atom);
            const char* vs = JS_ToCString(ctx, v);
            if (!vs) {
                JS_FreeValue(ctx, v);
                free(out);
                for (i = 0; i < n; i++)
                    JS_FreeAtom(ctx, tab[i].atom);
                js_free(ctx, tab);
                JS_FreeValue(ctx, h);
                return NULL;
            }
            {
                size_t vl = strlen(vs);
                if (len + vl + 2 > cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    char* no;
                    while (nc < len + vl + 2)
                        nc *= 2;
                    no = (char*)realloc(out, nc);
                    if (!no) {
                        JS_FreeCString(ctx, vs);
                        JS_FreeValue(ctx, v);
                        break;
                    }
                    out = no;
                    cap = nc;
                }
                if (len)
                    out[len++] = '\n';
                memcpy(out + len, vs, vl);
                len += vl;
                JS_FreeCString(ctx, vs);
            }
            JS_FreeValue(ctx, v);
        }
    }
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, h);
    if (out)
        out[len] = '\0';
    return out;
}

JSValue fe_finish_response(JSContext* ctx, JSValueConst fetcher,
    fe_t* f, const char* cur,
    JSValue res, int st)
{
    const double max_body = f->max_body;
    const int revalidate = f->revalidate;

    {
        JSValue b, hold_cl = JS_UNDEFINED;
        size_t bn = 0;
        const char* bs;

        {
            const char* clv = fe_header(ctx, res, "content-length",
                &hold_cl);
            if (!clv && JS_HasException(ctx)) {
                JS_FreeValue(ctx, hold_cl);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            if (clv) {
                double decl = httpk_delta_seconds(clv, strlen(clv));
                if (decl > max_body) {
                    JS_FreeCString(ctx, clv);
                    JS_FreeValue(ctx, hold_cl);
                    JS_FreeValue(ctx, res);
                    JS_ThrowRangeError(ctx,
                        "Fetcher: declared Content-Length %.0f exceeds "
                        "maxBodyBytes",
                        decl);
                    return JS_EXCEPTION;
                }
                JS_FreeCString(ctx, clv);
            }
            JS_FreeValue(ctx, hold_cl);
        }

        b = JS_GetPropertyStr(ctx, res, "body");
        bs = JS_ToCStringLen(ctx, &bn, b);
        if (!bs) {
            JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if ((double)bn > max_body) {
            JS_FreeCString(ctx, bs);
            JS_FreeValue(ctx, b);
            JS_FreeValue(ctx, res);
            JS_ThrowRangeError(ctx,
                "Fetcher: body of %zu bytes exceeds maxBodyBytes", bn);
            goto out;
        }
        if (st == 304 && revalidate) {
            fe_cache_ent_t* ce;
            f = fe_live(ctx, fetcher);
            ce = f ? fe_cache_find(f, cur) : NULL;
            if (ce && ce->body) {
                f->revalidated += 1;
                f->saved_bytes += (double)ce->blen;
                f->bytes += (double)bn;
                JS_FreeCString(ctx, bs);
                JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, res);
                res = JS_NewObject(ctx);
                JS_DefinePropertyValueStr(ctx, res, "status",
                    JS_NewInt32(ctx, 304), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "contentType",
                    JS_NewString(ctx, ce->ct ? ce->ct : ""), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "body",
                    JS_NewStringLen(ctx, ce->body, ce->blen), JS_PROP_C_W_E);
                f->fetched++;
                JS_DefinePropertyValueStr(ctx, res, "url",
                    JS_NewString(ctx, cur), JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_TRUE, JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, res, "notModified", JS_TRUE, JS_PROP_C_W_E);
                return res;
            }
        } else if (st >= 200 && st < 300) {
            const char *etg, *lmd, *ctv;
            JSValue he, hl, hc;
            etg = fe_header(ctx, res, "etag", &he);
            lmd = fe_header(ctx, res, "last-modified", &hl);
            ctv = fe_header(ctx, res, "content-type", &hc);
            if ((etg || lmd) && revalidate) {
                JSValue hcc = JS_UNDEFINED;
                const char* cc = fe_header(ctx, res, "cache-control",
                    &hcc);
                f = fe_live(ctx, fetcher);
                if (f && !fe_cc_no_store(cc))
                    fe_cache_put(f, cur, etg, lmd, ctv, bs, bn);
                if (cc)
                    JS_FreeCString(ctx, cc);
                JS_FreeValue(ctx, hcc);
            }
            if (etg)
                JS_FreeCString(ctx, etg);
            JS_FreeValue(ctx, he);
            if (lmd)
                JS_FreeCString(ctx, lmd);
            JS_FreeValue(ctx, hl);
            if (ctv)
                JS_FreeCString(ctx, ctv);
            JS_FreeValue(ctx, hc);
        }
        f = fe_live(ctx, fetcher);
        if (f)
            f->bytes += (double)bn;
        JS_FreeCString(ctx, bs);
        JS_FreeValue(ctx, b);
    }
    {
        char* xrt = fe_headers_all(ctx, res, "x-robots-tag");
        if (!xrt && JS_HasException(ctx)) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        if (xrt) {
            char* dirs;
            f = fe_live(ctx, fetcher);
            dirs = f ? fe_parse_robots_tag(xrt, f->agent) : NULL;
            free(xrt);
            if (dirs) {
                JSValue arr = JS_NewArray(ctx);
                size_t k = 0;
                uint32_t ai = 0;
                if (JS_IsException(arr)) {
                    free(dirs);
                    JS_FreeValue(ctx, JS_GetException(ctx));
                } else {
                    while (dirs[k]) {
                        size_t b = k;
                        while (dirs[k] && dirs[k] != '\n')
                            k++;
                        JS_DefinePropertyValueUint32(ctx, arr, ai++,
                            JS_NewStringLen(ctx, dirs + b, k - b), JS_PROP_C_W_E);
                        if (dirs[k])
                            k++;
                    }
                    JS_DefinePropertyValueStr(ctx, res, "robotsDirectives",
                        arr, JS_PROP_C_W_E);
                    free(dirs);
                }
            }
        }
        fe_link_canonical(ctx, res, cur, res);
        if (JS_HasException(ctx)) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    f = fe_live(ctx, fetcher);
    if (f)
        f->fetched++;
    {
        JSValue hold_ct = JS_UNDEFINED;
        const char* ctv = fe_header(ctx, res, "content-type", &hold_ct);
        if (!ctv && JS_HasException(ctx)) {
            JS_FreeValue(ctx, hold_ct);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValueStr(ctx, res, "contentType",
            JS_NewString(ctx, ctv ? ctv : ""),
            JS_PROP_C_W_E);
        if (ctv)
            JS_FreeCString(ctx, ctv);
        JS_FreeValue(ctx, hold_ct);
    }
    JS_DefinePropertyValueStr(ctx, res, "url", JS_NewString(ctx, cur), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "fromCache", JS_FALSE, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "notModified", JS_FALSE, JS_PROP_C_W_E);
    return res;
out:
    return JS_EXCEPTION;
}

#endif
