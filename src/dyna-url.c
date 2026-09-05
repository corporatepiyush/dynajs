/* dyna:url -- URL parsing and x-www-form-urlencoded. RFC 3986 grammar with the
   WHATWG component names and relative resolution (RFC 3986 sec.5.2).
   Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"
#include "core/dyn-pct.h"
#include "core/dyn-sb.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_URL)

#include "cutils.h"        /* TRUE/FALSE, unicode_{to,from}_utf8 */
#include "libunicode.h"    /* NFC for the IDNA pipeline */

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

/* Thin wrapper over core/dyn-sb.h: this module's struct name, its sticky-oom
   void-return convention and its seed (64) stay here; the growth curve and
   the realloc idiom are the core's. */
typedef struct { char *p; size_t n, cap; int oom; } dyn_sb_t;

static void dyn_sb_init(dyn_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_sb_free(dyn_sb_t *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

static void dyn_sb_put(dyn_sb_t *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_sb_putc(dyn_sb_t *b, char c) { dyn_sb_put(b, &c, 1); }
static void dyn_sb_puts(dyn_sb_t *b, const char *s) { dyn_sb_put(b, s, strlen(s)); }

/* ------------------------------------------------------ percent encoding */

/* One hex parser for the whole file: the core's (the census found six
 * hand-rolled copies of these five lines across modules). */
static int dyn_hexval(int c) { return dyn_pct_hexval(c); }

static void dyn_sb_sink(void *ud, const char *b, size_t n)
{
    dyn_sb_put((dyn_sb_t *)ud, b, n);
}

/* Decode %XX via the shared core (src/core/dyn-pct.h): malformed escapes
   stay LITERAL there -- losing bytes silently is how a decoder turns a bad
   request into a different one. */
static void dyn_pct_decode(dyn_sb_t *out, const char *s, size_t n, int plus_space)
{
    dyn_pct_decode_core(out, dyn_sb_sink, s, n, plus_space);
}

/* The exported codecs' two safe-sets live in the core now
 * (src/core/dyn-pct.h): form=1 is x-www-form-urlencoded (unreserved +
 * *-._, space->'+', the formEncode/encodeURIComponentStrict set);
 * form=2 is the WHATWG urlencoded set URLSearchParams serializes with --
 * it deliberately does NOT escape ~ ' ! * ( ) etc., matching the spec and
 * Node (`new URLSearchParams({a:"1~2"})` serialises "a=1~2"). */
static void dyn_pct_encode(dyn_sb_t *out, const char *s, size_t n, int form)
{
    dyn_pct_encode_core(out, dyn_sb_sink, s, n, form);
}

/* --------------------------------------------------------------- the URL */

/* WHATWG parse-time percent-encode sets (URL Standard, "percent-encode by
   default" applied by each parser state when a component is appended). Each
   is the C0-control set -- C0 controls, DEL, and every code point above it --
   plus a few ASCII bytes:
     opaque path:  the C0-control set only (space stays literal)
     path:         + space " # < > ? ` { }
     userinfo:     path set + / and :
     query:        + space " # < >      (+ ' for special schemes)
     fragment:     + space " < > `
   The components are STORED encoded (the spec stores the encoded form), so
   the .pathname/.search/.hash/.username getters read back the canonical
   spelling and href needs no component re-encoding to agree with them. */
typedef enum {
    DYN_SET_OPAQUE, DYN_SET_PATH, DYN_SET_USERINFO,
    DYN_SET_QUERY, DYN_SET_QUERY_SPECIAL, DYN_SET_FRAGMENT
} dyn_parse_set_t;

static int dyn_set_forbidden(dyn_parse_set_t set, unsigned char c)
{
    if (c <= 0x1F || c > 0x7E)
        return 1;                 /* the C0-control set */
    switch (set) {
    case DYN_SET_OPAQUE:
        return 0;
    case DYN_SET_PATH:
        /* the spec's path set: query set + ? ^ ` { } (U+005E was added to the
           standard's path set when the caret interop hole was closed) */
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>' ||
               c == '?' || c == '^' || c == '`' || c == '{' || c == '}';
    case DYN_SET_USERINFO:
        /* the WHATWG userinfo set: the path set plus / : ; = @ [ \ ] ^ |
           (a credential field may not smuggle delimiters or bracket
           syntax past a Host header) */
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>' ||
               c == '?' || c == '`' || c == '{' || c == '}' ||
               c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
               c == '[' || c == '\\' || c == ']' || c == '^' || c == '|';
    case DYN_SET_QUERY:
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>';
    case DYN_SET_QUERY_SPECIAL:
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>' ||
               c == '\'';
    default:                      /* DYN_SET_FRAGMENT */
        return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
    }
}

/* UTF-8 percent-encode `s` under `set` into `b`. Bytes not in the set pass
   through verbatim, INCLUDING a '%' with no hex pair behind it: the spec's
   encoder is a byte filter, not a decoder, so "%2f" stays "%2f". */
static void dyn_parse_encode(dyn_sb_t *b, const char *s, size_t n,
                             dyn_parse_set_t set)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_set_forbidden(set, c)) {
            dyn_sb_putc(b, (char)c);
        } else {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, HEX[c >> 4]);
            dyn_sb_putc(b, HEX[c & 0xF]);
        }
    }
}

/* Encode `s[0..n)` into a fresh malloc'd string the caller owns (NULL on
   OOM). */
