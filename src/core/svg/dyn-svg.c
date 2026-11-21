#include "../dyn-svg.h"
#include "../dyn-compress.h"
#include "../dyn-hash.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int png;
    uint8_t* out;
    size_t out_len;
    int w, h;
} dyn_svg_out_t;

static inline uint8_t dyn_svg_to8(float v)
{
    float c = fminf(fmaxf(v, 0.0f), 1.0f);
    return (uint8_t)(c * 255.0f + 0.5f);
}

static void dyn_svg_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint8_t* dyn_svg_chunk(uint8_t* p, const char* type, const uint8_t* data, size_t len)
{
    uint32_t crc;
    dyn_svg_be32(p, (uint32_t)len);
    memcpy(p + 4, type, 4);
    if (len)
        memcpy(p + 8, data, len);
    crc = dyn_crc32_update(0, p + 4, len + 4);
    dyn_svg_be32(p + 8 + len, crc);
    return p + 12 + len;
}

static int dyn_svg_png(dyn_svg_out_t* o, const float* rgba, int w, int h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t stride = (size_t)w * 4, rawlen = (size_t)h * (stride + 1), zlen = 0, nidat, total, off;
    uint8_t *raw, *z = NULL, *png, *p, ihdr[13];
    uint8_t* prev;
    int y;
    size_t x;
    if (stride > SIZE_MAX / 2 || rawlen / (stride + 1) != (size_t)h)
        return SVG2IMG_ERR_LIMIT;
    raw = (uint8_t*)malloc(rawlen + stride);
    if (!raw)
        return SVG2IMG_ERR_NOMEM;
    prev = raw + rawlen;
    memset(prev, 0, stride);
    for (y = 0; y < h; y++) {
        uint8_t* row = raw + (size_t)y * (stride + 1);
        const float* src = rgba + (size_t)y * stride;
        row[0] = 2;
        for (x = 0; x < stride; x++) {
            uint8_t v = dyn_svg_to8(src[x]);
            row[1 + x] = (uint8_t)(v - prev[x]);
            prev[x] = v;
        }
    }
    if (dyn_zlib_deflate(raw, rawlen, 2, NULL, &z, &zlen) != 0) {
        free(raw);
        return SVG2IMG_ERR_NOMEM;
    }
    free(raw);
    nidat = zlen ? (zlen + 65534) / 65535 : 1;
    total = 8 + 25 + nidat * 12 + zlen + 12;
    png = (uint8_t*)malloc(total);
    if (!png) {
        free(z);
        return SVG2IMG_ERR_NOMEM;
    }
    memcpy(png, sig, 8);
    dyn_svg_be32(ihdr, (uint32_t)w);
    dyn_svg_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    p = dyn_svg_chunk(png + 8, "IHDR", ihdr, 13);
    off = 0;
    do {
        size_t n = zlen - off > 65535 ? 65535 : zlen - off;
        p = dyn_svg_chunk(p, "IDAT", z + off, n);
        off += n;
    } while (off < zlen);
    p = dyn_svg_chunk(p, "IEND", NULL, 0);
    free(z);
    o->out = png;
    o->out_len = (size_t)(p - png);
    return SVG2IMG_OK;
}

static int dyn_svg_sink(void* ud, const float* rgba, int w, int h)
{
    dyn_svg_out_t* o = (dyn_svg_out_t*)ud;
    size_t n = (size_t)w * (size_t)h * 4, i;
    o->w = w;
    o->h = h;
    if (o->png)
        return dyn_svg_png(o, rgba, w, h);
    o->out = (uint8_t*)malloc(n ? n : 1);
    if (!o->out)
        return SVG2IMG_ERR_NOMEM;
    for (i = 0; i < n; i++)
        o->out[i] = dyn_svg_to8(rgba[i]);
    o->out_len = n;
    return SVG2IMG_OK;
}

int dyn_svg_render(const char* svg_data, size_t len, const svg2img_options* opts,
    int png, uint8_t** out, size_t* out_len, int* w, int* h)
{
    dyn_svg_out_t o;
    int rc;
    memset(&o, 0, sizeof(o));
    o.png = png;
    rc = svg2img_rasterize(svg_data, len, opts, dyn_svg_sink, &o);
    if (rc != SVG2IMG_OK) {
        free(o.out);
        return rc;
    }
    *out = o.out;
    *out_len = o.out_len;
    *w = o.w;
    *h = o.h;
    return SVG2IMG_OK;
}
