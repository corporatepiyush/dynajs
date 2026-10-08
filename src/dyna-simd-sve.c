#include <math.h>
#include "dyna-simd-kernels.h"

#if defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>

static inline int sve_f32_cnt(void) { return svcntw(); }

static float simd_sve_dot(const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t acc = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t va = svld1_f32(pg, &a[i]);
        svfloat32_t vb = svld1_f32(pg, &b[i]);
        acc = svmla_f32_z(pg, acc, va, vb);
    }
    float result = svaddv_f32(pg, acc);
    for (; i < n; i++)
        result += a[i] * b[i];
    return result;
}

static float simd_sve_dot_f(const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg64 = svptrue_b64();
    svbool_t pgh = svwhilelt_b32((size_t)0, (size_t)svcntd());
    svfloat64_t acc = svdup_f64(0.0);
    int dcnt = svcntd();
    size_t i = 0;
    for (; i + 2 * (size_t)dcnt <= n; i += 2 * (size_t)dcnt) {
        svfloat64_t a0 = svcvt_f64_f32_x(pg64, svld1_f32(pgh, &a[i]));
        svfloat64_t b0 = svcvt_f64_f32_x(pg64, svld1_f32(pgh, &b[i]));
        svfloat64_t a1 = svcvt_f64_f32_x(pg64, svld1_f32(pgh, &a[i + dcnt]));
        svfloat64_t b1 = svcvt_f64_f32_x(pg64, svld1_f32(pgh, &b[i + dcnt]));
        acc = svmla_f64_z(pg64, svmla_f64_z(pg64, acc, a0, b0), a1, b1);
    }
    double result = svaddv_f64(pg64, acc);
    for (; i < n; i++)
        result += (double)a[i] * b[i];
    return (float)result;
}

static float simd_sve_norm_l2_sq(const float* restrict x, size_t n)
{
    return simd_sve_dot(x, x, n);
}

static float simd_sve_norm_l2(const float* restrict x, size_t n)
{
    return sqrtf(simd_sve_norm_l2_sq(x, n));
}

static float simd_sve_norm_l1(const float* restrict x, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t acc = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vx = svld1_f32(pg, &x[i]);
        acc = svadd_f32_z(pg, acc, svabs_f32_x(pg, vx));
    }
    float result = svaddv_f32(pg, acc);
    for (; i < n; i++)
        result += fabsf(x[i]);
    return result;
}

static float simd_sve_sum(const float* restrict x, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t acc = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        acc = svadd_f32_z(pg, acc, svld1_f32(pg, &x[i]));
    float result = svaddv_f32(pg, acc);
    for (; i < n; i++)
        result += x[i];
    return result;
}

static float simd_sve_max(const float* restrict x, size_t n)
{
    if (n == 0)
        return -FLT_MAX;
    if (n < (size_t)sve_f32_cnt()) {
        float m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] > m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b32();
    svfloat32_t vmax = svld1_f32(pg, x);
    size_t i = sve_f32_cnt();
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        vmax = svmax_f32_z(pg, vmax, svld1_f32(pg, &x[i]));
    float result = svmaxv_f32(pg, vmax);
    for (; i < n; i++)
        if (x[i] > result)
            result = x[i];
    return result;
}

static float simd_sve_min(const float* restrict x, size_t n)
{
    if (n == 0)
        return FLT_MAX;
    if (n < (size_t)sve_f32_cnt()) {
        float m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] < m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b32();
    svfloat32_t vmin = svld1_f32(pg, x);
    size_t i = sve_f32_cnt();
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        vmin = svmin_f32_z(pg, vmin, svld1_f32(pg, &x[i]));
    float result = svminv_f32(pg, vmin);
    for (; i < n; i++)
        if (x[i] < result)
            result = x[i];
    return result;
}

static size_t simd_sve_argmax(const float* restrict x, size_t n)
{
    if (n == 0)
        return 0;
    if (n < (size_t)sve_f32_cnt()) {
        size_t k = 0;
        for (size_t i = 1; i < n; i++)
            if (x[i] > x[k])
                k = i;
        return k;
    }
    svbool_t pg = svptrue_b32();
    svfloat32_t vmax = svld1_f32(pg, x);
    svuint32_t vidx_max = svindex_u32(0, 1);
    size_t offset = sve_f32_cnt();
    int cnt = sve_f32_cnt();
    for (size_t i = offset; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &x[i]);
        svuint32_t vidxi = svadd_n_u32_z(pg, svindex_u32(0, 1), (uint32_t)i);
        svbool_t mask = svcmpgt_f32(pg, vi, vmax);
        vmax = svsel_f32(mask, vi, vmax);
        vidx_max = svsel_u32(mask, vidxi, vidx_max);
    }
    float tmp_max = svmaxv_f32(pg, vmax);
    uint32_t idx_arr[256];
    svst1_u32(pg, idx_arr, vidx_max);
    size_t best_idx = 0;
    for (int j = 0; j < cnt; j++) {
        if (idx_arr[j] >= (uint32_t)n)
            continue;
        if (x[idx_arr[j]] == tmp_max) {
            best_idx = idx_arr[j];
            break;
        }
    }
    for (size_t i = n - (n % (size_t)cnt); i < n; i++) {
        if (x[i] > tmp_max) {
            tmp_max = x[i];
            best_idx = i;
        }
    }
    return best_idx;
}

