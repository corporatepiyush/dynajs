#include "svg-int.h"

void path_free(Path* p) {
    if (!p) return;
    if (p->shared_points == PATH_ONE_BLOCK) {

        if (p->s != p->s_inline) svg_mfree(p->s);
        svg_mfree(p);
        return;
    }
    if (p->shared_points) {

        svg_mfree(p->pts_arena);
    } else {
        for (int i = 0; i < p->n; i++) svg_mfree(p->s[i].p);
    }
    svg_mfree(p->s);
    svg_mfree(p);
}

size_t path_block_size(const Path* p) {
    size_t total = 0;
    for (int i = 0; i < p->n; i++) total += (size_t)(p->s[i].n > 0 ? p->s[i].n : 0);
    return sizeof(Path) + (size_t)p->n * sizeof(SubPath) + total * sizeof(Pt);
}

Path* path_copy_xform(const Path* p, const Mat* m) {
    if (!p) return NULL;
    void* mem = svg_malloc(path_block_size(p));
    return mem ? path_copy_xform_at(p, m, mem) : NULL;
}

Path* path_copy_xform_at(const Path* p, const Mat* m, void* mem) {
    Path* q = (Path*)mem;
    memset(q, 0, sizeof(*q));
    SubPath* subs = (SubPath*)(void*)(q + 1);
    Pt* pts = (Pt*)(void*)(subs + p->n);
    q->s = q->s_inline = subs;
    q->n = q->cap = p->n;
    q->shared_points = PATH_ONE_BLOCK;
    for (int i = 0; i < p->n; i++) {
        int n = p->s[i].n > 0 ? p->s[i].n : 0;
        subs[i].n = n;
        subs[i].closed = p->s[i].closed;
        subs[i].p = pts;
        const Pt* src = p->s[i].p;
        if (m) {
            for (int k = 0; k < n; k++) {
                Pt v = src[k];
                pts[k].x = m->m[0] * v.x + m->m[2] * v.y + m->m[4];
                pts[k].y = m->m[1] * v.x + m->m[3] * v.y + m->m[5];
            }
        } else if (n) {
            memcpy(pts, src, (size_t)n * sizeof(Pt));
        }
        pts += n;
    }
    return q;
}

Path* path_copy(const Path* p) { return path_copy_xform(p, NULL); }

void path_transform(Path* p, Mat m) {
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        for (int k = 0; k < p->s[i].n; k++) {
            Pt q = p->s[i].p[k];
            p->s[i].p[k].x = m.m[0] * q.x + m.m[2] * q.y + m.m[4];
            p->s[i].p[k].y = m.m[1] * q.x + m.m[3] * q.y + m.m[5];
        }
    }
}

void path_bbox(const Path* p, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        for (int k = 0; k < p->s[i].n; k++) {
            Pt q = p->s[i].p[k];
            if (q.x < out[0]) out[0] = q.x;
            if (q.y < out[1]) out[1] = q.y;
            if (q.x > out[2]) out[2] = q.x;
            if (q.y > out[3]) out[3] = q.y;
        }
    }
    if (out[0] > out[2]) { out[0] = out[1] = out[2] = out[3] = 0.0f; }
}

void path_bbox_xform(const Path* p, Mat m, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    if (!p) return;
    for (int i = 0; i < p->n; i++) {
        for (int k = 0; k < p->s[i].n; k++) {
            Pt v = p->s[i].p[k];
            float x = m.m[0] * v.x + m.m[2] * v.y + m.m[4];
            float y = m.m[1] * v.x + m.m[3] * v.y + m.m[5];
            if (x < out[0]) out[0] = x;
            if (y < out[1]) out[1] = y;
            if (x > out[2]) out[2] = x;
            if (y > out[3]) out[3] = y;
        }
    }
    if (out[0] > out[2]) { out[0] = out[1] = out[2] = out[3] = 0.0f; }
}

float path_length(const Path* p) {
    if (!p) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < p->n; i++) {
        const SubPath* s = &p->s[i];
        for (int k = 1; k < s->n; k++) {
            float dx = s->p[k].x - s->p[k - 1].x;
            float dy = s->p[k].y - s->p[k - 1].y;
            total += sqrtf(dx * dx + dy * dy);
        }
        if (s->closed && s->n > 2) {
            float dx = s->p[0].x - s->p[s->n - 1].x;
            float dy = s->p[0].y - s->p[s->n - 1].y;
            total += sqrtf(dx * dx + dy * dy);
        }
    }
    return total;
}
