#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-httpk.h"
#include "core/dyn-prng.h"
#include "core/dyn-sb.h"
#include "cutils.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DYN_MP_MAX_PARTS 1024u
#define DYN_MP_MAX_PART_BYTES (16u << 20)
#define DYN_MP_MAX_HDR_LINE 4096u
#define DYN_MP_MAX_HDR_BLOCK 65536u
#define DYN_MP_MAX_BOUNDARY 70u

typedef struct {
    const uint8_t* p;
    size_t n;
    char* owned;
} dyn_mp_body_t;

typedef struct {
    dyn_sb_t name, fname;
    int have_name, have_fname;
} dyn_mp_disp_t;

int dyn_mp_ctl(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F)
            return 1;
    }
    return 0;
}

void dyn_hm_trim(const char* s, size_t* b, size_t* e)
{
    while (*b < *e && dyn_hm_ows(s[*b]))
        (*b)++;
    while (*e > *b && dyn_hm_ows(s[*e - 1]))
        (*e)--;
}

static void dyn_hm_lower(char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = (char)(s[i] - 'A' + 'a');
}

size_t dyn_hm_next_elem(const char* s, size_t n, size_t i, size_t* end)
{
    int q = 0;
    size_t b = i;
    while (i < n) {
        char c = s[i];
        if (q) {
            if (c == '\\' && i + 1 < n)
                i++;
            else if (c == '"')
                q = 0;
        } else if (c == '"') {
            q = 1;
        } else if (c == ',') {
            break;
        }
        i++;
    }
    *end = i;
    (void)b;
    return (i < n) ? i + 1 : i;
}

