#ifndef DYN_SVG_H
#define DYN_SVG_H

#include <stddef.h>
#include <stdint.h>

#define SVG2IMG_API

enum {
    SVG2IMG_OK = 0,
    SVG2IMG_ERR_FORMAT = 1,
    SVG2IMG_ERR_NOMEM = 2,
    SVG2IMG_ERR_ARG = 3,
    SVG2IMG_ERR_IO = 4,
    SVG2IMG_ERR_LIMIT = 5
};

typedef struct {
    int max_nodes;
    int max_depth;
    int max_path_points;
    int max_path_subpaths;
    int max_gradient_stops;
    int max_text_length;
    int max_clip_depth;
    int max_layers;
    int max_use_expansion;
    int max_pixels;
} svg2img_limits;

typedef struct {
    int width;
    int height;
    float scale;
    int aa;
    unsigned int background;
    int has_background;
    int verbose;
    svg2img_limits limits;
    const char* base_path;
    const char* font_path;
    int threads;
} svg2img_options;

typedef int (*svg2img_sink)(void* ud, const float* rgba, int w, int h);

void svg2img_default_limits(svg2img_limits* out);

int svg2img_rasterize(const char* svg_data, size_t len, const svg2img_options* opts,
    svg2img_sink sink, void* ud);

int dyn_svg_render(const char* svg_data, size_t len, const svg2img_options* opts,
    int png, uint8_t** out, size_t* out_len, int* w, int* h);

#endif
