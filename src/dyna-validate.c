/* dyna:validate -- format validators with a check digit or a real grammar.
   NOT dyna:schema: that name is reserved for the JSON Schema engine, and using
   it here would promise one. Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-codec.h"      /* base64url decode, the JWT shape check */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A validator answers a question about one field, not a document. */
#define DYN_V_MAX 4096

/* Borrow the argument's bytes, or -1 having thrown. */
static int dyn_v_arg(JSContext *ctx, int argc, JSValueConst *argv,
                     const char *fn, const char **s, size_t *n)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "%s(text): argument must be a string", fn);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, argv[0]);
    if (!*s)
        return -1;
    if (*n > DYN_V_MAX) {
        JS_FreeCString(ctx, *s);
        JS_ThrowRangeError(ctx, "%s(text): input exceeds %d bytes", fn, DYN_V_MAX);
        return -1;
    }
    return 0;
}

static int dyn_v_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int dyn_v_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* --------------------------------------------------------------- e-mail */

/* RFC 5322 atext, as a TABLE. It was `strchr("!#$...", c)`, and strchr(set, 0)
   returns a pointer to the set's own terminator -- so an embedded NUL passed
   validation and would truncate in any C consumer downstream. */
static const uint8_t DYN_V_ATEXT[256] = {
    ['!']=1,['#']=1,['$']=1,['%']=1,['&']=1,['\'']=1,['*']=1,['+']=1,['-']=1,
    ['/']=1,['=']=1,['?']=1,['^']=1,['_']=1,['`']=1,['{']=1,['|']=1,['}']=1,
    ['~']=1,
};

/* The practical grammar, not RFC 5322's: one unquoted local part, one dotted
   domain with a letters-only TLD. RFC 5322 accepts comments, quoted strings and
   nested folding that no mail system round-trips, so matching it exactly would
   accept addresses that bounce. */
static int dyn_v_email(const char *s, size_t n)
{
    size_t at = (size_t)-1, i, dot = (size_t)-1, dn;
    if (n < 3 || n > 254)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] == '@') {
            if (at != (size_t)-1)
                return 0;                     /* a second @ is not an address */
            at = i;
        }
    if (at == (size_t)-1 || at == 0 || at + 1 >= n)
        return 0;
    if (at > 64)
        return 0;                             /* local part limit, RFC 5321 */
    if (s[0] == '.' || s[at - 1] == '.')
        return 0;
    for (i = 0; i < at; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i + 1 < at && s[i + 1] == '.')
                return 0;                     /* no consecutive dots */
            continue;
        }
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && !DYN_V_ATEXT[c])
            return 0;
    }
    dn = n - at - 1;
    if (dn < 3 || dn > 253)
        return 0;
    if (s[at + 1] == '.' || s[at + 1] == '-' || s[n - 1] == '.' || s[n - 1] == '-')
        return 0;
    for (i = at + 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i + 1 < n && s[i + 1] == '.')
                return 0;
            if (i > at + 1 && s[i - 1] == '-')
                return 0;
            dot = i;
            continue;
        }
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && c != '-')
            return 0;
    }
    if (dot == (size_t)-1 || n - dot - 1 < 2)
        return 0;                             /* a TLD is at least two chars */
    for (i = dot + 1; i < n; i++)
        if (!dyn_v_alpha((unsigned char)s[i]))
            return 0;                         /* and letters only */
    return 1;
}

/* ----------------------------------------------------------------- Luhn */

/* The card-number check digit. It catches a single mistyped digit and most
   transpositions -- it says nothing about whether the card exists. */
static int dyn_v_luhn(const char *s, size_t n)
{
    int sum = 0, alt = 0, digits = 0;
    size_t i = n;
    while (i-- > 0) {
        unsigned char c = (unsigned char)s[i];
        int d;
        if (c == ' ' || c == '-')
            continue;
        if (!dyn_v_digit(c))
            return 0;
        d = c - '0';
        if (alt) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
        alt = !alt;
        digits++;
    }
    if (digits < 12 || digits > 19)
        return 0;
    return (sum % 10) == 0;
}