static size_t simd_sve_argmin(const float* restrict x, size_t n)
{
    if (n == 0)
        return 0;
    if (n < (size_t)sve_f32_cnt()) {
        size_t k = 0;
        for (size_t i = 1; i < n; i++)
            if (x[i] < x[k])
                k = i;
        return k;
    }
    svbool_t pg = svptrue_b32();
    svfloat32_t vmin = svld1_f32(pg, x);
    svuint32_t vidx_min = svindex_u32(0, 1);
    size_t offset = sve_f32_cnt();
    int cnt = sve_f32_cnt();
    for (size_t i = offset; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &x[i]);
        svuint32_t vidxi = svadd_n_u32_z(pg, svindex_u32(0, 1), (uint32_t)i);
        svbool_t mask = svcmplt_f32(pg, vi, vmin);
        vmin = svsel_f32(mask, vi, vmin);
        vidx_min = svsel_u32(mask, vidxi, vidx_min);
    }
    float tmp_min = svminv_f32(pg, vmin);
    uint32_t idx_arr[256];
    svst1_u32(pg, idx_arr, vidx_min);
    size_t best_idx = 0;
    for (int j = 0; j < cnt; j++) {
        if (idx_arr[j] >= (uint32_t)n)
            continue;
        if (x[idx_arr[j]] == tmp_min) {
            best_idx = idx_arr[j];
            break;
        }
    }
    for (size_t i = n - (n % (size_t)cnt); i < n; i++) {
        if (x[i] < tmp_min) {
            tmp_min = x[i];
            best_idx = i;
        }
    }
    return best_idx;
}

static void simd_sve_argminmax(const float* restrict x, size_t n,
    size_t* argmin_out, size_t* argmax_out)
{
    if (n == 0) {
        *argmin_out = *argmax_out = 0;
        return;
    }
    if (n < (size_t)sve_f32_cnt()) {
        size_t mn = 0, mx = 0;
        for (size_t i = 1; i < n; i++) {
            if (x[i] < x[mn])
                mn = i;
            if (x[i] > x[mx])
                mx = i;
        }
        *argmin_out = mn;
        *argmax_out = mx;
        return;
    }
    svbool_t pg = svptrue_b32();
    svfloat32_t vmin = svld1_f32(pg, x);
    svfloat32_t vmax = vmin;
    svuint32_t vidx_min = svindex_u32(0, 1);
    svuint32_t vidx_max = vidx_min;
    size_t offset = sve_f32_cnt();
    int cnt = sve_f32_cnt();
    for (size_t i = offset; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &x[i]);
        svuint32_t vidxi = svadd_n_u32_z(pg, svindex_u32(0, 1), (uint32_t)i);
        svbool_t mask_lt = svcmplt_f32(pg, vi, vmin);
        svbool_t mask_gt = svcmpgt_f32(pg, vi, vmax);
        vmin = svsel_f32(mask_lt, vi, vmin);
        vmax = svsel_f32(mask_gt, vi, vmax);
        vidx_min = svsel_u32(mask_lt, vidxi, vidx_min);
        vidx_max = svsel_u32(mask_gt, vidxi, vidx_max);
    }
    float best_min = svminv_f32(pg, vmin);
    float best_max = svmaxv_f32(pg, vmax);
    uint32_t idx_min_arr[256], idx_max_arr[256];
    svst1_u32(pg, idx_min_arr, vidx_min);
    svst1_u32(pg, idx_max_arr, vidx_max);
    size_t imin = 0, imax = 0;
    for (int j = 0; j < cnt; j++) {
        if (idx_min_arr[j] < (uint32_t)n && x[idx_min_arr[j]] == best_min) {
            imin = idx_min_arr[j];
            break;
        }
    }
    for (int j = 0; j < cnt; j++) {
        if (idx_max_arr[j] < (uint32_t)n && x[idx_max_arr[j]] == best_max) {
            imax = idx_max_arr[j];
            break;
        }
    }
    for (size_t i = n - (n % (size_t)cnt); i < n; i++) {
        if (x[i] < best_min) {
            best_min = x[i];
            imin = i;
        }
        if (x[i] > best_max) {
            best_max = x[i];
            imax = i;
        }
    }
    *argmin_out = imin;
    *argmax_out = imax;
}

static void simd_sve_add(float* z,
    const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &z[i],
            svadd_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i])));
    }
    for (; i < n; i++)
        z[i] = a[i] + b[i];
}

static void simd_sve_sub(float* z,
    const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &z[i],
            svsub_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i])));
    }
    for (; i < n; i++)
        z[i] = a[i] - b[i];
}

static void simd_sve_mul(float* z,
    const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &z[i],
            svmul_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i])));
    }
    for (; i < n; i++)
        z[i] = a[i] * b[i];
}

static void simd_sve_div(float* restrict z,
    const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &z[i],
            svdiv_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i])));
    }
    for (; i < n; i++)
        z[i] = a[i] / b[i];
}

static void simd_sve_abs(float* restrict out,
    const float* restrict in, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &out[i], svabs_f32_x(pg, svld1_f32(pg, &in[i])));
    }
    for (; i < n; i++)
        out[i] = fabsf(in[i]);
}

static void simd_sve_fma(float* restrict z,
    const float* restrict a,
    const float* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &z[i],
            svmla_f32_z(pg, svld1_f32(pg, &z[i]), svld1_f32(pg, &a[i]),
                svld1_f32(pg, &b[i])));
    }
    for (; i < n; i++)
        z[i] += a[i] * b[i];
}

static void simd_sve_add_s(float* z,
    const float* x, float s,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vs = svdup_f32(s);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_f32(pg, &z[i], svadd_f32_z(pg, svld1_f32(pg, &x[i]), vs));
    for (; i < n; i++)
        z[i] = x[i] + s;
}

static void simd_sve_mul_s(float* z,
    const float* x, float s,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vs = svdup_f32(s);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_f32(pg, &z[i], svmul_f32_z(pg, svld1_f32(pg, &x[i]), vs));
    for (; i < n; i++)
        z[i] = x[i] * s;
}

