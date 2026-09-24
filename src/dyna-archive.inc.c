/* tar and zip for dyna:compress: container formats over codecs the module
   already owns. NAMES ARE VALIDATED AT THE PARSE BOUNDARY, not at the write
   boundary -- tar-slip and zip-slip are the same bug. Exactly what that
   enforces: an entry name is refused at parse time unless it is relative and
   free of `..` segments, NULs, backslashes and drive letters, so a writer
   under a destination directory starts from a name that cannot name the
   directory's parent. {allowUnsafeNames:true} lifts that name gate and is
   documented as an opt-in escape -- such names then write OUTSIDE the
   destination and the caller owns the consequences. Writing a name to disk
   (ZipExtractAll) adds its own enforcement on top, stated on ar_write_member:
   under the destination directory no symlink is ever traversed or written
   through. Full API: see the module header. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define AR_MAX_ENTRIES  100000u
/* The zip EOCD records the entry count in a 16-bit field and no zip64 is
   written, so a larger count would wrap into a silently corrupt archive. */
#define ZIP_MAX_ENTRIES 65535u
#define AR_MAX_NAME     4096u
#define AR_MAX_MEMBER   (1u << 30)      /* one member, uncompressed */
#define TAR_BLOCK       512

/* -------------------------------------------------------------- name safety */

/* An archive name that a caller can safely join to a directory: relative, no
   `..` segment, no NUL, no backslash, no drive letter. Refusing at the PARSE
   boundary means every consumer inherits the check. */
static int ar_name_safe(const char *s, size_t n)
{
    size_t i = 0;

    if (n == 0 || n > AR_MAX_NAME)
        return 0;
    if (s[0] == '/' || s[0] == '\\')
        return 0;
    if (n >= 2 && s[1] == ':')
        return 0;                       /* a Windows drive letter */
    for (i = 0; i < n; i++) {
        if (s[i] == 0 || s[i] == '\\')
            return 0;
        if (s[i] == '.' && s[i + 1 <= n - 1 ? i + 1 : i] == '.'
            && (i == 0 || s[i - 1] == '/')
            && (i + 2 >= n || s[i + 2] == '/'))
            return 0;                   /* a `..` path segment */
    }
    return 1;
}

typedef struct { uint8_t *p; size_t n, cap; int oom; } ab_t;

static void ab_init(ab_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
static void ab_free(ab_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void ab_write(ab_t *b, const void *p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        uint8_t *np;
        while (nc < b->n + n) {
            if (nc < (1u << 20)) nc *= 2;
            else                 nc += nc / 4;
        }
        np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void ab_zero(ab_t *b, size_t n)
{
    static const uint8_t z[TAR_BLOCK] = { 0 };
    while (n) {
        size_t k = n > TAR_BLOCK ? TAR_BLOCK : n;
        ab_write(b, z, k);
        n -= k;
    }
}

static void ab_u16(ab_t *b, uint32_t v)
{
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    ab_write(b, t, 2);
}

static void ab_u32(ab_t *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    ab_write(b, t, 4);
}

static uint32_t ar_rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t ar_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------- tar */

/* Octal ASCII, space- or NUL-terminated. A field that is not octal is a
   malformed header, not a zero. */
static int tar_octal(const uint8_t *p, size_t n, uint64_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    int seen = 0;

    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > (UINT64_MAX - 7) / 8)
            return -1;
        v = v * 8 + (uint64_t)(p[i] - '0');
        seen = 1;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    if (!seen || i != n)
        return -1;
    *out = v;
    return 0;
}

static void tar_put_octal(uint8_t *dst, size_t n, uint64_t v)
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

/* The header checksum is computed with the checksum field read as spaces --
   which is why it can be stored in the header it covers. */
static uint32_t tar_checksum(const uint8_t *h)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i < TAR_BLOCK; i++)
        s += (i >= 148 && i < 156) ? ' ' : h[i];
    return s;
}

static int tar_is_zero_block(const uint8_t *h)
{
    int i;
    for (i = 0; i < TAR_BLOCK; i++)
        if (h[i])
            return 0;
    return 1;
}

typedef struct {
    char     name[AR_MAX_NAME + 1];
    size_t   nlen;
    uint64_t size, mtime, mode;
    char     type;
    char     link[AR_MAX_NAME + 1];     /* header (100B) or PAX linkpath */
    size_t   linklen;                   /* 0 when no linkname */
    size_t   body;                      /* offset of the body in the archive */
} tar_ent_t;

/* A PAX record block: "<len> <key>=<value>\n" repeated. Only `path` and `size`
   change what an entry IS; the rest is metadata this reader does not carry. */
/* One "<len> <key>=<value>\n" record. Returns its length, or 0 if malformed. */
static size_t tar_pax_one(const uint8_t *p, size_t avail, tar_ent_t *e)
{
    size_t j = 0, len = 0, eq, klen, vlen;

    while (j < avail && p[j] >= '0' && p[j] <= '9') {
        if (len > (SIZE_MAX - 9) / 10)
            return 0;                   /* a digit run that wraps is malformed */
        len = len * 10 + (size_t)(p[j++] - '0');
    }
    if (len < 4 || len > avail || j >= avail || p[j] != ' ')
        return 0;
    j++;
    for (eq = j; eq < len && p[eq] != '='; eq++)
        ;
    if (eq >= len)
        return len;                     /* no `=`: metadata this reader skips */
    if (eq + 2 > len)
        return len;                     /* `=` is the last byte: the value span
                                           (len - eq - 2) would wrap to SIZE_MAX
                                           and the size scan below would read
                                           past the archive buffer (a record is
                                           "%u key=value\n", so a well-formed
                                           record has at least \n after '=') */
    klen = eq - j;
    vlen = len - eq - 2;
    if (klen == 4 && memcmp(p + j, "path", 4) == 0 && vlen <= AR_MAX_NAME) {
        memcpy(e->name, p + eq + 1, vlen);
        e->name[vlen] = 0;
        e->nlen = vlen;
    } else if (klen == 8 && memcmp(p + j, "linkpath", 8) == 0
               && vlen <= AR_MAX_NAME) {
        /* Same as `path`: a PAX linkpath overrides the header field (audit
           13.8.2); validated below at the parse boundary like every name. */
        memcpy(e->link, p + eq + 1, vlen);
        e->link[vlen] = 0;
        e->linklen = vlen;
    } else if (klen == 4 && memcmp(p + j, "size", 4) == 0) {
        uint64_t v = 0;
        size_t k;
        for (k = 0; k < vlen; k++)
            if (p[eq + 1 + k] >= '0' && p[eq + 1 + k] <= '9') {
                /* A size wrapping uint64_t is certainly past the archive:
                   saturate; the bounds check in tar_next refuses it. */
                if (v > (UINT64_MAX - 9) / 10) { v = UINT64_MAX; break; }
                v = v * 10 + (uint64_t)(p[eq + 1 + k] - '0');
            }
        e->size = v;
    }
    return len;
}

/* A PAX record block. Only `path` and `size` change what an entry IS; the rest
   is metadata this reader does not carry. */
static void tar_pax(const uint8_t *p, size_t n, tar_ent_t *e)
{
    size_t i = 0;

    while (i < n) {
        size_t len = tar_pax_one(p + i, n - i, e);
        if (!len)
            return;
        i += len;
    }
}

typedef struct {
    JSContext     *ctx;
    const uint8_t *p;
    size_t         n, i;
    int            allow_unsafe;
    char           err[160];
} tar_rd_t;

/* One entry, or 0 at the end of the archive, or -1 having filled err. */
/* The name, from the ustar prefix + name pair. A `prefix` field is joined with
   a slash, which is the only way ustar carries a name over 100 bytes. */
static void tar_header_name(const uint8_t *h, tar_ent_t *e)
{
    size_t plen = 0, nlen = 0;

    while (nlen < 100 && h[nlen]) nlen++;
    if (h[345]) {
        while (plen < 155 && h[345 + plen]) plen++;
        memcpy(e->name, h + 345, plen);
        e->name[plen] = '/';
        plen++;
    }
    memcpy(e->name + plen, h, nlen);
    e->nlen = plen + nlen;
    e->name[e->nlen] = 0;
}