static char *dyn_strndup(const char *s, size_t n);   /* defined below */
static char *dyn_encode_dup(const char *s, size_t n, dyn_parse_set_t set)
{
    dyn_sb_t b;
    char *r;
    dyn_sb_init(&b);
    dyn_parse_encode(&b, s, n, set);
    if (b.oom) { dyn_sb_free(&b); return NULL; }
    r = dyn_strndup(b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

typedef struct {
    char *scheme, *username, *password, *host, *port, *path, *query, *fragment;
    char *drive;              /* file: a Windows drive that landed in the
                                 authority (file://d is file:///d:) */
    int has_authority;
    int refs;                 /* URLSearchParams bindings hold a raw ref */
} dyn_url_t;

static JSClassID dyn_url_class_id;

static void dyn_url_free(void *p)
{
    dyn_url_t *u = (dyn_url_t *)p;
    free(u->scheme); free(u->username); free(u->password); free(u->host);
    free(u->port); free(u->path); free(u->query); free(u->fragment);
    free(u->drive);
    free(u);
}

static void dyn_url_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_url_t *u = (dyn_url_t *)JS_GetOpaque(val, dyn_url_class_id);
    (void)rt;
    /* A bound URLSearchParams holds a raw reference (it must keep the URL's
       C state alive after the JS object is collected); free only when the
       last one drops. Not a refcount cycle: refs is never a JS edge. */
    if (u && --u->refs == 0)
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

/* Defined after the IDNA include; see the definition for the rules. */
static int dyn_url_normalize_host(dyn_url_t *u);

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
   `..` past the root is dropped, not allowed to escape above it.
   WHATWG addition: a "single-dot path segment" is "." or an ASCII
   case-insensitive "%2e"; a "double-dot" is "..", ".%2e", "%2e." or "%2e%2e"
   (same case rule) -- the parser runs this on the still-encoded path, so
   /foo/%2e must fold like /foo/. */
static int dyn_is_dot_seg(const char *p, size_t len, int dbl)
{
        if (dbl) {
            if (len == 2 && p[0] == '.' && p[1] == '.')
                return 1;
            if (len == 4 && p[0] == '.' &&
                (p[1] | 0x20) == '%' &&
                (p[2] | 0x20) == '2' && (p[3] | 0x20) == 'e')
                return 1;
            if (len == 4 && (p[0] | 0x20) == '%' &&
                (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e' &&
                p[3] == '.')
                return 1;
            if (len == 6 && (p[0] | 0x20) == '%' &&
                (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e' &&
                (p[3] | 0x20) == '%' &&
                (p[4] | 0x20) == '2' && (p[5] | 0x20) == 'e')
                return 1;
            return 0;
        }
    if (len == 1 && p[0] == '.')
        return 1;
    if (len == 3 && (p[0] | 0x20) == '%' &&
        (p[1] | 0x20) == '2' && (p[2] | 0x20) == 'e')
        return 1;
    return 0;
}

/* remove_dot_segments over the slash-separated segments of `p` (the path
   WITHOUT its leading slash; one is implied and re-added). The path is a
   LIST: empty segments are preserved, because ["",""] serializes as "//"
   and ["","p"] as "/.//p" (the "." is inserted at parse end, below, for
   every scheme but file). `..` pops the last segment; for a file URL a
   lone leading drive segment ("C:") is protected -- <..> from <file:///C:/>
   stays at /C:/ instead of climbing past the drive. */
static void dyn_remove_dots(dyn_sb_t *out, const char *p, size_t n,
                            int file_drive)
{
    dyn_sb_t seg;                 /* slash-joined kept segments */
    size_t i = 0;
    dyn_sb_init(&seg);
    while (i <= n) {
        size_t b = i;
        while (i < n && p[i] != '/')
            i++;
        {
            size_t len = i - b;
            if (dyn_is_dot_seg(p + b, len, 0)) {
                /* "." contributes nothing, but it DOES leave the path
                   ending in a slash when it ends the input: <./> resolves
                   to "/b/c/", not "/b/c" */
                if (i >= n)
                    dyn_sb_putc(&seg, '/');
            } else if (dyn_is_dot_seg(p + b, len, 1)) {
                if (file_drive && seg.n == 3 &&
                    ((seg.p[1] | 0x20) >= 'a' && (seg.p[1] | 0x20) <= 'z') &&
                    seg.p[2] == ':') {
                    /* a ".." at the drive root stays on the drive */
                } else {
                    size_t last = 0;
                    int has = 0;
                    size_t k;
                    for (k = 0; k < seg.n; k++)
                        if (seg.p[k] == '/') { last = k; has = 1; }
                    seg.n = has ? last : 0;
                }
                if (i >= n)
                    dyn_sb_putc(&seg, '/');
            } else {
                dyn_sb_putc(&seg, '/');
                dyn_sb_put(&seg, p + b, len);
            }
        }
        if (i >= n)
            break;
        i++;
    }
    if (seg.n == 0)
        dyn_sb_putc(out, '/');
    else
        dyn_sb_put(out, seg.p, seg.n);
    dyn_sb_free(&seg);
}

/* Split the authority into userinfo / host / port. IPv6 literals keep their
   brackets, and the LAST colon outside them is the port separator. Sets
   *port_colon when a ':' appeared in host position (even for an empty port):
   a non-special URL with an empty host and any port position is a parse
   failure. */
static int dyn_parse_authority(dyn_url_t *u, const char *s, size_t n,
                               int *port_colon)
{
    size_t at = (size_t)-1, i;
    size_t hb, he;
    int special = u->scheme ? dyn_scheme_is_special(u->scheme) : 0;
    for (i = 0; i < n; i++)
        if (s[i] == '@')
            at = i;                          /* last '@' wins, per spec */
    if (at != (size_t)-1) {
        size_t c = (size_t)-1;
        for (i = 0; i < at; i++)
            if (s[i] == ':') { c = i; break; }
        if (c != (size_t)-1) {
            u->username = dyn_encode_dup(s, c, DYN_SET_USERINFO);
            u->password = dyn_encode_dup(s + c + 1, at - c - 1,
                                         DYN_SET_USERINFO);
        } else {
            u->username = dyn_encode_dup(s, at, DYN_SET_USERINFO);
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
            /* Nothing may follow the closing bracket except a port colon:
               bytes after ']' used to be dropped silently, so http://[::1]j/
               parsed as http://[::1]/ -- one URL read as another. */
            if (close + 1 < n)
                return -1;
            he = close + 1;
        }
    } else if (!(u->scheme && !strcmp(u->scheme, "file"))) {
        /* file: has no port state -- the spec's file host state appends the
           ':' to the host buffer instead (file://d: is the DRIVE d:, and
           file://host: is a host-parse failure, not an empty port), so the
           generic last-colon port split does not run for it. */
        size_t c = (size_t)-1;
        for (i = hb; i < n; i++)
            if (s[i] == ':') c = i;
        if (c != (size_t)-1) {
            if (port_colon)
                *port_colon = 1;
            u->port = dyn_strndup(s + c + 1, n - c - 1);
            he = c;
        }
    }
    u->host = dyn_strndup(s + hb, he - hb);
    if (!u->host)
        return -1;
    /* WHATWG forbidden host code points that survive the split: space and the
       punctuation an authority can otherwise carry; a bare ':' (the bracketed
       form returns above) and '[' ']' outside a literal are host-state
       failures; NUL is refused outright. Raw C0/DEL: a domain host refuses
       them at parse time (a CR here would ride into a Host header built
       later from .host and split it), while an OPAQUE host merely
       percent-encodes them -- that encoding happens in
       dyn_url_normalize_host. The scan is length-based: a NUL in the middle
       of u->host must not end it early. */
    if (u->host[0] != '[') {
        const char *h = u->host;
        size_t hl = he - hb, k2;
        /* a Windows drive in the authority position (file://d/, file://C|/)
           is legal for file: and moves to the path later */
        int file_drive_host = 0;
        if (u->scheme && !strcmp(u->scheme, "file") &&
            hl >= 1 && hl <= 2 &&
            ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z') &&
            (hl == 1 || u->host[1] == '|' || u->host[1] == ':'))
            file_drive_host = 1;
        for (k2 = 0; k2 < hl && !file_drive_host; k2++) {
            unsigned char c = (unsigned char)h[k2];
            if (c == 0x00 || c == 0x20 || c == '<' || c == '>' || c == '\\' ||
                c == '^' || c == '|' || c == '[' || c == ']' || c == ':')
                return -1;
            if (special && ((c < 0x20) || c == 0x7F))
                return -1;
        }
    }
    if (u->port) {                            /* validate and normalise */
        const char *d;
        size_t k, pl = strlen(u->port);
        uint64_t pv = 0;
        /* WHATWG port parser: digits only, unbounded value arithmetic,
           failure past 65535. Leading zeros are syntax, not error: the
           parsed VALUE is the port, so 00000000000000 is 0 and 080 is 80
           (then dropped as the default). */
        for (k = 0; k < pl; k++) {
            if (u->port[k] < '0' || u->port[k] > '9')
                return -1;
            pv = pv * 10 + (uint64_t)(u->port[k] - '0');
            if (pv > 65535)
                return -1;
        }
        if (pl == 0) { free(u->port); u->port = NULL; }
        else {
            char canon[6];
            snprintf(canon, sizeof canon, "%llu", (unsigned long long)pv);
            free(u->port);
            u->port = dyn_strndup(canon, strlen(canon));
            if (!u->port)
                return -1;
        }
        /* file: has no port -- file://example:1/ is a parse failure */
        if (u->port && u->scheme && !strcmp(u->scheme, "file"))
            return -1;
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
        /* WHATWG no-scheme state: a base with an OPAQUE path has no
           authority and no path structure to resolve against -- only a
           #fragment-only reference (or the empty ref) is accepted,
           anything else is a failure (<i>, <../i>, <?i> against <sc:sd>;
           <#i> still works). A single-slash non-special base (<sc:/pa/pa>)
           has a NORMAL path and resolves fine. */
        if (!base->has_authority && !(n && s[0] == '#') &&
            !(base->path && base->path[0] == '/'))
            return -1;
        i = 0;
    }

    special = dyn_scheme_is_special(u->scheme);
    /* fragment and query are split off the tail first: both may contain any
       character, including the delimiters of the components before them. */
    {
        size_t hash = (size_t)-1, q = (size_t)-1, k;
        size_t end = n;
        for (k = i; k < n; k++) {
            if (s[k] == '#') { hash = k; break; }
        }
        if (hash != (size_t)-1) {
            u->fragment = dyn_encode_dup(s + hash + 1, n - hash - 1,
                                         DYN_SET_FRAGMENT);
            end = hash;
        }
        for (k = i; k < end; k++)
            if (s[k] == '?') { q = k; break; }
        if (q != (size_t)-1) {
            u->query = dyn_encode_dup(s + q + 1, end - q - 1,
                                      special ? DYN_SET_QUERY_SPECIAL
                                              : DYN_SET_QUERY);
            end = q;
        }
        n = end;
    }

    /* WHATWG preprocessing: for special schemes every backslash is a slash, so
       https:\evil.com and https://\evil.com parse as https://evil.com/. The
       authority scan below already treats '\' as an authority terminator for
       special schemes (dyn_authority_end); accepting it here as the leader
       matches the spec's single preprocessing step. */
    /* WHATWG "special relative or authority state": when the reference
       carries its own special scheme AND the base has the SAME scheme,
       anything not starting with a "//" (or "\") leader is a PATH-RELATIVE
       reference -- <http:foo.com> against an http base resolves to
       <base-dir>/foo.com, while with no base (or a different scheme) it is
       an authority name (the special authority slashes state accepts ANY
       run of '/' and '\', including NONE). Used again by the path merge
       below, so it lives outside the authority block. */
    int special_same_base = has_scheme && special && base && base->scheme &&
                            strcmp(base->scheme, u->scheme) == 0;
    /* a Windows-drive reference against a file base (<C|/foo/bar>,
       <file:c:/x>) starts a FRESH path instead of merging with the base's */
    int file_drive_ref = 0;
    int file_slash_opened = 0;
    int file_noslash_drive = 0;
    if (has_scheme && special && !strcmp(u->scheme, "file") && n - i >= 2) {
        /* <file:C|/m/> and <file:\c:\foo\bar>: the drive starts a fresh
           path; a leading slash/backslash is stepped over */
        size_t fj = (s[i] == '/' || s[i] == '\\') ? i + 1 : i;
        if (n - fj >= 2 &&
            ((s[fj] | 0x20) >= 'a' && (s[fj] | 0x20) <= 'z') &&
            (s[fj + 1] == '|' || s[fj + 1] == ':') &&
            (n - fj == 2 || s[fj + 2] == '/' || s[fj + 2] == '\\' ||
             s[fj + 2] == '?' || s[fj + 2] == '#')) {
            file_drive_ref = 1;
            if (fj == i)
                file_noslash_drive = 1;
        }
    } else if (base && base->scheme && !strcmp(base->scheme, "file")) {
        size_t fi = has_scheme ? i : 0;
        if (n - fi >= 2 &&
            ((s[fi] | 0x20) >= 'a' && (s[fi] | 0x20) <= 'z') &&
            (s[fi + 1] == '|' || s[fi + 1] == ':') &&
            (n - fi == 2 || s[fi + 2] == '/' || s[fi + 2] == '\\' ||
             s[fi + 2] == '?' || s[fi + 2] == '#'))
            file_drive_ref = 1;
    }
    {
        size_t ab = n + 1;                    /* n+1 = "no authority" sentinel */
        if (has_scheme && special && strcmp(u->scheme, "file") != 0 &&
            !special_same_base) {
            ab = i;
            while (ab < n && (s[ab] == '/' || s[ab] == '\\'))
                ab++;
        } else if (i + 1 < n &&
                   (s[i] == '/' || (special && s[i] == '\\')) &&
                   (s[i + 1] == '/' || (special && s[i + 1] == '\\'))) {
            /* a "//" (or "\\") leader defines its own authority: scheme-less
               network-path references (<//g>), non-special schemes
               (<sc://h>), file, and a same-scheme special ref with an
               explicit leader */
            ab = i + 2;
            /* the spec's special-authority-ignore-slashes state: for the
               SPECIAL schemes other than file, the leader is a RUN of '/'
               and '\' -- <///test> against an http base parses "test" as the
               host, and <http:////x> is http://x/. A non-special scheme
               takes exactly two slashes (its third would start the path),
               and file:'s host state swallows the extra slashes itself. */
            if (special && strcmp(u->scheme, "file") != 0)
                while (ab < n && (s[ab] == '/' || s[ab] == '\\'))
                    ab++;

        } else if (has_scheme && !strcmp(u->scheme, "file") && i >= n) {
            /* <file:>, <file:?q>, <file:#f>: no authority text at all, but
               the empty authority is still serialized (file:///?) */
            u->has_authority = 1;
            u->host = dyn_strndup("", 0);
            file_slash_opened = 1;
        } else if (has_scheme && !strcmp(u->scheme, "file") &&
                   i < n && (s[i] == '/' || s[i] == '\\')) {
            /* file's LONE slash opens the path; the authority is empty --
               file:/example.com/ is file:///example.com/ -- EXCEPT over a
               file base, whose host is kept (file:/C:/ over file://host/
               is file://host/C:/) */
            u->has_authority = 1;
            if (base && base->scheme && !strcmp(base->scheme, "file")) {
                u->has_authority = base->has_authority;
                if (base->host)
                    u->host = dyn_strndup(base->host, strlen(base->host));
            } else {
                u->host = dyn_strndup("", 0);
                file_slash_opened = 1;
            }
            i = i + 1;
        } else if (file_noslash_drive) {
            /* <file:C|/m/> (no base): the drive replaces any authority.
               With a file base the base's host is KEPT (file:C:/ over
               file://host/dir is file://host/C:/) and the inherit branch
               below still runs. */
            u->has_authority = 1;
            if (base && base->scheme && !strcmp(base->scheme, "file")) {
                u->has_authority = base->has_authority;
                if (base->host)
                    u->host = dyn_strndup(base->host, strlen(base->host));
            } else {
                u->host = dyn_strndup("", 0);
                file_slash_opened = 1;
            }
        }
        if (ab <= n) {
            size_t ae = ab;
            int port_colon = 0;
            while (ae < n && !dyn_authority_end(s[ae], special))
                ae++;
            if (dyn_parse_authority(u, s + ab, ae - ab, &port_colon) < 0)
                return -1;
            if (dyn_url_normalize_host(u) < 0)
                return -1;
            /* WHATWG: a special scheme with an empty host is a parse error
               (http:///p is rejected by browsers; the empty host would sail past
               host-allowlist gates and route by inherited/default host).
               file: is THE exception -- file:///path is its ordinary spelling,
               the origin is opaque anyway, and no network gate sees it.
               For a non-special scheme an empty host is legal ONLY bare:
               sc://@/ (credentials) and sc://:12/ (a port position) fail,
               exactly like the spec's host-state finalize. */
            if (special && strcmp(u->scheme, "file") != 0 &&
                u->host && u->host[0] == '\0')
                return -1;
            if (!special && u->host && u->host[0] == '\0' &&
                (port_colon || u->port || u->username || u->password))
                return -1;
            u->has_authority = 1;
            i = ae;
            base = NULL;      /* <//g> defines its own authority: the base's path
                                 and query are NOT inherited (RFC 3986 5.2.2) */
            if (i >= n) {
                /* WHATWG path serialization: a special-scheme URL with an
                   authority and no path segments carries a single empty
                   segment, which serializes as "/" -- new URL("http://x") and
                   the network-path reference <//www.example2.com> both have
                   pathname "/". A non-special scheme's empty path list stays
                   the empty string (new URL("sc://h").pathname === ""). */
                if (u->drive) {
                    /* file://d has no path: the drive IS it (file:///d:) */
                    u->path = dyn_strndup(u->drive, strlen(u->drive));
                } else if (special)
                    u->path = dyn_strndup("/", 1);
                else
                    u->path = dyn_strndup("", 0);
                return u->path ? 0 : -1;
            }
        } else if (base && !file_slash_opened &&
                   (!has_scheme || special_same_base || file_drive_ref)) {
            /* a relative reference (scheme-less, or a special scheme over
               its own base) inherits the base's authority (WHATWG
               "special relative or authority" / RFC 3986 5.2) */
            u->has_authority = base->has_authority;
            if (base->host)     u->host = dyn_strndup(base->host, strlen(base->host));
            if (base->port)     u->port = dyn_strndup(base->port, strlen(base->port));
            if (base->username) u->username = dyn_strndup(base->username, strlen(base->username));
            if (base->password) u->password = dyn_strndup(base->password, strlen(base->password));
        } else if (has_scheme && !file_slash_opened) {
            if (u->scheme && !strcmp(u->scheme, "file")) {
                /* the spec's file state ALWAYS sets the (empty) host before
                   falling to the path state: <file:a> over a non-file base
                   resolves fresh from the root as file:///a (and over a file
                   base the inherit branch above already ran) */
                u->has_authority = 1;
                u->host = dyn_strndup("", 0);
                if (!u->host)
                    return -1;
            } else {
                u->has_authority = 0;             /* opaque path, e.g. mailto: */
            }
        }
    }

    /* the path, resolved against the base when the reference is relative */
    {
        dyn_sb_t out;
        /* WHATWG path states treat '\' as '/' for SPECIAL schemes (the
           authority's leader/terminator already do, above); the path region
           gets one converted copy. Query and fragment are NOT converted. */
        char *conv = NULL;
        const char *ps = s;
        /* a scheme-less reference, or a special scheme over its own base,
           resolves against the base's path */
        int rel = !has_scheme || special_same_base;
        int is_file = u->scheme && !strcmp(u->scheme, "file");
        /* A non-special scheme followed by a SINGLE slash has a normal,
           dot-segmented path ("sc:/pa/pa" is not opaque -- the spec's path
           state runs there); only scheme-text with no leading slash is an
           opaque path (mailto:..., data:...). file: is NEVER opaque: the
           spec's file state always sets the empty host and falls through to
           the path state, so <file:a> is file:///a, not file:a. */
        int opaque = !u->has_authority && has_scheme && !is_file &&
                     !(i < n && s[i] == '/');
        if (special && i < n) {
            size_t k;
            int has = 0;
            for (k = i; k < n; k++)
                if (s[k] == '\\') { has = 1; break; }
            if (has) {
                conv = (char *)malloc(n - i);
                if (!conv)
                    return -1;
                for (k = i; k < n; k++)
                    conv[k - i] = (s[k] == '\\') ? '/' : s[k];
                ps = conv - i;    /* ps[i..n) is the converted region */
            }
        }
        dyn_sb_init(&out);
        if (i < n && dyn_authority_end(ps[i], special)) {
            /* the spec's file-slash-state drive quirk: a slash-opening
               reference against a file base whose path sits at a Windows
               drive (<file:///C:/a/b>) resolves from the DRIVE ROOT -- the
               drive segment is carried, the base's own directories are not
               (</> against file:///C:/a/b is file:///C:/, not file:///C:/a/)
               -- UNLESS the reference itself starts with a Windows drive
               letter (</c:/foo/bar> carries its own drive: file:///c:/foo/bar) */
            if (is_file && rel && base && base->path &&
                base->path[0] == '/' &&
                ((base->path[1] | 0x20) >= 'a' && (base->path[1] | 0x20) <= 'z') &&
                base->path[2] == ':' &&
                (base->path[3] == '\0' || base->path[3] == '/')) {
                size_t rl = n - i - 1;
                int ref_is_drive = rl >= 2 &&
                    ((ps[i + 1] | 0x20) >= 'a' && (ps[i + 1] | 0x20) <= 'z') &&
                    (ps[i + 2] == ':' || ps[i + 2] == '|') &&
                    (rl == 2 || ps[i + 3] == '/' || ps[i + 3] == '\\' ||
                     ps[i + 3] == '?' || ps[i + 3] == '#');
                if (!ref_is_drive) {
                    dyn_sb_t t;
                    dyn_sb_init(&t);
                    dyn_sb_put(&t, base->path + 1, 2);   /* the "C:" drive */
                    dyn_sb_putc(&t, '/');
                    dyn_sb_put(&t, ps + i + 1, n - i - 1);
                    dyn_remove_dots(&out, t.p, t.n, 1);
                    dyn_sb_free(&t);
                } else {
                    dyn_remove_dots(&out, ps + i + 1, n - i - 1, is_file);
                }
            } else {
                dyn_remove_dots(&out, ps + i + 1, n - i - 1, is_file);
            }
        } else if (opaque) {
            /* The spec's opaque path state, encoded HERE (not at store time)
               because the space rule needs position context: a U+0020 SPACE
               percent-encodes to %20 exactly when the remaining input starts
               with '?' or '#' -- the delimiter would otherwise be read as
               part of the path on re-parse -- and stays literal otherwise.
               Everything else: the C0-control percent-encode set. */
            static const char HEX[] = "0123456789ABCDEF";
            size_t k;
            for (k = i; k < n; k++) {
                unsigned char c = (unsigned char)ps[k];
                if (c == ' ') {
                    unsigned char nx = (k + 1 < n) ? (unsigned char)ps[k + 1]
                        : (u->query || u->fragment) ? (unsigned char)s[n] : 0;
                    if (nx == '?' || nx == '#')
                        dyn_sb_puts(&out, "%20");
                    else
                        dyn_sb_putc(&out, ' ');
                } else if (!dyn_set_forbidden(DYN_SET_OPAQUE, c)) {
                    dyn_sb_putc(&out, (char)c);
                } else {
                    dyn_sb_putc(&out, '%');
                    dyn_sb_putc(&out, HEX[c >> 4]);
                    dyn_sb_putc(&out, HEX[c & 0xF]);
                }
            }
        } else if (i >= n) {
            if (base && base->path && rel) {
                dyn_sb_puts(&out, base->path);
                /* RFC 3986 5.2.2: an empty path takes the base's query too, so
                   <> and <#s> keep ?q. Only when the reference had none. */
                if (!u->query && base->query)
                    u->query = dyn_strndup(base->query, strlen(base->query));
            } else {
                dyn_sb_putc(&out, '/');
            }
        } else if (file_drive_ref) {
            size_t first = has_scheme ? i : 0;
            /* <C|/foo/bar> against a file base starts at the drive root and
               never merges with the base path */
            dyn_sb_putc(&out, '/');
            dyn_sb_put(&out, ps + first, n - first);
            if (out.n > 2 && out.p[2] == '|')
                out.p[2] = ':';
            {
                dyn_sb_t t;
                dyn_sb_init(&t);
                dyn_remove_dots(&t, out.p + 1, out.n - 1, 1);
                dyn_sb_free(&out);
                out = t;
            }
        } else if (base && base->path && rel) {
            dyn_sb_t merged;                  /* RFC 3986 sec.5.3 merge */
            size_t bl = strlen(base->path), cut = 0, k;
            int skip = 1;
            dyn_sb_init(&merged);
            for (k = 0; k < bl; k++)
                if (base->path[k] == '/') cut = k + 1;
            if (cut == 0 && base->path[0] != '/') {
                /* an empty base path contributes no leading slash, so the
                   merged string IS the reference: remove_dots must see all
                   of it (it re-adds the leading '/' itself) */
                skip = 0;
            }
            dyn_sb_put(&merged, base->path, cut);
            dyn_sb_put(&merged, ps + i, n - i);
            dyn_remove_dots(&out, merged.p + skip,
                            merged.n ? merged.n - skip : 0, is_file);
            dyn_sb_free(&merged);
        } else {
            dyn_remove_dots(&out, ps + i, n - i, is_file);
        }
        /* A file path's first segment may be a Windows drive spelled with
           '|': file:///w|/m is file:///w:/m. Case is kept (c: stays c:);
           an "X||" first segment is NOT a drive and stays verbatim. */
        if (is_file && out.n >= 3 && out.p[0] == '/' &&
            ((out.p[1] | 0x20) >= 'a' && (out.p[1] | 0x20) <= 'z') &&
            ((out.p[2] == ':') ||
             (out.p[2] == '|' && (out.n == 3 || out.p[3] == '/')))) {
            if (out.p[2] == '|')
                out.p[2] = ':';
        }
        /* a drive that landed in the authority (file://d/) prefixes the
           path: file://d/ is file:///d:/ and file://d:/.. is file:///d:/ --
           the trailing empty segment of a slash- or dot-ref survives, exactly
           as the spec's path state appends it after the drive segment */
        if (u->drive) {
            dyn_sb_t t;
            dyn_sb_init(&t);
            dyn_sb_puts(&t, u->drive);
            dyn_sb_put(&t, out.p ? out.p : "/", out.n ? out.n : 1);
            dyn_sb_free(&out);
            out = t;
        }
        free(conv);
        if (out.oom) { dyn_sb_free(&out); return -1; }
        /* Components are stored PERCENT-ENCODED (what each WHATWG parser
           state appends): the path uses the path set, an opaque path (a
           non-special scheme with no authority) the C0-control set only. */
        /* Opaque paths were encoded in the branch above (the space rule
           needs position context, so re-encoding here would corrupt its
           %20). A normal path encodes now with the PATH set -- including a
           non-special scheme's single-slash path, which the spec runs
           through the path state (sc:/a b is sc:/a%20b, not sc:/a b). */
        if (opaque)
            u->path = dyn_strndup(out.p ? out.p : "", out.n);
        else
            u->path = dyn_encode_dup(out.p ? out.p : "", out.n,
                                     DYN_SET_PATH);
        dyn_sb_free(&out);
    }
    return u->path ? 0 : -1;
}

/* WHATWG serialization percent-encode sets. Each is the C0-control set (C0,
 * DEL, non-ASCII) plus space and a few ASCII bytes:
 *   path:     space " # < > ? ^ ` { }       (userinfo/host use this too)
 *   query:    space " # < >
 *   fragment: space " < > `
 * The parse-time sets above do the real encoding; this pass is the href
 * safety net and mirrors them (U+005E is in the standard's path set). */
typedef enum { DYN_PCT_PATH, DYN_PCT_QUERY, DYN_PCT_FRAGMENT } dyn_pct_set_t;

static int dyn_pct_forbidden(dyn_pct_set_t set, unsigned char c)
{
    if (c <= 0x1F || c >= 0x7F || c == ' ' || c == '"')
        return 1;                 /* the C0-control set plus space and `"` */
    switch (set) {
    case DYN_PCT_PATH:
        return c == '#' || c == '<' || c == '>' || c == '?' || c == '^'
            || c == '`' || c == '{' || c == '}';
    case DYN_PCT_QUERY:
        return c == '#' || c == '<' || c == '>';
    default:                      /* DYN_PCT_FRAGMENT */
        return c == '<' || c == '>' || c == '`';
    }
}

static void dyn_href_component(dyn_sb_t *b, const char *s, dyn_pct_set_t set)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    if (!s)
        return;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int h1, h2;
        /* An existing escape passes through VERBATIM: the spec's encoder is a
           byte filter and '%' is in no percent-encode set, so the hex case of
           an escape that was already in the input (or that the parse stored)
           is not ours to change -- urltestdata pins mixed case
           <%3A%3a%3C%3c>. Escapes this serializer produces itself (below) are
           still uppercase, which is what the spec's "ASCII upper hex digits"
           mandates. A bare % with no hex pair behind it is itself encoded. */
        if (c == '%' && (h1 = dyn_hexval((unsigned char)s[i + 1])) >= 0
                     && (h2 = dyn_hexval((unsigned char)s[i + 2])) >= 0) {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, s[i + 1]);
            dyn_sb_putc(b, s[i + 2]);
            i += 2;
            continue;
        }
        if (!dyn_pct_forbidden(set, c)) {
            dyn_sb_putc(b, (char)c);
        } else {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, HEX[c >> 4]);
            dyn_sb_putc(b, HEX[c & 0xF]);
        }
    }
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
        if ((u->username && *u->username) || (u->password && *u->password)) {
            /* an empty username with a password still serializes the
               colon: http://:pw@host/ (what :pw@ parsed to) */
            if (u->username)
                dyn_sb_puts(&b, u->username);
            if (u->password && *u->password) {
                dyn_sb_putc(&b, ':');
                dyn_sb_puts(&b, u->password);
            }
            dyn_sb_putc(&b, '@');
        }
        /* the host is stored canonical (lowercased/IDNA/bracketed) and was
           validated at parse; no percent-set pass over it */
        dyn_sb_puts(&b, u->host ? u->host : "");
        if (u->port) { dyn_sb_putc(&b, ':'); dyn_sb_puts(&b, u->port); }
    }
    /* The spec's URL serializer: a host-NULL URL (non-special scheme, no
       authority) whose path's first segment is empty -- the stored path
       starts with "//" -- serializes "/." before the path, so
       <web+demo:/path/..//h> round-trips instead of re-parsing its path as
       an authority. Opaque paths never start with '/' and file: URLs carry
       an authority, so neither reaches this. */
    if (u->path && !u->has_authority &&
        u->path[0] == '/' && u->path[1] == '/')
        dyn_sb_puts(&b, "/.");
    if (u->path) {
        if (u->has_authority)
            dyn_href_component(&b, u->path, DYN_PCT_PATH);
        else
            dyn_sb_puts(&b, u->path);   /* opaque: stored C0-encoded, raw */
    }
    if (u->query)    { dyn_sb_putc(&b, '?'); dyn_href_component(&b, u->query, DYN_PCT_QUERY); }
    if (u->fragment) { dyn_sb_putc(&b, '#'); dyn_href_component(&b, u->fragment, DYN_PCT_FRAGMENT); }
    if (b.oom) { dyn_sb_free(&b); return JS_ThrowOutOfMemory(ctx); }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static dyn_url_t *dyn_url_of(JSContext *ctx, JSValueConst v)
{
    return (dyn_url_t *)dyn_plain_get(ctx, v, dyn_url_class_id);
}