static void simd_sve_scale_add_s(float* z, float alpha,
    const float* x, float beta,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t va = svdup_f32(alpha);
    svfloat32_t vb = svdup_f32(beta);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_f32(pg, &z[i], svmla_f32_z(pg, vb, va, svld1_f32(pg, &x[i])));
    for (; i < n; i++)
        z[i] = alpha * x[i] + beta;
}

static void simd_sve_sigmoid(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t one = svdup_f32(1.0f);
    svfloat32_t lo = svdup_f32(-30.0f);
    svfloat32_t hi = svdup_f32(30.0f);
    svfloat32_t magic = svdup_f32(-12102203.0f);
    svfloat32_t bias = svdup_f32(1.0f);
    svint32_t zero_i = svdup_s32(0);
    svint32_t top_i = svdup_s32(0x7F800000);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        vi = svmax_f32_z(pg, svmin_f32_z(pg, vi, hi), lo);
        svint32_t bits = svcvt_s32_f32_z(pg, svmul_f32_z(pg, vi, magic));
        bits = svadd_s32_z(pg, bits, svreinterpret_s32_f32(bias));
        bits = svmax_s32_z(pg, bits, zero_i);
        bits = svmin_s32_z(pg, bits, top_i);
        svfloat32_t ve = svadd_f32_z(pg, one, svreinterpret_f32_s32(bits));
        svst1_f32(pg, &out[i], svdiv_f32_z(pg, one, ve));
    }
    for (; i < n; i++)
        out[i] = fast_sigmoid(in[i]);
}

static void simd_sve_tanh_fast(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t two = svdup_f32(2.0f);
    svfloat32_t one = svdup_f32(1.0f);
    svfloat32_t lo = svdup_f32(-10.0f);
    svfloat32_t hi = svdup_f32(10.0f);
    svfloat32_t magic = svdup_f32(-12102203.0f * 2.0f);
    svfloat32_t bias = svdup_f32(1.0f);
    svint32_t zero_i = svdup_s32(0);
    svint32_t top_i = svdup_s32(0x7F800000);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        vi = svmax_f32_z(pg, svmin_f32_z(pg, vi, hi), lo);
        svint32_t bits = svcvt_s32_f32_z(pg, svmul_f32_z(pg, vi, magic));
        bits = svadd_s32_z(pg, bits, svreinterpret_s32_f32(bias));
        bits = svmax_s32_z(pg, bits, zero_i);
        bits = svmin_s32_z(pg, bits, top_i);
        svfloat32_t ve = svsub_f32_z(
            pg,
            svdiv_f32_z(pg, two, svadd_f32_z(pg, one, svreinterpret_f32_s32(bits))),
            one);
        svst1_f32(pg, &out[i], ve);
    }
    for (; i < n; i++)
        out[i] = fast_tanh(in[i]);
}

static void simd_sve_gelu(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        float tmp[64];
        svst1_f32(pg, tmp, svld1_f32(pg, &in[i]));
        for (int j = 0; j < cnt; j++) {
            float x = tmp[j];
            float x3 = x * x * x;
            tmp[j] = 0.5f * x * (1.0f + tanhf(0.7978845608028654f * (x + 0.044715f * x3)));
        }
        svst1_f32(pg, &out[i], svld1_f32(pg, tmp));
    }
    for (; i < n; i++) {
        float x = in[i];
        float x3 = x * x * x;
        out[i] = 0.5f * x * (1.0f + tanhf(0.7978845608028654f * (x + 0.044715f * x3)));
    }
}

static void simd_sve_silu(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t one = svdup_f32(1.0f);
    svfloat32_t magic = svdup_f32(12102203.0f);
    svfloat32_t bias = svdup_f32(1.0f);
    svint32_t zero_i = svdup_s32(0);
    svint32_t top_i = svdup_s32(0x7F800000);
    svfloat32_t vzero = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t x = svld1_f32(pg, &in[i]);
        svfloat32_t neg_x = svneg_f32_z(pg, x);
        svint32_t bits = svcvt_s32_f32_z(pg,
            svmin_f32_z(pg, svmul_f32_z(pg, neg_x, magic),
                svdup_f32(1073741824.0f)));
        bits = svadd_s32_z(pg, bits, svreinterpret_s32_f32(bias));
        bits = svmax_s32_z(pg, bits, zero_i);
        bits = svmin_s32_z(pg, bits, top_i);
        svfloat32_t exp_neg_x = svmax_f32_z(pg, svreinterpret_f32_s32(bits), vzero);
        svfloat32_t sigmoid_x = svdiv_f32_z(pg, one, svadd_f32_z(pg, one, exp_neg_x));
        svst1_f32(pg, &out[i], svmul_f32_z(pg, x, sigmoid_x));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v / (1.0f + expf(-v));
    }
}

static void simd_sve_relu(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t zero = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(pg, &out[i], svmax_f32_z(pg, svld1_f32(pg, &in[i]), zero));
    }
    for (; i < n; i++)
        out[i] = in[i] > 0.0f ? in[i] : 0.0f;
}

static void simd_sve_relu6(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t zero = svdup_f32(0.0f);
    svfloat32_t six = svdup_f32(6.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svst1_f32(
            pg, &out[i],
            svmin_f32_z(pg, svmax_f32_z(pg, svld1_f32(pg, &in[i]), zero), six));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? (v > 6.0f ? 6.0f : v) : (v == v ? 0.0f : v);
    }
}

