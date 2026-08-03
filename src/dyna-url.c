/* dyna:url -- URL parsing and x-www-form-urlencoded. RFC 3986 grammar with the
   WHATWG component names and relative resolution (RFC 3986 sec.5.2).
   Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_URL)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A URL is not a document. Past this the input is refused rather than parsed,
   because every component below is bounded by it. */
#define DYN_URL_MAX 65536

/* ------------------------------------------------------------ byte buffer */

typedef struct { char *p; size_t n, cap; int oom; } dyn_sb_t;

static void dyn_sb_init(dyn_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_sb_free(dyn_sb_t *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

/* Overshoot costs ABSOLUTE bytes, so the factor decays with size. */
static size_t dyn_grow_cap(size_t cur, size_t need)
{
    size_t nc = cur ? cur : 64;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static void dyn_sb_put(dyn_sb_t *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n);
        char *np;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_sb_putc(dyn_sb_t *b, char c) { dyn_sb_put(b, &c, 1); }
static void dyn_sb_puts(dyn_sb_t *b, const char *s) { dyn_sb_put(b, s, strlen(s)); }

/* ------------------------------------------------------ percent encoding */

static int dyn_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode %XX. A malformed escape is left LITERAL rather than dropped: losing
   bytes silently is how a decoder turns a bad request into a different one. */
static void dyn_pct_decode(dyn_sb_t *out, const char *s, size_t n, int plus_space)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int h = dyn_hexval((unsigned char)s[i + 1]);
            int l = dyn_hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) {
                dyn_sb_putc(out, (char)((h << 4) | l));
                i += 2;
                continue;
            }
        }
        dyn_sb_putc(out, (plus_space && s[i] == '+') ? ' ' : s[i]);
    }
}

/* The x-www-form-urlencoded set: unreserved plus `*-._`, space becomes `+`. */
static int dyn_form_safe(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
}

static void dyn_pct_encode(dyn_sb_t *out, const char *s, size_t n, int form)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (dyn_form_safe(c) || (!form && (c == '~' || c == '!' || c == '\''
                                           || c == '(' || c == ')'))) {
            dyn_sb_putc(out, (char)c);
        } else if (form && c == ' ') {
            dyn_sb_putc(out, '+');
        } else {
            dyn_sb_putc(out, '%');
            dyn_sb_putc(out, HEX[c >> 4]);
            dyn_sb_putc(out, HEX[c & 0xF]);
        }
    }
}

/* --------------------------------------------------------------- the URL */

typedef struct {
    char *scheme, *username, *password, *host, *port, *path, *query, *fragment;
    int has_authority;
} dyn_url_t;

static JSClassID dyn_url_class_id;

static void dyn_url_free(void *p)
{
    dyn_url_t *u = (dyn_url_t *)p;
    free(u->scheme); free(u->username); free(u->password); free(u->host);
    free(u->port); free(u->path); free(u->query); free(u->fragment);
    free(u);
}

static void dyn_url_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_url_t *u = (dyn_url_t *)JS_GetOpaque(val, dyn_url_class_id);
    (void)rt;
    if (u)
        dyn_url_free(u);
}

static JSClassDef dyn_url_class = { "URL", .finalizer = dyn_url_finalizer };

static char *dyn_strndup(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r)
        return NULL;
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* Default port for the schemes the spec calls special. A port equal to the
   default is dropped, which is what makes two spellings of one origin equal. */
static const char *dyn_default_port(const char *scheme)
{
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws"))   return "80";
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return "443";
    if (!strcmp(scheme, "ftp"))                             return "21";
    return NULL;
}

/* The WHATWG "special" schemes. Distinct from dyn_default_port() because file:
   is special and has no port -- do not fold the two. */
static int dyn_scheme_is_special(const char *scheme)
{
    return !strcmp(scheme, "http")  || !strcmp(scheme, "https") ||
           !strcmp(scheme, "ws")    || !strcmp(scheme, "wss")   ||
           !strcmp(scheme, "ftp")   || !strcmp(scheme, "file");
}

/* For a special scheme a backslash ends the authority exactly as '/' does.
   Without this, https://example.com\@evil.com/ parses its host as evil.com --
   the whole string is read as userinfo -- while every WHATWG parser says
   example.com. A host allowlist checked here then disagrees with the fetcher. */
