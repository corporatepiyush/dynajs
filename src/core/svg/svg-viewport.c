#include "svg-doc.h"
#include <ctype.h>
#include <math.h>

int svg_parse_viewbox(const char* s, float* x, float* y, float* w, float* h) {
    if (!s) return 0;
    const char* p = s;
    float* out[4] = {x, y, w, h};
    for (int i = 0; i < 4; i++) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        char* end = NULL;
        *out[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
    }
    return 1;
}

#define SVG2IMG_MAX_DIM   (1 << 15)
#define SVG2IMG_MAX_PIXELS ((size_t)1 << 28)

void compute_viewport(const XMLNode* svg, const svg2img_options* opts,
                             int* W, int* H, Mat* tm,
                             const svg2img_limits* lim, int* clamped) {
    if (clamped) *clamped = 0;
    int max_dim = SVG2IMG_MAX_DIM;
    size_t max_pixels = SVG2IMG_MAX_PIXELS;
    if (lim) {

        if (lim->max_pixels > 0) {
            size_t mp = (size_t)lim->max_pixels;
            int d = 1;
            while ((size_t)d * (size_t)d <= mp && d < SVG2IMG_MAX_DIM) d++;
            if (d > 1) max_dim = d - 1;
            max_pixels = mp;
        }
    }
    float vw = length_parse(xml_attr(svg, "width"), 0.0f, 0.0f, 16.0f, 0, 0.0f);
    float vh = length_parse(xml_attr(svg, "height"), 0.0f, 0.0f, 16.0f, 1, 0.0f);

    float vbx = 0, vby = 0, vbw = 0, vbh = 0;
    int has_vb = svg_parse_viewbox(xml_attr(svg, "viewBox"), &vbx, &vby, &vbw, &vbh);
    if (!has_vb || !(vbw > 0.0f) || !(vbh > 0.0f)) has_vb = 0;

    if (!isfinite(vw) || vw <= 0.0f) vw = has_vb ? vbw : 300.0f;
    if (!isfinite(vh) || vh <= 0.0f) vh = has_vb ? vbh : 150.0f;

    float scale = (opts && opts->scale > 0.0f) ? opts->scale : 1.0f;
    if (!isfinite(scale)) scale = 1.0f;

    double dw = (double)vw * (double)scale + 0.5;
    double dh = (double)vh * (double)scale + 0.5;
    if (!(dw > 0.0)) dw = 1.0;
    if (!(dh > 0.0)) dh = 1.0;
    if (dw > (double)max_dim) { dw = (double)max_dim; if (clamped) *clamped = 1; }
    if (dh > (double)max_dim) { dh = (double)max_dim; if (clamped) *clamped = 1; }

    int outw = (opts && opts->width > 0)  ? opts->width  : (int)dw;
    int outh = (opts && opts->height > 0) ? opts->height : (int)dh;

    if (opts && opts->width > 0 && opts->height <= 0) {
        double h2 = (double)opts->width * (double)vh / (double)vw + 0.5;
        outh = h2 < 1.0 ? 1 : h2 > (double)max_dim ? max_dim : (int)h2;
    } else if (opts && opts->height > 0 && opts->width <= 0) {
        double w2 = (double)opts->height * (double)vw / (double)vh + 0.5;
        outw = w2 < 1.0 ? 1 : w2 > (double)max_dim ? max_dim : (int)w2;
    }
    if (outw > max_dim) { outw = max_dim; if (clamped) *clamped = 1; }
    if (outh > max_dim) { outh = max_dim; if (clamped) *clamped = 1; }
    if ((size_t)outw * (size_t)outh > max_pixels) {
        if (clamped) *clamped = 1;
        double k = sqrt((double)max_pixels /
                        ((double)outw * (double)outh));
        outw = (int)((double)outw * k);
        outh = (int)((double)outh * k);
        if (outw < 1) outw = 1;
        if (outh < 1) outh = 1;
    }

    Mat m = mat_scale((float)outw / vw, (float)outh / vh);
    if (has_vb) {
        float sx = (float)outw / vbw;
        float sy = (float)outh / vbh;

        int none = 0, slice = 0;
        int xa = 1, ya = 1;
        const char* par = xml_attr(svg, "preserveAspectRatio");
        if (par) {
            if (strncmp(par, "none", 4) == 0) {
                none = 1;
            } else {
                if (strstr(par, "xMin")) xa = 0;
                else if (strstr(par, "xMax")) xa = 2;
                if (strstr(par, "YMin")) ya = 0;
                else if (strstr(par, "YMax")) ya = 2;
                if (strstr(par, "slice")) slice = 1;
            }
        }

        if (none) {
            m = mat_mul(mat_scale(sx, sy), mat_translate(-vbx, -vby));
        } else {
            float s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
            float ex = (float)outw - vbw * s;
            float ey = (float)outh - vbh * s;

            float ox = (xa == 1) ? ex * 0.5f : (xa == 2 ? ex : 0.0f);
            float oy = (ya == 1) ? ey * 0.5f : (ya == 2 ? ey : 0.0f);
            m = mat_mul(mat_translate(ox, oy),
                        mat_mul(mat_scale(s, s),
                                mat_translate(-vbx, -vby)));
        }
    }
    *W = outw;
    *H = outh;
    *tm = m;
}