/* The numeric fields, or -1 if any of them is not octal. */
static int tar_header_nums(const uint8_t *h, uint64_t *size, uint64_t *mtime,
                           uint64_t *mode)
{
    uint64_t sum = 0;

    if (tar_octal(h + 148, 8, &sum) < 0 || (uint32_t)sum != tar_checksum(h))
        return -1;
    if (tar_octal(h + 124, 12, size) < 0)
        return -2;
    tar_octal(h + 136, 12, mtime);
    tar_octal(h + 100, 8, mode);
    return 0;
}

/* One entry, or 0 at the end of the archive, or -1 having filled err. */
static int tar_next(tar_rd_t *r, tar_ent_t *e)
{
    for (;;) {
        const uint8_t *h;
        uint64_t size = 0, mtime = 0, mode = 0;
        int rc;

        if (r->i + TAR_BLOCK > r->n) {
            if (r->i == r->n)
                return 0;               /* no end blocks, but no partial one */
            snprintf(r->err, sizeof r->err, "truncated header at %u",
                     (unsigned)r->i);
            return -1;
        }
        h = r->p + r->i;
        if (tar_is_zero_block(h))
            return 0;
        rc = tar_header_nums(h, &size, &mtime, &mode);
        if (rc < 0) {
            snprintf(r->err, sizeof r->err, rc == -1
                     ? "header checksum mismatch at block %u"
                     : "unreadable size field at block %u",
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
            /* A GNU long name, a PAX record, a PAX global header, or a GNU
             * long link DESCRIBES the NEXT entry; none of them is a member,
             * and treating their metadata block as file data produced a
             * spurious entry in the listing. */
            if (e->type == 'L') {
                size_t nlen = size > AR_MAX_NAME ? AR_MAX_NAME : (size_t)size;
                memcpy(e->name, r->p + r->i, nlen);
                while (nlen && e->name[nlen - 1] == 0) nlen--;
                e->name[nlen] = 0;
                e->nlen = nlen;
            } else if (e->type == 'K') {
                /* GNU long link: the link target of the NEXT entry, same
                 * shape as 'L' but for the link field. */
                if (e->linklen == 0) {
                    size_t klen = size > AR_MAX_NAME ? AR_MAX_NAME : (size_t)size;
                    memcpy(e->link, r->p + r->i, klen);
                    while (klen && e->link[klen - 1] == 0) klen--;
                    e->link[klen] = 0;
                    e->linklen = klen;
                }
            } else if (e->type != 'x' && e->type != 'X') {
                /* 'g': a PAX GLOBAL header carries defaults for the whole
                 * archive. This reader stores no global state, so the block
                 * is skipped, not listed. */
                r->i += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
                continue;
            } else {
                tar_pax(r->p + r->i, (size_t)size, e);
            }
            r->i += ((size_t)size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            continue;                   /* the real header follows */
        }
        if (e->nlen == 0)               /* not preceded by an L or x record */
            tar_header_name(h, e);
        if (e->linklen == 0) {          /* a PAX linkpath wins over the header */
            size_t k = 0;
            while (k < 100 && h[157 + k]) k++;
            memcpy(e->link, h + 157, k);
            e->link[k] = 0;
            e->linklen = k;
        }
        if (e->size == 0)
            e->size = size;             /* a PAX size wins over the header's */
        e->mtime = mtime;
        e->mode = mode;
        e->body = r->i;
        /* PAX size is authoritative (POSIX) yet only the header's was checked
           against the archive: an over-declared one read heap past the buffer. */
        if (e->size > r->n - e->body) {
            snprintf(r->err, sizeof r->err,
                     "entry declares %llu bytes with %u left in the archive",
                     (unsigned long long)e->size, (unsigned)(r->n - e->body));
            return -1;
        }
        /* Skip the member with the size that is AUTHORITATIVE for it: a PAX
           size record overrides the header's, and advancing by the header's
           would land the next header inside this member's data. Both sizes
           were bounds-checked above. */
        {
            uint64_t adv = e->size != size ? e->size : size;
            r->i += (size_t)((adv + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK);
        }
        if (!r->allow_unsafe && !ar_name_safe(e->name, e->nlen)) {
            snprintf(r->err, sizeof r->err,
                     "entry name is not safe to write anywhere: %.80s", e->name);
            return -1;
        }
        if (!r->allow_unsafe && e->linklen
            && !ar_name_safe(e->link, e->linklen)) {
            snprintf(r->err, sizeof r->err,
                     "link target is not safe to write anywhere: %.80s",
                     e->link);
            return -1;
        }
        return 1;
    }
}

/* ------------------------------------------------------------------- zip */

/* Raw DEFLATE through the gzip codec the module already owns: the zip central
   directory carries the CRC and the size, which is exactly the trailer a gzip
   stream needs, so the member becomes a well-formed gzip stream. */
static int zip_inflate_raw(const uint8_t *src, size_t n, uint32_t crc,
                           uint32_t usize, dyn_outbuf_t *out)
{
    static const uint8_t GZHDR[10] = { 0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff };
    uint8_t *tmp;
    int rc;

    /* n comes from a zip entry's compressed size, i.e. from the archive. A
       wrapped n + 18 would allocate small and the memcpy below would write n
       bytes into it. dyn_gzip_build_ctx does exactly this check 20 lines away
       (stored_sz > SIZE_MAX - 34); the inconsistency was the finding. */
    if (n > SIZE_MAX - 18)
        return -1;
    tmp = (uint8_t *)malloc(n + 18);
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

/* The other direction: gzip, minus its fixed 10-byte header and 8-byte trailer. */
static int zip_deflate_raw(const uint8_t *src, size_t n, uint8_t **out,
                           size_t *out_len)
{
    uint8_t *gz = NULL;
    size_t gn = 0;

    if (dyn_gzip_build(src, n, &gz, &gn) != 0)
        return -1;
    if (gn < 18) { free(gz); return -1; }
    memmove(gz, gz + 10, gn - 18);
    *out = gz;
    *out_len = gn - 18;
    return 0;
}

typedef struct {
    char     name[AR_MAX_NAME + 1];
    size_t   nlen;
    uint32_t crc, csize, usize, method, mtime;
    uint32_t mode;                       /* unix mode from external attrs (0 when absent) */
    char     comment[1025];              /* central comment, NUL-terminated (truncated at 1024) */
    size_t   clen;                       /* comment byte length (pre-truncation capped) */
    size_t   local;                     /* offset of the local header */
} zip_ent_t;

/* DOS datetime (the 4 bytes at central+12 / local+10) <-> unix seconds.
 * 0 is the "no time" sentinel both ways: a writer that has no mtime stores
 * 0, and a reader that sees 0 reports 0 (not 1980-01-01), so an archive
 * written without an mtime keeps reporting 0. */
static uint32_t zip_dos_from_unix(int64_t t)
{
    /* civil-from-days (Howard Hinnant) over unix days; no libc timezone. */
    int64_t days, secs, y, m, d, hh, mm, ss;
    int64_t z, era, doe, yoe, mp;
    uint32_t dos_date, dos_time;
    int yy;

    if (t <= 0)
        return 0;
    /* clamp to the DOS range 1980-01-01 .. 2107-12-31 23:59:58 */
    if (t < 315532800LL)   /* 1980-01-01 */
        t = 315532800LL;
    if (t > 4354819198LL)  /* 2107-12-31 23:59:58 */
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
    if (yy < 0) yy = 0;
    if (yy > 127) yy = 127;
    dos_date = (uint32_t)(((yy & 127) << 9) | ((m & 15) << 5) | (d & 31));
    dos_time = (uint32_t)(((hh & 31) << 11) | ((mm & 63) << 5) | ((ss / 2) & 31));
    return (dos_date << 16) | dos_time;
}

static int64_t zip_unix_from_dos(uint32_t dos)
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
        return 0;                        /* corrupt stamp: report "none" */
    y = yy;
    m = mm;
    y -= (m <= 2);
    /* days-from-civil -> unix days */
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    mp2 = m + (m > 2 ? -3 : 9);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + (153 * mp2 + 2) / 5 + dd - 1;
    z = doe + era * 146097 - 719468;
    days = z;
    return days * 86400 + hh * 3600 + mi * 60 + ss;
}

/* The END OF CENTRAL DIRECTORY record, found by scanning backwards over a
   BOUNDED window -- the comment field is at most 64 KiB. */
static long zip_find_eocd(const uint8_t *p, size_t n)
{
    size_t back = n < 65557 ? n : 65557, i;

    if (n < 22)
        return -1;
    for (i = n - 22 + 1; i-- > n - back;) {
        if (ar_rd32(p + i) == 0x06054b50)
            return (long)i;
        if (i == 0)
            break;
    }
    return -1;
}

typedef struct {
    JSContext     *ctx;
    const uint8_t *p;
    size_t         n;
    size_t         cd_off, cd_size;
    uint32_t       count;
    int            allow_unsafe;
    char           err[160];
} zip_rd_t;

static int zip_open(zip_rd_t *z)
{
    long e = zip_find_eocd(z->p, z->n);

    if (e < 0) {
        snprintf(z->err, sizeof z->err, "no end-of-central-directory record");
        return -1;
    }
    z->count = ar_rd16(z->p + e + 10);
    z->cd_size = ar_rd32(z->p + e + 12);
    z->cd_off = ar_rd32(z->p + e + 16);
    /* ZIP64 sentinel fields (0xFFFF/0xFFFFFFFF): real values live in the
     * ZIP64 EOCD the reader does not parse. A named refusal beats both a
     * generic "bad archive" and — worse — trusting a truncated count. */
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

/* Walk the central directory, which IS the format's index -- scanning for
   local headers instead is what makes a reader disagree with the writer. */
static int zip_entry_at(zip_rd_t *z, size_t *off, zip_ent_t *e)
{
    const uint8_t *p;
    uint32_t nlen, xlen, clen;
    uint32_t ext;

    if (*off + 46 > z->cd_off + z->cd_size)
        return 0;
    p = z->p + *off;
    if (ar_rd32(p) != 0x02014b50) {
        snprintf(z->err, sizeof z->err, "bad central directory signature");
        return -1;
    }
    e->method = ar_rd16(p + 10);
    e->mtime = ar_rd32(p + 12);
    e->crc = ar_rd32(p + 16);
    e->csize = ar_rd32(p + 20);
    e->usize = ar_rd32(p + 24);
    nlen = ar_rd16(p + 28);
    xlen = ar_rd16(p + 30);
    clen = ar_rd16(p + 32);
    ext = ar_rd32(p + 38);
    e->mode = (ext >> 16) & 0xFFFFu;
    e->local = ar_rd32(p + 42);
    if (*off + 46 + nlen + xlen + clen > z->cd_off + z->cd_size) {
        snprintf(z->err, sizeof z->err, "central directory entry overruns");
        return -1;
    }
    if (nlen > AR_MAX_NAME) {
        snprintf(z->err, sizeof z->err, "entry name exceeds %u bytes", AR_MAX_NAME);
        return -1;
    }
    {
        uint32_t stored = clen > 1024 ? 1024 : clen; /* truncate, keep parse */
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
    if (!z->allow_unsafe && !ar_name_safe(e->name, e->nlen)) {
        snprintf(z->err, sizeof z->err,
                 "entry name is not safe to write anywhere: %.80s", e->name);
        return -1;
    }
    *off += 46 + nlen + xlen + clen;
    return 1;
}

/* The member's bytes, verified against the LOCAL header as well: two headers
   that disagree is a real attack (one name, two files, to slip past a
   scanner), so a mismatch is refused rather than resolved. */
static int zip_member(zip_rd_t *z, const zip_ent_t *e, dyn_outbuf_t *out)
{
    const uint8_t *p;
    uint32_t nlen, xlen;
    size_t data;

    if (e->local + 30 > z->n || ar_rd32(z->p + e->local) != 0x04034b50) {
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
        out->buf = (uint8_t *)malloc(e->usize ? e->usize : 1);
        if (!out->buf)
            return -1;
        memcpy(out->buf, z->p + data, e->usize);
        out->len = out->cap = e->usize;
    } else if (e->method == 8) {
        if (zip_inflate_raw(z->p + data, e->csize, e->crc, e->usize, out) != 0) {
            snprintf(z->err, sizeof z->err, "member %.60s does not inflate",
                     e->name);
            return -1;
        }
    } else {
        snprintf(z->err, sizeof z->err,
                 "member %.60s uses compression method %u, and this reader has "
                 "store and deflate", e->name, e->method);
        return -1;
    }
    if (out->len != e->usize) {
        snprintf(z->err, sizeof z->err, "member %.60s decompressed to %u bytes, "
                 "not the %u the directory declares", e->name,
                 (unsigned)out->len, e->usize);
        return -1;
    }
    /* The CRC is the format's own integrity check: a member that fails it is
       an error, not a warning. */
    if (dyn_crc32(out->buf, out->len) != e->crc) {
        snprintf(z->err, sizeof z->err, "member %.60s fails its CRC", e->name);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- JS surface */

/* The {allowUnsafeNames} bag, strict like every options bag in the module:
   an unknown key throws a TypeError naming the key and the valid set. The
   relaxation is never inferred from anything but the explicit value. */
static int ar_opt_unsafe(JSContext *ctx, JSValueConst o, int *out)
{
    static const char *const ar_unsafe_keys[] = { "allowUnsafeNames" };
    JSValue v;

    *out = 0;
    if (!JS_IsObject(o))
        return 0;
    if (dyn_opts_strict(ctx, o, ar_unsafe_keys, 1) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "allowUnsafeNames");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        *out = JS_ToBool(ctx, v);
        if (*out < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue ar_entry_obj(JSContext *ctx, const char *name, size_t nlen,
                            uint64_t size, uint64_t mtime, uint64_t mode,
                            const char *type, const char *link)
{
    JSValue e = JS_NewObject(ctx);

    if (JS_IsException(e))
        return e;
    JS_SetPropertyStr(ctx, e, "name", JS_NewStringLen(ctx, name, nlen));
    JS_SetPropertyStr(ctx, e, "size", JS_NewInt64(ctx, (int64_t)size));
    JS_SetPropertyStr(ctx, e, "mtime", JS_NewInt64(ctx, (int64_t)mtime));
    JS_SetPropertyStr(ctx, e, "mode", JS_NewInt64(ctx, (int64_t)mode));
    JS_SetPropertyStr(ctx, e, "type", JS_NewString(ctx, type));
    if (link && link[0])
        JS_SetPropertyStr(ctx, e, "linkname", JS_NewString(ctx, link));
    return e;
}

static const char *tar_type_name(char t)
{
    switch (t) {
    case '5': return "directory";
    case '2': return "symlink";
    case '1': return "link";
    case '3': case '4': return "device";
    case '6': return "fifo";
    default:  return "file";
    }
}

/* magic 0 = TarList (metadata only), 1 = TarExtract (with the bodies) */
static JSValue dyn_tar_read(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    tar_rd_t r;
    tar_ent_t e;
    JSValue out;
    uint32_t k = 0;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(bytes): an archive is required",
                                 magic ? "TarExtract" : "TarList");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&r, 0, sizeof r);
    r.ctx = ctx; r.p = src; r.n = src_len;
    if (ar_opt_unsafe(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &r.allow_unsafe) < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) { free(src); return out; }
    for (;;) {
        JSValue eo;
        memset(&e, 0, sizeof e);
        rc = tar_next(&r, &e);
        if (rc <= 0)
            break;
        if (k >= AR_MAX_ENTRIES) {
            snprintf(r.err, sizeof r.err, "archive holds more than %u entries",
                     AR_MAX_ENTRIES);
            rc = -1;
            break;
        }
        eo = ar_entry_obj(ctx, e.name, e.nlen, e.size, e.mtime, e.mode,
                          tar_type_name(e.type), e.link);
        if (JS_IsException(eo)) { rc = -1; break; }
        if (magic == 1 && e.type != '5')
            JS_SetPropertyStr(ctx, eo, "data",
                              dyn_bytes_to_uint8(ctx, src + e.body, (size_t)e.size));
        if (JS_DefinePropertyValueUint32(ctx, out, k++, eo, JS_PROP_C_W_E) < 0) {
            rc = -1;
            break;
        }
    }
    free(src);
    if (rc < 0) {
        JS_FreeValue(ctx, out);
        return JS_ThrowSyntaxError(ctx, "%s: %s", magic ? "TarExtract" : "TarList",
                                   r.err[0] ? r.err : "malformed archive");
    }
    return out;
}

/* One ustar header block. A name over 100 bytes uses the prefix field, and a
   name too long for even that is refused rather than silently truncated. */
static int tar_emit_header(JSContext *ctx, ab_t *b, const char *name,
                           size_t nlen, uint64_t size, uint64_t mtime,
                           uint64_t mode, char type)
{
    uint8_t h[TAR_BLOCK];
    size_t split = 0;

    /* ustar's numeric fields are fixed-width octal: size 11 digits, mtime 11.
       A value past that cannot be represented, and silently writing its low
       digits would produce an archive whose metadata is WRONG rather than an
       error, so refuse at the write boundary (POSIX says PAX records, which
       this writer does not emit). */
    if (size > 077777777777ULL) {
        JS_ThrowRangeError(ctx, "TarPack: entry is larger than ustar's "
                                "11-octal-digit size field (%llu bytes); this "
                                "writer emits no PAX records",
                           (unsigned long long)size);
        return -1;
    }
    if (mtime > 077777777777ULL) {
        JS_ThrowRangeError(ctx, "TarPack: mtime %llu does not fit ustar's "
                                "11-octal-digit field (max 8589934591); this "
                                "writer emits no PAX records",
                           (unsigned long long)mtime);
        return -1;
    }

    memset(h, 0, sizeof h);
    if (nlen <= 100) {
        memcpy(h, name, nlen);
    } else {
        size_t i;
        /* The split must leave at most 100 bytes AFTER the slash and at most
           155 before it, so the search starts at the earliest slash that can
           satisfy the suffix -- not at the first slash in the name. */
        for (i = nlen > 101 ? nlen - 101 : 1; i < nlen && i <= 155; i++)
            if (name[i] == '/') { split = i; break; }
        if (!split || nlen - split - 1 > 100 || split > 155) {
            JS_ThrowRangeError(ctx, "TarPack: the name is too long for ustar "
                                    "and this writer emits no PAX records: %.60s",
                               name);
            return -1;
        }
        memcpy(h + 345, name, split);
        memcpy(h, name + split + 1, nlen - split - 1);
    }
    tar_put_octal(h + 100, 8, mode & 07777);
    tar_put_octal(h + 108, 8, 0);
    tar_put_octal(h + 116, 8, 0);
    tar_put_octal(h + 124, 12, size);
    tar_put_octal(h + 136, 12, mtime);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 5);
    h[263] = '0';
    h[264] = '0';
    {   /* The checksum field is SIX octal digits, then NUL, then space -- not
           the seven-digit form every other numeric field uses. */
        uint32_t sum = tar_checksum(h);
        int k;
        for (k = 5; k >= 0; k--) { h[148 + k] = (uint8_t)('0' + (sum & 7)); sum >>= 3; }
        h[154] = 0;
        h[155] = ' ';
    }
    ab_write(b, h, TAR_BLOCK);
    return 0;
}

static int ar_entry_field(JSContext *ctx, JSValueConst e, const char *key,
                          uint64_t *out, uint64_t dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, e, key);
    int64_t t;

    *out = dflt;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return 0; }
    if (JS_ToInt64(ctx, &t, v) < 0) { JS_FreeValue(ctx, v); return -1; }
    JS_FreeValue(ctx, v);
    if (t < 0) {
        JS_ThrowRangeError(ctx, "TarPack: %s must not be negative", key);
        return -1;
    }
    *out = (uint64_t)t;
    return 0;
}

/* One entry's name, bytes and metadata. `*ptypeflag` is the ustar type the
 * entry's `type` field names ("file"/"directory"), or 0 when absent -- the
 * caller then falls back to "a directory is an entry without data". */
static int tar_take_entry(JSContext *ctx, JSValueConst arr, uint32_t i,
                          char **name, size_t *nlen, uint8_t **data,
                          size_t *dlen, uint64_t *mode, uint64_t *mtime,
                          int *isdir, char *ptypeflag)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i), nv, dv, tv;
    const char *s;

    *name = NULL;
    *data = NULL;
    *ptypeflag = 0;
    if (JS_IsException(e))
        return -1;
    if (!JS_IsObject(e)) {
        JS_FreeValue(ctx, e);
        JS_ThrowTypeError(ctx, "TarPack: every entry is an object");
        return -1;
    }
    tv = JS_GetPropertyStr(ctx, e, "type");
    if (JS_IsException(tv)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) {
        const char *t = JS_ToCString(ctx, tv);
        JS_FreeValue(ctx, tv);
        if (!t) {
            JS_FreeValue(ctx, e);
            return -1;
        }
        if (!strcmp(t, "file")) *ptypeflag = '0';
        else if (!strcmp(t, "directory")) *ptypeflag = '5';
        else {
            JS_ThrowRangeError(ctx, "TarPack: entry type is \"file\" or "
                                    "\"directory\" (this writer carries no "
                                    "link metadata), not \"%.40s\"", t);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, e);
            return -1;
        }
        JS_FreeCString(ctx, t);
    } else {
        JS_FreeValue(ctx, tv);
    }
    nv = JS_GetPropertyStr(ctx, e, "name");
    s = JS_IsException(nv) ? NULL : JS_ToCStringLen(ctx, nlen, nv);
    JS_FreeValue(ctx, nv);
    if (!s) { JS_FreeValue(ctx, e); return -1; }
    if (!ar_name_safe(s, *nlen)) {
        JS_ThrowRangeError(ctx, "TarPack: %.60s is not a safe archive name", s);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        return -1;
    }
    *name = (char *)malloc(*nlen + 1);
    if (*name) {
        memcpy(*name, s, *nlen);
        (*name)[*nlen] = 0;
    }
    JS_FreeCString(ctx, s);
    if (!*name) {
        JS_FreeValue(ctx, e);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    dv = JS_GetPropertyStr(ctx, e, "data");
    *isdir = JS_IsUndefined(dv) || JS_IsNull(dv);
    if ((!*isdir && dyn_read_input(ctx, dv, data, dlen) < 0)
        || ar_entry_field(ctx, e, "mode", mode, *isdir ? 0755 : 0644) < 0
        || ar_entry_field(ctx, e, "mtime", mtime, 0) < 0) {
        JS_FreeValue(ctx, dv);
        JS_FreeValue(ctx, e);
        free(*name);
        free(*data);
        *name = NULL;
        *data = NULL;
        return -1;
    }
    JS_FreeValue(ctx, dv);
    JS_FreeValue(ctx, e);
    return 0;
}

static JSValue dyn_tar_pack(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ab_t b;
    JSValue lv, out;
    int64_t n = 0, i;

    (void)this_val;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "TarPack(entries): entries is an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 0 || n > AR_MAX_ENTRIES)
        return JS_ThrowRangeError(ctx, "TarPack: at most %u entries", AR_MAX_ENTRIES);
    ab_init(&b);
    for (i = 0; i < n; i++) {
        char *name;
        uint8_t *data;
        size_t nlen = 0, dlen = 0;
        uint64_t mode, mtime;
        int isdir, rc;
        char typeflag;

        if (tar_take_entry(ctx, argv[0], (uint32_t)i, &name, &nlen, &data,
                           &dlen, &mode, &mtime, &isdir, &typeflag) < 0)
            goto fail;
        isdir = typeflag == '5' || (typeflag == 0 && isdir);
        if (isdir && data) {
            /* an explicit directory entry's data is dead weight: writing it
               behind a size-0 header would desync every reader */
            free(data);
            data = NULL;
            dlen = 0;
        }
        rc = tar_emit_header(ctx, &b, name, nlen, isdir ? 0 : dlen, mtime, mode,
                             isdir ? '5' : '0');
        free(name);
        if (rc < 0) { free(data); goto fail; }
        if (dlen) {
            ab_write(&b, data, dlen);
            ab_zero(&b, (TAR_BLOCK - dlen % TAR_BLOCK) % TAR_BLOCK);
        }
        free(data);
        if (b.oom) { JS_ThrowOutOfMemory(ctx); goto fail; }
    }
    ab_zero(&b, TAR_BLOCK * 2);         /* the two end-of-archive blocks */
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto fail; }
    out = dyn_bytes_to_uint8(ctx, b.p, b.n);
    ab_free(&b);
    return out;
fail:
    ab_free(&b);
    return JS_EXCEPTION;
}

/* magic 0 = ZipList, 1 = ZipRead(bytes, name) */
/* One central-directory entry as a JS object. */
static JSValue zip_entry_obj(JSContext *ctx, const zip_ent_t *e)
{
    JSValue eo = ar_entry_obj(ctx, e->name, e->nlen, e->usize,
                              (uint64_t)zip_unix_from_dos(e->mtime), e->mode,
                              e->nlen && e->name[e->nlen - 1] == '/'
                                  ? "directory" : "file", NULL);
    if (JS_IsException(eo))
        return eo;
    JS_SetPropertyStr(ctx, eo, "compressedSize", JS_NewInt64(ctx, (int64_t)e->csize));
    JS_SetPropertyStr(ctx, eo, "crc32", JS_NewUint32(ctx, e->crc));
    JS_SetPropertyStr(ctx, eo, "method",
                      JS_NewString(ctx, e->method == 0 ? "store"
                                   : e->method == 8 ? "deflate" : "other"));
    JS_SetPropertyStr(ctx, eo, "comment",
                      JS_NewStringLen(ctx, e->comment, e->clen));
    return eo;
}

/* -1 archive error (err set), -2 a JS exception, 0 done. */
static int zip_walk_list(zip_rd_t *z, JSValue out)
{
    size_t off = z->cd_off;
    zip_ent_t e;
    uint32_t k = 0;

    for (;;) {
        JSValue eo;
        int r = zip_entry_at(z, &off, &e);
        if (r <= 0)
            return r;
        eo = zip_entry_obj(z->ctx, &e);
        if (JS_IsException(eo)
            || JS_DefinePropertyValueUint32(z->ctx, out, k++, eo, JS_PROP_C_W_E) < 0)
            return -2;
    }
}

static int zip_walk_find(zip_rd_t *z, const char *want, size_t wlen, JSValue *out)
{
    size_t off = z->cd_off;
    zip_ent_t e;

    for (;;) {
        int r = zip_entry_at(z, &off, &e);
        if (r < 0)
            return -1;
        if (r == 0) {
            snprintf(z->err, sizeof z->err, "no member named %.60s", want);
            return -1;
        }
        if (e.nlen == wlen && memcmp(e.name, want, wlen) == 0) {
            dyn_outbuf_t o = { NULL, 0, 0 };
            if (zip_member(z, &e, &o) < 0) {
                free(o.buf);
                return -1;
            }
            *out = dyn_bytes_to_uint8(z->ctx, o.buf, o.len);
            free(o.buf);
            return JS_IsException(*out) ? -2 : 0;
        }
    }
}

/* magic 0 = ZipList, 1 = ZipRead(bytes, name) */
static JSValue dyn_zip_read(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    uint8_t *src = NULL;
    size_t src_len = 0, wlen = 0;
    zip_rd_t z;
    JSValue out = JS_UNDEFINED;
    const char *want = NULL;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(bytes): an archive is required",
                                 magic ? "ZipRead" : "ZipList");
    if (magic == 1 && (argc < 2 || !JS_IsString(argv[1])))
        return JS_ThrowTypeError(ctx, "ZipRead(bytes, name): a name is required");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&z, 0, sizeof z);
    z.ctx = ctx; z.p = src; z.n = src_len;
    if (ar_opt_unsafe(ctx, argc > (magic ? 2 : 1) ? argv[magic ? 2 : 1] : JS_UNDEFINED,
                      &z.allow_unsafe) < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    if (magic == 1) {
        want = JS_ToCStringLen(ctx, &wlen, argv[1]);
        if (!want) { free(src); return JS_EXCEPTION; }
    }
    rc = zip_open(&z);
    if (rc == 0 && magic == 0) {
        out = JS_NewArray(ctx);
        rc = JS_IsException(out) ? -2 : zip_walk_list(&z, out);
    } else if (rc == 0) {
        rc = zip_walk_find(&z, want, wlen, &out);
    }
    if (want)
        JS_FreeCString(ctx, want);
    free(src);
    if (rc != 0) {
        JS_FreeValue(ctx, out);
        if (rc == -2)
            return JS_EXCEPTION;
        return JS_ThrowSyntaxError(ctx, "%s: %s", magic ? "ZipRead" : "ZipList",
                                   z.err[0] ? z.err : "malformed archive");
    }
    return out;
}

