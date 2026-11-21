#include "arck.h"

typedef struct {
    uint64_t lo, hi;
} zip_span_t;

int arck_ar_name_safe(const char* s, size_t n)
{
    size_t i = 0;

    if (n == 0 || n > AR_MAX_NAME)
        return 0;
    if (s[0] == '/' || s[0] == '\\')
        return 0;
    if (n >= 2 && s[1] == ':')
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == 0 || s[i] == '\\')
            return 0;
        if (s[i] == '.' && i + 1 < n && s[i + 1] == '.'
            && (i == 0 || s[i - 1] == '/')
            && (i + 2 >= n || s[i + 2] == '/'))
            return 0;
    }
    return 1;
}

void arck_ab_init(ab_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

void arck_ab_free(ab_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void arck_ab_write(ab_t* b, const void* p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
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

void arck_ab_zero(ab_t* b, size_t n)
{
    static const uint8_t z[TAR_BLOCK] = { 0 };
    while (n) {
        size_t k = n > TAR_BLOCK ? TAR_BLOCK : n;
        arck_ab_write(b, z, k);
        n -= k;
    }
}

void arck_ab_u16(ab_t* b, uint32_t v)
{
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    arck_ab_write(b, t, 2);
}

void arck_ab_u32(ab_t* b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
        (uint8_t)(v >> 24) };
    arck_ab_write(b, t, 4);
}

static uint32_t ar_rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static int tar_octal(const uint8_t* p, size_t n, uint64_t* out)
{
    size_t i = 0;
    uint64_t v = 0;
    int seen = 0;

    while (i < n && (p[i] == ' ' || p[i] == 0))
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > (UINT64_MAX - 7) / 8)
            return -1;
        v = v * 8 + (uint64_t)(p[i] - '0');
        seen = 1;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0))
        i++;
    if (!seen || i != n)
        return -1;
    *out = v;
    return 0;
}

void arck_tar_put_octal(uint8_t* dst, size_t n, uint64_t v)
{
    size_t i = n - 1;
    dst[i--] = 0;
    while (i < n) {
        dst[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
        if (i == 0)
            break;
        i--;
    }
}

uint32_t arck_tar_checksum(const uint8_t* h)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i < 148; i++)
        s += h[i];
    s += 8u * (uint32_t)' ';
    for (i = 156; i < TAR_BLOCK; i++)
        s += h[i];
    return s;
}

static int tar_is_zero_block(const uint8_t* h)
{
    int i;
    for (i = 0; i < TAR_BLOCK; i++)
        if (h[i])
            return 0;
    return 1;
}

static size_t tar_pax_one(const uint8_t* p, size_t avail, tar_ent_t* e)
{
    size_t j = 0, len = 0, eq, klen, vlen;

    while (j < avail && p[j] >= '0' && p[j] <= '9') {
        if (len > (SIZE_MAX - 9) / 10)
            return 0;
        len = len * 10 + (size_t)(p[j++] - '0');
    }
    if (len < 4 || len > avail || j >= avail || p[j] != ' ')
        return 0;
    j++;
    for (eq = j; eq < len && p[eq] != '='; eq++)
        ;
    if (eq >= len)
        return len;
    if (eq + 2 > len)
        return len;
    klen = eq - j;
    vlen = len - eq - 2;
    if (klen == 4 && memcmp(p + j, "path", 4) == 0 && vlen <= AR_MAX_NAME) {
        memcpy(e->name, p + eq + 1, vlen);
        e->name[vlen] = 0;
        e->nlen = vlen;
    } else if (klen == 8 && memcmp(p + j, "linkpath", 8) == 0
        && vlen <= AR_MAX_NAME) {
        memcpy(e->link, p + eq + 1, vlen);
        e->link[vlen] = 0;
        e->linklen = vlen;
    } else if (klen == 4 && memcmp(p + j, "size", 4) == 0) {
        uint64_t v = 0;
        size_t k;
        for (k = 0; k < vlen; k++)
            if (p[eq + 1 + k] >= '0' && p[eq + 1 + k] <= '9') {
                if (v > (UINT64_MAX - 9) / 10) {
                    v = UINT64_MAX;
                    break;
                }
                v = v * 10 + (uint64_t)(p[eq + 1 + k] - '0');
            }
        e->size = v;
    }
    return len;
}