static void simd_sve_leaky_relu(float* out,
    const float* in,
    float slope, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vzero = svdup_f32(0.0f);
    svfloat32_t vslope = svdup_f32(slope);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t mask = svcmpgt_f32(pg, vi, vzero);
        svfloat32_t neg = svmul_f32_z(mask, vi, vslope);
        svst1_f32(pg, &out[i], svsel_f32(mask, vi, neg));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? v : v * slope;
    }
}

static void simd_sve_elu(float* out,
    const float* in, float alpha,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vzero = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t mask = svcmpgt_f32(pg, vi, vzero);
        float tmp[64];
        svst1_f32(pg, tmp, vi);
        for (int j = 0; j < cnt; j++)
            tmp[j] = alpha * (expf(tmp[j]) - 1.0f);
        svfloat32_t neg = svld1_f32(pg, tmp);
        svst1_f32(pg, &out[i], svsel_f32(mask, vi, neg));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? v : alpha * (expf(v) - 1.0f);
    }
}

static void simd_sve_softmax(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_f32_cnt();
    if (n == 0)
        return;
    if (n < (size_t)cnt) {
        float maxv = in[0];
        for (size_t k = 1; k < n; k++)
            if (in[k] > maxv)
                maxv = in[k];
        float sum = 0.0f;
        for (size_t k = 0; k < n; k++)
            sum += out[k] = fast_exp(in[k] - maxv);
        for (size_t k = 0; k < n; k++)
            out[k] *= 1.0f / sum;
        return;
    }
    svfloat32_t vmax = svld1_f32(pg, in);
    size_t i = cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmax = svmax_f32_z(pg, vmax, svld1_f32(pg, &in[i]));
    float maxv = svmaxv_f32(pg, vmax);
    for (; i < n; i++)
        if (in[i] > maxv)
            maxv = in[i];

    svfloat32_t vmaxv = svdup_f32(maxv);
    svfloat32_t vzero = svdup_f32(0.0f);
    i = 0;
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svsub_f32_z(pg, svld1_f32(pg, &in[i]), vmaxv);
        svint32_t bits = svcvt_s32_f32_z(pg,
            svmla_f32_z(pg, svdup_f32(1065353216.0f), vi, svdup_f32(12102203.0f)));
        bits = svmax_s32_z(pg, bits, svdup_s32(0));
        bits = svmin_s32_z(pg, bits, svdup_s32(0x7F800000));
        svfloat32_t ve = svmax_f32_z(pg, svreinterpret_f32_s32(bits), vzero);
        svst1_f32(pg, &out[i], ve);
    }
    for (; i < n; i++)
        out[i] = fast_exp(in[i] - maxv);

    float sum = svaddv_f32(pg, svld1_f32(pg, out));
    for (i = cnt; i + (size_t)cnt <= n; i += cnt)
        sum += svaddv_f32(pg, svld1_f32(pg, &out[i]));
    for (; i < n; i++)
        sum += out[i];
    svfloat32_t vinv = svdup_f32(1.0f / sum);
    for (i = 0; i + (size_t)cnt <= n; i += cnt)
        svst1_f32(pg, &out[i], svmul_f32_z(pg, svld1_f32(pg, &out[i]), vinv));
    for (; i < n; i++)
        out[i] *= 1.0f / sum;
}

static void simd_sve_log_softmax(float* out,
    const float* in,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_f32_cnt();
    if (n == 0)
        return;
    if (n < (size_t)cnt) {
        float maxv = in[0];
        for (size_t k = 1; k < n; k++)
            if (in[k] > maxv)
                maxv = in[k];
        float sum = 0.0f;
        for (size_t k = 0; k < n; k++)
            sum += fast_exp(in[k] - maxv);
        float log_s = logf(sum);
        for (size_t k = 0; k < n; k++)
            out[k] = in[k] - maxv - log_s;
        return;
    }
    svfloat32_t vmax = svld1_f32(pg, in);
    size_t i = cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmax = svmax_f32_z(pg, vmax, svld1_f32(pg, &in[i]));
    float maxv = svmaxv_f32(pg, vmax);
    for (; i < n; i++)
        if (in[i] > maxv)
            maxv = in[i];

    svfloat32_t vmaxv = svdup_f32(maxv);
    svfloat32_t vsum = svdup_f32(0.0f);
    i = 0;
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svsub_f32_z(pg, svld1_f32(pg, &in[i]), vmaxv);
        svint32_t bits = svcvt_s32_f32_z(pg,
            svmla_f32_z(pg, svdup_f32(1065353216.0f), vi, svdup_f32(12102203.0f)));
        bits = svmax_s32_z(pg, bits, svdup_s32(0));
        bits = svmin_s32_z(pg, bits, svdup_s32(0x7F800000));
        svfloat32_t ve = svreinterpret_f32_s32(bits);
        ve = svmax_f32_z(pg, ve, svdup_f32(0.0f));
        vsum = svadd_f32_z(pg, vsum, ve);
    }
    float sum = svaddv_f32(pg, vsum);
    for (; i < n; i++)
        sum += fast_exp(in[i] - maxv);
    float log_s = logf(sum);
    svfloat32_t vlog_s = svdup_f32(log_s);
    for (i = 0; i + (size_t)cnt <= n; i += cnt)
        svst1_f32(
            pg, &out[i],
            svsub_f32_z(pg, svld1_f32(pg, &in[i]), svadd_f32_z(pg, vmaxv, vlog_s)));
    for (; i < n; i++)
        out[i] = in[i] - (maxv + log_s);
}

