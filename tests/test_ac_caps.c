#include "dyn-ac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int32_t) * DYN_AC_ALPHABET == 1024,
               "goto row must be 1 KiB for DYN_AC_MAX_STATES' math to hold");
_Static_assert(DYN_AC_MAX_STATES > 0 &&
               (DYN_AC_MAX_STATES & (DYN_AC_MAX_STATES - 1)) == 0,
               "cap is a power of two: doubling growth lands on it exactly");

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

static void pat4(uint8_t *out, uint32_t i)
{
    out[0] = 0xA5;
    out[1] = (uint8_t)(i >> 16);
    out[2] = (uint8_t)(i >> 8);
    out[3] = (uint8_t)i;
}

static size_t states4(size_t n)
{
    return n + (n + 255) / 256 + (n + 65535) / 65536 + 2;
}

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

        a = dyn_ac_new(n_under + 2);
        CHECK(a != NULL, "automaton alloc");
        fill4(a, n_under, &bad);
        CHECK(bad == 0, "all under-cap inserts accepted");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES - 1,
              "one state below the cap, got %zu", a->n_states);

        pat4(base, 777);
        memcpy(ext1, base, 4);  ext1[4] = 0x51;
        memcpy(ext2, base, 4);  ext2[4] = 0x52;
        CHECK(dyn_ac_insert(a, ext1, 5, (int)n_under) == 0,
              "the final state slot under the cap is granted");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "exactly at the cap, got %zu", a->n_states);

        CHECK(dyn_ac_insert(a, ext2, 5, (int)n_under + 1) == -1,
              "one state past the cap refuses with -1");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "refusal froze growth at the cap, got %zu", a->n_states);
        pat4(fresh, 0xFFFFFFu);
        CHECK(dyn_ac_insert(a, fresh, 4, (int)n_under + 1) == -1,
              "a multi-state insert past the cap also refuses");
        CHECK(a->n_states == (size_t)DYN_AC_MAX_STATES,
              "still frozen, got %zu", a->n_states);

        CHECK(dyn_ac_build(a) == 0, "build after refusal");
        memcpy(text, "\xA5\x00\x00\x00", 4);
        memcpy(text + 4, ext1, 5);
        memcpy(text + 9, ext2, 5);
        dyn_ac_run(a, text, sizeof text, record, &h);
        CHECK(h.n == 4, "4 hits (pat0@4, 777@8, ext1@9, 777@13), got %d", h.n);
        CHECK(hit_present(&h, 0, 4) && hit_present(&h, 777, 8) &&
              hit_present(&h, (int)n_under, 9) && hit_present(&h, 777, 13),
              "the accepted patterns answer at their exact end bytes");
        CHECK(!hit_present(&h, (int)n_under + 1, 14),
              "the refused pattern does not emit");
        dyn_ac_free(a);
    }

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