/* WHATWG input preprocessing: strip ALL tab, LF and CR from a URL string
   before parsing -- only the ENDS used to be trimmed, so an interior newline
   survived into .pathname/.search, where one URL read as two. Returns a
   malloc'd copy the caller frees and sets *outn, or NULL when there was
   nothing to strip (use the input as-is) or on OOM. */
static char *dyn_strip_tabnl(const char *s, size_t n, size_t *outn)
{
    char *r;
    size_t i, w = 0;
    int dirty = 0;
    for (i = 0; i < n; i++)
        if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') { dirty = 1; break; }
    if (!dirty)
        return NULL;
    r = (char *)malloc(n);
    if (!r)
        return NULL;
    for (i = 0; i < n; i++)
        if (s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
            r[w++] = s[i];
    *outn = w;
    return r;
}

static JSValue dyn_url_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    const char *s = NULL, *bs = NULL;   /* what we parse: JS buffer or a copy */
    const char *s_js = NULL, *bs_js = NULL;  /* JS-owned originals to free */
    char *cs = NULL, *cbs = NULL;       /* owned tab/LF/CR-stripped copies */
    size_t n = 0, bn = 0;
    dyn_url_t *u = NULL, base;
    int have_base = 0;

    memset(&base, 0, sizeof base);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new URL(input[, base]): input must be a string");
    s_js = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s_js)
        return JS_EXCEPTION;
    s = s_js;
    cs = dyn_strip_tabnl(s, n, &n);   /* strip interior tab/LF/CR first */
    if (cs)
        s = cs;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        bs_js = JS_ToCStringLen(ctx, &bn, argv[1]);
        if (!bs_js)
            goto fail;
        bs = bs_js;
        cbs = dyn_strip_tabnl(bs, bn, &bn);
        if (cbs)
            bs = cbs;
        if (bn > DYN_URL_MAX || dyn_url_parse(&base, bs, bn, NULL) < 0) {
            JS_ThrowTypeError(ctx, "new URL(input, base): base is not a valid URL");
            goto fail;
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
    free(cs);
    free(cbs);
    JS_FreeCString(ctx, s_js);
    if (bs_js) JS_FreeCString(ctx, bs_js);
    free(base.scheme); free(base.username); free(base.password); free(base.host);
    free(base.port); free(base.path); free(base.query); free(base.fragment);
    u->refs = 1;
    return dyn_plain_wrap(ctx, dyn_url_class_id, u, dyn_url_free);
 fail:
    free(cs);
    free(cbs);
    if (s_js) JS_FreeCString(ctx, s_js);
    if (bs_js) JS_FreeCString(ctx, bs_js);
    free(base.scheme); free(base.username); free(base.password); free(base.host);
    free(base.port); free(base.path); free(base.query); free(base.fragment);
    return JS_EXCEPTION;
}

