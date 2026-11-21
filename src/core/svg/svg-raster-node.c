#include "svg-raster.h"
#include <stdatomic.h>
#if !defined(SVG2IMG_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#include <sched.h>
#define SVG_YIELD() sched_yield()
#else
#define SVG_YIELD() ((void)0)
#endif

typedef struct {
    const svg2img_limits* lim;
    int layers;
    int clips;
    int masks;
    int filters;

    const Canvas* top;
    unsigned gen;
    struct FilterCache* fc;
} RenderState;

typedef struct {
    atomic_int state;
    atomic_int refs;
    FBuf* out;
    int ix0, iy0, iw, ih;
} FilterEntry;

typedef struct FilterCache {
    FilterEntry* e;
    int n;
    int band;
} FilterCache;

static void path_bbox_transformed(const Path* p, Mat m, float out[4]) {
    float bb[4];
    path_bbox(p, bb);
    if (bb[0] > bb[2]) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    Pt c[4] = { {bb[0], bb[1]}, {bb[2], bb[1]}, {bb[2], bb[3]}, {bb[0], bb[3]} };
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt q = mat_apply(m, c[i]);
        if (q.x < out[0]) out[0] = q.x;
        if (q.y < out[1]) out[1] = q.y;
        if (q.x > out[2]) out[2] = q.x;
        if (q.y > out[3]) out[3] = q.y;
    }
}

int node_bbox_world(const Node* n, Mat full_w, float out[4]) {
    if (!n || !n->visible || n->hidden_subtree) return 0;
    if (n->type == NODE_SHAPE && n->path && n->path->n > 0) {
        path_bbox_transformed(n->path, full_w, out);
        if (n->stroke.type != PAINT_NONE && n->sw > 0.0f) {

            float half = n->sw * 0.5f * mat_scale_factor(full_w);
            out[0] -= half; out[1] -= half;
            out[2] += half; out[3] += half;
        }
        return 1;
    }
    if (n->type == NODE_TEXT && n->nchunks > 0 && n->chunks) {
        out[0] = out[1] = 1e30f;
        out[2] = out[3] = -1e30f;
        int any = 0;
        for (int ci = 0; ci < n->nchunks; ci++) {
            const TextChunk* ch = &n->chunks[ci];
            if (!ch->text || !*ch->text || !(ch->fontsize > 0.0f)) continue;
            float scale = ch->fontsize / 7.0f;
            float adv = 6.0f * scale + ch->letterspacing;
            float total = (float)strlen(ch->text) * adv;
            float top = ch->y - 7.0f * scale;
            float bottom = ch->y;
            if (ch->font) {

                total = 0.0f;
                for (const char* q = ch->text; *q; q++) {
                    int gid = font_face_glyph_index(ch->font, (unsigned char)*q);
                    total += font_face_advance(ch->font, gid) * ch->fontsize +
                             ch->letterspacing;
                }
                float asc = 0.8f, desc = -0.2f;
                font_face_metrics(ch->font, &asc, &desc);
                top = ch->y - asc * ch->fontsize;
                bottom = ch->y - desc * ch->fontsize;
            }
            float x = ch->x;
            if (ch->anchor == 1) x -= total * 0.5f;
            else if (ch->anchor == 2) x -= total;
            Pt c[4] = { {x, top}, {x + total, top},
                        {x + total, bottom}, {x, bottom} };
            for (int i = 0; i < 4; i++) {
                Pt q = mat_apply(full_w, c[i]);
                if (q.x < out[0]) out[0] = q.x;
                if (q.y < out[1]) out[1] = q.y;
                if (q.x > out[2]) out[2] = q.x;
                if (q.y > out[3]) out[3] = q.y;
                any = 1;
            }
        }
        return any;
    }
    if (n->type == NODE_TEXT && n->text && *n->text && n->fontsize > 0.0f) {
        float scale = n->fontsize / 7.0f;
        float adv = 6.0f * scale + n->letterspacing;
        float total = 0.0f;
        for (const char* q = n->text; *q; q++) total += adv;
        float x = n->x;
        if (n->anchor == 1) x -= total * 0.5f;
        else if (n->anchor == 2) x -= total;
        float y = n->y;
        Pt corners[4] = {
            {x, y - 7.0f * scale}, {x + total, y - 7.0f * scale},
            {x + total, y}, {x, y}
        };
        out[0] = out[1] = 1e30f;
        out[2] = out[3] = -1e30f;
        for (int i = 0; i < 4; i++) {
            Pt q = mat_apply(full_w, corners[i]);
            if (q.x < out[0]) out[0] = q.x;
            if (q.y < out[1]) out[1] = q.y;
            if (q.x > out[2]) out[2] = q.x;
            if (q.y > out[3]) out[3] = q.y;
        }
        return 1;
    }
    if (n->type == NODE_GROUP) {
        int any = 0;
        out[0] = out[1] = 1e30f;
        out[2] = out[3] = -1e30f;
        for (int i = 0; i < n->nkids; i++) {
            float kb[4];
            Mat cw = mat_mul(full_w, n->kids[i]->tm);
            if (node_bbox_world(n->kids[i], cw, kb)) {
                if (kb[0] < out[0]) out[0] = kb[0];
                if (kb[1] < out[1]) out[1] = kb[1];
                if (kb[2] > out[2]) out[2] = kb[2];
                if (kb[3] > out[3]) out[3] = kb[3];
                any = 1;
            }
        }
        return any;
    }
    return 0;
}

