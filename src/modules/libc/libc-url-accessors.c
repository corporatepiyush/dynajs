#include "libc-url.h"

static JSValue dyn_url_join2(JSContext* ctx, const char* a, const char* m, const char* b)
{
    char buf[256];
    size_t an = strlen(a), mn = m ? strlen(m) : 0, bn = strlen(b);
    dyn_sb_t sb;
    JSValue r;
    if (an + mn + bn <= sizeof buf) {
        memcpy(buf, a, an);
        if (mn)
            memcpy(buf + an, m, mn);
        memcpy(buf + an + mn, b, bn);
        return JS_NewStringLen(ctx, buf, an + mn + bn);
    }
    dyn_sb_init(&sb);
    dyn_sb_put(&sb, a, an);
    dyn_sb_put(&sb, m ? m : "", mn);
    dyn_sb_put(&sb, b, bn);
    if (sb.oom) {
        dyn_sb_free(&sb);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, sb.p, sb.n);
    dyn_sb_free(&sb);
    return r;
}

JSValue dyn_url_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_url_t* u = dyn_url_of(ctx, this_val);
    dyn_sb_t b;
    JSValue r;
    if (!u)
        return JS_EXCEPTION;
    switch (magic) {
    case U_HREF:
        return dyn_url_href(ctx, u);
    case U_PROTOCOL:
        return dyn_url_join2(ctx, u->scheme, NULL, ":");
    case U_HOST:
        return u->port ? dyn_url_join2(ctx, u->host ? u->host : "", ":", u->port)
                       : JS_NewString(ctx, u->host ? u->host : "");
    case U_SEARCH:
        return u->query && *u->query ? dyn_url_join2(ctx, "?", NULL, u->query) : JS_NewString(ctx, "");
    case U_HASH:
        return u->fragment && *u->fragment ? dyn_url_join2(ctx, "#", NULL, u->fragment) : JS_NewString(ctx, "");
    case U_USERNAME:
        return JS_NewString(ctx, u->username ? u->username : "");
    case U_PASSWORD:
        return JS_NewString(ctx, u->password ? u->password : "");
    case U_HOSTNAME:
        return JS_NewString(ctx, u->host ? u->host : "");
    case U_PORT:
        return JS_NewString(ctx, u->port ? u->port : "");
    case U_PATHNAME:
        return JS_NewString(ctx, u->path ? u->path : "");
    case U_SEARCHPARAMS: {
        if (JS_IsUndefined(dyn_url_obj(u)->sp_wrapper)) {
            dyn_sp_t* sp = (dyn_sp_t*)calloc(1, sizeof(*sp));
            if (!sp)
                return JS_ThrowOutOfMemory(ctx);
            sp->host = u;
            u->refs++;
            dyn_url_obj(u)->sp_wrapper = dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_sp_class_id, sp, dyn_sp_free);
            if (JS_IsException(dyn_url_obj(u)->sp_wrapper)) {
                dyn_url_obj(u)->sp_wrapper = JS_UNDEFINED;
                dyn_sp_free(sp);
                return JS_EXCEPTION;
            }
        }
        return JS_DupValue(ctx, dyn_url_obj(u)->sp_wrapper);
    }
    default:
        if (!strcmp(u->scheme, "blob") && u->path) {
            dyn_url_t inner;
            memset(&inner, 0, sizeof inner);
            if (dyn_url_parse(&inner, u->path, strlen(u->path), NULL) == 0 && inner.has_authority && inner.host && *inner.host && (!strcmp(inner.scheme, "http") || !strcmp(inner.scheme, "https"))) {
                dyn_sb_init(&b);
                dyn_sb_puts(&b, inner.scheme);
                dyn_sb_puts(&b, "://");
                dyn_sb_puts(&b, inner.host);
                if (inner.port) {
                    dyn_sb_putc(&b, ':');
                    dyn_sb_puts(&b, inner.port);
                }
                if (b.oom) {
                    free(inner.scheme);
                    free(inner.username);
                    free(inner.password);
                    free(inner.host);
                    free(inner.port);
                    free(inner.path);
                    free(inner.query);
                    free(inner.fragment);
                    free(inner.drive);
                    dyn_sb_free(&b);
                    return JS_ThrowOutOfMemory(ctx);
                }
                r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
                dyn_sb_free(&b);
                free(inner.scheme);
                free(inner.username);
                free(inner.password);
                free(inner.host);
                free(inner.port);
                free(inner.path);
                free(inner.query);
                free(inner.fragment);
                free(inner.drive);
                return r;
            }
            free(inner.scheme);
            free(inner.username);
            free(inner.password);
            free(inner.host);
            free(inner.port);
            free(inner.path);
            free(inner.query);
            free(inner.fragment);
            free(inner.drive);
            return JS_NewString(ctx, "null");
        }
        if (!dyn_scheme_is_special(u->scheme))
            return JS_NewString(ctx, "null");
        if (!strcmp(u->scheme, "file"))
            return JS_NewString(ctx, "null");
        if (!u->has_authority || !u->host || !*u->host)
            return JS_NewString(ctx, "null");
        {
            char buf[512];
            size_t sn = strlen(u->scheme), hn = strlen(u->host), pn = u->port ? strlen(u->port) : 0;
            if (sn + hn + pn + 4 <= sizeof buf) {
                size_t o = 0;
                memcpy(buf, u->scheme, sn);
                o = sn;
                memcpy(buf + o, "://", 3);
                o += 3;
                memcpy(buf + o, u->host, hn);
                o += hn;
                if (u->port) {
                    buf[o++] = ':';
                    memcpy(buf + o, u->port, pn);
                    o += pn;
                }
                return JS_NewStringLen(ctx, buf, o);
            }
        }
        dyn_sb_init(&b);
        dyn_sb_puts(&b, u->scheme);
        dyn_sb_puts(&b, "://");
        dyn_sb_puts(&b, u->host);
        if (u->port) {
            dyn_sb_putc(&b, ':');
            dyn_sb_puts(&b, u->port);
        }
        break;
    }
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static JSValue dyn_url_apply(JSContext* ctx, dyn_url_t* u, int field,
    const char* raw, int move_query, int move_frag)
{
    dyn_sb_t b;
    char* cand;
    size_t cn = 0;
    int rc;

    dyn_sb_init(&b);
    {
        char* core;
        dyn_sb_t head;
        dyn_sb_init(&head);
        core = dyn_url_candidate(u, field, raw, &cn);
        if (!core) {
            dyn_sb_free(&head);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (field == URLF_PATH) {
            const char* h = strchr(core, '#');
            if (h)
                dyn_sb_put(&head, core, (size_t)(h - core));
            else
                dyn_sb_put(&head, core, cn);
            if (!move_query && u->query) {
                dyn_sb_putc(&head, '?');
                dyn_href_component(&head, u->query, DYN_PCT_QUERY);
            }
            if (h)
                dyn_sb_putc(&head, '#'), dyn_sb_puts(&head, h + 1);
        } else {
            dyn_sb_put(&head, core, cn);
            if (!move_frag && u->fragment) {
                dyn_sb_putc(&head, '#');
                dyn_href_component(&head, u->fragment, DYN_PCT_FRAGMENT);
            }
        }
        free(core);
        if (head.oom) {
            dyn_sb_free(&head);
            return JS_ThrowOutOfMemory(ctx);
        }
        cand = dyn_strndup(head.p ? head.p : "", head.n);
        cn = head.n;
        dyn_sb_free(&head);
    }
    dyn_sb_free(&b);
    if (!cand)
        return JS_ThrowOutOfMemory(ctx);
    rc = dyn_url_adopt(u, cand, cn, field, move_query, move_frag);
    free(cand);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue dyn_url_apply_host(JSContext* ctx, dyn_url_t* u, const char* raw,
    int allow_port)
{
    dyn_sb_t b;
    dyn_url_t tmp;
    char *cand, *hostpart;
    const char* newport;
    char portbuf[24];
    size_t n = strlen(raw), he, i, cn;
    const char* port = NULL;
    int rc;

    he = n;
    if (raw[0] == '[') {
        size_t close = 0;
        while (close < n && raw[close] != ']')
            close++;
        if (close >= n)
            return JS_UNDEFINED;
        he = close + 1;
        if (he < n) {
            if (raw[he] != ':')
                return JS_UNDEFINED;
            port = raw + he + 1;
        }
    } else {
        for (i = 0; i < n; i++)
            if (raw[i] == ':') {
                he = i;
                port = raw + i + 1;
                break;
            }
    }
    if (he == 0)
        return JS_UNDEFINED;
    if (raw[0] != '[') {
        size_t j;
        int special = dyn_scheme_is_special(u->scheme);
        for (j = 0; j < he; j++) {
            unsigned char ch = (unsigned char)raw[j];
            if ((special ? (ch <= 0x20 || ch == 0x7f) : (ch == 0 || ch == 0x20)) || ch == '<' || ch == '>' || ch == '@' || ch == '[' || ch == ']' || ch == '\\' || ch == '^' || ch == '|')
                return JS_UNDEFINED;
        }
    }
    if (port && !allow_port)
        return JS_UNDEFINED;
    newport = u->port;
    if (port && port[0]) {
        size_t d = 0;
        unsigned long long v = 0;
        int bad = 0;
        while (port[d] >= '0' && port[d] <= '9') {
            v = v * 10 + (unsigned long long)(port[d] - '0');
            if (v > 65535) {
                bad = 1;
                break;
            }
            d++;
        }
        if (bad || d == 0 || port[d] != 0)
            return JS_UNDEFINED;
        snprintf(portbuf, sizeof portbuf, "%llu", v);
        newport = portbuf;
    }
    hostpart = dyn_strndup(raw, he);
    if (!hostpart)
        return JS_ThrowOutOfMemory(ctx);
    tmp = *u;
    tmp.host = hostpart;
    tmp.port = DYN_UNCONST(newport);
    dyn_sb_init(&b);
    dyn_url_serialize(&b, &tmp);
    free(hostpart);
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    cand = dyn_strndup(b.p ? b.p : "", b.n);
    cn = b.n;
    dyn_sb_free(&b);
    if (!cand)
        return JS_ThrowOutOfMemory(ctx);
    rc = dyn_url_adopt(u, cand, cn, URLF_HOST, 0, 0);
    free(cand);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue dyn_url_set_protocol(dyn_url_t* u, const char* s, size_t n)
{
    size_t i = 1;
    char* scheme;
    const char* dp;
    int was_special, now_special;

    if (n == 0 || !((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z'))
        return JS_UNDEFINED;
    while (i < n && (((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.'))
        i++;
    if (i < n && s[i] != ':')
        return JS_UNDEFINED;
    scheme = dyn_strndup(s, i);
    if (!scheme)
        return JS_UNDEFINED;
    dyn_lower(scheme);
    was_special = dyn_scheme_is_special(u->scheme);
    now_special = dyn_scheme_is_special(scheme);
    if (was_special != now_special
        || (!strcmp(scheme, "file") && ((u->username && *u->username) || (u->password && *u->password) || u->port))
        || (!strcmp(u->scheme, "file") && (!u->host || !*u->host))) {
        free(scheme);
        return JS_UNDEFINED;
    }
    free(u->scheme);
    u->scheme = scheme;
    dp = dyn_url_default_port(scheme);
    if (dp && u->port && !strcmp(u->port, dp)) {
        free(u->port);
        u->port = NULL;
    }
    u->query_version++;
    return JS_UNDEFINED;
}

static JSValue dyn_url_set_userinfo(JSContext* ctx, dyn_url_t* u, char** dst, const char* s, size_t n)
{
    if (!u->has_authority || !u->host || !*u->host || !strcmp(u->scheme, "file"))
        return JS_UNDEFINED;
    if (n == 0) {
        free(*dst);
        *dst = NULL;
        return JS_UNDEFINED;
    }
    if (dyn_url_set_enc(dst, s, n, DYN_SET_USERINFO) < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

static JSValue dyn_url_set_value(JSContext* ctx, dyn_url_t* u, JSValueConst val, int magic,
    const char* s, size_t n)
{
    char* raw = NULL;
    JSValue r;
    int move_q = 0, move_f = 0;

    switch (magic) {
    case U_PROTOCOL:
        return dyn_url_set_protocol(u, s, n);
    case U_USERNAME:
    case U_PASSWORD:
        return dyn_url_set_userinfo(ctx, u, magic == U_USERNAME ? &u->username : &u->password, s, n);
    case U_HASH:
        if (n && s[0] == '#') {
            s++;
            n--;
        }
        move_f = 1;

        if (n == 0) {
            free(u->fragment);
            u->fragment = NULL;
        }
        raw = dyn_strndup(s, n);
        break;
    case U_SEARCH:
        if (n && s[0] == '?') {
            s++;
            n--;
        }
        move_q = 1;
        move_f = dyn_url_has_byte(s, n, '#');
        if (n == 0) {
            free(u->query);
            u->query = NULL;
            u->query_version++;
        }
        raw = dyn_strndup(s, n);
        break;
    case U_PATHNAME: {
        dyn_sb_t b;
        if (n == 0 && u->has_authority) {
            raw = dyn_strndup("/", 1);
        } else if (u->has_authority && s[0] != '/') {
            dyn_sb_init(&b);
            dyn_sb_putc(&b, '/');
            dyn_sb_put(&b, s, n);
            if (b.oom) {
                dyn_sb_free(&b);
                return JS_ThrowOutOfMemory(ctx);
            }
            raw = dyn_strndup(b.p, b.n);
            dyn_sb_free(&b);
        } else {
            raw = dyn_strndup(s, n);
        }
        {
            size_t rl = raw ? strlen(raw) : 0;
            move_q = dyn_url_has_byte(raw ? raw : "", rl, '?');
            move_f = dyn_url_has_byte(raw ? raw : "", rl, '#');
        }
        break;
    }
    case U_HOSTNAME:
        if (!u->has_authority || !u->host || n == 0
            || dyn_url_has_byte(s, n, '/') || dyn_url_has_byte(s, n, '?')
            || dyn_url_has_byte(s, n, '#')) {
            return JS_UNDEFINED;
        }
        raw = dyn_strndup(s, n);
        break;
    case U_HOST:
        if (!u->has_authority || !u->host || n == 0
            || dyn_url_has_byte(s, n, '/') || dyn_url_has_byte(s, n, '?')
            || dyn_url_has_byte(s, n, '#')) {
            return JS_UNDEFINED;
        }
        raw = dyn_strndup(s, n);
        break;
    case U_PORT: {
        size_t i = 0;
        if (!u->has_authority || !u->host || !*u->host || !strcmp(u->scheme, "file"))
            return JS_UNDEFINED;
        if (n == 0) {
            free(u->port);
            u->port = NULL;
            return JS_UNDEFINED;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        if (i == 0)
            return JS_UNDEFINED;
        raw = dyn_strndup(s, i);
        break;
    }
    case U_SEARCHPARAMS: {
        JSValue spv = dyn_sp_ctor(ctx, JS_UNDEFINED, 1, &val);
        dyn_sp_t* sp;
        if (JS_IsException(spv)) {
            return JS_EXCEPTION;
        }
        sp = (dyn_sp_t*)JS_GetOpaque(spv, dyn_sp_class_id);
        if (!sp) {
            JS_FreeValue(ctx, spv);
            return JS_EXCEPTION;
        }
        if (dyn_url_put(&u->query, sp->query && *sp->query ? sp->query : NULL) < 0) {
            JS_FreeValue(ctx, spv);
            return JS_ThrowOutOfMemory(ctx);
        }
        u->query_version++;
        JS_FreeValue(ctx, spv);
        return JS_UNDEFINED;
    }
    default:
        return JS_UNDEFINED;
    }
    if (!raw)
        return JS_ThrowOutOfMemory(ctx);
    if (magic == U_HOST || magic == U_HOSTNAME) {
        r = dyn_url_apply_host(ctx, u, raw, magic == U_HOST);
        free(raw);
        return r;
    }
    r = dyn_url_apply(ctx, u, magic == U_PORT ? URLF_PORT : magic == U_PATHNAME ? URLF_PATH
            : magic == U_SEARCH                                                 ? URLF_QUERY
                                                                                : URLF_FRAGMENT,
        raw, move_q, move_f);
    free(raw);
    return r;
}

JSValue dyn_url_set(JSContext* ctx, JSValueConst this_val,
    JSValueConst val, int magic)
{
    dyn_url_t* u = dyn_url_of(ctx, this_val);
    const char *base, *s;
    char *clean = NULL, *nul = NULL;
    size_t n = 0;
    JSValue r;

    if (!u)
        return JS_EXCEPTION;
    base = JS_ToCStringLenUTF8(ctx, &n, val);
    if (!base)
        return JS_EXCEPTION;
    s = base;
    if (magic != U_USERNAME && magic != U_PASSWORD && magic != U_SEARCHPARAMS) {
        clean = dyn_strip_tabnl(s, n, &n);
        if (clean)
            s = clean;
    }
    if (memchr(s, 0, n)) {
        if (magic == U_HOST || magic == U_HOSTNAME) {
            free(clean);
            JS_FreeCString(ctx, base);
            return JS_UNDEFINED;
        }
        if (magic != U_PORT && magic != U_PROTOCOL && magic != U_SEARCHPARAMS) {
            dyn_sb_t b;
            size_t i;
            dyn_sb_init(&b);
            for (i = 0; i < n; i++)
                if (s[i])
                    dyn_sb_putc(&b, s[i]);
                else
                    dyn_sb_put(&b, "%00", 3);
            if (b.oom) {
                dyn_sb_free(&b);
                free(clean);
                JS_FreeCString(ctx, base);
                return JS_ThrowOutOfMemory(ctx);
            }
            nul = b.p;
            n = b.n;
            s = nul;
        }
    }
    r = dyn_url_set_value(ctx, u, val, magic, s, n);
    free(nul);
    free(clean);
    JS_FreeCString(ctx, base);
    return r;
}

JSValue dyn_enc_strict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    dyn_sb_t b;
    JSValue r;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "encodeURIComponentStrict(text): argument must be a string");
    s = JS_ToCStringLenUTF8(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    dyn_sb_init(&b);

    dyn_pct_encode(&b, s, n, 4);
    JS_FreeCString(ctx, s);
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

