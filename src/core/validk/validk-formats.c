#include "validk.h"

typedef struct {
    char cc[2];
    uint8_t len;
} dyn_iban_t;

static const uint8_t DYN_V_ECLS[256] = {
    ['a'] = 7,
    ['b'] = 7,
    ['c'] = 7,
    ['d'] = 7,
    ['e'] = 7,
    ['f'] = 7,
    ['g'] = 7,
    ['h'] = 7,
    ['i'] = 7,
    ['j'] = 7,
    ['k'] = 7,
    ['l'] = 7,
    ['m'] = 7,
    ['n'] = 7,
    ['o'] = 7,
    ['p'] = 7,
    ['q'] = 7,
    ['r'] = 7,
    ['s'] = 7,
    ['t'] = 7,
    ['u'] = 7,
    ['v'] = 7,
    ['w'] = 7,
    ['x'] = 7,
    ['y'] = 7,
    ['z'] = 7,
    ['A'] = 7,
    ['B'] = 7,
    ['C'] = 7,
    ['D'] = 7,
    ['E'] = 7,
    ['F'] = 7,
    ['G'] = 7,
    ['H'] = 7,
    ['I'] = 7,
    ['J'] = 7,
    ['K'] = 7,
    ['L'] = 7,
    ['M'] = 7,
    ['N'] = 7,
    ['O'] = 7,
    ['P'] = 7,
    ['Q'] = 7,
    ['R'] = 7,
    ['S'] = 7,
    ['T'] = 7,
    ['U'] = 7,
    ['V'] = 7,
    ['W'] = 7,
    ['X'] = 7,
    ['Y'] = 7,
    ['Z'] = 7,
    ['0'] = 3,
    ['1'] = 3,
    ['2'] = 3,
    ['3'] = 3,
    ['4'] = 3,
    ['5'] = 3,
    ['6'] = 3,
    ['7'] = 3,
    ['8'] = 3,
    ['9'] = 3,
    ['-'] = 3,
    ['!'] = 1,
    ['#'] = 1,
    ['$'] = 1,
    ['%'] = 1,
    ['&'] = 1,
    ['\''] = 1,
    ['*'] = 1,
    ['+'] = 1,
    ['/'] = 1,
    ['='] = 1,
    ['?'] = 1,
    ['^'] = 1,
    ['_'] = 1,
    ['`'] = 1,
    ['{'] = 1,
    ['|'] = 1,
    ['}'] = 1,
    ['~'] = 1,
};

static const uint8_t DYN_V_MIMETOK[256] = {
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
    ['{'] = 1,
    ['|'] = 1,
    ['}'] = 1,
    ['~'] = 1,
};

int dyn_v_email(const char* s, size_t n)
{
    size_t at = (size_t)-1, i, dot = (size_t)-1, dn, lab = 0;
    if (n < 3 || n > 254)
        return 0;
    {
        const char* a = (const char*)memchr(s, '@', n);
        if (!a)
            return 0;
        at = (size_t)(a - s);
        if (at + 1 < n && memchr(a + 1, '@', n - at - 1))
            return 0;
    }
    if (at == 0 || at + 1 >= n)
        return 0;
    if (at > 64)
        return 0;
    if (s[0] == '.' || s[at - 1] == '.')
        return 0;
    for (i = 0; i < at; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i + 1 < at && s[i + 1] == '.')
                return 0;
            continue;
        }
        if (!(DYN_V_ECLS[c] & 1))
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
            if (lab > 63)
                return 0;
            lab = 0;
            dot = i;
            continue;
        }
        lab++;
        if (!(DYN_V_ECLS[c] & 2))
            return 0;
        if (c == '-' && s[i - 1] == '.')
            return 0;
    }
    if (lab > 63)
        return 0;
    if (dot == (size_t)-1 || n - dot - 1 < 2)
        return 0;
    for (i = dot + 1; i < n; i++)
        if (!(DYN_V_ECLS[(unsigned char)s[i]] & 4))
            return 0;
    return 1;
}