static Mask* node_clip_mask(const Node* n, Mat world, Canvas* c,
                            const Mask* inherited, const float* obj_bbox, int aa,
                            int* out_empty) {
    if (out_empty) *out_empty = 0;
    if (!n->clip) return NULL;
    Mask* m = make_clip_mask(n->clip, world, obj_bbox, c->w, c->h,
                             c->wy0, c->wy1, aa);
    if (!m) return NULL;
    if (!inherited) {
        if (m->empty) { mask_free(m); if (out_empty) *out_empty = 1; return NULL; }
        return m;
    }
    Mask* both = mask_intersect(inherited, m);
    mask_free(m);
    if (!both) { if (out_empty) *out_empty = 1; return NULL; }
    return both;
}

static void render_node(Canvas* c, const Node* n, Mat world, float opacity,
                        const Mask* clip, int aa, RenderState* rs);
static void paint_self(Canvas* c, const Node* n, Mat w, float op,
                       const Mask* eff, const Mask* self_mask, int aa, RenderState* rs);
static void render_filtered(Canvas* c, const Node* n, Mat w, float op,
                            const Mask* clip, const Mask* self_mask, int aa,
                            RenderState* rs);

#define RENDER_MAX_DEPTH 2048
static SVG_THREADLOCAL int g_render_depth = 0;

static void render_node_body(Canvas* c, const Node* n, Mat world, float opacity,
                             const Mask* clip, int aa, RenderState* rs);

static void render_node(Canvas* c, const Node* n, Mat world, float opacity,
                        const Mask* clip, int aa, RenderState* rs) {
    if (g_render_depth >= RENDER_MAX_DEPTH) return;
    g_render_depth++;
    render_node_body(c, n, world, opacity, clip, aa, rs);
    g_render_depth--;
}

static void composite_layer_mask(Canvas* c, int dx, int dy, const float* src,
                                 int lw, int lh, int sy0, int sh, float op,
                                 const Mask* m) {

    for (int yy = sy0; yy < lh && yy < sy0 + sh; yy++) {
        if (dy + yy < c->wy0 || dy + yy >= c->wy1) continue;
        int mx0 = 0, mx1 = 0;
        const float* mrow = mask_row(m, dy + yy, &mx0, &mx1);
        if (!mrow) continue;
        int lo = dx > mx0 ? dx : mx0;
        int hi = dx + lw < mx1 ? dx + lw : mx1;
        const float* srow = src + (size_t)(yy - sy0) * (size_t)lw * 4;
        for (int xx = lo - dx; xx < hi - dx; xx++) {
            const float* s = srow + (size_t)xx * 4;
            float a = s[3] * op * mrow[dx + xx - mx0];
            if (a <= 0.0f) continue;
            blend(c, dx + xx, dy + yy, (Color){s[0], s[1], s[2], s[3]}, a);
        }
    }
}

