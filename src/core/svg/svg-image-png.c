#include "svg-int.h"

typedef struct {
    const unsigned char* d;
    size_t n, pos;
    unsigned buf;
    int cnt;
} Bits;

static int getbits(Bits* b, int need) {
    while (b->cnt < need) {
        if (b->pos >= b->n) return -1;
        b->buf |= (unsigned)b->d[b->pos++] << b->cnt;
        b->cnt += 8;
    }
    int v = (int)(b->buf & ((1u << need) - 1u));
    b->buf >>= need;
    b->cnt -= need;
    return v;
}

static void align_byte(Bits* b) {
    int drop = b->cnt & 7;
    b->buf >>= drop;
    b->cnt -= drop;
}

typedef struct { short count[16]; short symbol[288]; } Huff;

static void huff_build(Huff* h, const unsigned char* len, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    short left[16];
    int total = 0;
    for (int i = 1; i < 16; i++) { left[i] = (short)total; total += h->count[i]; }
    for (int i = 0; i < n; i++)
        if (len[i]) h->symbol[left[len[i]]++] = (short)i;
}

static int huff_decode(Bits* b, const Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int bit = getbits(b, 1);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const unsigned short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const unsigned char DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

typedef struct { unsigned char* p; size_t n, cap, limit; } Buf;

static int buf_push(Buf* b, unsigned char c) {
    if (b->limit && b->n >= b->limit) return -1;
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        if (b->limit && nc > b->limit) nc = b->limit;
        unsigned char* np = (unsigned char*)svg_realloc(b->p, nc);
        if (!np) return -1;
        b->p = np;
        b->cap = nc;
    }
    b->p[b->n++] = c;
    return 0;
}

