#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
char* dyn_headers_to_string(JSContext* ctx, JSValueConst headers,
    int* perr)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    dyn_bytes_t buf = { 0 };

    *perr = 0;
    if (JS_IsUndefined(headers) || JS_IsNull(headers))
        return NULL;

    if (JS_IsString(headers)) {
        const char* s = JS_ToCString(ctx, headers);
        dyn_bytes_t b = { 0 };
        size_t hi, n;
        if (!s) {
            *perr = 1;
            return NULL;
        }
        n = strlen(s);
        {
            size_t vlen = n;
            if (vlen >= 2 && s[vlen - 2] == '\r' && s[vlen - 1] == '\n')
                vlen -= 2;
            if (memchr(s, '\r', vlen) || memchr(s, '\n', vlen)) {
                JS_FreeCString(ctx, s);
                *perr = 1;
                JS_ThrowTypeError(ctx,
                    "header name/value must not contain CR or LF");
                return NULL;
            }
        }
        hi = 0;
        while (hi < n) {
            size_t ls = hi, le;
            const char* colon;
            size_t nlen;
            while (hi + 1 < n && !(s[hi] == '\r' && s[hi + 1] == '\n'))
                hi++;
            if (hi + 1 < n) {
                le = hi;
                hi += 2;
            } else {
                le = n;
                hi = n;
            }
            if (le > ls) {
                colon = memchr(s + ls, ':', le - ls);
                nlen = colon ? (size_t)(colon - (s + ls)) : 0;
                if (!colon || !httpk_name_ok(s + ls, nlen)) {
                    JS_FreeCString(ctx, s);
                    free(b.data);
                    *perr = 1;
                    JS_ThrowTypeError(ctx,
                        "a header string must be \"Name: value\" with an HTTP token name");
                    return NULL;
                }
                if (!httpk_is_framing_name(s + ls, nlen)) {
                    if (dyn_bytes_append(&b, s + ls, le - ls) < 0 || dyn_bytes_append(&b, "\r\n", 2) < 0)
                        goto strform_oom;
                }
            }
        }
        if (dyn_bytes_append(&b, "\0", 1) < 0)
            goto strform_oom;
        JS_FreeCString(ctx, s);
        return b.data;
    strform_oom:
        JS_FreeCString(ctx, s);
        free(b.data);
        *perr = 1;
        return NULL;
    }

    if (!JS_IsObject(headers))
        return NULL;
    if (JS_IsArray(ctx, headers)) {
        *perr = 1;
        JS_ThrowTypeError(ctx,
            "headers must be an object of name/value pairs or a "
            "pre-formatted string, not an array");
        return NULL;
    }

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        *perr = 1;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        const char* name = JS_AtomToCString(ctx, tab[i].atom);
        const char* val;
        JSValue v;
        size_t nl, vl;

        if (!name)
            goto fail;
        v = JS_GetProperty(ctx, headers, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        val = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!val) {
            JS_FreeCString(ctx, name);
            goto fail;
        }
        nl = strlen(name);
        vl = strlen(val);
        if (memchr(name, '\r', nl) || memchr(name, '\n', nl) || memchr(val, '\r', vl) || memchr(val, '\n', vl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                "header name/value must not contain CR or LF");
            return NULL;
        }
        if (!httpk_name_ok(name, nl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            free(buf.data);
            JS_FreePropertyEnum(ctx, tab, len);
            *perr = 1;
            JS_ThrowTypeError(ctx,
                "header name must be a valid HTTP token "
                "(no spaces, controls or separators)");
            return NULL;
        }
        if (httpk_is_framing_name(name, nl)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            continue;
        }
        if (dyn_bytes_append(&buf, name, nl) < 0 || dyn_bytes_append(&buf, ": ", 2) < 0 || dyn_bytes_append(&buf, val, vl) < 0 || dyn_bytes_append(&buf, "\r\n", 2) < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, val);
            goto fail;
        }
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (buf.data) {
        if (dyn_bytes_append(&buf, "\0", 1) < 0) {
            free(buf.data);
            return NULL;
        }
    }
    return buf.data;

fail:
    free(buf.data);
    JS_FreePropertyEnum(ctx, tab, len);
    *perr = 1;
    return NULL;
}