static void render_group_layered(Canvas* c, const Node* n, Mat w, float op,
                                 const Mask* eff, const Mask* self_mask, int aa,
                                 RenderState* rs) {

    if (rs->lim && rs->layers >= rs->lim->max_layers) {
        for (int i = 0; i < n->nkids; i++)
            render_node(c, n->kids[i], w, op, eff, aa, rs);
        return;
    }
    rs->layers++;

    float bb[4];
    if (!node_bbox_world(n, w, bb)) { rs->layers--; return; }

    int x0 = svg_f2i(floorf(bb[0]), -1, c->w + 1, 0);
    int y0 = svg_f2i(floorf(bb[1]), -1, c->h + 1, 0);
    int x1 = svg_f2i(ceilf(bb[2]), -1, c->w + 1, 0);
    int y1 = svg_f2i(ceilf(bb[3]), -1, c->h + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) { rs->layers--; return; }

    if (y1 <= c->wy0 || y0 >= c->wy1) { rs->layers--; return; }

    Canvas* tmp = canvas_create_rows(x1 - x0, y1 - y0,
                                     c->wy0 > y0 ? c->wy0 - y0 : 0,
                                     c->wy1 < y1 ? c->wy1 - y0 : y1 - y0);
    if (!tmp) { rs->layers--; return; }

    Mat tw = mat_mul(mat_translate(-(float)x0, -(float)y0), w);

    Mask* tclip = mask_translate(eff, x0, y0);
    for (int i = 0; i < n->nkids; i++)
        render_node(tmp, n->kids[i], tw, 1.0f, tclip, aa, rs);
    mask_free(tclip);

    if (self_mask)
        composite_layer_mask(c, x0, y0, tmp->px, tmp->w, tmp->h, tmp->py0,
                             tmp->ph, op,
                             self_mask);
    else
        composite_layer(c, x0, y0, tmp->px, tmp->w, tmp->h, tmp->py0, tmp->ph,
                        op);
    canvas_free(tmp);
    rs->layers--;
}

static void render_node_body(Canvas* c, const Node* n, Mat world, float opacity,
                             const Mask* clip, int aa, RenderState* rs) {

    if (!n || n->hidden_subtree || n->type == NODE_SYMBOL ||
        n->type == NODE_IGNORE || n->type == NODE_PATTERNDEF ||
        n->type == NODE_GRADDEF || n->type == NODE_CLIPDEF ||
        n->type == NODE_MASKDEF || n->type == NODE_MARKERDEF) return;
    if (rs->top == c && n->ext_ok && n->ext_gen == rs->gen &&
        (n->ext_y1 <= (float)c->wy0 || n->ext_y0 >= (float)c->wy1))
        return;

    if (!n->visible && (n->type == NODE_SHAPE || n->type == NODE_TEXT)) return;
    Mat w = mat_mul(world, n->tm);
    float op = opacity * n->opacity;
    if (op <= 0.0f) return;

    int aae = n->crisp ? 1 : aa;

    float bb[4];
    int has_bb = node_bbox_world(n, w, bb);

    int clipped_away = 0;
    Mask* own = NULL;
    if (n->vp_clip && !rect_mask_covers_canvas(n, w, c->w, c->h)) {

        Mask* vm = make_rect_mask(n, w, c->w, c->h, aa);
        if (vm) {
            if (vm->empty) { mask_free(vm); return; }
            if (!clip) {
                own = vm;
            } else {
                own = mask_intersect(clip, vm);
                mask_free(vm);
                if (!own) return;
            }
        }
    }
    if (n->clip) {

        if (!rs->lim || rs->clips < rs->lim->max_clip_depth) {
            Mask* cm;

            float ubb[4];
            int has_ubb = node_bbox_world(n, mat_identity(), ubb);
            rs->clips++;
            cm = node_clip_mask(n, w, c, own ? own : clip, has_ubb ? ubb : NULL,
                                aa, &clipped_away);
            rs->clips--;
            if (clipped_away) { mask_free(own); return; }
            mask_free(own);
            own = cm;
        }
    }

    Mask* self_mask = NULL;
    if (n->mask && (!rs->lim || rs->masks < rs->lim->max_layers)) {
        rs->masks++;
        Mask* mm = make_mask_from_node(n->mask, w, has_bb ? bb : NULL,
                                       c->w, c->h, c->wy0, c->wy1, aa);
        rs->masks--;
        if (mm) {
            if (mm->empty) { mask_free(mm); mask_free(own); return; }
            self_mask = mm;
        }
    }
    const Mask* eff = own ? own : clip;

    if (n->filter && (!rs->lim || rs->filters < rs->lim->max_layers)) {
        rs->filters++;
        render_filtered(c, n, w, op, eff, self_mask, aae, rs);
        rs->filters--;
    } else {
        paint_self(c, n, w, op, eff, self_mask, aae, rs);
    }
    mask_free(self_mask);
    mask_free(own);
}

