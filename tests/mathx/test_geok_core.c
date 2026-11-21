// covers: c:src/core/geok
/*
 * test_geok_core.c -- src/core/geok (the dyna:mathx geo kernels) as a
 * standalone C library under ASan/UBSan.
 *
 * Checks: the parallel one-to-many haversine and point-in-ring sweeps are
 * bit-identical to their scalar forms on one thread and many; Vincenty's
 * direct problem inverts the inverse problem (and back) for random WGS84
 * pairs, and only nearly antipodal pairs fail to converge; destination and
 * haversine/bearing are mutual inverses on the sphere; every geohash decodes
 * to a cell containing its point, and lengthening a hash only refines it;
 * the octant triangle has area pi/2 R^2 and a pole-to-pole lune of width
 * theta has 2 theta R^2, in either orientation (edges are great circles), and
 * a ring that winds around a pole encloses the polar region, not its complement.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-geok.h"
#include "dyn-par.h"

static int fails, checks;

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

static uint64_t rs = 88172645463325252ull;

static double rndu(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0;
}

static double ang(double a, double b)
{
    return fabs(fmod(a - b + 540.0, 360.0) - 180.0);
}

static void test_many(void)
{
    size_t n = 300000, i, d1 = 0, d2 = 0;
    double *la = malloc(n * sizeof(double)), *lo = malloc(n * sizeof(double));
    double *o1 = malloc(n * sizeof(double)), *o2 = malloc(n * sizeof(double));
    double rl[5] = { 10, 30, 20, 40, -5 }, ro[5] = { 0, 10, 30, 50, 20 };
    uint8_t *p1 = malloc(n), *p2 = malloc(n);
    uint32_t *w1 = malloc(n * sizeof(uint32_t)), *w2 = malloc(n * sizeof(uint32_t));
    double *e1 = malloc(n * sizeof(double)), *e2 = malloc(n * sizeof(double));
    size_t k1, k2, k, d3 = 0;
    dyn_par_stats_t st0, st1;
    for (i = 0; i < n; i++) {
        la[i] = rndu() * 180.0 - 90.0;
        lo[i] = rndu() * 360.0 - 180.0;
    }
    dyn_par_set_spawn_limit(0);
    dyn_geo_haversine_many(12.5, -40.0, la, lo, n, DYN_GEO_MEAN_RADIUS, o1);
    dyn_geo_points_in_ring(la, lo, n, rl, ro, 5, p1);
    k1 = dyn_geo_haversine_within(12.5, -40.0, la, lo, n, 5.0e6, DYN_GEO_MEAN_RADIUS, w1, e1);
    dyn_par_set_spawn_limit(-1);
    dyn_par_stats(&st0);
    dyn_geo_haversine_many(12.5, -40.0, la, lo, n, DYN_GEO_MEAN_RADIUS, o2);
    dyn_geo_points_in_ring(la, lo, n, rl, ro, 5, p2);
    k2 = dyn_geo_haversine_within(12.5, -40.0, la, lo, n, 5.0e6, DYN_GEO_MEAN_RADIUS, w2, e2);
    dyn_par_stats(&st1);
    CHECK(st1.runs - st0.runs == 3 && st1.serial_runs == st0.serial_runs, "all three sweeps really ran on several workers (%llu runs, %llu serial)",
        (unsigned long long)(st1.runs - st0.runs), (unsigned long long)(st1.serial_runs - st0.serial_runs));
    for (i = 0; i < n; i++) {
        d1 += o1[i] != o2[i] || o1[i] != dyn_geo_haversine(12.5, -40.0, la[i], lo[i], DYN_GEO_MEAN_RADIUS);
        d2 += p1[i] != p2[i] || p1[i] != (uint8_t)dyn_geo_point_in_ring(la[i], lo[i], rl, ro, 5);
    }
    CHECK(d1 == 0, "parallel haversine sweep equals the scalar haversine (%zu differ)", d1);
    CHECK(d2 == 0, "parallel point-in-ring sweep equals the scalar test (%zu differ)", d2);
    for (i = 0, k = 0; i < n; i++)
        if (o1[i] <= 5.0e6) {
            d3 += k >= k1 || w1[k] != i || e1[k] != o1[i] || k >= k2 || w2[k] != i || e2[k] != o1[i];
            k++;
        }
    CHECK(d3 == 0 && k == k1 && k == k2 && k > n / 20 && k < n / 2,
        "serial and parallel within-scans equal the filtered haversine sweep, compacted in order (%zu/%zu/%zu, %zu differ)", k, k1, k2, d3);
    free(la);
    free(lo);
    free(o1);
    free(o2);
    free(p1);
    free(p2);
    free(w1);
    free(w2);
    free(e1);
    free(e2);
}

static void test_vincenty(void)
{
    int k, noconv = 0, bad = 0, badback = 0;
    double worst = 0.0;
    for (k = 0; k < 4000; k++) {
        double la1 = rndu() * 170.0 - 85.0, lo1 = rndu() * 360.0 - 180.0, la2 = rndu() * 170.0 - 85.0,
               lo2 = rndu() * 360.0 - 180.0, d, a1, a2, la3, lo3, a3;
        if (k % 8 == 0) {
            la2 = -la1 + (rndu() - 0.5) * 0.2;
            lo2 = lo1 + 180.0 + (rndu() - 0.5) * 0.2;
            lo2 = fmod(lo2 + 540.0, 360.0) - 180.0;
        }
        if (dyn_geo_vincenty_inverse(la1, lo1, la2, lo2, &d, &a1, &a2) != DYN_GEO_OK) {
            double s = dyn_geo_haversine(la1, lo1, la2, lo2, 1.0);
            noconv++;
            if (s < M_PI * 0.99)
                bad++;
            continue;
        }
        if (dyn_geo_vincenty_direct(la1, lo1, a1, d, &la3, &lo3, &a3) != DYN_GEO_OK
            || fabs(la3 - la2) > 1e-8 || ang(lo3, lo2) > 1e-8 / fmax(1e-3, cos(la2 * M_PI / 180.0)) || ang(a3, a2) > 1e-6)
            badback++;
        worst = fmax(worst, fabs(la3 - la2));
    }
    CHECK(bad == 0, "only nearly antipodal pairs fail to converge (%d of %d failures were not)", bad, noconv);
    CHECK(noconv > 0 && noconv < 500, "the planted near-antipodal pairs do sometimes fail to converge (%d)", noconv);
    CHECK(badback == 0, "direct(inverse(p, q)) returns to q (%d misses, worst %.3g deg)", badback, worst);
}

static void test_sphere(void)
{
    int k, bad = 0;
    for (k = 0; k < 5000; k++) {
        double la = rndu() * 160.0 - 80.0, lo = rndu() * 360.0 - 180.0, b = rndu() * 360.0, d = rndu() * 1.9e7;
        dyn_geo_pt_t p = dyn_geo_destination(la, lo, b, d, DYN_GEO_MEAN_RADIUS);
        double back = dyn_geo_haversine(la, lo, p.lat, p.lon, DYN_GEO_MEAN_RADIUS);
        if (fabs(back - d) > 1e-6 || (d > 1.0 && d < 1.9e7 && ang(dyn_geo_bearing(la, lo, p.lat, p.lon), b) > 1e-6))
            bad++;
    }
    CHECK(bad == 0, "destination and haversine/bearing are mutual inverses (%d bad)", bad);
}

static void test_geohash(void)
{
    int k, bad = 0;
    char h[16], h2[16];
    for (k = 0; k < 20000; k++) {
        double la = rndu() * 180.0 - 90.0, lo = rndu() * 360.0 - 180.0, cla, clo, el, eo, c2a, c2o, e2a, e2o;
        unsigned p = 1 + (unsigned)(rndu() * 11.0);
        if (dyn_geo_hash_encode(la, lo, p, h) || dyn_geo_hash_decode(h, p, &cla, &clo, &el, &eo)) {
            bad++;
            continue;
        }
        if (fabs(la - cla) > el || fabs(lo - clo) > eo)
            bad++;
        dyn_geo_hash_encode(la, lo, p + 1, h2);
        if (strncmp(h, h2, p) != 0 || dyn_geo_hash_decode(h2, p + 1, &c2a, &c2o, &e2a, &e2o) || fabs(c2a - cla) > el
            || fabs(c2o - clo) > eo || !(e2a < el || e2o < eo))
            bad++;
    }
    CHECK(bad == 0, "geohash cells contain their points and refine with precision (%d bad)", bad);
    CHECK(dyn_geo_hash_decode("ezs4a", 5, &(double) { 0 }, &(double) { 0 }, &(double) { 0 }, &(double) { 0 }) == DYN_GEO_E_DOMAIN,
        "'a' is not a geohash digit");
    CHECK(dyn_geo_hash_encode(91.0, 0.0, 5, h) == DYN_GEO_E_DOMAIN && dyn_geo_hash_encode(0.0, 0.0, 13, h) == DYN_GEO_E_DOMAIN,
        "encode rejects lat 91 and precision 13");
}

static void test_area(void)
{
    double ol[3] = { 0, 0, 90 }, oo[3] = { 0, 90, 0 }, R = DYN_GEO_MEAN_RADIUS;
    double bl[4] = { 90, 0, -90, 0 }, bo[4] = { 0, 0, 0, 50 };
    double rl[4], ro[4], a, want;
    int i;
    a = dyn_geo_ring_area(ol, oo, 3, R);
    CHECK(fabs(a / (M_PI / 2.0 * R * R) - 1.0) < 1e-12, "octant area is pi/2 R^2 (%.17g)", a);
    want = 2.0 * (50.0 * M_PI / 180.0) * R * R;
    a = dyn_geo_ring_area(bl, bo, 4, R);
    for (i = 0; i < 4; i++) {
        rl[i] = bl[3 - i];
        ro[i] = bo[3 - i];
    }
    CHECK(fabs(a / want - 1.0) < 1e-12 && fabs(dyn_geo_ring_area(rl, ro, 4, R) / want - 1.0) < 1e-12,
        "the pole-to-pole lune between meridians 0 and 50 has area 2 theta R^2 in either orientation (%.17g vs %.17g)", a, want);
    {
        double nl[4] = { 89.999, 89.999, 89.999, 89.999 }, sl[4] = { -89.999, -89.999, -89.999, -89.999 }, lo4[4] = { 0, 90, 180, -90 };
        double cap = 2.0 * M_PI * (1.0 - sin(89.999 * M_PI / 180.0));
        CHECK(fabs(dyn_geo_ring_area(nl, lo4, 4, 1.0) / cap - 2.0 / M_PI) < 1e-6 && fabs(dyn_geo_ring_area(sl, lo4, 4, 1.0) / cap - 2.0 / M_PI) < 1e-6,
            "a tiny square around either pole is the inscribed square of its polar cap (ratio 2/pi), not the rest of the sphere");
    }
}

int main(void)
{
    test_many();
    test_vincenty();
    test_sphere();
    test_geohash();
    test_area();
    printf("test_geok_core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
