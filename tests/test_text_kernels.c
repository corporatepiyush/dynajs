/*
 * test_text_kernels.c -- differential regression test for the shared SIMD
 * byte/text kernels added for dyna:text:
 *   hex_encode, hex_decode, latin1_to_utf8, utf8_to_latin1, count_utf8.
 *
 * Each kernel is checked TWO ways against an independent scalar reference:
 *   1. sc.*   -- the in-repo scalar kernel table (portable C; validated on
 *                every platform, including under qemu).
 *   2. simd.* -- the active best-ISA table chosen by simd_init(): NEON on the
 *                arm64 host, SSE4.2 under QEMU_CPU=Nehalem, AVX2 under
 *                QEMU_CPU=Haswell. Running this one binary on all three
 *                platforms is what proves the x86 kernels (never exercised on
 *                the arm64 dev host) match scalar byte-for-byte.
 *
 * OOB coverage (works WITHOUT ASan, so it is meaningful under qemu):
 *   - INPUT is placed flush against an unmapped guard page, so any read past
 *     src[n) SIGSEGVs (guards the historical "OOB block/seed load" bug class).
 *   - the device-under-test OUTPUT is placed flush against a second guard page
 *     sized to the exact expected length, so any over-write SIGSEGVs.
 *
 * Build + run natively (arm64 NEON / x86 host):
 *   clang -O2 -Isrc tests/test_text_kernels.c \
 *     src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
 *     src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
 *     src/dyna-simd-sve.c -lpthread -lm -o /tmp/ttk && /tmp/ttk
 * Exercise the x86 kernels under emulation:
 *   docker run --rm --platform linux/amd64 -e QEMU_CPU=Nehalem -v "$PWD":/src \
 *     -w /src dynascript-dev:amd64 bash -c '<build line above>; /tmp/ttk'   # SSE4.2
 *   docker run --rm --platform linux/amd64 -e QEMU_CPU=Haswell -v "$PWD":/src \
 *     -w /src dynascript-dev:amd64 bash -c '<build line above>; /tmp/ttk'   # AVX2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

/* ── independent scalar references (deliberately not shared with the kernels) ── */
static const char REFHEX[] = "0123456789abcdef";
static void ref_hex_encode(const uint8_t *s, size_t n, char *d) {
  for (size_t i = 0; i < n; i++) {
    d[2 * i] = REFHEX[s[i] >> 4];
    d[2 * i + 1] = REFHEX[s[i] & 0x0F];
  }
}
static int ref_hv(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static size_t ref_hex_decode(const char *s, size_t n, uint8_t *d) {
  size_t o = 0;
  if (n & 1) return SIZE_MAX;
  for (size_t i = 0; i < n; i += 2) {
    int hi = ref_hv((uint8_t)s[i]), lo = ref_hv((uint8_t)s[i + 1]);
    if (hi < 0 || lo < 0) return SIZE_MAX;
    d[o++] = (uint8_t)((hi << 4) | lo);
  }
  return o;
}
static size_t ref_l2u(const uint8_t *s, size_t n, uint8_t *d) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x80) d[o++] = c;
    else { d[o++] = (uint8_t)(0xC0 | (c >> 6)); d[o++] = (uint8_t)(0x80 | (c & 0x3F)); }
  }
  return o;
}
static int ref_u2l(const uint8_t *s, size_t n, uint8_t *d, size_t *ol) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t c = s[i];
    if (c < 0x80) { d[o++] = c; i++; continue; }
    if ((c & 0xE0) == 0xC0) {
      uint8_t c1; uint32_t cp;
      if (i + 1 >= n) return -1;
      c1 = s[i + 1];
      if ((c1 & 0xC0) != 0x80) return -1;
      cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
      if (cp < 0x80 || cp > 0xFF) return -1;
      d[o++] = (uint8_t)cp; i += 2; continue;
    }
    return -1;
  }
  *ol = o; return 0;
}
static size_t ref_cu(const uint8_t *s, size_t n) {
  size_t c = 0;
  for (size_t i = 0; i < n; i++) if ((s[i] & 0xC0) != 0x80) c++;
  return c;
}