static void paint_self(Canvas* c, const Node* n, Mat w, float op,
                       const Mask* eff, const Mask* self_mask, int aa,
                       RenderState* rs) {
    if (n->type == NODE_GROUP || n->type == NODE_SYMBOL ||
        n->type == NODE_TRANSPARENT) {

        if (op < 0.999f || self_mask) {
            float c_op = op;
            const Node* g = n;

            while (g->nkids == 1 && !g->clip && g->kids[0]->type == NODE_GROUP &&
                   !g->kids[0]->clip && !g->kids[0]->mask &&
                   !g->kids[0]->filter && !g->kids[0]->vp_clip) {
                const Node* child = g->kids[0];
                c_op *= child->opacity;
                g = child;
                if (c_op <= 0.0f) return;
            }
            if (g != n) {

                Mat chain_w = w;
                for (const Node* t = n->kids[0]; ; t = t->kids[0]) {
                    chain_w = mat_mul(chain_w, t->tm);
                    if (t == g) break;
                }
                render_group_layered(c, g, chain_w, c_op, eff, self_mask,
                                     aa, rs);
                return;
            }
            render_group_layered(c, n, w, op, eff, self_mask, aa, rs);
        } else {
            for (int i = 0; i < n->nkids; i++)
                render_node(c, n->kids[i], w, op, eff, aa, rs);
        }
    } else if (n->type == NODE_SHAPE || n->type == NODE_TEXT ||
               n->type == NODE_IMAGE) {

        const Mask* m = self_mask ? self_mask : eff;
        Mask* merged = NULL;
        if (self_mask && eff) {
            merged = mask_intersect(eff, self_mask);
            if (!merged) return;
            m = merged;
        }
        if (n->type == NODE_SHAPE) render_shape(c, n, w, op, m, aa);
        else if (n->type == NODE_TEXT) render_text(c, n, w, op, m, aa);
        else render_image(c, n, w, op, m, aa);
        if (merged) mask_free(merged);
    }
}

static int filter_region(const Node* f, Mat w, const float* lbb, int has_bb,
                         int cw, int ch, int* ox, int* oy, int* iw, int* ih) {
    float rx, ry, rw, rh;
    if (f->filt_units == UNITS_BBOX && has_bb) {
        float bw = lbb[2] - lbb[0];
        float bh = lbb[3] - lbb[1];
        if (!(bw > 0.0f) || !(bh > 0.0f)) return 0;
        rx = lbb[0] + f->filt_x * bw;
        ry = lbb[1] + f->filt_y * bh;
        rw = f->filt_w * bw;
        rh = f->filt_h * bh;
    } else {
        rx = f->filt_x;
        ry = f->filt_y;
        rw = f->filt_w;
        rh = f->filt_h;
    }
    if (!(rw > 0.0f) || !(rh > 0.0f)) return 0;
    Pt q[4] = { {rx, ry}, {rx + rw, ry}, {rx + rw, ry + rh}, {rx, ry + rh} };
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt p = mat_apply(w, q[i]);
        if (p.x < x0) x0 = p.x;
        if (p.y < y0) y0 = p.y;
        if (p.x > x1) x1 = p.x;
        if (p.y > y1) y1 = p.y;
    }
    int ix0 = svg_f2i(floorf(x0), -1, cw + 1, 0);
    int iy0 = svg_f2i(floorf(y0), -1, ch + 1, 0);
    int ix1 = svg_f2i(ceilf(x1), -1, cw + 1, 0);
    int iy1 = svg_f2i(ceilf(y1), -1, ch + 1, 0);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > cw) ix1 = cw;
    if (iy1 > ch) iy1 = ch;
    if (ix0 >= ix1 || iy0 >= iy1) return 0;
    *ox = ix0;
    *oy = iy0;
    *iw = ix1 - ix0;
    *ih = iy1 - iy0;
    return 1;
}

