#include "svg-raster.h"

void blend_row_const(Canvas* c, int x0, int y, int n,
                            Color s, const float* alpha) {
    float* dst = canvas_at(c, x0, y);
    int i = 0;

#if defined(SVG2IMG_NEON)

    for (; i + 4 <= n; i += 4) {
        float32x4_t a4 = vld1q_f32(alpha + i);
        float32x4_t one = vdupq_n_f32(1.0f);

        if (vminvq_u32(vcgeq_f32(a4, vdupq_n_f32(0.999f))) != 0) {
            float32x4x4_t out = { { vdupq_n_f32(s.r), vdupq_n_f32(s.g),
                                    vdupq_n_f32(s.b), one } };
            vst4q_f32(dst + (size_t)i * 4, out);
            continue;
        }
        float32x4x4_t px = vld4q_f32(dst + (size_t)i * 4);
        float32x4_t da = px.val[3];
        float32x4_t wa = vsubq_f32(one, a4);
        float32x4_t oa = vmlaq_f32(a4, da, wa);
        float32x4_t safe = vbslq_f32(vcgtq_f32(oa, vdupq_n_f32(1e-8f)), oa, one);
        float32x4_t inv = vrecpeq_f32(safe);
        inv = vmulq_f32(inv, vrecpsq_f32(inv, safe));
        float32x4_t dterm = vmulq_f32(da, wa);
        px.val[0] = vmulq_f32(vmlaq_f32(vmulq_f32(px.val[0], dterm),
                                        vdupq_n_f32(s.r), a4), inv);
        px.val[1] = vmulq_f32(vmlaq_f32(vmulq_f32(px.val[1], dterm),
                                        vdupq_n_f32(s.g), a4), inv);
        px.val[2] = vmulq_f32(vmlaq_f32(vmulq_f32(px.val[2], dterm),
                                        vdupq_n_f32(s.b), a4), inv);
        px.val[3] = oa;
        vst4q_f32(dst + (size_t)i * 4, px);
    }
#elif defined(SVG2IMG_SSE2)
    for (; i + 4 <= n; i += 4) {
        __m128 a4 = _mm_loadu_ps(alpha + i);
        __m128 one = _mm_set1_ps(1.0f);
        if (_mm_movemask_ps(_mm_cmpge_ps(a4, _mm_set1_ps(0.999f))) == 0xF) {
            __m128 r = _mm_set1_ps(s.r), g = _mm_set1_ps(s.g), b = _mm_set1_ps(s.b);
            _MM_TRANSPOSE4_PS(r, g, b, one);
            _mm_storeu_ps(dst + (size_t)i * 4, r);
            _mm_storeu_ps(dst + (size_t)i * 4 + 4, g);
            _mm_storeu_ps(dst + (size_t)i * 4 + 8, b);
            _mm_storeu_ps(dst + (size_t)i * 4 + 12, one);
            continue;
        }
        __m128 c0 = _mm_loadu_ps(dst + (size_t)i * 4);
        __m128 c1 = _mm_loadu_ps(dst + (size_t)i * 4 + 4);
        __m128 c2 = _mm_loadu_ps(dst + (size_t)i * 4 + 8);
        __m128 c3 = _mm_loadu_ps(dst + (size_t)i * 4 + 12);
        _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
        __m128 da = c3;
        __m128 wa = _mm_sub_ps(one, a4);
        __m128 oa = _mm_add_ps(a4, _mm_mul_ps(da, wa));
        __m128 safe = _mm_max_ps(oa, _mm_set1_ps(1e-8f));
        __m128 dterm = _mm_mul_ps(da, wa);
        __m128 r = _mm_div_ps(_mm_add_ps(_mm_mul_ps(c0, dterm),
                                         _mm_mul_ps(_mm_set1_ps(s.r), a4)), safe);
        __m128 g = _mm_div_ps(_mm_add_ps(_mm_mul_ps(c1, dterm),
                                         _mm_mul_ps(_mm_set1_ps(s.g), a4)), safe);
        __m128 b = _mm_div_ps(_mm_add_ps(_mm_mul_ps(c2, dterm),
                                         _mm_mul_ps(_mm_set1_ps(s.b), a4)), safe);
        _MM_TRANSPOSE4_PS(r, g, b, oa);
        _mm_storeu_ps(dst + (size_t)i * 4, r);
        _mm_storeu_ps(dst + (size_t)i * 4 + 4, g);
        _mm_storeu_ps(dst + (size_t)i * 4 + 8, b);
        _mm_storeu_ps(dst + (size_t)i * 4 + 12, oa);
    }
#endif

    for (; i < n; i++) {
        float a = alpha[i];
        if (a <= 0.0f) continue;
        if (a > 1.0f) a = 1.0f;
        float* d = dst + (size_t)i * 4;
        if (a >= 0.999f) { d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = 1.0f; continue; }
        float da = d[3];
        if (da <= 0.0f) {
            d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = a;
            continue;
        }
        float oa = a + da * (1.0f - a);
        if (oa <= 0.0f) { d[0] = d[1] = d[2] = d[3] = 0.0f; continue; }
        float inv = 1.0f / oa;
        float wa = 1.0f - a;
        d[0] = (s.r * a + d[0] * da * wa) * inv;
        d[1] = (s.g * a + d[1] * da * wa) * inv;
        d[2] = (s.b * a + d[2] * da * wa) * inv;
        d[3] = oa;
    }
}