/* ── guard-page regions: USABLE pages then one PROT_NONE page ─────────────── */
#define USABLE_PAGES 4
static long PG;
static uint8_t *in_end, *out_end; /* one-past-usable == start of guard page */

static uint8_t *mkregion(void) {
  uint8_t *b = mmap(0, (USABLE_PAGES + 1) * PG, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (b == MAP_FAILED) { perror("mmap"); exit(2); }
  if (mprotect(b + USABLE_PAGES * PG, PG, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
  return b;
}
/* copy `n` bytes so they end flush against the input guard page */
static uint8_t *in_slot(const void *src, size_t n) {
  uint8_t *p = in_end - n;
  memcpy(p, src, n);
  return p;
}
/* a writable slot of exactly `n` bytes ending flush against the output guard */
static uint8_t *out_slot(size_t n) { return out_end - n; }

static uint32_t rs = 0x2545F491u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int fails = 0;
static long cases = 0;
static void fail(const char *w, size_t n) {
  if (fails++ < 25) printf("FAIL %s n=%zu\n", w, n);
}

/* fill `buf` with `n` bytes under distribution `mode` */
static void gen_bytes(uint8_t *buf, size_t n, int mode) {
  static const uint8_t bd[] = {0x00, 0x7F, 0x80, 0xFF, 0xC2, 0xC3, 0x41, 0xBF};
  for (size_t i = 0; i < n; i++) {
    uint32_t r = rnd();
    switch (mode) {
      case 0: buf[i] = (uint8_t)r; break;               /* full random */
      case 1: buf[i] = (uint8_t)(r & 0x7F); break;      /* ASCII */
      case 2: buf[i] = (uint8_t)(0x80 | (r & 0x7F)); break; /* high bytes */
      default: buf[i] = bd[r & 7]; break;               /* boundary bytes */
    }
  }
}
/* append the UTF-8 of code point cp to buf[*o] */
static void put_cp(uint8_t *buf, size_t *o, uint32_t cp) {
  if (cp < 0x80) buf[(*o)++] = (uint8_t)cp;
  else if (cp < 0x800) {
    buf[(*o)++] = (uint8_t)(0xC0 | (cp >> 6));
    buf[(*o)++] = (uint8_t)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    buf[(*o)++] = (uint8_t)(0xE0 | (cp >> 12));
    buf[(*o)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    buf[(*o)++] = (uint8_t)(0x80 | (cp & 0x3F));
  } else {
    buf[(*o)++] = (uint8_t)(0xF0 | (cp >> 18));
    buf[(*o)++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    buf[(*o)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    buf[(*o)++] = (uint8_t)(0x80 | (cp & 0x3F));
  }
}

static const size_t LENS[] = {0,1,2,3,4,5,6,7,8,9,15,16,17,18,31,32,33,34,
                              47,48,63,64,65,66,95,96,127,128,129,130,255,
                              256,257,511,512,513,1000};
#define NLENS (sizeof(LENS) / sizeof(LENS[0]))

/* scratch heap buffers for the two reference computations */
static uint8_t rbuf[4096], sbuf[4096];
static char rce[8192];       /* ref hex-encode chars */
static uint8_t gen[4096];    /* generation scratch */

int main(void) {
  simd_init();
  simd_t sc; simd_override_scalar(&sc);
  unsigned long long c = cpu_features();
  printf("caps=0x%llx SSE42=%d AVX2=%d AVX512=%d NEON=%d\n", c,
         !!(c & CPU_SSE42), !!(c & CPU_AVX2), !!(c & CPU_AVX512F), !!(c & CPU_NEON));
  PG = sysconf(_SC_PAGESIZE);
  { uint8_t *r = mkregion(); in_end = r + USABLE_PAGES * PG; }
  { uint8_t *r = mkregion(); out_end = r + USABLE_PAGES * PG; }

  for (size_t li = 0; li < NLENS; li++) {
    size_t n = LENS[li];
    if (n > 2048) n = 2048;                 /* keep 2n within the guard region */
    for (int trial = 0; trial < 30; trial++) {

      /* ---- hex_encode: generic byte input, output exactly 2n ---- */
      for (int mode = 0; mode < 4; mode++) {
        gen_bytes(gen, n, mode);
        const uint8_t *in = in_slot(gen, n);
        char *rr = rce;                     /* ref output on the heap */
        char *ds = (char *)out_slot(2 * n); /* DUT output flush to guard */
        ref_hex_encode(in, n, rr);
        sc.hex_encode(in, n, (char *)sbuf);
        if (memcmp(sbuf, rr, 2 * n) != 0) fail("hex_encode/sc", n);
        simd.hex_encode(in, n, ds);
        if (memcmp(ds, rr, 2 * n) != 0) fail("hex_encode/simd", n);
        cases++;
      }

      /* ---- count_utf8: generic + valid-utf8 inputs ---- */
      for (int mode = 0; mode < 4; mode++) {
        gen_bytes(gen, n, mode);
        const uint8_t *in = in_slot(gen, n);
        size_t rc = ref_cu(in, n);
        if (sc.count_utf8(in, n) != rc) fail("count_utf8/sc", n);
        if (simd.count_utf8(in, n) != rc) fail("count_utf8/simd", n);
        cases++;
      }
      /* count_utf8 over genuinely valid UTF-8 (1..4-byte code points): the
       * count must equal the number of code points emitted. */
      {
        size_t un = 0, ncp = 0;
        while (un + 4 <= n) {
          uint32_t cp;
          do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
          put_cp(gen, &un, cp);
          ncp++;
        }
        const uint8_t *in = in_slot(gen, un);
        if (ref_cu(in, un) != ncp) fail("count_utf8/oracle", un); /* sanity */
        if (sc.count_utf8(in, un) != ncp) fail("count_utf8/sc(valid)", un);
        if (simd.count_utf8(in, un) != ncp) fail("count_utf8/simd(valid)", un);
        cases++;
      }

      /* ---- latin1_to_utf8: generic byte input, output up to 2n ---- */
      for (int mode = 0; mode < 4; mode++) {
        gen_bytes(gen, n, mode);
        const uint8_t *in = in_slot(gen, n);
        size_t rl = ref_l2u(in, n, rbuf);
        size_t sl = sc.latin1_to_utf8(in, n, sbuf);
        if (sl != rl || memcmp(sbuf, rbuf, rl) != 0) fail("latin1_to_utf8/sc", n);
        {
          uint8_t *ds = out_slot(rl);
          size_t dl = simd.latin1_to_utf8(in, n, ds);
          if (dl != rl || memcmp(ds, rbuf, rl) != 0) fail("latin1_to_utf8/simd", n);
        }
        cases++;
      }

      /* ---- utf8_to_latin1: (a) valid latin1->utf8 round-trip, (b) hostile ---- */
      {
        /* (a) build valid latin1, expand to utf8, expect exact recovery */
        gen_bytes(gen, n, trial & 1 ? 2 : 0); /* many high bytes half the time */
        size_t un = ref_l2u(gen, n, rbuf);    /* rbuf = valid utf8, len un */
        if (un <= 2048) {
          const uint8_t *in = in_slot(rbuf, un);
          size_t rol = 0, sol = 0, dol = 0;
          int rr = ref_u2l(in, un, sbuf /*unused*/, &rol);
          int sr = sc.utf8_to_latin1(in, un, sbuf, &sol);
          if (sr != rr || (rr == 0 && (sol != rol || memcmp(sbuf, gen, rol) != 0)))
            fail("utf8_to_latin1/sc(valid)", un);
          {
            uint8_t *ds = out_slot(un); /* output <= input length */
            int dr = simd.utf8_to_latin1(in, un, ds, &dol);
            if (dr != rr || (rr == 0 && (dol != rol || memcmp(ds, gen, rol) != 0)))
              fail("utf8_to_latin1/simd(valid)", un);
          }
          cases++;
        }
        /* (b) hostile: arbitrary bytes -- must agree on 0/-1 (and bytes if 0) */
        for (int mode = 0; mode < 4; mode++) {
          gen_bytes(gen, n, mode);
          const uint8_t *in = in_slot(gen, n);
          size_t rol = 0, sol = 0, dol = 0;
          int rr2 = ref_u2l(in, n, rbuf, &rol);
          int sr2 = sc.utf8_to_latin1(in, n, sbuf, &sol);
          if (sr2 != rr2 || (rr2 == 0 && (sol != rol || memcmp(sbuf, rbuf, rol) != 0)))
            fail("utf8_to_latin1/sc(hostile)", n);
          {
            uint8_t *ds = out_slot(n);
            int dr2 = simd.utf8_to_latin1(in, n, ds, &dol);
            if (dr2 != rr2 || (rr2 == 0 && (dol != rol || memcmp(ds, rbuf, rol) != 0)))
              fail("utf8_to_latin1/simd(hostile)", n);
          }
          cases++;
        }
      }

      /* ---- hex_decode: (0) valid hex, (1) one injected bad char, (2) random
       *      bytes (mostly invalid) -- output up to n/2 ---- */
      {
        static const char HD[] = "0123456789abcdefABCDEF";
        for (int variant = 0; variant < 3; variant++) {
          for (size_t k = 0; k < n; k++) {
            if (variant == 2) gen[k] = (uint8_t)rnd();            /* random bytes */
            else gen[k] = (uint8_t)HD[rnd() % (sizeof(HD) - 1)];  /* valid hex */
          }
          if (variant == 1 && n > 0)
            gen[rnd() % n] = (uint8_t)(0x67 + (rnd() & 3)); /* g..j: not hex */
          const uint8_t *in = in_slot(gen, n);
          size_t rl = ref_hex_decode((const char *)in, n, rbuf);
          size_t sl = sc.hex_decode((const char *)in, n, sbuf);
          if (sl != rl || (rl != SIZE_MAX && memcmp(sbuf, rbuf, rl) != 0))
            fail("hex_decode/sc", n);
          {
            size_t osz = (rl == SIZE_MAX) ? n / 2 : rl; /* DUT writes <= n/2 */
            uint8_t *ds = out_slot(osz);
            size_t dl = simd.hex_decode((const char *)in, n, ds);
            if (dl != rl || (rl != SIZE_MAX && memcmp(ds, rbuf, rl) != 0))
              fail("hex_decode/simd", n);
          }
          cases++;
        }
        /* odd length always yields SIZE_MAX (kernels reject before any load) */
        if (n > 1) {
          size_t odd = (n & 1) ? n : n - 1;
          for (size_t k = 0; k < odd; k++)
            gen[k] = (uint8_t)HD[rnd() % (sizeof(HD) - 1)];
          const uint8_t *in = in_slot(gen, odd);
          if (sc.hex_decode((const char *)in, odd, sbuf) != SIZE_MAX)
            fail("hex_decode/sc(odd)", odd);
          if (simd.hex_decode((const char *)in, odd, (uint8_t *)out_slot(odd / 2)) != SIZE_MAX)
            fail("hex_decode/simd(odd)", odd);
          cases++;
        }
      }
    }
  }

  if (fails == 0)
    printf("OK: %ld differential cases, all kernels match scalar reference, no guard-page fault\n", cases);
  else
    printf("FAILS=%d (of %ld cases)\n", fails, cases);
  return fails ? 1 : 0;
}
