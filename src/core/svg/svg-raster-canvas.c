#include "svg-raster.h"

Canvas* canvas_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;

    if ((size_t)w > (size_t)-1 / (size_t)h / 4 / (unsigned)sizeof(float))
        return NULL;
    Canvas* c = (Canvas*)svg_malloc(sizeof(Canvas));
    if (!c) return NULL;
    c->w = w; c->h = h;
    c->px = (float*)svg_calloc((size_t)w * (size_t)h * 4 * sizeof(float));
    c->scratch = NULL;
    c->scratch_cap = c->scratch_used = 0;
    c->scratch_depth = 0;
    c->wy0 = 0;
    c->wy1 = h;
    c->py0 = 0;
    c->ph = h;
    if (!c->px) { svg_mfree(c); return NULL; }
    return c;
}

Canvas* canvas_create_rows(int w, int h, int ry0, int ry1) {
    if (ry0 < 0) ry0 = 0;
    if (ry1 > h) ry1 = h;
    if (ry0 >= ry1) return NULL;
    Canvas* c = canvas_create(w, ry1 - ry0);
    if (!c) return NULL;
    c->h = h;
    c->py0 = ry0;
    c->ph = ry1 - ry0;
    c->wy0 = ry0;
    c->wy1 = ry1;
    return c;
}

void canvas_free(Canvas* c) {
    if (!c) return;
    svg_mfree(c->px);
    svg_mfree(c->scratch);
    svg_mfree(c);
}

void* canvas_scratch_alloc(Canvas* c, size_t n) {
    if (!c || c->scratch_depth > 0) return NULL;
    n = (n + 15u) & ~(size_t)15u;
    if (n > (size_t)-1 - c->scratch_used) return NULL;
    size_t need = c->scratch_used + n;
    if (need > c->scratch_cap) {
        size_t cap = c->scratch_cap ? c->scratch_cap : 65536;
        while (cap < need) {
            if (cap > (size_t)-1 / 2) { cap = need; break; }
            cap *= 2;
        }
        unsigned char* p = (unsigned char*)svg_malloc(cap);
        if (!p) return NULL;
        if (c->scratch_used) memcpy(p, c->scratch, c->scratch_used);
        svg_mfree(c->scratch);
        c->scratch = p;
        c->scratch_cap = cap;
    }
    unsigned char* p = c->scratch + c->scratch_used;
    c->scratch_used = need;
    return p;
}

size_t canvas_scratch_mark(const Canvas* c) {
    return c ? c->scratch_used : 0;
}

void canvas_scratch_reset(Canvas* c, size_t mark) {
    if (c && mark <= c->scratch_used) c->scratch_used = mark;
}

void canvas_clear(Canvas* c, Color col) {
    if (!c) return;
    size_t n = (size_t)c->w * (size_t)c->ph;
    for (size_t i = 0; i < n; i++) {
        float* p = c->px + i * 4;
        p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = col.a;
    }
}

void blend(Canvas* c, int x, int y, Color s, float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    float* d = canvas_at(c, x, y);
    float da = d[3];
    if (a >= 0.9995f && da <= 0.0005f) {
        d[0] = s.r; d[1] = s.g; d[2] = s.b; d[3] = a;
        return;
    }
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) { d[0] = d[1] = d[2] = d[3] = 0.0f; return; }
    float inv = 1.0f / oa;
    float wa = 1.0f - a;
    d[0] = (s.r * a + d[0] * da * wa) * inv;
    d[1] = (s.g * a + d[1] * da * wa) * inv;
    d[2] = (s.b * a + d[2] * da * wa) * inv;
    d[3] = oa;
}

void blend_row_var(Canvas* c, int x0, int y, int n, const Color* restrict col,
                   const float* restrict cov, float k) {
    float* row = canvas_at(c, x0, y);
    for (int i = 0; i < n; i++) {
        float cv = cov[i];
        if (cv <= 0.0f) continue;
        Color s = col[i];
        float a = s.a * k * cv;
        if (a <= 0.0f) continue;
        if (a > 1.0f) a = 1.0f;
        float* d = row + (size_t)i * 4;
        float da = d[3];
        if (a >= 0.9995f && da <= 0.0005f) {
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