static int inflate_codes(Bits* b, Buf* out, const Huff* lit, const Huff* dist) {
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (buf_push(out, (unsigned char)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = LEN_EXTRA[sym];
            int length = LEN_BASE[sym] + (e ? getbits(b, e) : 0);
            if (length < 0) return -1;
            int dsym = huff_decode(b, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int de = DIST_EXTRA[dsym];
            int d = DIST_BASE[dsym] + (de ? getbits(b, de) : 0);
            if (d < 0 || (size_t)d > out->n) return -1;
            for (int i = 0; i < length; i++)
                if (buf_push(out, out->p[out->n - (size_t)d])) return -1;
        }
    }
}

static int inflate_raw(Bits* b, Buf* out) {
    int final;
    do {
        final = getbits(b, 1);
        int type = getbits(b, 2);
        if (final < 0 || type < 0) return -1;
        if (type == 0) {
            align_byte(b);
            int len = getbits(b, 16);
            int nlen = getbits(b, 16);
            if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen) return -1;
            for (int i = 0; i < len; i++) {
                int c = getbits(b, 8);
                if (c < 0) return -1;
                if (buf_push(out, (unsigned char)c)) return -1;
            }
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {
                unsigned char l[288];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                huff_build(&lit, l, 288);
                unsigned char d[30];
                for (int k = 0; k < 30; k++) d[k] = 5;
                huff_build(&dist, d, 30);
            } else {
                static const unsigned char order[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                int hlit = getbits(b, 5);
                int hdist = getbits(b, 5);
                int hclen = getbits(b, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                hlit += 257;
                hdist += 1;
                hclen += 4;
                unsigned char cl[19];
                memset(cl, 0, sizeof(cl));
                for (int i = 0; i < hclen; i++) {
                    int v = getbits(b, 3);
                    if (v < 0) return -1;
                    cl[order[i]] = (unsigned char)v;
                }
                Huff clh;
                huff_build(&clh, cl, 19);
                unsigned char lengths[320];
                memset(lengths, 0, sizeof(lengths));
                int n = hlit + hdist;
                for (int i = 0; i < n; ) {
                    int sym = huff_decode(b, &clh);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lengths[i++] = (unsigned char)sym;
                    } else if (sym == 16) {
                        if (i == 0) return -1;
                        int e = getbits(b, 2);
                        if (e < 0) return -1;
                        unsigned char prev = lengths[i - 1];
                        for (int k = 0; k < 3 + e && i < n; k++) lengths[i++] = prev;
                    } else if (sym == 17) {
                        int e = getbits(b, 3);
                        if (e < 0) return -1;
                        for (int k = 0; k < 3 + e && i < n; k++) lengths[i++] = 0;
                    } else {
                        int e = getbits(b, 7);
                        if (e < 0) return -1;
                        for (int k = 0; k < 11 + e && i < n; k++) lengths[i++] = 0;
                    }
                }
                huff_build(&lit, lengths, hlit);
                huff_build(&dist, lengths + hlit, hdist);
            }
            if (inflate_codes(b, out, &lit, &dist)) return -1;
        } else {
            return -1;
        }
    } while (!final);
    return 0;
}

static int zlib_inflate(const unsigned char* d, size_t n, Buf* out) {
    if (n < 2) return -1;
    Bits b = { d + 2, n - 2, 0, 0, 0 };
    return inflate_raw(&b, out);
}

static unsigned be32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int sample_bits(const unsigned char* row, unsigned x, int depth) {
    size_t bit = (size_t)x * (size_t)depth;
    size_t byte = bit >> 3;
    int shift = 8 - depth - (int)(bit & 7);
    unsigned mask = (1u << depth) - 1u;
    return (row[byte] >> shift) & (int)mask;
}

unsigned char* png_decode_rgba(const unsigned char* d, size_t n,
                               int* out_w, int* out_h) {
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 8 || memcmp(d, sig, 8)) return NULL;

    unsigned w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    Buf idat = {0};
    const unsigned char* plte = NULL;
    unsigned plte_n = 0;
    const unsigned char* trns = NULL;
    unsigned trns_len = 0;

    size_t p = 8;
    while (p + 12 <= n) {
        unsigned len = be32(d + p);
        const unsigned char* type = d + p + 4;
        const unsigned char* data = d + p + 8;
        if (p + 12 + (size_t)len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            ctype = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            plte = data;
            plte_n = len / 3;
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = data;
            trns_len = len;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat.n + len > idat.cap) {
                size_t nc = idat.cap ? idat.cap : 4096;
                while (nc < idat.n + len) nc *= 2;
                unsigned char* np = (unsigned char*)svg_realloc(idat.p, nc);
                if (!np) { svg_mfree(idat.p); return NULL; }
                idat.p = np;
                idat.cap = nc;
            }
            memcpy(idat.p + idat.n, data, len);
            idat.n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + (size_t)len;
    }

    if (w == 0 || h == 0 || interlace != 0) { svg_mfree(idat.p); return NULL; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
        svg_mfree(idat.p);
        return NULL;
    }
    int channels = (ctype == 0) ? 1 : (ctype == 2) ? 3 : (ctype == 4) ? 2 :
                   (ctype == 6) ? 4 : (ctype == 3) ? 1 : 0;

    if (channels == 0) { svg_mfree(idat.p); return NULL; }
    if (depth < 8 && ctype != 0 && ctype != 3) { svg_mfree(idat.p); return NULL; }
    if (ctype == 3 && (!plte || plte_n == 0)) { svg_mfree(idat.p); return NULL; }

    if ((size_t)w > (size_t)SVG2IMG_LIMIT_MAX_PIXELS / (size_t)h ||
        (size_t)w > (size_t)-1 / (size_t)h / 4) {
        svg_mfree(idat.p);
        return NULL;
    }

    size_t packed = ((size_t)w * (size_t)channels * (size_t)depth + 7) / 8;
    size_t stride = (depth == 8) ? (size_t)w * (size_t)channels : packed;
    if (h > ((size_t)-1 - 1) / (stride + 1)) { svg_mfree(idat.p); return NULL; }
    size_t need = (stride + 1) * (size_t)h;

    Buf raw = {0};
    raw.limit = need;
    if (zlib_inflate(idat.p, idat.n, &raw)) { svg_mfree(idat.p); svg_mfree(raw.p); return NULL; }
    svg_mfree(idat.p);
    if (raw.n < need) { svg_mfree(raw.p); return NULL; }

    unsigned char* img = (unsigned char*)svg_malloc((size_t)w * (size_t)h * 4);
    if (!img) { svg_mfree(raw.p); return NULL; }

    unsigned char* prev = (unsigned char*)svg_malloc(stride);
    unsigned char* cur = (unsigned char*)svg_malloc(stride);
    if (!prev || !cur) { svg_mfree(prev); svg_mfree(cur); svg_mfree(img); svg_mfree(raw.p); return NULL; }
    memset(prev, 0, stride);

    for (unsigned y = 0; y < h; y++) {
        const unsigned char* src = raw.p + (stride + 1) * (size_t)y;
        int filter = src[0];
        memcpy(cur, src + 1, stride);
        for (size_t x = 0; x < stride; x++) {
            int a = (x >= (size_t)channels) ? cur[x - (size_t)channels] : 0;
            int b = prev[x];
            int c = (x >= (size_t)channels) ? prev[x - (size_t)channels] : 0;
            switch (filter) {
                case 0: break;
                case 1: cur[x] = (unsigned char)(cur[x] + a); break;
                case 2: cur[x] = (unsigned char)(cur[x] + b); break;
                case 3: cur[x] = (unsigned char)(cur[x] + ((a + b) >> 1)); break;
                case 4: cur[x] = (unsigned char)(cur[x] + paeth(a, b, c)); break;
                default: svg_mfree(prev); svg_mfree(cur); svg_mfree(img); svg_mfree(raw.p); return NULL;
            }
        }
        for (unsigned x = 0; x < w; x++) {
            unsigned char* o = img + ((size_t)y * w + x) * 4;
            if (ctype == 6) {
                o[0] = cur[x * 4]; o[1] = cur[x * 4 + 1];
                o[2] = cur[x * 4 + 2]; o[3] = cur[x * 4 + 3];
            } else if (ctype == 2) {
                o[0] = cur[x * 3]; o[1] = cur[x * 3 + 1];
                o[2] = cur[x * 3 + 2]; o[3] = 255;
            } else if (ctype == 0) {
                int v = depth == 8 ? cur[x] : sample_bits(cur, x, depth);
                v = depth == 8 ? v : v * 255 / ((1 << depth) - 1);
                o[0] = o[1] = o[2] = (unsigned char)v;
                o[3] = 255;
            } else if (ctype == 4) {
                o[0] = o[1] = o[2] = cur[x * 2];
                o[3] = cur[x * 2 + 1];
            } else {
                int idx = depth == 8 ? cur[x] : sample_bits(cur, x, depth);
                if ((unsigned)idx >= plte_n) idx = 0;
                o[0] = plte[(size_t)idx * 3 + 0];
                o[1] = plte[(size_t)idx * 3 + 1];
                o[2] = plte[(size_t)idx * 3 + 2];
                o[3] = (trns && (size_t)idx < trns_len) ? trns[idx] : 255;
            }
        }
        unsigned char* t = prev; prev = cur; cur = t;
    }

    svg_mfree(prev);
    svg_mfree(cur);
    svg_mfree(raw.p);
    *out_w = (int)w;
    *out_h = (int)h;
    return img;
}
