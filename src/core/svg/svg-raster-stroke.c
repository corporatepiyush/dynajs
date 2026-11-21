#include "svg-raster.h"
#include <math.h>
#include <limits.h>

static SubPath* path_add_sub(Path* p) {

    if (p->n == p->cap) {
        if (p->cap > INT_MAX / 2) return NULL;
        int nc = p->cap ? p->cap * 2 : 8;
        SubPath* ns;
        if (p->s && p->s == p->s_inline) {

            ns = (SubPath*)svg_malloc((size_t)nc * sizeof(SubPath));
            if (ns) memcpy(ns, p->s, (size_t)p->n * sizeof(SubPath));
        } else {
            ns = (SubPath*)svg_realloc(p->s, (size_t)nc * sizeof(SubPath));
        }
        if (!ns) return NULL;
        p->s = ns;
        p->cap = nc;
    }
    SubPath* sp = &p->s[p->n++];
    sp->p = NULL; sp->n = 0; sp->closed = 0;
    return sp;
}

static Pt dir_of(Pt v) {
    float l = sqrtf(v.x * v.x + v.y * v.y);
    if (l < 1e-9f) return (Pt){1, 0};
    Pt r = {v.x / l, v.y / l};
    return r;
}

static void add_polygon_arena(Path* path, Pt* arena, size_t* arena_used,
                              size_t arena_cap, const Pt* pts, int n) {
    if (n < 3) return;
    if (*arena_used + (size_t)n > arena_cap) return;
    SubPath* sp = path_add_sub(path);
    if (!sp) return;
    Pt* dst = arena + *arena_used;
    *arena_used += (size_t)n;
    sp->p = dst;
    sp->n = n;
    sp->closed = 1;
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0.0f) {
        for (int i = 0; i < n; i++) dst[i] = pts[n - 1 - i];
    } else {
        memcpy(dst, pts, (size_t)n * sizeof(Pt));
    }
}

static void emit_disk(Path* path, Pt c, float r, int seg,
                      Pt* arena, size_t* used, size_t arena_cap) {
    if (*used + (size_t)seg > arena_cap) return;
    Pt* pts = arena + *used;
    *used += (size_t)seg;
    for (int i = 0; i < seg; i++) {
        float a = 2.0f * SVG_PI * (float)i / (float)seg;
        pts[i].x = c.x + r * cosf(a);
        pts[i].y = c.y + r * sinf(a);
    }
    add_polygon_arena(path, arena, used, arena_cap, pts, seg);
}

static void emit_join(Path* path, Pt v, Pt d_in, Pt d_out, float h,
                      int join, float miterlimit, float w,
                      Pt* arena, size_t* used, size_t arena_cap) {
    Pt n_in = {-d_in.y, d_in.x};
    Pt n_out = {-d_out.y, d_out.x};
    Pt L_in = {v.x + n_in.x * h, v.y + n_in.y * h};
    Pt L_out = {v.x + n_out.x * h, v.y + n_out.y * h};
    Pt R_in = {v.x - n_in.x * h, v.y - n_in.y * h};
    Pt R_out = {v.x - n_out.x * h, v.y - n_out.y * h};
    if (join == 1) {
        emit_disk(path, v, h, 16, arena, used, arena_cap);
        return;
    }
    for (int side = 0; side < 2; side++) {
        Pt A = (side == 0) ? L_in : R_in;
        Pt B = (side == 0) ? L_out : R_out;
        float denom = d_in.x * d_out.y - d_in.y * d_out.x;
        Pt tri[3];
        if (join == 0 && fabsf(denom) > 1e-9f) {

            float t = ((B.x - A.x) * d_out.y - (B.y - A.y) * d_out.x) / denom;
            Pt m = {A.x + t * d_in.x, A.y + t * d_in.y};
            if (fabsf(t) <= miterlimit * w) {
                tri[0] = A; tri[1] = m; tri[2] = B;
                add_polygon_arena(path, arena, used, arena_cap, tri, 3);
                continue;
            }
        }
        tri[0] = A; tri[1] = v; tri[2] = B;
        add_polygon_arena(path, arena, used, arena_cap, tri, 3);
    }
}