static void tar_pax(const uint8_t* p, size_t n, tar_ent_t* e)
{
    size_t i = 0;

    while (i < n) {
        size_t len = tar_pax_one(p + i, n - i, e);
        if (!len)
            return;
        i += len;
    }
}

static void tar_header_name(const uint8_t* h, tar_ent_t* e)
{
    size_t plen = 0, nlen = 0;

    while (nlen < 100 && h[nlen])
        nlen++;
    if (h[345]) {
        while (plen < 155 && h[345 + plen])
            plen++;
        memcpy(e->name, h + 345, plen);
        e->name[plen] = '/';
        plen++;
    }
    memcpy(e->name + plen, h, nlen);
    e->nlen = plen + nlen;
    e->name[e->nlen] = 0;
}

static int tar_header_nums(const uint8_t* h, uint64_t* size, uint64_t* mtime,
    uint64_t* mode)
{
    uint64_t sum = 0;

    if (tar_octal(h + 148, 8, &sum) < 0 || (uint32_t)sum != arck_tar_checksum(h))
        return -1;
    if (tar_octal(h + 124, 12, size) < 0)
        return -2;
    tar_octal(h + 136, 12, mtime);
    tar_octal(h + 100, 8, mode);
    return 0;
}

int arck_tar_next(tar_rd_t* r, tar_ent_t* e)
{
    for (;;) {
        const uint8_t* h;
        uint64_t size = 0, mtime = 0, mode = 0;
        int rc;

        if (r->i + TAR_BLOCK > r->n) {
            if (r->i == r->n)
                return 0;
            snprintf(r->err, sizeof r->err, "truncated header at %u",
                (unsigned)r->i);
            return -1;
        }
        h = r->p + r->i;
        if (tar_is_zero_block(h))
            return 0;
        rc = tar_header_nums(h, &size, &mtime, &mode);
        if (rc < 0) {
            snprintf(r->err, sizeof r->err, rc == -1 ? "header checksum mismatch at block %u" : "unreadable size field at block %u",
                (unsigned)(r->i / TAR_BLOCK));
            return -1;
        }
        r->i += TAR_BLOCK;
        if (size > r->n - r->i) {
            snprintf(r->err, sizeof r->err,
                "entry declares %llu bytes with %u left in the archive",
                (unsigned long long)size, (unsigned)(r->n - r->i));
            return -1;
        }
        e->type = (char)h[156];
        if (e->type == 'L' || e->type == 'x' || e->type == 'X'
            || e->type == 'g' || e->type == 'K') {
            if ((e->type == 'L' || e->type == 'K') && size > AR_MAX_NAME + 1u) {
                snprintf(r->err, sizeof r->err,
                    "a long-name record exceeds %u bytes", AR_MAX_NAME);
                return -1;
            }
            if (e->type == 'L') {
                size_t nlen = size > AR_MAX_NAME ? AR_MAX_NAME : (size_t)size;
                memcpy(e->name, r->p + r->i, nlen);
                while (nlen && e->name[nlen - 1] == 0)
                    nlen--;
                e->name[nlen] = 0;
                e->nlen = nlen;
            } else if (e->type == 'K') {
                if (e->linklen == 0) {
                    size_t klen = size > AR_MAX_NAME ? AR_MAX_NAME : (size_t)size;
                    memcpy(e->link, r->p + r->i, klen);
                    while (klen && e->link[klen - 1] == 0)
                        klen--;
                    e->link[klen] = 0;
                    e->linklen = klen;
                }
            } else if (e->type != 'x' && e->type != 'X') {
                r->i += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
                continue;
            } else {
                tar_pax(r->p + r->i, (size_t)size, e);
            }
            r->i += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            continue;
        }
        if (e->nlen == 0)
            tar_header_name(h, e);
        if (e->linklen == 0) {
            size_t k = 0;
            while (k < 100 && h[157 + k])
                k++;
            memcpy(e->link, h + 157, k);
            e->link[k] = 0;
            e->linklen = k;
        }
        if (e->size == 0)
            e->size = size;
        e->mtime = mtime;
        e->mode = mode;
        e->body = r->i;
        if (e->size > r->n - e->body) {
            snprintf(r->err, sizeof r->err,
                "entry declares %llu bytes with %u left in the archive",
                (unsigned long long)e->size, (unsigned)(r->n - e->body));
            return -1;
        }
        {
            uint64_t adv = e->size != size ? e->size : size;
            r->i += (size_t)((adv + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK);
        }
        if (!r->allow_unsafe && !arck_ar_name_safe(e->name, e->nlen)) {
            snprintf(r->err, sizeof r->err,
                "entry name is not safe to write anywhere: %.80s", e->name);
            return -1;
        }
        if (!r->allow_unsafe && e->linklen
            && !arck_ar_name_safe(e->link, e->linklen)) {
            snprintf(r->err, sizeof r->err,
                "link target is not safe to write anywhere: %.80s",
                e->link);
            return -1;
        }
        return 1;
    }
}

static int zip_inflate_raw(const uint8_t* src, size_t n, uint32_t crc,
    uint32_t usize, dyn_outbuf_t* out)
{
    static const uint8_t GZHDR[10] = { 0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff };
    uint8_t* tmp;
    int rc;

    if (n > SIZE_MAX - 18)
        return -1;
    tmp = (uint8_t*)malloc(n + 18);
    if (!tmp)
        return -1;
    memcpy(tmp, GZHDR, 10);
    memcpy(tmp + 10, src, n);
    tmp[10 + n] = (uint8_t)crc;
    tmp[11 + n] = (uint8_t)(crc >> 8);
    tmp[12 + n] = (uint8_t)(crc >> 16);
    tmp[13 + n] = (uint8_t)(crc >> 24);
    tmp[14 + n] = (uint8_t)usize;
    tmp[15 + n] = (uint8_t)(usize >> 8);
    tmp[16 + n] = (uint8_t)(usize >> 16);
    tmp[17 + n] = (uint8_t)(usize >> 24);
    rc = dyn_gunzip_decode(tmp, n + 18, out);
    free(tmp);
    return rc;
}

int arck_zip_deflate_raw(const uint8_t* src, size_t n, uint8_t** out,
    size_t* out_len)
{
    uint8_t* gz = NULL;
    size_t gn = 0;

    if (dyn_gzip_build(src, n, &gz, &gn) != 0)
        return -1;
    if (gn < 18) {
        free(gz);
        return -1;
    }
    memmove(gz, gz + 10, gn - 18);
    *out = gz;
    *out_len = gn - 18;
    return 0;
}

uint32_t arck_zip_dos_from_unix(int64_t t)
{
    int64_t days, secs, y, m, d, hh, mm, ss;
    int64_t z, era, doe, yoe, mp;
    uint32_t dos_date, dos_time;
    int yy;

    if (t <= 0)
        return 0;
    if (t < 315532800LL)
        t = 315532800LL;
    if (t > 4354819198LL)
        t = 4354819198LL;
    days = t / 86400;
    secs = t % 86400;
    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doe -= (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doe + 2) / 153;
    d = doe - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    hh = secs / 3600;
    mm = (secs % 3600) / 60;
    ss = secs % 60;
    yy = (int)(y - 1980);
    if (yy < 0)
        yy = 0;
    if (yy > 127)
        yy = 127;
    dos_date = (uint32_t)(((yy & 127) << 9) | ((m & 15) << 5) | (d & 31));
    dos_time = (uint32_t)(((hh & 31) << 11) | ((mm & 63) << 5) | ((ss / 2) & 31));
    return (dos_date << 16) | dos_time;
}

int64_t arck_zip_unix_from_dos(uint32_t dos)
{
    int yy, mm, dd, hh, mi, ss;
    int64_t y, m, days, z, era, doe, yoe, mp2;
    if (!dos)
        return 0;
    ss = (int)((dos & 31) * 2);
    mi = (int)((dos >> 5) & 63);
    hh = (int)((dos >> 11) & 31);
    dd = (int)((dos >> 16) & 31);
    mm = (int)((dos >> 21) & 15);
    yy = (int)(((dos >> 25) & 127) + 1980);
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 60)
        return 0;
    y = yy;
    m = mm;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    mp2 = m + (m > 2 ? -3 : 9);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + (153 * mp2 + 2) / 5 + dd - 1;
    z = doe + era * 146097 - 719468;
    days = z;
    return days * 86400 + hh * 3600 + mi * 60 + ss;
}