static void simd_sve_vexp(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t magic = svdup_f32(12102203.0f);
    svfloat32_t bias = svdup_f32(1.0f);
    svfloat32_t vzero = svdup_f32(0.0f);
    svint32_t zero_i = svdup_s32(0);
    svint32_t top_i = svdup_s32(0x7F800000);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svint32_t bits = svcvt_s32_f32_z(pg,
            svmin_f32_z(pg, svmul_f32_z(pg, vi, magic), svdup_f32(1073741824.0f)));
        bits = svadd_s32_z(pg, bits, svreinterpret_s32_f32(bias));
        bits = svmax_s32_z(pg, bits, zero_i);
        bits = svmin_s32_z(pg, bits, top_i);
        svst1_f32(pg, &out[i], svmax_f32_z(pg, svreinterpret_f32_s32(bits), vzero));
    }
    for (; i < n; i++)
        out[i] = fast_exp(in[i]);
}

static void simd_sve_vlog(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        float tmp[64];
        svst1_f32(pg, tmp, svld1_f32(pg, &in[i]));
        for (int j = 0; j < cnt; j++)
            tmp[j] = tmp[j] > 0.0f ? logf(tmp[j]) : -FLT_MAX;
        svst1_f32(pg, &out[i], svld1_f32(pg, tmp));
    }
    for (; i < n; i++)
        out[i] = in[i] > 0.0f ? logf(in[i]) : -FLT_MAX;
}

static void simd_sve_vsqrt(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vzero = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t mask = svcmpge_f32(pg, vi, vzero);
        svst1_f32(pg, &out[i], svsel_f32(mask, svsqrt_f32_z(mask, vi), vzero));
    }
    for (; i < n; i++)
        out[i] = in[i] >= 0.0f ? sqrtf(in[i]) : 0.0f;
}

static void simd_sve_vrsqrt(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t pos = svcmpgt_f32(pg, vi, svdup_f32(0.0f));
        svfloat32_t vs = svdiv_f32_z(pos, svdup_f32(1.0f), svsqrt_f32_z(pos, vi));
        svst1_f32(pg, &out[i], svsel_f32(pos, vs, svdup_f32(0.0f)));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v > 0.0f ? 1.0f / sqrtf(v) : 0.0f;
    }
}

static void simd_sve_vinv(float* out,
    const float* in, size_t n)
{
    svbool_t pg = svptrue_b32();
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t nz = svcmpne_f32(pg, vi, svdup_f32(0.0f));
        svfloat32_t vs = svdiv_f32_z(nz, svdup_f32(1.0f), vi);
        svst1_f32(pg, &out[i], svsel_f32(nz, vs, svdup_f32(0.0f)));
    }
    for (; i < n; i++)
        out[i] = in[i] != 0.0f ? 1.0f / in[i] : 0.0f;
}

static float simd_sve_dist_l2_sq(const float* restrict a,
    const float* restrict b, size_t d)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t acc = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= d; i += cnt) {
        svfloat32_t diff = svsub_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i]));
        acc = svmla_f32_z(pg, acc, diff, diff);
    }
    float result = svaddv_f32(pg, acc);
    for (; i < d; i++) {
        float df = a[i] - b[i];
        result += df * df;
    }
    return result;
}

static float simd_sve_dist_l1(const float* restrict a,
    const float* restrict b, size_t d)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t acc = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= d; i += cnt) {
        svfloat32_t diff = svsub_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i]));
        acc = svadd_f32_z(pg, acc, svabs_f32_x(pg, diff));
    }
    float result = svaddv_f32(pg, acc);
    for (; i < d; i++)
        result += fabsf(a[i] - b[i]);
    return result;
}

static float simd_sve_dist_cos(const float* restrict a,
    const float* restrict b, size_t d)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vdot = svdup_f32(0.0f);
    svfloat32_t vna = svdup_f32(0.0f);
    svfloat32_t vnb = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= d; i += cnt) {
        svfloat32_t va = svld1_f32(pg, &a[i]);
        svfloat32_t vb = svld1_f32(pg, &b[i]);
        vdot = svmla_f32_z(pg, vdot, va, vb);
        vna = svmla_f32_z(pg, vna, va, va);
        vnb = svmla_f32_z(pg, vnb, vb, vb);
    }
    float dot = svaddv_f32(pg, vdot);
    float na = svaddv_f32(pg, vna);
    float nb = svaddv_f32(pg, vnb);
    for (; i < d; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    double denom = sqrt((double)na * (double)nb);
    if (denom < FLT_MIN)
        return 1.0f;
    return (float)(1.0 - dot / denom);
}

static float simd_sve_dist_cheb(const float* restrict a,
    const float* restrict b, size_t d)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vmax = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= d; i += cnt) {
        svfloat32_t adiff = svabs_f32_x(pg, svsub_f32_z(pg, svld1_f32(pg, &a[i]), svld1_f32(pg, &b[i])));
        vmax = svmax_f32_z(pg, vmax, adiff);
    }
    float result = svmaxv_f32(pg, vmax);
    for (; i < d; i++) {
        float df = fabsf(a[i] - b[i]);
        if (df > result)
            result = df;
    }
    return result;
}

static float simd_sve_dist_l2(const float* restrict a,
    const float* restrict b, size_t d)
{
    return sqrtf(simd_sve_dist_l2_sq(a, b, d));
}

static void simd_sve_dist_matrix_l2_sq(float* restrict out,
    const float* restrict a,
    const float* restrict b,
    size_t n, size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_sve_dist_l2_sq(&a[i * d], &b[j * d], d);
}

static void simd_sve_dist_matrix_l1(float* restrict out,
    const float* restrict a,
    const float* restrict b,
    size_t n, size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_sve_dist_l1(&a[i * d], &b[j * d], d);
}

static void simd_sve_dist_matrix_cos(float* restrict out,
    const float* restrict a,
    const float* restrict b,
    size_t n, size_t m, size_t d)
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            out[i * m + j] = simd_sve_dist_cos(&a[i * d], &b[j * d], d);
}