static FBuf* filter_compute(const Node* n, Mat w, int cw, int chh, int ix0,
                            int iy0, int iw, int ih, int aa, RenderState* rs) {
    Node* f = n->filter;
    float lbb[4];
    int has_bb = node_bbox_world(n, mat_identity(), lbb);
    (void)cw; (void)chh;
    Canvas* tmp = canvas_create(iw, ih);
    if (!tmp) return NULL;
    Mat local = mat_mul(mat_translate(-(float)ix0, -(float)iy0), w);
    RenderState sub;
    sub.lim = rs->lim;
    sub.layers = 0;
    sub.clips = 0;
    sub.masks = 0;
    sub.filters = rs->filters;
    sub.top = NULL;
    sub.gen = 0;
    sub.fc = NULL;
    paint_self(tmp, n, local, 1.0f, NULL, NULL, aa, &sub);

    FBuf src;
    src.w = tmp->w;
    src.h = tmp->h;
    src.px = tmp->px;
    tmp->px = NULL;
    canvas_free(tmp);

    size_t region_bytes = fb_bytes(iw, ih);
    size_t max_bytes = region_bytes * 6 + (8u << 20);
    if (max_bytes > (512u << 20)) max_bytes = 512u << 20;
    FBuf* out = filter_execute(f, n, &src, lbb, has_bb, w, aa, max_bytes,
                               ix0, iy0);
    if (!out) out = fbuf_copy(&src);
    svg_mfree(src.px);
    return out;
}

static void filter_composite(Canvas* c, const FBuf* out, int ix0, int iy0,
                             float op, const Mask* clip, const Mask* self_mask) {
    int iw = out->w, ih = out->h;
    for (int y = 0; y < ih; y++) {
        if (iy0 + y < c->wy0 || iy0 + y >= c->wy1) continue;
        int cx0 = 0, cx1 = 0, sx0 = 0, sx1 = 0;
        const float* crow = clip ? mask_row(clip, iy0 + y, &cx0, &cx1) : NULL;
        const float* srow = self_mask
                                ? mask_row(self_mask, iy0 + y, &sx0, &sx1)
                                : NULL;
        const float* rowp = out->px + (size_t)y * (size_t)iw * 4;
        for (int x = 0; x < iw; x++) {
            const float* s = rowp + (size_t)x * 4;
            float a = s[3] * op;
            if (clip) {
                int gx = ix0 + x;
                a *= (gx >= cx0 && gx < cx1) ? crow[gx - cx0] : 0.0f;
            }
            if (self_mask) {
                int gx = ix0 + x;
                a *= (gx >= sx0 && gx < sx1) ? srow[gx - sx0] : 0.0f;
            }
            if (a <= 0.0f) continue;
            blend(c, ix0 + x, iy0 + y,
                  (Color){s[0], s[1], s[2], s[3]}, a);
        }
    }
}

