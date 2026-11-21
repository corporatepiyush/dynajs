#include "config-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#define DYN_ENV_FILE_MAX (8u << 20)
#define DYN_ENV_VALUE_MAX (1u << 20)

static JSValue dyn_env_unescape(JSContext* ctx, const char* s, size_t b, size_t e)
{
    char* buf = (char*)js_malloc(ctx, (e - b) + 1);
    size_t o = 0, k;
    JSValue v;
    if (!buf)
        return JS_EXCEPTION;
    for (k = b; k < e; k++) {
        char c = s[k];
        if (c == '\\' && k + 1 < e) {
            char d = s[++k];
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
    v = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return v;
}

static JSValue dyn_env_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *buf, *src;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Env.parse(text): argument must be a string");
    buf = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!buf)
        return JS_EXCEPTION;
    src = buf;
    dyn_cfg_bom(&src, &n);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, buf);
        return JS_EXCEPTION;
    }

    while (dyn_env_next_record(src, n, &i)) {
        size_t kb, ke, vb, ve;
        char q;
        JSValue v;
        if (n - i >= 7 && memcmp(src + i, "export ", 7) == 0)
            i += 7;
        kb = i;
        while (i < n && src[i] != '=' && src[i] != '\n')
            i++;
        if (i >= n || src[i] != '=') {
            while (i < n && src[i] != '\n')
                i++;
            continue;
        }
        ke = i++;
        dyn_trim(src, &kb, &ke);
        dyn_env_read_value(src, n, &i, &vb, &ve, &q);
        if (ke == kb)
            continue;
        v = (q == '"') ? dyn_env_unescape(ctx, src, vb, ve)
                       : JS_NewStringLen(ctx, src + vb, ve - vb);
        if (JS_IsException(v) || dyn_def(ctx, obj, src + kb, ke - kb, v) < 0) {
            JS_FreeValue(ctx, obj);
            JS_FreeCString(ctx, buf);
            return JS_EXCEPTION;
        }
    }
    JS_FreeCString(ctx, buf);
    return obj;
}

static const char* dyn_env_get_len(const char* name, size_t* plen)
{
    const char* val = getenv(name);
    *plen = val ? strlen(val) : 0;
    return val;
}

static JSValue dyn_env_expand_value(JSContext* ctx, const char* s, size_t n)
{
    char* buf;
    size_t o = 0, cap = n + 32, i = 0;
    JSValue out;

    buf = (char*)malloc(cap);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
#define PUT(str, len)                                                              \
    do {                                                                           \
        size_t L_ = (len);                                                         \
        if (o + L_ > DYN_ENV_VALUE_MAX) {                                          \
            free(buf);                                                             \
            return JS_ThrowRangeError(ctx,                                         \
                "Env.load: expanded value exceeds %u bytes",                       \
                (unsigned)DYN_ENV_VALUE_MAX);                                      \
        }                                                                          \
        if (o + L_ > cap && !dyn_sb_reserve((void**)&buf, &cap, o + L_, n + 32)) { \
            free(buf);                                                             \
            return JS_EXCEPTION;                                                   \
        }                                                                          \
        memcpy(buf + o, (str), L_);                                                \
        o += L_;                                                                   \
    } while (0)
    while (i < n) {
        size_t nl = 0;
        const char* val;
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '$') {
            PUT("$", 1);
            i += 2;
            continue;
        }
        if (s[i] != '$') {
            PUT(s + i, 1);
            i++;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '{') {
            size_t j = i + 2;
            while (j < n && s[j] != '}')
                j++;
            if (j >= n) {
                PUT(s + i, 1);
                i++;
                continue;
            }
            {
                const char* nm = s + i + 2;
                char* tmp;
                const char* vstr;
                size_t vlen;
                nl = j - (i + 2);
                tmp = (char*)js_malloc(ctx, nl + 1);
                if (!tmp) {
                    free(buf);
                    return JS_ThrowOutOfMemory(ctx);
                }
                if (nl)
                    memcpy(tmp, nm, nl);
                tmp[nl] = '\0';
                vstr = dyn_env_get_len(tmp, &vlen);
                js_free(ctx, tmp);
                if (vstr)
                    PUT(vstr, vlen);
            }
            i = j + 1;
            continue;
        }
        {
            size_t j = i + 1;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_'))
                j++;
            if (j == i + 1) {
                PUT(s + i, 1);
                i++;
                continue;
            }
            {
                char* tmp = (char*)js_malloc(ctx, j - i);
                size_t vlen;
                if (!tmp) {
                    free(buf);
                    return JS_ThrowOutOfMemory(ctx);
                }
                memcpy(tmp, s + i + 1, j - i - 1);
                tmp[j - i - 1] = '\0';
                val = dyn_env_get_len(tmp, &vlen);
                js_free(ctx, tmp);
                if (val)
                    PUT(val, vlen);
            }
            i = j;
        }
    }
