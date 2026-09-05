/*
 * test_ac_caps.c -- pins the DYN_AC_MAX_STATES ceiling (audit E10-11).
 *
 * E10-11: the goto table is int32[256] per state = 1 KiB/state, so a large
 * pattern set grows the automaton at ~1 MiB per 1000 states with no bound.
 * The audited minimum remedy is cap + document: dyn_ac_new_state refuses
 * past DYN_AC_MAX_STATES with the existing -1/OOM contract, so an over-cap
 * set fails LOUDLY at insert instead of silently budgeting hundreds of MiB.
 *
 * This test pins three things:
 *   1. the per-state cost assumption itself (_Static_assert: 1 KiB goto row);
 *   2. the boundary: just-under / exactly-at / one-over the cap -- at the cap
 *      build+search still work, over the cap insert returns -1 and n_states
 *      freezes at the cap (no silent growth), and a refused automaton still
 *      builds and answers for the patterns it DID accept (no corruption);
 *   3. that the small-set semantics are unchanged ({he,she,his,hers}).
 *
 * State counting: patterns are [0xA5, i>>16, i>>8, i] (4 bytes, i = 0..n-1).
 * Distinct prefixes = root + the A5 node + ceil(n/65536) two-byte prefixes +
 * ceil(n/256) three-byte prefixes + n leaves, so
 *     states(n) = n + ceil(n/256) + ceil(n/65536) + 2
 * is exact and each insert is O(1) -- no O(n^2) deep-chain inserts. A chain
 * extension (an accepted pattern + one fresh byte) adds exactly one state,
 * which is how the boundary is probed one state at a time.
 *
 * Build/run standalone (the Makefile target wires the same shape):
 *   clang -std=gnu17 -Isrc/core -o /tmp/test_ac_caps \
 *       tests/test_ac_caps.c src/core/dyn-ac.c && /tmp/test_ac_caps
 */
#include "dyn-ac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The cap arithmetic in dyn-ac.h and every comment around it is per-state
 * 1 KiB; if the row size ever changes, the cap must be redone. */
_Static_assert(sizeof(int32_t) * DYN_AC_ALPHABET == 1024,
               "goto row must be 1 KiB for DYN_AC_MAX_STATES' math to hold");
_Static_assert(DYN_AC_MAX_STATES > 0 &&
               (DYN_AC_MAX_STATES & (DYN_AC_MAX_STATES - 1)) == 0,
               "cap is a power of two: doubling growth lands on it exactly");

/* ---- harness (same shape as test_ds_btree_oom.c) ------------------------- */
static int failures = 0;
static long checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            if (++failures > 20) { printf("too many\n"); exit(1); }           \
        }                                                                     \
    } while (0)

/* ---- hit recorder --------------------------------------------------------- */
#define MAX_HITS 64
typedef struct {
    int pat[MAX_HITS];
    size_t end[MAX_HITS];
    int n;
} hits_t;

static int record(void *ud, int pat, size_t end_byte)
{
    hits_t *h = (hits_t *)ud;
    if (h->n < MAX_HITS) {
        h->pat[h->n] = pat;
        h->end[h->n] = end_byte;
    }
    h->n++;
    return 0;
}

static int hit_present(const hits_t *h, int pat, size_t end_byte)
{
    int i;
    for (i = 0; i < h->n && i < MAX_HITS; i++)
        if (h->pat[i] == pat && h->end[i] == end_byte)
            return 1;
    return 0;
}

/* ---- the counting construction ------------------------------------------- */
static void pat4(uint8_t *out, uint32_t i)   /* [0xA5, i>>16, i>>8, i] */
{
    out[0] = 0xA5;
    out[1] = (uint8_t)(i >> 16);
    out[2] = (uint8_t)(i >> 8);
    out[3] = (uint8_t)i;
}

static size_t states4(size_t n)              /* exact trie size, see header */
{
    return n + (n + 255) / 256 + (n + 65535) / 65536 + 2;
}

/* Insert i = 0..n-1; every insert must return 0 and the state count must
 * track the exact formula (a formula that drifted from the real trie would
 * silently move where the boundary lands). */
static void fill4(dyn_ac_t *a, size_t n, int *bad)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t p[4];
        pat4(p, (uint32_t)i);
        if (dyn_ac_insert(a, p, 4, (int)i) < 0) {
            printf("FAIL insert of pattern %zu was refused under the cap\n", i);
            (*bad)++;
            return;
        }
    }
    *bad += a->n_states != states4(n);
    if (a->n_states != states4(n))
        printf("FAIL state formula drifted: got %zu want %zu\n",
               a->n_states, states4(n));
}

