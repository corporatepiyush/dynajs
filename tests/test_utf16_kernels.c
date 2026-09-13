/*
 * test_utf16_kernels.c -- differential regression test for the shared SIMD
 * UTF-8 <-> UTF-16 transcode + validation kernels added for dyna:text:
 *   utf8_to_utf16le, utf16le_to_utf8, validate_utf16le, count_utf16.
 *
 * Each kernel is checked against an independent scalar reference TWO ways:
 *   1. sc.*   -- the in-repo scalar kernel table (portable C; validated on
 *                every platform, including under emulation).
 *   2. simd.* -- the active best-ISA table chosen by simd_init(): NEON on the
 *                arm64 host, SSE4.2 under an emulated amd64 that caps at SSE4.2
 *                (OrbStack/Rosetta). Running this one binary on both platforms
 *                is what proves the x86 kernels (never exercised on the arm64
 *                dev host) match scalar byte-for-byte. (AVX2 UTF-16 kernels are
 *                gated OFF -- unverifiable here -- so AVX2 hardware runs SSE4.2.)
 *
 * Beyond the sc-vs-simd-vs-ref differential this asserts the round-trip
 * PROPERTY (utf8->utf16->utf8 == identity, and utf16->utf8->utf16 == identity
 * for well-formed input), cross-checks count_utf16(utf8_to_utf16(s)) against the
 * engine's own count_utf8(s), and pins hand-computed surrogate edge cases.
 *
 * Invalid-UTF-16 policy under test: STRICT / lossless (simdutf convert
 * semantics). A lone/misordered surrogate (or a high surrogate at end of
 * buffer) makes the input ill-formed; the transcoders REJECT it (return -1) and
 * validate_utf16le returns 0. No U+FFFD substitution. UTF-16 units are
 * host-endian uint16_t (== UTF-16LE on the LE targets this engine supports).
 *
 * OOB coverage (works WITHOUT ASan, so it is meaningful under emulation):
 *   - INPUT is placed flush against an unmapped guard page, so any read past
 *     the input end SIGSEGVs.
 *   - each device-under-test OUTPUT is placed flush against a second guard page
 *     sized to the exact expected length, so any over-write SIGSEGVs.
 *
 * Build + run natively (arm64 NEON / x86 host):
 *   clang -O2 -Isrc tests/test_utf16_kernels.c \
 *     src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
 *     src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
 *     src/dyna-simd-sve.c -lpthread -lm -o /tmp/tuk && /tmp/tuk
 * Exercise the SSE4.2 kernels under emulated amd64 (OrbStack/Rosetta):
 *   docker run --rm --platform linux/amd64 -v "$PWD":/host:ro dynascript-dev:amd64 \
 *     bash -c 'cp -r /host /b && cd /b && <build line above>; /tmp/tuk'
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dyna-simd-kernels.h"

/* ── independent scalar references (deliberately not shared with the kernels) ── */
static int ref_u8_to_u16(const uint8_t *s, size_t n, uint16_t *d, size_t *ol) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t c = s[i];
    size_t len, j;
    uint32_t cp;
    if (c < 0x80) { d[o++] = c; i++; continue; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return -1;
    if (i + len > n) return -1;
    for (j = 1; j < len; j++) {
      uint8_t cc = s[i + j];
      if ((cc & 0xC0) != 0x80) return -1;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 2 && cp < 0x80) return -1;
    if (len == 3 && cp < 0x800) return -1;
    if (len == 4 && cp < 0x10000) return -1;
    if (cp > 0x10FFFF) return -1;
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
    if (cp < 0x10000) d[o++] = (uint16_t)cp;
    else {
      cp -= 0x10000;
      d[o++] = (uint16_t)(0xD800 | (cp >> 10));
      d[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    }
    i += len;
  }
  *ol = o;
  return 0;
}
static int ref_u16_to_u8(const uint16_t *s, size_t units, uint8_t *d, size_t *ol) {
  size_t i = 0, o = 0;
  while (i < units) {
    uint32_t cp = s[i];
    if (cp < 0xD800 || cp > 0xDFFF) { i++; }
    else if (cp <= 0xDBFF) {
      uint32_t lo;
      if (i + 1 >= units) return -1;
      lo = s[i + 1];
      if (lo < 0xDC00 || lo > 0xDFFF) return -1;
      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      i += 2;
    } else return -1;
    if (cp < 0x80) d[o++] = (uint8_t)cp;
    else if (cp < 0x800) {
      d[o++] = (uint8_t)(0xC0 | (cp >> 6));
      d[o++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      d[o++] = (uint8_t)(0xE0 | (cp >> 12));
      d[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      d[o++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
      d[o++] = (uint8_t)(0xF0 | (cp >> 18));
      d[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
      d[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      d[o++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
  }
  *ol = o;
  return 0;
}
static int ref_validate_u16(const uint16_t *s, size_t units) {
  size_t i = 0;
  while (i < units) {
    uint32_t c = s[i];
    if (c < 0xD800 || c > 0xDFFF) { i++; continue; }
    if (c > 0xDBFF) return 0;
    if (i + 1 >= units) return 0;
    if (s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) return 0;
    i += 2;
  }
  return 1;
}
static size_t ref_count_u16(const uint16_t *s, size_t units) {
  size_t i, c = 0;
  for (i = 0; i < units; i++)
    if (s[i] < 0xDC00 || s[i] > 0xDFFF) c++;
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
/* copy `n` bytes so they end flush against the input guard page. When `n` is
 * even the returned pointer is 2-byte aligned (page end is aligned), so it is
 * safe to reinterpret as uint16_t for the UTF-16 kernels. */
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
  if (fails++ < 30) printf("FAIL %s n=%zu\n", w, n);
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

/* one random non-surrogate code point under `mode` */
static uint32_t gen_cp(int mode) {
  uint32_t r = rnd(), cp;
  switch (mode) {
    case 1: return r % 0x80;                                  /* ASCII */
    case 2: return (r & 7) ? (r % 0xD800) : (0x10000 + (r % 0x100000)); /* BMP + pairs */
    case 3: return 0x10000 + (r % 0x100000);                 /* non-BMP (pairs) */
    default:
      do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
      return cp;                                             /* any scalar value */
  }
}
/* fill valid UTF-8 to EXACTLY cap bytes (pad the remainder with ASCII) */
static size_t gen_valid_utf8(uint8_t *buf, size_t cap, int mode) {
  size_t o = 0;
  while (o + 4 <= cap) put_cp(buf, &o, gen_cp(mode));
  while (o < cap) buf[o++] = (uint8_t)(rnd() % 0x80);
  return o;
}
/* fill valid UTF-16 to EXACTLY cap units (pad the remainder with ASCII) */
static size_t gen_valid_utf16(uint16_t *buf, size_t cap, int mode) {
  size_t o = 0;
  while (o + 2 <= cap) {
    uint32_t cp = gen_cp(mode);
    if (cp < 0x10000) buf[o++] = (uint16_t)cp;
    else {
      cp -= 0x10000;
      buf[o++] = (uint16_t)(0xD800 | (cp >> 10));
      buf[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    }
  }
  while (o < cap) buf[o++] = (uint16_t)(rnd() % 0x80);
  return o;
}
/* arbitrary (mostly ill-formed) UTF-16 units, biased toward surrogates */
static void gen_hostile_u16(uint16_t *buf, size_t units, int mode) {
  for (size_t i = 0; i < units; i++) {
    uint32_t r = rnd();
    switch (mode) {
      case 0: buf[i] = (uint16_t)r; break;                          /* full random */
      case 1: buf[i] = (uint16_t)(0xD800 + (r % 0x800)); break;     /* any surrogate */
      case 2: buf[i] = (r & 1) ? (uint16_t)(0xD800 + (r % 0x400))   /* high surrogate */
                               : (uint16_t)(r % 0x80); break;       /* or ASCII */
      default: buf[i] = (uint16_t)(0xDC00 + (r % 0x400)); break;    /* low surrogate */
    }
  }
}
/* arbitrary (mostly ill-formed) UTF-8 bytes for the reject path */
static void gen_hostile_u8(uint8_t *buf, size_t n, int mode) {
  static const uint8_t bd[] = {0x00, 0x7F, 0x80, 0xFF, 0xC2, 0xE0, 0xF0, 0xBF};
  for (size_t i = 0; i < n; i++) {
    uint32_t r = rnd();
    buf[i] = mode == 0 ? (uint8_t)r
           : mode == 1 ? (uint8_t)(0x80 | (r & 0x7F))  /* high/continuation */
                       : bd[r & 7];                    /* boundary bytes */
  }
}

static const size_t LENS[] = {0,1,2,3,4,5,6,7,8,9,15,16,17,18,31,32,33,34,
                              47,48,63,64,65,66,95,96,127,128,129,130,255,
                              256,257,511,512,513,1000};
#define NLENS (sizeof(LENS) / sizeof(LENS[0]))

/* heap scratch for references and round-trips */
static uint8_t  u8ref[8192], u8gen[8192], u8back[8192];
static uint16_t u16ref[4096], u16gen[4096], u16back[4096];

static void eq_u16(const uint16_t *a, const uint16_t *b, size_t units,
                   const char *w) {
  if (memcmp(a, b, units * 2) != 0) fail(w, units);
}

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
    for (int trial = 0; trial < 24; trial++) {

      /* ==== A. utf8 -> utf16 on VALID UTF-8 (all four generation modes) ==== */
      for (int mode = 0; mode < 4; mode++) {
        size_t un = gen_valid_utf8(u8gen, n, mode);
        const uint8_t *in = in_slot(u8gen, un);
        size_t rol = 0, sol = 0, dol = 0;
        int rr = ref_u8_to_u16(in, un, u16ref, &rol);
        if (rr != 0) { fail("A/ref-should-be-valid", un); continue; }

        int sr = sc.utf8_to_utf16le(in, un, u16gen, &sol);
        if (sr != 0 || sol != rol) fail("utf8->utf16/sc(valid)", un);
        else eq_u16(u16gen, u16ref, rol, "utf8->utf16/sc bytes");

        { /* DUT output flush against the guard, sized to exactly rol units */
          uint16_t *ds = (uint16_t *)out_slot(rol * 2);
          int dr = simd.utf8_to_utf16le(in, un, ds, &dol);
          if (dr != 0 || dol != rol) fail("utf8->utf16/simd(valid)", un);
          else eq_u16(ds, u16ref, rol, "utf8->utf16/simd bytes");
        }

        /* round-trip: utf16 -> utf8 must recover the original UTF-8 bytes */
        { size_t bl = 0;
          int br = ref_u16_to_u8(u16ref, rol, u8back, &bl);
          if (br != 0 || bl != un || memcmp(u8back, u8gen, un) != 0)
            fail("roundtrip utf8->utf16->utf8", un);
        }
        /* cross-check: code-point count via UTF-16 == via UTF-8 (engine kernels) */
        if (simd.count_utf16(u16ref, rol) != simd.count_utf8(in, un))
          fail("countUtf16 vs countUtf8", un);
        if (sc.count_utf16(u16ref, rol) != ref_count_u16(u16ref, rol))
          fail("count_utf16/sc vs ref", rol);
        cases += 4;
      }

      /* ==== B. utf8 -> utf16 on HOSTILE bytes (agree on accept/reject) ==== */
      for (int mode = 0; mode < 3 && n <= 2048; mode++) {
        gen_hostile_u8(u8gen, n, mode);
        const uint8_t *in = in_slot(u8gen, n);
        size_t rol = 0, sol = 0, dol = 0;
        int rr = ref_u8_to_u16(in, n, u16ref, &rol);
        int sr = sc.utf8_to_utf16le(in, n, u16gen, &sol);
        if (sr != rr || (rr == 0 && (sol != rol || memcmp(u16gen, u16ref, rol * 2))))
          fail("utf8->utf16/sc(hostile)", n);
        { /* worst-case output size when accepted is rol units; when rejected the
           * kernel may write a partial prefix < n units, so size the slot n units */
          size_t osz = (rr == 0) ? rol : n;
          uint16_t *ds = (uint16_t *)out_slot(osz * 2);
          int dr = simd.utf8_to_utf16le(in, n, ds, &dol);
          if (dr != rr || (rr == 0 && (dol != rol || memcmp(ds, u16ref, rol * 2))))
            fail("utf8->utf16/simd(hostile)", n);
        }
        cases++;
      }

      /* ==== C. utf16 -> utf8 on VALID UTF-16 (all four generation modes) ==== */
      for (int mode = 0; mode < 4; mode++) {
        size_t un = gen_valid_utf16(u16gen, n <= 2000 ? n : 2000, mode);
        const uint16_t *in = (const uint16_t *)in_slot(u16gen, un * 2);
        size_t rol = 0, sol = 0, dol = 0;
        int rr = ref_u16_to_u8(in, un, u8ref, &rol);
        if (rr != 0) { fail("C/ref-should-be-valid", un); continue; }

        int sr = sc.utf16le_to_utf8(in, un, u8gen, &sol);
        if (sr != 0 || sol != rol || memcmp(u8gen, u8ref, rol) != 0)
          fail("utf16->utf8/sc(valid)", un);

        { uint8_t *ds = out_slot(rol);
          int dr = simd.utf16le_to_utf8(in, un, ds, &dol);
          if (dr != 0 || dol != rol || memcmp(ds, u8ref, rol) != 0)
            fail("utf16->utf8/simd(valid)", un);
        }
        /* round-trip: utf8 -> utf16 must recover the original units */
        { size_t bl = 0;
          int br = ref_u8_to_u16(u8ref, rol, u16back, &bl);
          if (br != 0 || bl != un || memcmp(u16back, in, un * 2) != 0)
            fail("roundtrip utf16->utf8->utf16", un);
        }
        /* valid input must validate; both tables and ref must agree */
        if (!sc.validate_utf16le(in, un) || !simd.validate_utf16le(in, un) ||
            !ref_validate_u16(in, un))
          fail("validate(valid)", un);
        cases += 4;
      }

      /* ==== D. utf16 -> utf8 on HOSTILE units (agree on accept/reject) ==== */
      for (int mode = 0; mode < 4; mode++) {
        size_t un = (n <= 2000 ? n : 2000);
        gen_hostile_u16(u16gen, un, mode);
        const uint16_t *in = (const uint16_t *)in_slot(u16gen, un * 2);
        size_t rol = 0, sol = 0, dol = 0;
        int rr = ref_u16_to_u8(in, un, u8ref, &rol);
        int sr = sc.utf16le_to_utf8(in, un, u8gen, &sol);
        if (sr != rr || (rr == 0 && (sol != rol || memcmp(u8gen, u8ref, rol))))
          fail("utf16->utf8/sc(hostile)", un);
        { size_t osz = (rr == 0) ? rol : un * 3; /* rejected: bounded by 3*units */
          uint8_t *ds = out_slot(osz ? osz : 1);
          int dr = simd.utf16le_to_utf8(in, un, ds, &dol);
          if (dr != rr || (rr == 0 && (dol != rol || memcmp(ds, u8ref, rol))))
            fail("utf16->utf8/simd(hostile)", un);
        }
        /* validate + count on the same hostile buffer must match the reference */
        { int rv = ref_validate_u16(in, un);
          if (sc.validate_utf16le(in, un) != rv ||
              simd.validate_utf16le(in, un) != rv)
            fail("validate(hostile)", un);
        }
        { size_t rcnt = ref_count_u16(in, un);
          if (sc.count_utf16(in, un) != rcnt || simd.count_utf16(in, un) != rcnt)
            fail("count(hostile)", un);
        }
        cases += 3;
      }
    }
  }

  /* ==== E. hand-computed surrogate / edge cases (independent of the refs) ==== */
  {
    struct { const uint16_t *u; size_t units; int valid; size_t cps;
             int transcodable; const uint8_t *utf8; size_t utf8len; const char *name; } E[] = {
#define C(...) ((const uint16_t[]){__VA_ARGS__})
#define B(...) ((const uint8_t[]){__VA_ARGS__})
      { C(0x0041, 0x0042), 2, 1, 2, 1, B(0x41, 0x42), 2, "ASCII AB" },
      { C(0x00E9), 1, 1, 1, 1, B(0xC3, 0xA9), 2, "U+00E9 e-acute" },
      { C(0x20AC), 1, 1, 1, 1, B(0xE2, 0x82, 0xAC), 3, "U+20AC euro" },
      { C(0xFEFF), 1, 1, 1, 1, B(0xEF, 0xBB, 0xBF), 3, "BOM U+FEFF" },
      { C(0xD83D, 0xDE00), 2, 1, 1, 1, B(0xF0, 0x9F, 0x98, 0x80), 4, "U+1F600 emoji" },
      { C(0xD800, 0xDC00), 2, 1, 1, 1, B(0xF0, 0x90, 0x80, 0x80), 4, "U+10000 min non-BMP" },
      { C(0xDBFF, 0xDFFF), 2, 1, 1, 1, B(0xF4, 0x8F, 0xBF, 0xBF), 4, "U+10FFFF max" },
      { C(0xD800), 1, 0, 1, 0, NULL, 0, "lone high surrogate" },
      { C(0xDC00), 1, 0, 0, 0, NULL, 0, "lone low surrogate" },
      { C(0xDC00, 0xD800), 2, 0, 1, 0, NULL, 0, "reversed pair low,high (0xD800 is not a low surrogate)" },
      { C(0xD800, 0x0041), 2, 0, 2, 0, NULL, 0, "high then ASCII" },
      { C(0xD800, 0xD800), 2, 0, 2, 0, NULL, 0, "high then high" },
      { C(0x0041, 0xD800), 2, 0, 2, 0, NULL, 0, "high at end of buffer" },
      { C(0x0041, 0x00E9, 0x4E2D), 3, 1, 3, 1, B(0x41, 0xC3, 0xA9, 0xE4, 0xB8, 0xAD), 6, "mixed ASCII/2/3-byte" },
#undef C
#undef B
    };
    for (size_t k = 0; k < sizeof(E) / sizeof(E[0]); k++) {
      /* validate + count: both tables agree with the pinned literal */
      if (sc.validate_utf16le(E[k].u, E[k].units) != E[k].valid) fail(E[k].name, 1000 + k);
      if (simd.validate_utf16le(E[k].u, E[k].units) != E[k].valid) fail(E[k].name, 2000 + k);
      if (sc.count_utf16(E[k].u, E[k].units) != E[k].cps) fail(E[k].name, 3000 + k);
      if (simd.count_utf16(E[k].u, E[k].units) != E[k].cps) fail(E[k].name, 4000 + k);
      /* transcode: match the pinned reject-or-bytes result on both tables */
      { uint8_t ob[16]; size_t ol = 0;
        int sr = sc.utf16le_to_utf8(E[k].u, E[k].units, ob, &ol);
        if (E[k].transcodable) {
          if (sr != 0 || ol != E[k].utf8len || memcmp(ob, E[k].utf8, ol) != 0)
            fail(E[k].name, 5000 + k);
        } else if (sr != -1) fail(E[k].name, 6000 + k);
      }
      { uint8_t ob[16]; size_t ol = 0;
        int dr = simd.utf16le_to_utf8(E[k].u, E[k].units, ob, &ol);
        if (E[k].transcodable) {
          if (dr != 0 || ol != E[k].utf8len || memcmp(ob, E[k].utf8, ol) != 0)
            fail(E[k].name, 7000 + k);
        } else if (dr != -1) fail(E[k].name, 8000 + k);
      }
      /* forward: valid UTF-8 -> UTF-16 recovers the pinned units */
      if (E[k].transcodable) {
        uint16_t ou[16]; size_t ou_n = 0;
        int fr_sc = sc.utf8_to_utf16le(E[k].utf8, E[k].utf8len, ou, &ou_n);
        if (fr_sc != 0 || ou_n != E[k].units || memcmp(ou, E[k].u, ou_n * 2) != 0)
          fail(E[k].name, 9000 + k);
        uint16_t ou2[16]; size_t ou2_n = 0;
        int fr_dut = simd.utf8_to_utf16le(E[k].utf8, E[k].utf8len, ou2, &ou2_n);
        if (fr_dut != 0 || ou2_n != E[k].units || memcmp(ou2, E[k].u, ou2_n * 2) != 0)
          fail(E[k].name, 10000 + k);
      }
      cases++;
    }
  }

  /* ==== F. empty input on every kernel ==== */
  {
    uint16_t iu = 0; uint8_t ib = 0; uint16_t ou[1]; uint8_t ob[1];
    size_t oul = 0, obl = 0;
    if (sc.utf8_to_utf16le(&ib, 0, ou, &oul) != 0 || oul != 0) fail("empty utf8->utf16", 0);
    if (simd.utf8_to_utf16le(&ib, 0, ou, &oul) != 0 || oul != 0) fail("empty utf8->utf16/simd", 0);
    if (sc.utf16le_to_utf8(&iu, 0, ob, &obl) != 0 || obl != 0) fail("empty utf16->utf8", 0);
    if (simd.utf16le_to_utf8(&iu, 0, ob, &obl) != 0 || obl != 0) fail("empty utf16->utf8/simd", 0);
    if (!sc.validate_utf16le(&iu, 0) || !simd.validate_utf16le(&iu, 0)) fail("empty validate", 0);
    if (sc.count_utf16(&iu, 0) != 0 || simd.count_utf16(&iu, 0) != 0) fail("empty count", 0);
    cases++;
  }

  if (fails == 0)
    printf("OK: %ld differential cases, all UTF-16 kernels match scalar reference, no guard-page fault\n", cases);
  else
    printf("FAILS=%d (of %ld cases)\n", fails, cases);
  return fails ? 1 : 0;
}