/* ----------------------------------------------------------------- IBAN */

/* Length per country, from the IBAN registry. An IBAN of the wrong length for
   its country is invalid even when the check digits happen to work out. */
typedef struct { char cc[2]; uint8_t len; } dyn_iban_t;
static const dyn_iban_t DYN_IBAN[] = {
    {{'A','D'},24},{{'A','E'},23},{{'A','L'},28},{{'A','T'},20},{{'A','Z'},28},
    {{'B','A'},20},{{'B','E'},16},{{'B','G'},22},{{'B','H'},22},{{'B','R'},29},
    {{'B','Y'},28},{{'C','H'},21},{{'C','R'},22},{{'C','Y'},28},{{'C','Z'},24},
    {{'D','E'},22},{{'D','K'},18},{{'D','O'},28},{{'E','E'},20},{{'E','G'},29},
    {{'E','S'},24},{{'F','I'},18},{{'F','O'},18},{{'F','R'},27},{{'G','B'},22},
    {{'G','E'},22},{{'G','I'},23},{{'G','L'},18},{{'G','R'},27},{{'G','T'},28},
    {{'H','R'},21},{{'H','U'},28},{{'I','E'},22},{{'I','L'},23},{{'I','S'},26},
    {{'I','T'},27},{{'J','O'},30},{{'K','W'},30},{{'K','Z'},20},{{'L','B'},28},
    {{'L','C'},32},{{'L','I'},21},{{'L','T'},20},{{'L','U'},20},{{'L','V'},21},
    {{'M','C'},27},{{'M','D'},24},{{'M','E'},22},{{'M','K'},19},{{'M','R'},27},
    {{'M','T'},31},{{'M','U'},30},{{'N','L'},18},{{'N','O'},15},{{'P','K'},24},
    {{'P','L'},28},{{'P','S'},29},{{'P','T'},25},{{'Q','A'},29},{{'R','O'},24},
    {{'R','S'},22},{{'S','A'},24},{{'S','E'},24},{{'S','I'},19},{{'S','K'},24},
    {{'S','M'},27},{{'T','N'},24},{{'T','R'},26},{{'U','A'},29},{{'V','G'},24},
    {{'X','K'},20},
};

/* mod-97 over the rearranged digits, folded incrementally: the expanded number
   is up to 70 digits and does not fit any integer type. */
static int dyn_v_iban(const char *s, size_t n)
{
    char buf[64];
    size_t k = 0, i;
    unsigned rem = 0;
    int len = 0;

    for (i = 0; i < n; i++) {                 /* strip the spaces banks print */
        unsigned char c = (unsigned char)s[i];
        if (c == ' ')
            continue;
        if (k >= sizeof buf)
            return 0;
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        buf[k++] = (char)c;
    }
    if (k < 15 || k > 34)
        return 0;
    if (!dyn_v_alpha((unsigned char)buf[0]) || !dyn_v_alpha((unsigned char)buf[1]))
        return 0;
    if (!dyn_v_digit((unsigned char)buf[2]) || !dyn_v_digit((unsigned char)buf[3]))
        return 0;
    for (i = 0; i < countof(DYN_IBAN); i++)
        if (DYN_IBAN[i].cc[0] == buf[0] && DYN_IBAN[i].cc[1] == buf[1]) {
            len = DYN_IBAN[i].len;
            break;
        }
    if (!len || (size_t)len != k)
        return 0;                             /* unknown country, or wrong length */
    for (i = 4; i < k; i++)
        if (!dyn_v_alpha((unsigned char)buf[i]) && !dyn_v_digit((unsigned char)buf[i]))
            return 0;
    /* the first four characters move to the end, letters become 10..35 */
    for (i = 0; i < k; i++) {
        unsigned char c = (unsigned char)buf[(i + 4) % k];
        if (dyn_v_digit(c)) {
            rem = (rem * 10 + (unsigned)(c - '0')) % 97;
        } else {
            unsigned v = (unsigned)(c - 'A') + 10;
            rem = (rem * 100 + v) % 97;
        }
    }
    return rem == 1;
}

