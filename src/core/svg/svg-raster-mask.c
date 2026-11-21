#include "svg-raster.h"

Mask* mask_alloc(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) {

        Mask* m = (Mask*)svg_malloc(sizeof(Mask));
        if (!m) return NULL;
        m->x0 = x0; m->y0 = y0; m->x1 = x1; m->y1 = y1;
        m->empty = 1;
        m->m = (float*)svg_malloc(sizeof(float));
        if (!m->m) { svg_mfree(m); return NULL; }
        m->m[0] = 0.0f;
        return m;
    }
    if ((size_t)(x1 - x0) > (size_t)-1 / (size_t)(y1 - y0) / sizeof(float))
        return NULL;
    Mask* m = (Mask*)svg_malloc(sizeof(Mask));
    if (!m) return NULL;
    m->x0 = x0; m->y0 = y0; m->x1 = x1; m->y1 = y1;
    m->empty = 0;
    size_t n = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    m->m = (float*)svg_malloc(n * sizeof(float));
    if (!m->m) { svg_mfree(m); return NULL; }
    memset(m->m, 0, n * sizeof(float));
    return m;
}

void mask_free(Mask* m) {
    if (!m) return;
    svg_mfree(m->m);
    svg_mfree(m);
}

float mask_get(const Mask* m, int x, int y) {
    if (!m) return 1.0f;
    if (x < m->x0 || y < m->y0 || x >= m->x1 || y >= m->y1) return 0.0f;
    return m->m[(size_t)(y - m->y0) * (size_t)(m->x1 - m->x0) + (size_t)(x - m->x0)];
}

const float* mask_row(const Mask* m, int y, int* x0, int* x1) {
    if (!m || y < m->y0 || y >= m->y1) return NULL;
    *x0 = m->x0;
    *x1 = m->x1;
    return m->m + (size_t)(y - m->y0) * (size_t)(m->x1 - m->x0);
}

static void mask_copy_window(const Mask* m, float* out, int ow, int oh,
                             int offx, int offy) {
    int ax0 = SVG_MAX(m->x0, offx), ax1 = SVG_MIN(m->x1, offx + ow);
    int ay0 = SVG_MAX(m->y0, offy), ay1 = SVG_MIN(m->y1, offy + oh);
    if (ax0 >= ax1 || ay0 >= ay1 || m->empty) return;
    size_t mw = (size_t)(m->x1 - m->x0);
    size_t n = (size_t)(ax1 - ax0) * sizeof(float);
    for (int y = ay0; y < ay1; y++)
        memcpy(out + (size_t)(y - offy) * (size_t)ow + (size_t)(ax0 - offx),
               m->m + (size_t)(y - m->y0) * mw + (size_t)(ax0 - m->x0), n);
}

Mask* mask_shift(const Mask* m, int x0, int y0, int x1, int y1) {
    if (!m) return NULL;
    Mask* r = mask_alloc(0, 0, x1 - x0, y1 - y0);
    if (!r) return NULL;
    if (!r->empty) mask_copy_window(m, r->m, x1 - x0, y1 - y0, x0, y0);
    return r;
}

Mask* mask_translate(const Mask* m, int dx, int dy) {
    if (!m) return NULL;
    int w = m->x1 - m->x0, h = m->y1 - m->y0;
    if (w <= 0 || h <= 0) return NULL;

    Mask* r = mask_alloc(m->x0 - dx, m->y0 - dy, m->x1 - dx, m->y1 - dy);
    if (!r) return NULL;
    if (m->empty) { r->empty = 1; return r; }
    memcpy(r->m, m->m, (size_t)w * (size_t)h * sizeof(float));
    r->empty = m->empty;
    return r;
}

Mask* mask_intersect(const Mask* a, const Mask* b) {
    if (!a) return mask_shift(b, b->x0, b->y0, b->x1, b->y1);
    if (!b) return mask_shift(a, a->x0, a->y0, a->x1, a->y1);
    int x0 = SVG_MAX(a->x0, b->x0), y0 = SVG_MAX(a->y0, b->y0);
    int x1 = SVG_MIN(a->x1, b->x1), y1 = SVG_MIN(a->y1, b->y1);
    if (x0 >= x1 || y0 >= y1) return NULL;
    Mask* r = mask_alloc(x0, y0, x1, y1);
    if (!r) return NULL;

    size_t aw = (size_t)(a->x1 - a->x0), bw = (size_t)(b->x1 - b->x0);
    int n = x1 - x0;
    for (int y = y0; y < y1; y++) {
        const float* restrict pa = a->m + (size_t)(y - a->y0) * aw + (size_t)(x0 - a->x0);
        const float* restrict pb = b->m + (size_t)(y - b->y0) * bw + (size_t)(x0 - b->x0);
        float* restrict o = r->m + (size_t)(y - y0) * (size_t)n;
        for (int x = 0; x < n; x++) o[x] = pa[x] * pb[x];
    }
    return r;
}

