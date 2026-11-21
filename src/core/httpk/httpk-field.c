#include "httpk.h"

const uint8_t httpk_tchar[256] = {
    ['!'] = 1,
    ['#'] = 1,
    ['$'] = 1,
    ['%'] = 1,
    ['&'] = 1,
    ['\''] = 1,
    ['*'] = 1,
    ['+'] = 1,
    ['-'] = 1,
    ['.'] = 1,
    ['^'] = 1,
    ['_'] = 1,
    ['`'] = 1,
    ['|'] = 1,
    ['~'] = 1,
    ['0'] = 1,
    ['1'] = 1,
    ['2'] = 1,
    ['3'] = 1,
    ['4'] = 1,
    ['5'] = 1,
    ['6'] = 1,
    ['7'] = 1,
    ['8'] = 1,
    ['9'] = 1,
    ['A'] = 1,
    ['B'] = 1,
    ['C'] = 1,
    ['D'] = 1,
    ['E'] = 1,
    ['F'] = 1,
    ['G'] = 1,
    ['H'] = 1,
    ['I'] = 1,
    ['J'] = 1,
    ['K'] = 1,
    ['L'] = 1,
    ['M'] = 1,
    ['N'] = 1,
    ['O'] = 1,
    ['P'] = 1,
    ['Q'] = 1,
    ['R'] = 1,
    ['S'] = 1,
    ['T'] = 1,
    ['U'] = 1,
    ['V'] = 1,
    ['W'] = 1,
    ['X'] = 1,
    ['Y'] = 1,
    ['Z'] = 1,
    ['a'] = 1,
    ['b'] = 1,
    ['c'] = 1,
    ['d'] = 1,
    ['e'] = 1,
    ['f'] = 1,
    ['g'] = 1,
    ['h'] = 1,
    ['i'] = 1,
    ['j'] = 1,
    ['k'] = 1,
    ['l'] = 1,
    ['m'] = 1,
    ['n'] = 1,
    ['o'] = 1,
    ['p'] = 1,
    ['q'] = 1,
    ['r'] = 1,
    ['s'] = 1,
    ['t'] = 1,
    ['u'] = 1,
    ['v'] = 1,
    ['w'] = 1,
    ['x'] = 1,
    ['y'] = 1,
    ['z'] = 1,
};

int httpk_name_ok(const char* s, size_t n)
{
    size_t i;
    if (!s || n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (!httpk_tchar[(unsigned char)s[i]])
            return 0;
    return 1;
}

int httpk_value_ok(const char* s, size_t n)
{
    size_t i;
    if (!s)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 0;
    }
    return 1;
}

int httpk_value_gen_ok(const char* s, size_t n)
{
    size_t i;
    if (!s)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

int httpk_ci_eq(const char* a, size_t an, const char* lit)
{
    size_t i;
    for (i = 0; i < an; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)lit[i];
        if (!y)
            return 0;
        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y + 32);
        if (x != y)
            return 0;
    }
    return lit[an] == 0;
}

static int httpk_ows(char c) { return c == ' ' || c == '\t'; }

static size_t httpk_elem(const char* v, size_t n, size_t i, size_t* b,
    size_t* e)
{
    size_t j;
    while (i < n && (httpk_ows(v[i]) || v[i] == ','))
        i++;
    j = i;
    while (j < n && v[j] != ',')
        j++;
    *b = i;
    *e = j;
    while (*e > *b && httpk_ows(v[*e - 1]))
        (*e)--;
    return j;
}

int httpk_list_has(const char* v, size_t n, const char* tok)
{
    size_t i = 0, b, e;
    if (!v)
        return 0;
    while (i < n) {
        i = httpk_elem(v, n, i, &b, &e);
        if (e > b && httpk_ci_eq(v + b, e - b, tok))
            return 1;
    }
    return 0;
}

int httpk_list_last_is(const char* v, size_t n, const char* tok)
{
    size_t i = 0, b, e, lb = 0, le = 0;
    int any = 0;
    if (!v)
        return 0;
    while (i < n) {
        i = httpk_elem(v, n, i, &b, &e);
        if (e > b) {
            lb = b;
            le = e;
            any = 1;
        }
    }
    return any && httpk_ci_eq(v + lb, le - lb, tok);
}

