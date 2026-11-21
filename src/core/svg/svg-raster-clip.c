#include "svg-raster.h"

static void render_clip_shape(Canvas* c, const Node* n, Mat world, int rule, int aa) {
    if (n->type != NODE_SHAPE || !n->path || n->path->n == 0) return;
    size_t smark = svg_scratch_mark();
    size_t sz = path_block_size(n->path);
    void* heap_copy = NULL;
    void* mem = sz <= 16384 ? svg_scratch_try(sz) : NULL;
    if (!mem) mem = heap_copy = svg_malloc(sz);
    if (!mem) return;
    Path* p = path_copy_xform_at(n->path, &world, mem);
    float bb[4];
    path_bbox(p, bb);
    Paint white;
    memset(&white, 0, sizeof(white));
    white.type = PAINT_COLOR;
    white.color = (Color){1, 1, 1, 1};
    fill_path(c, p, rule, &white, 1.0f, NULL, bb, NULL, aa, world);
    svg_mfree(heap_copy);
    svg_scratch_release(smark);
}

static int clip_bbox(const Node* clipdef, Mat cw, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    int any = 0;
    for (int i = 0; i < clipdef->nkids; i++) {
        const Node* kid = clipdef->kids[i];
        if (!kid->visible) continue;
        Mat kw = mat_mul(cw, kid->tm);
        if (kid->type == NODE_SHAPE && kid->path && kid->path->n > 0) {
            float kb[4];
            path_bbox_xform(kid->path, kw, kb);
            if (kb[0] < out[0]) out[0] = kb[0];
            if (kb[1] < out[1]) out[1] = kb[1];
            if (kb[2] > out[2]) out[2] = kb[2];
            if (kb[3] > out[3]) out[3] = kb[3];
            any = 1;
        } else if (kid->type == NODE_GROUP) {
            float gb[4];
            if (clip_bbox(kid, kw, gb)) {
                if (gb[0] < out[0]) out[0] = gb[0];
                if (gb[1] < out[1]) out[1] = gb[1];
                if (gb[2] > out[2]) out[2] = gb[2];
                if (gb[3] > out[3]) out[3] = gb[3];
                any = 1;
            }
        }
    }
    if (!any) out[0] = out[1] = out[2] = out[3] = 0.0f;
    return any;
}

static void render_clip_node(Canvas* c, Node* n, Mat world, int rule, int aa) {
    if (!n || !n->visible) return;
    Mat w = mat_mul(world, n->tm);
    if (n->type == NODE_SHAPE) {
        render_clip_shape(c, n, w, rule, aa);
    } else if (n->type == NODE_GROUP) {
        int r = (n->clip_rule >= 0) ? n->clip_rule : rule;
        for (int i = 0; i < n->nkids; i++) render_clip_node(c, n->kids[i], w, r, aa);
    }
}

Mask* make_rect_mask(const Node* n, Mat w, int W, int H, int aa) {
    (void)aa;
    if (!n) return NULL;
    Pt c[4] = {
        {n->vp_x,         n->vp_y},
        {n->vp_x + n->vp_w, n->vp_y},
        {n->vp_x + n->vp_w, n->vp_y + n->vp_h},
        {n->vp_x,         n->vp_y + n->vp_h}
    };
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt q = mat_apply(w, c[i]);
        if (!isfinite(q.x) || !isfinite(q.y)) return NULL;
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

    if (ix0 >= ix1 || iy0 >= iy1) return mask_alloc(0, 0, 0, 0);
    Mask* m = mask_alloc(ix0, iy0, ix1, iy1);
    if (!m) return NULL;
    for (int y = iy0; y < iy1; y++) {
        float* row = m->m + (size_t)(y - iy0) * (size_t)(ix1 - ix0);
        for (int x = ix0; x < ix1; x++) row[x - ix0] = 1.0f;
    }
    return m;
}

int rect_mask_covers_canvas(const Node* n, Mat w, int W, int H) {
    if (!n || W <= 0 || H <= 0) return 0;
    if (fabsf(w.m[1]) > 1e-6f || fabsf(w.m[2]) > 1e-6f) return 0;
    Pt c[4] = {
        {n->vp_x,           n->vp_y},
        {n->vp_x + n->vp_w, n->vp_y},
        {n->vp_x + n->vp_w, n->vp_y + n->vp_h},
        {n->vp_x,           n->vp_y + n->vp_h}
    };
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt q = mat_apply(w, c[i]);
        if (!isfinite(q.x) || !isfinite(q.y)) return 0;
        if (q.x < x0) x0 = q.x;
        if (q.y < y0) y0 = q.y;
        if (q.x > x1) x1 = q.x;
        if (q.y > y1) y1 = q.y;
    }
    return x0 <= 0.0f && y0 <= 0.0f &&
           x1 >= (float)W && y1 >= (float)H;
}

static int clip_child_bbox(const Node* kid, float out[4]) {
    if (kid->type == NODE_SHAPE && kid->path && kid->path->n > 0) {
        path_bbox(kid->path, out);
        return 1;
    }
    if (kid->type == NODE_GROUP) return clip_bbox(kid, mat_identity(), out);
    return 0;
}