Mask* make_mask_from_node(const Node* mn, Mat world, const float* bbox,
                          int W, int H, int wy0, int wy1, int aa) {
    if (!mn) return NULL;
    float ub[4] = {0, 0, 0, 0};
    int has_ub = 0;
    if (mn->mask_units == UNITS_BBOX && bbox) {
        Mat inv;
        if (mat_invert_checked(world, &inv)) {
            Pt c[4] = { {bbox[0], bbox[1]}, {bbox[2], bbox[1]},
                        {bbox[2], bbox[3]}, {bbox[0], bbox[3]} };
            ub[0] = ub[1] = 1e30f;
            ub[2] = ub[3] = -1e30f;
            for (int i = 0; i < 4; i++) {
                Pt q = mat_apply(inv, c[i]);
                if (q.x < ub[0]) ub[0] = q.x;
                if (q.y < ub[1]) ub[1] = q.y;
                if (q.x > ub[2]) ub[2] = q.x;
                if (q.y > ub[3]) ub[3] = q.y;
            }
            if (ub[0] <= ub[2] && ub[1] <= ub[3]) has_ub = 1;
        }
    }

    float rx, ry, rw, rh;
    if (mn->mask_units == UNITS_BBOX && has_ub) {
        rx = ub[0] + mn->mask_x * (ub[2] - ub[0]);
        ry = ub[1] + mn->mask_y * (ub[3] - ub[1]);
        rw = mn->mask_w * (ub[2] - ub[0]);
        rh = mn->mask_h * (ub[3] - ub[1]);
    } else {
        rx = mn->mask_x; ry = mn->mask_y;
        rw = mn->mask_w; rh = mn->mask_h;
    }
    if (!(rw > 0.0f) || !(rh > 0.0f)) {
        Mask* e = mask_alloc(0, 0, 0, 0);
        if (e) e->empty = 1;
        return e;
    }

    Pt c[4] = { {rx, ry}, {rx + rw, ry}, {rx + rw, ry + rh}, {rx, ry + rh} };
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt q = mat_apply(world, c[i]);
        if (q.x < x0) x0 = q.x;
        if (q.y < y0) y0 = q.y;
        if (q.x > x1) x1 = q.x;
        if (q.y > y1) y1 = q.y;
    }
    int ix0 = svg_f2i(floorf(x0), -1, W + 1, 0);
    int iy0 = svg_f2i(floorf(y0), -1, H + 1, 0);
    int ix1 = svg_f2i(ceilf(x1), -1, W + 1, 0);
    int iy1 = svg_f2i(ceilf(y1), -1, H + 1, 0);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > W) ix1 = W;
    if (iy1 > H) iy1 = H;
    if (ix0 >= ix1 || iy0 >= iy1) {
        Mask* e = mask_alloc(0, 0, 0, 0);
        if (e) e->empty = 1;
        return e;
    }

    if (iy1 <= wy0 || iy0 >= wy1) {
        Mask* e = mask_alloc(0, 0, 0, 0);
        if (e) e->empty = 1;
        return e;
    }

    int tw = ix1 - ix0, th = iy1 - iy0;
    if ((size_t)tw > (size_t)-1 / (size_t)th / 4 / sizeof(float)) return NULL;

    int ty0 = wy0 > iy0 ? wy0 - iy0 : 0;
    int ty1 = wy1 < iy1 ? wy1 - iy0 : th;
    Canvas* tile = canvas_create_rows(tw, th, ty0, ty1);
    if (!tile) return NULL;

    Mat content = mat_mul(mat_translate(-(float)ix0, -(float)iy0), world);
    if (mn->mask_content_units == UNITS_BBOX && has_ub) {
        content = mat_mul(content,
                          mat_mul(mat_translate(ub[0], ub[1]),
                                  mat_scale(ub[2] - ub[0], ub[3] - ub[1])));
    }
    render_subtree(tile, mn, content, aa);

    Mask* m = mask_alloc(ix0, iy0 + ty0, ix1, iy0 + ty1);
    if (!m) { canvas_free(tile); return NULL; }
    for (int y = ty0; y < ty1; y++) {
        for (int x = 0; x < tw; x++) {
            const float* p = canvas_at(tile, x, y);
            float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (lum < 0.0f) lum = 0.0f;
            if (lum > 1.0f) lum = 1.0f;
            m->m[(size_t)(y - ty0) * (size_t)tw + (size_t)x] = lum * p[3];
        }
    }
    canvas_free(tile);
    return m;
}
