#include "svg-int.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>

typedef struct { int type; Pt p0, c1, c2, p3; } Seg;

typedef struct { int off, n; int closed; } RawSub;
typedef struct { Seg* p; int n, cap; int heap; } SegPool;

static int grow_buf(void** p, int* cap, int* heap, int need, size_t esz) {
    if (need <= *cap) return 1;

    if (*cap > INT_MAX / 2) return 0;
    int nc = *cap * 2;
    if (nc < need) nc = need;
    void* np;
    if (*heap) {
        np = svg_realloc(*p, (size_t)nc * esz);
    } else {
        np = svg_malloc((size_t)nc * esz);
        if (np && *cap) memcpy(np, *p, (size_t)*cap * esz);
    }
    if (!np) return 0;
    *p = np; *cap = nc; *heap = 1;
    return 1;
}

static int raw_add(SegPool* pool, RawSub* r, Seg s) {
    if (pool->n == pool->cap &&
        !grow_buf((void**)&pool->p, &pool->cap, &pool->heap, pool->n + 1, sizeof(Seg)))
        return 0;
    pool->p[pool->n++] = s;
    r->n++;
    return 1;
}

static int read_num(const char* s, size_t len, size_t* i, float* out) {
    while (*i < len && (isspace((unsigned char)s[*i]) || s[*i] == ',')) (*i)++;
    if (*i >= len) return 0;
    char* end = NULL;
    float v = strtof(s + *i, &end);
    if (end == s + *i) return 0;
    *i = (size_t)(end - s);
    *out = v;
    return 1;
}

static int read_flag(const char* s, size_t len, size_t* i, int* out) {
    while (*i < len && (isspace((unsigned char)s[*i]) || s[*i] == ',')) (*i)++;
    if (*i >= len) return 0;
    if (s[*i] != '0' && s[*i] != '1') return 0;
    *out = s[*i] - '0';
    (*i)++;
    return 1;
}

static Pt midpt(Pt a, Pt b) { Pt r = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f }; return r; }

