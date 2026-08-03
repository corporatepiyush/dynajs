/* dyna:validate -- format validators with a check digit or a real grammar.
   NOT dyna:schema: that name is reserved for the JSON Schema engine, and using
   it here would promise one. Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    return JS_SetModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs));
}

int js_nat_init_validate(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:validate", dyn_v_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_VALIDATE */
