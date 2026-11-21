// covers: src/core/dyn-inflate-stream.c -- the streaming inflater behind DecompressionStream("gzip"|"deflate")
// Oracle: system zlib. Corpus = 10 data shapes (incl. 20 KB text/random runs: Huffman blocks followed by stored blocks) x sizes 0..3 MiB (past the 96 KiB output window, so the FULL/slide path
// runs) x levels 0/1/6/9 x strategies default/filtered/huffman/rle/fixed, raw deflate. Each stream is decoded whole,
// in 1-byte pieces and in random splits; output must be byte-identical and END must be reported with every input
// byte consumed. Then 4000 bit-flipped streams: whenever zlib reaches Z_STREAM_END our output must be identical and
// END; whenever zlib reports Z_DATA_ERROR or truncation we must not report END, and our output so far must match zlib's.
// Every piece is copied into an exact-size heap block, so under ASan any read past the caller's input (the fast loop
// reads 8 bytes per refill and refills up to twice per iteration) is a heap-buffer-overflow.
// Exit 77 when zlib.h is missing (SKIP).
#if defined(__has_include)
#if !__has_include(<zlib.h>)
int main(void) { return 77; }
#define NO_ZLIB 1
#endif
#endif
#ifndef NO_ZLIB
#include "dyn-inflate-stream.h"
#include "dyn-hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static void gen(int shape, uint8_t* b, size_t n)
{
    static const char* words[] = { "the ", "stream ", "deflate ", "window ", "of ", "a ", "match ", "literal ", "\n", "huffman " };
    size_t i = 0, k;
    switch (shape) {
    case 0:
        memset(b, 0, n);
        break;
    case 1:
        for (i = 0; i < n; i++)
            b[i] = (uint8_t)rnd();
        break;
    case 2:
        while (i < n) {
            const char* w = words[rnd() % 10];
            for (k = 0; w[k] && i < n; k++)
                b[i++] = (uint8_t)w[k];
        }
        break;
    case 3:
        for (i = 0; i < n; i++)
            b[i] = (uint8_t)((uint32_t)(i * 2654435761u) >> 26);
        break;
    case 9:
        for (i = 0; i < n; i++)
            b[i] = (i / 20000) & 1 ? (uint8_t)rnd() : (uint8_t)("abcabdabe"[i % 9]);
        break;
    default: {
        size_t per = (size_t)(shape - 3);
        for (i = 0; i < n; i++)
            b[i] = (uint8_t)(i < per ? rnd() : b[i - per]);
        if (shape == 8)
            for (i = 0; i < n; i += 1 + rnd() % 97)
                b[i] = (uint8_t)rnd();
        break;
    }
    }
}

static size_t zdeflate(const uint8_t* in, size_t n, uint8_t* out, size_t cap, int level, int strat)
{
    z_stream s;
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, level, Z_DEFLATED, -15, 9, strat) != Z_OK)
        return 0;
    s.next_in = (Bytef*)in;
    s.avail_in = (uInt)n;
    s.next_out = out;
    s.avail_out = (uInt)cap;
    if (deflate(&s, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&s);
        return 0;
    }
    n = s.total_out;
    deflateEnd(&s);
    return n;
}

static int zinflate(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* outn)
{
    z_stream s;
    int r;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -15) != Z_OK)
        return -99;
    s.next_in = (Bytef*)in;
    s.avail_in = (uInt)n;
    s.next_out = out;
    s.avail_out = (uInt)cap;
    r = inflate(&s, Z_FINISH);
    *outn = s.total_out;
    inflateEnd(&s);
    return r;
}

static int ours(const uint8_t* in, size_t n, int mode, uint8_t* out, size_t cap, size_t* outn, size_t* consumed)
{
    dyn_infs_t* z = dyn_infs_new();
    size_t off = 0, on = 0;
    int r = DYN_INFS_MORE;
    while (r == DYN_INFS_MORE || r == DYN_INFS_FULL) {
        size_t piece = n - off, used;
        const uint8_t* q;
        size_t k;
        if (mode == 1 && piece > 1)
            piece = 1;
        else if (mode == 2 && piece > 0)
            piece = 1 + rnd() % (piece < 70000 ? piece : 70000);
        if (r == DYN_INFS_MORE && off >= n && piece == 0) {
            r = dyn_infs_run(z, in + off, 0, &used, &q, &k);
            if (k && on + k <= cap)
                memcpy(out + on, q, k);
            on += k;
            if (r == DYN_INFS_MORE)
                break;
            continue;
        }
        {
            uint8_t* exact = (uint8_t*)malloc(piece ? piece : 1);
            memcpy(exact, in + off, piece);
            r = dyn_infs_run(z, exact, piece, &used, &q, &k);
            free(exact);
        }
        if (k) {
            if (on + k > cap) {
                r = -9;
                break;
            }
            memcpy(out + on, q, k);
        }
        on += k;
        off += used;
        if (r == DYN_INFS_MORE && used != piece) {
            r = -8;
            break;
        }
    }
    dyn_infs_free(z);
    *outn = on;
    *consumed = off;
    return r;
}

