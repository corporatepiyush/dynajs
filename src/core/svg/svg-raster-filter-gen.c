#include "svg-raster.h"

static unsigned hash_u32(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float lattice_rand(int ix, int iy, int seed, int chan) {
    unsigned h = hash_u32((unsigned)ix * 374761393u + (unsigned)iy * 668265263u +
                          (unsigned)seed * 1442695041u +
                          (unsigned)chan * 1013904223u);
    return (float)(h >> 8) / 16777216.0f;
}

static float vnoise(float x, float y, int seed, int chan) {

    int xi = svg_f2i(floorf(x), -1073741824, 1073741824, 0);
    int yi = svg_f2i(floorf(y), -1073741824, 1073741824, 0);
    float xf = x - (float)xi, yf = y - (float)yi;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float a = lattice_rand(xi, yi, seed, chan);
    float b = lattice_rand(xi + 1, yi, seed, chan);
    float c = lattice_rand(xi, yi + 1, seed, chan);
    float d = lattice_rand(xi + 1, yi + 1, seed, chan);
    float top = a * (1.0f - u) + b * u;
    float bot = c * (1.0f - u) + d * u;
    return top * (1.0f - v) + bot * v;
}

static float turbulence_channel(const FilterPrim* p, float x, float y, int chan) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    float fx = p->base_freq_x, fy = p->base_freq_y;
    int octaves = p->octaves;
    for (int o = 0; o < octaves; o++) {
        float n = vnoise(x * fx + (float)(chan * 17), y * fy + (float)(chan * 31),
                         p->seed + chan * 101, chan);
        if (p->turb_type == FE_TURB_TURBULENCE) sum += amp * fabsf(2.0f * n - 1.0f);
        else sum += amp * (2.0f * n - 1.0f);
        norm += amp;
        amp *= 0.5f;
        fx *= 2.0f;
        fy *= 2.0f;
    }
    if (norm <= 0.0f) return 0.0f;
    float v = sum / norm;
    if (p->turb_type == FE_TURB_FRACTAL) v = v * 0.5f + 0.5f;
    return v;
}

static FBuf* fe_turbulence(FilterCtx* fc, const FilterPrim* p) {
    FBuf* out = fe_new(fc, fc->src.w, fc->src.h);
    if (!out) return NULL;
    if (!(p->base_freq_x > 0.0f) && !(p->base_freq_y > 0.0f)) {

        for (size_t i = 0; i < (size_t)out->w * (size_t)out->h; i++) {
            float* d = out->px + i * 4;
            d[0] = d[1] = d[2] = p->turb_type == FE_TURB_FRACTAL ? 0.5f : 0.0f;
            d[3] = 1.0f;
        }
        return out;
    }
    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++) {
            float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
            d[0] = turbulence_channel(p, (float)x, (float)y, 0);
            d[1] = turbulence_channel(p, (float)x, (float)y, 1);
            d[2] = turbulence_channel(p, (float)x, (float)y, 2);
            d[3] = turbulence_channel(p, (float)x, (float)y, 3);
        }
    }
    return out;
}