typedef struct {
    char    *name;
    size_t   nlen, csize, usize, local;
    uint32_t crc, method;
    uint32_t dos_mtime;                  /* 0 = none */
    uint32_t mode;                       /* unix mode, 0 = default */
    char    *comment;                    /* malloc'd, may be NULL */
    size_t   clen;
} zip_out_t;

/* Per-entry fields: {name, data, method?, mtime?, mode?, comment?}. All
 * optional; absent means "inherit the pack-level default" (method) or "none"
 * (mtime/mode/comment). Strict: an unknown per-entry key throws a TypeError
 * naming the key and the valid set. The validation and the reads share ONE
 * own-keys listing per entry, so an all-defaults entry pays one listing and
 * the two required reads -- no per-field probing, no second fetch of the
 * entry object. */
static int zip_entry_opt_method(JSContext *ctx, JSValueConst v, int pack_store,
                                int *is_store)
{
    const char *s;

    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *is_store = pack_store;
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "ZipPack: entry method must be a string");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    if (!strcmp(s, "store")) *is_store = 1;
    else if (!strcmp(s, "deflate")) *is_store = 0;
    else {
        JS_ThrowRangeError(ctx, "ZipPack: entry method is \"deflate\" or \"store\"");
        JS_FreeCString(ctx, s);
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static int zip_entry_opt_mtime(JSContext *ctx, JSValueConst v, uint32_t *dos)
{
    int64_t t;

    *dos = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0) {
        JS_ThrowRangeError(ctx, "ZipPack: entry mtime must not be negative");
        return -1;
    }
    *dos = zip_dos_from_unix(t);
    return 0;
}