/* magic selects the component; one getter body for all of them. */
enum { U_HREF, U_PROTOCOL, U_USERNAME, U_PASSWORD, U_HOST, U_HOSTNAME, U_PORT,
       U_PATHNAME, U_SEARCH, U_HASH, U_ORIGIN, U_SEARCHPARAMS };

static JSClassID dyn_sp_class_id;
static void dyn_sp_free(void *p);
typedef struct {
    char *query;              /* owned; NULL when bound */
    dyn_url_t *host;          /* borrowed; the raw ref keeps the C state alive */
} dyn_sp_t;

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
    case U_SEARCHPARAMS: {
        /* Fresh bound object per access (the spec does the same): a cached one
           would pin the URL forever. The binding holds a raw ref to the URL's
           C state; mutations write through to u->query. */
        dyn_sp_t *sp = (dyn_sp_t *)calloc(1, sizeof(*sp));
        if (!sp)
            return JS_ThrowOutOfMemory(ctx);
        sp->host = u;
        u->refs++;
        return dyn_plain_wrap(ctx, dyn_sp_class_id, sp, dyn_sp_free);
    }
    default:                       /* U_ORIGIN: scheme://host[:port], or null */
        /* WHATWG URL, "origin getter": an origin is the (scheme, host, port)
           tuple ONLY for the special schemes other than file (file is opaque
           too); every other scheme serializes "null" -- ssh://example.com and
           sc://ho are not cross-origin-comparable with anything. */
        /* blob: first: the origin getter parses the opaque path (the whole
           "https://example.com:443/" text) as a URL, and when that inner URL
           is http or https the blob's origin IS its origin. An inner parse
           failure or any other inner scheme stays "null" (blob:ftp://x/). */
        if (!strcmp(u->scheme, "blob") && u->path) {
            dyn_url_t inner;
            memset(&inner, 0, sizeof inner);
            if (dyn_url_parse(&inner, u->path, strlen(u->path), NULL) == 0 &&
                inner.has_authority && inner.host && *inner.host &&
                (!strcmp(inner.scheme, "http") ||
                 !strcmp(inner.scheme, "https"))) {
                dyn_sb_init(&b);
                dyn_sb_puts(&b, inner.scheme);
                dyn_sb_puts(&b, "://");
                dyn_sb_puts(&b, inner.host);
                if (inner.port) { dyn_sb_putc(&b, ':'); dyn_sb_puts(&b, inner.port); }
                if (b.oom) {
                    free(inner.scheme); free(inner.username);
                    free(inner.password); free(inner.host); free(inner.port);
                    free(inner.path); free(inner.query); free(inner.fragment);
                    free(inner.drive);
                    dyn_sb_free(&b);
                    return JS_ThrowOutOfMemory(ctx);
                }
                r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
                dyn_sb_free(&b);
                free(inner.scheme); free(inner.username);
                free(inner.password); free(inner.host); free(inner.port);
                free(inner.path); free(inner.query); free(inner.fragment);
                free(inner.drive);
                return r;
            }
            free(inner.scheme); free(inner.username); free(inner.password);
            free(inner.host); free(inner.port); free(inner.path);
            free(inner.query); free(inner.fragment); free(inner.drive);
            return JS_NewString(ctx, "null");
        }
        if (!dyn_scheme_is_special(u->scheme))
            return JS_NewString(ctx, "null");
        if (!strcmp(u->scheme, "file"))
            return JS_NewString(ctx, "null");
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
    JS_CGETSET_MAGIC_DEF("searchParams", dyn_url_get, NULL, U_SEARCHPARAMS),
    JS_CFUNC_DEF("toString", 0, dyn_url_tostring),
    JS_CFUNC_DEF("toJSON", 0, dyn_url_tostring),
};

/* ---------------------------------------------------------- URLSearchParams */

/* URLSearchParams (WHATWG): the query list. Two modes: a BOUND instance came
   from url.searchParams and writes through to the URL's query slot, so a
   mutation changes url.search and url.href; a STANDALONE instance owns its
   query. The spec creates a fresh bound object on every searchParams access.
   entries()/keys()/values() return ARRAYS rather than iterator objects --
   `it.next()` is unsupported; spread, for..of, Array.from and destructuring
   work. sort() orders by the DECODED key's byte order, which for astral code
   points can diverge from the UTF-16 code-unit order the spec requires. */
typedef struct { char *k, *v; } dyn_sp_pair_t;
typedef struct { dyn_sp_pair_t *a; size_t n, cap; } dyn_sp_list_t;

#define DYN_SP_MAX_PAIRS 65536   /* bound a ctor that would loop on a huge array */

static void dyn_sp_list_init(dyn_sp_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

static void dyn_sp_list_free(dyn_sp_list_t *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->a[i].k);
        free(l->a[i].v);
    }
    free(l->a);
    memset(l, 0, sizeof(*l));
}

