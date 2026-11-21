#include "idna-tables.h"
#include "libunicode.h"

static void* dyn_dbuf_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

int idna_nfc(const uint32_t* src, int n, uint32_t** pout)
{
    return unicode_normalize(pout, src, n, UNICODE_NFC, NULL, dyn_dbuf_realloc);
}

int idna_is_nfc(const uint32_t* cps, int n)
{
    uint32_t* norm;
    int nl = idna_nfc(cps, n, &norm);
    if (nl < 0)
        return -1;
    if (nl != n) {
        free(norm);
        return 0;
    }
    if (memcmp(norm, cps, (size_t)n * sizeof(uint32_t)) != 0) {
        free(norm);
        return 0;
    }
    free(norm);
    return 1;
}

static void labs_init(dyn_idna_labs_t* l)
{
    l->a = NULL;
    l->n = l->cap = 0;
    l->oom = 0;
}

void dyn_idna_labs_free(dyn_idna_labs_t* l)
{
    free(l->a);
    l->a = NULL;
    l->n = l->cap = 0;
}

static int labs_put(dyn_idna_labs_t* l, size_t off, size_t len)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        dyn_idna_lab_t* na = (dyn_idna_lab_t*)realloc(l->a, nc * sizeof(dyn_idna_lab_t));
        if (!na) {
            l->oom = 1;
            return -1;
        }
        l->a = na;
        l->cap = nc;
    }
    l->a[l->n].off = off;
    l->a[l->n].len = len;
    l->a[l->n].decoded = 0;
    l->n++;
    return 0;
}

static void proc_err(dyn_idna_proc_t* p, const char* stage)
{
    p->nerrors++;
    if (!p->stage)
        p->stage = stage;
}

static int idna_is_greek(uint32_t cp)
{
    return cp >= 0x0370 && cp <= 0x03FF;
}

static int idna_is_hebrew(uint32_t cp)
{
    return cp >= 0x0590 && cp <= 0x05FF;
}

static int idna_is_han(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x3040 && cp <= 0x30FF)
        || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)
        || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FA1F);
}

static int idna_ctxo_ok(const uint32_t* lab, size_t len, size_t i)
{
    uint32_t cp = lab[i];
    if (cp == 0x00B7)
        return i > 0 && i + 1 < len && lab[i - 1] == 'l' && lab[i + 1] == 'l';
    if (cp == 0x0375)
        return i + 1 < len && idna_is_greek(lab[i + 1]);
    if (cp == 0x05F3 || cp == 0x05F4)
        return i > 0 && idna_is_hebrew(lab[i - 1]);
    if (cp == 0x30FB) {
        size_t k;
        for (k = 0; k < len; k++)
            if (lab[k] != 0x30FB && idna_is_han(lab[k]))
                return 1;
        return 0;
    }
    return 1;
}