static int dyn_authority_end(char c, int special)
{
    return c == '/' || (special && c == '\\');
}

/* RFC 3986 sec.5.2.4 remove_dot_segments, iteratively over an output cursor.
   `..` past the root is dropped, not allowed to escape above it. */
static void dyn_remove_dots(dyn_sb_t *out, const char *p, size_t n)
{
    size_t i = 0;
    dyn_sb_t seg;
    dyn_sb_init(&seg);
    while (i < n) {
        size_t b = i;
        while (i < n && p[i] != '/')
            i++;
        {
            size_t len = i - b;
            if (len == 1 && p[b] == '.') {
                /* current directory: contributes nothing */
            } else if (len == 2 && p[b] == '.' && p[b + 1] == '.') {
                while (seg.n && seg.p[seg.n - 1] != '/')
                    seg.n--;
                if (seg.n)
                    seg.n--;                 /* drop the slash too */
            } else {
                dyn_sb_putc(&seg, '/');
                dyn_sb_put(&seg, p + b, len);
                if (i >= n)
                    goto done;
                i++;
                if (i >= n)
                    dyn_sb_putc(&seg, '/');   /* input ended on the slash */
                continue;
            }
            /* A `.` or `..` segment contributes no name, but it DOES leave the
               path ending in a slash -- that is what makes <./> resolve to
               "/b/c/" and not "/b/c". True whether or not input remains. */
            if (i >= n) {
                dyn_sb_putc(&seg, '/');
                goto done;
            }
            i++;
            if (i >= n)
                dyn_sb_putc(&seg, '/');
        }
    }
 done:
    if (seg.n == 0)
        dyn_sb_putc(out, '/');
    else
        dyn_sb_put(out, seg.p, seg.n);
    dyn_sb_free(&seg);
}

/* Split the authority into userinfo / host / port. IPv6 literals keep their
   brackets, and the LAST colon outside them is the port separator. */
static int dyn_parse_authority(dyn_url_t *u, const char *s, size_t n)
{
    size_t at = (size_t)-1, i;
    size_t hb, he;
    for (i = 0; i < n; i++)
        if (s[i] == '@')
            at = i;                          /* last '@' wins, per spec */
    if (at != (size_t)-1) {
        size_t c = (size_t)-1;
        for (i = 0; i < at; i++)
            if (s[i] == ':') { c = i; break; }
        if (c != (size_t)-1) {
            u->username = dyn_strndup(s, c);
            u->password = dyn_strndup(s + c + 1, at - c - 1);
        } else {
            u->username = dyn_strndup(s, at);
        }
        hb = at + 1;
    } else {
        hb = 0;
    }
    he = n;
    if (hb < n && s[hb] == '[') {            /* IPv6 literal */
        size_t close = hb;
        while (close < n && s[close] != ']')
            close++;
        if (close >= n)
            return -1;                        /* unterminated: refuse */
        if (close + 1 < n && s[close + 1] == ':') {
            u->port = dyn_strndup(s + close + 2, n - close - 2);
            he = close + 1;
        } else {
            he = close + 1;
        }
    } else {
        size_t c = (size_t)-1;
        for (i = hb; i < n; i++)
            if (s[i] == ':') c = i;
        if (c != (size_t)-1) {
            u->port = dyn_strndup(s + c + 1, n - c - 1);
            he = c;
        }
    }
    u->host = dyn_strndup(s + hb, he - hb);
    if (u->port) {                            /* validate and normalise */
        const char *d;
        size_t k, pl = strlen(u->port);
        for (k = 0; k < pl; k++)
            if (u->port[k] < '0' || u->port[k] > '9')
                return -1;
        if (pl == 0) { free(u->port); u->port = NULL; }
        else if (pl > 5 || atoi(u->port) > 65535) return -1;
        d = u->scheme ? dyn_default_port(u->scheme) : NULL;
        if (u->port && d && strcmp(u->port, d) == 0) {
            free(u->port);
            u->port = NULL;
        }
    }
    return u->host ? 0 : -1;
}

/* Lowercase a scheme in place; schemes are ASCII case-insensitive. */
static void dyn_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static int dyn_scheme_char(char c, int first)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    if (first)
        return 0;
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Parse `s` into `u`. Returns 0, or -1 without throwing (the caller names the
   input in the error). `base` may be NULL. */