static int dyn_hm_put_param(JSContext* ctx, JSValueConst obj, const char* k,
    size_t kn, const char* v, size_t vn)
{
    char stack[64], *buf = stack;
    int r;
    if (kn == 0)
        return 0;
    if (kn + 1 > sizeof stack) {
        buf = (char*)malloc(kn + 1);
        if (!buf) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    memcpy(buf, k, kn);
    buf[kn] = 0;
    dyn_hm_lower(buf, kn);
    r = JS_DefinePropertyValueStr(ctx, obj, buf, JS_NewStringLen(ctx, v, vn),
        JS_PROP_C_W_E);
    if (buf != stack)
        free(buf);
    return r < 0 ? -1 : 0;
}

static int dyn_hm_params(JSContext* ctx, JSValueConst obj, const char* s,
    size_t i, size_t n)
{
    while (i < n) {
        size_t nb, ne, vb, ve;
        while (i < n && (s[i] == ';' || dyn_hm_ows(s[i])))
            i++;
        if (i >= n)
            break;
        nb = i;
        while (i < n && s[i] != '=' && s[i] != ';')
            i++;
        ne = i;
        dyn_hm_trim(s, &nb, &ne);
        if (i < n && s[i] == '=') {
            i++;
            if (i < n && s[i] == '"') {
                dyn_sb_t v;
                dyn_sb_init(&v);
                i++;
                while (i < n && s[i] != '"') {
                    if (s[i] == '\\' && i + 1 < n) {
                        unsigned char e = (unsigned char)s[i + 1];
                        if ((e < 0x20 && e != '\t') || e == 0x7f) {
                            dyn_sb_free(&v);
                            return -1;
                        }
                        i++;
                    } else if (((unsigned char)s[i] < 0x20 && s[i] != '\t')
                        || (unsigned char)s[i] == 0x7f) {
                        dyn_sb_free(&v);
                        return -1;
                    }
                    dyn_sb_putc(&v, s[i]);
                    i++;
                }
                if (i < n)
                    i++;
                if (v.oom) {
                    dyn_sb_free(&v);
                    JS_ThrowOutOfMemory(ctx);
                    return -2;
                }
                if (dyn_hm_put_param(ctx, obj, s + nb, ne - nb, v.p ? v.p : "", v.n) < 0) {
                    dyn_sb_free(&v);
                    return -2;
                }
                dyn_sb_free(&v);
                continue;
            }
            vb = i;
            while (i < n && s[i] != ';')
                i++;
            ve = i;
            dyn_hm_trim(s, &vb, &ve);
        } else {
            vb = ve = i;
        }
        if (dyn_hm_put_param(ctx, obj, s + nb, ne - nb, s + vb, ve - vb) < 0)
            return -2;
    }
    return 0;
}

JSValue dyn_hm_ctype_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n, i = 0, tb, te, sb, se;
    JSValue o, params;
    char* buf;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "ContentTypeParse(header): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    while (i < n && dyn_hm_ows(s[i]))
        i++;
    tb = i;
    while (i < n && httpk_tchar[(unsigned char)s[i]])
        i++;
    te = i;
    if (te == tb || i >= n || s[i] != '/') {
        JS_FreeCString(ctx, s);
        return JS_NULL;
    }
    i++;
    sb = i;
    while (i < n && httpk_tchar[(unsigned char)s[i]])
        i++;
    se = i;
    if (se == sb) {
        JS_FreeCString(ctx, s);
        return JS_NULL;
    }
    while (i < n && dyn_hm_ows(s[i]))
        i++;
    if (i < n && s[i] != ';') {
        JS_FreeCString(ctx, s);
        return JS_NULL;
    }

    o = JS_NewObject(ctx);
    params = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_IsException(params)) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, params);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    buf = (char*)malloc((te - tb > se - sb ? te - tb : se - sb) + 1);
    if (!buf) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, params);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(buf, s + tb, te - tb);
    buf[te - tb] = 0;
    dyn_hm_lower(buf, te - tb);
    int r1 = JS_DefinePropertyValueStr(ctx, o, "type", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    memcpy(buf, s + sb, se - sb);
    buf[se - sb] = 0;
    dyn_hm_lower(buf, se - sb);
    int r2 = JS_DefinePropertyValueStr(ctx, o, "subtype", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    free(buf);
    int pr = r1 < 0 || r2 < 0 ? -2 : dyn_hm_params(ctx, params, s, i, n);
    if (pr < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, params);
        JS_FreeCString(ctx, s);
        return pr == -2 ? JS_EXCEPTION : JS_NULL;
    }
    JS_FreeCString(ctx, s);
    if (JS_DefinePropertyValueStr(ctx, o, "parameters", params, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int dyn_mp_bchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z'))
        return 1;
    switch (c) {
    case '\'':
    case '+':
    case '_':
    case ',':
    case '-':
    case '.':
    case '/':
    case ':':
    case '=':
    case '?':
        return 1;
    default:
        return 0;
    }
}

static int dyn_mp_boundary_ok(const char* b, size_t n)
{
    size_t i;
    if (n == 0 || n > DYN_MP_MAX_BOUNDARY)
        return 0;
    for (i = 0; i < n; i++)
        if (!dyn_mp_bchar((unsigned char)b[i]))
            return 0;
    return 1;
}

static void dyn_mp_body_free(JSContext* ctx, dyn_mp_body_t* v)
{
    if (v->owned) {
        JS_FreeCString(ctx, v->owned);
        v->owned = NULL;
    }
}

static JSValue dyn_mp_body_view(JSContext* ctx, JSValueConst v,
    dyn_mp_body_t* out)
{
    JSValue buf;
    size_t off, len, bpe, ab;
    uint8_t* base;

    out->p = NULL;
    out->n = 0;
    out->owned = NULL;
    if (JS_IsString(v)) {
        out->p = (const uint8_t*)JS_ToCStringLen(ctx, &out->n, v);
        if (!out->p)
            return JS_EXCEPTION;
        out->owned = DYN_UNCONST(out->p);
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(v))
        return JS_ThrowTypeError(ctx,
            "MultipartParse: body must be a string or a byte view");
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (!JS_IsException(buf)) {
        if (bpe != 1) {
            JS_FreeValue(ctx, buf);
            return JS_ThrowTypeError(ctx,
                "MultipartParse: body must be a byte view (Uint8Array)");
        }
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base)
            return JS_EXCEPTION;
        if (off > ab || len > ab - off)
            return JS_ThrowRangeError(ctx,
                "MultipartParse: typed array out of bounds");
        out->p = base + off;
        out->n = len;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        out->p = base;
        out->n = ab;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_ThrowTypeError(ctx,
        "MultipartParse: body must be a string or a byte view");
}

static void dyn_mp_escape(dyn_sb_t* b, const char* s, size_t n)
{
    size_t i;
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\')
            dyn_sb_putc(b, '\\');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

static size_t dyn_mp_find_delim(const uint8_t* b, size_t n, size_t start,
    const char* bnd, size_t blen, int* final)
{
    size_t i, end;
    *final = 0;
    if (n < start + blen + 2)
        return SIZE_MAX;
    if (b[start] == '-' && b[start + 1] == '-'
        && memcmp(b + start + 2, bnd, blen) == 0) {
        end = start + blen + 2;
        if (end + 2 <= n && b[end] == '-' && b[end + 1] == '-') {
            *final = 1;
            return start;
        }
        if (end + 1 < n && b[end] == '\r' && b[end + 1] == '\n')
            return start;
    }
    for (i = start; i + blen + 4 <= n; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '-'
            && b[i + 3] == '-' && b[i + 4] == (unsigned char)bnd[0]
            && memcmp(b + i + 4, bnd, blen) == 0) {
            end = i + blen + 4;
            if (end + 2 <= n && b[end] == '-' && b[end + 1] == '-') {
                *final = 1;
                return i + 2;
            }
            if (end + 1 < n && b[end] == '\r' && b[end + 1] == '\n')
                return i + 2;
        }
    }
    return SIZE_MAX;
}

static size_t dyn_mp_param(const char* v, size_t vn, size_t i,
    size_t* pb, size_t* pe, size_t* vb, size_t* ve,
    int* quoted)
{
    size_t k;
    *pb = i;
    while (i < vn && v[i] != '=' && v[i] != ';')
        i++;
    *pe = i;
    dyn_hm_trim(v, pb, pe);
    if (*pe == *pb)
        return SIZE_MAX;
    for (k = *pb; k < *pe; k++)
        if (!httpk_tchar[(unsigned char)v[k]])
            return SIZE_MAX;
    if (i >= vn || v[i] != '=')
        return SIZE_MAX;
    i++;
    if (i < vn && v[i] == '"') {
        *quoted = 1;
        i++;
        *vb = i;
        while (i < vn && v[i] != '"') {
            if (v[i] == '\\' && i + 1 < vn) {
                if ((v[i + 1] < 0x20 && v[i + 1] != '\t') || v[i + 1] == 0x7f)
                    return SIZE_MAX;
                i++;
            } else if ((v[i] < 0x20 && v[i] != '\t') || v[i] == 0x7f) {
                return SIZE_MAX;
            }
            i++;
        }
        if (i >= vn)
            return SIZE_MAX;
        *ve = i;
        i++;
        while (i < vn && dyn_hm_ows(v[i]))
            i++;
        if (i < vn && v[i] != ';')
            return SIZE_MAX;
        return i;
    }
    *quoted = 0;
    *vb = i;
    while (i < vn && v[i] != ';')
        i++;
    *ve = i;
    dyn_hm_trim(v, vb, ve);
    for (k = *vb; k < *ve; k++)
        if (!httpk_tchar[(unsigned char)v[k]])
            return SIZE_MAX;
    return i;
}

static int dyn_mp_disposition(JSContext* ctx, const char* v, size_t vn,
    dyn_mp_disp_t* d)
{
    size_t i = 0;
    d->have_name = 0;
    d->have_fname = 0;
    while (i < vn && dyn_hm_ows(v[i]))
        i++;
    if (vn - i < 9 || strncasecmp(v + i, "form-data", 9) != 0) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: Content-Disposition type must be form-data");
        return -1;
    }
    i += 9;
    while (i < vn) {
        size_t pb, pe, vb, ve, k;
        int quoted;
        dyn_sb_t* dst;
        while (i < vn && (dyn_hm_ows(v[i]) || v[i] == ';'))
            i++;
        if (i >= vn)
            break;
        i = dyn_mp_param(v, vn, i, &pb, &pe, &vb, &ve, &quoted);
        if (i == SIZE_MAX) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: malformed Content-Disposition parameter");
            return -1;
        }
        if (pe - pb == 4 && strncasecmp(v + pb, "name", 4) == 0) {
            if (d->have_name) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: duplicate name parameter");
                return -1;
            }
            dst = &d->name;
            d->have_name = 1;
        } else if (pe - pb == 8 && strncasecmp(v + pb, "filename", 8) == 0) {
            if (d->have_fname) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: duplicate filename parameter");
                return -1;
            }
            dst = &d->fname;
            d->have_fname = 1;
        } else {
            dst = NULL;
        }
        if (dst) {
            if (quoted) {
                for (k = vb; k < ve; k++) {
                    if (v[k] == '\\' && k + 1 < ve)
                        k++;
                    dyn_sb_putc(dst, v[k]);
                }
            } else {
                dyn_sb_put(dst, v + vb, ve - vb);
            }
            if (dst->oom) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        }
        if (i == vn)
            break;
    }
    return 0;
}