/* ------------------------------------------------------------ char classes */

enum { V_ALPHA, V_ALNUM, V_ASCII, V_EMAIL, V_LUHN, V_IBAN };

static JSValue dyn_v_check(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    static const char *const NAMES[] = {
        "IsAlpha", "IsAlphanumeric", "IsAscii", "IsEmail", "IsCreditCard", "IsIBAN"
    };
    const char *s;
    size_t n, i;
    int ok = 1;

    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case V_EMAIL: ok = dyn_v_email(s, n); break;
    case V_LUHN:  ok = dyn_v_luhn(s, n);  break;
    case V_IBAN:  ok = dyn_v_iban(s, n);  break;
    default:
        /* An empty string satisfies no class: "is it all letters" over nothing
           is a question with no useful yes. */
        if (n == 0) { ok = 0; break; }
        for (i = 0; i < n && ok; i++) {
            unsigned char c = (unsigned char)s[i];
            if (magic == V_ALPHA)      ok = dyn_v_alpha(c);
            else if (magic == V_ALNUM) ok = dyn_v_alpha(c) || dyn_v_digit(c);
            else                       ok = c < 0x80;
        }
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* ------------------------------------------------------- module bridge */

/* The parsers behind IsURL/IsDomain/IsUUID/IsSemver are static inside their
   own modules, so the module's JS exports are the entry points: a one-import
   bridge module hands the export over. The cache lives in a non-enumerable
   holder ON globalThis -- never in a C static: a static JSValue is a leaked
   refcount at JS_FreeRuntime (the gc_obj_list assert), and a global keyed on
   nothing breaks worker contexts. These validators REQUIRE dyna:url/net/uuid/
   semver to be registered; a build without one throws the bridge's named
   error on first use. */

#define DYN_V_HOLDER "__dyna_vx"

/* Evaluate `body` as a module; it must store its result on
   globalThis.__dyna_v. Returns the stored value. */
static JSValue dyn_v_bridge(JSContext *ctx, const char *body)
{
    JSValue r, v, g;

    r = JS_Eval(ctx, body, strlen(body), "<dyna:validate>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        return r;
    v = JS_EvalFunction(ctx, r);       /* consumes r; runs the import graph */
    if (JS_IsException(v))
        return v;
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_DupValue(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        return JS_Throw(ctx, err);      /* a module we depend on is missing */
    }
    JS_FreeValue(ctx, v);
    g = JS_GetGlobalObject(ctx);
    v = JS_GetPropertyStr(ctx, g, "__dyna_v");
    JS_FreeValue(ctx, g);
    return v;
}

/* Read the cached export for `slotname` from the global holder, or
   JS_UNDEFINED (missing holder or missing slot). */
static JSValue dyn_v_slot_get(JSContext *ctx, const char *slotname)
{
    JSValue g, holder, v;
    g = JS_GetGlobalObject(ctx);
    holder = JS_GetPropertyStr(ctx, g, DYN_V_HOLDER);
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(holder)) {
        JS_FreeValue(ctx, holder);
        return JS_UNDEFINED;
    }
    v = JS_GetPropertyStr(ctx, holder, slotname);
    JS_FreeValue(ctx, holder);
    return v;
}

/* Fetch `fnname` from `module`, bridging on first use. The cache entry is a
   property of the global holder KEYED BY "module.fnname" (bare fnname
   collides: isValid exists in both dyna:net and dyna:semver), so
   JS_FreeContext releases the refcount. */
static JSValue dyn_v_export(JSContext *ctx, const char *module,
                            const char *fnname)
{
    char src[192], key[96];
    JSValue v;

    snprintf(key, sizeof key, "%s.%s", module, fnname);
    v = dyn_v_slot_get(ctx, key);
    if (JS_IsFunction(ctx, v))
        return v;
    JS_FreeValue(ctx, v);
    snprintf(src, sizeof src,
             "import * as m from \"%s\";\n"
             "globalThis." DYN_V_HOLDER " = globalThis." DYN_V_HOLDER " || {};\n"
             "globalThis." DYN_V_HOLDER "[\"%s\"] = m.%s;\n"
             "globalThis.__dyna_v = m.%s;",
             module, key, fnname, fnname);
    v = dyn_v_bridge(ctx, src);
    if (JS_IsException(v))
        return v;
    if (!JS_IsFunction(ctx, v)) {
        JSValue e = v;
        v = JS_ThrowTypeError(ctx, "dyna:validate: %s has no export %s",
                              module, fnname);
        JS_FreeValue(ctx, e);
        return v;
    }
    return v;
}

/* Call a module export with one string argument: 1/0 from JS_ToBool, -1 on a
   bridge or allocation failure. The validators below never throw on content,
   so a swallowed exception reads as false. */
static int dyn_v_js_bool(JSContext *ctx, const char *module,
                         const char *fnname, const char *s, size_t n)
{
    JSValue fn, arg, v;
    JSValueConst a1[1];
    int ok;

    fn = dyn_v_export(ctx, module, fnname);
    if (JS_IsException(fn))
        return -1;
    arg = JS_NewStringLen(ctx, s, n);
    if (JS_IsException(arg)) {
        JS_FreeValue(ctx, fn);
        return -1;
    }
    a1[0] = arg;
    v = JS_Call(ctx, fn, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    ok = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return ok < 0 ? 0 : ok;
}

/* ---------------------------------------------------------------- slug */

/* A slug is a URL-label-shaped token: lowercase letters, digits and single
   hyphens. No RFC; the 64-char bound keeps it inside any URL label. */
static int dyn_v_slug(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n > 64)
        return 0;
    if (s[0] == '-')
        return 0;                         /* no leading hyphen */
    if (s[n - 1] == '-')
        return 0;                         /* no trailing hyphen */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z')
            continue;
        if (dyn_v_digit(c))
            continue;
        if (c == '-') {
            if (i + 1 < n && s[i + 1] == '-')
                return 0;                 /* no double hyphens */
            continue;
        }
        return 0;                         /* anything else is not a slug */
    }
    return 1;
}