static void simd_sve_gemv(float* restrict y,
    const float* restrict a,
    const float* restrict x, size_t m,
    size_t n, float beta)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_f32_cnt();
    for (size_t i = 0; i < m; i++) {
        svfloat32_t acc = svdup_f32(0.0f);
        size_t j = 0;
        for (; j + (size_t)cnt <= n; j += cnt) {
            svfloat32_t va = svld1_f32(pg, &a[i * n + j]);
            svfloat32_t vx = svld1_f32(pg, &x[j]);
            acc = svmla_f32_z(pg, acc, va, vx);
        }
        float result = svaddv_f32(pg, acc);
        for (; j < n; j++)
            result += a[i * n + j] * x[j];
        y[i] = beta == 0.0f ? result : beta * y[i] + result;
    }
}

static void simd_sve_gemv_t(float* restrict y,
    const float* restrict a,
    const float* restrict x, size_t m,
    size_t n, float beta)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_f32_cnt();
    if (beta == 0.0f) {
        for (size_t j = 0; j < n; j++)
            y[j] = 0.0f;
    } else {
        for (size_t j = 0; j < n; j++)
            y[j] *= beta;
    }
    for (size_t i = 0; i < m; i++) {
        svfloat32_t vxi = svdup_f32(x[i]);
        const float* row = &a[i * n];
        size_t j = 0;
        for (; j + (size_t)cnt <= n; j += cnt) {
            svfloat32_t vy = svld1_f32(pg, &y[j]);
            svfloat32_t va = svld1_f32(pg, &row[j]);
            svst1_f32(pg, &y[j], svmla_f32_z(pg, vy, vxi, va));
        }
        for (; j < n; j++)
            y[j] += x[i] * row[j];
    }
}

static void simd_sve_gemm(float* restrict c,
    const float* restrict a,
    const float* restrict b, size_t m,
    size_t n, size_t k, float alpha, float beta)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_f32_cnt();
    if (beta == 0.0f) {
        for (size_t i = 0; i < m; i++)
            for (size_t j = 0; j < n; j++)
                c[i * n + j] = 0.0f;
    } else {
        for (size_t i = 0; i < m; i++) {
            size_t j = 0;
            for (; j + (size_t)cnt <= n; j += cnt) {
                svfloat32_t vc = svld1_f32(pg, &c[i * n + j]);
                svst1_f32(pg, &c[i * n + j], svmul_f32_z(pg, vc, svdup_f32(beta)));
            }
            for (; j < n; j++)
                c[i * n + j] *= beta;
        }
    }
    const size_t T = 32;
    for (size_t i0 = 0; i0 < m; i0 += T) {
        size_t imax = i0 + T < m ? i0 + T : m;
        for (size_t j0 = 0; j0 < n; j0 += T) {
            size_t jmax = j0 + T < n ? j0 + T : n;
            for (size_t k0 = 0; k0 < k; k0 += T) {
                size_t kmax = k0 + T < k ? k0 + T : k;
                for (size_t i = i0; i < imax; i++) {
                    for (size_t kk = k0; kk < kmax; kk++) {
                        svfloat32_t vaik = svdup_f32(alpha * a[i * k + kk]);
                        size_t j = j0;
                        for (; j + (size_t)cnt <= jmax; j += cnt) {
                            svfloat32_t vb = svld1_f32(pg, &b[kk * n + j]);
                            svfloat32_t vc = svld1_f32(pg, &c[i * n + j]);
                            svst1_f32(pg, &c[i * n + j], svmla_f32_z(pg, vc, vaik, vb));
                        }
                        for (; j < jmax; j++)
                            c[i * n + j] += alpha * a[i * k + kk] * b[kk * n + j];
                    }
                }
            }
        }
    }
}

static void simd_sve_threshold(float* out,
    const float* in, float t,
    size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vt = svdup_f32(t);
    svfloat32_t one = svdup_f32(1.0f);
    svfloat32_t zero = svdup_f32(0.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t mask = svcmpgt_f32(pg, vi, vt);
        svst1_f32(pg, &out[i], svsel_f32(mask, one, zero));
    }
    for (; i < n; i++)
        out[i] = in[i] > t ? 1.0f : 0.0f;
}

static void simd_sve_threshold_sign(float* restrict out,
    const float* restrict in,
    float t, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vt = svdup_f32(t);
    svfloat32_t pos = svdup_f32(1.0f);
    svfloat32_t neg = svdup_f32(-1.0f);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svbool_t mask = svcmpge_f32(pg, vi, vt);
        svst1_f32(pg, &out[i], svsel_f32(mask, pos, neg));
    }
    for (; i < n; i++)
        out[i] = in[i] >= t ? 1.0f : -1.0f;
}

static float simd_sve_hamming(const uint32_t* restrict a,
    const uint32_t* restrict b,
    size_t n_words)
{
    svbool_t pg = svptrue_b32();
    svuint32_t acc = svdup_u32(0);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n_words; i += cnt) {
        svuint32_t va = svld1_u32(pg, &a[i]);
        svuint32_t vb = svld1_u32(pg, &b[i]);
        svuint32_t xored = sveor_u32_z(pg, va, vb);
        svbool_t pg8 = svptrue_b8();
        svbool_t pg16 = svptrue_b16();
        svuint8_t c8 = svcnt_u8_z(pg8, svreinterpret_u8_u32(xored));
        svuint16_t s16 = svadd_u16_z(pg16, svunpklo_u16(c8), svunpkhi_u16(c8));
        svuint32_t s32 = svadd_u32_z(pg, svunpklo_u32(s16), svunpkhi_u32(s16));
        acc = svadd_u32_z(pg, acc, s32);
    }
    uint32_t result = svaddv_u32(pg, acc);
    for (; i < n_words; i++)
        result += (uint32_t)dyn_popcount32(a[i] ^ b[i]);
    return (float)result;
}