static int dyn_sp_list_put(dyn_sp_list_t *l, const char *k, size_t kn,
                           const char *v, size_t vn)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        dyn_sp_pair_t *na = (dyn_sp_pair_t *)realloc(l->a, nc * sizeof(*na));
        if (!na)
            return -1;
        l->a = na;
        l->cap = nc;
    }
    l->a[l->n].k = dyn_strndup(k, kn);
    if (!l->a[l->n].k)
        return -1;
    l->a[l->n].v = dyn_strndup(v, vn);
    if (!l->a[l->n].v) {
        free(l->a[l->n].k);
        return -1;
    }
    l->n++;
    return 0;
}

static dyn_sp_t *dyn_sp_of(JSContext *ctx, JSValueConst this_val)
{
    return (dyn_sp_t *)dyn_plain_get(ctx, this_val, dyn_sp_class_id);
}

static void dyn_sp_free(void *p)
{
    dyn_sp_t *sp = (dyn_sp_t *)p;
    free(sp->query);
    if (sp->host && --sp->host->refs == 0)
        dyn_url_free(sp->host);
    free(sp);
}

static const char *dyn_sp_query_str(const dyn_sp_t *sp)
{
    const char *q = sp->host ? sp->host->query : sp->query;
    return q ? q : "";
}

static int dyn_sp_set_query(dyn_sp_t *sp, const char *q, size_t n)
{
    char **slot = sp->host ? &sp->host->query : &sp->query;
    char *nq = dyn_strndup(q, n);
    if (!nq)
        return -1;
    free(*slot);
    *slot = nq;
    return 0;
}

static void dyn_sp_parse(const char *q, size_t n, dyn_sp_list_t *l)
{
    size_t i = 0;
    while (i < n) {
        size_t b = i, eq = (size_t)-1;
        dyn_sb_t k, v;
        while (i < n && q[i] != '&') {
            if (q[i] == '=' && eq == (size_t)-1)
                eq = i;
            i++;
        }
        dyn_sb_init(&k);
        dyn_sb_init(&v);
        if (eq == (size_t)-1)
            dyn_pct_decode(&k, q + b, i - b, 1);
        else {
            dyn_pct_decode(&k, q + b, eq - b, 1);
            dyn_pct_decode(&v, q + eq + 1, i - eq - 1, 1);
        }
        if (!k.oom && !v.oom)
            dyn_sp_list_put(l, k.p ? k.p : "", k.n, v.p ? v.p : "", v.n);
        dyn_sb_free(&k);
        dyn_sb_free(&v);
        i++;
    }
}

static int dyn_sp_serialize(const dyn_sp_list_t *l, dyn_sb_t *out)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (i)
            dyn_sb_putc(out, '&');
        dyn_pct_encode(out, l->a[i].k, strlen(l->a[i].k), 2);
        dyn_sb_putc(out, '=');
        dyn_pct_encode(out, l->a[i].v, strlen(l->a[i].v), 2);
    }
    return out->oom ? -1 : 0;
}

static int dyn_sp_apply(dyn_sp_t *sp, const dyn_sp_list_t *l)
{
    dyn_sb_t out;
    int rc;
    dyn_sb_init(&out);
    rc = dyn_sp_serialize(l, &out);
    if (rc == 0)
        rc = dyn_sp_set_query(sp, out.p ? out.p : "", out.n);
    dyn_sb_free(&out);
    return rc;
}

/* Resolve (key[, value]) into C strings. WHATWG USVString parameters are
   TOSTRING-COERCED, never type-checked: get(123) looks up "123" and a Symbol
   still throws (JS_ToCStringLen runs ToString). A missing/undefined value
   becomes "". Returns 0, or -1 with a pending exception (both strings
   borrowed; the caller frees them with JS_FreeCString). */
static int dyn_sp_kv(JSContext *ctx, int argc, JSValueConst *argv,
                     const char **ks, size_t *kn,
                     const char **vs, size_t *vn)
{
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "URLSearchParams method: a key is required");
        return -1;
    }
    *ks = JS_ToCStringLen(ctx, kn, argv[0]);
    if (!*ks)
        return -1;
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        if (vs) *vs = NULL;
        if (vn) *vn = 0;
    } else {
        const char *v = JS_ToCStringLen(ctx, vn, argv[1]);
        if (!v) {
            JS_FreeCString(ctx, *ks);
            return -1;
        }
        if (vs) *vs = v;
    }
    return 0;
}

static JSValue dyn_sp_append(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    if (dyn_sp_list_put(&l, ks, kn, vs ? vs : "", vn) < 0 ||
        dyn_sp_apply(sp, &l) < 0) {
        dyn_sp_list_free(&l);
        JS_FreeCString(ctx, ks);
        if (vs) JS_FreeCString(ctx, vs);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    if (vs) JS_FreeCString(ctx, vs);
    return JS_UNDEFINED;
}

static JSValue dyn_sp_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL;
    size_t kn = 0, i;
    JSValue r = JS_NULL;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, NULL, NULL) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    for (i = 0; i < l.n; i++)
        if (strcmp(l.a[i].k, ks) == 0) {
            r = JS_NewString(ctx, l.a[i].v);
            break;
        }
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    return r;
}

static JSValue dyn_sp_get_all(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL;
    size_t kn = 0, i;
    JSValue arr;
    uint32_t n = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, NULL, NULL) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_sp_list_free(&l);
        JS_FreeCString(ctx, ks);
        return JS_EXCEPTION;
    }
    for (i = 0; i < l.n; i++)
        if (strcmp(l.a[i].k, ks) == 0)
            JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, l.a[i].v));
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    return arr;
}

static JSValue dyn_sp_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL;
    size_t kn = 0, i;
    JSValue r = JS_FALSE;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, NULL, NULL) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    for (i = 0; i < l.n; i++)
        if (strcmp(l.a[i].k, ks) == 0) {
            r = JS_TRUE;
            break;
        }
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    return r;
}

static JSValue dyn_sp_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0, i, w = 0;
    int rc = 0;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    /* The spec's delete(name, value) form: with a second argument only pairs
       matching BOTH are removed (it used to be parsed and then ignored). */
    for (i = 0; i < l.n; i++)
        if (strcmp(l.a[i].k, ks) != 0 ||
            (vs && strcmp(l.a[i].v, vs) != 0)) {
            if (w != i) {
                l.a[w].k = l.a[i].k;
                l.a[w].v = l.a[i].v;
            }
            w++;
        }
    l.n = w;
    if (dyn_sp_apply(sp, &l) < 0)
        rc = -1;
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    if (vs) JS_FreeCString(ctx, vs);
    return rc ? JS_ThrowOutOfMemory(ctx) : JS_UNDEFINED;
}

static JSValue dyn_sp_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0, i, w = 0;
    int replaced = 0, rc = -1;
    if (!sp)
        return JS_EXCEPTION;
    if (dyn_sp_kv(ctx, argc, argv, &ks, &kn, &vs, &vn) < 0)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    for (i = 0; i < l.n; i++) {
        if (!replaced && strcmp(l.a[i].k, ks) == 0) {
            /* first match: swap in the new value, drop the later ones */
            free(l.a[i].v);
            l.a[i].v = dyn_strndup(vs ? vs : "", vn);
            if (!l.a[i].v)
                goto done;
            replaced = 1;
            w++;
        } else if (strcmp(l.a[i].k, ks) != 0) {
            if (w != i) {
                l.a[w].k = l.a[i].k;
                l.a[w].v = l.a[i].v;
            }
            w++;
        }
    }
    if (!replaced) {
        if (dyn_sp_list_put(&l, ks, kn, vs ? vs : "", vn) < 0)
            goto done;
        w++;
    }
    l.n = w;
    if (dyn_sp_apply(sp, &l) == 0)
        rc = 0;
 done:
    dyn_sp_list_free(&l);
    JS_FreeCString(ctx, ks);
    if (vs) JS_FreeCString(ctx, vs);
    return rc ? JS_ThrowOutOfMemory(ctx) : JS_UNDEFINED;
}

/* qsort is UNSTABLE, and equal (name, value) pairs give the comparator
   nothing to break a tie with, so sort() used to REORDER duplicates --
   observable, since the serialized query is the object's state. Sorting
   through a keyed view whose comparator falls back to the ORIGINAL index
   makes the order stable by construction. */
typedef struct { dyn_sp_pair_t p; size_t idx; } dyn_sp_sort_ent_t;

static int dyn_sp_cmp(const void *a, const void *b)
{
    const dyn_sp_sort_ent_t *x = (const dyn_sp_sort_ent_t *)a;
    const dyn_sp_sort_ent_t *y = (const dyn_sp_sort_ent_t *)b;
    int c = strcmp(x->p.k, y->p.k);
    if (c != 0)
        return c;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

static JSValue dyn_sp_sort(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    dyn_sp_sort_ent_t *ent;
    size_t i;
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    ent = (dyn_sp_sort_ent_t *)malloc((l.n ? l.n : 1) * sizeof(*ent));
    if (!ent) {
        dyn_sp_list_free(&l);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < l.n; i++) { ent[i].p = l.a[i]; ent[i].idx = i; }
    qsort(ent, l.n, sizeof(ent[0]), dyn_sp_cmp);
    for (i = 0; i < l.n; i++)
        l.a[i] = ent[i].p;      /* ent holds copies; the pairs move wholesale */
    free(ent);
    if (dyn_sp_apply(sp, &l) < 0) {
        dyn_sp_list_free(&l);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_sp_list_free(&l);
    return JS_UNDEFINED;
}

static JSValue dyn_sp_tostring(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    dyn_sb_t out;
    JSValue r;
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    dyn_sb_init(&out);
    if (dyn_sp_serialize(&l, &out) < 0) {
        dyn_sp_list_free(&l);
        dyn_sb_free(&out);
        return JS_ThrowOutOfMemory(ctx);
    }
    dyn_sp_list_free(&l);
    r = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
    dyn_sb_free(&out);
    return r;
}

static JSValue dyn_sp_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    JSValue r;
    (void)magic;
    if (!sp)
        return JS_EXCEPTION;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    r = JS_NewInt32(ctx, (int32_t)l.n);
    dyn_sp_list_free(&l);
    return r;
}

/* Shared builder for keys()/values()/entries(): mode 0 keys, 1 values,
   2 entries. Each returns an ARRAY; see the class comment. */
static JSValue dyn_sp_seq(JSContext *ctx, dyn_sp_t *sp, int mode)
{
    dyn_sp_list_t l;
    size_t i;
    uint32_t n = 0;
    JSValue arr;
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_sp_list_free(&l);
        return JS_EXCEPTION;
    }
    for (i = 0; i < l.n; i++) {
        JSValue v;
        if (mode == 1) {
            v = JS_NewString(ctx, l.a[i].v);
        } else if (mode == 2) {
            JSValue p = JS_NewArray(ctx);
            if (JS_IsException(p)) {
                JS_FreeValue(ctx, arr);
                dyn_sp_list_free(&l);
                return JS_EXCEPTION;
            }
            JS_SetPropertyUint32(ctx, p, 0, JS_NewString(ctx, l.a[i].k));
            JS_SetPropertyUint32(ctx, p, 1, JS_NewString(ctx, l.a[i].v));
            v = p;
        } else {
            v = JS_NewString(ctx, l.a[i].k);
        }
        JS_SetPropertyUint32(ctx, arr, n++, v);
    }
    dyn_sp_list_free(&l);
    return arr;
}