static JSValue dyn_mp_part_obj(JSContext* ctx, const dyn_mp_disp_t* d,
    const uint8_t* b, size_t cs, size_t ce,
    size_t body_start, size_t body_end)
{
    static const uint8_t zero = 0;
    JSValue o, ab, ta;
    JSValueConst ta3[3];

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, o, "name",
            JS_NewStringLen(ctx, d->name.p ? d->name.p : "", d->name.n), JS_PROP_C_W_E) < 0
        || (d->have_fname && JS_DefinePropertyValueStr(ctx, o, "filename",
                JS_NewStringLen(ctx, d->fname.p ? d->fname.p : "", d->fname.n), JS_PROP_C_W_E) < 0)
        || (cs != SIZE_MAX && JS_DefinePropertyValueStr(ctx, o, "contentType",
                JS_NewStringLen(ctx, (const char*)b + cs, ce - cs), JS_PROP_C_W_E) < 0)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    ab = JS_NewArrayBufferCopy(ctx,
        body_end > body_start ? b + body_start : &zero,
        body_end - body_start);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    ta3[0] = ab;
    ta3[1] = JS_UNDEFINED;
    ta3[2] = JS_UNDEFINED;
    ta = JS_NewTypedArray(ctx, 3, ta3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(ta)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "body", ta, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue dyn_mp_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *ct = NULL, *bnd = NULL;
    size_t ctn = 0, blen = 0;
    dyn_mp_body_t body;
    JSValue ctv = JS_UNDEFINED, params = JS_UNDEFINED, bndv = JS_UNDEFINED;
    JSValue arr = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    uint32_t out = 0;
    const uint8_t* b;
    size_t q, E, D2, hd;
    int final;

    body.p = NULL;
    body.n = 0;
    body.owned = NULL;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "MultipartParse(contentType, body): contentType must be a string");
    ct = JS_ToCStringLen(ctx, &ctn, argv[0]);
    if (!ct)
        return JS_EXCEPTION;
    if (JS_IsException(dyn_mp_body_view(ctx, argv[1], &body))) {
        JS_FreeCString(ctx, ct);
        return JS_EXCEPTION;
    }
    b = body.p;
    ctv = dyn_hm_ctype_parse(ctx, JS_UNDEFINED, 1, argv);
    if (JS_IsException(ctv))
        goto done;
    if (JS_IsNull(ctv)) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: Content-Type must be multipart/form-data");
        goto done;
    }
    {
        JSValue tv = JS_GetPropertyStr(ctx, ctv, "type");
        JSValue sv = JS_IsException(tv) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, ctv, "subtype");
        const char *ts, *ss;
        size_t tn, sn;
        int bad = 1;
        if (JS_IsException(sv)) {
            JS_FreeValue(ctx, tv);
            goto done;
        }
        if (JS_IsString(tv) && JS_IsString(sv)) {
            ts = JS_ToCStringLen(ctx, &tn, tv);
            ss = JS_ToCStringLen(ctx, &sn, sv);
            if (ts && ss && tn == 9 && sn == 9
                && memcmp(ts, "multipart", 9) == 0
                && memcmp(ss, "form-data", 9) == 0)
                bad = 0;
            if (ts)
                JS_FreeCString(ctx, ts);
            if (ss)
                JS_FreeCString(ctx, ss);
        }
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, sv);
        if (bad) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: Content-Type must be multipart/form-data");
            goto done;
        }
    }
    params = JS_GetPropertyStr(ctx, ctv, "parameters");
    if (JS_IsException(params))
        goto done;
    if (JS_IsObject(params)) {
        bndv = JS_GetPropertyStr(ctx, params, "boundary");
        if (JS_IsException(bndv))
            goto done;
        if (JS_IsString(bndv))
            bnd = JS_ToCStringLen(ctx, &blen, bndv);
    }
    if (!bnd || !dyn_mp_boundary_ok(bnd, blen)) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: boundary parameter is missing or invalid (1-70 bcharsnospace)");
        goto done;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto done;
    D2 = dyn_mp_find_delim(b, body.n, 0, bnd, blen, &final);
    if (D2 == SIZE_MAX) {
        JS_ThrowTypeError(ctx, "MultipartParse: no opening boundary");
        goto done;
    }
    if (final) {
        ret = arr;
        goto done;
    }
    q = D2 + blen + 4;
    for (;;) {
        size_t r, ds = SIZE_MAX, de = 0, cs = SIZE_MAX, ce = 0;
        size_t body_start, body_end, plen;
        int disp_lines = 0;
        dyn_mp_disp_t d;
        JSValue part;

        if (out >= DYN_MP_MAX_PARTS) {
            JS_ThrowTypeError(ctx, "MultipartParse: more than 1024 parts");
            goto done;
        }
        {
            size_t k;
            E = SIZE_MAX;
            for (k = q; k + 3 < body.n; k++)
                if (b[k] == '\r' && b[k + 1] == '\n'
                    && b[k + 2] == '\r' && b[k + 3] == '\n') {
                    E = k;
                    break;
                }
            if (E == SIZE_MAX) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part headers not terminated by a blank line");
                goto done;
            }
            if (E - q > DYN_MP_MAX_HDR_BLOCK) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part header block exceeds 65536 bytes");
                goto done;
            }
        }
        hd = dyn_mp_find_delim(b, E, q, bnd, blen, &final);
        if (hd != SIZE_MAX) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: boundary delimiter inside part headers");
            goto done;
        }
        r = q;
        while (r < E) {
            size_t le = r, colon, hb, ve;
            size_t k;
            while (le <= E && !(b[le] == '\r' && b[le + 1] == '\n'))
                le++;
            if (le > E)
                break;
            if (le - r > DYN_MP_MAX_HDR_LINE) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part header line exceeds 4096 bytes");
                goto done;
            }
            for (k = r; k < le; k++)
                if (b[k] == '\r' || b[k] == '\n') {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: bare CR or LF in a header line");
                    goto done;
                }
            for (colon = r; colon < le && b[colon] != ':'; colon++)
                ;
            if (colon == le || colon == r) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: malformed part header line");
                goto done;
            }
            for (k = r; k < colon; k++)
                if (!httpk_tchar[(unsigned char)b[k]]) {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: malformed part header name");
                    goto done;
                }
            hb = colon + 1;
            while (hb < le && dyn_hm_ows(b[hb]))
                hb++;
            ve = le;
            while (ve > hb && dyn_hm_ows(b[ve - 1]))
                ve--;
            if (colon - r == 19
                && strncasecmp((const char*)b + r, "Content-Disposition",
                       19)
                    == 0) {
                if (disp_lines) {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: more than one Content-Disposition header");
                    goto done;
                }
                disp_lines = 1;
                ds = hb;
                de = ve;
            } else if (colon - r == 12
                && strncasecmp((const char*)b + r, "Content-Type", 12) == 0) {
                cs = hb;
                ce = ve;
            }
            r = le + 2;
        }
        if (!disp_lines) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: part has no Content-Disposition header");
            goto done;
        }
        dyn_sb_init(&d.name);
        dyn_sb_init(&d.fname);
        if (dyn_mp_disposition(ctx, (const char*)b + ds, de - ds, &d) < 0) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            goto done;
        }
        if (!d.have_name) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: Content-Disposition lacks a name parameter");
            goto done;
        }
        body_start = E + 4;
        D2 = dyn_mp_find_delim(b, body.n, body_start, bnd, blen, &final);
        if (D2 == SIZE_MAX) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: missing closing boundary");
            goto done;
        }
        body_end = (D2 == body_start) ? D2 : D2 - 2;
        plen = body_end - body_start;
        if (plen > DYN_MP_MAX_PART_BYTES) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: part body exceeds 16 MiB");
            goto done;
        }
        part = dyn_mp_part_obj(ctx, &d, b, cs, ce, body_start, body_end);
        dyn_sb_free(&d.name);
        dyn_sb_free(&d.fname);
        if (JS_IsException(part))
            goto done;
        JS_DefinePropertyValueUint32(ctx, arr, out++, part, JS_PROP_C_W_E);
        if (final) {
            ret = arr;
            goto done;
        }
        q = D2 + blen + 4;
    }
