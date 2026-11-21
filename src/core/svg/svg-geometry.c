#include "svg-doc.h"
#include <ctype.h>

float len_attr(Ctx* ctx, const XMLNode* xn, const char* name,
                      int axis, float def) {
    const char* v = xml_attr(xn, name);
    if (!v) return def;
    return length_parse(v, ctx->vw, ctx->vh, ctx->em, axis, def);
}

Path* path_from_rect(float x, float y, float w, float h, float rx, float ry) {

    if (!(w > 0.0f) || !(h > 0.0f)) return path_parse("");
    if (rx < 0.0f) rx = 0.0f;
    if (ry < 0.0f) ry = 0.0f;
    if (rx > w * 0.5f) rx = w * 0.5f;
    if (ry > h * 0.5f) ry = h * 0.5f;

    char buf[512];
    if (rx > 0.0f && ry > 0.0f) {
        snprintf(buf, sizeof(buf),
                 "M%g,%g H%g A%g,%g 0 0 1 %g,%g V%g A%g,%g 0 0 1 %g,%g H%g "
                 "A%g,%g 0 0 1 %g,%g V%g A%g,%g 0 0 1 %g,%g Z",
                 (double)(x + rx), (double)y, (double)(x + w - rx), (double)rx, (double)ry, (double)(x + w), (double)(y + ry),
                 (double)(y + h - ry), (double)rx, (double)ry, (double)(x + w - rx), (double)(y + h), (double)(x + rx),
                 (double)rx, (double)ry, (double)x, (double)(y + h - ry), (double)(y + ry), (double)rx, (double)ry, (double)(x + rx), (double)y);
    } else {
        snprintf(buf, sizeof(buf), "M%g,%g H%g V%g H%g Z", (double)x, (double)y, (double)(x + w), (double)(y + h), (double)x);
    }
    return path_parse(buf);
}

Path* path_from_ellipse(float cx, float cy, float rx, float ry) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) return path_parse("");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "M%g,%g A%g,%g 0 1 0 %g,%g A%g,%g 0 1 0 %g,%g Z",
             (double)(cx - rx), (double)cy, (double)rx, (double)ry, (double)(cx + rx), (double)cy, (double)rx, (double)ry, (double)(cx - rx), (double)cy);
    return path_parse(buf);
}

Path* path_from_line(float x1, float y1, float x2, float y2) {
    char buf[128];
    snprintf(buf, sizeof(buf), "M%g,%g L%g,%g", (double)x1, (double)y1, (double)x2, (double)y2);
    return path_parse(buf);
}

Path* path_from_points(const char* s, int close) {
    return path_from_points_lim(s, close, NULL, NULL);
}

Path* path_from_points_lim(const char* s, int close, const svg2img_limits* lim,
                           int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    const int max_points = lim ? lim->max_path_points : SVG2IMG_LIMIT_MAX_PATH_POINTS;

    Pt local[256];
    Pt* pts = local;
    int n = 0, cap = 256, heap = 0;
    const char* q = s;
    while (q && *q) {
        while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
        if (!*q) break;
        char* end = NULL;
        float x = strtof(q, &end);
        if (end == q) break;
        q = end;
        while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
        float y = strtof(q, &end);
        if (end == q) break;
        q = end;

        if (n >= max_points) {
            if (limit_hit) *limit_hit = 1;
            break;
        }
        if (n == cap) {
            int ncap = cap * 2;
            if (ncap > max_points) ncap = max_points;
            Pt* np = heap ? (Pt*)svg_realloc(pts, (size_t)ncap * sizeof(Pt))
                          : (Pt*)svg_malloc((size_t)ncap * sizeof(Pt));
            if (!np) break;
            if (!heap) memcpy(np, pts, (size_t)n * sizeof(Pt));
            pts = np; cap = ncap; heap = 1;
        }
        pts[n].x = x;
        pts[n].y = y;
        n++;
    }
    Path* p = (Path*)svg_malloc(sizeof(Path) + sizeof(SubPath) + (size_t)n * sizeof(Pt));
    if (p) {
        memset(p, 0, sizeof(*p));
        SubPath* sp = (SubPath*)(void*)(p + 1);
        Pt* op = (Pt*)(void*)(sp + 1);
        if (n) memcpy(op, pts, (size_t)n * sizeof(Pt));
        p->s = p->s_inline = sp;
        p->n = p->cap = 1;
        p->shared_points = PATH_ONE_BLOCK;
        sp->p = op;
        sp->n = n;
        sp->closed = close;
    }
    if (heap) svg_mfree(pts);
    return p;
}
