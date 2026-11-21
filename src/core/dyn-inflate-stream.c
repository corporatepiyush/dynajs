#include "dyn-inflate-stream.h"
#include <stdlib.h>
#include <string.h>

#define WSZ 32768u
#define OUTMAX 98304u
#define HCAP (WSZ + OUTMAX)
#define FAST_ROOM 320u
#define HMIN 1024u
#define LB 10
#define DB 9
#define CB 7
#define LT_CAP ((1u << LB) + 288u * (1u << (15 - LB)))
#define DT_CAP ((1u << DB) + 32u * (1u << (15 - DB)))

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

enum { S_HDR, S_SLEN, S_SCOPY, S_TAB, S_CLEN, S_LENS, S_CODES, S_DONE };
enum { T_BAD = 0, T_LIT = 1, T_LEN = 2, T_EOB = 4, T_SUB = 6, T_DIST = 8 };
enum { K_LIT, K_DIST, K_CL };

#define ENT(v, t, x, l) (((uint32_t)(v) << 16) | ((uint32_t)(x) << 12) | ((uint32_t)(t) << 8) | (uint32_t)(l))
#define E_LEN(e) ((e) & 63u)
#define E_EXTRA(e) (((e) >> 12) & 15u)
#define E_TYPE(e) (((e) >> 8) & 15u)
#define E_ISLIT(e) ((e) & 0x100u)
#define E_CL(e) (((e) >> 12) & 15u)
#define E_XV(e, b) (size_t)(((b) & ((1u << E_LEN(e)) - 1u)) >> E_CL(e))
#define E_VAL(e) ((e) >> 16)

struct dyn_infs {
    uint64_t bb;
    unsigned bc;
    int state, last, hlit, hdist, hclen, idx, lt_fixed;
    unsigned slen;
    uint8_t* hb;
    size_t hpos, hcap;
    short lens[320];
    uint32_t* lt;
    uint32_t* dt;
    uint32_t ct[1u << CB];
};

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static uint32_t sym_ent(int kind, int s, int l)
{
    if (kind == K_CL)
        return ENT(s, T_LIT, 0, l);
    if (kind == K_DIST)
        return s < 30 ? ENT(dbase[s], T_DIST, l, l + dext[s]) : ENT(0, T_BAD, 0, l);
    if (s < 256)
        return ENT(s, T_LIT, 0, l);
    if (s == 256)
        return ENT(0, T_EOB, 0, l);
    if (s < 286)
        return ENT(lbase[s - 257], T_LEN, l, l + lext[s - 257]);
    return ENT(0, T_BAD, 0, l);
}

static unsigned rev_bits(unsigned c, int l)
{
    unsigned r = 0;
    while (l--) {
        r = (r << 1) | (c & 1u);
        c >>= 1;
    }
    return r;
}

static int hbuild(uint32_t* t, int tb, size_t cap, unsigned sub0, const short* length, int n, int kind, short* count)
{
    unsigned next[16], code = 0, tsz = 1u << tb, nsub = sub0;
    int left = 1, l, s, maxlen = 0;
    memset(count, 0, 16 * sizeof(short));
    for (s = 0; s < n; s++)
        count[length[s]]++;
    if (count[0] == n) {
        memset(t, 0, tsz * sizeof(uint32_t));
        return 0;
    }
    for (l = 1; l <= 15; l++) {
        left <<= 1;
        left -= count[l];
        if (left < 0)
            return left;
        if (count[l])
            maxlen = l;
    }
    for (l = 1; l <= 15; l++) {
        code = (code + (l == 1 ? 0u : (unsigned)count[l - 1])) << 1;
        next[l] = code;
    }
    if (left)
        memset(t, 0, tsz * sizeof(uint32_t));
    if (maxlen > tb) {
        unsigned nx[16];
        memcpy(nx, next, sizeof nx);
        for (s = 0; s < n; s++) {
            l = length[s];
            if (l > tb)
                t[rev_bits(nx[l]++, l) & (tsz - 1)] = 0;
        }
    }
    for (s = 0; s < n; s++) {
        unsigned r, k;
        uint32_t e;
        l = length[s];
        if (!l)
            continue;
        r = rev_bits(next[l]++, l);
        e = sym_ent(kind, s, l);
        if (l <= tb) {
            for (k = r; k < tsz; k += 1u << l)
                t[k] = e;
        } else {
            unsigned pre = r & (tsz - 1), sb = (unsigned)(maxlen - tb), base;
            if (E_TYPE(t[pre]) != T_SUB) {
                if (nsub + (1u << sb) > cap)
                    return -1;
                if (left)
                    memset(t + nsub, 0, ((size_t)1 << sb) * sizeof(uint32_t));
                t[pre] = ENT(nsub, T_SUB, sb, 0);
                nsub += 1u << sb;
            }
            base = E_VAL(t[pre]);
            for (k = r >> tb; k < (1u << sb); k += 1u << (l - tb))
                t[base + k] = e;
        }
    }
    return left;
}

