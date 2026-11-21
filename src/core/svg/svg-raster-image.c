#include "svg-raster.h"

static void sample_bilinear8(const unsigned char* src, int sw, int sh, float fx,
                             float fy, float* out) {
    if (sw <= 0 || sh <= 0) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float)(sw - 1)) fx = (float)(sw - 1);
    if (fy > (float)(sh - 1)) fy = (float)(sh - 1);
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int x1 = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
    int y1 = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++) {
        int yy = j ? y1 : y0;
        float wy = j ? ty : 1.0f - ty;
        for (int i = 0; i < 2; i++) {
            int xx = i ? x1 : x0;
            float w = (i ? tx : 1.0f - tx) * wy;
            const unsigned char* p = src + ((size_t)yy * (size_t)sw + (size_t)xx) * 4;
            float a = (float)p[3] / 255.0f;
            acc[0] += (float)p[0] / 255.0f * a * w;
            acc[1] += (float)p[1] / 255.0f * a * w;
            acc[2] += (float)p[2] / 255.0f * a * w;
            acc[3] += a * w;
        }
    }
    if (acc[3] > 1e-6f) {
        out[0] = acc[0] / acc[3];
        out[1] = acc[1] / acc[3];
        out[2] = acc[2] / acc[3];
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    out[3] = acc[3];
}

void image_blit_rgba8(Canvas* dst, int dx, int dy, int dw, int dh,
                      const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            float c[4];
            sample_bilinear8(src, sw, sh, fx, fy, c);
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
            d[3] = c[3];
        }
    }
}

void image_blit_fbuf(FBuf* dst, int dx, int dy, int dw, int dh,
                     const unsigned char* src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w) continue;
            float c[4];
            sample_bilinear8(src, sw, sh, fx, fy, c);
            float* d = dst->px + ((size_t)ty * (size_t)dst->w + (size_t)tx) * 4;
            d[0] = c[0];
            d[1] = c[1];
            d[2] = c[2];
            d[3] = c[3];
        }
    }
}

static void dev_rect(Mat world, float x, float y, float w, float h, float r[4]) {
    Pt q[4] = { {x, y}, {x + w, y}, {x + w, y + h}, {x, y + h} };
    r[0] = r[1] = 1e30f;
    r[2] = r[3] = -1e30f;
    for (int i = 0; i < 4; i++) {
        Pt p = mat_apply(world, q[i]);
        if (p.x < r[0]) r[0] = p.x;
        if (p.y < r[1]) r[1] = p.y;
        if (p.x > r[2]) r[2] = p.x;
        if (p.y > r[3]) r[3] = p.y;
    }
}

void render_image(Canvas* c, const Node* n, Mat world, float opacity,
                  const Mask* clip, int aa) {
    (void)aa;
    if (!n || !n->image_href || !(n->img_w > 0.0f) || !(n->img_h > 0.0f)) return;

    const unsigned char* raw = n->img_px;
    int sw = n->img_iw, sh = n->img_ih;
    if (!raw || sw <= 0 || sh <= 0) return;

    float px = n->img_x, py = n->img_y, pw = n->img_w, ph = n->img_h;
    if (!n->img_par_none) {
        float sx = n->img_w / (float)sw, sy = n->img_h / (float)sh;
        float s = n->img_par_slice ? fmaxf(sx, sy) : fminf(sx, sy);
        pw = (float)sw * s;
        ph = (float)sh * s;
        px = n->img_x + (n->img_w - pw) * 0.5f * (float)n->img_par_xa;
        py = n->img_y + (n->img_h - ph) * 0.5f * (float)n->img_par_ya;
    }
    float vr[4], pr[4];
    dev_rect(world, n->img_x, n->img_y, n->img_w, n->img_h, vr);
    dev_rect(world, px, py, pw, ph, pr);
    if (!(pr[2] > pr[0]) || !(pr[3] > pr[1])) return;

    float x0 = fmaxf(vr[0], pr[0]), y0 = fmaxf(vr[1], pr[1]);
    float x1 = fminf(vr[2], pr[2]), y1 = fminf(vr[3], pr[3]);
    int ix0 = svg_f2i(floorf(x0 + 0.5f), 0, c->w, 0);
    int iy0 = svg_f2i(floorf(y0 + 0.5f), 0, c->h, 0);
    int ix1 = svg_f2i(floorf(x1 + 0.5f), 0, c->w, 0);
    int iy1 = svg_f2i(floorf(y1 + 0.5f), 0, c->h, 0);
    if (ix0 >= ix1 || iy0 >= iy1) return;
    int dw = ix1 - ix0, dh = iy1 - iy0;

    Canvas* img = canvas_create(dw, dh);
    if (!img) return;
    float kx = (float)sw / (pr[2] - pr[0]), ky = (float)sh / (pr[3] - pr[1]);
    if (!isfinite(kx) || !isfinite(ky) || !isfinite(pr[0]) || !isfinite(pr[1])) {
        canvas_free(img);
        return;
    }
    int ylo = c->wy0 - iy0, yhi = c->wy1 - iy0;
    if (ylo < 0) ylo = 0;
    if (yhi > dh) yhi = dh;
    for (int y = ylo; y < yhi; y++) {
        float fy = ((float)(iy0 + y) + 0.5f - pr[1]) * ky - 0.5f;
        float* d = img->px + (size_t)y * (size_t)dw * 4;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)(ix0 + x) + 0.5f - pr[0]) * kx - 0.5f;
            sample_bilinear8(raw, sw, sh, fx, fy, d + (size_t)x * 4);
        }
    }

    for (int y = ylo; y < yhi; y++) {
        int mx0 = 0, mx1 = 0;
        const float* mrow = clip ? mask_row(clip, iy0 + y, &mx0, &mx1) : NULL;
        const float* src = img->px + (size_t)y * (size_t)dw * 4;
        for (int x = 0; x < dw; x++) {
            const float* s = src + (size_t)x * 4;
            float a = s[3] * opacity;
            if (clip) {
                int gx = ix0 + x;
                a *= (gx >= mx0 && gx < mx1) ? mrow[gx - mx0] : 0.0f;
            }
            if (a <= 0.0f) continue;
            blend(c, ix0 + x, iy0 + y, (Color){s[0], s[1], s[2], s[3]}, a);
        }
    }
    canvas_free(img);
}
