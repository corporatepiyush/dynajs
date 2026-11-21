#include "yamlscalar.h"

__attribute__((noinline)) int yb_grow(yb_t* b, size_t n)
{
    if (b->oom)
        return 0;
    if (!dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 128)) {
        b->oom = 1;
        return 0;
    }
    return 1;
}

size_t yml_plain_end(const char* s, size_t start, size_t end)
{
    size_t i = start, last = start;
    while (i < end) {
        if (s[i] == '#' && i > start
            && (s[i - 1] == ' ' || s[i - 1] == '\t'))
            break;
        if (s[i] != ' ' && s[i] != '\t')
            last = i + 1;
        i++;
    }
    return last;
}

static int yml_digits(const char* s, size_t i, size_t n, int base, uint64_t lim, uint64_t* v)
{
    if (i >= n)
        return 0;
    for (; i < n; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9')
            d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f')
            d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F')
            d = s[i] - 'A' + 10;
        else
            return 0;
        if (d >= base || *v > (lim - (uint64_t)d) / (uint64_t)base)
            return 0;
        *v = *v * (uint64_t)base + (uint64_t)d;
    }
    return 1;
}

int yml_is_int(const char* s, size_t n, int64_t* out)
{
    size_t i = 0;
    int neg = 0, base = 10;
    uint64_t v = 0;

    if (!n)
        return 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i + 2 < n && s[i] == '0') {
        if (s[i + 1] == 'x' || s[i + 1] == 'X') {
            base = 16;
            i += 2;
        } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
            base = 8;
            i += 2;
        }
    }
    if (!yml_digits(s, i, n, base, (uint64_t)INT64_MAX + (uint64_t)neg, &v))
        return 0;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 1;
}

int yml_is_int_syntax(const char* s, size_t n)
{
    size_t i = 0;
    int base = 10;
    if (!n)
        return 0;
    if (s[0] == '-' || s[0] == '+')
        i = 1;
    if (i + 2 < n && s[i] == '0') {
        if (s[i + 1] == 'x' || s[i + 1] == 'X') {
            base = 16;
            i += 2;
        } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
            base = 8;
            i += 2;
        }
    }
    if (i >= n)
        return 0;
    for (; i < n; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9')
            d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f')
            d = 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F')
            d = 10;
        else
            return 0;
        if (d >= base)
            return 0;
    }
    return 1;
}

int yml_is_float(const char* s, size_t n)
{
    size_t i = 0;
    int digits = 0, dot = 0;
    if (!n)
        return 0;
    if (s[0] == '-' || s[0] == '+')
        i = 1;
    for (; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            digits = 1;
            continue;
        }
        if (s[i] == '.' && !dot) {
            dot = 1;
            continue;
        }
        if ((s[i] == 'e' || s[i] == 'E') && digits) {
            i++;
            if (i < n && (s[i] == '-' || s[i] == '+'))
                i++;
            if (i >= n)
                return 0;
            for (; i < n; i++)
                if (s[i] < '0' || s[i] > '9')
                    return 0;
            return 1;
        }
        return 0;
    }
    return digits && dot;
}

static const uint8_t yml_kc_special[256] = {
    ['"'] = 1, ['\''] = 1, ['['] = 1, ['{'] = 1, [']'] = 1, ['}'] = 1, ['#'] = 1, [':'] = 1
};

size_t yml_key_colon(const char* s, size_t st, size_t end)
{
    size_t i = st;
    int flow = 0, qclosed = 0;
    char q = 0;

    while (i < end) {
        char c = s[i];
        if (q) {
            if (q == '"' && c == '\\' && i + 1 < end) {
                i += 2;
                continue;
            }
            if (c == q) {
                q = 0;
                qclosed = 1;
                i++;
                continue;
            }
            qclosed = 0;
            i++;
            if (q == '\'') {
                const char* e = memchr(s + i, '\'', end - i);
                i = e ? (size_t)(e - s) : end;
            } else {
                while (i < end && s[i] != '"' && s[i] != '\\')
                    i++;
            }
            continue;
        }
        if (!yml_kc_special[(uint8_t)c]) {
            qclosed = 0;
            i++;
            while (i < end && !yml_kc_special[(uint8_t)s[i]])
                i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            q = c;
            qclosed = 0;
        } else if (c == '[' || c == '{') {
            flow++;
        } else if (c == ']' || c == '}') {
            if (flow)
                flow--;
        } else if (c == '#' && i > st && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
            break;
        } else if (c == ':' && !flow
            && (i + 1 >= end || s[i + 1] == ' ' || s[i + 1] == '\t'
                || qclosed)) {
            return i;
        } else {
            qclosed = 0;
        }
        i++;
    }
    return (size_t)-1;
}

void yml_put_escaped_body(yb_t* b, const char* s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            yb_puts(b, "\\\"");
            break;
        case '\\':
            yb_puts(b, "\\\\");
            break;
        case '\n':
            yb_puts(b, "\\n");
            break;
        case '\t':
            yb_puts(b, "\\t");
            break;
        case '\r':
            yb_puts(b, "\\r");
            break;
        default:
            if (c < 0x20) {
                char t[8];
                int k = snprintf(t, sizeof t, "\\u%04x", c);
                yb_write(b, t, (size_t)k);
            } else {
                yb_put(b, c);
            }
        }
    }
}