static void idna_validate(dyn_idna_proc_t* p, size_t off, size_t len, int trans,
    int bidi_domain, int check_joiners)
{
    const uint32_t* lab = p->cps.p + off;
    size_t i;
    if (len == 0)
        return;
    if (len >= 4 && lab[0] == 'x' && lab[1] == 'n' && lab[2] == '-' && lab[3] == '-')
        proc_err(p, "validation");
    if (len >= 4 && lab[2] == '-' && lab[3] == '-')
        proc_err(p, "validation");
    if (lab[0] == '-' || lab[len - 1] == '-')
        proc_err(p, "validation");
    if (dyn_idna_combining(lab[0]))
        proc_err(p, "validation");
    for (i = 0; i < len; i++) {
        int st = dyn_idna_status(lab[i]);
        int ok = (st == IDNA_ST_VALID) || (st == IDNA_ST_DEVIATION && !trans);
        if (!ok)
            proc_err(p, "validation");
        if (!idna_ctxo_ok(lab, len, i))
            proc_err(p, "validation");
        if (lab[i] < 0x80 && !((lab[i] >= 'a' && lab[i] <= 'z') || (lab[i] >= '0' && lab[i] <= '9') || lab[i] == '-'))
            proc_err(p, "validation");
    }
    for (i = 0; i < len && check_joiners; i++) {
        if (lab[i] == 0x200C) {
            int ok = 0;
            if (i > 0 && dyn_idna_virama(lab[i - 1]))
                continue;
            {
                long long j = (long long)i - 1;
                while (j >= 0 && dyn_idna_jt(lab[j]) == 3)
                    j--;
                if (j >= 0 && (dyn_idna_jt(lab[j]) == 0 || dyn_idna_jt(lab[j]) == 1)) {
                    size_t k = i + 1;
                    while (k < len && dyn_idna_jt(lab[k]) == 3)
                        k++;
                    if (k < len && (dyn_idna_jt(lab[k]) == 1 || dyn_idna_jt(lab[k]) == 2))
                        ok = 1;
                }
            }
            if (!ok)
                proc_err(p, "validation");
        } else if (lab[i] == 0x200D) {
            if (!(i > 0 && dyn_idna_virama(lab[i - 1])))
                proc_err(p, "validation");
        }
    }
    if (bidi_domain) {
        int c0 = dyn_idna_bidi_class(lab[0]);
        if (!(c0 == 0 || c0 == 1 || c0 == 2)) {
            proc_err(p, "validation");
            return;
        }
        if (c0 == 1 || c0 == 2) {
            for (i = 0; i < len; i++)
                if (dyn_idna_bidi_class(lab[i]) == 0) {
                    proc_err(p, "validation");
                    break;
                }
            {
                long long k = (long long)len - 1;
                while (k >= 0 && dyn_idna_bidi_class(lab[k]) == 5)
                    k--;
                if (k < 0 || (dyn_idna_bidi_class(lab[k]) != 1 && dyn_idna_bidi_class(lab[k]) != 2 && dyn_idna_bidi_class(lab[k]) != 4 && dyn_idna_bidi_class(lab[k]) != 3))
                    proc_err(p, "validation");
            }
            {
                int has_en = 0, has_an = 0;
                for (i = 0; i < len; i++) {
                    if (dyn_idna_bidi_class(lab[i]) == 4)
                        has_en = 1;
                    if (dyn_idna_bidi_class(lab[i]) == 3)
                        has_an = 1;
                }
                if (has_en && has_an)
                    proc_err(p, "validation");
            }
        } else {
            for (i = 0; i < len; i++)
                if (dyn_idna_bidi_class(lab[i]) == 1 || dyn_idna_bidi_class(lab[i]) == 2 || dyn_idna_bidi_class(lab[i]) == 3) {
                    proc_err(p, "validation");
                    break;
                }
            {
                long long k = (long long)len - 1;
                while (k >= 0 && dyn_idna_bidi_class(lab[k]) == 5)
                    k--;
                if (k < 0 || (dyn_idna_bidi_class(lab[k]) != 0 && dyn_idna_bidi_class(lab[k]) != 4))
                    proc_err(p, "validation");
            }
        }
    }
}