int dyn_head_parse(const char* raw, size_t head_len, httpk_head_t* h,
    httpk_hdr_t* small, size_t nsmall, httpk_hdr_t** heap)
{
    size_t lines = 0, i;
    *heap = NULL;
    for (i = 0; i < head_len; i++)
        lines += raw[i] == '\n';
    memset(h, 0, sizeof *h);
    if (lines <= nsmall) {
        h->hdrs = small;
        h->cap_hdrs = (uint32_t)nsmall;
    } else {
        *heap = (httpk_hdr_t*)malloc(lines * sizeof **heap);
        if (!*heap)
            return HTTPK_E_TOO_MANY;
        h->hdrs = *heap;
        h->cap_hdrs = (uint32_t)lines;
    }
    return httpk_resp_head(raw, head_len, h);
}

static uint32_t dyn_hdr_ci_hash(const char* s, size_t n)
{
    uint32_t x = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        x = (x ^ c) * 16777619u;
    }
    return x;
}

void dyn_head_why(const char* raw, const httpk_head_t* h, int rc, char* why,
    size_t whyn)
{
    if (rc == HTTPK_E_DUP && h->dup_field < h->n_hdrs) {
        const httpk_hdr_t* f = &h->hdrs[h->dup_field];
        snprintf(why, whyn, "conflicting %.*s values",
            (int)(f->name_len > 64 ? 64 : f->name_len), raw + f->name);
    } else {
        snprintf(why, whyn, "%s", httpk_strerror(rc));
    }
}

JSValue dyn_headers_js(JSContext* ctx, const char* raw, const httpk_head_t* h)
{
    uint32_t n = h->n_hdrs, tsize = 16, i, *mem, *next, *tail, *tab;
    JSValue obj = JS_NewObject(ctx);
    dyn_sb_t sb;
    if (JS_IsException(obj) || n == 0)
        return obj;
    while (tsize < 2 * n)
        tsize <<= 1;
    mem = (uint32_t*)calloc((size_t)2 * n + tsize, sizeof *mem);
    if (!mem) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    next = mem;
    tail = mem + n;
    tab = mem + 2 * n;
    for (i = 0; i < n; i++) {
        const httpk_hdr_t* f = &h->hdrs[i];
        uint32_t slot = dyn_hdr_ci_hash(raw + f->name, f->name_len) & (tsize - 1);
        next[i] = UINT32_MAX;
        tail[i] = i;
        for (;;) {
            uint32_t j = tab[slot];
            if (!j) {
                tab[slot] = i + 1;
                break;
            }
            j--;
            if (h->hdrs[j].name_len == f->name_len
                && strncasecmp(raw + h->hdrs[j].name, raw + f->name, f->name_len) == 0) {
                next[tail[j]] = i;
                tail[j] = i;
                tail[i] = UINT32_MAX;
                break;
            }
            slot = (slot + 1) & (tsize - 1);
        }
    }
    dyn_sb_init(&sb);
    for (i = 0; i < n; i++) {
        const httpk_hdr_t* f = &h->hdrs[i];
        JSAtom key;
        JSValue v;
        uint32_t k;
        int kind;
        if (tail[i] == UINT32_MAX)
            continue;
        kind = httpk_field_kind(raw + f->name, f->name_len);
        sb.n = 0;
        dyn_sb_put(&sb, raw + f->value, f->value_len);
        if (kind != HTTPK_FIELD_SINGLETON) {
            for (k = next[i]; k != UINT32_MAX; k = next[k]) {
                const httpk_hdr_t* g = &h->hdrs[k];
                dyn_sb_put(&sb, kind == HTTPK_FIELD_SET_COOKIE ? "\n" : ", ",
                    kind == HTTPK_FIELD_SET_COOKIE ? 1 : 2);
                dyn_sb_put(&sb, raw + g->value, g->value_len);
            }
        }
        if (sb.oom) {
            dyn_sb_free(&sb);
            free(mem);
            JS_FreeValue(ctx, obj);
            return JS_ThrowOutOfMemory(ctx);
        }
        key = JS_NewAtomLen(ctx, raw + f->name, f->name_len);
        v = key == JS_ATOM_NULL ? JS_EXCEPTION : JS_NewStringLen(ctx, sb.p ? sb.p : "", sb.n);
        if (JS_IsException(v) || JS_DefinePropertyValue(ctx, obj, key, v, JS_PROP_C_W_E) < 0) {
            if (key != JS_ATOM_NULL)
                JS_FreeAtom(ctx, key);
            dyn_sb_free(&sb);
            free(mem);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, key);
    }
    dyn_sb_free(&sb);
    free(mem);
    return obj;
}

#endif
