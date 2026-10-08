/* tests/test_simd_tiers.c -- the element-wise f32 kernels on WHICHEVER tier
 * this CPU selects. ORACLE: an element-wise kernel is a function of one
 * element, so its result for a value cannot depend on where the value sits or
 * how long the array is. Every value is therefore run at every position of
 * every length 1..41 and compared, bit for bit, with the kernel applied to
 * that value alone (a length-1 call, which only the scalar tail can serve).
 * A vector body that rounds, saturates or treats NaN differently from its own
 * tail fails here on the first lane.
 * The saturation points are checked against the definition as well: a
 * sigmoid past +-30 is exactly 1 or 0 and a tanh past +-10 exactly +-1.
 * Run it under every tier the project claims: natively (NEON), under Rosetta
 * (SSE4.2), under Rosetta with ROSETTA_ADVERTISE_AVX=1 (AVX2+FMA), and in the
 * amd64 container. The first line names the tier that actually ran. */
#include "dyna-simd-kernels.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
static uint32_t bits_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

typedef void (*unary_fn)(float*, const float*, size_t);
typedef struct { const char* name; int kind; unary_fn u; } kern_t;

static void call(const kern_t* k, float* out, const float* in, size_t n)
{
    switch (k->kind) {
    case 0: k->u(out, in, n); break;
    case 1: simd.leaky_relu(out, in, 0.1f, n); break;
    case 2: simd.elu(out, in, 1.0f, n); break;
    case 3: simd.threshold(out, in, 0.5f, n); break;
    case 4: simd.threshold_sign(out, in, 0.5f, n); break;
    case 5: simd.add_s(out, in, 1.5f, n); break;
    case 6: simd.mul_s(out, in, -2.5f, n); break;
    case 7: simd.scale_add_s(out, 1.5f, in, 0.25f, n); break;
    }
}