static void append_arc(SegPool* pool, RawSub* r, Pt p1, Pt p2, double rx, double ry,
                       double rot, int laf, int sf) {

    if (!isfinite(rx) || !isfinite(ry) || !isfinite(rot) ||
        !isfinite(p1.x) || !isfinite(p1.y) || !isfinite(p2.x) || !isfinite(p2.y) ||
        rx == 0.0 || ry == 0.0 || (p1.x == p2.x && p1.y == p2.y)) {
        Seg s; s.type = 0; s.p0 = p1;
        s.p3 = (isfinite(p2.x) && isfinite(p2.y)) ? p2 : p1;
        s.c1 = s.p0; s.c2 = s.p3;
        raw_add(pool, r, s);
        return;
    }
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;

    double phi = rot * (double)SVG_PI / 180.0;
    double cosp = cos(phi), sinp = sin(phi);

    double x1p =  cosp * (double)(p1.x - p2.x) / 2.0 + sinp * (double)(p1.y - p2.y) / 2.0;
    double y1p = -sinp * (double)(p1.x - p2.x) / 2.0 + cosp * (double)(p1.y - p2.y) / 2.0;

    double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        double sq = sqrt(lambda);
        rx *= sq; ry *= sq;
    }

    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;

    if (num < 0.0) num = 0.0;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double coef = (laf != sf ? 1.0 : -1.0) * sqrt(num / (den > 0 ? den : 1e-30));
    double cxp = coef * (rx * y1p / ry);
    double cyp = coef * (-ry * x1p / rx);
    double cx  = cosp * cxp - sinp * cyp + (double)(p1.x + p2.x) / 2.0;
    double cy  = sinp * cxp + cosp * cyp + (double)(p1.y + p2.y) / 2.0;

    double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

    double theta1 = atan2(uy, ux);
    double dtheta = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sf && dtheta > 0.0) dtheta -= 2.0 * (double)SVG_PI;
    else if (sf && dtheta < 0.0) dtheta += 2.0 * (double)SVG_PI;

    if (!isfinite(rx) || !isfinite(ry) || !isfinite(rot) ||
        !isfinite(p1.x) || !isfinite(p1.y) || !isfinite(p2.x) || !isfinite(p2.y) ||
        !isfinite(theta1) || !isfinite(dtheta) ||
        rx == 0.0 || ry == 0.0 || (p1.x == p2.x && p1.y == p2.y)) {
        Seg s;
        s.type = 0;
        s.p0 = p1;
        s.p3 = (isfinite(p2.x) && isfinite(p2.y)) ? p2 : p1;
        s.c1 = s.p0; s.c2 = s.p3;
        raw_add(pool, r, s);
        return;
    }

    int nseg = (int)ceil(fabs(dtheta) / ((double)SVG_PI / 2.0));
    if (!(nseg >= 1)) nseg = 1;
    if (nseg > 64) nseg = 64;
    double dt = dtheta / nseg;

    for (int k = 0; k < nseg; k++) {
        double t1 = theta1 + k * dt;
        double t2 = t1 + dt;
        double h = (t2 - t1) / 2.0;
        double kk = (4.0 / 3.0) * tan(h / 2.0);

        double ax1 = rx * cos(t1),  ay1 = ry * sin(t1);
        double ax2 = rx * cos(t2),  ay2 = ry * sin(t2);
        double dx1 = -rx * sin(t1), dy1 = ry * cos(t1);
        double dx2 = -rx * sin(t2), dy2 = ry * cos(t2);

        Pt q1, qc1, qc2, q2;
        q1.x  = (float)(cosp * ax1 - sinp * ay1 + cx);
        q1.y  = (float)(sinp * ax1 + cosp * ay1 + cy);
        qc1.x = (float)(cosp * (ax1 + kk * dx1) - sinp * (ay1 + kk * dy1) + cx);
        qc1.y = (float)(sinp * (ax1 + kk * dx1) + cosp * (ay1 + kk * dy1) + cy);
        qc2.x = (float)(cosp * (ax2 - kk * dx2) - sinp * (ay2 - kk * dy2) + cx);
        qc2.y = (float)(sinp * (ax2 - kk * dx2) + cosp * (ay2 - kk * dy2) + cy);
        q2.x  = (float)(cosp * ax2 - sinp * ay2 + cx);
        q2.y  = (float)(sinp * ax2 + cosp * ay2 + cy);

        Seg s; s.type = 1; s.p0 = q1; s.c1 = qc1; s.c2 = qc2; s.p3 = q2;
        raw_add(pool, r, s);
    }
}

Path* path_parse(const char* d) {
    svg2img_limits def;
    svg2img_default_limits(&def);
    return path_parse_lim(d, &def, NULL);
}