static JSValue dyn_sp_keys(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 0);
}

static JSValue dyn_sp_values(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 1);
}

/* Wrap an array in the runtime's own array iterator (arr[Symbol.iterator]()),
   so spread / for-of / Array.from get a real .next method. */
static JSValue dyn_sp_arr_iter(JSContext *ctx, JSValue arr)
{
    JSValue g, sym, key, meth, iter;
    g = JS_GetGlobalObject(ctx);
    sym = JS_GetPropertyStr(ctx, g, "Symbol");
    key = JS_GetPropertyStr(ctx, sym, "iterator");
    meth = JS_GetProperty(ctx, arr, JS_ValueToAtom(ctx, key));
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, g);
    if (JS_IsException(meth)) {
        JS_FreeValue(ctx, arr);
        return meth;
    }
    iter = JS_Call(ctx, meth, arr, 0, NULL);
    JS_FreeValue(ctx, meth);
    JS_FreeValue(ctx, arr);
    return iter;
}

static JSValue dyn_sp_symbol_iterator(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    JSValue arr;
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    arr = dyn_sp_seq(ctx, sp, 2);
    if (JS_IsException(arr))
        return arr;
    return dyn_sp_arr_iter(ctx, arr);
}

static JSValue dyn_sp_entries(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    (void)argc; (void)argv;
    if (!sp)
        return JS_EXCEPTION;
    return dyn_sp_seq(ctx, sp, 2);
}

static JSValue dyn_sp_for_each(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_sp_t *sp = dyn_sp_of(ctx, this_val);
    dyn_sp_list_t l;
    size_t i;
    if (!sp)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        JS_ThrowTypeError(ctx, "forEach(callback[, thisArg]): callback must be a function");
        return JS_EXCEPTION;
    }
    dyn_sp_list_init(&l);
    dyn_sp_parse(dyn_sp_query_str(sp), strlen(dyn_sp_query_str(sp)), &l);
    for (i = 0; i < l.n; i++) {
        JSValue args[3], r;
        args[0] = JS_NewString(ctx, l.a[i].v);
        args[1] = JS_NewString(ctx, l.a[i].k);
        args[2] = JS_DupValue(ctx, this_val);
        r = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        if (JS_IsException(r)) {
            dyn_sp_list_free(&l);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, r);
    }
    dyn_sp_list_free(&l);
    return JS_UNDEFINED;
}

/* Serialize one (k, v) pair into out; *first toggles the '&' separator.
   Values are coerced with ToString per WHATWG. Returns 0, or -1 with a
   pending exception. `v` is always freed here. */
static int dyn_sp_enc_pair(JSContext *ctx, dyn_sb_t *out, int *first,
                           JSValue k, JSValue v)
{
    const char *ks = NULL, *vs = NULL;
    size_t kn = 0, vn = 0;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        v = JS_NewString(ctx, "");
    } else if (!JS_IsString(v)) {
        JSValue nv = JS_ToString(ctx, v);
        JS_FreeValue(ctx, v);
        v = nv;
    }
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    if (!JS_IsString(k)) {
        JSValue nk = JS_ToString(ctx, k);
        JS_FreeValue(ctx, k);
        k = nk;
    }
    if (JS_IsException(k)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    ks = JS_ToCStringLen(ctx, &kn, k);
    vs = JS_ToCStringLen(ctx, &vn, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, k);
    if (!ks || !vs) {
        if (ks) JS_FreeCString(ctx, ks);
        if (vs) JS_FreeCString(ctx, vs);
        return -1;
    }
    if (!*first)
        dyn_sb_putc(out, '&');
    dyn_pct_encode(out, ks, kn, 2);
    dyn_sb_putc(out, '=');
    dyn_pct_encode(out, vs, vn, 2);
    JS_FreeCString(ctx, ks);
    JS_FreeCString(ctx, vs);
    if (out->oom)
        return -1;
    *first = 0;
    return 0;
}