static long zip_find_eocd(const uint8_t* p, size_t n)
{
    size_t back = n < 65557 ? n : 65557, i;

    if (n < 22)
        return -1;
    for (i = n - 22 + 1; i-- > n - back;) {
        if (dyn_ld32le(p + i) == 0x06054b50)
            return (long)i;
        if (i == 0)
            break;
    }
    return -1;
}

int arck_zip_open(zip_rd_t* z)
{
    long e = zip_find_eocd(z->p, z->n);

    if (e < 0) {
        snprintf(z->err, sizeof z->err, "no end-of-central-directory record");
        return -1;
    }
    z->count = ar_rd16(z->p + e + 10);
    z->cd_size = dyn_ld32le(z->p + e + 12);
    z->cd_off = dyn_ld32le(z->p + e + 16);
    if (z->count == 0xFFFF || z->cd_size == 0xFFFFFFFF || z->cd_off == 0xFFFFFFFF) {
        snprintf(z->err, sizeof z->err,
            "ZIP64 archives are not supported");
        return -1;
    }
    if (z->count > AR_MAX_ENTRIES) {
        snprintf(z->err, sizeof z->err, "archive declares %u entries", z->count);
        return -1;
    }
    if (z->cd_off > z->n || z->cd_size > z->n - z->cd_off) {
        snprintf(z->err, sizeof z->err,
            "the central directory lies outside the archive");
        return -1;
    }
    return 0;
}