static int httpk_parse_q(const char* s, size_t n)
{
    size_t i = 0;
    int q = 0, k, tail = 0;
    if (n == 0 || (s[0] != '0' && s[0] != '1'))
        return -1;
    q = (s[0] - '0') * 1000;
    i = 1;
    if (i < n && s[i] == '.') {
        i++;
        for (k = 100; i < n && s[i] >= '0' && s[i] <= '9'; i++, k /= 10) {
            q += (s[i] - '0') * k;
            if (!k && s[i] != '0')
                tail = 1;
        }
    }
    if (i != n || q > 1000 || (q == 1000 && tail))
        return -1;
    return q == 0 && tail ? 1 : q;
}

int httpk_weight(const char* el, size_t n)
{
    size_t i = 0;
    if (!el)
        return 1000;
    while (i < n && el[i] != ';')
        i++;
    while (i < n) {
        size_t pb = i + 1, pe, eq;
        while (pb < n && httpk_ows(el[pb]))
            pb++;
        pe = pb;
        while (pe < n && el[pe] != ';')
            pe++;
        i = pe;
        if (pb >= pe || (el[pb] != 'q' && el[pb] != 'Q'))
            continue;
        eq = pb + 1;
        while (eq < pe && httpk_ows(el[eq]))
            eq++;
        if (eq >= pe || el[eq] != '=')
            continue;
        eq++;
        while (eq < pe && httpk_ows(el[eq]))
            eq++;
        while (pe > eq && httpk_ows(el[pe - 1]))
            pe--;
        {
            int q = httpk_parse_q(el + eq, pe - eq);
            return q < 0 ? 1000 : q;
        }
    }
    return 1000;
}

int httpk_qvalue(const char* v, size_t n, const char* tok)
{
    size_t i = 0, b, e;
    if (!v)
        return -1;
    while (i < n) {
        size_t te;
        i = httpk_elem(v, n, i, &b, &e);
        if (e <= b)
            continue;
        te = b;
        while (te < e && v[te] != ';')
            te++;
        while (te > b && httpk_ows(v[te - 1]))
            te--;
        if (httpk_ci_eq(v + b, te - b, tok))
            return httpk_weight(v + b, e - b);
    }
    return -1;
}

int httpk_is_framing_name(const char* s, size_t n)
{
    return httpk_ci_eq(s, n, "host") || httpk_ci_eq(s, n, "content-length")
        || httpk_ci_eq(s, n, "transfer-encoding")
        || httpk_ci_eq(s, n, "connection") || httpk_ci_eq(s, n, "keep-alive");
}

static const char* const httpk_single[] = {
    "content-length",
    "host",
    "content-type",
    "location",
    "etag",
    "last-modified",
    "date",
    "expires",
    "age",
    "retry-after",
    "content-location",
    "authorization",
    "proxy-authorization",
    "if-modified-since",
    "if-unmodified-since",
    "max-forwards",
    "referer",
    "user-agent",
    "from",
    "content-range",
    "range",
};

int httpk_singleton_slot(const char* name, size_t n)
{
    size_t i;
    for (i = 0; i < HTTPK_SINGLETONS; i++)
        if (httpk_ci_eq(name, n, httpk_single[i]))
            return (int)i;
    return -1;
}

int httpk_field_kind(const char* name, size_t n)
{
    if (httpk_ci_eq(name, n, "set-cookie"))
        return HTTPK_FIELD_SET_COOKIE;
    return httpk_singleton_slot(name, n) >= 0 ? HTTPK_FIELD_SINGLETON
                                              : HTTPK_FIELD_LIST;
}

int httpk_is_credential_name(const char* s, size_t n)
{
    return httpk_ci_eq(s, n, "authorization") || httpk_ci_eq(s, n, "cookie")
        || httpk_ci_eq(s, n, "cookie2")
        || httpk_ci_eq(s, n, "proxy-authorization");
}

int httpk_block_has(const char* blk, size_t n, const char* name)
{
    size_t k = strlen(name), i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && blk[e] != '\n')
            e++;
        if (e - i > k && blk[i + k] == ':' && httpk_ci_eq(blk + i, k, name))
            return 1;
        i = e + 1;
    }
    return 0;
}