static void render_filtered(Canvas* c, const Node* n, Mat w, float op,
                            const Mask* clip, const Mask* self_mask, int aa,
                            RenderState* rs) {
    Node* f = n->filter;
    float lbb[4];
    int has_bb = node_bbox_world(n, mat_identity(), lbb);
    int ix0 = 0, iy0 = 0, iw = 0, ih = 0;
    if (!filter_region(f, w, lbb, has_bb, c->w, c->h, &ix0, &iy0, &iw, &ih))
        return;

    if (iy0 + ih <= c->wy0 || iy0 >= c->wy1) return;

    if (rs->fc && rs->top == c && n->ext_gen == rs->gen &&
        n->ext_fidx >= 0 && n->ext_fidx < rs->fc->n) {
        FilterEntry* e = &rs->fc->e[n->ext_fidx];
        int expect = 0;
        if (atomic_compare_exchange_strong(&e->state, &expect, 1)) {
            e->out = filter_compute(n, w, c->w, c->h, ix0, iy0, iw, ih, aa, rs);
            e->ix0 = ix0;
            e->iy0 = iy0;
            int b0 = iy0 / rs->fc->band, b1 = (iy0 + ih - 1) / rs->fc->band;
            atomic_store(&e->refs, b1 - b0 + 1);
            atomic_store_explicit(&e->state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&e->state, memory_order_acquire) != 2)
                SVG_YIELD();
        }
        if (e->out) filter_composite(c, e->out, e->ix0, e->iy0, op, clip, self_mask);
        if (atomic_fetch_sub(&e->refs, 1) == 1) {
            fbuf_free(e->out);
            e->out = NULL;
        }
        return;
    }

    FBuf* out = filter_compute(n, w, c->w, c->h, ix0, iy0, iw, ih, aa, rs);
    if (!out) return;
    filter_composite(c, out, ix0, iy0, op, clip, self_mask);
    fbuf_free(out);
}

void render_subtree(Canvas* c, const Node* n, Mat world, int aa) {
    if (!c || !n) return;
    RenderState st;
    st.lim = NULL;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    st.filters = 0;
    st.top = NULL;
    st.gen = 0;
    st.fc = NULL;
    for (int i = 0; i < n->nkids; i++)
        render_node(c, n->kids[i], world, 1.0f, NULL, aa, &st);
}

void render_one(Canvas* c, const Node* n, Mat world, int aa) {
    if (!c || !n) return;
    RenderState st;
    st.lim = NULL;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    st.filters = 0;
    st.top = NULL;
    st.gen = 0;
    st.fc = NULL;
    render_node(c, n, world, 1.0f, NULL, aa, &st);
}

void render_children(Canvas* c, const Node* n, Mat world, float opacity,
                     const Mask* clip, int aa) {
    if (!c || !n) return;
    RenderState st;
    st.lim = NULL;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    st.filters = 0;
    st.top = NULL;
    st.gen = 0;
    st.fc = NULL;
    for (int i = 0; i < n->nkids; i++)
        render_node(c, n->kids[i], world, opacity, clip, aa, &st);
}

typedef struct {
    const Canvas* c;
    const Node*   root;
    Mat           tm;
    int           aa;
    const svg2img_limits* lim;
    int           band, nbands;
    unsigned      gen;
    FilterCache*  fc;
    atomic_int    next;
} BandJob;

static void band_worker(void* arg, int worker) {
    BandJob* j = (BandJob*)arg;

    if (worker == 0) svg_alloc_worker(1);
    Canvas v = *j->c;
    v.scratch = NULL;
    v.scratch_cap = v.scratch_used = 0;
    v.scratch_depth = 0;
    for (;;) {
        int b = atomic_fetch_add(&j->next, 1);
        if (b >= j->nbands) break;
        v.wy0 = b * j->band;
        v.wy1 = v.wy0 + j->band < v.h ? v.wy0 + j->band : v.h;
        RenderState st = { j->lim, 0, 0, 0, 0, &v, j->gen, j->fc };
        render_node(&v, j->root, j->tm, 1.0f, NULL, j->aa, &st);
    }
    svg_mfree(v.scratch);
    if (worker == 0) svg_alloc_worker(0);
}

typedef struct {
    unsigned gen;
    int W, H;
    int nfilters;
} ExtentPass;

#define EXT_EMPTY     0
#define EXT_BOUNDED   1
#define EXT_UNBOUNDED 2

static float mat_y_gain(Mat m) {
    return sqrtf(m.m[1] * m.m[1] + m.m[3] * m.m[3]);
}

