#include "validk.h"

int dyn_v_luhn(const char* s, size_t n)
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
            if (d > 9)
                d -= 9;
        }
        sum += d;
        alt = !alt;
        digits++;
    }
    if (digits < 12 || digits > 19)
        return 0;
    return (sum % 10) == 0;
}

int dyn_v_port(const char* s, size_t n)
{
    size_t i;
    unsigned v = 0;

    if (n == 0 || n > 5)
        return 0;
    if (n > 1 && s[0] == '0')
        return 0;
    for (i = 0; i < n; i++) {
        if (!dyn_v_digit((unsigned char)s[i]))
            return 0;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    return v <= 65535;
}

static int dyn_v_b64val(unsigned char c)
{
    if (dyn_v_alpha(c))
        return c <= 'Z' ? c - 'A' : c - 'a' + 26;
    if (dyn_v_digit(c))
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int dyn_v_base64(const char* s, size_t n)
{
    size_t i, core;

    if (n == 0 || (n % 4) != 0)
        return 0;
    core = n;
    while (core > 0 && s[core - 1] == '=')
        core--;
    if (n - core > 2)
        return 0;
    for (i = 0; i < core; i++)
        if (dyn_v_b64val((unsigned char)s[i]) < 0)
            return 0;
    if (core < n) {
        int v = dyn_v_b64val((unsigned char)s[core - 1]);
        if (n - core == 2 && (v & 0x0f))
            return 0;
        if (n - core == 1 && (v & 0x03))
            return 0;
    }
    return 1;
}

int dyn_v_base64url(const char* s, size_t n)
{
    size_t i;
    int v;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_alpha(c) && !dyn_v_digit(c) && c != '-' && c != '_')
            return 0;
    }
    if ((n % 4) == 1)
        return 0;
    if ((n % 4) == 2) {
        v = dyn_v_b64val((unsigned char)s[n - 1]);
        if (v & 0x0f)
            return 0;
    } else if ((n % 4) == 3) {
        v = dyn_v_b64val((unsigned char)s[n - 1]);
        if (v & 0x03)
            return 0;
    }
    return 1;
}

int dyn_v_hex(const char* s, size_t n)
{
    size_t i;

    if (n == 0 || (n % 2) != 0)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

int dyn_v_hexcolor(const char* s, size_t n)
{
    size_t i;

    if (n != 4 && n != 7 && n != 9)
        return 0;
    if (s[0] != '#')
        return 0;
    for (i = 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!dyn_v_digit(c) && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return 0;
    }
    return 1;
}

int dyn_v_numeric(const char* s, size_t n)
{
    size_t i = 0, digits = 0, dots = 0;

    if (i < n && (s[i] == '+' || s[i] == '-'))
        i++;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (++dots > 1)
                return 0;
            continue;
        }
        if (!dyn_v_digit(c))
            return 0;
        digits++;
    }
    return digits > 0;
}

int dyn_v_slug(const char* s, size_t n)
{
    size_t i;

    if (n == 0 || n > 64)
        return 0;
    if (s[0] == '-')
        return 0;
    if (s[n - 1] == '-')
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z')
            continue;
        if (dyn_v_digit(c))
            continue;
        if (c == '-') {
            if (i + 1 < n && s[i + 1] == '-')
                return 0;
            continue;
        }
        return 0;
    }
    return 1;
}

int dyn_v_e164(const char* s, size_t n)
{
    size_t i = 0, digits = 0;

    if (s[0] == '+')
        i = 1;
    if (i == n)
        return 0;
    for (; i < n; i++) {
        if (!dyn_v_digit((unsigned char)s[i]))
            return 0;
        digits++;
    }
    return digits >= 8 && digits <= 15;
}

int dyn_v_domain_grammar(const char* s, size_t n)
{
    size_t i, lab = 0, dots = 0;

    if (n < 3 || n > 253)
        return 0;
    for (i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? (unsigned char)s[i] : (unsigned char)'.';
        if (c == '.') {
            if (lab == 0 || lab > 63)
                return 0;
            if (s[i - lab] == '-' || s[i - 1] == '-')
                return 0;
            if (i < n)
                dots++;
            lab = 0;
        } else if (dyn_v_alpha(c) || dyn_v_digit(c) || c == '-') {
            lab++;
        } else {
            return 0;
        }
    }
    return dots >= 1;
}