static int zip_entry_opt_mode(JSContext *ctx, JSValueConst v, uint32_t *mode,
                              int isdir)
{
    int64_t t;

    *mode = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *mode = (uint32_t)(isdir ? 0755 : 0644);
        return 0;
    }
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0 || t > 0777777) {
        JS_ThrowRangeError(ctx, "ZipPack: entry mode must be 0..0777777");
        return -1;
    }
    *mode = (uint32_t)t;
    return 0;
}

static int zip_entry_opt_string(JSContext *ctx, JSValueConst v,
                                char **out, size_t *outlen)
{
    const char *s;
    size_t n = 0;

    *out = NULL;
    if (outlen) *outlen = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "ZipPack: entry \"comment\" must be a string");
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    *out = (char *)malloc(n + 1);
    if (!*out) { JS_FreeCString(ctx, s); JS_ThrowOutOfMemory(ctx); return -1; }
    memcpy(*out, s, n);
    (*out)[n] = 0;
    if (outlen) *outlen = n;
    JS_FreeCString(ctx, s);
    return 0;
}

/* The per-entry key atoms, interned once per ZipPack call and compared by
   identity in the per-entry pass (no per-key string conversion). */
typedef struct {
    JSAtom name, data, method, mtime, mode, comment;
} zip_key_atoms_t;

