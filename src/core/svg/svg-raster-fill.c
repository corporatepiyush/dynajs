#include "svg-raster.h"

typedef struct { float x_ymin, ymin, ymax, invslope; int dir; } Edge;
typedef struct { float x; int dir; int e; } Cross;

static int sort_cross_nearly(Cross* a, int n, long budget) {
    long moves = 0;
    for (int i = 1; i < n; i++) {
        Cross key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].x > key.x) {
            a[j + 1] = a[j];
            j--;
            if (++moves > budget) { a[j + 1] = key; return 0; }
        }
        a[j + 1] = key;
    }
    return 1;
}

static void sort_cross(Cross* a, int n) {
    if (n < 2) return;
    if (n >= 32 && sort_cross_nearly(a, n, 4L * n + 64)) return;
    if (n < 32) {
        sort_cross_nearly(a, n, (long)n * n);
        return;
    }

    int lo[64], hi[64], sp = 0;
    lo[sp] = 0; hi[sp] = n - 1; sp++;
    while (sp > 0) {
        sp--;
        int l = lo[sp], h = hi[sp];
        while (h - l >= 16) {
            float pivot = a[l + (h - l) / 2].x;
            int i = l - 1, j = h + 1;
            for (;;) {
                do i++; while (a[i].x < pivot);
                do j--; while (a[j].x > pivot);
                if (i >= j) break;
                Cross t = a[i]; a[i] = a[j]; a[j] = t;
            }
            if (j - l < h - j) {
                lo[sp] = j + 1; hi[sp] = h; sp++;
                h = j;
            } else {
                lo[sp] = l; hi[sp] = j; sp++;
                l = j + 1;
            }
        }
        sort_cross_nearly(a + l, h - l + 1, (long)(h - l + 1) * (h - l + 1));
    }
}

static int count_edges(const Path* path) {
    int n = 0;
    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];
        if (sp->n < 2 || !sp->p) continue;
        n += sp->n;
    }
    return n;
}

static int build_edges_into(const Path* path, Edge* e, int cap) {
    int k = 0;
    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];

        if (sp->n < 2 || !sp->p) continue;
        const int nseg = sp->n;
        const Pt* pts = sp->p;
        for (int i = 0; i < nseg; i++) {
            Pt a = pts[i];
            Pt b = pts[(i + 1) % sp->n];
            float dy = b.y - a.y;
            if (fabsf(dy) < 1e-12f) continue;
            if (!isfinite(a.x) || !isfinite(a.y) ||
                !isfinite(b.x) || !isfinite(b.y)) continue;
            if (k >= cap) break;
            Edge* ed = &e[k++];
            ed->ymin = a.y < b.y ? a.y : b.y;
            ed->ymax = a.y < b.y ? b.y : a.y;
            ed->x_ymin = a.y < b.y ? a.x : b.x;
            ed->invslope = (b.x - a.x) / dy;
            ed->dir = (dy > 0.0f) ? 1 : -1;        }
    }
    return k;
}

static void add_run(float* cov, int x0, int s0, int s1, int aa) {
    if (s1 <= s0) return;
    float* row = cov - x0;
    const float k = 1.0f / (float)(aa * aa);
    const float full = (float)aa * k;
    int first = s0 / aa;
    int last = (s1 - 1) / aa;
    if (first == last) {
        row[first] += (float)(s1 - s0) * k;
        return;
    }
    row[first] += (float)((first + 1) * aa - s0) * k;
    for (int p = first + 1; p < last; p++) row[p] += full;
    row[last] += (float)(s1 - last * aa) * k;
}