static void emit_cap(Path* path, Pt p, Pt d, float h, int cap,
                     Pt* arena, size_t* used, size_t arena_cap) {
    if (cap == 0) return;
    Pt n = {-d.y, d.x};
    if (cap == 2) {
        Pt q[4] = {
            {p.x + n.x * h, p.y + n.y * h},
            {p.x + d.x * h + n.x * h, p.y + d.y * h + n.y * h},
            {p.x + d.x * h - n.x * h, p.y + d.y * h - n.y * h},
            {p.x - n.x * h, p.y - n.y * h}
        };
        add_polygon_arena(path, arena, used, arena_cap, q, 4);
    } else {
        emit_disk(path, p, h, 16, arena, used, arena_cap);
    }
}

static void stroke_polyline(Path* out, const Pt* pts, int n, int closed,
                            float w, int cap, int join, float miterlimit,
                            Pt* arena, size_t* used, size_t arena_cap) {
    if (n < 2) return;
    float h = w * 0.5f;

    const float eps = 1e-4f;
    size_t smark = svg_scratch_mark();
    Pt* clean = (Pt*)svg_scratch_alloc((size_t)n * sizeof(Pt));
    if (!clean) { svg_scratch_release(smark); return; }
    int cn = 0;
    for (int i = 0; i < n; i++) {
        if (cn > 0) {
            float dx = pts[i].x - clean[cn - 1].x;
            float dy = pts[i].y - clean[cn - 1].y;
            if (dx * dx + dy * dy <= eps * eps) continue;
        }
        clean[cn++] = pts[i];
    }
    if (closed && cn > 1) {
        float dx = clean[0].x - clean[cn - 1].x;
        float dy = clean[0].y - clean[cn - 1].y;
        if (dx * dx + dy * dy <= eps * eps) cn--;
    }
    if (cn < 2) { svg_scratch_release(smark); return; }
    pts = clean;
    n = cn;
    closed = closed && (n >= 3);

    if (closed) {
        if (*used + (size_t)(2 * n) > arena_cap) { svg_scratch_release(smark); return; }
        Pt* dst = arena + *used;
        *used += (size_t)(2 * n);

        for (int i = 0; i < n; i++) {
            Pt p_prev = pts[(i - 1 + n) % n];
            Pt p = pts[i];
            Pt p_next = pts[(i + 1) % n];
            Pt d1 = dir_of((Pt){p.x - p_prev.x, p.y - p_prev.y});
            Pt d2 = dir_of((Pt){p_next.x - p.x, p_next.y - p.y});
            Pt n1 = {-d1.y, d1.x};
            Pt n2 = {-d2.y, d2.x};
            Pt ns = {n1.x + n2.x, n1.y + n2.y};
            float len = sqrtf(ns.x * ns.x + ns.y * ns.y);
            if (len < 1e-6f) { ns = n2; len = 1.0f; }
            ns.x /= len;
            ns.y /= len;
            float cosh = ns.x * n2.x + ns.y * n2.y;
            if (cosh < 0.25f) cosh = 0.25f;
            float scale = h / cosh;
            dst[i].x = p.x + ns.x * scale;
            dst[i].y = p.y + ns.y * scale;
            dst[n + i].x = p.x - ns.x * scale;
            dst[n + i].y = p.y - ns.y * scale;
        }

        SubPath* outer = path_add_sub(out);
        if (!outer) { svg_scratch_release(smark); return; }
        outer->p = dst;
        outer->n = n;
        outer->closed = 1;

        SubPath* inner = path_add_sub(out);
        if (!inner) { svg_scratch_release(smark); return; }
        inner->p = dst + n;
        inner->n = n;
        inner->closed = 1;
        for (int i = 0; i < n / 2; i++) {
            Pt t = dst[n + i];
            dst[n + i] = dst[n + n - 1 - i];
            dst[n + n - 1 - i] = t;
        }
        svg_scratch_release(smark);
        return;
    }

    int nseg = n - 1;
    for (int i = 0; i < nseg; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        Pt d = dir_of((Pt){b.x - a.x, b.y - a.y});
        Pt nn = {-d.y, d.x};
        Pt q[4] = {
            {a.x + nn.x * h, a.y + nn.y * h},
            {b.x + nn.x * h, b.y + nn.y * h},
            {b.x - nn.x * h, b.y - nn.y * h},
            {a.x - nn.x * h, a.y - nn.y * h}
        };
        add_polygon_arena(out, arena, used, arena_cap, q, 4);
    }

    for (int i = 1; i < n - 1; i++) {
        Pt d_in = dir_of((Pt){pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y});
        Pt d_out = dir_of((Pt){pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y});
        emit_join(out, pts[i], d_in, d_out, h, join, miterlimit, w,
                  arena, used, arena_cap);
    }
    Pt d0 = dir_of((Pt){pts[1].x - pts[0].x, pts[1].y - pts[0].y});
    emit_cap(out, pts[0], (Pt){-d0.x, -d0.y}, h, cap, arena, used, arena_cap);
    Pt d1 = dir_of((Pt){pts[n - 1].x - pts[n - 2].x, pts[n - 1].y - pts[n - 2].y});
    emit_cap(out, pts[n - 1], d1, h, cap, arena, used, arena_cap);

    svg_scratch_release(smark);
}