Path* path_parse_lim(const char* d, const svg2img_limits* lim, int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    const int max_points = lim ? lim->max_path_points : SVG2IMG_LIMIT_MAX_PATH_POINTS;
    const int max_subpaths = lim ? lim->max_path_subpaths : SVG2IMG_LIMIT_MAX_PATH_SUBPATHS;
    if (!d) {
        Path* empty = (Path*)svg_malloc(sizeof(Path));
        if (empty) memset(empty, 0, sizeof(*empty));
        return empty;
    }

    size_t len = strlen(d);
    size_t i = 0;

    RawSub sub_local[8];
    RawSub* subs = sub_local;
    int nsubs = 0, capsubs = 8, subs_heap = 0;
    Seg seg_local[64];
    SegPool pool = { seg_local, 0, 64, 0 };
    int cur = -1;

    Pt cp = { 0, 0 }, sp = { 0, 0 };
    Pt prev_c2 = { 0, 0 }; int prev_cubic = 0;
    Pt prev_q1 = { 0, 0 }; int prev_quad = 0;
    char cmd = 0;

    #define NEW_SUB() do { \
        if (nsubs >= max_subpaths) { if (limit_hit) *limit_hit = 1; goto flatten; } \
        if (nsubs == capsubs && \
            !grow_buf((void**)&subs, &capsubs, &subs_heap, nsubs + 1, sizeof(RawSub))) \
            goto flatten; \
        cur = nsubs++; \
        subs[cur].off = pool.n; subs[cur].n = 0; subs[cur].closed = 0; \
    } while (0)

    #define ADD_LINE(to) do { \
        if (cur < 0) { NEW_SUB(); sp = cp; } \
        Seg s_; s_.type = 0; s_.p0 = cp; s_.p3 = (to); \
        s_.c1 = s_.p0; s_.c2 = s_.p3; raw_add(&pool, &subs[cur], s_); \
        cp = (to); \
    } while (0)

    #define ADD_CUBIC(c1_, c2_, p3_) do { \
        if (cur < 0) { NEW_SUB(); sp = cp; } \
        Seg s_; s_.type = 1; s_.p0 = cp; s_.c1 = (c1_); s_.c2 = (c2_); s_.p3 = (p3_); \
        raw_add(&pool, &subs[cur], s_); \
        prev_c2 = (c2_); prev_cubic = 1; prev_quad = 0; \
        cp = (p3_); \
    } while (0)

    while (i < len) {
        size_t iter_start = i;
        while (i < len && (isspace((unsigned char)d[i]) || d[i] == ',')) i++;
        if (i >= len) break;
        char c = d[i];
        if (isalpha((unsigned char)c)) { cmd = c; i++; }
        else if (!cmd) break;

        float v[7];
        switch (cmd) {
        case 'M': case 'm': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            float x = v[0], y = v[1];
            if (cmd == 'm') { x += cp.x; y += cp.y; }
            NEW_SUB();
            cp.x = sp.x = x; cp.y = sp.y = y;
            prev_cubic = 0; prev_quad = 0;

            for (;;) {
                size_t save = i;
                while (i < len && (isspace((unsigned char)d[i]) || d[i] == ',')) i++;
                if (i >= len) break;
                if (isalpha((unsigned char)d[i])) { i = save; break; }
                float x2, y2;
                if (!read_num(d, len, &i, &x2)) { i = save; break; }
                if (!read_num(d, len, &i, &y2)) { i = save; break; }
                if (cmd == 'm') { x2 += cp.x; y2 += cp.y; }
                Pt to = { x2, y2 };
                ADD_LINE(to);
            }
            break;
        }
        case 'L': case 'l': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            float x = v[0], y = v[1];
            if (cmd == 'l') { x += cp.x; y += cp.y; }
            Pt to = { x, y };
            ADD_LINE(to);
            break;
        }
        case 'H': case 'h': {
            if (!read_num(d, len, &i, &v[0])) break;
            float x = v[0];
            if (cmd == 'h') x += cp.x;
            Pt to = { x, cp.y };
            ADD_LINE(to);
            break;
        }
        case 'V': case 'v': {
            if (!read_num(d, len, &i, &v[0])) break;
            float y = v[0];
            if (cmd == 'v') y += cp.y;
            Pt to = { cp.x, y };
            ADD_LINE(to);
            break;
        }
        case 'C': case 'c': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            if (!read_num(d, len, &i, &v[2])) break;
            if (!read_num(d, len, &i, &v[3])) break;
            if (!read_num(d, len, &i, &v[4])) break;
            if (!read_num(d, len, &i, &v[5])) break;
            float x1 = v[0], y1 = v[1], x2 = v[2], y2 = v[3], x = v[4], y = v[5];
            if (cmd == 'c') { x1 += cp.x; y1 += cp.y; x2 += cp.x; y2 += cp.y; x += cp.x; y += cp.y; }
            Pt c1 = { x1, y1 }, c2 = { x2, y2 }, p3 = { x, y };
            ADD_CUBIC(c1, c2, p3);
            break;
        }
        case 'S': case 's': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            if (!read_num(d, len, &i, &v[2])) break;
            if (!read_num(d, len, &i, &v[3])) break;
            Pt c1 = prev_cubic ? (Pt){ 2 * cp.x - prev_c2.x, 2 * cp.y - prev_c2.y } : cp;
            float x2 = v[0], y2 = v[1], x = v[2], y = v[3];
            if (cmd == 's') { x2 += cp.x; y2 += cp.y; x += cp.x; y += cp.y; }
            Pt c2 = { x2, y2 }, p3 = { x, y };
            ADD_CUBIC(c1, c2, p3);
            break;
        }
        case 'Q': case 'q': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            if (!read_num(d, len, &i, &v[2])) break;
            if (!read_num(d, len, &i, &v[3])) break;
            float qx = v[0], qy = v[1], x = v[2], y = v[3];
            if (cmd == 'q') { qx += cp.x; qy += cp.y; x += cp.x; y += cp.y; }
            Pt q1 = { qx, qy }, p3 = { x, y };
            Pt c1 = { cp.x + (qx - cp.x) * 2.0f / 3.0f, cp.y + (qy - cp.y) * 2.0f / 3.0f };
            Pt c2 = { p3.x + (qx - p3.x) * 2.0f / 3.0f, p3.y + (qy - p3.y) * 2.0f / 3.0f };
            ADD_CUBIC(c1, c2, p3);
            prev_q1 = q1; prev_quad = 1; prev_cubic = 0;
            break;
        }
        case 'T': case 't': {
            if (!read_num(d, len, &i, &v[0])) break;
            if (!read_num(d, len, &i, &v[1])) break;
            float x = v[0], y = v[1];
            if (cmd == 't') { x += cp.x; y += cp.y; }
            Pt q1 = prev_quad ? (Pt){ 2 * cp.x - prev_q1.x, 2 * cp.y - prev_q1.y } : cp;
            Pt p3 = { x, y };
            Pt c1 = { cp.x + (q1.x - cp.x) * 2.0f / 3.0f, cp.y + (q1.y - cp.y) * 2.0f / 3.0f };
            Pt c2 = { p3.x + (q1.x - p3.x) * 2.0f / 3.0f, p3.y + (q1.y - p3.y) * 2.0f / 3.0f };
            ADD_CUBIC(c1, c2, p3);
            prev_q1 = q1; prev_quad = 1; prev_cubic = 0;
            break;
        }
        case 'A': case 'a': {
            float rx, ry, rot, x, y;
            int laf, sf;
            if (!read_num(d, len, &i, &rx)) break;
            if (!read_num(d, len, &i, &ry)) break;
            if (!read_num(d, len, &i, &rot)) break;
            if (!read_flag(d, len, &i, &laf)) break;
            if (!read_flag(d, len, &i, &sf)) break;
            if (!read_num(d, len, &i, &x)) break;
            if (!read_num(d, len, &i, &y)) break;
            if (cmd == 'a') { x += cp.x; y += cp.y; }
            Pt p2 = { x, y };
            if (cur < 0) { NEW_SUB(); sp = cp; }
            append_arc(&pool, &subs[cur], cp, p2, (double)rx, (double)ry, (double)rot, laf, sf);
            prev_cubic = 1; prev_quad = 0;

            if (subs[cur].n > 0)
                prev_c2 = pool.p[subs[cur].off + subs[cur].n - 1].c2;
            else
                prev_cubic = 0;
            cp = p2;
            break;
        }
        case 'Z': case 'z': {
            if (cur >= 0) {
                subs[cur].closed = 1;
                cp = sp;
            }

            {
                size_t save = i;
                while (i < len && (isspace((unsigned char)d[i]) || d[i] == ',')) i++;
                if (i < len && !isalpha((unsigned char)d[i])) {

                    if (cur >= 0) {
                        NEW_SUB();
                        cp.x = sp.x; cp.y = sp.y;
                    }
                    cmd = (cmd == 'Z') ? 'L' : 'l';
                    i = save;
                }
            }
            break;
        }
        default:

            i++;
            break;
        }

        if (i == iter_start) i++;
    }