#undef PUT
    out = JS_NewStringLen(ctx, buf, o);
    free(buf);
    return out;
}

static JSValue dyn_env_unescape_expand(JSContext* ctx, const char* s,
    size_t b, size_t e)
{
    char* buf;
    size_t o = 0, cap = (e - b) + 32, i = b;
    JSValue out;

    buf = (char*)malloc(cap);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
#define EPUT(str, len)                                                                   \
    do {                                                                                 \
        size_t L_ = (len);                                                               \
        if (o + L_ > DYN_ENV_VALUE_MAX) {                                                \
            free(buf);                                                                   \
            return JS_ThrowRangeError(ctx,                                               \
                "Env.load: expanded value exceeds %u bytes",                             \
                (unsigned)DYN_ENV_VALUE_MAX);                                            \
        }                                                                                \
        if (o + L_ > cap && !dyn_sb_reserve((void**)&buf, &cap, o + L_, (e - b) + 32)) { \
            free(buf);                                                                   \
            return JS_ThrowOutOfMemory(ctx);                                             \
        }                                                                                \
        memcpy(buf + o, (str), L_);                                                      \
        o += L_;                                                                         \
    } while (0)
    while (i < e) {
        char c = s[i];
        const char* val;
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
            case '$':
                EPUT("$", 1);
                i++;
                goto next;
            default:
                c = d;
                break;
            }
            EPUT(&c, 1);
            i++;
            goto next;
        }
        if (c == '$') {
            size_t j;
            if (i + 1 < e && s[i + 1] == '{') {
                j = i + 2;
                while (j < e && s[j] != '}')
                    j++;
                if (j < e) {
                    size_t nl = j - (i + 2);
                    size_t vlen;
                    char* tmp = (char*)js_malloc(ctx, nl + 1);
                    if (!tmp) {
                        free(buf);
                        return JS_ThrowOutOfMemory(ctx);
                    }
                    if (nl)
                        memcpy(tmp, s + i + 2, nl);
                    tmp[nl] = '\0';
                    val = dyn_env_get_len(tmp, &vlen);
                    js_free(ctx, tmp);
                    if (val)
                        EPUT(val, vlen);
                    i = j + 1;
                    goto next;
                }
                EPUT("$", 1);
                i++;
                goto next;
            }
            j = i + 1;
            while (j < e && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_'))
                j++;
            if (j == i + 1) {
                EPUT("$", 1);
                i++;
                goto next;
            }
            {
                char* tmp = (char*)js_malloc(ctx, j - i);
                size_t vlen;
                if (!tmp) {
                    free(buf);
                    return JS_ThrowOutOfMemory(ctx);
                }
                memcpy(tmp, s + i + 1, j - i - 1);
                tmp[j - i - 1] = '\0';
                val = dyn_env_get_len(tmp, &vlen);
                js_free(ctx, tmp);
                if (val)
                    EPUT(val, vlen);
            }
            i = j;
            goto next;
        }
        EPUT(&c, 1);
        i++;
    next:
        continue;
    }
#undef EPUT
    out = JS_NewStringLen(ctx, buf, o);
    free(buf);
    return out;
}

static const char* const dyn_env_load_keys[] = { "assign", "override", "expand" };

static void dyn_env_order_free(JSContext* ctx, JSAtom* order, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        JS_FreeAtom(ctx, order[i]);
    js_free(ctx, order);
}