static int dyn_url_parse(dyn_url_t *u, const char *s, size_t n,
                         const dyn_url_t *base)
{
    size_t i = 0, scheme_end = 0;
    int has_scheme = 0, special = 0;

    while (n && (unsigned char)s[n - 1] <= 0x20) n--;    /* trim C0 and space */
    while (n && (unsigned char)*s <= 0x20) { s++; n--; }

    if (n && dyn_scheme_char(s[0], 1)) {
        for (i = 1; i < n && dyn_scheme_char(s[i], 0); i++)
            ;
        if (i < n && s[i] == ':') { has_scheme = 1; scheme_end = i; }
    }
    if (has_scheme) {
        u->scheme = dyn_strndup(s, scheme_end);
        if (!u->scheme) return -1;
        dyn_lower(u->scheme);
        i = scheme_end + 1;
    } else {
        if (!base)
            return -1;                        /* relative with no base */
        u->scheme = dyn_strndup(base->scheme, strlen(base->scheme));
        i = 0;
    }

    /* fragment and query are split off the tail first: both may contain any
       character, including the delimiters of the components before them. */
    {
        size_t hash = (size_t)-1, q = (size_t)-1, k;
        size_t end = n;
        for (k = i; k < n; k++) {
            if (s[k] == '#') { hash = k; break; }
        }
        if (hash != (size_t)-1) {
            u->fragment = dyn_strndup(s + hash + 1, n - hash - 1);
            end = hash;
        }
        for (k = i; k < end; k++)
            if (s[k] == '?') { q = k; break; }
        if (q != (size_t)-1) {
            u->query = dyn_strndup(s + q + 1, end - q - 1);
            end = q;
        }
        n = end;
    }

    special = dyn_scheme_is_special(u->scheme);
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        size_t ab = i + 2, ae = ab;
        while (ae < n && !dyn_authority_end(s[ae], special))
            ae++;
        if (dyn_parse_authority(u, s + ab, ae - ab) < 0)
            return -1;
        u->has_authority = 1;
        i = ae;
        base = NULL;      /* <//g> defines its own authority: the base's path
                             and query are NOT inherited (RFC 3986 5.2.2) */
        if (i >= n) {
            u->path = dyn_strndup("", 0);   /* empty path, not "/" */
            return u->path ? 0 : -1;
        }
    } else if (has_scheme) {
        u->has_authority = 0;                 /* opaque path, e.g. mailto: */
    } else if (base) {
        /* inherit the base's authority for a relative reference */
        u->has_authority = base->has_authority;
        if (base->host)     u->host = dyn_strndup(base->host, strlen(base->host));
        if (base->port)     u->port = dyn_strndup(base->port, strlen(base->port));
        if (base->username) u->username = dyn_strndup(base->username, strlen(base->username));
        if (base->password) u->password = dyn_strndup(base->password, strlen(base->password));
    }

    /* the path, resolved against the base when the reference is relative */
    {
        dyn_sb_t out;
        dyn_sb_init(&out);
        if (i < n && dyn_authority_end(s[i], special)) {
            dyn_remove_dots(&out, s + i + 1, n - i - 1);
        } else if (!u->has_authority && has_scheme) {
            dyn_sb_put(&out, s + i, n - i);   /* opaque: kept verbatim */
        } else if (i >= n) {
            if (base && base->path && !has_scheme) {
                dyn_sb_puts(&out, base->path);
                /* RFC 3986 5.2.2: an empty path takes the base's query too, so
                   <> and <#s> keep ?q. Only when the reference had none. */
                if (!u->query && base->query)
                    u->query = dyn_strndup(base->query, strlen(base->query));
            } else {
                dyn_sb_putc(&out, '/');
            }
        } else if (base && base->path && !has_scheme) {
            dyn_sb_t merged;                  /* RFC 3986 sec.5.3 merge */
            size_t bl = strlen(base->path), cut = 0, k;
            dyn_sb_init(&merged);
            for (k = 0; k < bl; k++)
                if (base->path[k] == '/') cut = k + 1;
            dyn_sb_put(&merged, base->path, cut);
            dyn_sb_put(&merged, s + i, n - i);
            dyn_remove_dots(&out, merged.p + 1, merged.n ? merged.n - 1 : 0);
            dyn_sb_free(&merged);
        } else {
            dyn_remove_dots(&out, s + i, n - i);
        }
        if (out.oom) { dyn_sb_free(&out); return -1; }
        u->path = dyn_strndup(out.p ? out.p : "", out.n);
        dyn_sb_free(&out);
    }
    return u->path ? 0 : -1;
}

