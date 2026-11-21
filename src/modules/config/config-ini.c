#include "config-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#define DYN_CFG_MAX_DEPTH 16

static JSValue dyn_get_own(JSContext* ctx, JSValueConst obj, const char* k,
    size_t klen)
{
    JSAtom a = JS_NewAtomLen(ctx, k, klen);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &desc, obj, a);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

static JSValue dyn_ini_value(JSContext* ctx, const char* s, size_t b, size_t e)
{
    char* buf;
    size_t o = 0, i;
    char q;
    if (e - b >= 2 && (s[b] == '"' || s[b] == '\'') && s[e - 1] == s[b]) {
        q = s[b];
        b++;
        e--;
    } else {
        return JS_NewStringLen(ctx, s + b, e - b);
    }
    buf = (char*)js_malloc(ctx, (e - b) + 1);
    if (!buf)
        return JS_EXCEPTION;
    for (i = b; i < e; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < e) {
            char d = s[++i];
            switch (d) {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            case '0':
                c = '\0';
                break;
            default:
                c = d;
                break;
            }
        }
        buf[o++] = c;
    }
    (void)q;
    {
        JSValue v = JS_NewStringLen(ctx, buf, o);
        js_free(ctx, buf);
        return v;
    }
}

static JSValue dyn_ini_section(JSContext* ctx, JSValueConst root,
    const char* s, size_t b, size_t e)
{
    JSValue cur = JS_DupValue(ctx, root);
    int depth = 0;
    while (b < e) {
        size_t seg = b, kb, ke;
        JSValue next;
        while (seg < e && s[seg] != '.')
            seg++;
        kb = b;
        ke = seg;
        dyn_trim(s, &kb, &ke);
        if (++depth > DYN_CFG_MAX_DEPTH) {
            JS_FreeValue(ctx, cur);
            JS_ThrowRangeError(ctx, "INI.parse: section nesting deeper than %d",
                DYN_CFG_MAX_DEPTH);
            return JS_EXCEPTION;
        }
        next = dyn_get_own(ctx, cur, s + kb, ke - kb);
        if (JS_IsException(next)) {
            JS_FreeValue(ctx, cur);
            return JS_EXCEPTION;
        }
        if (!JS_IsObject(next)) {
            JS_FreeValue(ctx, next);
            next = JS_NewObject(ctx);
            if (JS_IsException(next)) {
                JS_FreeValue(ctx, cur);
                return JS_EXCEPTION;
            }
            if (dyn_def(ctx, cur, s + kb, ke - kb, JS_DupValue(ctx, next)) < 0) {
                JS_FreeValue(ctx, next);
                JS_FreeValue(ctx, cur);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, cur);
        cur = next;
        b = (seg < e) ? seg + 1 : e;
    }
    return cur;
}

static int dyn_ini_append(JSContext* ctx, JSValueConst obj, const char* s,
    size_t kb, size_t ke, JSValue val)
{
    JSValue arr = dyn_get_own(ctx, obj, s + kb, ke - kb);
    int64_t len = 0;
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    if (!JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) {
            JS_FreeValue(ctx, val);
            return -1;
        }
        if (dyn_def(ctx, obj, s + kb, ke - kb, JS_DupValue(ctx, arr)) < 0) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, val);
            return -1;
        }
    } else {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, val);
            return -1;
        }
        JS_FreeValue(ctx, lv);
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)len, val,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    JS_FreeValue(ctx, arr);
    return 0;
}