dyn_idna_proc_t dyn_idna_process(const uint32_t* src, int n, int transitional,
    int mode, int strict)
{
    dyn_idna_proc_t p;
    dyn_cp_t mapped, rebuilt;
    uint32_t* norm = NULL;
    int norm_len, i;
    size_t k;
    int bidi_domain = 0;

    memset(&p, 0, sizeof(p));
    p.trans = transitional;
    dyn_cp_init(&p.cps);
    labs_init(&p.labs);
    dyn_cp_init(&mapped);
    dyn_cp_init(&rebuilt);

    for (i = 0; i < n; i++) {
        int mr = dyn_idna_map_cp(src[i], transitional, mode != 0, &mapped);
        if (mr < 0)
            goto oom;
        if (mr > 0 && strict)
            proc_err(&p, "validation");
    }
    norm_len = idna_nfc(mapped.p, (int)mapped.n, &norm);
    if (norm_len < 0) {
        proc_err(&p, "validation");
        goto done;
    }
    {
        size_t start = 0;
        int j;
        for (j = 0; j <= norm_len; j++) {
            if (j == norm_len || norm[j] == 0x2E) {
                if (labs_put(&p.labs, start, (size_t)j - start))
                    goto oom2;
                start = (size_t)j + 1;
            }
        }
        {
            size_t total = 0, ndots = 0;
            size_t j2;
            if (norm_len > 4096) {
                proc_err(&p, "validation");
                goto done;
            }
            for (j2 = 0; j2 < p.labs.n; j2++) {
                if (mode != 0 && p.labs.a[j2].len > 63)
                    proc_err(&p, "dns-length");
                total += p.labs.a[j2].len;
                if (j2 + 1 < p.labs.n)
                    ndots++;
            }
            if (p.labs.n > 0 && p.labs.a[p.labs.n - 1].len == 0)
                ndots--;
            total += ndots;
            if (mode != 0 && total > 253)
                proc_err(&p, "dns-length");
        }
    }
    for (k = 0; k < p.labs.n; k++) {
        dyn_idna_lab_t* l = &p.labs.a[k];
        const uint32_t* lab = norm + l->off;
        int is_alabel = l->len >= 4 && lab[0] == 'x' && lab[1] == 'n' && lab[2] == '-' && lab[3] == '-';
        if (!is_alabel) {
            for (i = 0; i < (int)l->len; i++)
                if (dyn_cp_put(&rebuilt, lab[i]))
                    goto oom2;
            l->off = rebuilt.n - l->len;
            continue;
        }
        {
            int nonascii = 0;
            for (i = 4; i < (int)l->len; i++)
                if (lab[i] > 0x7F) {
                    nonascii = 1;
                    break;
                }
            if (nonascii) {
                proc_err(&p, "punycode");
                for (i = 0; i < (int)l->len; i++)
                    if (dyn_cp_put(&rebuilt, lab[i]))
                        goto oom2;
                l->off = rebuilt.n - l->len;
                continue;
            }
        }
        {
            dyn_cp_t dec;
            char* bs = (char*)malloc(l->len - 3);
            size_t j, blen;
            int all_ascii = 1;
            if (!bs)
                goto oom2;
            dyn_cp_init(&dec);
            for (j = 0; j + 4 < l->len; j++)
                bs[j] = (char)lab[j + 4];
            blen = l->len - 4;
            if (pc_decode(bs, blen, &dec) < 0) {
                proc_err(&p, "punycode");
                l->len = 0;
                l->off = rebuilt.n;
                free(bs);
                dyn_cp_free(&dec);
                continue;
            }
            free(bs);
            for (j = 0; j < dec.n; j++)
                if (dec.p[j] > 0x7F) {
                    all_ascii = 0;
                    break;
                }
            if (dec.n == 0 || all_ascii)
                proc_err(&p, "punycode");
            l->decoded = 1;
            l->off = rebuilt.n;
            for (j = 0; j < dec.n; j++)
                if (dyn_cp_put(&rebuilt, dec.p[j])) {
                    dyn_cp_free(&dec);
                    goto oom2;
                }
            l->len = dec.n;
            dyn_cp_free(&dec);
        }
    }
    {
        size_t acc = 0, ri = 0;
        for (k = 0; k < p.labs.n; k++) {
            p.labs.a[k].off = acc;
            {
                size_t j;
                for (j = 0; j < p.labs.a[k].len; j++) {
                    if (dyn_cp_put(&p.cps, rebuilt.p[ri++]))
                        goto oom2;
                }
            }
            acc += p.labs.a[k].len;
            if (k + 1 < p.labs.n) {
                if (dyn_cp_put(&p.cps, 0x2E))
                    goto oom2;
                acc++;
            }
        }
    }

    if (p.nerrors)
        goto done;
    for (k = 0; k < p.labs.n; k++) {
        dyn_idna_lab_t* l = &p.labs.a[k];
        for (i = 0; i < (int)l->len; i++) {
            int bc = dyn_idna_bidi_class(p.cps.p[l->off + i]);
            if (bc == 1 || bc == 2 || bc == 3) {
                bidi_domain = 1;
                break;
            }
        }
        if (bidi_domain)
            break;
    }
    {
        size_t nempty = 0;
        for (k = 0; k < p.labs.n; k++)
            if (p.labs.a[k].len == 0)
                nempty++;
        if (nempty) {
            int trailing_only = nempty == 1 && p.labs.a[p.labs.n - 1].len == 0 && p.labs.n > 1;
            if (!trailing_only)
                proc_err(&p, "validation");
        }
    }
    for (k = 0; k < p.labs.n; k++) {
        dyn_idna_lab_t* l = &p.labs.a[k];
        const uint32_t* lab = p.cps.p + l->off;
        int is_failed_alabel = !l->decoded && l->len >= 4 && lab[0] == 'x' && lab[1] == 'n' && lab[2] == '-' && lab[3] == '-';
        if (l->len == 0 || is_failed_alabel)
            continue;
        if (l->decoded) {
            int nfc = idna_is_nfc(lab, (int)l->len);
            if (nfc < 0)
                proc_err(&p, "validation");
            else if (!nfc)
                proc_err(&p, "validation");
            idna_validate(&p, l->off, l->len, 0, bidi_domain, 1);
        } else {
            idna_validate(&p, l->off, l->len, transitional, bidi_domain, 1);
        }
    }
done:
    dyn_cp_free(&mapped);
    dyn_cp_free(&rebuilt);
    free(norm);
    return p;
oom2:
    dyn_cp_free(&rebuilt);
oom:
    p.oom = 1;
    proc_err(&p, "validation");
    free(norm);
    dyn_cp_free(&mapped);
    return p;
}