static const dyn_iban_t DYN_IBAN[] = {
    { { 'A', 'D' }, 24 },
    { { 'A', 'E' }, 23 },
    { { 'A', 'F' }, 28 },
    { { 'A', 'L' }, 28 },
    { { 'A', 'T' }, 20 },
    { { 'A', 'Z' }, 28 },
    { { 'B', 'A' }, 20 },
    { { 'B', 'E' }, 16 },
    { { 'B', 'G' }, 22 },
    { { 'B', 'H' }, 22 },
    { { 'B', 'I' }, 27 },
    { { 'B', 'R' }, 29 },
    { { 'B', 'Y' }, 28 },
    { { 'C', 'H' }, 21 },
    { { 'C', 'R' }, 22 },
    { { 'C', 'Y' }, 28 },
    { { 'C', 'Z' }, 24 },
    { { 'D', 'E' }, 22 },
    { { 'D', 'J' }, 27 },
    { { 'D', 'K' }, 18 },
    { { 'D', 'O' }, 28 },
    { { 'E', 'E' }, 20 },
    { { 'E', 'G' }, 29 },
    { { 'E', 'S' }, 24 },
    { { 'F', 'I' }, 18 },
    { { 'F', 'O' }, 18 },
    { { 'F', 'R' }, 27 },
    { { 'G', 'B' }, 22 },
    { { 'G', 'E' }, 22 },
    { { 'G', 'I' }, 23 },
    { { 'G', 'L' }, 18 },
    { { 'G', 'R' }, 27 },
    { { 'G', 'T' }, 28 },
    { { 'H', 'R' }, 21 },
    { { 'H', 'U' }, 28 },
    { { 'I', 'E' }, 22 },
    { { 'I', 'L' }, 23 },
    { { 'I', 'Q' }, 23 },
    { { 'I', 'R' }, 26 },
    { { 'I', 'S' }, 26 },
    { { 'I', 'T' }, 27 },
    { { 'J', 'O' }, 30 },
    { { 'K', 'W' }, 30 },
    { { 'K', 'Z' }, 20 },
    { { 'L', 'B' }, 28 },
    { { 'L', 'C' }, 32 },
    { { 'L', 'I' }, 21 },
    { { 'L', 'T' }, 20 },
    { { 'L', 'U' }, 20 },
    { { 'L', 'V' }, 21 },
    { { 'L', 'Y' }, 25 },
    { { 'M', 'C' }, 27 },
    { { 'M', 'D' }, 24 },
    { { 'M', 'E' }, 22 },
    { { 'M', 'K' }, 19 },
    { { 'M', 'N' }, 20 },
    { { 'M', 'R' }, 27 },
    { { 'M', 'T' }, 31 },
    { { 'M', 'U' }, 30 },
    { { 'N', 'I' }, 28 },
    { { 'N', 'L' }, 18 },
    { { 'N', 'O' }, 15 },
    { { 'O', 'M' }, 23 },
    { { 'P', 'K' }, 24 },
    { { 'P', 'L' }, 28 },
    { { 'P', 'S' }, 29 },
    { { 'P', 'T' }, 25 },
    { { 'Q', 'A' }, 29 },
    { { 'R', 'O' }, 24 },
    { { 'R', 'S' }, 22 },
    { { 'R', 'U' }, 33 },
    { { 'S', 'A' }, 24 },
    { { 'S', 'C' }, 31 },
    { { 'S', 'D' }, 18 },
    { { 'S', 'E' }, 24 },
    { { 'S', 'I' }, 19 },
    { { 'S', 'K' }, 24 },
    { { 'S', 'M' }, 27 },
    { { 'S', 'O' }, 23 },
    { { 'S', 'T' }, 25 },
    { { 'S', 'V' }, 28 },
    { { 'T', 'L' }, 23 },
    { { 'T', 'N' }, 24 },
    { { 'T', 'R' }, 26 },
    { { 'U', 'A' }, 29 },
    { { 'V', 'A' }, 22 },
    { { 'V', 'G' }, 24 },
    { { 'X', 'K' }, 20 },
};

int dyn_v_iban(const char* s, size_t n)
{
    char buf[64];
    size_t k = 0, i;
    unsigned rem = 0;
    int len = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '-')
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
    for (i = 0; i < sizeof(DYN_IBAN) / sizeof(DYN_IBAN[0]); i++)
        if (DYN_IBAN[i].cc[0] == buf[0] && DYN_IBAN[i].cc[1] == buf[1]) {
            len = DYN_IBAN[i].len;
            break;
        }
    if (!len || (size_t)len != k)
        return 0;
    for (i = 4; i < k; i++)
        if (!dyn_v_alpha((unsigned char)buf[i]) && !dyn_v_digit((unsigned char)buf[i]))
            return 0;
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