static int zip_key_atoms_init(JSContext *ctx, zip_key_atoms_t *A)
{
    A->name = JS_NewAtom(ctx, "name");
    A->data = JS_NewAtom(ctx, "data");
    A->method = JS_NewAtom(ctx, "method");
    A->mtime = JS_NewAtom(ctx, "mtime");
    A->mode = JS_NewAtom(ctx, "mode");
    A->comment = JS_NewAtom(ctx, "comment");
    if (A->name == JS_ATOM_NULL || A->data == JS_ATOM_NULL ||
        A->method == JS_ATOM_NULL || A->mtime == JS_ATOM_NULL ||
        A->mode == JS_ATOM_NULL || A->comment == JS_ATOM_NULL)
        return -1;
    return 0;
}

static void zip_key_atoms_free(JSContext *ctx, zip_key_atoms_t *A)
{
    JS_FreeAtom(ctx, A->name);
    JS_FreeAtom(ctx, A->data);
    JS_FreeAtom(ctx, A->method);
    JS_FreeAtom(ctx, A->mtime);
    JS_FreeAtom(ctx, A->mode);
    JS_FreeAtom(ctx, A->comment);
}

/* One entry's name, bytes and per-entry fields; the strings/bytes belong to
   the caller on success. The entry object is fetched exactly once. */