static void simd_sve_topk_indices(const float* restrict vals,
    uint32_t* restrict indices,
    size_t n, size_t k)
{
    simd_scalar_topk_indices(vals, indices, n, k);
}

static void simd_sve_clamp(float* out,
    const float* in, float lo,
    float hi, size_t n)
{
    svbool_t pg = svptrue_b32();
    svfloat32_t vlo = svdup_f32(lo);
    svfloat32_t vhi = svdup_f32(hi);
    size_t i = 0;
    int cnt = sve_f32_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat32_t vi = svld1_f32(pg, &in[i]);
        svst1_f32(pg, &out[i], svmin_f32_z(pg, svmax_f32_z(pg, vi, vlo), vhi));
    }
    for (; i < n; i++) {
        float v = in[i];
        out[i] = v < lo ? lo : (v > hi ? hi : v);
    }
}

#ifdef DYNAJS_SIMD_F64_SVE
static inline int sve_f64_cnt(void) { return svcntd(); }

static double simd_sve_f64_sum(const double* restrict x, size_t n)
{
    svbool_t pg = svptrue_b64();
    svfloat64_t acc = svdup_f64(0.0);
    size_t i = 0;
    int cnt = sve_f64_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        acc = svadd_f64_z(pg, acc, svld1_f64(pg, &x[i]));
    double result = svaddv_f64(pg, acc);
    for (; i < n; i++)
        result += x[i];
    return result;
}

static double simd_sve_f64_dot(const double* restrict a,
    const double* restrict b, size_t n)
{
    svbool_t pg = svptrue_b64();
    svfloat64_t acc = svdup_f64(0.0);
    size_t i = 0;
    int cnt = sve_f64_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        acc = svmla_f64_z(pg, acc, svld1_f64(pg, &a[i]), svld1_f64(pg, &b[i]));
    double result = svaddv_f64(pg, acc);
    for (; i < n; i++)
        result += a[i] * b[i];
    return result;
}

static double simd_sve_f64_max(const double* restrict x, size_t n)
{
    if (n == 0)
        return -DBL_MAX;
    if (n < (size_t)sve_f64_cnt()) {
        double m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] > m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b64();
    svfloat64_t vmax = svld1_f64(pg, x);
    int cnt = sve_f64_cnt();
    size_t i = (size_t)cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmax = svmax_f64_z(pg, vmax, svld1_f64(pg, &x[i]));
    double result = svmaxv_f64(pg, vmax);
    for (; i < n; i++)
        if (x[i] > result)
            result = x[i];
    return result;
}

static double simd_sve_f64_min(const double* restrict x, size_t n)
{
    if (n == 0)
        return DBL_MAX;
    if (n < (size_t)sve_f64_cnt()) {
        double m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] < m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b64();
    svfloat64_t vmin = svld1_f64(pg, x);
    int cnt = sve_f64_cnt();
    size_t i = (size_t)cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmin = svmin_f64_z(pg, vmin, svld1_f64(pg, &x[i]));
    double result = svminv_f64(pg, vmin);
    for (; i < n; i++)
        if (x[i] < result)
            result = x[i];
    return result;
}

static void simd_sve_f64_scale(double* out, const double* x,
    double s, size_t n)
{
    svbool_t pg = svptrue_b64();
    svfloat64_t vs = svdup_f64(s);
    size_t i = 0;
    int cnt = sve_f64_cnt();
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_f64(pg, &out[i], svmul_f64_z(pg, svld1_f64(pg, &x[i]), vs));
    for (; i < n; i++)
        out[i] = x[i] * s;
}

static void simd_sve_f64_axpy(double* restrict y, double a,
    const double* restrict x, size_t n)
{
    svbool_t pg = svptrue_b64();
    svfloat64_t va = svdup_f64(a);
    size_t i = 0;
    int cnt = sve_f64_cnt();
    for (; i + (size_t)cnt <= n; i += cnt) {
        svfloat64_t p = svmul_f64_z(pg, va, svld1_f64(pg, &x[i]));
        svst1_f64(pg, &y[i], svadd_f64_z(pg, svld1_f64(pg, &y[i]), p));
    }
    for (; i < n; i++) {
        double p = a * x[i];
        y[i] = y[i] + p;
    }
}

#endif
#ifdef DYNAJS_SIMD_INT_SVE
#endif

#ifdef DYNAJS_SIMD_INT_SVE
static inline int sve_i32_cnt(void) { return svcntw(); }

static int64_t simd_sve_i32_sum(const int32_t* restrict x, size_t n)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_i32_cnt();
    int64_t result = 0;
    size_t i = 0;
    for (; i + (size_t)cnt <= n; i += cnt)
        result += svaddv_s32(pg, svld1_s32(pg, &x[i]));
    for (; i < n; i++)
        result += (int64_t)x[i];
    return result;
}

static int32_t simd_sve_i32_min(const int32_t* restrict x, size_t n)
{
    if (n == 0)
        return INT32_MAX;
    int cnt = sve_i32_cnt();
    if (n < (size_t)cnt) {
        int32_t m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] < m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b32();
    svint32_t vmin = svld1_s32(pg, x);
    size_t i = (size_t)cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmin = svmin_s32_z(pg, vmin, svld1_s32(pg, &x[i]));
    int32_t result = svminv_s32(pg, vmin);
    for (; i < n; i++)
        if (x[i] < result)
            result = x[i];
    return result;
}