static JSValue dyn_sp_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    dyn_sp_t *sp;
    (void)new_target;
    if (argc > 0 && (JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])))
        argc = 0;
    sp = (dyn_sp_t *)calloc(1, sizeof(*sp));
    if (!sp)
        return JS_ThrowOutOfMemory(ctx);
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s, *owned;
        size_t n;
        owned = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!owned) {
            free(sp);
            return JS_EXCEPTION;
        }
        s = owned;
        if (n && s[0] == '?') { s++; n--; } /* leading '?' is not part of the list */
        sp->query = dyn_strndup(s, n);
        JS_FreeCString(ctx, owned);
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    } else if (argc > 0 && dyn_plain_get(ctx, argv[0], dyn_sp_class_id) != NULL) {
        dyn_sp_t *src = (dyn_sp_t *)dyn_plain_get(ctx, argv[0], dyn_sp_class_id);
        const char *q = dyn_sp_query_str(src);
        sp->query = dyn_strndup(q, strlen(q));
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    } else if (argc > 0 && JS_IsObject(argv[0])) {
        dyn_sb_t out;
        int first = 1;
        dyn_sb_init(&out);
        if (JS_IsArray(ctx, argv[0])) {
            JSValue lenv = JS_GetPropertyStr(ctx, argv[0], "length");
            uint32_t len = 0, i;
            JS_ToUint32(ctx, &len, lenv);
            JS_FreeValue(ctx, lenv);
            if (len > DYN_SP_MAX_PAIRS) {
                dyn_sb_free(&out);
                free(sp);
                return JS_ThrowRangeError(ctx, "new URLSearchParams(): too many pairs");
            }
            for (i = 0; i < len; i++) {
                JSValue pr = JS_GetPropertyUint32(ctx, argv[0], i);
                JSValue k, v;
                int prlen = 0;
                if (JS_IsException(pr)) {
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (!JS_IsArray(ctx, pr)) { JS_FreeValue(ctx, pr); continue; }
                {   JSValue lv = JS_GetPropertyStr(ctx, pr, "length");
                    JS_ToInt32(ctx, &prlen, lv);
                    JS_FreeValue(ctx, lv); }
                k = JS_GetPropertyUint32(ctx, pr, 0);
                v = prlen >= 2 ? JS_GetPropertyUint32(ctx, pr, 1) : JS_UNDEFINED;
                JS_FreeValue(ctx, pr);
                if (JS_IsException(k) || JS_IsException(v)) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (dyn_sp_enc_pair(ctx, &out, &first, k, v) < 0) {
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
            }
        } else {
            /* plain record: own enumerable string keys, values ToString'd */
            JSPropertyEnum *tab = NULL;
            uint32_t len = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                dyn_sb_free(&out);
                free(sp);
                return JS_EXCEPTION;
            }
            for (i = 0; i < len; i++) {
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, argv[0], tab[i].atom);
                if (JS_IsException(k) || JS_IsException(v)) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                    JS_FreePropertyEnum(ctx, tab, len);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
                if (dyn_sp_enc_pair(ctx, &out, &first, k, v) < 0) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    dyn_sb_free(&out);
                    free(sp);
                    return JS_EXCEPTION;
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
        if (out.oom) {
            dyn_sb_free(&out);
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
        sp->query = dyn_strndup(out.p ? out.p : "", out.n);
        dyn_sb_free(&out);
        if (!sp->query) {
            free(sp);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    return dyn_plain_wrap(ctx, dyn_sp_class_id, sp, dyn_sp_free);
}

static void dyn_sp_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_sp_t *sp = (dyn_sp_t *)JS_GetOpaque(val, dyn_sp_class_id);
    (void)rt;
    if (sp)
        dyn_sp_free(sp);
}

static JSClassDef dyn_sp_class = { "URLSearchParams",
                                   .finalizer = dyn_sp_finalizer };

static const JSCFunctionListEntry dyn_sp_proto[] = {
    JS_CFUNC_DEF("append", 2, dyn_sp_append),
    JS_CFUNC_DEF("delete", 1, dyn_sp_delete),
    JS_CFUNC_DEF("get", 1, dyn_sp_get),
    JS_CFUNC_DEF("getAll", 1, dyn_sp_get_all),
    JS_CFUNC_DEF("has", 1, dyn_sp_has),
    JS_CFUNC_DEF("set", 2, dyn_sp_set),
    JS_CFUNC_DEF("sort", 0, dyn_sp_sort),
    JS_CFUNC_DEF("toString", 0, dyn_sp_tostring),
    JS_CFUNC_DEF("forEach", 1, dyn_sp_for_each),
    JS_CFUNC_DEF("keys", 0, dyn_sp_keys),
    JS_CFUNC_DEF("values", 0, dyn_sp_values),
    JS_CFUNC_DEF("entries", 0, dyn_sp_entries),
    JS_CGETSET_MAGIC_DEF("size", dyn_sp_size, NULL, 0),
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

#include "dyna-idna.inc.c"   /* IDNA 2008 (UTS #46) + punycode, plan 3.3 */

/* Lenient UTS #46 to-ASCII for a special-scheme host (WHATWG "domain to
   ASCII", non-strict): map, NFC, punycode non-ASCII labels with the xn--
   prefix. Unlike the exported domainToASCII, disallowed code points are KEPT
   -- Node accepts `_foo.example` as a URL host while the strict IDNA
   exported function refuses it -- and no DNS-length checks apply. */
static int dyn_url_host_idna(dyn_url_t *u)
{
    const uint8_t *p = (const uint8_t *)u->host;
    const uint8_t *end = p + strlen(u->host);
    dyn_cp_t cps, mapped, out;
    uint32_t *norm = NULL;
    int norm_len;
    size_t i, start;
    int rc = -1;

    dyn_cp_init(&cps);
    dyn_cp_init(&mapped);
    dyn_cp_init(&out);
    while (p < end) {
        const uint8_t *np;
        uint32_t cp = (uint32_t)unicode_from_utf8(p, (int)(end - p), &np);
        if (cp == (uint32_t)-1)
            goto done;          /* invalid UTF-8: refuse the URL */
        if (dyn_cp_put(&cps, cp))
            goto oom;
        p = np;
    }
    for (i = 0; i < cps.n; i++) {
        int ri = dyn_tab3(idna_status_ranges, countof(idna_status_ranges),
                          cps.p[i]);
        /* ToASCII fails on UTS #46 plain-disallowed input (U+FDD0..,
           U+FFFD, C1). disallowed_STD3 (5) stays: the URL host parser maps
           leniently, so _foo.example keeps its underscore. */
        if (ri >= 0 && idna_status_ranges[ri][2] == 3)
            goto done;
        if (dyn_idna_map_cp(cps.p[i], 0, 0, &mapped))   /* non-transitional,
                                                           keep disallowed */
            goto oom;
    }
    norm_len = idna_nfc(mapped.p, (int)mapped.n, &norm);
    if (norm_len < 0)
        goto done;
    if (norm_len == 0)
        goto done;              /* a domain that maps to nothing (soft
                                   hyphens only) is a failure, not "" */
    start = 0;
    for (i = 0; i <= (size_t)norm_len; i++) {
        if (i == (size_t)norm_len || norm[i] == 0x2E) {
            size_t k;
            int has_nonascii = 0;
            for (k = start; k < i; k++)
                if (norm[k] > 0x7F) { has_nonascii = 1; break; }
            if (has_nonascii) {
                if (dyn_cp_put(&out, 'x') || dyn_cp_put(&out, 'n') ||
                    dyn_cp_put(&out, '-') || dyn_cp_put(&out, '-'))
                    goto oom;
                if (pc_encode(norm + start, (int)(i - start), &out) < 0)
                    goto done;
            } else {
                for (k = start; k < i; k++)
                    if (dyn_cp_put(&out, norm[k]))
                        goto oom;
            }
            if (i < (size_t)norm_len)
                if (dyn_cp_put(&out, 0x2E))
                    goto oom;
            start = i + 1;
        }
    }
    {
        /* cp list -> UTF-8 into u->host (at most 4 bytes per code point) */
        char *h = (char *)malloc(out.n * 4 + 1);
        size_t hn = 0, k;
        if (!h)
            goto oom;
        for (k = 0; k < out.n; k++) {
            uint32_t c = out.p[k];
            if (c < 0x80) {
                h[hn++] = (char)c;
            } else if (c < 0x800) {
                h[hn++] = (char)(0xC0 | (c >> 6));
                h[hn++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                h[hn++] = (char)(0xE0 | (c >> 12));
                h[hn++] = (char)(0x80 | ((c >> 6) & 0x3F));
                h[hn++] = (char)(0x80 | (c & 0x3F));
            } else {
                h[hn++] = (char)(0xF0 | (c >> 18));
                h[hn++] = (char)(0x80 | ((c >> 12) & 0x3F));
                h[hn++] = (char)(0x80 | ((c >> 6) & 0x3F));
                h[hn++] = (char)(0x80 | (c & 0x3F));
            }
        }
        h[hn] = 0;
        free(u->host);
        u->host = h;
        rc = 0;
    }
    goto done;
 oom:
    rc = -1;
 done:
    free(norm);
    dyn_cp_free(&cps);
    dyn_cp_free(&mapped);
    dyn_cp_free(&out);
    return rc;
}

/* --------------------------------------------------- WHATWG IP-literal hosts
 * A special-scheme host that "ends in a number" is an IPv4 ADDRESS, not a
 * name: http://0x7f.1 and http://2130706433 are 127.0.0.1, and a malformed
 * almost-address (999.1.1.1) is a parse failure. Before this, such hosts were
 * accepted verbatim, so a host allowlist checked against the parsed URL could
 * disagree with the resolver (the SSRF-gate divergence the URL audit flags
 * as D4). Bracketed literals were likewise accepted unvalidated: http://[zz]/
 * kept the hostname [zz] (D5). */

/* Parse one IPv4 number: "0x"/"0X" hex, a leading-0 octal, else decimal.
   Returns 0 with *out set, or -1 on a syntax failure ("09": a 9 is not an
   octal digit). A value past 32 bits saturates at 2^32, which fails every
   consumer's bound (parts outside the last must be <= 255, and the last
   cannot carry 256^(5 - np)), matching the spec's unbounded arithmetic. */
static int dyn_ipv4_number(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    int base = 10;
    size_t i = 0;

    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (n >= 2 && s[0] == '0') {
        base = 8;
        i = 1;
    }
    if (i == n) {
        if (n == 0)
            return -1;            /* an empty part is a failure, not zero */
        *out = 0;                 /* a bare "0x" prefix is the number 0 */
        return 0;
    }
    for (; i < n; i++) {
        int d = dyn_hexval((unsigned char)s[i]);
        if (d < 0 || d >= base)
            return -1;
        if (v <= 0xFFFFFFFFull)   /* already past 2^32: keep scanning for
                                     syntax errors, ignore the magnitude */
            v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = v > 0xFFFFFFFFull ? 0x100000000ull : v;
    return 0;
}

/* WHATWG "ends in a number" checker: the host's last dot-delimited part is
   all decimal digits or parses as an IPv4 number (hex and octal included). */
static int dyn_host_ends_in_number(const char *h)
{
    size_t len = strlen(h), b, e = len, i;
    int digits = 1;
    uint64_t v;

    if (e && h[e - 1] == '.')     /* one trailing empty part is ignored */
        e--;
    if (e == 0)
        return 0;
    b = e;
    while (b && h[b - 1] != '.')
        b--;
    for (i = b; i < e; i++)       /* an EMPTY part is not "digits": "http://.."
                                     stays a name, like the spec checker */
        if (h[i] < '0' || h[i] > '9') { digits = 0; break; }
    if (digits && b < e)
        return 1;
    return dyn_ipv4_number(h + b, e - b, &v) == 0;
}

/* WHATWG IPv4 parser: 1-4 dot-separated parts of mixed radix, place-valued
   with the last part carrying the remainder, canonicalized to four decimal
   octets. A malformed host is a parse failure, per spec. */
static int dyn_host_ipv4_rewrite(dyn_url_t *u)
{
    const char *h = u->host;
    size_t len = strlen(h), end = len, b = 0;
    uint64_t nums[4];
    uint64_t v;
    int np = 0, i;
    char out[16];

    if (end && h[end - 1] == '.')   /* IPv4-empty-part: a validation error,
                                       not a failure -- drop it */
        end--;
    while (b <= end) {
        size_t e = b;
        while (e < end && h[e] != '.')
            e++;
        if (np == 4)
            return -1;                /* IPv4-too-many-parts */
        if (dyn_ipv4_number(h + b, e - b, &nums[np]) < 0)
            return -1;                /* IPv4-non-numeric-part */
        np++;
        b = e + 1;
    }
    for (i = 0; i < np - 1; i++)
        if (nums[i] > 255)
            return -1;                /* IPv4-out-of-range-part */
    if ((uint64_t)nums[np - 1] >= ((uint64_t)1 << (8 * (5 - np))))
        return -1;                    /* the last part cannot carry this much */
    v = nums[np - 1];
    for (i = 0; i < np - 1; i++)
        v += (uint64_t)nums[i] << (8 * (3 - i));
    snprintf(out, sizeof out, "%u.%u.%u.%u",
             (unsigned)((v >> 24) & 0xFF), (unsigned)((v >> 16) & 0xFF),
             (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
    {
        char *nh = dyn_strndup(out, strlen(out));
        if (!nh)
            return -1;
        free(u->host);
        u->host = nh;
    }
    return 0;
}

/* WHATWG IPv6 parser over the bracket-free literal: 1-8 hex groups, one "::"
   compression zone, an optional trailing embedded IPv4 (no leading zeros).
   Fills piece[0..7]; returns 0, or -1 on any malformation. */
static int dyn_ipv6_parse(const char *s, size_t n, uint16_t piece[8])
{
    size_t p = 0;
    int np = 0, compress = -1;

    memset(piece, 0, 8 * sizeof *piece);
    if (n && s[0] == ':') {
        if (n < 2 || s[1] != ':')
            return -1;               /* compression that is not "::" */
        p = 2;
        compress = ++np;             /* piece 0 stays zero */
    }
    while (p < n) {
        uint32_t v = 0;
        int len = 0;
        if (np == 8)
            return -1;               /* IPv6-too-many-pieces */
        if (s[p] == ':') {
            if (compress >= 0)
                return -1;           /* IPv6-multiple-compression */
            compress = ++np;
            p++;
            /* this ':' is the SECOND of a "::" (the group separator took
               the first), so a trailing "[1:0::]" ends right here -- the
               compressed tail simply has no pieces left */
            continue;
        }
        while (p < n && len < 4) {
            int d = dyn_hexval((unsigned char)s[p]);
            if (d < 0)
                break;
            v = v * 16 + (uint32_t)d;
            p++;
            len++;
        }
        if (p < n && s[p] == '.') {  /* embedded IPv4 tail */
            int seen = 0;
            if (len == 0)
                return -1;
            p -= (size_t)len;        /* re-read the digits as DECIMAL */
            if (np > 6)
                return -1;           /* the tail needs the last two pieces */
            while (p < n) {
                uint32_t num = 0;
                int nd = 0;
                if (seen > 0) {
                    if (s[p] == '.' && seen < 4)
                        p++;
                    else
                        return -1;   /* IPv4-in-IPv6-too-many-pieces */
                }
                if (p >= n || s[p] < '0' || s[p] > '9')
                    return -1;
                while (p < n && s[p] >= '0' && s[p] <= '9') {
                    if (nd == 0)
                        num = (uint32_t)(s[p] - '0');
                    else {
                        if (num == 0)
                            return -1;   /* a leading zero: "IPv4 piece is
                                            already 0" per spec */
                        num = num * 10 + (uint32_t)(s[p] - '0');
                    }
                    if (num > 255)
                        return -1;
                    nd++;
                    p++;
                }
                piece[np] = (uint16_t)(piece[np] * 0x100 + num);
                seen++;
                if (seen == 2 || seen == 4)
                    np++;
            }
            if (seen != 4)
                return -1;           /* IPv4-in-IPv6-too-few-parts */
            break;                   /* the tail is necessarily the end */
        }
        if (len == 0)
            return -1;               /* IPv6-invalid-code-point */
        piece[np] = (uint16_t)v;
        np++;
        if (p >= n)
            break;
        if (s[p] != ':')
            return -1;               /* a group must be followed by ':' */
        p++;
        if (p == n)
            return -1;               /* a lone trailing ':' */
    }
    if (compress < 0)
        return np == 8 ? 0 : -1;     /* IPv6-too-few-pieces */
    /* No compression in the input means the tail pieces were written from the
       front; move them where they belong. */
    {
        int swaps = np - compress, pi = 7;
        while (pi != 0 && swaps > 0) {
            uint16_t t = piece[pi];
            piece[pi] = piece[compress + swaps - 1];
            piece[compress + swaps - 1] = t;
            pi--;
            swaps--;
        }
    }
    return 0;
}

/* Validate a bracketed host and rewrite it in canonical WHATWG form:
   lowercase hex, the first-longest zero run (two pieces or more) compressed
   with "::". Returns 0, or -1 when the literal is malformed. */
static int dyn_host_ipv6_rewrite(dyn_url_t *u)
{
    const char *h = u->host;
    size_t n = strlen(h);
    uint16_t piece[8];
    int i, k = 1, ignore0 = 0;
    int found = -1, foundn = 0, longest = -1, longn = 1;
    char out[64];

    if (n < 2 || h[0] != '[' || h[n - 1] != ']')
        return -1;                   /* the authority split guarantees this */
    if (dyn_ipv6_parse(h + 1, n - 2, piece) < 0)
        return -1;
    for (i = 0; i < 8; i++) {
        if (piece[i] != 0) {
            if (foundn > longn) { longest = found; longn = foundn; }
            found = -1;
            foundn = 0;
        } else {
            if (found < 0)
                found = i;
            foundn++;
        }
    }
    if (foundn > longn) { longest = found; longn = foundn; }
    out[0] = '[';
    for (i = 0; i < 8; i++) {
        if (ignore0 && piece[i] == 0)
            continue;
        if (ignore0)
            ignore0 = 0;
        if (longest == i) {
            if (i == 0)
                out[k++] = ':';
            out[k++] = ':';
            ignore0 = 1;
            continue;
        }
        k += snprintf(out + k, (size_t)(sizeof out - k), "%x", piece[i]);
        if (i != 7)
            out[k++] = ':';
    }
    out[k++] = ']';
    {
        char *nh = dyn_strndup(out, (size_t)k);
        if (!nh)
            return -1;
        free(u->host);
        u->host = nh;
    }
    return 0;
}

/* WHATWG forbidden domain code points, checked on the FINAL (post-mapping)
   host spelling: forbidden host code points, '%' and DEL and C0. UTS #46 has
   already case-folded and mapped by now (U+3002 became '.', fullwidth
   digits became ASCII), so only genuinely forbidden characters can appear. */
static int dyn_forbidden_domain_byte(unsigned char c)
{
    if (c <= 0x1F || c == 0x20 || c == 0x7F || c == '%')
        return 1;
    if (c == '#' || c == '/' || c == ':' || c == '<' || c == '>' ||
        c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' ||
        c == '^' || c == '|')
        return 1;
    return 0;
}

/* The domain-host path of the WHATWG host parser, applied to special and
   file hosts before domain-to-ASCII: percent-decode the buffer (a malformed
   escape keeps its '%', which the post-mapping check then rejects) and
   UTF-8 decode (any invalid sequence is a failure). On success u->host is
   REPLACED with the decoded spelling, which is what domain-to-ASCII and the
   "ends in a number" check must see (http://%30%78%63%30%2e... IS
   0xc0.02...). Code-point validation happens after UTS #46 mapping, in
   dyn_host_validate. Returns 0, or -1 on failure/OOM. */
static int dyn_host_decode(dyn_url_t *u)
{
    const char *h = u->host;
    size_t n = strlen(h), i;
    dyn_sb_t dec;
    char *nh;

    dyn_sb_init(&dec);
    for (i = 0; i < n; i++) {
        if (h[i] == '%') {
            int h1 = (i + 1 < n) ? dyn_hexval((unsigned char)h[i + 1]) : -1;
            int h2 = (i + 2 < n) ? dyn_hexval((unsigned char)h[i + 2]) : -1;
            if (h1 >= 0 && h2 >= 0) {
                dyn_sb_putc(&dec, (char)((h1 << 4) | h2));
                i += 2;
                continue;
            }
        }
        dyn_sb_putc(&dec, h[i]);
    }
    if (dec.oom) { dyn_sb_free(&dec); return -1; }
    {
        const uint8_t *p = (const uint8_t *)dec.p;
        const uint8_t *end = (const uint8_t *)(dec.p + dec.n);
        while (p < end) {
            const uint8_t *np;
            uint32_t cp = (uint32_t)unicode_from_utf8(p, (int)(end - p), &np);
            if (cp == (uint32_t)-1) { dyn_sb_free(&dec); return -1; }
            /* decoded ASCII bytes are checked here on the length-bounded
               buffer: an embedded NUL must not truncate a strlen() scan
               (http://hello%00 is a failure) */
            if (cp <= 0x7F && dyn_forbidden_domain_byte((unsigned char)cp)) {
                dyn_sb_free(&dec);
                return -1;
            }
            p = np;
        }
    }
    nh = dyn_strndup(dec.p ? dec.p : "", dec.n);
    dyn_sb_free(&dec);
    if (!nh)
        return -1;
    free(u->host);
    u->host = nh;
    return 0;
}

/* Post-mapping domain validation (URL host parser step 4: "if asciiDomain
   contains a forbidden domain code point, failure") plus the UTS #46
   plain-disallowed check (U+FDD0.., U+FFFD, C1: these SURVIVE the mapping,
   and ToASCII must fail on them). `domain` is the final host spelling;
   length-based so an embedded NUL cannot hide. Returns 0, or -1. */
static int dyn_host_validate(const char *h, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t *np;
        uint32_t cp = (uint32_t)unicode_from_utf8((const uint8_t *)h + i,
                                                  (int)(n - i), &np);
        if (cp == (uint32_t)-1)
            return -1;
        if (cp > 0x7F) {
            /* non-ASCII survived mapping only if UTS #46 kept it */
            int ri = dyn_tab3(idna_status_ranges,
                              countof(idna_status_ranges), cp);
            if (ri >= 0 && idna_status_ranges[ri][2] == 3)
                return -1;              /* disallowed: U+FDD0, U+FFFD, C1 */
        } else if (dyn_forbidden_domain_byte((unsigned char)cp)) {
            return -1;
        }
        i = (size_t)(np - (const uint8_t *)h);
    }
    return 0;
}

/* WHATWG host normalization, applied after the authority parse. Special
   schemes: ASCII-lowercase, then IDNA (see dyn_url_host_idna). Non-special
   schemes: kept verbatim except non-ASCII bytes percent-encoded (Node:
   foo://münich/ -> m%C3%BCnich, case preserved). Bracketed IPv6 literals are
   validated and canonicalized for EVERY scheme; a special-scheme domain that
   ends in a number is IPv4-parsed and canonicalized. Returns 0, or -1 (caller
   rejects the URL). */
static int dyn_url_normalize_host(dyn_url_t *u)
{
    const char *h = u->host, *p;
    int nonascii = 0;
    int is_file = u->scheme && !strcmp(u->scheme, "file");
    if (!h)
        return 0;
    if (h[0] == '[')          /* IPv6 literal: validate + canonicalize */
        return dyn_host_ipv6_rewrite(u);
    /* special schemes and file are DOMAIN hosts: decode, then validate */
    if (is_file && ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z')) {
        size_t hl = strlen(u->host);
        if (hl == 2 && u->host[1] != '\0' &&
            (u->host[1] == '|' || u->host[1] == ':')) {
            /* case kept: file://D|/ is file:///D:/ */
            char d[4];
            d[0] = '/';
            d[1] = u->host[0];
            d[2] = ':';
            d[3] = '\0';
            free(u->drive);
            u->drive = dyn_strndup(d, 3);
            free(u->host);
            u->host = dyn_strndup("", 0);
            if (!u->drive)
                return -1;
            return 0;
        }
    }
    if (dyn_scheme_is_special(u->scheme) && dyn_host_decode(u) < 0)
        return -1;
    h = u->host;
    for (p = h; *p; p++)
        if ((unsigned char)*p > 0x7F || (unsigned char)*p < 0x20) {
            nonascii = 1; break;
        }
    if (!dyn_scheme_is_special(u->scheme)) {
        if (nonascii) {
            /* opaque host: percent-encode the bytes outside the URL code
               range (C0 controls and non-ASCII), keep case */
            dyn_sb_t out;
            char *nh;
            dyn_sb_init(&out);
            for (p = h; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (c >= 0x7F || c < 0x20) {
                    /* the host percent-encode set: C0 controls, DEL and
                       non-ASCII (sc://%01..%7F) */
                    static const char HEX[] = "0123456789ABCDEF";
                    dyn_sb_putc(&out, '%');
                    dyn_sb_putc(&out, HEX[c >> 4]);
                    dyn_sb_putc(&out, HEX[c & 0xF]);
                } else {
                    dyn_sb_putc(&out, (char)c);
                }
            }
            if (out.oom) {
                dyn_sb_free(&out);
                return -1;
            }
            nh = dyn_strndup(out.p ? out.p : "", out.n);
            dyn_sb_free(&out);
            if (!nh)
                return -1;
            free(u->host);
            u->host = nh;
        }
        return 0;               /* opaque hosts keep their case */
    }
    if (is_file) {
        /* a Windows drive that landed in the authority -- file://C|/ is
           file:///C:/ -- moves to the path's front; it is exempt from the
           domain checks (the '|' is a drive spelling, not a forbidden pipe).
           The drive letter is case-insensitive and its case is kept. */
        {
            size_t hl = strlen(u->host);
            if (hl == 2 &&
                ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z') &&
                (u->host[1] == '|' || u->host[1] == ':')) {
                char d[4];
                d[0] = '/';
                d[1] = u->host[0];
                d[2] = ':';
                d[3] = '\0';
                free(u->drive);
                u->drive = dyn_strndup(d, 3);
                free(u->host);
                u->host = dyn_strndup("", 0);
                if (!u->drive)
                    return -1;
                return 0;
            }
        }
        /* a file host is a domain but never an IPv4 address, and the empty
           host of file:/// is legal; UTS #46 mapping applies (a soft hyphen
           is ignored, so file://a<SHY>b/p -> file://ab/p) */
        if (nonascii && dyn_url_host_idna(u) < 0)
            return -1;
        else
            dyn_lower(u->host);
        if (!strcmp(u->host, "localhost"))
            u->host[0] = '\0';     /* file://localhost IS file:/// */
        return dyn_host_validate(u->host, strlen(u->host));
    }
    if (!nonascii) {
        dyn_lower(u->host);     /* special scheme: ASCII case-fold */
    } else if (dyn_url_host_idna(u) < 0) {
        return -1;              /* the IDNA output is ASCII either way */
    }
    /* The spec checks "ends in a number" AFTER domain-to-ASCII, so a name
       whose punycode form ends in a numeric label is IPv4-parsed too (and
       then fails, since punycode is not a number). */
    if (u->host[0] && dyn_host_validate(u->host, strlen(u->host)) < 0)
        return -1;
    if (u->host[0] && dyn_host_ends_in_number(u->host))
        return dyn_host_ipv4_rewrite(u);
    return 0;
}

static const JSCFunctionListEntry dyn_url_funcs[] = {
    JS_CFUNC_DEF("formEncode", 1, dyn_form_encode),
    JS_CFUNC_DEF("formDecode", 1, dyn_form_decode),
    JS_CFUNC_DEF("encodeURIComponentStrict", 1, dyn_enc_strict),
    JS_CFUNC_DEF("domainToASCII", 1, dyn_domain_to_ascii),
    JS_CFUNC_DEF("domainToUnicode", 1, dyn_domain_to_unicode),
    JS_CFUNC_DEF("punycodeEncode", 1, dyn_punycode_encode),
    JS_CFUNC_DEF("punycodeDecode", 1, dyn_punycode_decode),
};

static int dyn_url_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_url_class_id, &dyn_url_class,
                                 dyn_url_proto, countof(dyn_url_proto),
                                 dyn_url_ctor, "URL") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_sp_class_id, &dyn_sp_class,
                                 dyn_sp_proto, countof(dyn_sp_proto),
                                 dyn_sp_ctor, "URLSearchParams") < 0)
        return -1;
    {
        JSValue proto = JS_GetClassProto(ctx, dyn_sp_class_id);
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue sym = JS_GetPropertyStr(ctx, g, "Symbol");
        JSValue itkey = JS_GetPropertyStr(ctx, sym, "iterator");
        JSAtom katom = JS_ValueToAtom(ctx, itkey);
        JSValue it = JS_NewCFunction(ctx, dyn_sp_symbol_iterator, "entries", 0);
        JS_DefinePropertyValue(ctx, proto, katom, it, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, katom);
        JS_FreeValue(ctx, itkey);
        JS_FreeValue(ctx, sym);
        JS_FreeValue(ctx, g);
        JS_FreeValue(ctx, proto);
    }
    return JS_SetModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

int js_nat_init_url(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:url", dyn_url_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "URL");
    JS_AddModuleExport(ctx, m, "URLSearchParams");
    return JS_AddModuleExportList(ctx, m, dyn_url_funcs, countof(dyn_url_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_URL */