void fill_path(Canvas* c, const Path* path, int rule, const Paint* paint,
                      float opacity, const Mask* clip, const float bounds[4],
                      const float grad_bbox[4], int aa, Mat world) {
    if (!path || path->n == 0 || opacity <= 0.0f) return;
    if (paint->type != PAINT_COLOR && paint->type != PAINT_LINEAR &&
        paint->type != PAINT_RADIAL && paint->type != PAINT_PATTERN) return;

    int x0 = svg_f2i(floorf(bounds[0]), -1, c->w + 1, 0);
    int y0 = svg_f2i(floorf(bounds[1]), -1, c->h + 1, 0);
    int x1 = svg_f2i(ceilf(bounds[2]), -1, c->w + 1, 0);
    int y1 = svg_f2i(ceilf(bounds[3]), -1, c->h + 1, 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (y0 < c->wy0) y0 = c->wy0;
    if (y1 > c->wy1) y1 = c->wy1;
    if (x0 >= x1 || y0 >= y1) return;

    Grad grad;
    int has_grad = 0;
    if (paint->type == PAINT_LINEAR || paint->type == PAINT_RADIAL) {
        grad_build(&grad, paint, grad_bbox ? grad_bbox : bounds, world);
        has_grad = 1;
    }
    Pattern pat;
    int has_pat = 0;
    if (paint->type == PAINT_PATTERN && paint->pat)
        has_pat = pattern_build(&pat, paint->pat, bounds, world, aa);
    int has_shader = has_grad || has_pat;
    Color solid = paint->color;

    const int direct = (aa == 1 && !has_shader && !clip &&
                        solid.a * opacity >= 0.999f);

    const int row_len = x1 - x0;

    const int sy0 = y0 * aa;
    const int sy1 = y1 * aa;
    const int bucket_n = sy1 - sy0;
    int ne = count_edges(path);
    if (ne <= 0) {
        if (has_pat) pattern_free(&pat);
        return;
    }

    size_t sz_head = (size_t)(bucket_n + 1) * sizeof(int);
    size_t sz_n    = (size_t)ne * sizeof(int);
    size_t sz_cr   = (size_t)ne * sizeof(Cross);
    size_t sz_row  = (size_t)row_len * sizeof(float);
    size_t edge_bytes = (size_t)ne * sizeof(Edge);
    size_t sz_shade = has_shader ? (size_t)row_len * sizeof(Color) : 0;
    size_t arena_bytes = sz_head + sz_n * 2 + sz_cr + sz_row * 2 + sz_n + sz_shade;
    size_t scratch_mark = canvas_scratch_mark(c);
    int arena_owned = 0;
    unsigned char* block = (unsigned char*)canvas_scratch_alloc(
        c, edge_bytes + arena_bytes);
    if (!block) {
        block = (unsigned char*)svg_malloc(edge_bytes + arena_bytes);
        arena_owned = 1;
    }
    if (!block) {
        if (has_pat) pattern_free(&pat);
        return;
    }
    Edge* edges = (Edge*)block;
    unsigned char* arena = block + edge_bytes;
    int nk = build_edges_into(path, edges, ne);
    if (nk == 0) {
        if (arena_owned) svg_mfree(block);
        canvas_scratch_reset(c, scratch_mark);
        if (has_pat) pattern_free(&pat);
        return;
    }
    ne = nk;
    c->scratch_depth++;

    int* restrict       head      = (int*)arena;
    int* restrict       nxt       = (int*)(arena + sz_head);
    int* restrict       active    = (int*)(arena + sz_head + sz_n);
    Cross* restrict     cross     = (Cross*)(arena + sz_head + sz_n * 2);
    float* restrict     cov       = (float*)(arena + sz_head + sz_n * 2 + sz_cr);
    float* restrict     alpha_row = (float*)(arena + sz_head + sz_n * 2 + sz_cr + sz_row);
    int* restrict       ylast     = (int*)(arena + sz_head + sz_n * 2 + sz_cr + sz_row * 2);

    Color* restrict     shade     = (Color*)(arena + sz_head + sz_n * 2 + sz_cr + sz_row * 2 + sz_n);

    for (int i = 0; i <= bucket_n; i++) head[i] = -1;
    for (int i = 0; i < ne; i++) {

        double vf = ceil((double)edges[i].ymin * (double)aa - 0.5);
        double vl = ceil((double)edges[i].ymax * (double)aa - 0.5) - 1.0;

        if (vf > (double)(sy1 - 1)) continue;
        int f = (vf >= (double)sy0)
                    ? ((vf > (double)(sy1 - 1)) ? sy1 - 1 : (int)vf)
                    : sy0;
        int l = (vl >= (double)sy0)
                    ? ((vl > (double)(sy1 - 1)) ? sy1 - 1 : (int)vl)
                    : sy0 - 1;
        if (l < f) continue;
        ylast[i] = l;
        nxt[i] = head[f - sy0];
        head[f - sy0] = i;
    }

    int na = 0;
    const float invaa = 1.0f / (float)aa;

    for (int py = y0; py < y1; py++) {
        if (!direct) {

            memset(cov, 0, (size_t)row_len * sizeof(float));
        }
        int touched_lo = x1, touched_hi = x0;

        for (int sub = 0; sub < aa; sub++) {
            int sy = py * aa + sub;
            for (int e = head[sy - sy0]; e != -1; e = nxt[e]) active[na++] = e;
            {
                int w = 0;
                for (int i = 0; i < na; i++)
                    if (ylast[active[i]] >= sy) active[w++] = active[i];
                na = w;
            }
            if (na == 0) continue;

            const float y = ((float)sy + 0.5f) * invaa;

            for (int i = 0; i < na; i++) {
                const Edge* ed = &edges[active[i]];
                cross[i].x = ed->x_ymin + (y - ed->ymin) * ed->invslope;
                cross[i].dir = ed->dir;
                cross[i].e = active[i];
            }
            int nc = na;
            if (nc < 2) continue;
            sort_cross(cross, nc);

            for (int i = 0; i < nc; i++) active[i] = cross[i].e;

            int sx_lo = x0 * aa, sx_hi = x1 * aa;
            int wind = 0;
            int run_start = sx_lo;
            int i = 0;
            while (i < nc) {
                float cx0 = cross[i].x;
                int sx = svg_f2i_clamped(floorf(cx0 * (float)aa),
                                         sx_lo - 1, sx_hi + 1);
                if (sx < run_start) sx = run_start;
                if (sx > sx_hi) sx = sx_hi;
                if (sx > run_start) {
                    int inside = (rule == 0) ? (wind != 0) : ((wind & 1) != 0);
                    if (inside) {
                        int p0 = run_start / aa;
                        int p1 = (sx + aa - 1) / aa;
                        if (p0 < x0) p0 = x0;
                        if (p1 > x1) p1 = x1;
                        if (p0 < touched_lo) touched_lo = p0;
                        if (p1 > touched_hi) touched_hi = p1;
                        if (direct) {
                            float* d = canvas_at(c, p0, py);
                            for (int p = 0, cnt = p1 - p0; p < cnt; p++) {
                                d[p * 4 + 0] = solid.r;
                                d[p * 4 + 1] = solid.g;
                                d[p * 4 + 2] = solid.b;
                                d[p * 4 + 3] = 1.0f;
                            }
                        } else {
                            add_run(cov, x0, run_start, sx, aa);
                        }
                    }
                    run_start = sx;
                }
                do { wind += cross[i].dir; i++; }
                while (i < nc && cross[i].x == cx0);
            }

            if (run_start < sx_hi) {
                int inside = (rule == 0) ? (wind != 0) : ((wind & 1) != 0);
                if (inside) {
                    int p0 = run_start / aa;
                    int p1 = (sx_hi + aa - 1) / aa;
                    if (p0 < x0) p0 = x0;
                    if (p1 > x1) p1 = x1;
                    if (p0 < touched_lo) touched_lo = p0;
                    if (p1 > touched_hi) touched_hi = p1;
                    if (direct) {
                        float* d = canvas_at(c, p0, py);
                        for (int p = 0, cnt = p1 - p0; p < cnt; p++) {
                            d[p * 4 + 0] = solid.r;
                            d[p * 4 + 1] = solid.g;
                            d[p * 4 + 2] = solid.b;
                            d[p * 4 + 3] = 1.0f;
                        }
                    } else {
                        add_run(cov, x0, run_start, sx_hi, aa);
                    }
                }
            }
        }

        if (direct) continue;

        int b_lo = touched_hi > touched_lo ? touched_lo : x0;
        int b_hi = touched_hi > touched_lo ? touched_hi : x0;
        int b_len = b_hi - b_lo;

        if (!has_shader && !clip && b_len > 0) {
            float k = solid.a * opacity;
            if (k >= 0.999f) {
                blend_row_const(c, b_lo, py, b_len, solid, cov + (b_lo - x0));
            } else {
                const float* src = cov + (b_lo - x0);
                for (int p = 0; p < b_len; p++) alpha_row[p] = src[p] * k;
                blend_row_const(c, b_lo, py, b_len, solid, alpha_row);
            }
        } else if (b_len > 0 && has_shader && !clip) {

            if (has_pat) pattern_eval_row(&pat, b_lo, b_len, (float)py + 0.5f, shade);
            else         grad_eval_row(&grad, b_lo, b_len, (float)py + 0.5f, shade);
            blend_row_var(c, b_lo, py, b_len, shade, cov + (b_lo - x0), opacity);
        } else if (b_len > 0 && !has_shader) {

            int mx0 = 0, mx1 = 0;
            const float* mrow = mask_row(clip, py, &mx0, &mx1);
            if (mrow) {
                int lo = b_lo > mx0 ? b_lo : mx0;
                int hi = b_hi < mx1 ? b_hi : mx1;
                if (lo < hi) {
                    float k = solid.a * opacity;
                    const float* src = cov + (lo - x0);
                    const float* mk = mrow + (lo - mx0);
                    int n = hi - lo;
                    for (int p = 0; p < n; p++)
                        alpha_row[p] = src[p] * mk[p] * k;
                    blend_row_const(c, lo, py, n, solid, alpha_row);
                }
            }
        } else if (b_len > 0) {

            int mx0 = 0, mx1 = 0;
            const float* mrow = mask_row(clip, py, &mx0, &mx1);
            int clo = b_lo > mx0 ? b_lo : mx0;
            int chi = b_hi < mx1 ? b_hi : mx1;
            if (mrow && clo < chi) {
                int n = chi - clo;
                for (int p = 0; p < n; p++)
                    alpha_row[p] = cov[clo - x0 + p] * mrow[clo - mx0 + p];
                if (has_pat) pattern_eval_row(&pat, clo, n, (float)py + 0.5f, shade);
                else         grad_eval_row(&grad, clo, n, (float)py + 0.5f, shade);
                blend_row_var(c, clo, py, n, shade, alpha_row, opacity);
            }
        }

    }

    if (arena_owned) svg_mfree(block);
    canvas_scratch_reset(c, scratch_mark);
    c->scratch_depth--;
    if (has_pat) pattern_free(&pat);
}