static Mask* make_clip_mask_depth(const Node* clipdef, Mat world,
                                  const float* obj_bbox, int W, int H,
                                  int wy0, int wy1, int aa,
                                  int depth, int* work);

#define CLIP_CHAIN_MAX 16

#define CLIP_NESTED_BUDGET 64

Mask* make_clip_mask(const Node* clipdef, Mat world,
                            const float* obj_bbox, int W, int H,
                            int wy0, int wy1, int aa) {
    int work = 0;
    return make_clip_mask_depth(clipdef, world, obj_bbox, W, H, wy0, wy1, aa,
                                0, &work);
}

static Mask* make_clip_mask_depth(const Node* clipdef, Mat world,
                                  const float* obj_bbox, int W, int H,
                                  int wy0, int wy1, int aa,
                                  int depth, int* work) {
    if (!clipdef) return NULL;
    if (depth >= CLIP_CHAIN_MAX || ++*work > CLIP_NESTED_BUDGET) {
        Mask* z = mask_alloc(0, 0, W, H);
        if (z) z->empty = 1;
        return z;
    }

    Mat cw = mat_mul(world, clipdef->tm);
    if (clipdef->clip_units == UNITS_BBOX && obj_bbox) {
        float bw = obj_bbox[2] - obj_bbox[0];
        float bh = obj_bbox[3] - obj_bbox[1];
        if (bw < 1e-9f) bw = 1e-9f;
        if (bh < 1e-9f) bh = 1e-9f;
        cw = mat_mul(cw, mat_mul(mat_translate(obj_bbox[0], obj_bbox[1]),
                                 mat_scale(bw, bh)));
    }

    float bb[4];
    if (!clip_bbox(clipdef, cw, bb)) {

        return mask_alloc(0, 0, W, H);
    }

    int x0 = svg_f2i(floorf(bb[0]), -1, W + 1, 0);
    int y0 = svg_f2i(floorf(bb[1]), -1, H + 1, 0);
    int x1 = svg_f2i(ceilf(bb[2]), -1, W + 1, 0);
    int y1 = svg_f2i(ceilf(bb[3]), -1, H + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return mask_alloc(0, 0, W, H);

    if (y1 <= wy0 || y0 >= wy1) return mask_alloc(0, 0, 0, 0);

    int ty0 = wy0 > y0 ? wy0 - y0 : 0;
    int ty1 = wy1 < y1 ? wy1 - y0 : y1 - y0;

    Canvas* tmp = canvas_create_rows(x1 - x0, y1 - y0, ty0, ty1);
    if (!tmp) return NULL;
    Mat tw = mat_mul(mat_translate(-(float)x0, -(float)y0), cw);
    for (int i = 0; i < clipdef->nkids; i++) {
        Node* kid = clipdef->kids[i];
        if (!kid->clip) {
            render_clip_node(tmp, kid, tw, clipdef->clip_rule, aa);
            continue;
        }

        if (!kid->visible) continue;
        Canvas* kc = canvas_create_rows(tmp->w, tmp->h, ty0, ty1);
        if (!kc) continue;
        render_clip_node(kc, kid, tw, clipdef->clip_rule, aa);
        float kb[4];
        int has_kb = clip_child_bbox(kid, kb);
        Mask* km = make_clip_mask_depth(kid->clip, mat_mul(cw, kid->tm),
                                        has_kb ? kb : NULL, W, H, wy0, wy1,
                                        aa, depth + 1, work);
        for (int y = ty0; y < ty1; y++) {
            float* d = canvas_at(tmp, 0, y);
            const float* k = canvas_at(kc, 0, y);

            int mx0 = 0, mx1 = 0;
            const float* mr = km ? mask_row(km, y + y0, &mx0, &mx1) : NULL;
            if (km && !mr) continue;
            for (int x = 0; x < tmp->w; x++) {
                float a = k[(size_t)x * 4 + 3];
                if (km) {
                    int ax = x + x0;
                    a *= (ax >= mx0 && ax < mx1) ? mr[ax - mx0] : 0.0f;
                }
                d[(size_t)x * 4 + 3] = d[(size_t)x * 4 + 3] + a * (1.0f - d[(size_t)x * 4 + 3]);
            }
        }
        mask_free(km);
        canvas_free(kc);
    }

    Mask* m = mask_alloc(x0, y0 + ty0, x1, y0 + ty1);
    if (!m) { canvas_free(tmp); return NULL; }
    size_t row = (size_t)(x1 - x0);
    int any = 0;
    for (int y = ty0; y < ty1; y++) {
        for (int x = 0; x < x1 - x0; x++) {
            float v = canvas_at(tmp, x, y)[3];
            m->m[(size_t)(y - ty0) * row + (size_t)x] = v;
            if (v > 0.0f) any = 1;
        }
    }
    m->empty = !any;
    canvas_free(tmp);

    if (clipdef->clip && !m->empty) {
        Mask* outer = make_clip_mask_depth(clipdef->clip, world, obj_bbox,
                                           W, H, wy0, wy1, aa, depth + 1, work);
        if (outer) {
            Mask* both = mask_intersect(m, outer);
            mask_free(outer);
            mask_free(m);
            m = both ? both : mask_alloc(0, 0, W, H);
            if (m && !both) m->empty = 1;
        }
    }
    return m;
}