static int dyn_v_ipv6_group(const char* s, size_t n)
{
    size_t i;

    if (n == 0 || n > 4)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

int dyn_v_ipv6(const char* s, size_t n)
{
    size_t i = 0, groups = 0;
    int compressed = 0;

    if (n < 2 || n > 45)
        return 0;
    if (s[0] == ':') {
        if (s[1] != ':')
            return 0;
        compressed = 1;
        i = 2;
    }
    while (i < n) {
        size_t start = i;
        int dots = 0;
        while (i < n && s[i] != ':') {
            if (s[i] == '.')
                dots = 1;
            i++;
        }
        if (dots) {
            if (i != n)
                return 0;
            if (!dyn_v_ipv4(s + start, i - start))
                return 0;
            groups += 2;
            break;
        }
        if (!dyn_v_ipv6_group(s + start, i - start))
            return 0;
        groups++;
        if (i < n) {
            i++;
            if (i < n && s[i] == ':') {
                if (compressed)
                    return 0;
                compressed = 1;
                i++;
            } else if (i == n) {
                return 0;
            }
        }
    }
    if (compressed)
        return groups <= 7;
    return groups == 8;
}

int dyn_v_rfc3339(const char* s, size_t n)
{
    size_t i;
    int h, mi, sec;

    if (n < 20)
        return 0;
    if (!dyn_v_datestring(s, 10))
        return 0;
    if (s[10] != 'T' && s[10] != 't')
        return 0;
    for (i = 11; i < 19; i++) {
        if (i == 13 || i == 16) {
            if (s[i] != ':')
                return 0;
        } else if (!dyn_v_digit((unsigned char)s[i])) {
            return 0;
        }
    }
    h = (s[11] - '0') * 10 + (s[12] - '0');
    mi = (s[14] - '0') * 10 + (s[15] - '0');
    sec = (s[17] - '0') * 10 + (s[18] - '0');
    if (h > 23 || mi > 59 || sec > 60)
        return 0;
    i = 19;
    if (i < n && s[i] == '.') {
        size_t fd = 0;
        i++;
        while (i < n && dyn_v_digit((unsigned char)s[i])) {
            i++;
            fd++;
        }
        if (fd == 0)
            return 0;
    }
    if (i < n && (s[i] == 'Z' || s[i] == 'z'))
        return i + 1 == n;
    if (i + 6 != n || (s[i] != '+' && s[i] != '-'))
        return 0;
    if (!dyn_v_digit((unsigned char)s[i + 1]) || !dyn_v_digit((unsigned char)s[i + 2]) || s[i + 3] != ':' || !dyn_v_digit((unsigned char)s[i + 4]) || !dyn_v_digit((unsigned char)s[i + 5]))
        return 0;
    h = (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
    mi = (s[i + 4] - '0') * 10 + (s[i + 5] - '0');
    return h <= 23 && mi <= 59;
}

static int dyn_v_mime_token(const char* s, size_t n)
{
    size_t i;

    if (n == 0 || n > 127)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && !DYN_V_MIMETOK[c])
            return 0;
    }
    return 1;
}

static int dyn_v_mime_names_equal(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z')
            x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (unsigned char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

int dyn_v_mime(const char* s, size_t n)
{
    const char* np[64];
    size_t nl[64], nn = 0, k, i = 0, start;

    if (n < 3 || n > 255)
        return 0;
    start = i;
    while (i < n && s[i] != '/' && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
        i++;
    if (i >= n || s[i] != '/')
        return 0;
    if (!dyn_v_mime_token(s + start, i - start))
        return 0;
    i++;
    start = i;
    while (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
        i++;
    if (!dyn_v_mime_token(s + start, i - start))
        return 0;
    while (i < n) {
        size_t plen, w, vstart;
        if (s[i] == ' ' || s[i] == '\t') {
            while (i < n && (s[i] == ' ' || s[i] == '\t'))
                i++;
            if (i >= n || s[i] != ';')
                return 0;
        }
        if (s[i] != ';')
            return 0;
        i++;
        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i >= n)
            return 0;
        start = i;
        while (i < n && s[i] != '=' && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
            i++;
        plen = i - start;
        if (!dyn_v_mime_token(s + start, plen))
            return 0;
        for (k = 0; k < nn; k++)
            if (nl[k] == plen && dyn_v_mime_names_equal(np[k], s + start, plen))
                return 0;
        w = i;
        while (w < n && (s[w] == ' ' || s[w] == '\t'))
            w++;
        if (w >= n || s[w] != '=')
            return 0;
        if (nn < sizeof(np) / sizeof(np[0])) {
            np[nn] = s + start;
            nl[nn] = plen;
            nn++;
        }
        i = w + 1;
        if (i >= n)
            return 0;
        if (s[i] == '"') {
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\') {
                    if (i + 1 >= n)
                        return 0;
                    i++;
                } else if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f) {
                    return 0;
                }
                i++;
            }
            if (i >= n)
                return 0;
            i++;
            if (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
                return 0;
        } else {
            vstart = i;
            while (i < n && s[i] != ';' && s[i] != ' ' && s[i] != '\t')
                i++;
            if (!dyn_v_mime_token(s + vstart, i - vstart))
                return 0;
        }
    }
    return 1;
}