static Pt point_at_dist(const Pt* pts, int n, int closed, float d,
                        int* seg_out, float* acc_out) {
    int nseg = closed ? n : n - 1;
    float acc = 0.0f;
    for (int i = 0; i < nseg; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        float len = hypotf(b.x - a.x, b.y - a.y);
        if (acc + len >= d || i == nseg - 1) {
            float t = len > 1e-9f ? (d - acc) / len : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            if (seg_out) *seg_out = i;
            if (acc_out) *acc_out = acc;
            Pt p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            return p;
        }
        acc += len;
    }
    if (seg_out) *seg_out = 0;
    if (acc_out) *acc_out = 0.0f;
    return pts[0];
}

static void extract_range(const Pt* pts, int n, int closed, float d0, float d1,
                          Pt** out, int* nout, int* capp) {
    *nout = 0;
    int seg0 = 0;
    float acc0 = 0.0f;
    Pt p0 = point_at_dist(pts, n, closed, d0, &seg0, &acc0);
    #define PSH(P) do { \
        if (*nout == *capp) { \
            int nc_ = *capp > INT_MAX / 2 ? -1 : (*capp ? *capp * 2 : 8); \
            Pt* np_ = nc_ > 0 ? (Pt*)svg_realloc(*out, (size_t)nc_ * sizeof(Pt)) : NULL; \
            if (np_) { *out = np_; *capp = nc_; } \
        } \
        if (*nout < *capp) (*out)[(*nout)++] = (P); \
    } while (0)
    PSH(p0);
    int nseg = closed ? n : n - 1;
    float acc = acc0;
    for (int i = seg0; i < nseg; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        float len = hypotf(b.x - a.x, b.y - a.y);
        float seg_end = acc + len;
        if (seg_end >= d1) {
            float t = len > 1e-9f ? (d1 - acc) / len : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            Pt p = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            PSH(p);
            break;
        }
        PSH(b);
        acc = seg_end;
    }
    #undef PSH
}

