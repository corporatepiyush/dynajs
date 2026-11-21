#include <stdlib.h>
#include "dd.h"
#include "../../libregexp.h"

#define DD_RE_STEPS 100000000ull

static size_t re_strip_extended(const char* p, size_t n, char* out)
{
    size_t i, o = 0;
    int in_class = 0;
    for (i = 0; i < n; i++) {
        char ch = p[i];
        if (ch == '\\' && i + 1 < n) {
            out[o++] = ch;
            out[o++] = p[++i];
            continue;
        }
        if (in_class) {
            if (ch == ']')
                in_class = 0;
            out[o++] = ch;
            continue;
        }
        if (ch == '[') {
            in_class = 1;
            out[o++] = ch;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v')
            continue;
        if (ch == '#') {
            while (i < n && p[i] != '\n')
                i++;
            continue;
        }
        out[o++] = ch;
    }
    return o;
}

int dd_regex_compile(dd_regex* r, const char* pat, size_t plen, const char* opts, size_t olen, void* opaque, dd_err* e)
{
    int flags = 0, len = 0;
    size_t i;
    char err[128];
    char* tmp = NULL;
    uint8_t* bc;
    memset(r, 0, sizeof *r);
    if (!opaque)
        return dd_fail(e, DD_E_BADVALUE, "$regex is not allowed in this context");
    if (memchr(pat, 0, plen))
        return dd_fail(e, DD_E_BADVALUE, "Regular expression cannot contain an embedded null byte");
    for (i = 0; i < olen; i++) {
        switch (opts[i]) {
        case 'i':
            flags |= LRE_FLAG_IGNORECASE;
            break;
        case 'm':
            flags |= LRE_FLAG_MULTILINE;
            break;
        case 's':
            flags |= LRE_FLAG_DOTALL;
            break;
        case 'x':
            if (!tmp) {
                tmp = malloc(plen + 1);
                if (!tmp)
                    return dd_fail(e, DD_E_OOM, "out of memory");
                plen = re_strip_extended(pat, plen, tmp);
                tmp[plen] = '\0';
                pat = tmp;
            }
            break;
        case 'u':
        case 'l':
            break;
        default:
            free(tmp);
            return dd_fail(e, DD_E_BADVALUE, "invalid flag in regex options: %c", opts[i]);
        }
    }
    bc = lre_compile(&len, err, sizeof err, pat, plen, flags, opaque);
    free(tmp);
    if (!bc)
        return dd_fail(e, DD_E_BADVALUE, "Regular expression is invalid: %s", err);
    r->bc = malloc((size_t)len);
    if (!r->bc) {
        lre_realloc(opaque, bc, 0);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    memcpy(r->bc, bc, (size_t)len);
    lre_realloc(opaque, bc, 0);
    r->ncap = lre_get_alloc_count(r->bc);
    return 0;
}

static int re_grow(dd_regex* r, size_t need)
{
    if (r->u16cap >= need)
        return 0;
    {
        size_t nc = r->u16cap ? r->u16cap : 64;
        uint16_t* np;
        while (nc < need)
            nc *= 2;
        np = realloc(r->u16, nc * sizeof *np);
        if (!np)
            return -1;
        r->u16 = np;
        r->u16cap = nc;
    }
    return 0;
}

int dd_regex_match(dd_regex* r, const uint8_t* s, size_t n, void* opaque, dd_err* e)
{
    uint8_t* capstack[32];
    uint8_t** cap = capstack;
    const uint8_t* buf = s;
    int type = 0, len = (int)n, rc;
    size_t i;
    if (n > 0x7FFFFFFF)
        return dd_fail(e, DD_E_BADVALUE, "string too long for $regex");
    for (i = 0; i < n; i++)
        if (s[i] >= 0x80)
            break;
    if (i < n) {
        size_t o = 0;
        if (re_grow(r, n + 1) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        for (i = 0; i < n;) {
            uint32_t c = s[i];
            if (c < 0x80) {
                i++;
            } else if (c < 0xE0 && i + 1 < n) {
                c = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
                i += 2;
            } else if (c < 0xF0 && i + 2 < n) {
                c = ((c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
                i += 3;
            } else if (i + 3 < n) {
                c = ((c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) | ((uint32_t)(s[i + 2] & 0x3F) << 6)
                    | (s[i + 3] & 0x3F);
                i += 4;
            } else {
                c = 0xFFFD;
                i++;
            }
            if (c >= 0x10000) {
                c -= 0x10000;
                r->u16[o++] = (uint16_t)(0xD800 + (c >> 10));
                r->u16[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
            } else {
                r->u16[o++] = (uint16_t)c;
            }
        }
        buf = (const uint8_t*)r->u16;
        len = (int)o;
        type = 1;
    }
    if (r->ncap > 32) {
        cap = malloc((size_t)r->ncap * sizeof *cap);
        if (!cap)
            return dd_fail(e, DD_E_OOM, "out of memory");
    }
    rc = lre_exec_budget(cap, r->bc, buf, 0, len, type, opaque, DD_RE_STEPS);
    if (cap != capstack)
        free(cap);
    if (rc < 0) {
        if (rc == LRE_RET_TIMEOUT)
            return dd_fail(e, DD_E_REGEXTIMEOUT, "$regex execution exceeded its step budget");
        return dd_fail(e, DD_E_OOM, "$regex execution failed (out of memory)");
    }
    return rc == 1;
}

void dd_regex_free(dd_regex* r)
{
    free(r->bc);
    free(r->u16);
    memset(r, 0, sizeof *r);
}