/* Rebuild the serialized form from the components. */
static JSValue dyn_url_href(JSContext *ctx, const dyn_url_t *u)
{
    dyn_sb_t b;
    JSValue r;
    dyn_sb_init(&b);
    dyn_sb_puts(&b, u->scheme);
    dyn_sb_putc(&b, ':');
    if (u->has_authority) {
        dyn_sb_puts(&b, "//");
        if (u->username && *u->username) {
            dyn_sb_puts(&b, u->username);
            if (u->password && *u->password) {
                dyn_sb_putc(&b, ':');
                dyn_sb_puts(&b, u->password);
            }
            dyn_sb_putc(&b, '@');
        }
        dyn_sb_puts(&b, u->host ? u->host : "");
        if (u->port) { dyn_sb_putc(&b, ':'); dyn_sb_puts(&b, u->port); }
    }
    dyn_sb_puts(&b, u->path ? u->path : "");
    if (u->query)    { dyn_sb_putc(&b, '?'); dyn_sb_puts(&b, u->query); }
    if (u->fragment) { dyn_sb_putc(&b, '#'); dyn_sb_puts(&b, u->fragment); }
    if (b.oom) { dyn_sb_free(&b); return JS_ThrowOutOfMemory(ctx); }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static dyn_url_t *dyn_url_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_url_t *)dyn_plain_get(ctx, v, dyn_url_class_id);
}

static JSValue dyn_url_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    const char *s = NULL, *bs = NULL;
    size_t n = 0, bn = 0;
    dyn_url_t *u = NULL, base;
    int have_base = 0;

    memset(&base, 0, sizeof base);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new URL(input[, base]): input must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        bs = JS_ToCStringLen(ctx, &bn, argv[1]);
        if (!bs) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
        if (bn > DYN_URL_MAX || dyn_url_parse(&base, bs, bn, NULL) < 0) {
            free(base.scheme); free(base.username); free(base.password);
            free(base.host); free(base.port); free(base.path);
            free(base.query); free(base.fragment);
            JS_FreeCString(ctx, s); JS_FreeCString(ctx, bs);
            return JS_ThrowTypeError(ctx, "new URL(input, base): base is not a valid URL");
        }
        have_base = 1;
    }
    if (n > DYN_URL_MAX) {
        JS_ThrowRangeError(ctx, "new URL(input): input exceeds %u bytes", DYN_URL_MAX);
        goto fail;
    }
    u = (dyn_url_t *)calloc(1, sizeof *u);
    if (!u) { JS_ThrowOutOfMemory(ctx); goto fail; }
    if (dyn_url_parse(u, s, n, have_base ? &base : NULL) < 0) {
        dyn_url_free(u);
        u = NULL;
        JS_ThrowTypeError(ctx, "new URL(input): \"%.*s\" is not a valid URL",
                          (int)(n > 80 ? 80 : n), s);
        goto fail;
    }
    JS_FreeCString(ctx, s);
    if (bs) JS_FreeCString(ctx, bs);
    free(base.scheme); free(base.username); free(base.password); free(base.host);
    free(base.port); free(base.path); free(base.query); free(base.fragment);
    return dyn_plain_wrap(ctx, dyn_url_class_id, u, dyn_url_free);
 fail:
    JS_FreeCString(ctx, s);
    if (bs) JS_FreeCString(ctx, bs);
    free(base.scheme); free(base.username); free(base.password); free(base.host);
    free(base.port); free(base.path); free(base.query); free(base.fragment);
    return JS_EXCEPTION;
}

/* magic selects the component; one getter body for all of them. */
enum { U_HREF, U_PROTOCOL, U_USERNAME, U_PASSWORD, U_HOST, U_HOSTNAME, U_PORT,
       U_PATHNAME, U_SEARCH, U_HASH, U_ORIGIN };