static JSValue dyn_ini_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *buf, *src;
    size_t n, lb, le;
    dyn_cur_t cur;
    JSValue root, sect = JS_UNDEFINED;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "INI.parse(text): argument must be a string");
    buf = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!buf)
        return JS_EXCEPTION;
    src = buf;
    dyn_cfg_bom(&src, &n);
    root = JS_NewObject(ctx);
    if (JS_IsException(root)) {
        JS_FreeCString(ctx, buf);
        return JS_EXCEPTION;
    }
    sect = JS_DupValue(ctx, root);

    cur.s = src;
    cur.n = n;
    cur.i = 0;
    while (dyn_next_line(&cur, &lb, &le)) {
        size_t b = lb, e = le, eq, kb, ke, vb, ve;
        int is_append = 0;
        dyn_trim(src, &b, &e);
        if (b == e || src[b] == ';' || src[b] == '#')
            continue;
        if (src[b] == '[') {
            size_t sb = b + 1, se = e;
            while (se > sb && src[se - 1] != ']')
                se--;
            if (se <= sb)
                continue;
            se--;
            JS_FreeValue(ctx, sect);
            sect = dyn_ini_section(ctx, root, src, sb, se);
            if (JS_IsException(sect))
                goto fail;
            continue;
        }
        for (eq = b; eq < e && src[eq] != '='; eq++)
            ;
        kb = b;
        ke = (eq < e) ? eq : e;
        dyn_trim(src, &kb, &ke);
        if (ke - kb >= 2 && src[ke - 2] == '[' && src[ke - 1] == ']') {
            is_append = 1;
            ke -= 2;
            dyn_trim(src, &kb, &ke);
        }
        if (ke == kb)
            continue;
        if (eq >= e) {
            if (dyn_def(ctx, sect, src + kb, ke - kb, JS_TRUE) < 0)
                goto fail;
            continue;
        }
        vb = eq + 1;
        ve = e;
        dyn_trim(src, &vb, &ve);
        {
            JSValue v = dyn_ini_value(ctx, src, vb, ve);
            if (JS_IsException(v))
                goto fail;
            if (is_append) {
                if (dyn_ini_append(ctx, sect, src, kb, ke, v) < 0)
                    goto fail;
            } else if (dyn_def(ctx, sect, src + kb, ke - kb, v) < 0) {
                goto fail;
            }
        }
    }
    JS_FreeValue(ctx, sect);
    JS_FreeCString(ctx, buf);
    return root;
fail:
    JS_FreeValue(ctx, sect);
    JS_FreeValue(ctx, root);
    JS_FreeCString(ctx, buf);
    return JS_EXCEPTION;
}

static int dyn_ini_emit_pair(JSContext* ctx, JSValueConst obj,
    dyn_tml_sb_t* out, JSAtom atom)
{
    JSValue val = JS_GetProperty(ctx, obj, atom);
    const char* key;
    size_t klen = 0;
    const char* raw;
    size_t rawlen = 0;
    char numbuf[40];

    if (JS_IsException(val))
        return -1;
    key = JS_AtomToCStringLen(ctx, &klen, atom);
    if (!key)
        goto fail;
    if (klen == 0 || memchr(key, '=', klen) || memchr(key, '\n', klen) || memchr(key, '[', klen) || memchr(key, ']', klen)) {
        JS_ThrowTypeError(ctx, "INI.stringify: invalid key");
        goto fail;
    }
    if (JS_IsBool(val)) {
        raw = JS_ToBool(ctx, val) ? "true" : "false";
        rawlen = strlen(raw);
    } else if (JS_IsNumber(val)) {
        double d;
        if (JS_ToFloat64(ctx, &d, val) || !isfinite(d)) {
            JS_ThrowTypeError(ctx, "INI.stringify: numbers must be finite");
            goto fail;
        }
        dyn_cfg_dtoa(numbuf, d);
        raw = numbuf;
        rawlen = strlen(numbuf);
    } else if (JS_IsString(val)) {
        raw = JS_ToCStringLen(ctx, &rawlen, val);
        if (!raw)
            goto fail;
    } else {
        JS_ThrowTypeError(ctx,
            "INI.stringify: values must be strings, numbers or booleans");
        goto fail;
    }
    dyn_tml_sb_put(out, key, klen);
    dyn_tml_sb_puts(out, " = ");
    if (JS_IsString(val) && dyn_ini_needs_quotes(raw, rawlen)) {
        size_t j;
        dyn_tml_sb_putc(out, '"');
        for (j = 0; j < rawlen; j++) {
            char c = raw[j];
            switch (c) {
            case '"':
                dyn_tml_sb_puts(out, "\\\"");
                break;
            case '\\':
                dyn_tml_sb_puts(out, "\\\\");
                break;
            case '\n':
                dyn_tml_sb_puts(out, "\\n");
                break;
            case '\r':
                dyn_tml_sb_puts(out, "\\r");
                break;
            case '\t':
                dyn_tml_sb_puts(out, "\\t");
                break;
            default:
                dyn_tml_sb_putc(out, c);
                break;
            }
        }
        dyn_tml_sb_putc(out, '"');
    } else {
        dyn_tml_sb_put(out, raw, rawlen);
    }
    dyn_tml_sb_putc(out, '\n');
    if (JS_IsString(val))
        JS_FreeCString(ctx, raw);
    JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, val);
    return 0;