static void ext_add_rect(Mat w, float x0, float y0, float x1, float y1,
                         float* lo, float* hi) {
    Pt c[4] = { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} };
    for (int i = 0; i < 4; i++) {
        float y = mat_apply(w, c[i]).y;
        if (y < *lo) *lo = y;
        if (y > *hi) *hi = y;
    }
}

static int text_extent(const Node* n, Mat w, float* lo, float* hi) {
    if (n->nchunks <= 0 || !n->chunks)
        return (n->text && *n->text) ? EXT_UNBOUNDED : EXT_EMPTY;
    int any = 0;
    for (int ci = 0; ci < n->nchunks; ci++) {
        const TextChunk* ch = &n->chunks[ci];
        if (!ch->text || !*ch->text || !(ch->fontsize > 0.0f)) continue;
        if (ch->path || ch->refpath || ch->path_ref || ch->ngpos > 0 ||
            ch->nrot > 0 || ch->writing != 0)
            return EXT_UNBOUNDED;
        float fs = ch->fontsize;
        float total = 0.0f;
        for (const char* q = ch->text; *q; q++) {
            float adv = ch->font
                ? font_face_advance(ch->font,
                                    font_face_glyph_index(ch->font, (unsigned char)*q)) * fs
                : fs;
            total += fabsf(adv) + fabsf(ch->letterspacing);
        }
        float pad = 2.0f * fs + fabsf(ch->baseline) * fs +
                    (ch->stroke.type != PAINT_NONE ? fabsf(ch->stroke_width) * 4.0f : 0.0f);
        ext_add_rect(w, ch->x - total - pad, ch->y - pad,
                     ch->x + total + pad, ch->y + pad, lo, hi);
        any = 1;
    }
    return any ? EXT_BOUNDED : EXT_EMPTY;
}

static int ext_walk(Node* n, Mat world, ExtentPass* ep, int depth,
                    float* out_lo, float* out_hi) {
    if (!n) return EXT_EMPTY;
    if (n->ext_gen == ep->gen) {
        n->ext_ok = 0;
        n->ext_fidx = -1;
        return EXT_UNBOUNDED;
    }
    n->ext_gen = ep->gen;
    n->ext_ok = 0;
    n->ext_fidx = -1;
    if (depth > RENDER_MAX_DEPTH) return EXT_UNBOUNDED;
    if (n->hidden_subtree || n->type == NODE_SYMBOL || n->type == NODE_IGNORE ||
        n->type == NODE_PATTERNDEF || n->type == NODE_GRADDEF ||
        n->type == NODE_CLIPDEF || n->type == NODE_MASKDEF ||
        n->type == NODE_MARKERDEF || n->type == NODE_FILTERDEF)
        return EXT_EMPTY;

    Mat w = mat_mul(world, n->tm);
    float lo = 1e30f, hi = -1e30f;
    int r = EXT_EMPTY;

    if (n->filter) {

        float lbb[4];
        int has_bb = node_bbox_world(n, mat_identity(), lbb);
        int ix0 = 0, iy0 = 0, iw = 0, ih = 0;
        if (filter_region(n->filter, w, lbb, has_bb, ep->W, ep->H,
                          &ix0, &iy0, &iw, &ih)) {
            lo = (float)iy0;
            hi = (float)(iy0 + ih);
            r = EXT_BOUNDED;
            n->ext_fidx = ep->nfilters++;
        }
    } else if (n->type == NODE_SHAPE) {
        if (!n->visible) {
            r = EXT_EMPTY;
        } else if (n->marker_start || n->marker_mid || n->marker_end) {
            r = EXT_UNBOUNDED;
        } else if (n->path && n->path->n > 0) {
            float bb[4];
            path_bbox_transformed(n->path, w, bb);
            lo = bb[1];
            hi = bb[3];
            if (n->stroke.type != PAINT_NONE && n->sw > 0.0f) {

                float ml = n->miterlimit > 1.0f ? n->miterlimit : 1.0f;
                float reach = n->sw * 0.5f * (1.0f + 2.0f * ml) * mat_y_gain(w);
                lo -= reach;
                hi += reach;
            }
            r = EXT_BOUNDED;
        }
    } else if (n->type == NODE_TEXT) {
        r = n->visible ? text_extent(n, w, &lo, &hi) : EXT_EMPTY;
    } else if (n->type == NODE_IMAGE) {

        if (n->img_w > 0.0f && n->img_h > 0.0f) {
            ext_add_rect(w, n->img_x, n->img_y, n->img_x + n->img_w,
                         n->img_y + n->img_h, &lo, &hi);
            r = EXT_BOUNDED;
        }
    } else if (n->type == NODE_GROUP || n->type == NODE_TRANSPARENT) {
        for (int i = 0; i < n->nkids; i++) {
            float kl, kh;
            int kr = ext_walk(n->kids[i], w, ep, depth + 1, &kl, &kh);
            if (kr == EXT_UNBOUNDED) r = EXT_UNBOUNDED;
            else if (kr == EXT_BOUNDED && r != EXT_UNBOUNDED) {
                if (kl < lo) lo = kl;
                if (kh > hi) hi = kh;
                r = EXT_BOUNDED;
            }
        }
    } else {
        r = EXT_UNBOUNDED;
    }

    if (r == EXT_BOUNDED) {
        if (!isfinite(lo) || !isfinite(hi)) return EXT_UNBOUNDED;
        lo = floorf(lo) - 2.0f;
        hi = ceilf(hi) + 2.0f;
        *out_lo = lo;
        *out_hi = hi;
    } else if (r == EXT_EMPTY) {
        lo = 1e30f;
        hi = -1e30f;
    } else {
        return EXT_UNBOUNDED;
    }
    n->ext_y0 = lo;
    n->ext_y1 = hi;
    n->ext_ok = 1;
    return r;
}