static JSValue dyn_url_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_url_t *u = dyn_url_of(ctx, this_val);
    dyn_sb_t b;
    JSValue r;
    if (!u)
        return JS_EXCEPTION;
    switch (magic) {
    case U_HREF:     return dyn_url_href(ctx, u);
    case U_USERNAME: return JS_NewString(ctx, u->username ? u->username : "");
    case U_PASSWORD: return JS_NewString(ctx, u->password ? u->password : "");
    case U_HOSTNAME: return JS_NewString(ctx, u->host ? u->host : "");
    case U_PORT:     return JS_NewString(ctx, u->port ? u->port : "");
    case U_PATHNAME: return JS_NewString(ctx, u->path ? u->path : "");
    case U_PROTOCOL:
        dyn_sb_init(&b);
        dyn_sb_puts(&b, u->scheme);
        dyn_sb_putc(&b, ':');
        break;
    case U_HOST:
        dyn_sb_init(&b);
        dyn_sb_puts(&b, u->host ? u->host : "");
        if (u->port) { dyn_sb_putc(&b, ':'); dyn_sb_puts(&b, u->port); }
        break;
    case U_SEARCH:
        dyn_sb_init(&b);
        if (u->query && *u->query) { dyn_sb_putc(&b, '?'); dyn_sb_puts(&b, u->query); }
        break;
    case U_HASH:
        dyn_sb_init(&b);
        if (u->fragment && *u->fragment) { dyn_sb_putc(&b, '#'); dyn_sb_puts(&b, u->fragment); }
        break;
    default:                       /* U_ORIGIN: scheme://host[:port], or null */
        if (!u->has_authority || !u->host || !*u->host)
            return JS_NewString(ctx, "null");
        dyn_sb_init(&b);
        dyn_sb_puts(&b, u->scheme);
        dyn_sb_puts(&b, "://");
        dyn_sb_puts(&b, u->host);
        if (u->port) { dyn_sb_putc(&b, ':'); dyn_sb_puts(&b, u->port); }
        break;
    }
    if (b.oom) { dyn_sb_free(&b); return JS_ThrowOutOfMemory(ctx); }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static JSValue dyn_url_tostring(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    dyn_url_t *u = dyn_url_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!u)
        return JS_EXCEPTION;
    return dyn_url_href(ctx, u);
}

static const JSCFunctionListEntry dyn_url_proto[] = {
    JS_CGETSET_MAGIC_DEF("href", dyn_url_get, NULL, U_HREF),
    JS_CGETSET_MAGIC_DEF("protocol", dyn_url_get, NULL, U_PROTOCOL),
    JS_CGETSET_MAGIC_DEF("username", dyn_url_get, NULL, U_USERNAME),
    JS_CGETSET_MAGIC_DEF("password", dyn_url_get, NULL, U_PASSWORD),
    JS_CGETSET_MAGIC_DEF("host", dyn_url_get, NULL, U_HOST),
    JS_CGETSET_MAGIC_DEF("hostname", dyn_url_get, NULL, U_HOSTNAME),
    JS_CGETSET_MAGIC_DEF("port", dyn_url_get, NULL, U_PORT),
    JS_CGETSET_MAGIC_DEF("pathname", dyn_url_get, NULL, U_PATHNAME),
    JS_CGETSET_MAGIC_DEF("search", dyn_url_get, NULL, U_SEARCH),
    JS_CGETSET_MAGIC_DEF("hash", dyn_url_get, NULL, U_HASH),
    JS_CGETSET_MAGIC_DEF("origin", dyn_url_get, NULL, U_ORIGIN),
    JS_CFUNC_DEF("toString", 0, dyn_url_tostring),
    JS_CFUNC_DEF("toJSON", 0, dyn_url_tostring),
};

/* ------------------------------------------------- form-urlencoded codec */

/* formDecode(text) -> object of the LAST value per key. Keys are written with
   define semantics: `__proto__=x` must not retarget the prototype. */