static inline uint32_t tlook(const uint32_t* t, int tb, uint64_t bb)
{
    uint32_t e = t[bb & ((1u << tb) - 1u)];
    if (UNLIKELY(E_TYPE(e) == T_SUB))
        e = t[E_VAL(e) + ((bb >> tb) & ((1u << E_EXTRA(e)) - 1u))];
    return e;
}

static inline int tpeek(const uint32_t* t, int tb, uint64_t bb, unsigned bc, uint32_t* out)
{
    uint32_t e = tlook(t, tb, bb);
    unsigned l = E_LEN(e);
    if (!l)
        return bc >= 15 ? DYN_INFS_BAD : DYN_INFS_MORE;
    if (l > bc)
        return DYN_INFS_MORE;
    *out = e;
    return 1;
}

static inline uint64_t load64le(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static void fill(dyn_infs_t* z, const uint8_t** p, const uint8_t* e)
{
    while (z->bc < 56 && *p < e) {
        z->bb |= (uint64_t)*(*p)++ << z->bc;
        z->bc += 8;
    }
}

static void drop(dyn_infs_t* z, unsigned n)
{
    z->bb >>= n;
    z->bc -= n;
}

dyn_infs_t* dyn_infs_new(void)
{
    return (dyn_infs_t*)calloc(1, sizeof(dyn_infs_t));
}

void dyn_infs_free(dyn_infs_t* z)
{
    if (!z)
        return;
    free(z->hb);
    free(z->lt);
    free(z);
}

static int tables(dyn_infs_t* z)
{
    if (z->lt)
        return 0;
    z->lt = (uint32_t*)malloc((LT_CAP + DT_CAP) * sizeof(uint32_t));
    if (!z->lt)
        return -1;
    z->dt = z->lt + (1u << LB);
    return 0;
}

static void fixed_tables(dyn_infs_t* z)
{
    short l[288], cnt[16];
    int i;
    if (z->lt_fixed)
        return;
    for (i = 0; i < 144; i++)
        l[i] = 8;
    for (; i < 256; i++)
        l[i] = 9;
    for (; i < 280; i++)
        l[i] = 7;
    for (; i < 288; i++)
        l[i] = 8;
    hbuild(z->lt, LB, LT_CAP + DT_CAP, (1u << LB) + DT_CAP, l, 288, K_LIT, cnt);
    for (i = 0; i < 30; i++)
        l[i] = 5;
    hbuild(z->dt, DB, DT_CAP, 1u << DB, l, 30, K_DIST, cnt);
    z->lt_fixed = 1;
}

static inline void match_copy(uint8_t* op, size_t dist, size_t len)
{
    const uint8_t* src = op - dist;
    uint8_t* end = op + len;
    if (LIKELY(dist >= 16)) {
        do {
            memcpy(op, src, 16);
            op += 16;
            src += 16;
        } while (op < end);
    } else if (dist >= 8) {
        do {
            memcpy(op, src, 8);
            op += 8;
            src += 8;
        } while (op < end);
    } else if (dist == 1) {
        memset(op, *src, len);
    } else {
        while (op < end)
            *op++ = *src++;
    }
}

static inline void fast_copy(uint8_t* op, const uint8_t* src, size_t dist, size_t len)
{
    if (LIKELY(dist >= 16)) {
        uint8_t* end = op + len;
        memcpy(op, src, 16);
        memcpy(op + 16, src + 16, 16);
        memcpy(op + 32, src + 32, 16);
        if (UNLIKELY(len > 48)) {
            op += 48;
            src += 48;
            do {
                memcpy(op, src, 16);
                op += 16;
                src += 16;
            } while (op < end);
        }
    } else
        match_copy(op, dist, len);
}

static int codes(dyn_infs_t* z, const uint8_t** pp, const uint8_t* e)
{
    uint8_t* const ob = z->hb;
    uint8_t* op = ob + z->hpos;
    uint8_t* const olim = ob + z->hcap;
    const uint32_t* const lt = z->lt;
    const uint32_t* const dt = z->dt;
    const uint8_t* p = *pp;
    uint64_t bb = z->bb;
    unsigned bc = z->bc;
    int r;

    while (LIKELY(e - p >= 16 && (size_t)(olim - op) >= FAST_ROOM)) {
        uint32_t ent, de;
        size_t len, dist;
        bb |= load64le(p) << bc;
        p += (63 - bc) >> 3;
        bc |= 56;
        ent = lt[bb & ((1u << LB) - 1u)];
    have:
        if (LIKELY(E_ISLIT(ent))) {
            bb >>= E_LEN(ent);
            bc -= E_LEN(ent);
            *op++ = (uint8_t)E_VAL(ent);
            ent = lt[bb & ((1u << LB) - 1u)];
            if (E_ISLIT(ent)) {
                bb >>= E_LEN(ent);
                bc -= E_LEN(ent);
                *op++ = (uint8_t)E_VAL(ent);
                ent = lt[bb & ((1u << LB) - 1u)];
                if (E_ISLIT(ent)) {
                    bb >>= E_LEN(ent);
                    bc -= E_LEN(ent);
                    *op++ = (uint8_t)E_VAL(ent);
                    continue;
                }
            }
            bb |= load64le(p) << bc;
            p += (63 - bc) >> 3;
            bc |= 56;
        }
        if (UNLIKELY(E_TYPE(ent) == T_SUB)) {
            ent = lt[E_VAL(ent) + ((bb >> LB) & ((1u << E_EXTRA(ent)) - 1u))];
            if (E_ISLIT(ent)) {
                bb >>= E_LEN(ent);
                bc -= E_LEN(ent);
                *op++ = (uint8_t)E_VAL(ent);
                continue;
            }
        }
        if (LIKELY(E_TYPE(ent) == T_LEN)) {
            len = E_VAL(ent) + E_XV(ent, bb);
            bb >>= E_LEN(ent);
            bc -= E_LEN(ent);
            de = tlook(dt, DB, bb);
            if (UNLIKELY(E_TYPE(de) != T_DIST))
                goto bad;
            dist = E_VAL(de) + E_XV(de, bb);
            bb >>= E_LEN(de);
            bc -= E_LEN(de);
            if (UNLIKELY(dist > (size_t)(op - ob)))
                goto bad;
            if (LIKELY(e - p >= 16)) {
                bb |= load64le(p) << bc;
                p += (63 - bc) >> 3;
                bc |= 56;
                ent = lt[bb & ((1u << LB) - 1u)];
                fast_copy(op, op - dist, dist, len);
                op += len;
                if (LIKELY((size_t)(olim - op) >= FAST_ROOM))
                    goto have;
                break;
            }
            fast_copy(op, op - dist, dist, len);
            op += len;
            continue;
        }
        if (E_TYPE(ent) == T_EOB) {
            bb >>= E_LEN(ent);
            bc -= E_LEN(ent);
            r = 3;
            goto out;
        }
        goto bad;
    }
    z->bb = bb;
    z->bc = bc;
    for (;;) {
        uint32_t ent, de;
        unsigned need;
        size_t len, dist;
        if (z->bc < 48)
            fill(z, &p, e);
        r = tpeek(lt, LB, z->bb, z->bc, &ent);
        if (r <= 0)
            goto slow_out;
        if (E_TYPE(ent) == T_LIT) {
            if (op >= olim) {
                r = DYN_INFS_FULL;
                goto slow_out;
            }
            drop(z, E_LEN(ent));
            *op++ = (uint8_t)E_VAL(ent);
            continue;
        }
        if (E_TYPE(ent) == T_EOB) {
            drop(z, E_LEN(ent));
            r = 3;
            goto slow_out;
        }
        if (E_TYPE(ent) != T_LEN) {
            r = DYN_INFS_BAD;
            goto slow_out;
        }
        need = E_LEN(ent);
        len = E_VAL(ent) + E_XV(ent, z->bb);
        r = tpeek(dt, DB, z->bb >> need, z->bc - need, &de);
        if (r <= 0)
            goto slow_out;
        if (E_TYPE(de) != T_DIST) {
            r = DYN_INFS_BAD;
            goto slow_out;
        }
        dist = E_VAL(de) + E_XV(de, z->bb >> need);
        if (dist > (size_t)(op - ob)) {
            r = DYN_INFS_BAD;
            goto slow_out;
        }
        if ((size_t)(olim - op) < len) {
            r = DYN_INFS_FULL;
            goto slow_out;
        }
        drop(z, need + E_LEN(de));
        {
            const uint8_t* src = op - dist;
            size_t k;
            for (k = 0; k < len; k++)
                op[k] = src[k];
            op += len;
        }
    }
bad:
    r = DYN_INFS_BAD;
out:
    z->bb = bb;
    z->bc = bc;
slow_out:
    *pp = p;
    z->hpos = (size_t)(op - ob);
    if (r == 3)
        z->state = z->last ? S_DONE : S_HDR;
    return r;
}

int dyn_infs_run(dyn_infs_t* z, const uint8_t* in, size_t n, size_t* used, const uint8_t** out, size_t* outn)
{
    const uint8_t *p = in, *e = in + n;
    int r = DYN_INFS_MORE;
    size_t start;
    short cnt[16];
    if (z->hcap < HCAP && z->hcap - z->hpos < z->hcap / 2 + FAST_ROOM) {
        size_t nc = z->hcap ? z->hcap * 2 : HMIN;
        uint8_t* nb;
        if (nc > HCAP)
            nc = HCAP;
        nb = (uint8_t*)realloc(z->hb, nc);
        if (!nb) {
            *used = 0;
            *out = NULL;
            *outn = 0;
            return DYN_INFS_OOM;
        }
        z->hb = nb;
        z->hcap = nc;
    }
    if (z->hcap == HCAP && HCAP - z->hpos < WSZ && z->hpos > WSZ) {
        memmove(z->hb, z->hb + z->hpos - WSZ, WSZ);
        z->hpos = WSZ;
    }
    start = z->hpos;
    for (;;) {
        fill(z, &p, e);
        switch (z->state) {
        case S_HDR: {
            int type;
            if (z->bc < 3)
                goto out;
            z->last = (int)(z->bb & 1u);
            type = (int)((z->bb >> 1) & 3u);
            drop(z, 3);
            if (type == 0) {
                drop(z, z->bc & 7);
                z->state = S_SLEN;
            } else if (type == 1) {
                if (tables(z) < 0) {
                    r = DYN_INFS_OOM;
                    goto out;
                }
                fixed_tables(z);
                z->state = S_CODES;
            } else if (type == 2) {
                z->state = S_TAB;
            } else {
                r = DYN_INFS_BAD;
                goto out;
            }
            break;
        }
        case S_SLEN: {
            unsigned len, nlen;
            if (z->bc < 32)
                goto out;
            len = (unsigned)(z->bb & 0xffffu);
            nlen = (unsigned)((z->bb >> 16) & 0xffffu);
            drop(z, 32);
            if ((len ^ 0xffffu) != nlen) {
                r = DYN_INFS_BAD;
                goto out;
            }
            z->slen = len;
            z->state = S_SCOPY;
            break;
        }
        case S_SCOPY: {
            size_t room = z->hcap - z->hpos, k;
            while (z->slen && z->bc >= 8 && room) {
                z->hb[z->hpos++] = (uint8_t)(z->bb & 0xffu);
                drop(z, 8);
                z->slen--;
                room--;
            }
            if (z->slen && !z->bc) {
                k = (size_t)(e - p);
                if (k > z->slen)
                    k = z->slen;
                if (k > room)
                    k = room;
                memcpy(z->hb + z->hpos, p, k);
                z->hpos += k;
                p += k;
                z->slen -= (unsigned)k;
                room -= k;
            }
            if (z->slen) {
                if (!room)
                    r = DYN_INFS_FULL;
                goto out;
            }
            z->state = z->last ? S_DONE : S_HDR;
            break;
        }
        case S_TAB:
            if (z->bc < 14)
                goto out;
            z->hlit = (int)(z->bb & 31u) + 257;
            z->hdist = (int)((z->bb >> 5) & 31u) + 1;
            z->hclen = (int)((z->bb >> 10) & 15u) + 4;
            drop(z, 14);
            if (z->hlit > 286 || z->hdist > 30) {
                r = DYN_INFS_BAD;
                goto out;
            }
            memset(z->lens, 0, 19 * sizeof(short));
            z->idx = 0;
            z->state = S_CLEN;
            break;
        case S_CLEN:
            while (z->idx < z->hclen) {
                if (z->bc < 3)
                    goto out;
                z->lens[clorder[z->idx++]] = (short)(z->bb & 7u);
                drop(z, 3);
            }
            if (hbuild(z->ct, CB, 1u << CB, 1u << CB, z->lens, 19, K_CL, cnt) != 0) {
                r = DYN_INFS_BAD;
                goto out;
            }
            z->idx = 0;
            z->state = S_LENS;
            break;
        case S_LENS: {
            int total = z->hlit + z->hdist, err;
            while (z->idx < total) {
                unsigned ul, x;
                int sym, rep, pr;
                uint32_t ent;
                short val = 0;
                fill(z, &p, e);
                pr = tpeek(z->ct, CB, z->bb, z->bc, &ent);
                if (pr == DYN_INFS_MORE)
                    goto out;
                if (pr < 0) {
                    r = DYN_INFS_BAD;
                    goto out;
                }
                sym = (int)E_VAL(ent);
                ul = E_LEN(ent);
                if (sym < 16) {
                    drop(z, ul);
                    z->lens[z->idx++] = (short)sym;
                    continue;
                }
                x = sym == 16 ? 2 : sym == 17 ? 3 : 7;
                if (ul + x > z->bc)
                    goto out;
                rep = (sym == 16 ? 3 : sym == 17 ? 3 : 11) + (int)((z->bb >> ul) & ((1u << x) - 1u));
                if (sym == 16) {
                    if (z->idx == 0) {
                        r = DYN_INFS_BAD;
                        goto out;
                    }
                    val = z->lens[z->idx - 1];
                }
                if (z->idx + rep > total) {
                    r = DYN_INFS_BAD;
                    goto out;
                }
                drop(z, ul + x);
                while (rep--)
                    z->lens[z->idx++] = val;
            }
            if (z->lens[256] == 0) {
                r = DYN_INFS_BAD;
                goto out;
            }
            if (tables(z) < 0) {
                r = DYN_INFS_OOM;
                goto out;
            }
            z->lt_fixed = 0;
            err = hbuild(z->lt, LB, LT_CAP + DT_CAP, (1u << LB) + DT_CAP, z->lens, z->hlit, K_LIT, cnt);
            if (err && (err < 0 || z->hlit != cnt[0] + cnt[1])) {
                r = DYN_INFS_BAD;
                goto out;
            }
            err = hbuild(z->dt, DB, DT_CAP, 1u << DB, z->lens + z->hlit, z->hdist, K_DIST, cnt);
            if (err && (err < 0 || z->hdist != cnt[0] + cnt[1])) {
                r = DYN_INFS_BAD;
                goto out;
            }
            z->state = S_CODES;
            break;
        }
        case S_CODES: {
            int c = codes(z, &p, e);
            if (c != 3) {
                r = c;
                goto out;
            }
            break;
        }
        case S_DONE:
            drop(z, z->bc & 7);
            r = DYN_INFS_END;
            goto out;
        }
    }
out:
    *used = (size_t)(p - in);
    *out = z->hb + start;
    *outn = z->hpos - start;
    return r;
}

size_t dyn_infs_tail(dyn_infs_t* z, uint8_t* buf)
{
    size_t k = 0;
    while (z->bc >= 8) {
        buf[k++] = (uint8_t)(z->bb & 0xffu);
        drop(z, 8);
    }
    return k;
}