void composite_layer(Canvas* c, int dx, int dy,
                            const float* src, int lw, int lh, int sy0, int sh,
                            float op) {

    int ylo = c->wy0 - dy, yhi = c->wy1 - dy;
    if (ylo < sy0) ylo = sy0;
    if (yhi > sy0 + sh) yhi = sy0 + sh;
    if (yhi > lh) yhi = lh;
    for (int yy = ylo; yy < yhi; yy++) {
        const float* srow = src + (size_t)(yy - sy0) * (size_t)lw * 4;
        float* drow = canvas_at(c, dx, dy + yy);
        int xx = 0;

#if defined(SVG2IMG_NEON)
        for (; xx + 4 <= lw; xx += 4) {
            float32x4x4_t px = vld4q_f32(srow + (size_t)xx * 4);
            float32x4x4_t d = vld4q_f32(drow + (size_t)xx * 4);
            float32x4_t a4 = vmulq_f32(px.val[3], vdupq_n_f32(op));
            float32x4_t zero = vdupq_n_f32(0.0f);

            uint32x4_t live = vcgtq_f32(a4, zero);
            float32x4_t one = vdupq_n_f32(1.0f);
            float32x4_t wa = vsubq_f32(one, a4);
            float32x4_t oa = vmlaq_f32(a4, d.val[3], wa);
            float32x4_t safe = vbslq_f32(vcgtq_f32(oa, vdupq_n_f32(1e-8f)), oa, one);
            float32x4_t inv = vrecpeq_f32(safe);
            inv = vmulq_f32(inv, vrecpsq_f32(inv, safe));
            float32x4_t dterm = vmulq_f32(d.val[3], wa);
            float32x4_t r = vmulq_f32(vmlaq_f32(vmulq_f32(d.val[0], dterm),
                                                px.val[0], a4), inv);
            float32x4_t g = vmulq_f32(vmlaq_f32(vmulq_f32(d.val[1], dterm),
                                                px.val[1], a4), inv);
            float32x4_t b = vmulq_f32(vmlaq_f32(vmulq_f32(d.val[2], dterm),
                                                px.val[2], a4), inv);

            d.val[0] = vbslq_f32(live, r, d.val[0]);
            d.val[1] = vbslq_f32(live, g, d.val[1]);
            d.val[2] = vbslq_f32(live, b, d.val[2]);
            d.val[3] = vbslq_f32(live, oa, d.val[3]);
            vst4q_f32(drow + (size_t)xx * 4, d);
        }
#elif defined(SVG2IMG_SSE2)
        for (; xx + 4 <= lw; xx += 4) {
            __m128 s0 = _mm_loadu_ps(srow + (size_t)xx * 4);
            __m128 s1 = _mm_loadu_ps(srow + (size_t)xx * 4 + 4);
            __m128 s2 = _mm_loadu_ps(srow + (size_t)xx * 4 + 8);
            __m128 s3 = _mm_loadu_ps(srow + (size_t)xx * 4 + 12);
            _MM_TRANSPOSE4_PS(s0, s1, s2, s3);
            __m128 d0 = _mm_loadu_ps(drow + (size_t)xx * 4);
            __m128 d1 = _mm_loadu_ps(drow + (size_t)xx * 4 + 4);
            __m128 d2 = _mm_loadu_ps(drow + (size_t)xx * 4 + 8);
            __m128 d3 = _mm_loadu_ps(drow + (size_t)xx * 4 + 12);
            _MM_TRANSPOSE4_PS(d0, d1, d2, d3);
            __m128 opv = _mm_set1_ps(op);
            __m128 one = _mm_set1_ps(1.0f);
            __m128 a4 = _mm_mul_ps(s3, opv);
            __m128 wa = _mm_sub_ps(one, a4);
            __m128 oa = _mm_add_ps(a4, _mm_mul_ps(d3, wa));
            __m128 safe = _mm_max_ps(oa, _mm_set1_ps(1e-8f));
            __m128 dterm = _mm_mul_ps(d3, wa);
            __m128 r = _mm_div_ps(_mm_add_ps(_mm_mul_ps(d0, dterm),
                                             _mm_mul_ps(s0, a4)), safe);
            __m128 g = _mm_div_ps(_mm_add_ps(_mm_mul_ps(d1, dterm),
                                             _mm_mul_ps(s1, a4)), safe);
            __m128 b = _mm_div_ps(_mm_add_ps(_mm_mul_ps(d2, dterm),
                                             _mm_mul_ps(s2, a4)), safe);

            __m128 mask = _mm_cmpgt_ps(a4, _mm_setzero_ps());
            r = _mm_or_ps(_mm_and_ps(mask, r), _mm_andnot_ps(mask, d0));
            g = _mm_or_ps(_mm_and_ps(mask, g), _mm_andnot_ps(mask, d1));
            b = _mm_or_ps(_mm_and_ps(mask, b), _mm_andnot_ps(mask, d2));
            oa = _mm_or_ps(_mm_and_ps(mask, oa), _mm_andnot_ps(mask, d3));
            _MM_TRANSPOSE4_PS(r, g, b, oa);
            _mm_storeu_ps(drow + (size_t)xx * 4, r);
            _mm_storeu_ps(drow + (size_t)xx * 4 + 4, g);
            _mm_storeu_ps(drow + (size_t)xx * 4 + 8, b);
            _mm_storeu_ps(drow + (size_t)xx * 4 + 12, oa);
        }
#endif
        for (; xx < lw; xx++) {
            float sa = srow[(size_t)xx * 4 + 3];
            float a = sa * op;
            if (a <= 0.0f) continue;
            blend(c, dx + xx, dy + yy,
                  (Color){srow[(size_t)xx * 4], srow[(size_t)xx * 4 + 1],
                          srow[(size_t)xx * 4 + 2], sa}, a);
        }
    }
}