flatten: {

    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int s = 0; s < nsubs; s++) {
        for (int k = 0; k < subs[s].n; k++) {
            const Seg* g = &pool.p[subs[s].off + k];
            float xs[4] = { g->p0.x, g->p3.x, g->c1.x, g->c2.x };
            float ys[4] = { g->p0.y, g->p3.y, g->c1.y, g->c2.y };
            for (int q = 0; q < 4; q++) {
                if (xs[q] < bx0) bx0 = xs[q];
                if (xs[q] > bx1) bx1 = xs[q];
                if (ys[q] < by0) by0 = ys[q];
                if (ys[q] > by1) by1 = ys[q];
            }
        }
    }
    float diag = sqrtf((bx1 - bx0) * (bx1 - bx0) + (by1 - by0) * (by1 - by0));
    if (!(diag > 0.0f)) diag = 1.0f;

    float tol = diag / 2000.0f;
    if (tol < 1e-6f) tol = 1e-6f;
    if (tol > 2.0f) tol = 2.0f;

    Pt pt_local[256];
    Pt* pts = pt_local;
    int npts = 0, cappts = 256, pts_heap = 0;
    int sub_n_local[8];
    int* sub_n = nsubs <= 8 ? sub_n_local : (int*)svg_malloc((size_t)nsubs * sizeof(int));
    if (!sub_n) nsubs = 0;
    for (int s = 0; s < nsubs; s++) {
        int on = 0;
        int full = 0;

        #define PUSH_PT(P) do { \
            if (on >= max_points) { full = 1; if (limit_hit) *limit_hit = 1; } \
            else if (npts == cappts && \
                     !grow_buf((void**)&pts, &cappts, &pts_heap, npts + 1, sizeof(Pt))) { \
                full = 1; \
            } else { pts[npts++] = (P); on++; } \
        } while (0)
        for (int k = 0; k < subs[s].n && !full; k++) {
            const Seg* g = &pool.p[subs[s].off + k];
            if (k == 0) PUSH_PT(g->p0);
            if (g->type == 0) {
                PUSH_PT(g->p3);
            } else {

                typedef struct { Pt p0, c1, c2, p3; int depth; } FlatEntry;
                FlatEntry st[64];
                int spn = 0;
                st[spn].p0 = g->p0; st[spn].c1 = g->c1;
                st[spn].c2 = g->c2; st[spn].p3 = g->p3; st[spn].depth = 0;
                spn++;
                while (spn > 0) {

                    FlatEntry e = st[--spn];
                    float dx = e.p3.x - e.p0.x, dy = e.p3.y - e.p0.y;
                    float l = sqrtf(dx * dx + dy * dy);
                    float d1 = l > 1e-9f ? fabsf((e.c1.x - e.p0.x) * dy -
                                                  (e.c1.y - e.p0.y) * dx) / l : 0.0f;
                    float d2 = l > 1e-9f ? fabsf((e.c2.x - e.p0.x) * dy -
                                                  (e.c2.y - e.p0.y) * dx) / l : 0.0f;
                    if (e.depth >= 24 || (d1 <= tol && d2 <= tol)) {
                        PUSH_PT(e.p3);
                        continue;
                    }
                    Pt p01 = midpt(e.p0, e.c1);
                    Pt p12 = midpt(e.c1, e.c2);
                    Pt p23 = midpt(e.c2, e.p3);
                    Pt p012 = midpt(p01, p12);
                    Pt p123 = midpt(p12, p23);
                    Pt p0123 = midpt(p012, p123);

                    if (spn + 2 < 64) {
                        st[spn].p0 = p0123; st[spn].c1 = p123;
                        st[spn].c2 = p23;   st[spn].p3 = e.p3;
                        st[spn].depth = e.depth + 1;
                        spn++;
                        st[spn].p0 = e.p0; st[spn].c1 = p01;
                        st[spn].c2 = p012;  st[spn].p3 = p0123;
                        st[spn].depth = e.depth + 1;
                        spn++;
                    } else {
                        PUSH_PT(e.p3);
                    }
                }
            }
        }
        #undef PUSH_PT
        sub_n[s] = on;
    }
    Path* path = (Path*)svg_malloc(sizeof(Path) + (size_t)nsubs * sizeof(SubPath) +
                                   (size_t)npts * sizeof(Pt));
    if (path) {
        memset(path, 0, sizeof(*path));
        SubPath* out = (SubPath*)(void*)(path + 1);
        Pt* op = (Pt*)(void*)(out + nsubs);
        if (npts) memcpy(op, pts, (size_t)npts * sizeof(Pt));
        path->s = path->s_inline = out;
        path->n = path->cap = nsubs;
        path->shared_points = PATH_ONE_BLOCK;
        for (int s = 0; s < nsubs; s++) {
            out[s].p = op;
            out[s].n = sub_n[s];
            out[s].closed = subs[s].closed;
            op += sub_n[s];
        }
    }
    if (sub_n != sub_n_local) svg_mfree(sub_n);
    if (pts_heap) svg_mfree(pts);
    if (pool.heap) svg_mfree(pool.p);
    if (subs_heap) svg_mfree(subs);
    #undef NEW_SUB
    #undef ADD_LINE
    #undef ADD_CUBIC
    return path;
}
}