static int dyn_idna_rfc5892_disallowed(uint32_t cp)
{
    return cp == 0x0640 || cp == 0x07FA || cp == 0x302E || cp == 0x302F || (cp >= 0x3031 && cp <= 0x3035) || cp == 0x303B;
}

static int dyn_idna_rfc5892_shape(uint32_t cp)
{
    if (cp < 0x80 || lre_is_id_continue(cp))
        return 1;
    return cp == 0x00B7 || cp == 0x0375 || cp == 0x05F3 || cp == 0x05F4 || cp == 0x30FB || cp == 0x06FD || cp == 0x06FE
        || cp == 0x0F0B || cp == 0x3007 || cp == 0x200C || cp == 0x200D || (cp >= 0x0660 && cp <= 0x0669)
        || (cp >= 0x06F0 && cp <= 0x06F9);
}

static int dyn_idna_alabel_ok(const uint32_t* in, size_t len, const uint32_t* dec, size_t dlen)
{
    dyn_cp_t enc;
    size_t i;
    int ok = 1;
    for (i = 0; i < dlen; i++)
        if (dyn_idna_status(dec[i]) != IDNA_ST_VALID && dyn_idna_status(dec[i]) != IDNA_ST_DEVIATION)
            return 0;
    dyn_cp_init(&enc);
    if (pc_encode(dec, (int)dlen, &enc) < 0 || enc.n + 4 != len)
        ok = 0;
    for (i = 0; ok && i < enc.n; i++) {
        uint32_t a = in[i + 4];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (enc.p[i] != a)
            ok = 0;
    }
    dyn_cp_free(&enc);
    return ok;
}

int dyn_idna_hostname_ok(const char* s, size_t n)
{
    dyn_cp_t in;
    dyn_idna_proc_t p;
    size_t i = 0, k, pos = 0, total = 0;
    int ok = 1, wide = 0;

    if (n == 0 || n > 1024)
        return 0;
    dyn_cp_init(&in);
    while (i < n) {
        const unsigned char* u = (const unsigned char*)s;
        uint32_t cp;
        size_t need, j;
        if (u[i] < 0x80) {
            cp = u[i];
            need = 0;
        } else if ((u[i] & 0xE0) == 0xC0 && u[i] >= 0xC2) {
            cp = u[i] & 0x1F;
            need = 1;
        } else if ((u[i] & 0xF0) == 0xE0) {
            cp = u[i] & 0x0F;
            need = 2;
        } else if ((u[i] & 0xF8) == 0xF0 && u[i] <= 0xF4) {
            cp = u[i] & 0x07;
            need = 3;
        } else {
            dyn_cp_free(&in);
            return 0;
        }
        for (j = 1; j <= need; j++) {
            if (i + j >= n || (u[i + j] & 0xC0) != 0x80) {
                dyn_cp_free(&in);
                return 0;
            }
            cp = (cp << 6) | (u[i + j] & 0x3F);
        }
        if ((need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF))
            || dyn_idna_rfc5892_disallowed(cp) || dyn_cp_put(&in, cp)) {
            dyn_cp_free(&in);
            return 0;
        }
        i += need + 1;
    }
    p = dyn_idna_process(in.p, (int)in.n, 0, 1, 1);
    if (p.oom || p.nerrors)
        ok = 0;
    for (k = 0; ok && k < p.labs.n; k++) {
        dyn_idna_lab_t* l = &p.labs.a[k];
        size_t end = pos, m;
        while (end < in.n && in.p[end] != 0x2E && in.p[end] != 0x3002 && in.p[end] != 0xFF0E && in.p[end] != 0xFF61)
            end++;
        if (l->len == 0)
            ok = 0;
        for (m = 0; ok && m < l->len; m++) {
            uint32_t c2 = p.cps.p[l->off + m];
            if (dyn_idna_rfc5892_disallowed(c2) || !dyn_idna_rfc5892_shape(c2))
                ok = 0;
            if (c2 > 0x7F)
                wide = 1;
        }
        if (ok && wide) {
            dyn_cp_t enc;
            dyn_cp_init(&enc);
            if (pc_encode(p.cps.p + l->off, (int)l->len, &enc) < 0 || enc.n + 4 > 63)
                ok = 0;
            total += enc.n + 4;
            dyn_cp_free(&enc);
        } else {
            total += l->len;
        }
        wide = 0;
        if (ok && l->decoded && !dyn_idna_alabel_ok(in.p + pos, end - pos, p.cps.p + l->off, l->len))
            ok = 0;
        pos = end + 1;
    }
    if (ok && total + (p.labs.n ? p.labs.n - 1 : 0) > 253)
        ok = 0;
    dyn_idna_labs_free(&p.labs);
    dyn_cp_free(&p.cps);
    dyn_cp_free(&in);
    return ok;
}

