#include "svg-raster.h"

size_t fb_bytes(int w, int h) {
    return (size_t)w * (size_t)h * 4 * sizeof(float);
}

FBuf* fbuf_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    if ((size_t)w > (size_t)-1 / (size_t)h / 4 / sizeof(float)) return NULL;
    FBuf* b = (FBuf*)svg_malloc(sizeof(FBuf));
    if (!b) return NULL;
    b->w = w;
    b->h = h;
    b->px = (float*)svg_malloc(fb_bytes(w, h));
    if (!b->px) { svg_mfree(b); return NULL; }
    memset(b->px, 0, fb_bytes(w, h));
    return b;
}

void fbuf_free(FBuf* b) {
    if (!b) return;
    svg_mfree(b->px);
    svg_mfree(b);
}

FBuf* fbuf_copy(const FBuf* b) {
    if (!b) return NULL;
    FBuf* c = (FBuf*)svg_malloc(sizeof(FBuf));
    if (!c) return NULL;
    c->w = b->w;
    c->h = b->h;
    c->px = (float*)svg_malloc(fb_bytes(b->w, b->h));
    if (!c->px) { svg_mfree(c); return NULL; }
    memcpy(c->px, b->px, fb_bytes(b->w, b->h));
    return c;
}

static float s2l(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.04045f ? c / 12.92f
                         : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float l2s(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

Color fe_tolinear(Color c) {
    Color r;
    r.r = s2l(c.r);
    r.g = s2l(c.g);
    r.b = s2l(c.b);
    r.a = c.a;
    return r;
}

Color fe_tosrgb(Color c) {
    Color r;
    r.r = l2s(c.r);
    r.g = l2s(c.g);
    r.b = l2s(c.b);
    r.a = c.a;
    return r;
}

static void fe_convert(FBuf* b, float (*fn)(float)) {
    if (!b) return;
    size_t n = (size_t)b->w * (size_t)b->h;
    float in[3] = {0.0f, 0.0f, 0.0f}, out[3];
    for (int k = 0; k < 3; k++) out[k] = fn(0.0f);
    for (size_t i = 0; i < n; i++) {
        float* p = b->px + i * 4;
        for (int k = 0; k < 3; k++) {
            if (p[k] != in[k]) { in[k] = p[k]; out[k] = fn(p[k]); }
            p[k] = out[k];
        }
    }
}

void fe_srgb_to_linear(FBuf* b) { fe_convert(b, s2l); }

void fe_linear_to_srgb(FBuf* b) { fe_convert(b, l2s); }

void fe_premul(const FBuf* in, float* out) {
    size_t n = (size_t)in->w * (size_t)in->h;
    const float* s = in->px;
    for (size_t i = 0; i < n; i++) {
        float a = s[i * 4 + 3];
        out[i * 4 + 0] = s[i * 4 + 0] * a;
        out[i * 4 + 1] = s[i * 4 + 1] * a;
        out[i * 4 + 2] = s[i * 4 + 2] * a;
        out[i * 4 + 3] = a;
    }
}

void fe_unpremul(const float* in, FBuf* out) {
    size_t n = (size_t)out->w * (size_t)out->h;
    for (size_t i = 0; i < n; i++) {
        float a = in[i * 4 + 3];
        float* d = out->px + i * 4;
        if (a > 1e-6f) {
            float inv = 1.0f / a;
            d[0] = in[i * 4 + 0] * inv;
            d[1] = in[i * 4 + 1] * inv;
            d[2] = in[i * 4 + 2] * inv;
        } else {
            d[0] = d[1] = d[2] = 0.0f;
        }
        d[3] = a;
    }
}

int fe_chan(const FBuf* b, int x, int y, int c) {
    if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h) return 0;
    return svg_f2i(b->px[((size_t)y * (size_t)b->w + (size_t)x) * 4 + (size_t)c]
                   * 255.0f + 0.5f, 0, 255, 0);
}

FBuf* fe_new(FilterCtx* fc, int w, int h) {
    if (fc->failed) return NULL;
    if (fc->bytes + fb_bytes(w, h) > fc->max_bytes) { fc->failed = 1; return NULL; }
    FBuf* b = fbuf_create(w, h);
    if (!b) { fc->failed = 1; return NULL; }
    fc->bytes += fb_bytes(w, h);
    return b;
}

void fe_release(FilterCtx* fc, FBuf* b) {
    if (!b) return;
    fc->bytes -= fb_bytes(b->w, b->h);
    fbuf_free(b);
}

const NamedBuf* fe_lookup(FilterCtx* fc, const char* name) {
    if (!fc) return NULL;
    if (!name || !*name) return fc->last >= 0 ? &fc->bufs[fc->last] : NULL;
    for (int i = 0; i < fc->n; i++)
        if (fc->bufs[i].name && !strcmp(fc->bufs[i].name, name))
            return &fc->bufs[i];
    return NULL;
}

static FBuf* fe_transparent(FilterCtx* fc) {
    if (fc->has_transparent) return &fc->transparent;
    if (fc->bytes + fb_bytes(fc->src.w, fc->src.h) > fc->max_bytes) {
        fc->failed = 1;
        return NULL;
    }
    FBuf* b = fbuf_create(fc->src.w, fc->src.h);
    if (!b) return NULL;
    fc->transparent = *b;
    svg_mfree(b);
    fc->has_transparent = 1;
    fc->bytes += fb_bytes(fc->src.w, fc->src.h);
    return &fc->transparent;
}

static FBuf* fe_paint(FilterCtx* fc, int want_stroke) {
    if (want_stroke ? fc->has_stroke_paint : fc->has_fill_paint)
        return (want_stroke ? &fc->stroke_paint : &fc->fill_paint);
    if (!fc->src_node || fc->src_node->type != NODE_SHAPE)
        return fe_transparent(fc);
    FBuf* b = fbuf_create(fc->src.w, fc->src.h);
    if (!b) return NULL;
    if (fc->bytes + fb_bytes(b->w, b->h) > fc->max_bytes) { fbuf_free(b); return NULL; }
    fc->bytes += fb_bytes(b->w, b->h);
    Canvas tmp = { b->w, b->h, b->px, NULL, 0, 0, 0, 0, b->h, 0, b->h };
    Mat local = mat_mul(mat_translate(-(float)fc->ox, -(float)fc->oy),
                        fc->world);
    render_shape_into(&tmp, fc->src_node, local, fc->aa, want_stroke);
    svg_mfree(tmp.scratch);
    tmp.scratch = NULL;
    if (fc->linear) fe_srgb_to_linear(b);
    if (want_stroke) { fc->stroke_paint = *b; fc->has_stroke_paint = 1; }
    else             { fc->fill_paint = *b;   fc->has_fill_paint = 1; }
    svg_mfree(b);
    return (want_stroke ? &fc->stroke_paint : &fc->fill_paint);
}

const FBuf* fe_input(FilterCtx* fc, const char* name) {
    if (!fc) return NULL;
    if (!name || !*name) return fc->last >= 0 ? &fc->bufs[fc->last].fb : &fc->src;
    if (!strcmp(name, "SourceGraphic")) return &fc->src;
    if (!strcmp(name, "SourceAlpha")) return &fc->alpha;
    if (!strcmp(name, "FillPaint")) return fe_paint(fc, 0);
    if (!strcmp(name, "StrokePaint")) return fe_paint(fc, 1);
    if (!strcmp(name, "BackgroundAlpha") || !strcmp(name, "BackgroundImage"))
        return fe_transparent(fc);
    const NamedBuf* e = fe_lookup(fc, name);
    return e ? &e->fb : NULL;
}

float fe_len(FilterCtx* fc, float v, int axis) {
    if (fc->prim_units_bbox) {
        float dim = (axis == 0) ? fc->bw : (axis == 1) ? fc->bh
                                                       : (fc->bw + fc->bh) * 0.5f;
        v *= dim;
    }
    return v * mat_scale_factor(fc->world);
}

static NamedBuf* fe_add(FilterCtx* fc, const char* name, FBuf* b) {
    if (fc->n == fc->cap) {
        int cap = fc->cap ? fc->cap * 2 : 8;
        NamedBuf* nb = (NamedBuf*)svg_realloc(fc->bufs, (size_t)cap * sizeof(NamedBuf));
        if (!nb) { fe_release(fc, b); return NULL; }
        fc->bufs = nb;
        fc->cap = cap;
    }
    char* nm = name ? svg_strdup(name) : NULL;
    if (name && !nm) { fe_release(fc, b); return NULL; }
    NamedBuf* e = &fc->bufs[fc->n];
    e->name = nm;
    e->fb = *b;
    e->sx = fc->last_sx;
    e->sy = fc->last_sy;
    e->sw = fc->last_sw;
    e->sh = fc->last_sh;
    svg_mfree(b);
    fc->n++;
    fc->last_sx = fc->last_sy = 0;
    fc->last_sw = fc->last_sh = 0;
    return e;
}

static void fe_apply_subregion(FilterCtx* fc, const FilterPrim* p, FBuf* out) {
    if (!p->sub_mask || !out || out->w <= 0 || out->h <= 0) return;

    float x0 = -1e30f, y0 = -1e30f, x1 = 1e30f, y1 = 1e30f;
    float ux = p->sub[0], uy = p->sub[1], uw = p->sub[2], uh = p->sub[3];
    if (fc->prim_units_bbox && fc->has_bbox) {
        float bw = fc->bbox[2] - fc->bbox[0], bh = fc->bbox[3] - fc->bbox[1];
        ux = fc->bbox[0] + ux * bw;
        uy = fc->bbox[1] + uy * bh;
        uw *= bw;
        uh *= bh;
    }
    int empty = ((p->sub_mask & 4) && !(uw > 0.0f)) ||
                ((p->sub_mask & 8) && !(uh > 0.0f));
    if (!empty) {

        Pt q[4] = { {ux, uy}, {ux + uw, uy}, {ux, uy + uh}, {ux + uw, uy + uh} };
        float dx0 = 1e30f, dy0 = 1e30f, dx1 = -1e30f, dy1 = -1e30f;
        for (int k = 0; k < 4; k++) {
            Pt d = mat_apply(fc->world, q[k]);
            if (d.x < dx0) dx0 = d.x;
            if (d.x > dx1) dx1 = d.x;
            if (d.y < dy0) dy0 = d.y;
            if (d.y > dy1) dy1 = d.y;
        }

        float k = mat_scale_factor(fc->world);
        if (p->sub_mask & 1) x0 = dx0;
        if (p->sub_mask & 2) y0 = dy0;
        if (p->sub_mask & 4) x1 = (p->sub_mask & 1) ? dx1 : (float)fc->ox + k * uw;
        if (p->sub_mask & 8) y1 = (p->sub_mask & 2) ? dy1 : (float)fc->oy + k * uh;
    }
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (!empty) {
        bx0 = svg_f2i(floorf(x0 + 0.5f) - (float)fc->ox, 0, out->w, 0);
        by0 = svg_f2i(floorf(y0 + 0.5f) - (float)fc->oy, 0, out->h, 0);
        bx1 = svg_f2i(floorf(x1 + 0.5f) - (float)fc->ox, 0, out->w, out->w);
        by1 = svg_f2i(floorf(y1 + 0.5f) - (float)fc->oy, 0, out->h, out->h);
        if (bx1 < bx0) bx1 = bx0;
        if (by1 < by0) by1 = by0;
    }
    for (int y = 0; y < out->h; y++) {
        float* row = out->px + (size_t)y * (size_t)out->w * 4;
        if (y < by0 || y >= by1) {
            memset(row, 0, (size_t)out->w * 4 * sizeof(float));
            continue;
        }
        if (bx0 > 0) memset(row, 0, (size_t)bx0 * 4 * sizeof(float));
        if (bx1 < out->w)
            memset(row + (size_t)bx1 * 4, 0, (size_t)(out->w - bx1) * 4 * sizeof(float));
    }
    fc->last_sx = bx0;
    fc->last_sy = by0;
    fc->last_sw = bx1 - bx0;
    fc->last_sh = by1 - by0;
}

FBuf* filter_execute(const Node* f, const Node* src_node, const FBuf* source,
                     const float bbox[4], int has_bbox, Mat world, int aa,
                     size_t max_bytes, int origin_x, int origin_y) {
    if (!f || !source || f->nprims <= 0 || !source->px) return NULL;

    FilterCtx fc;
    memset(&fc, 0, sizeof(fc));
    fc.bbox = bbox;
    fc.has_bbox = has_bbox;
    fc.bw = (has_bbox && bbox[2] > bbox[0]) ? bbox[2] - bbox[0] : 0.0f;
    fc.bh = (has_bbox && bbox[3] > bbox[1]) ? bbox[3] - bbox[1] : 0.0f;
    fc.prim_units_bbox = (f->filt_prim_units == UNITS_BBOX);
    fc.linear = f->filt_linear;
    fc.world = world;
    fc.aa = aa;
    fc.max_bytes = max_bytes;
    fc.ox = origin_x;
    fc.oy = origin_y;
    fc.src_node = src_node;
    fc.last = -1;
    fc.last_sx = fc.last_sy = 0;
    fc.last_sw = source->w;
    fc.last_sh = source->h;

    FBuf* src = fbuf_copy(source);
    if (!src) return NULL;
    fc.bytes = fb_bytes(src->w, src->h);
    if (fc.max_bytes < fc.bytes) { fbuf_free(src); return NULL; }

    FBuf* al = fbuf_create(src->w, src->h);
    if (!al) { fbuf_free(src); return NULL; }
    fc.bytes += fb_bytes(al->w, al->h);
    size_t npix = (size_t)al->w * (size_t)al->h;
    for (size_t i = 0; i < npix; i++) {
        al->px[i * 4 + 0] = 0.0f;
        al->px[i * 4 + 1] = 0.0f;
        al->px[i * 4 + 2] = 0.0f;
        al->px[i * 4 + 3] = src->px[i * 4 + 3];
    }
    fc.src = *src;
    fc.alpha = *al;
    svg_mfree(src);
    svg_mfree(al);

    if (fc.linear) fe_srgb_to_linear(&fc.src);

    FBuf* result = NULL;
    for (int i = 0; i < f->nprims; i++) {
        FBuf* out = fe_run(&fc, &f->prims[i]);
        if (!out) { fc.failed = 1; break; }
        fe_apply_subregion(&fc, &f->prims[i], out);
        NamedBuf* e = fe_add(&fc, f->prims[i].result, out);
        if (!e) { fc.failed = 1; break; }
        fc.last = fc.n - 1;
    }

    if (!fc.failed) {
        const FBuf* last = (fc.last >= 0) ? &fc.bufs[fc.last].fb : &fc.src;
        result = fbuf_copy(last);
        if (result && fc.linear) fe_linear_to_srgb(result);
    }

    svg_mfree(fc.src.px);
    svg_mfree(fc.alpha.px);
    if (fc.has_transparent) svg_mfree(fc.transparent.px);
    if (fc.has_fill_paint) svg_mfree(fc.fill_paint.px);
    if (fc.has_stroke_paint) svg_mfree(fc.stroke_paint.px);
    for (int i = 0; i < fc.n; i++) {
        svg_mfree(fc.bufs[i].name);
        svg_mfree(fc.bufs[i].fb.px);
    }
    svg_mfree(fc.bufs);
    return result;
}

static FBuf* fe_offset(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    int dx = svg_f2i(floorf(fe_len(fc, p->num[0], 0) + 0.5f),
                     -100000, 100000, 0);
    int dy = svg_f2i(floorf(fe_len(fc, p->num[1], 1) + 0.5f),
                     -100000, 100000, 0);
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    for (int y = 0; y < in->h; y++) {
        int sy = y - dy;
        if (sy < 0 || sy >= in->h) continue;
        for (int x = 0; x < in->w; x++) {
            int sx = x - dx;
            if (sx < 0 || sx >= in->w) continue;
            memcpy(out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4,
                   in->px + ((size_t)sy * (size_t)in->w + (size_t)sx) * 4,
                   4 * sizeof(float));
        }
    }
    return out;
}

static int box_pass(float* a, int w, int h, int left, int right, int horiz) {
    int r = left + right;
    if (r <= 0) return 1;
    float* tmp = (float*)svg_malloc((size_t)(horiz ? w : h) * 4 * sizeof(float));
    if (!tmp) return 0;
    int lines = horiz ? h : w;
    int len = horiz ? w : h;
    for (int l = 0; l < lines; l++) {
        float sum[4] = {0, 0, 0, 0};

        for (int i = -left; i <= right; i++) {
            int x = i < 0 ? 0 : (i >= len ? len - 1 : i);
            int px = horiz ? x : l;
            int py = horiz ? l : x;
            if (i < 0 || i >= len) continue;
            const float* s = a + ((size_t)py * (size_t)w + (size_t)px) * 4;
            sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
        }
        float inv = 1.0f / (float)(r + 1);
        for (int i = 0; i < len; i++) {
            float* d = tmp + (size_t)i * 4;
            d[0] = sum[0] * inv;
            d[1] = sum[1] * inv;
            d[2] = sum[2] * inv;
            d[3] = sum[3] * inv;

            int out_i = i - left;
            int in_i = i + right + 1;
            if (out_i >= 0 && out_i < len) {
                int px = horiz ? out_i : l;
                int py = horiz ? l : out_i;
                const float* s = a + ((size_t)py * (size_t)w + (size_t)px) * 4;
                sum[0] -= s[0]; sum[1] -= s[1]; sum[2] -= s[2]; sum[3] -= s[3];
            }
            if (in_i >= 0 && in_i < len) {
                int px = horiz ? in_i : l;
                int py = horiz ? l : in_i;
                const float* s = a + ((size_t)py * (size_t)w + (size_t)px) * 4;
                sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
            }
        }
        for (int i = 0; i < len; i++) {
            int px = horiz ? i : l;
            int py = horiz ? l : i;
            float* d = a + ((size_t)py * (size_t)w + (size_t)px) * 4;
            d[0] = tmp[(size_t)i * 4 + 0];
            d[1] = tmp[(size_t)i * 4 + 1];
            d[2] = tmp[(size_t)i * 4 + 2];
            d[3] = tmp[(size_t)i * 4 + 3];
        }
    }
    svg_mfree(tmp);
    return 1;
}

static FBuf* fe_blur(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    float sx = fe_len(fc, p->num[0], 0);
    float sy = fe_len(fc, p->num[1], 1);
    if (!(sx > 0.0f) && !(sy > 0.0f)) return fe_offset(fc, p);

    int dx = svg_f2i(floorf(sx * 3.0f * 2.5066282746f / 4.0f + 0.5f),
                     0, 1000, 0);
    int dy = svg_f2i(floorf(sy * 3.0f * 2.5066282746f / 4.0f + 0.5f),
                     0, 1000, 0);

    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    if (!in->px) return out;

    float* buf = (float*)svg_malloc((size_t)in->w * (size_t)in->h * 4 * sizeof(float));
    if (!buf) { fe_release(fc, out); fc->failed = 1; return NULL; }
    fe_premul(in, buf);

    int d[2] = { dx, dy };
    for (int axis = 0; axis < 2; axis++) {
        int n = d[axis];
        if (n <= 0) continue;
        int horiz = (axis == 0);
        int ok;
        if (n % 2 == 1) {
            int r = (n - 1) / 2;
            ok = box_pass(buf, in->w, in->h, r, r, horiz) &&
                 box_pass(buf, in->w, in->h, r, r, horiz) &&
                 box_pass(buf, in->w, in->h, r, r, horiz);
        } else {
            int a = n / 2, b = n / 2 - 1;
            ok = box_pass(buf, in->w, in->h, b, a, horiz) &&
                 box_pass(buf, in->w, in->h, a, b, horiz) &&
                 box_pass(buf, in->w, in->h, b, b, horiz);
        }
        if (!ok) {
            svg_mfree(buf);
            fe_release(fc, out);
            fc->failed = 1;
            return NULL;
        }
    }
    fe_unpremul(buf, out);
    svg_mfree(buf);
    return out;
}

static FBuf* fe_flood(FilterCtx* fc, const FilterPrim* p) {
    FBuf* out = fe_new(fc, fc->src.w, fc->src.h);
    if (!out) return NULL;
    Color c = p->light_color;
    c.a = p->num[0];
    if (c.a < 0.0f) c.a = 0.0f;
    if (c.a > 1.0f) c.a = 1.0f;
    if (fc->linear) c = fe_tolinear(c);

    size_t n = (size_t)out->w * (size_t)out->h;
    for (size_t i = 0; i < n; i++) {
        float* d = out->px + i * 4;
        d[0] = c.r;
        d[1] = c.g;
        d[2] = c.b;
        d[3] = c.a;
    }
    return out;
}

static FBuf* fe_tile(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;

    int sx = 0, sy = 0, sw = in->w, sh = in->h;
    const NamedBuf* e = fe_lookup(fc, p->in);
    if (e && e->sw > 0 && e->sh > 0) {
        sx = e->sx;
        sy = e->sy;
        sw = e->sw;
        sh = e->sh;
    }
    FBuf* out = fe_new(fc, fc->src.w, fc->src.h);
    if (!out) return NULL;
    if (sw <= 0 || sh <= 0) return out;
    for (int y = 0; y < out->h; y++) {
        int ty = sy + (int)(((y - sy) % sh + sh) % sh);
        if (ty < 0) ty = 0;
        if (ty >= in->h) ty = in->h - 1;
        for (int x = 0; x < out->w; x++) {
            int tx = sx + (int)(((x - sx) % sw + sw) % sw);
            if (tx < 0) tx = 0;
            if (tx >= in->w) tx = in->w - 1;
            memcpy(out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4,
                   in->px + ((size_t)ty * (size_t)in->w + (size_t)tx) * 4,
                   4 * sizeof(float));
        }
    }
    return out;
}

static void over_pixel(float* d, const float* s) {
    float sa = s[3];
    float da = d[3];
    if (sa <= 0.0f) return;
    float oa = sa + da * (1.0f - sa);
    if (oa <= 1e-9f) { d[0] = d[1] = d[2] = d[3] = 0.0f; return; }
    float inv = 1.0f / oa;
    d[0] = (s[0] * sa + d[0] * da * (1.0f - sa)) * inv;
    d[1] = (s[1] * sa + d[1] * da * (1.0f - sa)) * inv;
    d[2] = (s[2] * sa + d[2] * da * (1.0f - sa)) * inv;
    d[3] = oa;
}

static float comp_alpha(int mode, float sa, float da) {
    switch (mode) {
        case FE_COMP_IN:   return sa * da;
        case FE_COMP_OUT:  return sa * (1.0f - da);
        case FE_COMP_ATOP: return da;
        case FE_COMP_XOR:  return sa * (1.0f - da) + da * (1.0f - sa);
        default:           return sa + da * (1.0f - sa);
    }
}

static float clamp01(float v) {
    if (!(v > 0.0f)) return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

static FBuf* fe_composite(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* a = fe_input(fc, p->in);
    const FBuf* b = fe_input(fc, p->in2);
    if (!a) a = &fc->src;
    if (!b) b = a;
    FBuf* out = fe_new(fc, a->w, a->h);
    if (!out) return NULL;
    int mode = (int)p->num[0];
    size_t n = (size_t)out->w * (size_t)out->h;
    for (size_t i = 0; i < n; i++) {
        const float* s = a->px + i * 4;
        const float* d = b->px + i * 4;
        float* o = out->px + i * 4;
        float sa = s[3], da = d[3];
        if (mode == FE_COMP_ARITH) {
            float k1 = p->num[1], k2 = p->num[2], k3 = p->num[3], k4 = p->num[4];

            for (int c = 0; c < 4; c++) {
                float sc = s[c] * sa, dc = d[c] * da;
                if (c == 3) { sc = sa; dc = da; }
                o[c] = clamp01(k1 * sc * dc + k2 * sc + k3 * dc + k4);
            }
            continue;
        }
        float oa = comp_alpha(mode, sa, da);
        if (oa <= 1e-9f) { o[0] = o[1] = o[2] = o[3] = 0.0f; continue; }

        float fa, fb;
        switch (mode) {
            case FE_COMP_IN:   fa = da; fb = 0.0f; break;
            case FE_COMP_OUT:  fa = 1.0f - da; fb = 0.0f; break;
            case FE_COMP_ATOP: fa = da; fb = 1.0f - sa; break;
            case FE_COMP_XOR:  fa = 1.0f - da; fb = 1.0f - sa; break;
            default:           fa = 1.0f; fb = 1.0f - sa; break;
        }
        float inv = 1.0f / oa;
        o[0] = (s[0] * sa * fa + d[0] * da * fb) * inv;
        o[1] = (s[1] * sa * fa + d[1] * da * fb) * inv;
        o[2] = (s[2] * sa * fa + d[2] * da * fb) * inv;
        o[3] = oa;
    }
    return out;
}

static float blend_channel(int mode, float cb, float cs) {
    switch (mode) {
        case FE_BLEND_MULTIPLY: return cb * cs;
        case FE_BLEND_SCREEN:   return cb + cs - cb * cs;
        case FE_BLEND_DARKEN:   return cb < cs ? cb : cs;
        case FE_BLEND_LIGHTEN:  return cb > cs ? cb : cs;
        default:                return cs;
    }
}

static FBuf* fe_blend(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* s = fe_input(fc, p->in);
    const FBuf* d = fe_input(fc, p->in2);
    if (!s) s = &fc->src;
    if (!d) d = s;
    FBuf* out = fe_new(fc, s->w, s->h);
    if (!out) return NULL;
    int mode = (int)p->num[0];
    size_t n = (size_t)out->w * (size_t)out->h;
    for (size_t i = 0; i < n; i++) {
        const float* sp = s->px + i * 4;
        const float* dp = d->px + i * 4;
        float* o = out->px + i * 4;
        float sa = sp[3], da = dp[3];
        float oa = sa + da * (1.0f - sa);
        if (oa <= 1e-9f) { o[0] = o[1] = o[2] = o[3] = 0.0f; continue; }
        for (int c = 0; c < 3; c++) {
            float cs = sa > 1e-6f ? sp[c] : 0.0f;
            float cb = da > 1e-6f ? dp[c] : 0.0f;
            float blended = blend_channel(mode, cb, cs);

            float co = (1.0f - da) * cs + da * blended;
            o[c] = (co * sa + cb * da * (1.0f - sa)) / oa;
        }
        o[3] = oa;
    }
    return out;
}

static FBuf* fe_color_matrix(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;

    float m[20];
    int mode = (int)p->num[0];
    if (mode == FE_CM_MATRIX) {
        memcpy(m, p->values, sizeof m);
    } else if (mode == FE_CM_SATURATE) {
        float s = p->num[1];
        float lr = 0.213f, lg = 0.715f, lb = 0.072f;
        m[0] = lr + (1 - lr) * s;  m[1] = lg - lg * s;       m[2] = lb - lb * s;
        m[5] = lr - lr * s;        m[6] = lg + (1 - lg) * s;  m[7] = lb - lb * s;
        m[10] = lr - lr * s;       m[11] = lg - lg * s;      m[12] = lb + (1 - lb) * s;
        m[15] = 0; m[16] = 0; m[17] = 0; m[18] = 1; m[19] = 0;
        m[3] = m[4] = m[8] = m[9] = m[13] = m[14] = 0;
    } else if (mode == FE_CM_HUE_ROTATE) {
        float a = p->num[1] * (float)SVG_PI / 180.0f;
        float cs = cosf(a), sn = sinf(a);
        float lr = 0.213f, lg = 0.715f, lb = 0.072f;
        m[0] = lr + cs * (1 - lr) + sn * (-lr);
        m[1] = lg + cs * (-lg) + sn * (-lg);
        m[2] = lb + cs * (-lb) + sn * (1 - lb);
        m[5] = lr + cs * (-lr) + sn * 0.143f;
        m[6] = lg + cs * (1 - lg) + sn * 0.140f;
        m[7] = lb + cs * (-lb) + sn * (-0.283f);
        m[10] = lr + cs * (-lr) + sn * (-(1 - lr));
        m[11] = lg + cs * (-lg) + sn * lg;
        m[12] = lb + cs * (1 - lb) + sn * lb;
        m[3] = m[4] = m[8] = m[9] = m[13] = m[14] = 0;
        m[15] = 0; m[16] = 0; m[17] = 0; m[18] = 1; m[19] = 0;
    } else {

        memset(m, 0, sizeof m);
        m[0] = 0; m[1] = 0; m[2] = 0;
        m[4] = 0.2126f; m[5] = 0.7152f; m[6] = 0.0722f;
        m[18] = 0;
    }

    size_t npix = (size_t)in->w * (size_t)in->h;
    for (size_t i = 0; i < npix; i++) {
        const float* s = in->px + i * 4;
        float* d = out->px + i * 4;
        if (mode == FE_CM_LUMINANCE_TO_ALPHA) {
            d[0] = d[1] = d[2] = 0.0f;
            d[3] = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
            if (d[3] < 0.0f) d[3] = 0.0f;
            if (d[3] > 1.0f) d[3] = 1.0f;
            continue;
        }
        d[0] = m[0] * s[0] + m[1] * s[1] + m[2] * s[2] + m[3] * s[3] + m[4];
        d[1] = m[5] * s[0] + m[6] * s[1] + m[7] * s[2] + m[8] * s[3] + m[9];
        d[2] = m[10] * s[0] + m[11] * s[1] + m[12] * s[2] + m[13] * s[3] + m[14];
        d[3] = m[15] * s[0] + m[16] * s[1] + m[17] * s[2] + m[18] * s[3] + m[19];

        for (int c = 0; c < 4; c++) {
            if (!(d[c] >= 0.0f)) d[c] = 0.0f;
            if (d[c] > 1.0f) d[c] = 1.0f;
        }
    }
    return out;
}

static float apply_transfer(const FilterPrim* p, int ch, float v) {
    int t = p->ct_type[ch];
    switch (t) {
        case FE_CT_DISCRETE: {
            int n = p->ct_ntable[ch];
            if (n <= 0) return v;

            int k = svg_f2i(v * (float)n, 0, n - 1, 0);
            return p->ct_table[ch][k];
        }
        case FE_CT_LINEAR:
            return p->ct_slope[ch] * v + p->ct_intercept[ch];
        case FE_CT_GAMMA:
            return p->ct_amplitude[ch] * powf(v < 0.0f ? 0.0f : v,
                                              p->ct_exponent[ch]) +
                   p->ct_offset[ch];
        case FE_CT_TABLE: {
            int n = p->ct_ntable[ch];
            if (n <= 0) return v;
            if (n == 1) return p->ct_table[ch][0];
            float x = v * (float)(n - 1);
            int i = svg_f2i(floorf(x), 0, n - 1, 0);
            if (i >= n - 1) return p->ct_table[ch][n - 1];
            float f = x - (float)i;
            if (!(f > 0.0f)) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            return p->ct_table[ch][i] * (1.0f - f) + p->ct_table[ch][i + 1] * f;
        }
        default:
            return v;
    }
}

static FBuf* fe_component_transfer(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    size_t npix = (size_t)in->w * (size_t)in->h;
    for (size_t i = 0; i < npix; i++) {
        const float* s = in->px + i * 4;
        float* d = out->px + i * 4;
        for (int c = 0; c < 4; c++) {
            float v = apply_transfer(p, c, s[c]);
            if (!(v >= 0.0f)) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            d[c] = v;
        }
    }
    return out;
}

static FBuf* fe_merge(FilterCtx* fc, const FilterPrim* p) {
    FBuf* out = fe_new(fc, fc->src.w, fc->src.h);
    if (!out) return NULL;
    for (int i = 0; i < p->nmerge; i++) {
        const FBuf* in = fe_input(fc, p->merge_in[i]);
        if (!in) in = &fc->src;
        size_t n = (size_t)out->w * (size_t)out->h;
        for (size_t k = 0; k < n; k++)
            over_pixel(out->px + k * 4, in->px + k * 4);
    }
    return out;
}

static FBuf* fe_morphology(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    float rx = fe_len(fc, p->num[0], 0);
    float ry = fe_len(fc, p->num[1], 1);
    int irx = svg_f2i(floorf(rx + 0.5f), 0, 1000, 0);
    int iry = svg_f2i(floorf(ry + 0.5f), 0, 1000, 0);

    double budget = 200000000.0;
    while ((irx > 0 || iry > 0) &&
           (double)in->w * (double)in->h *
               (2.0 * (double)irx + 1.0) * (2.0 * (double)iry + 1.0) > budget) {
        if (irx >= iry) irx--;
        else iry--;
    }
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    if (irx == 0 && iry == 0) {
        memcpy(out->px, in->px, fb_bytes(in->w, in->h));
        return out;
    }
    int dilate = (int)p->num[4];
    float* buf = (float*)svg_malloc((size_t)in->w * (size_t)in->h * 4 * sizeof(float));
    if (!buf) { fe_release(fc, out); fc->failed = 1; return NULL; }
    fe_premul(in, buf);

    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            float best[4] = {0, 0, 0, 0};
            if (!dilate) { best[0] = best[1] = best[2] = best[3] = 1e30f; }
            int found = 0;
            for (int ky = -iry; ky <= iry; ky++) {
                int sy = y + ky;
                if (sy < 0 || sy >= in->h) continue;
                for (int kx = -irx; kx <= irx; kx++) {
                    if (irx > 0 || iry > 0) {
                        float fx = irx > 0 ? (float)kx / (float)irx : 0.0f;
                        float fy = iry > 0 ? (float)ky / (float)iry : 0.0f;
                        if (fx * fx + fy * fy > 1.0f) continue;
                    }
                    int sx = x + kx;
                    if (sx < 0 || sx >= in->w) continue;
                    const float* s = buf + ((size_t)sy * (size_t)in->w + (size_t)sx) * 4;
                    if (!found) { best[0] = s[0]; best[1] = s[1]; best[2] = s[2]; best[3] = s[3]; found = 1; }
                    else if (dilate) {
                        if (s[0] > best[0]) best[0] = s[0];
                        if (s[1] > best[1]) best[1] = s[1];
                        if (s[2] > best[2]) best[2] = s[2];
                        if (s[3] > best[3]) best[3] = s[3];
                    } else {
                        if (s[0] < best[0]) best[0] = s[0];
                        if (s[1] < best[1]) best[1] = s[1];
                        if (s[2] < best[2]) best[2] = s[2];
                        if (s[3] < best[3]) best[3] = s[3];
                    }
                }
            }
            if (!found) continue;
            float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
            d[0] = best[0];
            d[1] = best[1];
            d[2] = best[2];
            d[3] = best[3];
        }
    }

    float* tmp = (float*)svg_malloc((size_t)out->w * (size_t)out->h * 4 * sizeof(float));
    if (!tmp) {
        svg_mfree(buf);
        fe_release(fc, out);
        fc->failed = 1;
        return NULL;
    }
    memcpy(tmp, out->px, fb_bytes(out->w, out->h));
    fe_unpremul(tmp, out);
    svg_mfree(tmp);
    svg_mfree(buf);
    return out;
}

FBuf* fe_run(FilterCtx* fc, const FilterPrim* p) {
    if (!fc || fc->failed || !p) return NULL;
    switch (p->type) {
        case FE_OFFSET:             return fe_offset(fc, p);
        case FE_GAUSSIAN_BLUR:      return fe_blur(fc, p);
        case FE_FLOOD:              return fe_flood(fc, p);
        case FE_TILE:               return fe_tile(fc, p);
        case FE_COMPOSITE:          return fe_composite(fc, p);
        case FE_BLEND:              return fe_blend(fc, p);
        case FE_COLOR_MATRIX:       return fe_color_matrix(fc, p);
        case FE_COMPONENT_TRANSFER: return fe_component_transfer(fc, p);
        case FE_MERGE:              return fe_merge(fc, p);
        case FE_MORPHOLOGY:         return fe_morphology(fc, p);
        default:                    return fe_run_generic(fc, p);
    }
}
