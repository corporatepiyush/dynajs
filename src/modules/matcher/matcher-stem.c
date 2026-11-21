#include "matcher-internal.h"
#include <stdlib.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

typedef struct {
    uint8_t* w;
    size_t n;
    size_t r1;
    size_t r2;
} p2_t;

typedef struct {
    const char* s;
    uint8_t len;
} p2_suf_t;

#define P2S(x) { x, (uint8_t)(sizeof(x) - 1) }

static int p2_v(uint8_t c)
{
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

static size_t p2_cstart(const uint8_t* w, size_t i)
{
    while (i > 0 && (w[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static size_t p2_nchars(const uint8_t* w, size_t a, size_t b)
{
    size_t k = 0;
    for (; a < b; a++)
        if ((w[a] & 0xC0) != 0x80)
            k++;
    return k;
}

static int p2_ends(const p2_t* s, const char* suf, size_t len)
{
    const uint8_t* e;
    size_t k;
    if (s->n < len)
        return 0;
    e = s->w + s->n - len;
    for (k = len; k-- > 0;)
        if (e[k] != (uint8_t)suf[k])
            return 0;
    return 1;
}

static const p2_suf_t* p2_longest(const p2_t* s, const p2_suf_t* tab, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p2_ends(s, tab[i].s, tab[i].len))
            return &tab[i];
    return NULL;
}

static void p2_set(p2_t* s, size_t cut, const char* rep)
{
    size_t rl = strlen(rep);
    s->n -= cut;
    memcpy(s->w + s->n, rep, rl);
    s->n += rl;
}

static int p2_has_vowel(const p2_t* s, size_t end)
{
    size_t i;
    for (i = 0; i < end; i++)
        if (p2_v(s->w[i]))
            return 1;
    return 0;
}

static int p2_short_syl(const p2_t* s, size_t end)
{
    const uint8_t* w = s->w;
    size_t c3, c1;
    uint8_t last;
    if (end == 0)
        return 0;
    if (end == 4 && memcmp(w, "past", 4) == 0)
        return 1;
    c3 = p2_cstart(w, end - 1);
    last = w[c3];
    if (p2_v(last))
        return 0;
    if (c3 == 1 && p2_v(w[0]))
        return 1;
    if (last == 'w' || last == 'x' || last == 'Y')
        return 0;
    if (c3 < 2 || !p2_v(w[c3 - 1]))
        return 0;
    c1 = p2_cstart(w, c3 - 2);
    return !p2_v(w[c1]);
}

static int p2_is_short(const p2_t* s)
{
    return s->r1 >= s->n && p2_short_syl(s, s->n);
}

static size_t p2_region(const uint8_t* w, size_t n, size_t from)
{
    size_t i = from;
    int prev_v = 0;
    while (i < n) {
        size_t j = i + 1;
        int v = p2_v(w[i]);
        while (j < n && (w[j] & 0xC0) == 0x80)
            j++;
        if (!v && prev_v)
            return j;
        prev_v = v;
        i = j;
    }
    return n;
}

#define P2E(a, b) { a, (uint8_t)(sizeof(a) - 1), b, (uint8_t)(sizeof(b) - 1) }

static const struct {
    const char* w;
    uint8_t wl;
    const char* stem;
    uint8_t sl;
} p2_ex1[] = {
    P2E("skis", "ski"),
    P2E("skies", "sky"),
    P2E("dying", "die"),
    P2E("lying", "lie"),
    P2E("tying", "tie"),
    P2E("idly", "idl"),
    P2E("gently", "gentl"),
    P2E("ugly", "ugli"),
    P2E("early", "earli"),
    P2E("only", "onli"),
    P2E("singly", "singl"),
    P2E("sky", "sky"),
    P2E("news", "news"),
    P2E("howe", "howe"),
    P2E("atlas", "atlas"),
    P2E("cosmos", "cosmos"),
    P2E("bias", "bias"),
    P2E("andes", "andes"),
};

static const p2_suf_t p2_ex2[] = {
    P2S("inning"),
    P2S("outing"),
    P2S("canning"),
    P2S("herring"),
    P2S("earring"),
    P2S("proceed"),
    P2S("exceed"),
    P2S("succeed"),
    P2S("evening"),
};

static const p2_suf_t p2_pre[] = {
    P2S("gener"),
    P2S("commun"),
    P2S("arsen"),
    P2S("past"),
    P2S("univers"),
    P2S("later"),
    P2S("emerg"),
    P2S("organ"),
    P2S("inter"),
};

static const p2_suf_t p2_s0[] = { P2S("'s'"), P2S("'s"), P2S("'") };
static const p2_suf_t p2_s1a[] = { P2S("sses"), P2S("ied"), P2S("ies"), P2S("us"), P2S("ss"), P2S("s") };
static const p2_suf_t p2_s1b[] = { P2S("eedly"), P2S("ingly"), P2S("edly"), P2S("eed"), P2S("ing"), P2S("ed") };
static const p2_suf_t p2_s1b2[] = {
    P2S("at"),
    P2S("bl"),
    P2S("iz"),
    P2S("bb"),
    P2S("dd"),
    P2S("ff"),
    P2S("gg"),
    P2S("mm"),
    P2S("nn"),
    P2S("pp"),
    P2S("rr"),
    P2S("tt"),
};
static const p2_suf_t p2_s2[] = {
    P2S("ization"),
    P2S("ational"),
    P2S("fulness"),
    P2S("ousness"),
    P2S("iveness"),
    P2S("tional"),
    P2S("biliti"),
    P2S("lessli"),
    P2S("entli"),
    P2S("ation"),
    P2S("alism"),
    P2S("aliti"),
    P2S("ousli"),
    P2S("iviti"),
    P2S("fulli"),
    P2S("ogist"),
    P2S("enci"),
    P2S("anci"),
    P2S("abli"),
    P2S("izer"),
    P2S("ator"),
    P2S("alli"),
    P2S("bli"),
    P2S("ogi"),
    P2S("li"),
};
static const char* const p2_s2_rep[] = {
    "ize",
    "ate",
    "ful",
    "ous",
    "ive",
    "tion",
    "ble",
    "less",
    "ent",
    "ate",
    "al",
    "al",
    "ous",
    "ive",
    "ful",
    "og",
    "ence",
    "ance",
    "able",
    "ize",
    "ate",
    "al",
    "ble",
    "og",
    "",
};
static const p2_suf_t p2_s3[] = {
    P2S("ational"),
    P2S("tional"),
    P2S("alize"),
    P2S("icate"),
    P2S("iciti"),
    P2S("ative"),
    P2S("ical"),
    P2S("ness"),
    P2S("ful"),
};
static const char* const p2_s3_rep[] = { "ate", "tion", "al", "ic", "ic", "", "ic", "", "" };
static const p2_suf_t p2_s4[] = {
    P2S("ement"),
    P2S("ance"),
    P2S("ence"),
    P2S("able"),
    P2S("ible"),
    P2S("ment"),
    P2S("ant"),
    P2S("ent"),
    P2S("ism"),
    P2S("ate"),
    P2S("iti"),
    P2S("ous"),
    P2S("ive"),
    P2S("ize"),
    P2S("ion"),
    P2S("al"),
    P2S("er"),
    P2S("ic"),
};

static void p2_step1b(p2_t* s)
{
    const p2_suf_t* x = p2_longest(s, p2_s1b, countof(p2_s1b));
    size_t start;
    if (!x)
        return;
    start = s->n - x->len;
    if (x->s[0] == 'e' && x->s[1] == 'e') {
        if (start >= s->r1)
            p2_set(s, x->len, "ee");
        return;
    }
    if (x->len == 3 && x->s[0] == 'i' && p2_nchars(s->w, 0, s->n) == 5
        && !p2_v(s->w[0]) && s->w[s->n - 4] == 'y') {
        p2_set(s, 4, "ie");
        return;
    }
    if (!p2_has_vowel(s, start))
        return;
    s->n = start;
    x = p2_longest(s, p2_s1b2, countof(p2_s1b2));
    if (x && x - p2_s1b2 < 3)
        p2_set(s, 0, "e");
    else if (x) {
        if (p2_nchars(s->w, 0, s->n - 2) >= 2)
            s->n--;
    } else if (p2_is_short(s))
        p2_set(s, 0, "e");
}

static void p2_steps(p2_t* s)
{
    const p2_suf_t* x;
    size_t start, i;

    p2_step1b(s);
    if (s->n >= 2 && (s->w[s->n - 1] == 'y' || s->w[s->n - 1] == 'Y')) {
        size_t p = p2_cstart(s->w, s->n - 2);
        if (p > 0 && !p2_v(s->w[p]))
            s->w[s->n - 1] = 'i';
    }
    x = p2_longest(s, p2_s2, countof(p2_s2));
    if (x && (start = s->n - x->len) >= s->r1) {
        i = (size_t)(x - p2_s2);
        if (!strcmp(x->s, "ogi")) {
            if (start > 0 && s->w[start - 1] == 'l')
                p2_set(s, x->len, "og");
        } else if (!strcmp(x->s, "li")) {
            if (start > 0 && strchr("cdeghkmnrt", s->w[start - 1]) && s->w[start - 1])
                s->n = start;
        } else
            p2_set(s, x->len, p2_s2_rep[i]);
    }
    x = p2_longest(s, p2_s3, countof(p2_s3));
    if (x && (start = s->n - x->len) >= s->r1) {
        i = (size_t)(x - p2_s3);
        if (!strcmp(x->s, "ative")) {
            if (start >= s->r2)
                s->n = start;
        } else
            p2_set(s, x->len, p2_s3_rep[i]);
    }
    x = p2_longest(s, p2_s4, countof(p2_s4));
    if (x && (start = s->n - x->len) >= s->r2) {
        if (!strcmp(x->s, "ion")) {
            if (start > 0 && (s->w[start - 1] == 's' || s->w[start - 1] == 't'))
                s->n = start;
        } else
            s->n = start;
    }
    if (s->n >= 1 && s->w[s->n - 1] == 'e') {
        size_t e = s->n - 1;
        if (e >= s->r2 || (e >= s->r1 && !p2_short_syl(s, e)))
            s->n = e;
    } else if (s->n >= 2 && s->w[s->n - 1] == 'l') {
        if (s->n - 1 >= s->r2 && s->w[s->n - 2] == 'l')
            s->n--;
    }
}

size_t dyn_porter2(const uint8_t* in, size_t n, uint8_t* out)
{
    p2_t s;
    const p2_suf_t* x;
    size_t i;

    for (i = 0; i < countof(p2_ex1); i++) {
        if (p2_ex1[i].wl == n && in[0] == (uint8_t)p2_ex1[i].w[0] && memcmp(in, p2_ex1[i].w, n) == 0) {
            memcpy(out, p2_ex1[i].stem, p2_ex1[i].sl);
            return p2_ex1[i].sl;
        }
    }
    memcpy(out, in, n);
    if (p2_nchars(in, 0, n) <= 2)
        return n;
    s.w = out;
    s.n = n;
    if (s.w[0] == '\'') {
        memmove(s.w, s.w + 1, --s.n);
    }
    if (s.n && s.w[0] == 'y')
        s.w[0] = 'Y';
    for (i = 1; i < s.n; i++)
        if (s.w[i] == 'y' && p2_v(s.w[i - 1]))
            s.w[i] = 'Y';
    s.r1 = SIZE_MAX;
    for (i = 0; i < countof(p2_pre); i++) {
        size_t l = p2_pre[i].len;
        if (s.n >= l && s.w[0] == (uint8_t)p2_pre[i].s[0] && memcmp(s.w, p2_pre[i].s, l) == 0) {
            s.r1 = l;
            break;
        }
    }
    if (s.r1 == SIZE_MAX)
        s.r1 = p2_region(s.w, s.n, 0);
    s.r2 = p2_region(s.w, s.n, s.r1);
    x = p2_longest(&s, p2_s0, countof(p2_s0));
    if (x)
        s.n -= x->len;
    x = p2_longest(&s, p2_s1a, countof(p2_s1a));
    if (x) {
        size_t start = s.n - x->len;
        if (!strcmp(x->s, "sses"))
            p2_set(&s, x->len, "ss");
        else if (x->s[0] == 'i')
            p2_set(&s, x->len, p2_nchars(s.w, 0, start) > 1 ? "i" : "ie");
        else if (!strcmp(x->s, "s")) {
            if (start >= 1 && p2_has_vowel(&s, p2_cstart(s.w, start - 1)))
                s.n = start;
        }
    }
    for (i = 0; i < countof(p2_ex2); i++) {
        size_t l = p2_ex2[i].len;
        if (l == s.n && s.w[0] == (uint8_t)p2_ex2[i].s[0] && memcmp(s.w, p2_ex2[i].s, l) == 0)
            goto done;
    }
    p2_steps(&s);
done:
    for (i = 0; i < s.n; i++)
        if (s.w[i] == 'Y')
            s.w[i] = 'y';
    return s.n;
}

JSValue dyn_matcher_stem(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char* in;
    size_t n;
    uint8_t buf[256];
    uint8_t* out = buf;
    JSValue r;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "stem(word: string)");
    in = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!in)
        return JS_EXCEPTION;
    if (n + 1 > sizeof buf) {
        out = (uint8_t*)malloc(n + 1);
        if (!out) {
            JS_FreeCString(ctx, in);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    n = dyn_porter2((const uint8_t*)in, n, out);
    r = JS_NewStringLen(ctx, (const char*)out, n);
    JS_FreeCString(ctx, in);
    if (out != buf)
        free(out);
    return r;
}
#endif