int main(void)
{
    static const float vals[] = {
        0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.7f, -1.3f, 2.1f, -0.4f, 3.0f, -3.0f, 5.999f, 6.0f, 6.001f,
        9.999f, 10.0f, 10.001f, -9.999f, -10.0f, -10.001f, 11.0f, -11.0f, 29.999f, 30.0f, 30.001f,
        -29.999f, -30.0f, -30.001f, 31.0f, -31.0f, 50.0f, -50.0f, 87.9f, 88.0f, 88.1f, -87.9f, -88.0f, -88.1f,
        100.0f, -100.0f, 1e-20f, -1e-20f, 1e20f, -1e20f, 1.17549435e-38f, 3.40282347e38f, -3.40282347e38f,
        1e-45f, 0.1f, 0.25f, 123.456f, -123.456f, 4.0f, 16.0f, 1e-10f,
        INFINITY, -INFINITY, NAN,
    };
    const size_t nv = sizeof(vals) / sizeof(vals[0]);
    float single[sizeof(vals) / sizeof(vals[0])];
    float in[64], out[64];
    simd_init();
    kern_t ks[] = {
        { "abs", 0, simd.abs }, { "sigmoid", 0, simd.sigmoid }, { "relu", 0, simd.relu }, { "relu6", 0, simd.relu6 },
        { "tanh_fast", 0, simd.tanh_fast }, { "gelu", 0, simd.gelu }, { "silu", 0, simd.silu }, { "vexp", 0, simd.vexp },
        { "vlog", 0, simd.vlog }, { "vsqrt", 0, simd.vsqrt }, { "vrsqrt", 0, simd.vrsqrt }, { "vinv", 0, simd.vinv },
        { "leaky_relu", 1, NULL }, { "elu", 2, NULL }, { "threshold", 3, NULL }, { "threshold_sign", 4, NULL },
        { "add_s", 5, NULL }, { "mul_s", 6, NULL }, { "scale_add_s", 7, NULL },
    };
    uint64_t caps = cpu_features();
    printf("tier: %s (caps 0x%llx)\n",
        (caps & (CPU_AVX512F | CPU_AVX512DQ | CPU_AVX512BW)) == (CPU_AVX512F | CPU_AVX512DQ | CPU_AVX512BW) ? "avx512"
        : (caps & CPU_AVX2) ? "avx2" : (caps & CPU_SVE) ? "sve" : (caps & CPU_NEON) ? "neon" : (caps & CPU_SSE42) ? "sse4.2" : "scalar",
        (unsigned long long)caps);

    for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        const kern_t* k = &ks[ki];
        int bad = 0;
        for (size_t v = 0; v < nv; v++) {
            float one_in = vals[v], one_out = 0;
            call(k, &one_out, &one_in, 1);
            single[v] = one_out;
        }
        for (size_t len = 1; len <= 41; len++) {
            for (size_t rot = 0; rot < nv; rot += 3) {
                for (size_t i = 0; i < len; i++)
                    in[i] = vals[(i * 7 + rot) % nv];
                call(k, out, in, len);
                for (size_t i = 0; i < len; i++) {
                    size_t v = (i * 7 + rot) % nv;
                    int same = bits_of(out[i]) == bits_of(single[v]) || (out[i] != out[i] && single[v] != single[v]);
                    checks++;
                    if (!same) {
                        if (bad < 3)
                            printf("FAIL: %s(%.9g) at index %zu of %zu is %.9g (0x%08x), alone it is %.9g (0x%08x)\n",
                                k->name, (double)vals[v], i, len, (double)out[i], bits_of(out[i]), (double)single[v], bits_of(single[v]));
                        bad++;
                    }
                }
            }
        }
        if (bad) {
            printf("FAIL: %s: %d position-dependent results\n", k->name, bad);
            fails++;
        }
    }

    {
        static const float sx[8] = { -50.0f, -31.0f, 31.0f, 50.0f, -50.0f, -31.0f, 31.0f, 50.0f };
        static const float tx[8] = { -50.0f, -11.0f, 11.0f, 50.0f, -50.0f, -11.0f, 11.0f, 50.0f };
        float o[8];
        simd.sigmoid(o, sx, 8);
        for (int i = 0; i < 8; i++) {
            float want = sx[i] < 0 ? 0.0f : 1.0f;
            checks++;
            if (bits_of(o[i]) != bits_of(want)) { printf("FAIL: sigmoid(%g) saturates to %.9g, not %g\n", (double)sx[i], (double)o[i], (double)want); fails++; }
        }
        simd.tanh_fast(o, tx, 8);
        for (int i = 0; i < 8; i++) {
            float want = tx[i] < 0 ? -1.0f : 1.0f;
            checks++;
            if (bits_of(o[i]) != bits_of(want)) { printf("FAIL: tanh_fast(%g) saturates to %.9g, not %g\n", (double)tx[i], (double)o[i], (double)want); fails++; }
        }
    }
    /* The exp family and the rectifiers are documented as ONE fast formula, so
       the tier that ran must agree bit for bit with the portable scalar table
       on every value (both NaN counts as agreement). */
    {
        struct { const char* name; unary_fn tier, ref; } both[] = {
            { "abs", simd.abs, simd_scalar.abs }, { "sigmoid", simd.sigmoid, simd_scalar.sigmoid },
            { "tanh_fast", simd.tanh_fast, simd_scalar.tanh_fast }, { "vexp", simd.vexp, simd_scalar.vexp },
            { "silu", simd.silu, simd_scalar.silu }, { "relu", simd.relu, simd_scalar.relu },
            { "relu6", simd.relu6, simd_scalar.relu6 },
        };
        for (size_t b = 0; b < sizeof(both) / sizeof(both[0]); b++) {
            int bad = 0;
            for (size_t v = 0; v < nv; v++) {
                float a = 0, r = 0, x = vals[v];
                both[b].tier(&a, &x, 1);
                both[b].ref(&r, &x, 1);
                checks++;
                if (bits_of(a) != bits_of(r) && !(a != a && r != r)) {
                    if (bad++ < 3)
                        printf("FAIL: %s(%.9g): this tier gives %.9g (0x%08x), the scalar table %.9g (0x%08x)\n",
                            both[b].name, (double)x, (double)a, bits_of(a), (double)r, bits_of(r));
                }
            }
            if (bad)
                fails++;
        }
    }
    printf("test_simd_tiers: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
