#include "csv-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CSV)

char* opt_str(JSContext* ctx, JSValueConst obj, const char* key, int* present)
{
    if (present)
        *present = 0;
    if (!JS_IsObject(obj))
        return NULL;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char* s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return NULL;
    char* d = strdup(s);
    JS_FreeCString(ctx, s);
    if (present)
        *present = 1;
    return d;
}

int opt_int(JSContext* ctx, JSValueConst obj, const char* key, int64_t def,
    int64_t* out, int* present)
{
    if (present)
        *present = 0;
    if (!JS_IsObject(obj)) {
        *out = def;
        return 0;
    }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        *out = def;
        return 0;
    }
    int r = JS_ToInt64(ctx, out, v);
    JS_FreeValue(ctx, v);
    if (r)
        return -1;
    if (present)
        *present = 1;
    return 0;
}

int opt_bool_def(JSContext* ctx, JSValueConst obj, const char* key, int def)
{
    if (!JS_IsObject(obj))
        return def;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return def;
    }
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

int opt_bool(JSContext* ctx, JSValueConst obj, const char* key)
{
    return opt_bool_def(ctx, obj, key, 0);
}

int opt_durable(JSContext* ctx, JSValueConst obj)
{
    return opt_bool(ctx, obj, "durable");
}

static void str_array_free(char** a, uint32_t n)
{
    for (uint32_t j = 0; j < n; j++)
        free(a[j]);
    free(a);
}

int opt_str_array(JSContext* ctx, JSValueConst obj, const char* key, char*** arr)
{
    *arr = NULL;
    if (!JS_IsObject(obj))
        return 0;
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "csv: '%s' must be an array", key);
        return -1;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0, n = 0, cap = 0;
    char** out = NULL;
    if (JS_ToUint32(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len > INT_MAX) {
        JS_FreeValue(ctx, v);
        JS_ThrowRangeError(ctx, "csv: '%s' has more entries than a header can hold", key);
        return -1;
    }
    for (; n < len; n++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, n);
        if (JS_IsException(e))
            goto fail;
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "csv: '%s'[%u] must be a string", key, n);
            goto fail;
        }
        if (n == cap) {
            uint32_t ncap = cap ? cap * 2 : 8;
            char** grown = (char**)realloc(out, (size_t)ncap * sizeof(char*));
            if (!grown) {
                JS_FreeValue(ctx, e);
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            out = grown;
            cap = ncap;
        }
        const char* s = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!s)
            goto fail;
        out[n] = strdup(s);
        JS_FreeCString(ctx, s);
        if (!out[n]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    JS_FreeValue(ctx, v);
    *arr = out;
    return (int)n;
fail:
    str_array_free(out, n);
    JS_FreeValue(ctx, v);
    return -1;
}

int opts_check(JSContext* ctx, JSValueConst obj, const char* pfx,
    const char* const* keys, size_t nkeys)
{
    JSPropertyEnum* tab = NULL;
    uint32_t n, i;
    if (!JS_IsObject(obj))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, obj, JS_GPN_STRING_MASK) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char* key = JS_AtomToCString(ctx, tab[i].atom);
        int known;
        if (!key) {
            JS_FreePropertyEnum(ctx, tab, n);
            return -1;
        }
        known = 0;
        for (size_t k = 0; k < nkeys; k++)
            if (strcmp(key, keys[k]) == 0) {
                known = 1;
                break;
            }
        if (!known) {
            Buf vb;
            memset(&vb, 0, sizeof(vb));
            for (size_t k = 0; k < nkeys; k++) {
                if ((k && buf_put(&vb, ", ", 2)) || buf_put(&vb, keys[k], strlen(keys[k])))
                    break;
            }
            if (buf_putc(&vb, '\0')) {
                buf_free(&vb);
                vb.p = NULL;
            }
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\" (valid: %s)",
                pfx, key, vb.p ? vb.p : "");
            buf_free(&vb);
            JS_FreePropertyEnum(ctx, tab, n);
            JS_FreeCString(ctx, key);
            return -1;
        }
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return 0;
}

static int opts_one_char(JSContext* ctx, const char* pfx, const char* key,
    char* s, uint8_t* out,
    uint8_t reserved, const char* reserved_role)
{
    size_t slen = strlen(s);
    if (slen != 1) {
        JS_ThrowTypeError(ctx, "%s: '%s' must be exactly one ASCII character", pfx, key);
        free(s);
        return -1;
    }
    if (s[0] == (char)reserved) {
        JS_ThrowTypeError(ctx, "%s: '%s' must not be the %s character '%c'", pfx, key, reserved_role, reserved);
        free(s);
        return -1;
    }
    if (s[0] == '\n' || s[0] == '\r') {
        JS_ThrowTypeError(ctx, "%s: '%s' must not be CR or LF", pfx, key);
        free(s);
        return -1;
    }
    *out = (uint8_t)s[0];
    free(s);
    return 0;
}

int opts_delim_quote(JSContext* ctx, JSValueConst obj, const char* pfx,
    uint8_t* pdelim, uint8_t* pquote)
{
    *pdelim = ',';
    *pquote = '"';
    char* s;
    s = opt_str(ctx, obj, "delimiter", NULL);
    if (!s && JS_HasException(ctx))
        return -1;
    if (s && opts_one_char(ctx, pfx, "delimiter", s, pdelim, '"', "quote"))
        return -1;
    s = opt_str(ctx, obj, "quote", NULL);
    if (!s && JS_HasException(ctx))
        return -1;
    if (s && opts_one_char(ctx, pfx, "quote", s, pquote, ',', "delimiter"))
        return -1;
    if (*pdelim == *pquote) {
        JS_ThrowTypeError(ctx, "%s: 'delimiter' and 'quote' must differ", pfx);
        return -1;
    }
    return 0;
}

#endif