int arck_zip_entry_at(zip_rd_t* z, size_t* off, zip_ent_t* e)
{
    const uint8_t* p;
    uint32_t nlen, xlen, clen;
    uint32_t ext;

    if (*off + 46 > z->cd_off + z->cd_size)
        return 0;
    p = z->p + *off;
    if (dyn_ld32le(p) != 0x02014b50) {
        snprintf(z->err, sizeof z->err, "bad central directory signature");
        return -1;
    }
    e->method = ar_rd16(p + 10);
    e->mtime = dyn_ld32le(p + 12);
    e->crc = dyn_ld32le(p + 16);
    e->csize = dyn_ld32le(p + 20);
    e->usize = dyn_ld32le(p + 24);
    nlen = ar_rd16(p + 28);
    xlen = ar_rd16(p + 30);
    clen = ar_rd16(p + 32);
    ext = dyn_ld32le(p + 38);
    e->mode = (ext >> 16) & 0xFFFFu;
    e->local = dyn_ld32le(p + 42);
    if (*off + 46 + nlen + xlen + clen > z->cd_off + z->cd_size) {
        snprintf(z->err, sizeof z->err, "central directory entry overruns");
        return -1;
    }
    if (nlen > AR_MAX_NAME) {
        snprintf(z->err, sizeof z->err, "entry name exceeds %u bytes", AR_MAX_NAME);
        return -1;
    }
    {
        uint32_t stored = clen > 1024 ? 1024 : clen;
        memcpy(e->name, p + 46, nlen);
        e->name[nlen] = 0;
        e->nlen = nlen;
        if (stored) {
            memcpy(e->comment, p + 46 + nlen + xlen, stored);
            e->comment[stored] = 0;
        } else {
            e->comment[0] = 0;
        }
        e->clen = stored;
    }
    if (!z->allow_unsafe && !arck_ar_name_safe(e->name, e->nlen)) {
        snprintf(z->err, sizeof z->err,
            "entry name is not safe to write anywhere: %.80s", e->name);
        return -1;
    }
    *off += 46 + nlen + xlen + clen;
    return 1;
}