static JSValue dyn_form_decode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const char *owned, *s;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "formDecode(text): argument must be a string");
    owned = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!owned)
        return JS_EXCEPTION;
    /* Skip a leading '?' with a CURSOR. JS_FreeCString must get back the exact
       pointer JS_ToCStringLen returned -- it finds the string header from it,
       so freeing an offset pointer corrupts the heap. */
    s = owned;
    if (n && s[0] == '?') { s++; n--; }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) { JS_FreeCString(ctx, owned); return JS_EXCEPTION; }
    while (i < n) {
        size_t b = i, eq = (size_t)-1;
        dyn_sb_t k, v;
        JSAtom a;
        while (i < n && s[i] != '&') {
            if (s[i] == '=' && eq == (size_t)-1)
                eq = i;
            i++;
        }
        if (i > b) {
            dyn_sb_init(&k); dyn_sb_init(&v);
            if (eq == (size_t)-1) {
                dyn_pct_decode(&k, s + b, i - b, 1);
            } else {
                dyn_pct_decode(&k, s + b, eq - b, 1);
                dyn_pct_decode(&v, s + eq + 1, i - eq - 1, 1);
            }
            a = JS_NewAtomLen(ctx, k.p ? k.p : "", k.n);
            if (a != JS_ATOM_NULL) {
                JS_DefinePropertyValue(ctx, obj, a,
                    JS_NewStringLen(ctx, v.p ? v.p : "", v.n), JS_PROP_C_W_E);
                JS_FreeAtom(ctx, a);
            }
            dyn_sb_free(&k); dyn_sb_free(&v);
        }
        i++;
    }
    JS_FreeCString(ctx, owned);
    return obj;
}

/* formEncode(obj) -> "a=1&b=2" over the object's own enumerable string keys. */
static JSValue dyn_form_encode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    dyn_sb_t b;
    JSValue ret = JS_EXCEPTION;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "formEncode(obj): argument must be an object");
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    for (i = 0; i < len; i++) {
        JSValue kv = JS_AtomToString(ctx, tab[i].atom);
        JSValue pv = JS_GetProperty(ctx, argv[0], tab[i].atom);
        const char *ks = NULL, *vs = NULL;
        size_t kn = 0, vn = 0;
        if (JS_IsException(kv) || JS_IsException(pv)) {
            JS_FreeValue(ctx, kv); JS_FreeValue(ctx, pv);
            goto done;
        }
        if (JS_IsUndefined(pv)) { JS_FreeValue(ctx, kv); JS_FreeValue(ctx, pv); continue; }
        ks = JS_ToCStringLen(ctx, &kn, kv);
        vs = JS_ToCStringLen(ctx, &vn, pv);
        JS_FreeValue(ctx, kv); JS_FreeValue(ctx, pv);
        if (!ks || !vs) {
            if (ks) JS_FreeCString(ctx, ks);
            if (vs) JS_FreeCString(ctx, vs);
            goto done;
        }
        if (b.n) dyn_sb_putc(&b, '&');
        dyn_pct_encode(&b, ks, kn, 1);
        dyn_sb_putc(&b, '=');
        dyn_pct_encode(&b, vs, vn, 1);
        JS_FreeCString(ctx, ks);
        JS_FreeCString(ctx, vs);
    }
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto done; }
    ret = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
 done:
    dyn_sb_free(&b);
    JS_FreePropertyEnum(ctx, tab, len);
    return ret;
}

/* encodeURIComponentStrict(s): also escapes !'()~, which encodeURIComponent
   leaves alone and which several servers treat as delimiters. */
static JSValue dyn_enc_strict(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    dyn_sb_t b;
    JSValue r;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "encodeURIComponentStrict(text): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    dyn_sb_init(&b);
    dyn_pct_encode(&b, s, n, 1);
    JS_FreeCString(ctx, s);
    if (b.oom) { dyn_sb_free(&b); return JS_ThrowOutOfMemory(ctx); }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_url_funcs[] = {
    JS_CFUNC_DEF("formEncode", 1, dyn_form_encode),
    JS_CFUNC_DEF("formDecode", 1, dyn_form_decode),
    JS_CFUNC_DEF("encodeURIComponentStrict", 1, dyn_enc_strict),
};

static int dyn_url_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_url_class_id, &dyn_url_class,
                                 dyn_url_proto, countof(dyn_url_proto),
                                 dyn_url_ctor, "URL") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

int js_nat_init_url(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:url", dyn_url_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "URL");
    return JS_AddModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_URL */