done:
    if (bnd)
        JS_FreeCString(ctx, bnd);
    JS_FreeValue(ctx, bndv);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, ctv);
    dyn_mp_body_free(ctx, &body);
    JS_FreeCString(ctx, ct);
    if (JS_IsException(ret))
        JS_FreeValue(ctx, arr);
    return ret;
}

JSValue dyn_http_multipart_format(JSContext* ctx, JSValueConst parts)
{
    return dyn_mp_format(ctx, JS_UNDEFINED, 1, &parts);
}

JSValue dyn_http_multipart_parse(JSContext* ctx, JSValueConst ctype, JSValueConst body)
{
    JSValueConst a[2];
    a[0] = ctype;
    a[1] = body;
    return dyn_mp_parse(ctx, JS_UNDEFINED, 2, a);
}

JSValue dyn_mp_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sb_t out;
    char bnd[71];
    size_t blen;
    int64_t nparts = 0, k;
    JSValue ret = JS_EXCEPTION;
    JSValue el = JS_UNDEFINED, nm = JS_UNDEFINED, val = JS_UNDEFINED;
    JSValue bd = JS_UNDEFINED, fn = JS_UNDEFINED, ct = JS_UNDEFINED;
    const char* ns = NULL;
    size_t nn = 0;
    dyn_mp_body_t payload;

    payload.p = NULL;
    payload.n = 0;
    payload.owned = NULL;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "MultipartFormat(parts, boundary?): parts must be an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToInt64(ctx, &nparts, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char* s;
        size_t sn;
        if (!JS_IsString(argv[1]))
            return JS_ThrowTypeError(ctx,
                "MultipartFormat(parts, boundary?): boundary must be a string");
        s = JS_ToCStringLen(ctx, &sn, argv[1]);
        if (!s)
            return JS_EXCEPTION;
        if (!dyn_mp_boundary_ok(s, sn)) {
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx,
                "MultipartFormat: boundary must be 1-70 bcharsnospace characters");
        }
        memcpy(bnd, s, sn);
        bnd[sn] = 0;
        blen = sn;
        JS_FreeCString(ctx, s);
    } else {
        dyn_prng_t r;
        size_t i;
        static const char B62[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        if (dyn_prng_seed_random(&r) < 0)
            return JS_ThrowInternalError(ctx,
                "MultipartFormat: OS entropy unavailable");
        memcpy(bnd, "------------------------", 24);
        for (i = 24; i < 48; i++)
            bnd[i] = B62[dyn_prng_next_bounded(&r, 62)];
        bnd[48] = 0;
        blen = 48;
    }
    dyn_sb_init(&out);
    for (k = 0; k < nparts; k++) {
        int have_payload = 0;

        el = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)k);
        if (JS_IsException(el))
            goto done;
        if (!JS_IsObject(el)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: each part must be an object");
            goto err;
        }
        nm = JS_GetPropertyStr(ctx, el, "name");
        if (JS_IsException(nm))
            goto err;
        if (!JS_IsString(nm)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: each part needs a string name");
            goto err;
        }
        ns = JS_ToCStringLen(ctx, &nn, nm);
        if (!ns)
            goto err;
        if (nn == 0 || dyn_mp_ctl(ns, nn)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: name must be a non-empty string with no control characters");
            goto err;
        }
        val = JS_GetPropertyStr(ctx, el, "value");
        if (JS_IsException(val))
            goto err;
        bd = JS_GetPropertyStr(ctx, el, "body");
        if (JS_IsException(bd))
            goto err;
        fn = JS_GetPropertyStr(ctx, el, "filename");
        if (JS_IsException(fn))
            goto err;
        ct = JS_GetPropertyStr(ctx, el, "contentType");
        if (JS_IsException(ct))
            goto err;
        if (!JS_IsUndefined(val) && !JS_IsUndefined(bd)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: a part must have value OR body, not both");
            goto err;
        }
        if (!JS_IsUndefined(bd)) {
            if (JS_IsException(dyn_mp_body_view(ctx, bd, &payload)))
                goto err;
            have_payload = 1;
        } else if (!JS_IsUndefined(val)) {
            if (!JS_IsString(val)) {
                JS_ThrowTypeError(ctx, "MultipartFormat: value must be a string");
                goto err;
            }
            payload.p = (const uint8_t*)JS_ToCStringLen(ctx, &payload.n, val);
            if (!payload.p)
                goto err;
            payload.owned = DYN_UNCONST(payload.p);
            have_payload = 1;
        } else {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: a part needs value or body");
            goto err;
        }
        dyn_sb_putc(&out, '-');
        dyn_sb_putc(&out, '-');
        dyn_sb_put(&out, bnd, blen);
        dyn_sb_puts(&out, "\r\nContent-Disposition: form-data; name=");
        dyn_mp_escape(&out, ns, nn);
        if (!JS_IsUndefined(fn)) {
            const char* fs;
            size_t fnl = 0;
            if (!JS_IsString(fn)) {
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: filename must be a string");
                goto err;
            }
            fs = JS_ToCStringLen(ctx, &fnl, fn);
            if (!fs)
                goto err;
            if (dyn_mp_ctl(fs, fnl)) {
                JS_FreeCString(ctx, fs);
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: filename must not contain control characters");
                goto err;
            }
            dyn_sb_puts(&out, "; filename=");
            dyn_mp_escape(&out, fs, fnl);
            JS_FreeCString(ctx, fs);
        }
        dyn_sb_puts(&out, "\r\n");
        if (!JS_IsUndefined(ct)) {
            const char* cts;
            size_t ctn2 = 0;
            if (!JS_IsString(ct)) {
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: contentType must be a string");
                goto err;
            }
            cts = JS_ToCStringLen(ctx, &ctn2, ct);
            if (!cts)
                goto err;
            if (dyn_mp_ctl(cts, ctn2)) {
                JS_FreeCString(ctx, cts);
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: contentType must not contain control characters");
                goto err;
            }
            dyn_sb_puts(&out, "Content-Type: ");
            dyn_sb_put(&out, cts, ctn2);
            dyn_sb_puts(&out, "\r\n");
            JS_FreeCString(ctx, cts);
        }
        dyn_sb_puts(&out, "\r\n");
        if (have_payload)
            dyn_sb_put(&out, (const char*)payload.p, payload.n);
        dyn_sb_puts(&out, "\r\n");
        if (out.oom) {
            JS_ThrowOutOfMemory(ctx);
            goto err;
        }
        dyn_mp_body_free(ctx, &payload);
        payload.p = NULL;
        payload.n = 0;
        JS_FreeCString(ctx, ns);
        ns = NULL;
        nn = 0;
        JS_FreeValue(ctx, el);
        el = JS_UNDEFINED;
        JS_FreeValue(ctx, nm);
        nm = JS_UNDEFINED;
        JS_FreeValue(ctx, val);
        val = JS_UNDEFINED;
        JS_FreeValue(ctx, bd);
        bd = JS_UNDEFINED;
        JS_FreeValue(ctx, fn);
        fn = JS_UNDEFINED;
        JS_FreeValue(ctx, ct);
        ct = JS_UNDEFINED;
    }
    dyn_sb_put(&out, "--", 2);
    dyn_sb_put(&out, bnd, blen);
    dyn_sb_puts(&out, "--\r\n");
    if (out.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto err;
    }
    {
        dyn_sb_t hdrb;
        JSValue o, ab, ta;
        JSValueConst ta3[3];
        dyn_sb_init(&hdrb);
        dyn_sb_puts(&hdrb, "multipart/form-data; boundary=");
        dyn_sb_put(&hdrb, bnd, blen);
        if (hdrb.oom) {
            dyn_sb_free(&hdrb);
            JS_ThrowOutOfMemory(ctx);
            goto err;
        }
        o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            dyn_sb_free(&hdrb);
            goto err;
        }
        int r1 = JS_DefinePropertyValueStr(ctx, o, "contentType",
            JS_NewStringLen(ctx, hdrb.p ? hdrb.p : "", hdrb.n), JS_PROP_C_W_E);
        int r2 = r1 < 0 ? -1 : JS_DefinePropertyValueStr(ctx, o, "boundary",
            JS_NewStringLen(ctx, bnd, blen), JS_PROP_C_W_E);
        dyn_sb_free(&hdrb);
        if (r2 < 0) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        ab = JS_NewArrayBufferCopy(ctx,
            (const uint8_t*)(out.p ? out.p : ""), out.n);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        ta3[0] = ab;
        ta3[1] = JS_UNDEFINED;
        ta3[2] = JS_UNDEFINED;
        ta = JS_NewTypedArray(ctx, 3, ta3, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(ta)) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        if (JS_DefinePropertyValueStr(ctx, o, "body", ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        ret = o;
    }
done:
    dyn_sb_free(&out);
    return ret;
err:
    dyn_mp_body_free(ctx, &payload);
    if (ns)
        JS_FreeCString(ctx, ns);
    JS_FreeValue(ctx, el);
    JS_FreeValue(ctx, nm);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, bd);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ct);
    dyn_sb_free(&out);
    return JS_EXCEPTION;
}