static int32_t simd_sve_i32_max(const int32_t* restrict x, size_t n)
{
    if (n == 0)
        return INT32_MIN;
    int cnt = sve_i32_cnt();
    if (n < (size_t)cnt) {
        int32_t m = x[0];
        for (size_t i = 1; i < n; i++)
            if (x[i] > m)
                m = x[i];
        return m;
    }
    svbool_t pg = svptrue_b32();
    svint32_t vmax = svld1_s32(pg, x);
    size_t i = (size_t)cnt;
    for (; i + (size_t)cnt <= n; i += cnt)
        vmax = svmax_s32_z(pg, vmax, svld1_s32(pg, &x[i]));
    int32_t result = svmaxv_s32(pg, vmax);
    for (; i < n; i++)
        if (x[i] > result)
            result = x[i];
    return result;
}

static void simd_sve_i32_add(int32_t* restrict out, const int32_t* restrict a,
    const int32_t* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_i32_cnt();
    size_t i = 0;
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_s32(pg, &out[i],
            svadd_s32_z(pg, svld1_s32(pg, &a[i]), svld1_s32(pg, &b[i])));
    for (; i < n; i++)
        out[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}

static void simd_sve_i32_mul(int32_t* restrict out, const int32_t* restrict a,
    const int32_t* restrict b, size_t n)
{
    svbool_t pg = svptrue_b32();
    int cnt = sve_i32_cnt();
    size_t i = 0;
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_s32(pg, &out[i],
            svmul_s32_z(pg, svld1_s32(pg, &a[i]), svld1_s32(pg, &b[i])));
    for (; i < n; i++)
        out[i] = (int32_t)((uint32_t)a[i] * (uint32_t)b[i]);
}

static void simd_sve_i32_scale(int32_t* out, const int32_t* x,
    int32_t s, size_t n)
{
    svbool_t pg = svptrue_b32();
    svint32_t vs = svdup_s32(s);
    int cnt = sve_i32_cnt();
    size_t i = 0;
    for (; i + (size_t)cnt <= n; i += cnt)
        svst1_s32(pg, &out[i], svmul_s32_z(pg, svld1_s32(pg, &x[i]), vs));
    for (; i < n; i++)
        out[i] = (int32_t)((uint32_t)x[i] * (uint32_t)s);
}
#endif

void simd_override_sve(simd_t* t)
{
    t->dot = simd_sve_dot;
    t->dot_f = simd_sve_dot_f;
    t->norm_l2_sq = simd_sve_norm_l2_sq;
    t->norm_l2 = simd_sve_norm_l2;
    t->norm_l1 = simd_sve_norm_l1;
    t->add = simd_sve_add;
    t->sub = simd_sve_sub;
    t->mul = simd_sve_mul;
    t->div = simd_sve_div;
    t->abs = simd_sve_abs;
    t->fma = simd_sve_fma;
    t->add_s = simd_sve_add_s;
    t->mul_s = simd_sve_mul_s;
    t->scale_add_s = simd_sve_scale_add_s;
    t->sum = simd_sve_sum;
    t->max = simd_sve_max;
    t->min = simd_sve_min;
    t->argmax = simd_sve_argmax;
    t->argmin = simd_sve_argmin;
    t->argminmax = simd_sve_argminmax;
    t->sigmoid = simd_sve_sigmoid;
    t->relu = simd_sve_relu;
    t->relu6 = simd_sve_relu6;
    t->leaky_relu = simd_sve_leaky_relu;
    t->elu = simd_sve_elu;
    t->tanh_fast = simd_sve_tanh_fast;
    t->gelu = simd_sve_gelu;
    t->silu = simd_sve_silu;
    t->softmax = simd_sve_softmax;
    t->log_softmax = simd_sve_log_softmax;
    t->vexp = simd_sve_vexp;
    t->vlog = simd_sve_vlog;
    t->vsqrt = simd_sve_vsqrt;
    t->vrsqrt = simd_sve_vrsqrt;
    t->vinv = simd_sve_vinv;
    t->dist_l2_sq = simd_sve_dist_l2_sq;
    t->dist_l2 = simd_sve_dist_l2;
    t->dist_l1 = simd_sve_dist_l1;
    t->dist_cos = simd_sve_dist_cos;
    t->dist_cheb = simd_sve_dist_cheb;
    t->dist_matrix_l2_sq = simd_sve_dist_matrix_l2_sq;
    t->dist_matrix_cos = simd_sve_dist_matrix_cos;
    t->dist_matrix_l1 = simd_sve_dist_matrix_l1;
    t->gemv = simd_sve_gemv;
    t->gemv_t = simd_sve_gemv_t;
    t->gemm = simd_sve_gemm;
    t->threshold = simd_sve_threshold;
    t->threshold_sign = simd_sve_threshold_sign;
    t->hamming = simd_sve_hamming;
    t->topk_indices = simd_sve_topk_indices;
    t->clamp = simd_sve_clamp;
#if defined(DYNAJS_SIMD_F64_SVE)
    t->f64_sum = simd_sve_f64_sum;
    t->f64_dot = simd_sve_f64_dot;
    t->f64_min = simd_sve_f64_min;
    t->f64_max = simd_sve_f64_max;
    t->f64_scale = simd_sve_f64_scale;
    t->f64_axpy = simd_sve_f64_axpy;
#endif
#if defined(DYNAJS_SIMD_INT_SVE)
    t->i32_sum = simd_sve_i32_sum;
    t->i32_min = simd_sve_i32_min;
    t->i32_max = simd_sve_i32_max;
    t->i32_add = simd_sve_i32_add;
    t->i32_mul = simd_sve_i32_mul;
    t->i32_scale = simd_sve_i32_scale;
#endif
}

#else
void simd_override_sve(simd_t* t) { (void)t; }
#endif