int main(void)
{
    static const size_t sizes[] = { 0, 1, 100, 70000, 300000, 3u << 20 };
    static const int levels[] = { 0, 1, 6, 9 };
    static const int strats[] = { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    size_t maxn = 3u << 20, ccap = maxn + maxn / 8 + 1024;
    uint8_t *raw = malloc(maxn), *comp = malloc(ccap), *got = malloc(maxn + 1024), *ref = malloc(maxn + 1024);
    int fails = 0, cases = 0, shape, si, li, ti, mode, fz;
    long agree_end = 0, agree_bad = 0, short_ok = 0;
    if (!raw || !comp || !got || !ref)
        return 2;
    for (shape = 0; shape < 10; shape++)
        for (si = 0; si < 6; si++) {
            size_t n = sizes[si];
            if (n > 300000 && shape != 1 && shape != 2 && shape != 3 && shape != 7 && shape != 9)
                continue;
            gen(shape, raw, n);
            for (li = 0; li < 4; li++)
                for (ti = 0; ti < 5; ti++) {
                    size_t cn = zdeflate(raw, n, comp, ccap, levels[li], strats[ti]), on, used;
                    if (!cn && n) {
                        printf("FAIL deflate shape %d n %zu\n", shape, n);
                        fails++;
                        continue;
                    }
                    for (mode = 0; mode < 3; mode++) {
                        int r;
                        if (mode == 1 && cn > 400000)
                            continue;
                        cases++;
                        r = ours(comp, cn, mode, got, maxn + 1024, &on, &used);
                        if (r != DYN_INFS_END || on != n || used != cn || memcmp(got, raw, n)) {
                            printf("FAIL shape %d n %zu level %d strat %d mode %d: r %d out %zu used %zu/%zu\n", shape, n, levels[li], strats[ti], mode, r, on, used, cn);
                            fails++;
                        }
                    }
                }
        }
    for (fz = 0; fz < 4000; fz++) {
        size_t n = 1 + rnd() % 200000, cn, zn, on, used;
        int zr, r, flips = 1 + (int)(rnd() % 3), f;
        shape = (int)(rnd() % 10);
        gen(shape, raw, n);
        cn = zdeflate(raw, n, comp, ccap, levels[rnd() % 4], strats[rnd() % 5]);
        for (f = 0; f < flips && cn; f++)
            comp[rnd() % cn] ^= (uint8_t)(1u << (rnd() % 8));
        zr = zinflate(comp, cn, ref, maxn + 1024, &zn);
        r = ours(comp, cn, (int)(rnd() % 3 == 0 ? 2 : 0), got, maxn + 1024, &on, &used);
        cases++;
        if (zr == Z_STREAM_END) {
            if (r != DYN_INFS_END || on != zn || memcmp(got, ref, zn)) {
                printf("FAIL fuzz %d: zlib END %zu, ours r %d out %zu\n", fz, zn, r, on);
                fails++;
            } else
                agree_end++;
        } else if (zr == Z_DATA_ERROR) {
            if (r == DYN_INFS_END) {
                printf("FAIL fuzz %d: zlib DATA_ERROR, ours END out %zu\n", fz, on);
                fails++;
            } else if (r == DYN_INFS_BAD)
                agree_bad++;
            else
                short_ok++;
            if (on > zn + 300 || memcmp(got, ref, on < zn ? on : zn)) {
                printf("FAIL fuzz %d: output before the error differs (ours %zu zlib %zu)\n", fz, on, zn);
                fails++;
            }
        } else if (zr == Z_BUF_ERROR) {
            if (r == DYN_INFS_END) {
                printf("FAIL fuzz %d: zlib truncated, ours END\n", fz);
                fails++;
            } else
                short_ok++;
        }
    }
    {
        uint8_t z0[4] = { 0, 0, 0, 0 };
        uint32_t c = dyn_crc32_update(0, (const uint8_t*)"123456789", 9);
        uint32_t c2 = dyn_crc32_update(dyn_crc32_update(0, (const uint8_t*)"1234", 4), (const uint8_t*)"56789", 5);
        cases++;
        if (c != 0xCBF43926u || c2 != c || dyn_crc32_update(0, z0, 0) != 0) {
            printf("FAIL crc32 %08x %08x\n", c, c2);
            fails++;
        }
        gen(1, raw, 1 << 20);
        cases++;
        if (dyn_crc32_update(0, raw, 1 << 20) != (uint32_t)crc32(0, raw, 1 << 20)
            || dyn_crc32_update(dyn_crc32_update(0, raw, 777), raw + 777, (1 << 20) - 777) != (uint32_t)crc32(0, raw, 1 << 20)) {
            printf("FAIL crc32 vs zlib\n");
            fails++;
        }
    }
    printf("%s: %d cases, %d fails (fuzz: %ld both END, %ld both BAD, %ld zlib error / ours short)\n", fails ? "FAIL" : "PASS", cases, fails, agree_end, agree_bad, short_ok);
    free(raw);
    free(comp);
    free(got);
    free(ref);
    return fails ? 1 : 0;
}
#endif
