#include "htmlkit.h"

void hb_write_grow(hb_t* b, const void* p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        uint8_t* np;
        while (nc < b->n + n) {
            if (nc < (1u << 20))
                nc *= 2;
            else
                nc += nc / 4;
        }
        np = (uint8_t*)realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

void hb_reserve(hb_t* b, size_t need)
{
    if (b->oom || need <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc < (1u << 20))
            nc *= 2;
        else
            nc += nc / 4;
    }
    uint8_t* np = (uint8_t*)realloc(b->p, nc);
    if (!np) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = nc;
}

static void hb_putc(hb_t* b, uint32_t cp)
{
    uint8_t t[4];
    hb_write(b, t, (size_t)dyn_utf8_encode(cp, t));
}

const char* const HT_VOID[] = {
    "area",
    "base",
    "br",
    "col",
    "embed",
    "hr",
    "img",
    "input",
    "link",
    "meta",
    "param",
    "source",
    "track",
    "wbr",
};

const char* const HT_RAW[] = { "script", "style", "textarea", "title" };

static const struct {
    const char* name;
    uint32_t cp;
} HT_ENT[] = {
    { "amp", 38 },
    { "lt", 60 },
    { "gt", 62 },
    { "quot", 34 },
    { "apos", 39 },
    { "nbsp", 160 },
    { "copy", 169 },
    { "reg", 174 },
    { "trade", 8482 },
    { "hellip", 8230 },
    { "mdash", 8212 },
    { "ndash", 8211 },
    { "lsquo", 8216 },
    { "rsquo", 8217 },
    { "ldquo", 8220 },
    { "rdquo", 8221 },
    { "bull", 8226 },
    { "dagger", 8224 },
    { "permil", 8240 },
    { "lsaquo", 8249 },
    { "rsaquo", 8250 },
    { "euro", 8364 },
    { "pound", 163 },
    { "yen", 165 },
    { "cent", 162 },
    { "sect", 167 },
    { "para", 182 },
    { "middot", 183 },
    { "deg", 176 },
    { "plusmn", 177 },
    { "times", 215 },
    { "divide", 247 },
    { "frac12", 189 },
    { "frac14", 188 },
    { "frac34", 190 },
    { "laquo", 171 },
    { "raquo", 187 },
    { "iexcl", 161 },
    { "iquest", 191 },
    { "szlig", 223 },
    { "agrave", 224 },
    { "aacute", 225 },
    { "eacute", 233 },
    { "egrave", 232 },
    { "ccedil", 231 },
    { "ntilde", 241 },
    { "ouml", 246 },
    { "uuml", 252 },
    { "auml", 228 },
    { "alpha", 945 },
    { "beta", 946 },
    { "gamma", 947 },
    { "pi", 960 },
    { "micro", 181 },
    { "larr", 8592 },
    { "rarr", 8594 },
    { "harr", 8596 },
    { "crarr", 8629 },
    { "infin", 8734 },
    { "ne", 8800 },
    { "le", 8804 },
    { "ge", 8805 },
    { "asymp", 8776 },
    { "sum", 8721 },
    { "radic", 8730 },
    { "star", 9733 },
    { "hearts", 9829 },
    { "check", 10003 },
    { "cross", 10007 },
};

int ht_entity(const char* s, size_t n, size_t* pi, hb_t* out)
{
    size_t i = *pi + 1, j;
    uint32_t cp = 0;

    if (i < n && s[i] == '#') {
        int hex = (i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')), any = 0;
        j = hex ? i + 2 : i + 1;
        for (; j < n; j++) {
            int d;
            uint32_t base = hex ? 16u : 10u;
            if (s[j] >= '0' && s[j] <= '9')
                d = s[j] - '0';
            else if (hex && s[j] >= 'a' && s[j] <= 'f')
                d = s[j] - 'a' + 10;
            else if (hex && s[j] >= 'A' && s[j] <= 'F')
                d = s[j] - 'A' + 10;
            else
                break;
            if (cp > (0xFFFFFFFFu - (uint32_t)d) / base || cp > 0x10FFFF) {
                cp = 0x110000;
                any = 1;
                while (++j < n) {
                    if (s[j] >= '0' && s[j] <= '9')
                        continue;
                    if (hex && s[j] >= 'a' && s[j] <= 'f')
                        continue;
                    if (hex && s[j] >= 'A' && s[j] <= 'F')
                        continue;
                    break;
                }
                break;
            }
            cp = cp * base + (uint32_t)d;
            any = 1;
        }
        if (!any)
            return 0;
        if (j < n && s[j] == ';')
            j++;
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            cp = 0xFFFD;
        hb_putc(out, cp);
        *pi = j;
        return 1;
    }
    for (j = i; j < n && j < i + 32; j++)
        if (s[j] == ';')
            break;
    if (j >= n || j >= i + 32 || j == i)
        return 0;
    {
        size_t k, len = j - i;
        for (k = 0; k < countof(HT_ENT); k++)
            if (ht_ieq(s + i, len, HT_ENT[k].name)) {
                hb_putc(out, HT_ENT[k].cp);
                *pi = j + 1;
                return 1;
            }
    }
    return 0;
}

const uint8_t HT_ESC[256] = { ['<'] = 1, ['>'] = 1, ['&'] = 1, ['"'] = 1, ['\0'] = 1 };

const uint8_t HT_ESC_SET[5] = { '<', '>', '&', '"', 0 };

void ht_escape(hb_t* b, const char* s, size_t n, int attr)
{
    size_t i = 0, run;

    while (i < n) {
        run = i;
        {
            size_t probe = i + HT_SIMD_MIN;
            if (probe > n)
                probe = n;
            while (i < probe && !HT_ESC[(unsigned char)s[i]])
                i++;
            if (i == probe && i < n) {
                size_t k = simd.find_first_of((const uint8_t*)s + i, n - i,
                    HT_ESC_SET, sizeof HT_ESC_SET);
                i = (k == (size_t)-1) ? n : i + k;
            }
        }
        hb_write(b, s + run, i - run);
        if (i >= n)
            break;
        switch (s[i]) {
        case '<':
            hb_puts(b, "&lt;");
            break;
        case '>':
            hb_puts(b, "&gt;");
            break;
        case '&':
            hb_puts(b, "&amp;");
            break;
        case '\0':
            hb_write(b, "\xEF\xBF\xBD", 3);
            break;
        default:
            hb_puts(b, attr ? "&quot;" : "\"");
            break;
        }
        i++;
    }
}

const char* const HT_CLOSES_P[] = {
    "address",
    "article",
    "aside",
    "blockquote",
    "div",
    "dl",
    "fieldset",
    "figure",
    "footer",
    "form",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "header",
    "hr",
    "main",
    "nav",
    "ol",
    "p",
    "pre",
    "section",
    "table",
    "ul",
};

const char* const HT_RAW_ESCAPELESS[] = { "script", "style" };

const uint8_t TPL_ESC[256] = {
    ['<'] = 1,
    ['>'] = 1,
    ['&'] = 1,
    ['"'] = 1,
    ['\''] = 1,
};

const uint8_t TPL_ESC_SET[5] = { '<', '>', '&', '"', '\'' };