/* ----------------------------------------------------------------- E.164 */

/* ITU-T E.164 sec.3.1: an optional '+' and at most 15 digits, nothing else. */
static int dyn_v_e164(const char *s, size_t n)
{
    size_t i = 0, digits = 0;

    if (s[0] == '+')
        i = 1;
    if (i == n)
        return 0;                         /* '+' alone is not a number */
    for (; i < n; i++) {
        if (!dyn_v_digit((unsigned char)s[i]))
            return 0;                     /* only digits survive the '+' */
        digits++;
    }
    return digits <= 15;                  /* E.164 maximum length */
}

/* --------------------------------------------------------------- domain */

/* RFC 1035 label grammar over the input, with at least one dot (a domain, not
   a bare host). net.isValid (dyna:net) decides the IP-literal half. */
static int dyn_v_domain_grammar(const char *s, size_t n)
{
    size_t i, lab = 0, dots = 0;

    if (n < 3 || n > 253)
        return 0;                         /* RFC 1035: name <= 253 octets */
    for (i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? (unsigned char)s[i] : (unsigned char)'.';
        if (c == '.') {
            if (lab == 0 || lab > 63)
                return 0;                 /* empty or over-long label */
            if (s[i - lab] == '-' || s[i - 1] == '-')
                return 0;                 /* no hyphen at a label edge */
            if (i < n)
                dots++;
            lab = 0;
        } else if (dyn_v_alpha(c) || dyn_v_digit(c) || c == '-') {
            lab++;
        } else {
            return 0;                     /* not a domain character */
        }
    }
    return dots >= 1;
}