static atomic_uint g_ext_gen;

void render_document(Canvas* c, Node* root, Mat view_tm,
                     const svg2img_options* opts, const svg2img_limits* lim) {
    int aa = (opts && opts->aa > 0) ? opts->aa : 4;
    if (aa < 1) aa = 1;
    if (aa > 8) aa = 8;
    int threads = opts ? opts->threads : 1;
    if (threads > 1 && c->h >= 64) {

        int nb = threads * 3;
        if (nb > c->h / 16) nb = c->h / 16;
        BandJob j;
        j.c = c; j.root = root; j.tm = view_tm; j.aa = aa; j.lim = lim;
        j.band = (c->h + nb - 1) / nb;
        j.nbands = (c->h + j.band - 1) / j.band;
        ExtentPass ep;
        ep.gen = atomic_fetch_add(&g_ext_gen, 1u) + 1u;
        if (ep.gen == 0) ep.gen = atomic_fetch_add(&g_ext_gen, 1u) + 1u;
        ep.W = c->w;
        ep.H = c->h;
        ep.nfilters = 0;
        float lo, hi;
        ext_walk(root, view_tm, &ep, 0, &lo, &hi);
        j.gen = ep.gen;
        FilterCache fc;
        fc.n = ep.nfilters;
        fc.band = j.band;
        fc.e = fc.n ? (FilterEntry*)svg_calloc((size_t)fc.n * sizeof(FilterEntry))
                    : NULL;
        if (fc.n && !fc.e) fc.n = 0;
        for (int i = 0; i < fc.n; i++) {
            atomic_init(&fc.e[i].state, 0);
            atomic_init(&fc.e[i].refs, 0);
        }
        j.fc = &fc;
        atomic_init(&j.next, 0);
        svg_parallel_run(threads < j.nbands ? threads : j.nbands,
                         band_worker, &j);

        for (int i = 0; i < fc.n; i++) fbuf_free(fc.e[i].out);
        svg_mfree(fc.e);
        return;
    }
    RenderState st;
    st.lim = lim;
    st.layers = 0;
    st.clips = 0;
    st.masks = 0;
    st.filters = 0;
    st.top = NULL;
    st.gen = 0;
    st.fc = NULL;
    render_node(c, root, view_tm, 1.0f, NULL, aa, &st);
}