static int zip_span_cmp(const void* a, const void* b)
{
    const zip_span_t* x = (const zip_span_t*)a;
    const zip_span_t* y = (const zip_span_t*)b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

int arck_zip_preflight(zip_rd_t* z)
{
    size_t off = z->cd_off;
    zip_span_t* spans;
    zip_ent_t* e;
    uint64_t total = 0;
    uint32_t k = 0, i;
    int rc = -1;

    spans = (zip_span_t*)malloc((z->count ? z->count : 1) * sizeof(*spans));
    e = (zip_ent_t*)malloc(sizeof(*e));
    if (!spans || !e) {
        snprintf(z->err, sizeof z->err, "out of memory");
        goto done;
    }
    for (;;) {
        const uint8_t* p;
        uint64_t data;
        int r = arck_zip_entry_at(z, &off, e);
        if (r < 0)
            goto done;
        if (r == 0)
            break;
        if (k >= z->count) {
            snprintf(z->err, sizeof z->err,
                "the central directory holds more than the %u entries the "
                "archive declares",
                z->count);
            goto done;
        }
        if (e->method != 0 && e->method != 8) {
            snprintf(z->err, sizeof z->err,
                "member %.60s uses compression method %u, and this reader "
                "has store and deflate",
                e->name, e->method);
            goto done;
        }
        if (e->usize > AR_MAX_MEMBER) {
            snprintf(z->err, sizeof z->err, "member exceeds the size limit");
            goto done;
        }
        total += e->usize;
        if (total > AR_MAX_TOTAL_OUT) {
            snprintf(z->err, sizeof z->err, "aggregate output exceeds 1 GiB");
            goto done;
        }
        if ((uint64_t)e->local + 30 > (uint64_t)z->n
            || dyn_ld32le(z->p + e->local) != 0x04034b50) {
            snprintf(z->err, sizeof z->err, "bad local header for %.60s",
                e->name);
            goto done;
        }
        p = z->p + e->local;
        data = (uint64_t)e->local + 30 + ar_rd16(p + 26) + ar_rd16(p + 28);
        if (data > z->n || e->csize > z->n - data) {
            snprintf(z->err, sizeof z->err,
                "member data lies outside the archive");
            goto done;
        }
        if (ar_rd16(p + 26) != e->nlen
            || memcmp(p + 30, e->name, e->nlen) != 0) {
            snprintf(z->err, sizeof z->err,
                "the local header names a different file than the directory");
            goto done;
        }
        spans[k].lo = e->local;
        spans[k].hi = data + e->csize;
        k++;
    }
    if (k != z->count) {
        snprintf(z->err, sizeof z->err,
            "the archive declares %u entries and its directory holds %u",
            z->count, k);
        goto done;
    }
    qsort(spans, k, sizeof(*spans), zip_span_cmp);
    for (i = 1; i < k; i++) {
        if (spans[i].lo < spans[i - 1].hi) {
            snprintf(z->err, sizeof z->err, "two members share archive bytes");
            goto done;
        }
    }
    rc = 0;
done:
    free(spans);
    free(e);
    return rc;
}

int arck_zip_member(zip_rd_t* z, const zip_ent_t* e, dyn_outbuf_t* out)
{
    const uint8_t* p;
    uint32_t nlen, xlen;
    size_t data;

    if ((uint64_t)e->local + 30 > (uint64_t)z->n
        || dyn_ld32le(z->p + e->local) != 0x04034b50) {
        snprintf(z->err, sizeof z->err, "bad local header for %.60s", e->name);
        return -1;
    }
    p = z->p + e->local;
    nlen = ar_rd16(p + 26);
    xlen = ar_rd16(p + 28);
    data = e->local + 30 + nlen + xlen;
    if (data > z->n || e->csize > z->n - data) {
        snprintf(z->err, sizeof z->err, "member data lies outside the archive");
        return -1;
    }
    if (nlen != e->nlen || memcmp(p + 30, e->name, nlen) != 0) {
        snprintf(z->err, sizeof z->err,
            "the local header names a different file than the directory");
        return -1;
    }
    if (e->usize > AR_MAX_MEMBER) {
        snprintf(z->err, sizeof z->err, "member exceeds the size limit");
        return -1;
    }
    if (e->method == 0) {
        if (e->csize != e->usize) {
            snprintf(z->err, sizeof z->err, "a stored member's sizes disagree");
            return -1;
        }
        out->buf = (uint8_t*)malloc(e->usize ? e->usize : 1);
        if (!out->buf)
            return -1;
        memcpy(out->buf, z->p + data, e->usize);
        out->len = out->cap = e->usize;
    } else if (e->method == 8) {
        out->limit = e->usize ? e->usize : 1;
        if (zip_inflate_raw(z->p + data, e->csize, e->crc, e->usize, out) != 0) {
            snprintf(z->err, sizeof z->err, "member %.60s does not inflate",
                e->name);
            return -1;
        }
    } else {
        snprintf(z->err, sizeof z->err,
            "member %.60s uses compression method %u, and this reader has "
            "store and deflate",
            e->name, e->method);
        return -1;
    }
    if (out->len != e->usize) {
        snprintf(z->err, sizeof z->err, "member %.60s decompressed to %u bytes, "
                                        "not the %u the directory declares",
            e->name,
            (unsigned)out->len, e->usize);
        return -1;
    }
    if (dyn_crc32(out->buf, out->len) != e->crc) {
        snprintf(z->err, sizeof z->err, "member %.60s fails its CRC", e->name);
        return -1;
    }
    return 0;
}