fail:
    if (key)
        JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, val);
    return -1;
}

static JSValue dyn_ini_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSPropertyEnum* tab = NULL;
    uint32_t tn = 0, k;
    dyn_tml_sb_t out;
    JSValue str;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "INI.stringify(record): the record must be an object");
    dyn_tml_sb_init(&out);
    if (JS_GetOwnPropertyNames(ctx, &tab, &tn, argv[0],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return JS_EXCEPTION;
    for (k = 0; k < tn; k++) {
        JSValue val = JS_GetProperty(ctx, argv[0], tab[k].atom);
        if (JS_IsException(val)) {
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
        if (JS_IsObject(val) && !JS_IsArray(ctx, val)) {
            JS_FreeValue(ctx, val);
            continue;
        }
        JS_FreeValue(ctx, val);
        if (dyn_ini_emit_pair(ctx, argv[0], &out, tab[k].atom) < 0) {
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
    }
    for (k = 0; k < tn; k++) {
        JSValue val = JS_GetProperty(ctx, argv[0], tab[k].atom);
        const char* key;
        size_t keylen;
        if (JS_IsException(val)) {
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
        if (!(JS_IsObject(val) && !JS_IsArray(ctx, val))) {
            JS_FreeValue(ctx, val);
            continue;
        }
        JS_FreeValue(ctx, val);
        key = JS_AtomToCStringLen(ctx, &keylen, tab[k].atom);
        if (!key || keylen == 0 || memchr(key, ']', keylen) || memchr(key, '\n', keylen)) {
            JS_FreeCString(ctx, key);
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_ThrowTypeError(ctx,
                "INI.stringify: section name must be non-empty and free of ] and newline");
        }
        dyn_tml_sb_putc(&out, '\n');
        dyn_tml_sb_putc(&out, '[');
        dyn_tml_sb_put(&out, key, keylen);
        dyn_tml_sb_puts(&out, "]\n");
        JS_FreeCString(ctx, key);
        {
            JSPropertyEnum* stab = NULL;
            uint32_t sn = 0, j;
            JSValue sect = JS_GetProperty(ctx, argv[0], tab[k].atom);
            if (JS_IsException(sect) || JS_GetOwnPropertyNames(ctx, &stab, &sn, sect, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, sect);
                JS_FreePropertyEnum(ctx, tab, tn);
                dyn_tml_sb_free(&out);
                return JS_EXCEPTION;
            }
            for (j = 0; j < sn; j++) {
                if (dyn_ini_emit_pair(ctx, sect, &out, stab[j].atom) < 0) {
                    JS_FreePropertyEnum(ctx, stab, sn);
                    JS_FreeValue(ctx, sect);
                    JS_FreePropertyEnum(ctx, tab, tn);
                    dyn_tml_sb_free(&out);
                    return JS_EXCEPTION;
                }
            }
            JS_FreePropertyEnum(ctx, stab, sn);
            JS_FreeValue(ctx, sect);
        }
    }
    JS_FreePropertyEnum(ctx, tab, tn);
    str = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
    dyn_tml_sb_free(&out);
    return str;
}

const JSCFunctionListEntry dyn_ini_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_ini_parse),
    JS_CFUNC_DEF("stringify", 1, dyn_ini_stringify),
};

#endif