static int zip_take_entry(JSContext *ctx, JSValueConst arr, uint32_t i,
                          const zip_key_atoms_t *A, zip_out_t *o,
                          uint8_t **data, size_t *dlen,
                          int pack_store, int *is_store)
{
    JSValue e, v, m = JS_UNDEFINED, mt = JS_UNDEFINED, md = JS_UNDEFINED,
            cm = JS_UNDEFINED;
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, j;
    const char *s;
    int r = -1;

    *data = NULL;
    *dlen = 0;
    o->name = NULL;
    o->comment = NULL;
    o->clen = 0;
    e = JS_GetPropertyUint32(ctx, arr, i);
    if (JS_IsException(e))
        return -1;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, e,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        goto done;
    for (j = 0; j < nprops; j++) {
        JSAtom at = props[j].atom;
        JSValue *slot = NULL;

        if (at == A->method) slot = &m;
        else if (at == A->mtime) slot = &mt;
        else if (at == A->mode) slot = &md;
        else if (at == A->comment) slot = &cm;
        else if (at == A->name || at == A->data) continue;
        else {
            const char *nm = JS_AtomToCString(ctx, at);
            if (nm) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: name, data, method, "
                    "mtime, mode, comment)", nm);
                JS_FreeCString(ctx, nm);
            }
            goto done;
        }
        *slot = JS_GetProperty(ctx, e, at);
        if (JS_IsException(*slot))
            goto done;
    }
    /* name and data are read unconditionally (an entry may inherit them from
       its prototype); the optional fields were already read above only where
       the entry carries them. */
    v = JS_GetProperty(ctx, e, A->name);
    s = NULL;
    if (!JS_IsException(v) && JS_IsString(v))
        s = JS_ToCStringLen(ctx, &o->nlen, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        JS_ThrowTypeError(ctx, "ZipPack: every entry needs a string name");
        goto done;
    }
    if (!ar_name_safe(s, o->nlen)) {
        JS_ThrowRangeError(ctx, "ZipPack: %.60s is not a safe archive name", s);
        JS_FreeCString(ctx, s);
        goto done;
    }
    o->name = (char *)malloc(o->nlen + 1);
    if (!o->name) {
        JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memcpy(o->name, s, o->nlen);
    o->name[o->nlen] = 0;
    JS_FreeCString(ctx, s);
    v = JS_GetProperty(ctx, e, A->data);
    if (JS_IsException(v) || dyn_read_input(ctx, v, data, dlen) < 0) {
        JS_FreeValue(ctx, v);
        free(o->name);
        o->name = NULL;
        goto done;
    }
    JS_FreeValue(ctx, v);
    if (zip_entry_opt_method(ctx, m, pack_store, is_store) < 0 ||
        zip_entry_opt_mtime(ctx, mt, &o->dos_mtime) < 0 ||
        zip_entry_opt_mode(ctx, md, &o->mode,
                           o->nlen && o->name[o->nlen - 1] == '/') < 0 ||
        zip_entry_opt_string(ctx, cm, &o->comment, &o->clen) < 0) {
        free(*data);
        *data = NULL;
        *dlen = 0;
        free(o->name);
        o->name = NULL;
        free(o->comment);
        o->comment = NULL;
        o->clen = 0;
        goto done;
    }
    r = 0;
done:
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, mt);
    JS_FreeValue(ctx, md);
    JS_FreeValue(ctx, cm);
    if (props) {
        uint32_t q;
        for (q = 0; q < nprops; q++)
            JS_FreeAtom(ctx, props[q].atom);
        js_free(ctx, props);
    }
    JS_FreeValue(ctx, e);
    return r;
}

/* The local header and the member bytes. */
static void zip_emit_local(ab_t *b, zip_out_t *o, const uint8_t *body)
{
    ab_u32(b, 0x04034b50);
    ab_u16(b, 20);                      /* version needed */
    ab_u16(b, 0);
    ab_u16(b, (uint32_t)o->method);
    ab_u32(b, o->dos_mtime);
    ab_u32(b, o->crc);
    ab_u32(b, (uint32_t)o->csize);
    ab_u32(b, (uint32_t)o->usize);
    ab_u16(b, (uint32_t)o->nlen);
    ab_u16(b, 0);
    ab_write(b, o->name, o->nlen);
    ab_write(b, body, o->csize);
}

static void zip_emit_central(ab_t *b, const zip_out_t *o)
{
    ab_u32(b, 0x02014b50);
    ab_u16(b, 20 | (3 << 8));           /* made by: unix */
    ab_u16(b, 20);
    ab_u16(b, 0);
    ab_u16(b, (uint32_t)o->method);
    ab_u32(b, o->dos_mtime);
    ab_u32(b, o->crc);
    ab_u32(b, (uint32_t)o->csize);
    ab_u32(b, (uint32_t)o->usize);
    ab_u16(b, (uint32_t)o->nlen);
    ab_u16(b, 0);
    ab_u16(b, (uint32_t)o->clen);
    ab_u16(b, 0);
    ab_u16(b, 0);
    ab_u32(b, o->mode << 16);           /* unix file mode in external attrs */
    ab_u32(b, (uint32_t)o->local);
    ab_write(b, o->name, o->nlen);
    if (o->clen)
        ab_write(b, o->comment, o->clen);
}

static int zip_pack_method(JSContext *ctx, JSValueConst o, int *store_only)
{
    JSValue v;
    const char *s;

    *store_only = 0;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "method");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsString(v)) { JS_FreeValue(ctx, v); return 0; }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *store_only = strcmp(s, "store") == 0;
    if (!*store_only && strcmp(s, "deflate") != 0) {
        JS_FreeCString(ctx, s);
        JS_ThrowRangeError(ctx, "ZipPack: method is \"deflate\" or \"store\"");
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue dyn_zip_pack(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ab_t b;
    JSValue lv, out;
    int64_t n = 0, i;
    zip_out_t *ents = NULL;
    zip_key_atoms_t Z;
    size_t cd_off;
    int store_only = 0;

    (void)this_val;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "ZipPack(entries): entries is an array");
    if (zip_pack_method(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &store_only) < 0)
        return JS_EXCEPTION;
    /* the pack-level bag accepts only {method} */
    if (argc > 1 && JS_IsObject(argv[1]) &&
        dyn_opts_strict(ctx, argv[1], (const char *const[]){"method"}, 1) < 0)
        return JS_EXCEPTION;
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 0 || n > ZIP_MAX_ENTRIES)
        return JS_ThrowRangeError(ctx, "ZipPack: at most %u entries (the zip "
                                  "EOCD count field is 16-bit; zip64 is not "
                                  "written)", ZIP_MAX_ENTRIES);
    if (zip_key_atoms_init(ctx, &Z) < 0) {
        zip_key_atoms_free(ctx, &Z);
        return JS_ThrowOutOfMemory(ctx);
    }
    ents = (zip_out_t *)calloc((size_t)(n ? n : 1), sizeof *ents);
    if (!ents) {
        zip_key_atoms_free(ctx, &Z);
        return JS_ThrowOutOfMemory(ctx);
    }
    ab_init(&b);
    for (i = 0; i < n; i++) {
        uint8_t *data = NULL, *comp = NULL;
        size_t dlen = 0, clen = 0;
        int is_store;

        if (zip_take_entry(ctx, argv[0], (uint32_t)i, &Z, &ents[i], &data,
                           &dlen, store_only, &is_store) < 0)
            goto fail;
        if (ents[i].comment && ents[i].clen > 1024) {
            /* Central comment capped: truncate, keep NUL. */
            ents[i].comment[1024] = 0;
            ents[i].clen = 1024;
        }
        if (dlen > AR_MAX_MEMBER) {
            JS_ThrowRangeError(ctx, "ZipPack: entry %u exceeds the size limit",
                               (unsigned)i);
            free(data);
            goto fail;
        }
        ents[i].crc = dyn_crc32(data, dlen);
        ents[i].usize = dlen;
        ents[i].local = b.n;
        if (!is_store && dlen && zip_deflate_raw(data, dlen, &comp, &clen) == 0
            && clen < dlen) {
            ents[i].method = 8;
            ents[i].csize = clen;
        } else {
            free(comp);
            comp = NULL;
            ents[i].method = 0;
            ents[i].csize = dlen;
        }
        zip_emit_local(&b, &ents[i], comp ? comp : data);
        free(comp);
        free(data);
        if (b.oom) { JS_ThrowOutOfMemory(ctx); goto fail; }
    }
    cd_off = b.n;
    for (i = 0; i < n; i++)
        zip_emit_central(&b, &ents[i]);
    {   /* The directory's size must be taken BEFORE the record that reports
           it: computing it afterwards is short by the 12 bytes already
           written, which is exactly what unzip(1) called "missing 12 bytes". */
        size_t cd_size = b.n - cd_off;
        ab_u32(&b, 0x06054b50);
        ab_u16(&b, 0);
        ab_u16(&b, 0);
        ab_u16(&b, (uint32_t)n);
        ab_u16(&b, (uint32_t)n);
        ab_u32(&b, (uint32_t)cd_size);
        ab_u32(&b, (uint32_t)cd_off);
        ab_u16(&b, 0);
    }
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto fail; }
    zip_key_atoms_free(ctx, &Z);
    for (i = 0; i < n; i++) {
        free(ents[i].name);
        free(ents[i].comment);
    }
    free(ents);
    out = dyn_bytes_to_uint8(ctx, b.p, b.n);
    ab_free(&b);
    return out;