static JSValue dyn_env_load(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    int assign = 1, override = 0, expand = 0;
    int fd;
    struct stat st;
    char* buf;
    size_t n, i = 0;
    JSValue obj;
    JSAtom* order = NULL;
    size_t nord = 0, ocap = 0, k;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Env.load(path): path must be a string");
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        int* slots[3] = { &assign, &override, &expand };
        if (!JS_IsObject(argv[1])) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "Env.load: options must be an object");
        }
        if (dyn_opts_strict(ctx, argv[1], dyn_env_load_keys,
                countof(dyn_env_load_keys))
            < 0) {
            JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
        for (k = 0; k < 3; k++) {
            JSValue b = JS_GetPropertyStr(ctx, argv[1], dyn_env_load_keys[k]);
            if (JS_IsException(b)) {
                JS_FreeCString(ctx, path);
                return JS_EXCEPTION;
            }
            if (!JS_IsUndefined(b) && !JS_IsNull(b)) {
                if (!JS_IsBool(b)) {
                    JS_FreeValue(ctx, b);
                    JS_FreeCString(ctx, path);
                    return JS_ThrowTypeError(ctx,
                        "Env.load: options.%s must be a boolean",
                        dyn_env_load_keys[k]);
                }
                *slots[k] = JS_ToBool(ctx, b);
            }
            JS_FreeValue(ctx, b);
        }
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        JSValue err = JS_ThrowTypeError(ctx, "Env.load(\"%s\"): %s", path,
            strerror(e));
        JS_FreeCString(ctx, path);
        return err;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (off_t)DYN_ENV_FILE_MAX) {
        close(fd);
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx,
            "Env.load: not a regular file or over %u bytes", DYN_ENV_FILE_MAX);
    }
    n = (size_t)st.st_size;
    buf = (char*)js_malloc(ctx, n + 1);
    if (!buf) {
        close(fd);
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, buf + got, n - got);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                close(fd);
                js_free(ctx, buf);
                JS_FreeCString(ctx, path);
                return JS_ThrowTypeError(ctx, "Env.load: read failed");
            }
            if (r == 0) {
                close(fd);
                js_free(ctx, buf);
                JS_FreeCString(ctx, path);
                return JS_ThrowTypeError(ctx, "Env.load: file truncated");
            }
            got += (size_t)r;
        }
    }
    close(fd);
    buf[n] = '\0';
    JS_FreeCString(ctx, path);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        js_free(ctx, buf);
        return JS_EXCEPTION;
    }
    while (dyn_env_next_record(buf, n, &i)) {
        size_t kb, ke, vb, ve;
        char q;
        JSValue val;
        JSAtom a;
        if (n - i >= 7 && memcmp(buf + i, "export ", 7) == 0)
            i += 7;
        kb = i;
        while (i < n && buf[i] != '=' && buf[i] != '\n')
            i++;
        if (i >= n || buf[i] != '=') {
            while (i < n && buf[i] != '\n')
                i++;
            continue;
        }
        ke = i++;
        dyn_trim(buf, &kb, &ke);
        dyn_env_read_value(buf, n, &i, &vb, &ve, &q);
        if (ke == kb)
            continue;
        if (expand && q == '"' && ve < n && buf[ve] == q)
            val = JS_NewStringLen(ctx, buf + vb - 1, ve - vb + 2);
        else
            val = (q == '"')
                ? (expand ? JS_NewStringLen(ctx, buf + vb, ve - vb)
                          : dyn_env_unescape(ctx, buf, vb, ve))
                : JS_NewStringLen(ctx, buf + vb, ve - vb);
        if (JS_IsException(val)) {
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
        a = JS_NewAtomLen(ctx, buf + kb, ke - kb);
        if (a == JS_ATOM_NULL
            || JS_DefinePropertyValue(ctx, obj, a, val, JS_PROP_C_W_E) < 0) {
            if (a != JS_ATOM_NULL)
                JS_FreeAtom(ctx, a);
            else
                JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            js_free(ctx, buf);
            return JS_EXCEPTION;
        }
        if (nord == ocap) {
            size_t nc = ocap ? ocap * 2 : 16;
            JSAtom* no = (JSAtom*)js_realloc(ctx, order, nc * sizeof(*order));
            if (!no) {
                JS_FreeAtom(ctx, a);
                dyn_env_order_free(ctx, order, nord);
                JS_FreeValue(ctx, obj);
                js_free(ctx, buf);
                return JS_EXCEPTION;
            }
            order = no;
            ocap = nc;
        }
        order[nord++] = a;
    }
    js_free(ctx, buf);

    for (k = 0; k < nord; k++) {
        JSValue val = JS_GetProperty(ctx, obj, order[k]);
        const char* key;
        const char* raw;
        size_t klen = 0, vlen = 0;
        if (JS_IsException(val)) {
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        key = JS_AtomToCStringLen(ctx, &klen, order[k]);
        if (!key) {
            JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        raw = JS_ToCStringLen(ctx, &vlen, val);
        if (!raw) {
            JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (!dyn_env_key_ok(key, klen)) {
            JS_ThrowTypeError(ctx,
                "Env.load: key is not settable in the environment");
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, raw);
            JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (expand) {
            int was_quoted = vlen >= 2 && raw[0] == '"' && raw[vlen - 1] == '"';
            JSValue x = was_quoted
                ? dyn_env_unescape_expand(ctx, raw, 1, vlen - 1)
                : dyn_env_expand_value(ctx, raw, vlen);
            if (JS_IsException(x)) {
                JS_FreeCString(ctx, key);
                JS_FreeCString(ctx, raw);
                JS_FreeValue(ctx, val);
                dyn_env_order_free(ctx, order, nord);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, val);
            val = x;
            JS_FreeCString(ctx, raw);
            raw = JS_ToCStringLen(ctx, &vlen, val);
            if (!raw) {
                JS_FreeCString(ctx, key);
                dyn_env_order_free(ctx, order, nord);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
        }
        if (vlen != strlen(raw)) {
            JS_ThrowTypeError(ctx,
                "Env.load: value for a key must not contain NUL");
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, raw);
            JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValue(ctx, obj, order[k], JS_DupValue(ctx, val), JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, raw);
            JS_FreeValue(ctx, val);
            dyn_env_order_free(ctx, order, nord);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (assign) {
            if (override || getenv(key) == NULL) {
                if (setenv(key, raw, 1) != 0) {
                    JS_ThrowTypeError(ctx, "Env.load: setenv failed");
                    JS_FreeCString(ctx, key);
                    JS_FreeCString(ctx, raw);
                    JS_FreeValue(ctx, val);
                    dyn_env_order_free(ctx, order, nord);
                    JS_FreeValue(ctx, obj);
                    return JS_EXCEPTION;
                }
            } else {
                JS_DefinePropertyValue(ctx, obj, order[k],
                    JS_NewString(ctx, getenv(key)), JS_PROP_C_W_E);
            }
        }
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, raw);
        JS_FreeValue(ctx, val);
    }
    dyn_env_order_free(ctx, order, nord);
    return obj;
}

static JSValue dyn_env_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSPropertyEnum* tab = NULL;
    uint32_t tn = 0, k;
    dyn_tml_sb_t out;
    JSValue str;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "Env.stringify(record): the record must be an object");
    dyn_tml_sb_init(&out);
    if (JS_GetOwnPropertyNames(ctx, &tab, &tn, argv[0],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return JS_EXCEPTION;
    for (k = 0; k < tn; k++) {
        JSValue val = JS_GetProperty(ctx, argv[0], tab[k].atom);
        const char* key;
        size_t keylen;
        const char* raw;
        size_t rawlen;
        if (JS_IsException(val)) {
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
        key = JS_AtomToCStringLen(ctx, &keylen, tab[k].atom);
        if (!key) {
            JS_FreeValue(ctx, val);
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
        if (!dyn_env_key_ok(key, keylen)) {
            JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, val);
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_ThrowTypeError(ctx,
                "Env.stringify: key is not valid in the environment");
        }
        if (!JS_IsString(val)) {
            JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, val);
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_ThrowTypeError(ctx,
                "Env.stringify: every value must be a string");
        }
        raw = JS_ToCStringLen(ctx, &rawlen, val);
        if (!raw) {
            JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, val);
            JS_FreePropertyEnum(ctx, tab, tn);
            dyn_tml_sb_free(&out);
            return JS_EXCEPTION;
        }
        dyn_tml_sb_put(&out, key, keylen);
        dyn_tml_sb_putc(&out, '=');
        if (dyn_env_needs_quotes(raw, rawlen)) {
            size_t j;
            dyn_tml_sb_putc(&out, '"');
            for (j = 0; j < rawlen; j++) {
                char c = raw[j];
                size_t r = j;
                while (r < rawlen && raw[r] != '"' && raw[r] != '\\' && raw[r] != '\n' && raw[r] != '\r' && raw[r] != '\t' && raw[r] != '\0')
                    r++;
                if (r > j) {
                    dyn_tml_sb_put(&out, raw + j, r - j);
                    j = r - 1;
                    continue;
                }
                switch (c) {
                case '"':
                    dyn_tml_sb_puts(&out, "\\\"");
                    break;
                case '\\':
                    dyn_tml_sb_puts(&out, "\\\\");
                    break;
                case '\n':
                    dyn_tml_sb_puts(&out, "\\n");
                    break;
                case '\r':
                    dyn_tml_sb_puts(&out, "\\r");
                    break;
                case '\t':
                    dyn_tml_sb_puts(&out, "\\t");
                    break;
                case '\0':
                    dyn_tml_sb_puts(&out, "\\0");
                    break;
                default:
                    dyn_tml_sb_putc(&out, c);
                    break;
                }
            }
            dyn_tml_sb_putc(&out, '"');
        } else {
            dyn_tml_sb_put(&out, raw, rawlen);
        }
        dyn_tml_sb_putc(&out, '\n');
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, raw);
        JS_FreeValue(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, tn);
    str = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
    dyn_tml_sb_free(&out);
    return str;
}

const JSCFunctionListEntry dyn_env_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_env_parse),
    JS_CFUNC_DEF("load", 2, dyn_env_load),
    JS_CFUNC_DEF("stringify", 1, dyn_env_stringify),
};

#endif
