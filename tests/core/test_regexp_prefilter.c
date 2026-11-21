#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cutils.h"
#include "libregexp.h"
#include "../lib/dyn-xalloc.h"
BOOL lre_check_stack_overflow(void* o, size_t n)
{
    (void)o;
    (void)n;
    return FALSE;
}
void* lre_realloc(void* o, void* p, size_t n)
{
    (void)o;
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}
static int polls = 0;
int lre_check_timeout(void* o)
{
    (void)o;
    polls++;
    return 0;
}

static int fails = 0, cases = 0;

static int brute(const uint32_t* hay, int hl, const uint32_t* nd, int nl)
{
    for (int i = 0; i + nl <= hl; i++) {
        int j = 0;
        while (j < nl && hay[i + j] == nd[j])
            j++;
        if (j == nl)
            return i;
    }
    return -1;
}
static void check(const uint32_t* hay, int hl, const uint32_t* nd, int nl, int wide)
{
    char pat[256];
    int p = 0;
    for (int i = 0; i < nl; i++)
        p += sprintf(pat + p, "\\u%04X", nd[i]);
    pat[p] = 0;
    int bclen;
    char err[128];
    uint8_t* bc = lre_compile(&bclen, err, sizeof(err), pat, strlen(pat), 0, NULL);
    if (!bc) {
        printf("compile fail %s: %s\n", pat, err);
        fails++;
        return;
    }
    uint8_t* sub;
    if (wide) {
        uint16_t* b = xmalloc((hl ? hl : 1) * 2);
        for (int i = 0; i < hl; i++)
            b[i] = (uint16_t)hay[i];
        sub = (uint8_t*)b;
    } else {
        uint8_t* b = xmalloc(hl ? hl : 1);
        for (int i = 0; i < hl; i++)
            b[i] = (uint8_t)hay[i];
        sub = b;
    }
    uint8_t** cap = xmalloc(sizeof(*cap) * lre_get_alloc_count(bc));
    int ret = lre_exec(cap, bc, sub, 0, hl, wide, NULL);
    int want = brute(hay, hl, nd, nl);
    int got = ret == 1 ? (int)((cap[0] - sub) >> wide) : -1;
    cases++;
    if ((want < 0) != (ret != 1) || (want >= 0 && got != want)) {
        printf("MISMATCH wide=%d hl=%d nl=%d want=%d got=%d ret=%d pat=%s\n",
            wide, hl, nl, want, got, ret, pat);
        fails++;
    }
    free(cap);
    free(sub);
    free(bc);
}
static void check_class(const char* cls, int (*member)(int), int wide)
{
    char pat[64];
    snprintf(pat, sizeof pat, "%sZQX", cls);
    int bclen;
    char err[128];
    uint8_t* bc = lre_compile(&bclen, err, sizeof err, pat, strlen(pat), 0, NULL);
    if (!bc) {
        printf("compile %s: %s\n", pat, err);
        fails++;
        return;
    }
    for (int hl = 0; hl <= 200; hl++) {
        uint32_t hay[256];
        for (int i = 0; i < hl; i++)
            hay[i] = (uint32_t)(" azAZ09_-\t~"[i % 12]);
        int want = -1;
        for (int i = 0; i + 4 <= hl; i++)
            if (member((int)hay[i]) && hay[i + 1] == 'Z' && hay[i + 2] == 'Q' && hay[i + 3] == 'X') {
                want = i;
                break;
            }
        if (want < 0 && hl >= 8) {
            hay[hl - 4] = 'a';
            hay[hl - 3] = 'Z';
            hay[hl - 2] = 'Q';
            hay[hl - 1] = 'X';
            want = member('a') ? hl - 4 : -1;
        }
        uint8_t* sub;
        if (wide) {
            uint16_t* b = xmalloc((hl ? hl : 1) * 2);
            for (int i = 0; i < hl; i++)
                b[i] = (uint16_t)hay[i];
            sub = (uint8_t*)b;
        } else {
            uint8_t* b = xmalloc(hl ? hl : 1);
            for (int i = 0; i < hl; i++)
                b[i] = (uint8_t)hay[i];
            sub = b;
        }
        uint8_t** cap = xmalloc(sizeof(*cap) * lre_get_alloc_count(bc));
        int ret = lre_exec(cap, bc, sub, 0, hl, wide, NULL);
        int got = ret == 1 ? (int)((cap[0] - sub) >> wide) : -1;
        cases++;
        if (got != want) {
            if (fails < 8)
                printf("CLASS MISMATCH %s wide=%d hl=%d want=%d got=%d\n", cls, wide, hl, want, got);
            fails++;
        }
        free(cap);
        free(sub);
    }
    free(bc);
}
static void check_alt(const char* pat, const char* alts[], int nalts)
{
    int bclen;
    char err[128];
    uint8_t* bc = lre_compile(&bclen, err, sizeof err, pat, strlen(pat), 0, NULL);
    if (!bc) {
        printf("compile %s: %s\n", pat, err);
        fails++;
        return;
    }
    static const char alpha[] = "abcdxyz019_ ";
    for (int hl = 0; hl <= 160; hl++) {
        char hay[200];
        for (int i = 0; i < hl; i++)
            hay[i] = alpha[(i * 7 + hl) % 12];
        if (hl > 20 && (hl & 1)) {
            const char* a = alts[hl % nalts];
            size_t al = strlen(a);
            if ((size_t)hl > al + 2)
                memcpy(hay + hl - al - 1, a, al);
        }
        int want = -1;
        for (int i = 0; i < hl && want < 0; i++)
            for (int k = 0; k < nalts; k++) {
                size_t al = strlen(alts[k]);
                if ((size_t)(hl - i) >= al && memcmp(hay + i, alts[k], al) == 0) {
                    want = i;
                    break;
                }
            }
        uint8_t** cap = xmalloc(sizeof(*cap) * lre_get_alloc_count(bc));
        int ret = lre_exec(cap, bc, (uint8_t*)hay, 0, hl, 0, NULL);
        int got = ret == 1 ? (int)((cap[0] - (uint8_t*)hay)) : -1;
        cases++;
        if (got != want) {
            if (fails < 8)
                printf("ALT MISMATCH %s hl=%d want=%d got=%d\n", pat, hl, want, got);
            fails++;
        }
        free(cap);
    }
    free(bc);
}
static int m_digit(int c) { return c >= '0' && c <= '9'; }
static int m_lower(int c) { return c >= 'a' && c <= 'z'; }
static int m_word(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int m_pair(int c) { return c == 'a' || c == 'Z'; }

// The prefilter cache (re_pf_build_cached) is keyed by bytecode AND subject width: a prefilter built for an
// 8-bit subject is never wrong on a 16-bit one (8-bit kinds fall back to "every position"), only useless, so
// the width bits are a performance key. Pinned by counting lre_check_timeout polls (one per 10000 backtracker
// steps): a pattern with no candidate in a 200k-unit subject must run 0 steps whichever width ran before.
static int polls_for(const uint8_t* bc, const void* sub, int n, int wide, int* ret)
{
    uint8_t* cap[64];
    polls = 0;
    *ret = lre_exec(cap, bc, (const uint8_t*)sub, 0, n, wide, NULL);
    return polls;
}
static void check_cache_width(void)
{
    const char* pats[] = { "kelvin|sun|yes", "[ksy]x|q", "(?:kb|qd)+z" };
    const int n = 200000;
    uint8_t* s8 = xmalloc(n);
    uint16_t* s16 = xmalloc(n * 2);
    for (int i = 0; i < n; i++) {
        s8[i] = (uint8_t)"plain text "[i % 11];
        s16[i] = i % 11 == 10 ? 0x2014 : (uint16_t)"plain text "[i % 11];
    }
    for (int k = 0; k < 3; k++) {
        int bclen, r1, r2, r3, r4;
        char err[128];
        uint8_t* bc = lre_compile(&bclen, err, sizeof err, pats[k], strlen(pats[k]), 0, NULL);
        if (!bc || lre_get_alloc_count(bc) > 64) {
            printf("compile fail %s\n", pats[k]);
            fails++;
            continue;
        }
        int p8 = polls_for(bc, s8, n, 0, &r1);
        int p16 = polls_for(bc, s16, n, 1, &r2);
        int q16 = polls_for(bc, s16, n, 1, &r3);
        int q8 = polls_for(bc, s8, n, 0, &r4);
        cases++;
        if (r1 || r2 || r3 || r4 || p8 > 1 || p16 > 1 || q16 > 1 || q8 > 1) {
            printf("FAIL cache width %s: polls 8-bit %d, 16-bit after 8-bit %d, again %d, 8-bit after 16-bit %d (ret %d %d %d %d)\n",
                pats[k], p8, p16, q16, q8, r1, r2, r3, r4);
            fails++;
        }
        free(bc);
    }
    free(s8);
    free(s16);
}

int main(void)
{
    uint32_t hay[80], nd[20];
    for (int wide = 0; wide < 2; wide++) {
        for (int hl = 0; hl <= 64; hl++) {
            for (int nl = 1; nl <= (hl < 17 ? hl + 2 : 17); nl++) {
                for (int variant = 0; variant < 3; variant++) {
                    for (int i = 0; i < hl; i++) {
                        uint32_t base = wide ? 0x100 : 0x41;
                        hay[i] = base + ((i * variant + i / 3) % 3);
                    }
                    for (int j = 0; j < nl; j++) {
                        uint32_t base = wide ? 0x100 : 0x41;
                        nd[j] = base + ((j + variant) % 3);
                    }
                    check(hay, hl, nd, nl, wide);
                    if (hl >= nl) {
                        for (int j = 0; j < nl; j++)
                            hay[hl - nl + j] = nd[j];
                        check(hay, hl, nd, nl, wide);
                    }
                    if (hl >= nl) {
                        for (int j = 0; j < nl; j++)
                            hay[j] = nd[j];
                        check(hay, hl, nd, nl, wide);
                    }
                }
            }
        }
    }
    check_class("[0-9]", m_digit, 0);
    check_class("[0-9]", m_digit, 1);
    check_class("[a-z]", m_lower, 0);
    check_class("[a-z]", m_lower, 1);
    check_class("[a-zA-Z0-9_]", m_word, 0);
    check_class("[a-zA-Z0-9_]", m_word, 1);
    check_class("[aZ]", m_pair, 0);
    {
        const char* a[] = { "ab", "cd" };
        check_alt("(ab|cd)", a, 2);
    }
    {
        const char* a[] = { "ab", "cd" };
        check_alt("(?:ab|cd)", a, 2);
    }
    {
        const char* a[] = { "a", "b", "c" };
        check_alt("(a|b|c)", a, 3);
    }
    {
        const char* a[] = { "xy", "z9", "01" };
        check_alt("(xy|z9|01)", a, 3);
    }
    {
        const char* a[] = { "ax", "bx", "cx" };
        check_alt("((a|b)x|cx)", a, 3);
    }
    {
        const char* a[] = { "0z", "9z", "_z" };
        check_alt("([0-9_]z)", a, 3);
    }
    {
        const char* a[] = { "ac", "c", "bc" };
        check_alt("(a?|b)c", a, 3);
    }
    {
        const char* a[] = { "xb" };
        check_alt("(x|xy)b", a, 1);
    }
    check_cache_width();
    printf("#PF cases=%d fails=%d\n", cases, fails);
    return fails ? 1 : 0;
}