/* ------------------------------------------------------------------ JWT */

/* JWS Compact Serialization (RFC 7515 sec.3.1): three unpadded base64url
   segments. dyna-crypto exposes no decode-without-verify, so the structure is
   checked here with its decoder and the engine's own JSON parser. */
static size_t dyn_v_b64len(size_t n)
{
    size_t r = n % 4;
    return n / 4 * 3 + (r == 2 ? 1 : r == 3 ? 2 : 0);
}

static int dyn_v_jwt(JSContext *ctx, const char *s, size_t n)
{
    size_t d1 = (size_t)-1, d2 = (size_t)-1, i;
    uint8_t hb[3073], pb[3073], sb[3073];
    char hs[4100], ps[4100], ss[4100];
    size_t hl, pl, sl, hlen, plen;
    JSValue h, v;

    for (i = 0; i < n; i++)
        if (s[i] == '.') {
            if (d1 == (size_t)-1)
                d1 = i;
            else if (d2 == (size_t)-1)
                d2 = i;
            else
                return 0;                 /* a third dot: not three segments */
        }
    if (d1 == (size_t)-1 || d2 == (size_t)-1)
        return 0;                         /* need two dots */
    if (d1 == 0 || d2 == d1 + 1 || n - d2 - 1 == 0)
        return 0;                         /* every segment is non-empty */
    hlen = d1;
    plen = d2 - d1 - 1;

    hl = dyn_codec_base64url_decode(s, hlen, hb, hs);
    if (hl == DYN_CODEC_BAD || hl > dyn_v_b64len(hlen))
        return 0;                         /* header must be base64url */
    hb[hl] = 0;
    h = JS_ParseJSON(ctx, (const char *)hb, hl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;                         /* header must be JSON */
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;                         /* header must be a JSON object */
    }
    v = JS_GetPropertyStr(ctx, h, "alg");
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, h);
        return 0;                         /* header must name an alg */
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, h);

    pl = dyn_codec_base64url_decode(s + d1 + 1, plen, pb, ps);
    if (pl == DYN_CODEC_BAD || pl > dyn_v_b64len(plen))
        return 0;                         /* claims must be base64url */
    pb[pl] = 0;
    h = JS_ParseJSON(ctx, (const char *)pb, pl, "<jwt>");
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;                         /* claims must be JSON */
    }
    if (!JS_IsObject(h) || JS_IsArray(ctx, h)) {
        JS_FreeValue(ctx, h);
        return 0;                         /* claims must be a JSON object */
    }
    JS_FreeValue(ctx, h);

    sl = dyn_codec_base64url_decode(s + d2 + 1, n - d2 - 1, sb, ss);
    if (sl == DYN_CODEC_BAD || sl > dyn_v_b64len(n - d2 - 1))
        return 0;                         /* signature must be base64url */
    return 1;
}

/* ----------------------------------------------------------- extensions */

enum { VX_URL, VX_DOMAIN, VX_SLUG, VX_UUID, VX_JWT, VX_SEMVER, VX_E164 };