static float edge_sample_from(const float* buf, int w, int h, int x, int y,
                              int c, int mode) {
    if (mode == 2) {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
        return buf[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c];
    }
    if (mode == 1) {
        x %= w; if (x < 0) x += w;
        y %= h; if (y < 0) y += h;
        return buf[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c];
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return buf[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c];
}

static FBuf* fe_convolve(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    int ox = p->order_x, oy = p->order_y;
    int n = ox * oy;

    if (n <= 0 || p->nkernel != n) {
        memcpy(out->px, in->px, fb_bytes(in->w, in->h));
        return out;
    }
    float div = p->divisor;
    if (div == 0.0f || !isfinite(div)) div = 1.0f;
    else if (p->divisor < 0.0f) {
        div = 0.0f;
        for (int i = 0; i < n; i++) div += p->kernel[i];
        if (fabsf(div) < 1e-9f) div = 1.0f;
    }
    int tx = svg_f2i(floorf(p->target_x + 0.5f), 0, ox - 1, 0);
    int ty = svg_f2i(floorf(p->target_y + 0.5f), 0, oy - 1, 0);

    if (p->preserve_alpha) {

        for (int y = 0; y < in->h; y++) {
            for (int x = 0; x < in->w; x++) {
                float sum[3] = {0, 0, 0};
                for (int ky = 0; ky < oy; ky++)
                    for (int kx = 0; kx < ox; kx++) {
                        float k = p->kernel[ky * ox + kx];
                        if (k == 0.0f) continue;
                        for (int c = 0; c < 3; c++)
                            sum[c] += k * edge_sample_from(in->px, in->w, in->h,
                                                           x + kx - tx, y + ky - ty,
                                                           c, p->edge_mode);
                    }
                const float* s = in->px + ((size_t)y * (size_t)in->w + (size_t)x) * 4;
                float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
                for (int c = 0; c < 3; c++) {
                    float v = sum[c] / div + p->bias;
                    d[c] = !(v >= 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
                }
                d[3] = s[3];
            }
        }
        return out;
    }

    float* buf = (float*)svg_malloc((size_t)in->w * (size_t)in->h * 4 * sizeof(float));
    if (!buf) { fe_release(fc, out); fc->failed = 1; return NULL; }
    fe_premul(in, buf);

    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            float sum[4] = {0, 0, 0, 0};
            for (int ky = 0; ky < oy; ky++) {
                for (int kx = 0; kx < ox; kx++) {
                    float k = p->kernel[ky * ox + kx];
                    if (k == 0.0f) continue;
                    int sx = x + kx - tx;
                    int sy = y + ky - ty;
                    for (int c = 0; c < 4; c++)
                        sum[c] += k * edge_sample_from(buf, in->w, in->h, sx, sy,
                                                       c, p->edge_mode);
                }
            }
            float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
            for (int c = 0; c < 4; c++) {
                float v = sum[c] / div + p->bias;
                if (!(v >= 0.0f)) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                d[c] = v;
            }
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

static void sample_bilinear(const FBuf* b, float x, float y, float* out) {
    if (!b || x < 0.0f || y < 0.0f || x > (float)(b->w - 1) || y > (float)(b->h - 1)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    int x0 = svg_f2i(floorf(x), 0, b->w - 1, 0);
    int y0 = svg_f2i(floorf(y), 0, b->h - 1, 0);
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 >= b->w) x1 = b->w - 1;
    if (y1 >= b->h) y1 = b->h - 1;
    float fx = x - (float)x0, fy = y - (float)y0;
    for (int c = 0; c < 4; c++) {
        float p00 = b->px[((size_t)y0 * (size_t)b->w + (size_t)x0) * 4 + (size_t)c];
        float p10 = b->px[((size_t)y0 * (size_t)b->w + (size_t)x1) * 4 + (size_t)c];
        float p01 = b->px[((size_t)y1 * (size_t)b->w + (size_t)x0) * 4 + (size_t)c];
        float p11 = b->px[((size_t)y1 * (size_t)b->w + (size_t)x1) * 4 + (size_t)c];
        out[c] = (p00 * (1.0f - fx) + p10 * fx) * (1.0f - fy) +
                 (p01 * (1.0f - fx) + p11 * fx) * fy;
    }
}

static FBuf* fe_displacement(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    const FBuf* map = fe_input(fc, p->in2);
    if (!in) in = &fc->src;
    if (!map) map = in;
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    float scale = fe_len(fc, p->num[0], 2);
    int xc = svg_f2i(p->num[1], 0, 3, 0);
    int yc = svg_f2i(p->num[2], 0, 3, 0);
    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++) {
            float mx = (float)fe_chan(map, x, y, xc) / 255.0f - 0.5f;
            float my = (float)fe_chan(map, x, y, yc) / 255.0f - 0.5f;
            float sx = (float)x + scale * mx;
            float sy = (float)y + scale * my;
            float s[4];
            sample_bilinear(in, sx, sy, s);
            float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    return out;
}

static FBuf* fe_image(FilterCtx* fc, const FilterPrim* p) {
    FBuf* out = fe_new(fc, fc->src.w, fc->src.h);
    if (!out) return NULL;
    if (p->ref_node) {

        Canvas* tmp = canvas_create(out->w, out->h);
        if (!tmp) return out;
        Mat buf = mat_mul(mat_translate(-(float)fc->ox, -(float)fc->oy),
                          fc->world);
        render_one(tmp, p->ref_node, buf, fc->aa);
        size_t npix = (size_t)out->w * (size_t)out->h;
        for (size_t i = 0; i < npix; i++) {
            const float* s = tmp->px + i * 4;
            float* d = out->px + i * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
        canvas_free(tmp);
        if (fc->linear) fe_srgb_to_linear(out);
        fc->last_sx = 0;
        fc->last_sy = 0;
        fc->last_sw = out->w;
        fc->last_sh = out->h;
        return out;
    }
    if (!p->href) return out;

    const unsigned char* raw = p->px;
    int sw = p->pw, sh = p->ph;
    if (!raw) return out;

    float ux = p->num[0], uy = p->num[1];
    float uw = p->num[2], uh = p->num[3];
    if (fc->prim_units_bbox && fc->has_bbox) {
        float bw = fc->bbox[2] - fc->bbox[0];
        float bh = fc->bbox[3] - fc->bbox[1];
        ux = fc->bbox[0] + ux * bw;
        uy = fc->bbox[1] + uy * bh;
        uw *= bw;
        uh *= bh;
    }
    Pt a = mat_apply(fc->world, (Pt){ux, uy});
    Pt b = mat_apply(fc->world, (Pt){ux + uw, uy + uh});

    float ax = floorf(a.x + 0.5f) - (float)fc->ox;
    float ay = floorf(a.y + 0.5f) - (float)fc->oy;
    float bx = floorf(b.x + 0.5f) - (float)fc->ox;
    float by = floorf(b.y + 0.5f) - (float)fc->oy;
    int dx = svg_f2i(ax, -out->w - 1, out->w + 1, 0);
    int dy = svg_f2i(ay, -out->h - 1, out->h + 1, 0);
    int dw = svg_f2i(bx - ax, 0, out->w + 1, 0);
    int dh = svg_f2i(by - ay, 0, out->h + 1, 0);
    if (!(p->num[2] > 0.0f) || !(p->num[3] > 0.0f) || dw <= 0 || dh <= 0) {
        dw = out->w - dx;
        dh = out->h - dy;
        if (dw < 0) dw = 0;
        if (dh < 0) dh = 0;
        if (dw > out->w + 1) dw = out->w + 1;
        if (dh > out->h + 1) dh = out->h + 1;
    }
    if (dw > 0 && dh > 0) image_blit_fbuf(out, dx, dy, dw, dh, raw, sw, sh);
    if (fc->linear) fe_srgb_to_linear(out);

    fc->last_sx = dx;
    fc->last_sy = dy;
    fc->last_sw = dw;
    fc->last_sh = dh;
    return out;
}

static float height_at(const FBuf* b, int x, int y, float surface_scale) {
    if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h) return 0.0f;
    return b->px[((size_t)y * (size_t)b->w + (size_t)x) * 4 + 3] * surface_scale;
}

static void light_vector(const FilterPrim* p, float px, float py, float scale,
                         float* L) {
    if (p->light_type == FE_LIGHT_POINT) {
        float lx = p->lx * scale, ly = p->ly * scale, lz = p->lz * scale;
        L[0] = lx - px;
        L[1] = ly - py;
        L[2] = lz;
    } else if (p->light_type == FE_LIGHT_SPOT) {
        float lx = p->lx * scale, ly = p->ly * scale, lz = p->lz * scale;
        L[0] = lx - px;
        L[1] = ly - py;
        L[2] = lz;
    } else {
        float az = p->azimuth * (float)SVG_PI / 180.0f;
        float el = p->elevation * (float)SVG_PI / 180.0f;
        L[0] = cosf(az) * cosf(el);
        L[1] = sinf(az) * cosf(el);
        L[2] = sinf(el);
    }
    float len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (len > 1e-6f) {
        L[0] /= len;
        L[1] /= len;
        L[2] /= len;
    } else {
        L[0] = 0.0f;
        L[1] = 0.0f;
        L[2] = 1.0f;
    }
}

static FBuf* fe_lighting(FilterCtx* fc, const FilterPrim* p) {
    const FBuf* in = fe_input(fc, p->in);
    if (!in) in = &fc->src;
    FBuf* out = fe_new(fc, in->w, in->h);
    if (!out) return NULL;
    float ss = fe_len(fc, p->surface_scale, 2);
    float kd = p->diffuse_constant;
    float ks = p->specular_constant;
    float se = p->specular_exponent;
    Color lc = p->light_color;
    if (fc->linear) lc = fe_tolinear(lc);
    float scale = mat_scale_factor(fc->world);
    if (!(scale > 1e-6f)) scale = 1.0f;
    int specular = (p->type == FE_SPECULAR_LIGHTING);

    for (int y = 0; y < out->h; y++) {
        for (int x = 0; x < out->w; x++) {

            float hl = height_at(in, x - 1, y, ss);
            float hr = height_at(in, x + 1, y, ss);
            float hu = height_at(in, x, y - 1, ss);
            float hd = height_at(in, x, y + 1, ss);
            float nx = (hl - hr);
            float ny = (hu - hd);
            float nz = 1.0f;
            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }

            float L[3];
            light_vector(p, (float)x, (float)y, scale, L);

            float ndl = nx * L[0] + ny * L[1] + nz * L[2];
            if (ndl < 0.0f) ndl = 0.0f;
            float atten = 1.0f;
            if (p->light_type == FE_LIGHT_SPOT) {
                float dx = p->points_at_x * scale - p->lx * scale;
                float dy = p->points_at_y * scale - p->ly * scale;
                float dz = p->points_at_z * scale - p->lz * scale;
                float dl = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dl > 1e-6f) {
                    dx /= dl; dy /= dl; dz /= dl;
                    float cosang = -(L[0] * dx + L[1] * dy + L[2] * dz);
                    if (p->limiting_cone_angle >= 0.0f) {
                        float lim = cosf(p->limiting_cone_angle * (float)SVG_PI
                                         / 180.0f);
                        if (cosang < lim) atten = 0.0f;
                        else {
                            float t = (cosang - lim) / (1.0f - lim + 1e-6f);
                            atten = powf(t < 0.0f ? 0.0f : t,
                                         p->specular_exponent);
                        }
                    } else {
                        atten = powf(cosang < 0.0f ? 0.0f : cosang,
                                     p->specular_exponent);
                    }
                }
            }

            float* d = out->px + ((size_t)y * (size_t)out->w + (size_t)x) * 4;
            if (!specular) {
                float k = kd * ndl * atten;
                d[0] = lc.r * k;
                d[1] = lc.g * k;
                d[2] = lc.b * k;
                d[3] = 1.0f;
            } else {

                float hx = L[0], hy = L[1], hz = L[2] + 1.0f;
                float hl2 = sqrtf(hx * hx + hy * hy + hz * hz);
                if (hl2 > 1e-6f) { hx /= hl2; hy /= hl2; hz /= hl2; }
                float ndh = nx * hx + ny * hy + nz * hz;
                if (ndh < 0.0f) ndh = 0.0f;

                float k = ks * powf(ndh, se) * atten;
                d[0] = lc.r * k;
                d[1] = lc.g * k;
                d[2] = lc.b * k;
                float a = d[0] > d[1] ? (d[0] > d[2] ? d[0] : d[2])
                                      : (d[1] > d[2] ? d[1] : d[2]);
                if (a > 1.0f) a = 1.0f;
                d[3] = a;
                if (a > 1e-6f) {
                    d[0] /= a;
                    d[1] /= a;
                    d[2] /= a;
                } else {
                    d[0] = d[1] = d[2] = 0.0f;
                }
            }
            for (int c = 0; c < 3; c++) {
                if (d[c] < 0.0f) d[c] = 0.0f;
                if (d[c] > 1.0f) d[c] = 1.0f;
            }
        }
    }
    return out;
}

FBuf* fe_run_generic(FilterCtx* fc, const FilterPrim* p) {
    if (!fc || fc->failed || !p) return NULL;
    switch (p->type) {
        case FE_TURBULENCE:         return fe_turbulence(fc, p);
        case FE_CONVOLVE_MATRIX:    return fe_convolve(fc, p);
        case FE_DISPLACEMENT_MAP:   return fe_displacement(fc, p);
        case FE_IMAGE:              return fe_image(fc, p);
        case FE_DIFFUSE_LIGHTING:
        case FE_SPECULAR_LIGHTING:  return fe_lighting(fc, p);
        default:                    return NULL;
    }
}
