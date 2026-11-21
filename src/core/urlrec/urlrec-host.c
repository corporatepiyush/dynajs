#include "urlrec.h"
#include "../dyn-snappend.h"

#define DYN_URL_IDNA_MAX_CPS 4096

static int dyn_url_host_idna(dyn_url_t* u)
{
    const uint8_t* p = (const uint8_t*)u->host;
    const uint8_t* end = p + strlen(u->host);
    dyn_cp_t cps, mapped, out;
    uint32_t* norm = NULL;
    int norm_len;
    size_t i, start;
    int rc = -1;

    dyn_cp_init(&cps);
    dyn_cp_init(&mapped);
    dyn_cp_init(&out);
    while (p < end) {
        const uint8_t* np;
        uint32_t cp = (uint32_t)dyn_idna_utf8_next(p, (int)(end - p), &np);
        if (cp == (uint32_t)-1)
            goto done;
        if (dyn_cp_put(&cps, cp))
            goto oom;
        p = np;
    }
    for (i = 0; i < cps.n; i++) {
        int ri = dyn_tab3(idna_status_ranges, countof(idna_status_ranges),
            cps.p[i]);
        if (ri >= 0 && idna_status_ranges[ri][2] == 3)
            goto done;
        if (dyn_idna_map_cp(cps.p[i], 0, 0, &mapped))
            goto oom;
    }
    if (mapped.n > DYN_URL_IDNA_MAX_CPS)
        goto done;
    norm_len = idna_nfc(mapped.p, (int)mapped.n, &norm);
    if (norm_len < 0)
        goto done;
    if (norm_len == 0 || norm_len > DYN_URL_IDNA_MAX_CPS)
        goto done;
    start = 0;
    for (i = 0; i <= (size_t)norm_len; i++) {
        if (i == (size_t)norm_len || norm[i] == 0x2E) {
            size_t k;
            int has_nonascii = 0;
            for (k = start; k < i; k++)
                if (norm[k] > 0x7F) {
                    has_nonascii = 1;
                    break;
                }
            if (has_nonascii) {
                if (dyn_cp_put(&out, 'x') || dyn_cp_put(&out, 'n') || dyn_cp_put(&out, '-') || dyn_cp_put(&out, '-'))
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
        char* h = (char*)malloc(out.n * 4 + 1);
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

static int dyn_ipv4_number(const char* s, size_t n, uint64_t* out)
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
            return -1;
        *out = 0;
        return 0;
    }
    for (; i < n; i++) {
        int d = dyn_hexval((unsigned char)s[i]);
        if (d < 0 || d >= base)
            return -1;
        if (v <= 0xFFFFFFFFull)
            v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = v > 0xFFFFFFFFull ? 0x100000000ull : v;
    return 0;
}

static int dyn_host_ends_in_number(const char* h)
{
    size_t len = strlen(h), b, e = len, i;
    int digits = 1;
    uint64_t v;

    if (e && h[e - 1] == '.')
        e--;
    if (e == 0)
        return 0;
    b = e;
    while (b && h[b - 1] != '.')
        b--;
    for (i = b; i < e; i++)
        if (h[i] < '0' || h[i] > '9') {
            digits = 0;
            break;
        }
    if (digits && b < e)
        return 1;
    return dyn_ipv4_number(h + b, e - b, &v) == 0;
}

static int dyn_host_ipv4_rewrite(dyn_url_t* u)
{
    const char* h = u->host;
    size_t len = strlen(h), end = len, b = 0;
    uint64_t nums[4];
    uint64_t v;
    int np = 0, i;
    char out[16];

    if (end && h[end - 1] == '.')
        end--;
    while (b <= end) {
        size_t e = b;
        while (e < end && h[e] != '.')
            e++;
        if (np == 4)
            return -1;
        if (dyn_ipv4_number(h + b, e - b, &nums[np]) < 0)
            return -1;
        np++;
        b = e + 1;
    }
    for (i = 0; i < np - 1; i++)
        if (nums[i] > 255)
            return -1;
    if ((uint64_t)nums[np - 1] >= ((uint64_t)1 << (8 * (5 - np))))
        return -1;
    v = nums[np - 1];
    for (i = 0; i < np - 1; i++)
        v += (uint64_t)nums[i] << (8 * (3 - i));
    snprintf(out, sizeof out, "%u.%u.%u.%u",
        (unsigned)((v >> 24) & 0xFF), (unsigned)((v >> 16) & 0xFF),
        (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
    {
        char* nh = dyn_strndup(out, strlen(out));
        if (!nh)
            return -1;
        free(u->host);
        u->host = nh;
    }
    return 0;
}

static int dyn_ipv6_parse(const char* s, size_t n, uint16_t piece[8])
{
    size_t p = 0;
    int np = 0, compress = -1;

    memset(piece, 0, 8 * sizeof *piece);
    if (n && s[0] == ':') {
        if (n < 2 || s[1] != ':')
            return -1;
        p = 2;
        compress = ++np;
    }
    while (p < n) {
        uint32_t v = 0;
        int len = 0;
        if (np == 8)
            return -1;
        if (s[p] == ':') {
            if (compress >= 0)
                return -1;
            compress = ++np;
            p++;
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
        if (p < n && s[p] == '.') {
            int seen = 0;
            if (len == 0)
                return -1;
            p -= (size_t)len;
            if (np > 6)
                return -1;
            while (p < n) {
                uint32_t num = 0;
                int nd = 0;
                if (seen > 0) {
                    if (s[p] == '.' && seen < 4)
                        p++;
                    else
                        return -1;
                }
                if (p >= n || s[p] < '0' || s[p] > '9')
                    return -1;
                while (p < n && s[p] >= '0' && s[p] <= '9') {
                    if (nd == 0)
                        num = (uint32_t)(s[p] - '0');
                    else {
                        if (num == 0)
                            return -1;
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
                return -1;
            break;
        }
        if (len == 0)
            return -1;
        piece[np] = (uint16_t)v;
        np++;
        if (p >= n)
            break;
        if (s[p] != ':')
            return -1;
        p++;
        if (p == n)
            return -1;
    }
    if (compress < 0)
        return np == 8 ? 0 : -1;
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

static int dyn_host_ipv6_rewrite(dyn_url_t* u)
{
    const char* h = u->host;
    size_t n = strlen(h);
    uint16_t piece[8];
    int i, k = 1, ignore0 = 0;
    int found = -1, foundn = 0, longest = -1, longn = 1;
    char out[64];

    if (n < 2 || h[0] != '[' || h[n - 1] != ']')
        return -1;
    if (dyn_ipv6_parse(h + 1, n - 2, piece) < 0)
        return -1;
    for (i = 0; i < 8; i++) {
        if (piece[i] != 0) {
            if (foundn > longn) {
                longest = found;
                longn = foundn;
            }
            found = -1;
            foundn = 0;
        } else {
            if (found < 0)
                found = i;
            foundn++;
        }
    }
    if (foundn > longn) {
        longest = found;
        longn = foundn;
    }
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
        k = (int)dyn_snappend(out, sizeof out, (size_t)k, "%x", piece[i]);
        if (i != 7)
            out[k++] = ':';
    }
    out[k++] = ']';
    {
        char* nh = dyn_strndup(out, (size_t)k);
        if (!nh)
            return -1;
        free(u->host);
        u->host = nh;
    }
    return 0;
}

static int dyn_forbidden_domain_byte(unsigned char c)
{
    return dyn_url_cls_has(6u, c);
}

static int dyn_host_decode(dyn_url_t* u)
{
    const char* h = u->host;
    size_t n = strlen(h), i;
    dyn_sb_t dec;
    char* nh;

    if (!memchr(h, '%', n)) {
        const uint8_t* p = (const uint8_t*)h;
        const uint8_t* end = p + n;
        while (p < end) {
            const uint8_t* np;
            uint32_t cp;
            if (*p < 0x80) {
                if (dyn_forbidden_domain_byte(*p))
                    return -1;
                p++;
                continue;
            }
            cp = (uint32_t)dyn_idna_utf8_next(p, (int)(end - p), &np);
            if (cp == (uint32_t)-1)
                return -1;
            p = np;
        }
        return 0;
    }
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
    if (dec.oom) {
        dyn_sb_free(&dec);
        return -1;
    }
    {
        const uint8_t* p = (const uint8_t*)dec.p;
        const uint8_t* end = (const uint8_t*)(dec.p + dec.n);
        while (p < end) {
            const uint8_t* np;
            uint32_t cp = (uint32_t)dyn_idna_utf8_next(p, (int)(end - p), &np);
            if (cp == (uint32_t)-1) {
                dyn_sb_free(&dec);
                return -1;
            }
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

static int dyn_host_alabel_roundtrip(const char* h, size_t n)
{
    size_t i, ls = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || h[i] == '.') {
            size_t ll = i - ls;
            if (ll >= 4 && (h[ls] | 0x20) == 'x' && (h[ls + 1] | 0x20) == 'n' && h[ls + 2] == '-' && h[ls + 3] == '-') {
                dyn_cp_t dec, enc;
                size_t blen = ll - 4, k;
                if (blen == 0) {
                    ls = i + 1;
                    continue;
                }
                dyn_cp_init(&dec);
                dyn_cp_init(&enc);
                if (pc_decode(h + ls + 4, blen, &dec) < 0 || dec.n == 0 || pc_encode(dec.p, (int)dec.n, &enc) < 0 || enc.n != blen || idna_is_nfc(dec.p, (int)dec.n) != 1) {
                    dyn_cp_free(&dec);
                    dyn_cp_free(&enc);
                    return -1;
                }
                for (k = 0; k < dec.n; k++) {
                    if (dec.p[k] > 0x7F) {
                        int ri = dyn_tab3(idna_status_ranges,
                            countof(idna_status_ranges),
                            dec.p[k]);
                        if (ri >= 0 && idna_status_ranges[ri][2] == 3) {
                            dyn_cp_free(&dec);
                            dyn_cp_free(&enc);
                            return -1;
                        }
                    }
                }
                for (k = 0; k < blen; k++) {
                    if ((uint32_t)(uint8_t)h[ls + 4 + k] != enc.p[k]) {
                        dyn_cp_free(&dec);
                        dyn_cp_free(&enc);
                        return -1;
                    }
                }
                dyn_cp_free(&dec);
                dyn_cp_free(&enc);
            }
            ls = i + 1;
        }
    }
    return 0;
}

static int dyn_host_validate(const char* h, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t* np;
        uint32_t cp = (uint32_t)dyn_idna_utf8_next((const uint8_t*)h + i,
            (int)(n - i), &np);
        if (cp == (uint32_t)-1)
            return -1;
        if (cp > 0x7F) {
            int ri = dyn_tab3(idna_status_ranges,
                countof(idna_status_ranges), cp);
            if (ri >= 0 && idna_status_ranges[ri][2] == 3)
                return -1;
        } else if (dyn_forbidden_domain_byte((unsigned char)cp)) {
            return -1;
        }
        i = (size_t)(np - (const uint8_t*)h);
    }
    return dyn_host_alabel_roundtrip(h, n);
}

int dyn_url_normalize_host(dyn_url_t* u)
{
    const char *h = u->host, *p;
    int nonascii = 0;
    int is_file = u->scheme && !strcmp(u->scheme, "file");
    if (!h)
        return 0;
    if (h[0] == '[')
        return dyn_host_ipv6_rewrite(u);
    if (is_file && ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z')) {
        size_t hl = strlen(u->host);
        if (hl == 2 && u->host[1] != '\0' && (u->host[1] == '|' || u->host[1] == ':')) {
            char d[4];
            d[0] = '/';
            d[1] = u->host[0];
            d[2] = ':';
            d[3] = '\0';
            if (dyn_url_set_str(&u->drive, d, 3) < 0)
                return -1;
            if (dyn_url_set_str(&u->host, "", 0) < 0)
                return -1;
            return 0;
        }
    }
    if (dyn_scheme_is_special(u->scheme) && dyn_host_decode(u) < 0)
        return -1;
    h = u->host;
    for (p = h; *p; p++)
        if ((unsigned char)*p > 0x7F || (unsigned char)*p < 0x20) {
            nonascii = 1;
            break;
        }
    if (!dyn_scheme_is_special(u->scheme)) {
        if (nonascii) {
            dyn_sb_t out;
            char* nh;
            dyn_sb_init(&out);
            for (p = h; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (c >= 0x7F || c < 0x20) {
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
        return 0;
    }
    if (is_file) {
        {
            size_t hl = strlen(u->host);
            if (hl == 2 && ((u->host[0] | 0x20) >= 'a' && (u->host[0] | 0x20) <= 'z') && (u->host[1] == '|' || u->host[1] == ':')) {
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
        if (nonascii && dyn_url_host_idna(u) < 0)
            return -1;
        else
            dyn_lower(u->host);
        if (!strcmp(u->host, "localhost"))
            u->host[0] = '\0';
        return dyn_host_validate(u->host, strlen(u->host));
    }
    if (!nonascii) {
        dyn_lower(u->host);
    } else if (dyn_url_host_idna(u) < 0) {
        return -1;
    }
    if (u->host[0] && dyn_host_validate(u->host, strlen(u->host)) < 0)
        return -1;
    if (u->host[0] && dyn_host_ends_in_number(u->host))
        return dyn_host_ipv4_rewrite(u);
    return 0;
}