static JSValue dyn_v_check_ext(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const NAMES[] = {
        "IsURL", "IsDomain", "IsSlug", "IsUUID", "IsJWT", "IsSemver", "IsE164"
    };
    const char *s;
    size_t n;
    int ok = 0;

    (void)this_val;
    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case VX_URL: {
        JSValue fn, arg, u;
        JSValueConst a1[1];
        fn = dyn_v_export(ctx, "dyna:url", "URL");
        if (JS_IsException(fn)) { JS_FreeCString(ctx, s); return fn; }
        arg = JS_NewStringLen(ctx, s, n);
        if (JS_IsException(arg)) {
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, s);
            return arg;
        }
        a1[0] = arg;
        u = JS_CallConstructor(ctx, fn, 1, a1);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(u)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;                        /* the ctor refused it: not a URL */
        }
        ok = 1;
        /* A special scheme with an empty host is the parser's lenient corner
           ("https://"); the validator checks the value the ctor returned. */
        {
            JSValue proto = JS_GetPropertyStr(ctx, u, "protocol");
            JSValue host = JS_GetPropertyStr(ctx, u, "hostname");
            const char *p = JS_IsString(proto) ? JS_ToCString(ctx, proto) : NULL;
            const char *h = JS_IsString(host) ? JS_ToCString(ctx, host) : NULL;
            if (!p || !h) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                ok = 0;
            } else if (h[0] == '\0' &&
                       (!strcmp(p, "http:") || !strcmp(p, "https:") ||
                        !strcmp(p, "ws:") || !strcmp(p, "wss:") ||
                        !strcmp(p, "ftp:"))) {
                ok = 0;
            }
            if (p) JS_FreeCString(ctx, p);
            if (h) JS_FreeCString(ctx, h);
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, host);
        }
        JS_FreeValue(ctx, u);
        break;
    }
    case VX_DOMAIN:
        if (dyn_v_domain_grammar(s, n)) {
            int r = dyn_v_js_bool(ctx, "dyna:net", "isValid",
                                  s, n);
            if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            ok = !r;                      /* an IP literal is not a domain */
        }
        break;
    case VX_SLUG:
        ok = dyn_v_slug(s, n);
        break;
    case VX_UUID:
        /* RFC 4122 canonical 8-4-4-4-12 only: 36 chars, then the module's
           parser decides the hex and dash positions. */
        if (n == 36) {
            int r = dyn_v_js_bool(ctx, "dyna:uuid", "validate",
                                  s, n);
            if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
            ok = r;
        }
        break;
    case VX_JWT:
        ok = dyn_v_jwt(ctx, s, n);
        break;
    case VX_SEMVER: {
        int r = dyn_v_js_bool(ctx, "dyna:semver", "isValid",
                              s, n);
        if (r < 0) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
        ok = r;
        break;
    }
    case VX_E164:
        ok = dyn_v_e164(s, n);
        break;
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry dyn_v_funcs_ext[] = {
    JS_CFUNC_MAGIC_DEF("IsURL", 1, dyn_v_check_ext, VX_URL),
    JS_CFUNC_MAGIC_DEF("IsDomain", 1, dyn_v_check_ext, VX_DOMAIN),
    JS_CFUNC_MAGIC_DEF("IsSlug", 1, dyn_v_check_ext, VX_SLUG),
    JS_CFUNC_MAGIC_DEF("IsUUID", 1, dyn_v_check_ext, VX_UUID),
    JS_CFUNC_MAGIC_DEF("IsJWT", 1, dyn_v_check_ext, VX_JWT),
    JS_CFUNC_MAGIC_DEF("IsSemver", 1, dyn_v_check_ext, VX_SEMVER),
    JS_CFUNC_MAGIC_DEF("IsE164", 1, dyn_v_check_ext, VX_E164),
};

static const JSCFunctionListEntry dyn_v_funcs[] = {
    JS_CFUNC_MAGIC_DEF("IsAlpha", 1, dyn_v_check, V_ALPHA),
    JS_CFUNC_MAGIC_DEF("IsAlphanumeric", 1, dyn_v_check, V_ALNUM),
    JS_CFUNC_MAGIC_DEF("IsAscii", 1, dyn_v_check, V_ASCII),
    JS_CFUNC_MAGIC_DEF("IsEmail", 1, dyn_v_check, V_EMAIL),
    JS_CFUNC_MAGIC_DEF("IsCreditCard", 1, dyn_v_check, V_LUHN),
    JS_CFUNC_MAGIC_DEF("IsIBAN", 1, dyn_v_check, V_IBAN),
};

static int dyn_v_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_v_funcs_ext,
                                  countof(dyn_v_funcs_ext));
}

int js_nat_init_validate(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:validate", dyn_v_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_v_funcs_ext,
                                  countof(dyn_v_funcs_ext));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_VALIDATE */