void dyn_idna_to_ascii(dyn_idna_proc_t* p, const uint32_t* src, int n, int transitional,
    int strict, dyn_cp_t* out_ascii)
{
    dyn_idna_proc_t proc = dyn_idna_process(src, n, transitional, transitional ? 2 : 1, strict);
    size_t k;
    *p = proc;
    if (proc.nerrors)
        return;
    for (k = 0; k < proc.labs.n; k++) {
        dyn_idna_lab_t* l = &proc.labs.a[k];
        int has_nonascii = 0, i;
        size_t start = out_ascii->n;
        for (i = 0; i < (int)l->len; i++)
            if (proc.cps.p[l->off + i] > 0x7F) {
                has_nonascii = 1;
                break;
            }
        if (!has_nonascii) {
            for (i = 0; i < (int)l->len; i++)
                if (dyn_cp_put(out_ascii, proc.cps.p[l->off + i]))
                    goto oom;
            if (k + 1 < proc.labs.n)
                if (dyn_cp_put(out_ascii, 0x2E))
                    goto oom;
            (void)start;
            continue;
        }
        if (dyn_cp_put(out_ascii, 'x'))
            goto oom;
        if (dyn_cp_put(out_ascii, 'n'))
            goto oom;
        if (dyn_cp_put(out_ascii, '-'))
            goto oom;
        if (dyn_cp_put(out_ascii, '-'))
            goto oom;
        {
            dyn_cp_t lab;
            dyn_cp_init(&lab);
            for (i = 0; i < (int)l->len; i++) {
                uint32_t cp = proc.cps.p[l->off + i];
                if (dyn_idna_status(cp) == IDNA_ST_DISALLOWED)
                    continue;
                if (dyn_cp_put(&lab, cp)) {
                    dyn_cp_free(&lab);
                    goto oom;
                }
            }
            if (pc_encode(lab.p, (int)lab.n, out_ascii) < 0)
                proc_err(p, "punycode");
            dyn_cp_free(&lab);
        }
        if (k + 1 < proc.labs.n)
            if (dyn_cp_put(out_ascii, 0x2E))
                goto oom;
    }
    {
        size_t total = 0, lablen = 0, ndots = 0, nlabels = 0;
        size_t i;
        for (i = 0; i <= out_ascii->n; i++) {
            if (i == out_ascii->n || out_ascii->p[i] == 0x2E) {
                if (i == out_ascii->n && lablen == 0 && i > 0
                    && out_ascii->p[i - 1] == 0x2E)
                    continue;
                if (lablen == 0)
                    proc_err(p, "dns-length");
                else if (lablen > 63)
                    proc_err(p, "dns-length");
                total += lablen;
                nlabels++;
                if (i < out_ascii->n)
                    ndots++;
                lablen = 0;
            } else {
                lablen++;
            }
        }
        if (out_ascii->n > 0 && out_ascii->p[out_ascii->n - 1] == 0x2E)
            ndots--;
        total += ndots;
        if (total == 0)
            proc_err(p, "dns-length");
        if (total > 253)
            proc_err(p, "dns-length");
        (void)nlabels;
    }
    return;
oom:
    p->oom = 1;
    proc_err(p, "validation");
}

int dyn_idna_ascii_fast_check(const char* s, size_t len)
{
    size_t i, lablen = 0, total = 0, ndots = 0;
    if (len == 0 || len > 254)
        return 0;
    for (i = 0; i <= len; i++) {
        char c = (i < len) ? s[i] : '.';
        if (c == '.') {
            if (i == len && i > 0 && s[i - 1] == '.')
                break;
            if (lablen == 0 || lablen > 63)
                return 0;
            if (s[i - lablen] == '-' || s[i - 1] == '-')
                return 0;
            if (lablen >= 4 && s[i - lablen + 2] == '-' && s[i - lablen + 3] == '-')
                return 0;
            total += lablen;
            ndots++;
            lablen = 0;
        } else {
            unsigned char uc = (unsigned char)c;
            if (uc >= 0x80)
                return 0;
            if (!((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '-'))
                return 0;
            lablen++;
        }
    }
    if (len > 0 && s[len - 1] == '.')
        ndots--;
    total += ndots;
    if (total == 0 || total > 253)
        return 0;
    return 1;
}