static void emit_dashes(Path* out, const Pt* pts, int n, int closed,
                        float w, int cap, int join, float miterlimit,
                        const float* dash, int ndash, float dashoff,
                        Pt* arena, size_t* used, size_t arena_cap) {
    if (ndash <= 0 || !dash) return;
    int nseg = closed ? n : n - 1;
    if (nseg < 1) return;
    float total = 0.0f;
    for (int i = 0; i < nseg; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        total += hypotf(b.x - a.x, b.y - a.y);
    }
    if (total <= 1e-9f) return;

    int en = ndash > 256 ? 256 : ndash;

    float owned[512];
    float cum[513];
    const float* eff = dash;
    if (ndash % 2 == 1) {

        en = en * 2;
        for (int i = 0; i < en; i++) owned[i] = dash[i % ndash];
        eff = owned;
    }
    cum[0] = 0.0f;
    float L = 0.0f;
    for (int i = 0; i < en; i++) { L += eff[i]; cum[i + 1] = L; }
    if (L <= 1e-9f) return;
    Pt* sub = NULL;
    int subcap = 0;

    float start = fmodf(dashoff, L);
    if (start < 0.0f) start += L;
    int idx = 0;
    for (int i = 0; i < en; i++) {
        if (start >= cum[i] && start < cum[i + 1]) { idx = i; break; }
    }
    float d = 0.0f;
    float pp = start;
    int guard = 0;
    while (d < total - 1e-6f && guard++ < 100000) {
        float boundary = cum[idx + 1];
        float step = boundary - pp;
        if (step <= 1e-9f) {
            idx++;
            if (idx >= en) { idx = 0; pp -= L; }
            continue;
        }
        if (d + step > total) step = total - d;
        if ((idx % 2) == 0) {
            int sn = 0;
            extract_range(pts, n, closed, d, d + step, &sub, &sn, &subcap);
            if (sn >= 2) stroke_polyline(out, sub, sn, 0, w, cap, join, miterlimit,
                                         arena, used, arena_cap);
        }
        d += step;
        pp += step;
        while (pp >= boundary - 1e-9f) {
            idx++;
            if (idx >= en) { idx = 0; pp -= L; }
            boundary = cum[idx + 1];
        }
    }
    svg_mfree(sub);
}

Path* build_stroke(const Path* path, float w, int cap, int join,
                          float miterlimit, const float* dash, int ndash,
                          float dashoff) {
    if (w <= 0.0f) {
        Path* empty = (Path*)svg_malloc(sizeof(Path));
        if (empty) memset(empty, 0, sizeof(*empty));
        return empty;
    }

    size_t total_pts = 0, total_subs = 0;
    const size_t PT_MAX = ((size_t)1 << 28) + 64;
    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];
        if (sp->n < 2) continue;
        size_t nseg = sp->closed ? (size_t)sp->n : (size_t)sp->n - 1;
        size_t want = nseg * 4 + ((size_t)sp->n + 2) * 16 + 64;
        total_pts += want < PT_MAX ? want : PT_MAX;

        total_subs += nseg + (size_t)sp->n * 2 + 4;
        if (total_pts > PT_MAX) { total_pts = PT_MAX; break; }
    }
    if (total_pts < 64) total_pts = 64;
    if (total_subs > total_pts / 3) total_subs = total_pts / 3;

    Path* out = (Path*)svg_malloc(sizeof(Path) + total_subs * sizeof(SubPath) +
                                  total_pts * sizeof(Pt));
    if (!out) return NULL;
    memset(out, 0, sizeof(*out));
    SubPath* subs = (SubPath*)(void*)(out + 1);
    Pt* arena = (Pt*)(void*)(subs + total_subs);
    out->s = out->s_inline = subs;
    out->cap = (int)total_subs;
    out->shared_points = PATH_ONE_BLOCK;
    size_t used = 0;

    for (int s = 0; s < path->n; s++) {
        const SubPath* sp = &path->s[s];
        if (sp->n < 2) continue;
        if (ndash > 0 && dash) {
            emit_dashes(out, sp->p, sp->n, sp->closed, w, cap, join, miterlimit,
                        dash, ndash, dashoff, arena, &used, total_pts);
        } else {
            stroke_polyline(out, sp->p, sp->n, sp->closed, w, cap, join, miterlimit,
                            arena, &used, total_pts);
        }
    }

    return out;
}