int main(void)
{
    /* ---- 0. small-set semantics unchanged by the cap check ---------------- */
    {
        static const char *pats[] = { "he", "she", "his", "hers" };
        dyn_ac_t *a = dyn_ac_new(4);
        hits_t h = { {0}, {0}, 0 };
        int i;
        for (i = 0; i < 4; i++)
            CHECK(dyn_ac_insert(a, (const uint8_t *)pats[i],
                                strlen(pats[i]), i) == 0, "insert %d", i);
        CHECK(dyn_ac_build(a) == 0, "build");
        dyn_ac_run(a, (const uint8_t *)"ushers", 6, record, &h);
        CHECK(h.n == 3, "ushers emits 3 hits, got %d", h.n);
        CHECK(hit_present(&h, 1, 4) && hit_present(&h, 0, 4) &&
              hit_present(&h, 3, 6),
              "she@4 he@4 hers@6 (out_link chain intact)");
        dyn_ac_free(a);
    }

    /* ---- 1. under -> exactly at -> one over, one state at a time ---------- */
    /* n_under is the largest n with states4(n) < DYN_AC_MAX_STATES, derived
     * from the formula rather than hardcoded so a cap change keeps the test
     * meaningful. */
    {
        size_t n_under = 0, n;
        uint8_t base[4], ext1[5], ext2[5], fresh[4];
        dyn_ac_t *a;
        hits_t h = { {0}, {0}, 0 };
        uint8_t text[14];
        int bad = 0;

        for (n = 1; states4(n) + 1 <= (size_t)DYN_AC_MAX_STATES; n++)
            n_under = n;
        CHECK(states4(n_under) <= (size_t)DYN_AC_MAX_STATES - 1 &&
              states4(n_under + 1) > (size_t)DYN_AC_MAX_STATES - 1,
              "n_under %zu straddles the cap (states %zu)",
              n_under, states4(n_under));

        a = dyn_ac_new(n_under + 2);        /* +2 chain-extension slots */
        CHECK(a != NULL, "automaton alloc");
        fill4(a, n_under, &bad);
        CHECK(bad == 0, "all under-cap inserts accepted");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES - 1,
              "one state below the cap, got %zu", a->n_states);

        /* A chain extension of an accepted pattern adds exactly one state:
         * the last legal state must still be accepted. */
        pat4(base, 777);
        memcpy(ext1, base, 4);  ext1[4] = 0x51;   /* 'Q' */
        memcpy(ext2, base, 4);  ext2[4] = 0x52;   /* 'R' */
        CHECK(dyn_ac_insert(a, ext1, 5, (int)n_under) == 0,
              "the final state slot under the cap is granted");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "exactly at the cap, got %zu", a->n_states);

        /* One over: same one-state shape, refused loudly. */
        CHECK(dyn_ac_insert(a, ext2, 5, (int)n_under + 1) == -1,
              "one state past the cap refuses with -1");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "refusal froze growth at the cap, got %zu", a->n_states);
        pat4(fresh, 0xFFFFFFu);              /* needs 4 states: also refused */
        CHECK(dyn_ac_insert(a, fresh, 4, (int)n_under + 1) == -1,
              "a multi-state insert past the cap also refuses");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "still frozen, got %zu", a->n_states);

        /* The refused automaton is not corrupted: it builds, and the accepted
         * set answers. ext2 was never given an output, so it cannot emit. */
        CHECK(dyn_ac_build(a) == 0, "build after refusal");
        memcpy(text, "\xA5\x00\x00\x00", 4);         /* pattern 0          */
        memcpy(text + 4, ext1, 5);                   /* 777 then ext1      */
        memcpy(text + 9, ext2, 5);                   /* 777 again, no ext2 */
        dyn_ac_run(a, text, sizeof text, record, &h);
        CHECK(h.n == 4, "4 hits (pat0@4, 777@8, ext1@9, 777@13), got %d", h.n);
        CHECK(hit_present(&h, 0, 4) && hit_present(&h, 777, 8) &&
              hit_present(&h, (int)n_under, 9) && hit_present(&h, 777, 13),
              "the accepted patterns answer at their exact end bytes");
        CHECK(!hit_present(&h, (int)n_under + 1, 14),
              "the refused pattern does not emit");
        dyn_ac_free(a);
    }

    /* ---- 2. a full-cap set built in bulk: last insert accepted, one more
     * refused, build+search correct at the ceiling -------------------------- */
    {
        size_t n_at = 0, n;
        dyn_ac_t *a;
        hits_t h = { {0}, {0}, 0 };
        uint8_t text[8], p[4];
        int bad = 0;

        for (n = 1; states4(n) <= (size_t)DYN_AC_MAX_STATES; n++)
            n_at = n;
        CHECK(states4(n_at) == (size_t)DYN_AC_MAX_STATES,
              "n_at %zu lands exactly on the cap (states %zu)",
              n_at, states4(n_at));

        a = dyn_ac_new(n_at + 1);
        CHECK(a != NULL, "automaton alloc");
        fill4(a, n_at, &bad);
        CHECK(bad == 0, "every insert of an exactly-at-cap set is accepted");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "at the cap, got %zu", a->n_states);

        pat4(p, 0xFFFFFFu);
        CHECK(dyn_ac_insert(a, p, 4, (int)n_at) == -1,
              "one pattern over the cap refuses with -1");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "growth frozen at the cap, got %zu", a->n_states);
        CHECK(dyn_ac_build(a) == 0, "build at the cap");

        pat4(text, 5);
        pat4(text + 4, (uint32_t)(n_at - 1));
        dyn_ac_run(a, text, sizeof text, record, &h);
        CHECK(h.n == 2, "2 hits at the cap, got %d", h.n);
        CHECK(hit_present(&h, 5, 4) && hit_present(&h, (int)n_at - 1, 8),
              "first and last accepted patterns both answer");
        dyn_ac_free(a);
    }

    printf("%s: %ld checks, %d failures\n",
           failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
