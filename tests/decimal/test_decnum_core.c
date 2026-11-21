/*
 * test_decnum_core.c -- src/core/decnum (the dyna:decimal arithmetic) as a
 * standalone C library: no engine, a counting host allocator.
 *
 * Expected values come from outside this code:
 *  - sqrt(2), e, ln(10), ln(2) to 34 significant digits: OEIS A002193,
 *    A001113, A002392, A002162 (34 digits, correctly rounded half-even).
 *  - 1/3 to 34 digits: thirty-four 3s (exact rational expansion).
 *  - Exact add/sub/mul are exact rational results (0.1 + 0.2 = 0.3,
 *    1.20 * 3 = 3.6, 1.3 - 1.07 = 0.23); the text is canonical (no trailing
 *    zeros), the module's own convention, pinned here as a property.
 *  - ISO 4217 minor units: JPY 0, BHD 3, USD 2.
 * Every allocation the library makes goes through dyn_dec_set_allocator; the
 * test installs a counting allocator and requires the count to return to zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-decnum.h"

static int fails, checks;
static long live_blocks, total_allocs;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static void* t_malloc(size_t n)
{
    void* p = malloc(n);
    if (p) {
        live_blocks++;
        total_allocs++;
    }
    return p;
}

static void* t_calloc(size_t n, size_t s)
{
    void* p = calloc(n, s);
    if (p) {
        live_blocks++;
        total_allocs++;
    }
    return p;
}

static void* t_realloc(void* p, size_t n)
{
    int fresh = p == NULL;
    void* q = realloc(p, n);
    if (q && fresh) {
        live_blocks++;
        total_allocs++;
    }
    return q;
}

static void t_free(void* p)
{
    if (p)
        live_blocks--;
    free(p);
}

static const dyn_dec_allocator_t t_alloc = { t_malloc, t_calloc, t_realloc, t_free };

static void text(const dec_t* x, char* out, size_t cap)
{
    db_t b;
    db_init(&b);
    dec_write(&b, x);
    snprintf(out, cap, "%.*s", (int)b.n, b.p ? (const char*)b.p : "");
    db_free(&b);
}

static void parse(dec_t* x, const char* s)
{
    dec_init(x);
    CHECK(dec_parse(x, s, strlen(s)) == 0, "parse %s", s);
}

static void expect(const char* what, const dec_t* x, const char* want)
{
    char buf[256];
    text(x, buf, sizeof buf);
    CHECK(!strcmp(buf, want), "%s = %s, want %s", what, buf, want);
}

static void test_exact(void)
{
    dec_t a, b, r;
    parse(&a, "0.1");
    parse(&b, "0.2");
    dec_init(&r);
    CHECK(dec_add(&r, &a, &b) == 0, "add");
    expect("0.1 + 0.2", &r, "0.3");
    dec_free(&a);
    dec_free(&b);
    parse(&a, "1.20");
    parse(&b, "3");
    CHECK(dec_mul(&r, &a, &b) == 0, "mul");
    expect("1.20 * 3", &r, "3.6");
    parse(&a, "1.3");
    parse(&b, "1.07");
    CHECK(dec_sub(&r, &a, &b) == 0, "sub");
    expect("1.3 - 1.07", &r, "0.23");
    CHECK(dec_cmp(&a, &b) > 0 && dec_cmp(&b, &a) < 0 && dec_cmp(&a, &a) == 0, "cmp orders 1.3 > 1.07");
    dec_free(&a);
    dec_free(&b);
    dec_free(&r);
}

static void test_inexact(void)
{
    dec_t a, b, r;
    parse(&a, "1");
    parse(&b, "3");
    dec_init(&r);
    CHECK(dec_div(&r, &a, &b, 34, RND_HALF_EVEN) == 0, "div");
    expect("1/3 @34", &r, "0.3333333333333333333333333333333333");
    dec_free(&a);
    dec_free(&b);
    parse(&a, "2");
    CHECK(dec_sqrt(&r, &a, 34) == 0, "sqrt");
    expect("sqrt(2) @34 (OEIS A002193)", &r, "1.414213562373095048801688724209698");
    dec_free(&a);
    parse(&a, "1");
    CHECK(dec_exp(&r, &a, 34) == 0, "exp");
    expect("exp(1) @34 (OEIS A001113)", &r, "2.718281828459045235360287471352662");
    dec_free(&a);
    parse(&a, "10");
    CHECK(dec_ln(&r, &a, 34) == 0, "ln");
    expect("ln(10) @34 (OEIS A002392)", &r, "2.302585092994045684017991454684364");
    dec_free(&a);
    parse(&a, "2");
    CHECK(dec_ln(&r, &a, 34) == 0, "ln2");
    expect("ln(2) @34 (OEIS A002162)", &r, "0.6931471805599453094172321214581766");
    dec_free(&a);
    dec_free(&r);
}

static void test_big(void)
{
    char s[1201];
    dec_t a, r;
    memset(s, '9', 1200);
    s[1200] = 0;
    parse(&a, s);
    dec_init(&r);
    CHECK(dec_mul(&r, &a, &a) == 0, "big mul");
    CHECK(r.nd == 2400 && r.d[0] == 1 && r.d[1200] == 8 && r.d[2399] == 9,
        "(10^1200-1)^2 = 9..980..01 (nd %u)", r.nd);
    dec_free(&a);
    dec_free(&r);
}

static void test_money(void)
{
    CHECK(money_minor("JPY") == 0, "ISO 4217 JPY has 0 minor units");
    CHECK(money_minor("BHD") == 3, "ISO 4217 BHD has 3 minor units");
    CHECK(money_minor("USD") == 2, "ISO 4217 USD has 2 minor units");
}

int main(void)
{
    dyn_dec_set_allocator(&t_alloc);
    test_exact();
    test_inexact();
    test_big();
    test_money();
    CHECK(total_allocs > 0, "the library allocated through the host hooks (%ld)", total_allocs);
    CHECK(live_blocks == 0, "every host allocation was released (%ld live)", live_blocks);
    printf("test_decnum_core: %d checks, %ld host allocations, %d failed\n", checks, total_allocs, fails);
    return fails ? 1 : 0;
}