fail:
    zip_key_atoms_free(ctx, &Z);
    for (i = 0; i < n; i++) {
        free(ents[i].name);
        free(ents[i].comment);
    }
    free(ents);
    ab_free(&b);
    return JS_EXCEPTION;
}

/* Raw DEFLATE one-shots (RFC 1951, no gzip framing).
 * deflate(data, {level?}) -> Uint8Array; inflate(data, {asString?}) -> bytes.
 * deflate output verifies vs ZipPack's embedded codec: same core
 * (dyn_raw_deflate vs zip_deflate_raw over dyn_gzip_build), so the member
 * bytes of a deflated ZipPack entry equal deflate(entry.data) at level 1. */
static JSValue dyn_deflate_js(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 1;
    static const char *const lvl_keys[] = { "level" };

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "deflate(data): input required");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue lv;
        int64_t t;
        if (dyn_opts_strict(ctx, argv[1], lvl_keys, 1) < 0)
            return JS_EXCEPTION;
        lv = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(lv) && !JS_IsNull(lv)) {
            if (JS_ToInt64(ctx, &t, lv) < 0) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
            if (t < 1 || t > 12) {
                JS_FreeValue(ctx, lv);
                return JS_ThrowRangeError(ctx, "deflate: level must be 1..12");
            }
            level = (int)t;
        }
        JS_FreeValue(ctx, lv);
    } else if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        return JS_ThrowTypeError(ctx, "deflate(data, opts): opts must be an object");
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (src_len > AR_MAX_MEMBER) {
        free(src);
        return JS_ThrowRangeError(ctx, "deflate: input exceeds the size limit");
    }
    if (dyn_raw_deflate(src, src_len, level, NULL, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    {
        JSValue r = dyn_bytes_to_uint8(ctx, out, out_len);
        free(out);
        return r;
    }
}

static JSValue dyn_inflate_js(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    static const char *const as_keys[] = { "asString" };

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "inflate(data): input required");
    if (argc > 1 && JS_IsObject(argv[1]) &&
        dyn_opts_strict(ctx, argv[1], as_keys, 1) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_raw_inflate(src, src_len, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowSyntaxError(ctx, "inflate: malformed DEFLATE stream");
    }
    free(src);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue av = JS_GetPropertyStr(ctx, argv[1], "asString");
        int as = 0;
        if (JS_IsException(av)) { free(o.buf); return JS_EXCEPTION; }
        if (!JS_IsUndefined(av) && !JS_IsNull(av)) as = JS_ToBool(ctx, av);
        JS_FreeValue(ctx, av);
        if (as < 0) { free(o.buf); return JS_EXCEPTION; }
        if (as) {
            JSValue s = JS_NewStringLen(ctx, (const char *)o.buf, o.len);
            free(o.buf);
            return s;
        }
    }
    {
        JSValue r = dyn_bytes_to_uint8(ctx, o.buf, o.len);
        free(o.buf);
        return r;
    }
}

/* ZipReadAt(bytes, index) -- entry by-index (central-directory order).
 * Returns {name, size, mtime, mode, type, compressedSize, crc32, method,
 * comment, data}. Index out of range is a RangeError. */
static JSValue dyn_zip_read_at(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    zip_rd_t z;
    int64_t idx = 0;
    size_t off;
    zip_ent_t e;
    int rc, k = 0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ZipReadAt(bytes, index): both required");
    if (JS_ToInt64(ctx, &idx, argv[1]) < 0)
        return JS_EXCEPTION;
    if (idx < 0)
        return JS_ThrowRangeError(ctx, "ZipReadAt: index must be >= 0");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&z, 0, sizeof z);
    z.ctx = ctx; z.p = src; z.n = src_len;
    if (ar_opt_unsafe(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &z.allow_unsafe) < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    if (zip_open(&z) < 0) {
        free(src);
        return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
                                   z.err[0] ? z.err : "malformed archive");
    }
    off = z.cd_off;
    memset(&e, 0, sizeof e);
    for (;;) {
        rc = zip_entry_at(&z, &off, &e);
        if (rc < 0) {
            free(src);
            return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
                                       z.err[0] ? z.err : "malformed archive");
        }
        if (rc == 0) {
            free(src);
            return JS_ThrowRangeError(ctx, "ZipReadAt: index %lld out of range",
                                      (long long)idx);
        }
        if (k == idx)
            break;
        k++;
        memset(&e, 0, sizeof e);
    }
    {
        dyn_outbuf_t o = { NULL, 0, 0 };
        JSValue eo;
        if (zip_member(&z, &e, &o) < 0) {
            free(o.buf);
            free(src);
            return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
                                       z.err[0] ? z.err : "malformed archive");
        }
        eo = zip_entry_obj(ctx, &e);
        free(src);
        if (JS_IsException(eo)) { free(o.buf); return JS_EXCEPTION; }
        {
            JSValue dv = dyn_bytes_to_uint8(ctx, o.buf, o.len);
            if (JS_IsException(dv)) {
                JS_FreeValue(ctx, eo);
                free(o.buf);
                return JS_EXCEPTION;
            }
            JS_SetPropertyStr(ctx, eo, "data", dv);
        }
        free(o.buf);
        return eo;
    }
}

/* ---- writing members under a destination directory ------------------------
 *
 * ar_write_member writes one member's bytes as `name` under `rootfd`, the
 * pinned destination directory. Exactly what the walk enforces:
 *
 *   - no symlink is ever followed. Every directory component is entered with
 *     openat(..., O_NOFOLLOW|O_DIRECTORY) and the final name is opened with
 *     O_NOFOLLOW|O_EXCL, so a symlink planted in the destination beforehand
 *     -- or swapped in while the walk runs -- is refused instead of being
 *     written through. Every step resolves against a HELD fd, so the lookup
 *     and the write cannot be raced apart (no TOCTOU window);
 *   - an existing regular file is REPLACED (unlinked, then re-created with
 *     0666 & ~umask, the fopen default), never truncated in place: a
 *     pre-existing hard link to a file outside the destination must not be
 *     clobbered through it. Anything that is not a regular file (symlink,
 *     directory, fifo, device) is refused, so no member blocks on or
 *     overwrites one;
 *   - `..` segments move outward only where the archive's names already
 *     passed the parse-boundary gate with {allowUnsafeNames:true} -- the
 *     documented opt-in escape. Otherwise no name reaching here can name the
 *     destination's parent.
 *
 * Returns 0, or -1 with errno set. */
static int ar_write_member(int rootfd, const char *name, size_t nlen,
                           const uint8_t *buf, size_t len)
{
    char *path, *comp, *sep;
    int cur = rootfd, cur_owned = 0, next, fd = -1, rc = -1;
    struct stat st;

    path = (char *)malloc(nlen + 1);
    if (!path) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(path, name, nlen);
    path[nlen] = '\0';
    comp = path;
    for (;;) {
        sep = strchr(comp, '/');
        if (!sep)
            break;
        *sep = '\0';
        if (comp[0] != '\0' && !(comp[0] == '.' && comp[1] == '\0')) {
            if (mkdirat(cur, comp, 0755) < 0 && errno != EEXIST)
                goto out;
            next = openat(cur, comp,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
                goto out;
            if (cur_owned)
                close(cur);
            cur = next;
            cur_owned = 1;
        }
        comp = sep + 1;
    }
    if (comp[0] == '\0' || (comp[0] == '.' && comp[1] == '\0')) {
        errno = EISDIR;                 /* no writable leaf name */
        goto out;
    }
    if (fstatat(cur, comp, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = ELOOP;              /* symlink, dir, fifo, device */
            goto out;
        }
        if (unlinkat(cur, comp, 0) < 0)
            goto out;
    } else if (errno != ENOENT) {
        goto out;
    }
    fd = openat(cur, comp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0666);
    if (fd < 0)
        goto out;
    while (len) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        buf += (size_t)w;
        len -= (size_t)w;
    }
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    if (cur_owned)
        close(cur);
    free(path);
    return rc;
}

/* Copy a destination argument (a string or a Path) into *pdir. Returns 0, or
   -1 with a pending exception. */
static int ar_dir_value(JSContext *ctx, JSValueConst v, char **pdir,
                        size_t *plen)
{
    const char *s;
    size_t n = 0;
    int is_str = JS_IsString(v);

    if (is_str) {
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
    } else if (dyn_value_is_path(v)) {
        s = dyn_path_borrow(ctx, v, "ZipExtractAll: dir", &n);
        if (!s)
            return -1;
    } else {
        JS_ThrowTypeError(ctx, "ZipExtractAll: dir must be a string or Path");
        return -1;
    }
    *pdir = (char *)malloc(n + 1);
    if (!*pdir) {
        if (is_str)
            JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(*pdir, s, n);
    (*pdir)[n] = 0;
    *plen = n;
    if (is_str)
        JS_FreeCString(ctx, s);
    return 0;
}

/* ZipExtractAll(bytes[, dir][, opts]) -- every member with its bytes.
 * Without a destination: returns [{name, size, mtime, mode, type,
 * compressedSize, crc32, method, comment, data}]. The destination is either
 * the second argument (a string or a Path) or `dir` in the strict options bag
 * {dir, allowUnsafeNames}. A plain object is the bag, a string or Path is the
 * destination, and anything else is refused rather than coerced -- a bag used
 * to stringify to "[object Object]" and become a directory. The name-gate
 * relaxation is never implied by anything but the explicit
 * {allowUnsafeNames:true}, which together with a destination is a DOCUMENTED
 * OPT-IN ESCAPE: an unsafe name then writes OUTSIDE dir (see the module
 * header). Writing enforces the ar_write_member contract -- under dir no
 * symlink is ever traversed or written through. Files are created with the
 * default mode (0666 & ~umask); a member's stored mode is reported in its
 * record, never applied. */
static JSValue dyn_zip_extract_all(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    static const char *const ar_extract_keys[] = { "dir", "allowUnsafeNames" };
    uint8_t *src = NULL;
    size_t src_len = 0;
    zip_rd_t z;
    JSValue out = JS_UNDEFINED;
    uint32_t k = 0;
    size_t off, dirlen = 0;
    char *dir = NULL;
    int rootfd = -1, allow_unsafe = 0, a;
    JSValueConst bag = JS_UNDEFINED;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ZipExtractAll(bytes[, dir]): archive required");
    for (a = 1; a < argc; a++) {
        JSValueConst v = argv[a];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (JS_IsString(v) || dyn_value_is_path(v)) {
            if (dir) {
                JS_ThrowTypeError(ctx, "ZipExtractAll: dir given twice");
                goto fail;
            }
            if (ar_dir_value(ctx, v, &dir, &dirlen) < 0)
                goto fail;
        } else if (JS_IsObject(v) && JS_IsArray(ctx, v) != 1) {
            if (!JS_IsUndefined(bag)) {
                JS_ThrowTypeError(ctx, "ZipExtractAll: one options bag");
                goto fail;
            }
            bag = v;
        } else {
            JS_ThrowTypeError(ctx, "ZipExtractAll: dir must be a string or "
                                   "Path, opts an object");
            goto fail;
        }
    }
    if (!JS_IsUndefined(bag)) {
        JSValue v;
        if (dyn_opts_strict(ctx, bag, ar_extract_keys, 2) < 0)
            goto fail;
        v = JS_GetPropertyStr(ctx, bag, "dir");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (dir) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "ZipExtractAll: dir given twice");
                goto fail;
            }
            if (ar_dir_value(ctx, v, &dir, &dirlen) < 0) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, bag, "allowUnsafeNames");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                goto fail;
            allow_unsafe = b;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        goto fail;
    memset(&z, 0, sizeof z);
    z.ctx = ctx; z.p = src; z.n = src_len;
    z.allow_unsafe = allow_unsafe;
    if (zip_open(&z) < 0) {
        JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                            z.err[0] ? z.err : "malformed archive");
        goto fail;
    }
    if (dir) {
        /* mkdir -p the destination path itself, then pin it with a directory
           fd. The path is the caller's own choice (a symlink in it, like
           macOS's /tmp -> /private/tmp, is followed); everything under it is
           reached through the pinned fd per the ar_write_member contract. */
        char *tmp = (char *)malloc(dirlen + 1);
        if (tmp) {
            size_t i;
            memcpy(tmp, dir, dirlen + 1);
            for (i = 1; i < dirlen; i++) {
                if (tmp[i] == '/') {
                    tmp[i] = 0;
                    mkdir(tmp, 0755);
                    tmp[i] = '/';
                }
            }
            mkdir(tmp, 0755);
            free(tmp);
        }
        rootfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (rootfd < 0) {
            JS_ThrowTypeError(ctx, "ZipExtractAll: cannot write '%.80s'", dir);
            goto fail;
        }
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto fail;
    off = z.cd_off;
    for (;;) {
        zip_ent_t e;
        dyn_outbuf_t o = { NULL, 0, 0 };
        JSValue eo, dv;
        int r;

        memset(&e, 0, sizeof e);
        r = zip_entry_at(&z, &off, &e);
        if (r < 0) {
            JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                                z.err[0] ? z.err : "malformed archive");
            goto fail;
        }
        if (r == 0)
            break;
        if (k >= AR_MAX_ENTRIES) {
            JS_ThrowRangeError(ctx, "ZipExtractAll: too many entries");
            goto fail;
        }
        if (zip_member(&z, &e, &o) < 0) {
            free(o.buf);
            JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                                z.err[0] ? z.err : "malformed archive");
            goto fail;
        }
        eo = zip_entry_obj(ctx, &e);
        if (JS_IsException(eo)) {
            free(o.buf);
            goto fail;
        }
        dv = dyn_bytes_to_uint8(ctx, o.buf, o.len);
        if (JS_IsException(dv)) {
            JS_FreeValue(ctx, eo);
            free(o.buf);
            goto fail;
        }
        if (JS_SetPropertyStr(ctx, eo, "data", dv) < 0) {   /* consumes dv */
            JS_FreeValue(ctx, eo);
            free(o.buf);
            goto fail;
        }
        if (rootfd >= 0 && !(e.nlen && e.name[e.nlen - 1] == '/')) {
            if (ar_write_member(rootfd, e.name, e.nlen, o.buf, o.len) < 0) {
                /* every exit frees the record it was building */
                JS_FreeValue(ctx, eo);
                free(o.buf);
                JS_ThrowTypeError(ctx, "ZipExtractAll: cannot write '%.80s'",
                                  e.name);
                goto fail;
            }
        }
        free(o.buf);
        if (JS_DefinePropertyValueUint32(ctx, out, k++, eo, JS_PROP_C_W_E) < 0)
            goto fail;                  /* consumes eo either way */
    }
    free(src);
    free(dir);
    if (rootfd >= 0)
        close(rootfd);
    return out;
fail:
    JS_FreeValue(ctx, out);
    free(src);
    free(dir);
    if (rootfd >= 0)
        close(rootfd);
    return JS_EXCEPTION;
}
