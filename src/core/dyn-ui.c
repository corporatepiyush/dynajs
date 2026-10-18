#include "dyn-ui.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define UI_MAX_ELEMENTS (1u << 20)
#define UI_MAX_TEXT (1u << 26)
#define UI_MAX_DEPTH 256
#define UI_MAX_IDSTACK 256
#define UI_MAX_FONTS 64
#define UI_MAX_IMAGES 4096
#define UI_ATLAS_W 1024
#define UI_ATLAS_MAX_H 8192
#define UI_TYPED_MAX 4096
#define UI_MAX_COORD 16777216.0f
#define UI_MAX_POPUPS 32

typedef struct {
    void* p;
    size_t n, cap, esz;
} ui_vec;

static int ui_vec_reserve(ui_vec* v, size_t want)
{
    size_t cap;
    void* p;
    if (want <= v->cap)
        return 0;
    cap = v->cap ? v->cap : 16;
    while (cap < want)
        cap = cap < 4096 ? cap * 2 : cap + cap / 2;
    if (cap > (size_t)-1 / v->esz)
        return -1;
    p = realloc(v->p, cap * v->esz);
    if (!p)
        return -1;
    v->p = p;
    v->cap = cap;
    return 0;
}

static void* ui_vec_push(ui_vec* v)
{
    if (ui_vec_reserve(v, v->n + 1))
        return NULL;
    return (char*)v->p + v->n++ * v->esz;
}

static void ui_vec_free(ui_vec* v)
{
    free(v->p);
    v->p = NULL;
    v->n = v->cap = 0;
}

typedef struct {
    dyn_ui_id id;
    int32_t parent, first, last, next;
    dyn_ui_layout layout;
    dyn_ui_style style;
    uint32_t color;
    uint32_t text_off, text_len;
    uint32_t image;
    uint32_t tri_first, tri_count;
    float font_px, stroke;
    int16_t font;
    uint8_t kind;
    uint8_t wrap;
    uint8_t text_align;
    float fit_w, fit_h, min_w, min_h, text_w;
    dyn_ui_rect box;
    float content_w, content_h;
    int32_t line, run;
    uint32_t run_gen, line_first, line_count;
    uint8_t ellipsis;
} ui_el;

#define UI_EL_INPUT offsetof(ui_el, fit_w)

_Static_assert(UI_EL_INPUT % 8 == 0, "the declared part of ui_el is compared in 8-byte words");
_Static_assert(offsetof(dyn_ui_size, kind) == 0 && sizeof(dyn_ui_size) == offsetof(dyn_ui_size, value) + 3 * sizeof(float)
        && sizeof(dyn_ui_layout) == 2 * sizeof(dyn_ui_size) + 7 * sizeof(float) + sizeof(int32_t) + offsetof(dyn_ui_layout, z) - offsetof(dyn_ui_layout, dir),
    "ui_layout_sane zeroes exactly the padding of dyn_ui_layout");

typedef struct {
    float w, h, scale, insets[4];
    dyn_ui_id hot, thumb;
    uint32_t kind, scroll_gen, atlas_version, nfonts;
} ui_input;

enum { UI_EL_BOX,
    UI_EL_TEXT,
    UI_EL_IMAGE,
    UI_EL_TRI,
    UI_EL_LINE };

typedef struct {
    uint8_t kind;
    int key, mods;
    uint32_t cp;
} ui_event;

#define UI_MAX_EVENTS 4096

typedef struct {
    uint32_t off, len;
    float w;
} ui_line;

typedef struct {
    int32_t glyph;
    uint32_t cp;
    float kern;
} ui_rec;

typedef struct {
    uint64_t key;
    uint32_t bytes, nbytes, first, count;
    int font;
    uint32_t px_bits;
    float full, word;
} ui_run;

#define UI_RUN_MAX_RECS (1u << 18)

typedef struct {
    dyn_ui_id id;
    dyn_ui_rect box;
    dyn_ui_rect clip;
    uint32_t layer;
    dyn_ui_id root, owner;
    dyn_ui_rect track;
    uint8_t scroll;
} ui_hit;

typedef struct {
    dyn_ui_id id;
    dyn_ui_rect box;
} ui_named;

typedef struct {
    uint32_t pressed, released;
    int key_hit[DYN_UI_KEY_COUNT];
    char typed[UI_TYPED_MAX];
    size_t ntyped;
    float wheel_x, wheel_y;
} ui_frame_input;

typedef struct {
    dyn_ui_id id;
    uint64_t touched;
    size_t size;
    void* data;
    int layout;
} ui_slot;

typedef struct {
    int width, height;
    uint8_t* rgba;
    int used;
    uint32_t version;
} ui_image;

typedef struct {
    int16_t font;
    uint32_t px_bits, cp;
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float advance;
    int used;
} ui_glyph;

typedef struct {
    uint8_t* data;
    size_t len;
    int owned;
    stbtt_fontinfo info;
    int ascent, descent, line_gap;
    int kern_mode;
    int16_t* kern_ascii;
    int has_ellipsis;
} ui_font;

struct dyn_ui {
    int error;
    char error_text[160];
    uint64_t frame;
    float width, height;
    ui_vec els;
    ui_vec text;
    ui_vec lines;
    ui_vec tris;
    ui_vec cmds;
    ui_vec quads;
    ui_vec verts;
    ui_vec hits_prev, hits_cur, boxes_prev, boxes_cur;
    ui_vec floats;
    int32_t stack[UI_MAX_DEPTH];
    int depth;
    dyn_ui_id ids[UI_MAX_IDSTACK];
    int nids;
    uint32_t layers;
    ui_slot* slots;
    size_t nslots, cslots;
    ui_image images[UI_MAX_IMAGES];
    ui_font fonts[UI_MAX_FONTS];
    int nfonts;
    ui_glyph* glyphs;
    size_t nglyphs, cglyphs;
    struct {
        int font;
        uint32_t px_bits;
        uint32_t stamp;
        int32_t idx[128];
    } gcache[4];
    uint32_t gstamp;
    uint8_t* atlas;
    int atlas_h;
    uint32_t atlas_version, image_version;
    int atlas_full;
    int shelf_x, shelf_y, shelf_h;
    dyn_ui_pointer ptr, ptr_prev;
    uint32_t pressed, released;
    int key_down[DYN_UI_KEY_COUNT];
    int key_hit[DYN_UI_KEY_COUNT];
    int mods;
    char typed[UI_TYPED_MAX];
    size_t ntyped;
    ui_frame_input fin;
    dyn_ui_id hot, hot_root, active, focus, last_click;
    double last_click_time;
    ui_input last_in;
    dyn_ui_theme last_rt;
    uint32_t last_atlas, last_image;
    int frame_open;
    dyn_ui_theme theme;
    double now, wake;
    dyn_ui_clipboard_get clip_get;
    dyn_ui_clipboard_set clip_set;
    void* clip_ud;
    char* clip;
    size_t clip_n;
    ui_vec focus_prev, focus_cur;
    int focus_visible;
    dyn_ui_id popups[UI_MAX_POPUPS], prev_popups[UI_MAX_POPUPS], press_popups[UI_MAX_POPUPS];
    int npopups, nprev_popups, npress_popups;
    ui_vec ev_pending, ev_frame;
    uint32_t right_pressed;
    dyn_ui_id last_id, last_items, last_interact;
    uint32_t last_items_off;
    int last_cols_n;
    float last_cols[256];
    ui_vec dnd_src_prev, dnd_src_cur, dnd_tgt_prev, dnd_tgt_cur;
    dyn_ui_id dnd_src, dnd_over;
    int dnd, dnd_drop;
    struct uw_state* w;
    dyn_ui_theme rt;
    float scale;
    float insets[4];
    float press_x, press_y;
    double press_time;
    int long_fired, pressing;
    dyn_ui_id context_id, flags_id;
    int active_flags;
    dyn_ui_id drag_scroll;
    int drag_axes;
    float drag_sx, drag_sy;
    dyn_ui_id fling;
    float fling_vx, fling_vy;
    double fling_t;
    float vel_x, vel_y, last_x, last_y;
    double last_t;
    dyn_ui_id thumb;
    int thumb_axis;
    float thumb_grab;
    dyn_ui_rect thumb_track;
    float thumb_len;
    int editing;
    dyn_ui_rect edit_rect;
    char compose[256];
    size_t ncompose;
    int compose_cursor;
    ui_vec tverts, tidx, tbatch;
    struct {
        float x, y;
    } tpath[4][72];
    dyn_ui_rect tclips[UI_MAX_DEPTH];
    ui_vec runs, recs, rbytes;
    int32_t* rtab;
    size_t rcap;
    uint32_t run_gstamp, run_gen;
    uint64_t reused;
    ui_vec els_prev, text_prev, tris_prev;
    uint32_t scroll_gen;
    int have_input;
};

static void uw_free(dyn_ui_t* ui);

static void ui_fail(dyn_ui_t* ui, int code, const char* why)
{
    if (ui->error)
        return;
    ui->error = code;
    strncpy(ui->error_text, why, sizeof(ui->error_text) - 1);
    ui->error_text[sizeof(ui->error_text) - 1] = 0;
}

int dyn_ui_error(const dyn_ui_t* ui)
{
    return ui->error;
}

const char* dyn_ui_error_text(const dyn_ui_t* ui)
{
    return ui->error ? ui->error_text : "";
}

dyn_ui_id dyn_ui_hash(dyn_ui_id seed, const void* data, size_t n)
{
    const uint8_t* p = data;
    uint32_t h = seed ? seed : 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

dyn_ui_id dyn_ui_id_of(dyn_ui_t* ui, const char* label, size_t n)
{
    return dyn_ui_hash(ui->nids ? ui->ids[ui->nids - 1] : 0, label, n);
}

void dyn_ui_push_id(dyn_ui_t* ui, dyn_ui_id id)
{
    if (ui->nids >= UI_MAX_IDSTACK) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "pushId nests deeper than 256");
        return;
    }
    ui->ids[ui->nids] = dyn_ui_hash(ui->nids ? ui->ids[ui->nids - 1] : 0, &id, sizeof(id));
    ui->nids++;
}

void dyn_ui_pop_id(dyn_ui_t* ui)
{
    if (ui->nids > 0)
        ui->nids--;
    else
        ui_fail(ui, DYN_UI_ERR_NESTING, "popId without a matching pushId");
}

static size_t ui_slot_find(const dyn_ui_t* ui, dyn_ui_id id)
{
    size_t mask = ui->cslots - 1, i = (id * 2654435761u) & mask;
    while (ui->slots[i].id && ui->slots[i].id != id)
        i = (i + 1) & mask;
    return i;
}

static int ui_slots_grow(dyn_ui_t* ui)
{
    size_t old = ui->cslots, i, ncap = old ? old * 2 : 256;
    ui_slot* prev = ui->slots;
    ui_slot* s = calloc(ncap, sizeof(*s));
    if (!s)
        return -1;
    ui->slots = s;
    ui->cslots = ncap;
    for (i = 0; i < old; i++)
        if (prev[i].id)
            ui->slots[ui_slot_find(ui, prev[i].id)] = prev[i];
    free(prev);
    return 0;
}

void* dyn_ui_state(dyn_ui_t* ui, dyn_ui_id id, size_t size, int* fresh)
{
    size_t i;
    ui_slot* s;
    if (fresh)
        *fresh = 0;
    if (!id || size == 0 || size > (1u << 20))
        return NULL;
    if ((ui->nslots + 1) * 4 >= ui->cslots * 3 && ui_slots_grow(ui)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for widget state");
        return NULL;
    }
    i = ui_slot_find(ui, id);
    s = &ui->slots[i];
    if (s->id == id && s->size == size) {
        s->touched = ui->frame;
        return s->data;
    }
    if (s->id == id) {
        void* p = realloc(s->data, size);
        if (!p) {
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for widget state");
            return NULL;
        }
        memset(p, 0, size);
        s->data = p;
        s->size = size;
        s->touched = ui->frame;
        if (fresh)
            *fresh = 1;
        return p;
    }
    s->data = calloc(1, size);
    if (!s->data) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for widget state");
        return NULL;
    }
    s->id = id;
    s->size = size;
    s->touched = ui->frame;
    ui->nslots++;
    if (fresh)
        *fresh = 1;
    return s->data;
}

static void ui_slots_sweep(dyn_ui_t* ui)
{
    size_t i, old = ui->cslots;
    ui_slot* prev = ui->slots;
    size_t keep = 0;
    if (!old)
        return;
    for (i = 0; i < old; i++)
        if (prev[i].id && prev[i].touched + 2 >= ui->frame)
            keep++;
    if (keep == ui->nslots)
        return;
    ui->slots = calloc(old, sizeof(ui_slot));
    if (!ui->slots) {
        ui->slots = prev;
        return;
    }
    ui->nslots = 0;
    for (i = 0; i < old; i++) {
        if (!prev[i].id)
            continue;
        if (prev[i].touched + 2 >= ui->frame) {
            ui->slots[ui_slot_find(ui, prev[i].id)] = prev[i];
            ui->nslots++;
        } else {
            free(prev[i].data);
        }
    }
    free(prev);
}

int dyn_ui_image_add(dyn_ui_t* ui, int width, int height, const uint8_t* rgba)
{
    int i;
    size_t bytes;
    if (width < 1 || height < 1 || width > 16384 || height > 16384 || (int64_t)width * height > (1 << 26))
        return -1;
    for (i = 1; i < UI_MAX_IMAGES; i++)
        if (!ui->images[i].used)
            break;
    if (i == UI_MAX_IMAGES)
        return -1;
    bytes = (size_t)width * (size_t)height * 4;
    ui->images[i].rgba = malloc(bytes);
    if (!ui->images[i].rgba)
        return -1;
    if (rgba)
        memcpy(ui->images[i].rgba, rgba, bytes);
    else
        memset(ui->images[i].rgba, 0, bytes);
    ui->images[i].width = width;
    ui->images[i].height = height;
    ui->images[i].used = 1;
    ui->images[i].version = ++ui->image_version;
    return i;
}

int dyn_ui_image_update(dyn_ui_t* ui, uint32_t image, const uint8_t* rgba)
{
    if (image == 0 || image >= UI_MAX_IMAGES || !ui->images[image].used)
        return -1;
    memcpy(ui->images[image].rgba, rgba, (size_t)ui->images[image].width * (size_t)ui->images[image].height * 4);
    ui->images[image].version = ++ui->image_version;
    return 0;
}

void dyn_ui_image_remove(dyn_ui_t* ui, uint32_t image)
{
    if (image == 0 || image >= UI_MAX_IMAGES || !ui->images[image].used)
        return;
    free(ui->images[image].rgba);
    memset(&ui->images[image], 0, sizeof(ui->images[image]));
    ui->image_version++;
}

int dyn_ui_image_size(const dyn_ui_t* ui, uint32_t image, int* width, int* height)
{
    if (image == 0 || image >= UI_MAX_IMAGES || !ui->images[image].used)
        return -1;
    *width = ui->images[image].width;
    *height = ui->images[image].height;
    return 0;
}

uint32_t dyn_ui_image_version(const dyn_ui_t* ui, uint32_t image)
{
    if (image == 0 || image >= UI_MAX_IMAGES || !ui->images[image].used)
        return 0;
    return ui->images[image].version;
}

const uint8_t* dyn_ui_image_pixels(const dyn_ui_t* ui, uint32_t image, int* width, int* height)
{
    if (image == 0 || image >= UI_MAX_IMAGES || !ui->images[image].used)
        return NULL;
    *width = ui->images[image].width;
    *height = ui->images[image].height;
    return ui->images[image].rgba;
}

static uint32_t ui_rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int ui_font_tables_ok(const uint8_t* d, size_t n)
{
    uint32_t tag, i, ntab;
    if (n < 12)
        return 0;
    tag = ui_rd32(d);
    if (tag != 0x00010000u && tag != 0x74727565u && tag != 0x4F54544Fu)
        return 0;
    ntab = ((uint32_t)d[4] << 8) | d[5];
    if (ntab == 0 || 12 + (size_t)ntab * 16 > n)
        return 0;
    for (i = 0; i < ntab; i++) {
        const uint8_t* r = d + 12 + (size_t)i * 16;
        uint64_t off = ui_rd32(r + 8), len = ui_rd32(r + 12);
        if (off > n || len > n - off)
            return 0;
    }
    return 1;
}

static void ui_kern_summary(ui_font* f)
{
    int a, b, any = 0;
    int16_t* t;
    f->kern_mode = 0;
    f->kern_ascii = NULL;
    if (!f->info.kern && !f->info.gpos)
        return;
    t = calloc(95 * 95, sizeof(int16_t));
    if (!t) {
        f->kern_mode = 2;
        return;
    }
    for (a = 0; a < 95; a++) {
        int ga = stbtt_FindGlyphIndex(&f->info, a + 32);
        for (b = 0; b < 95; b++) {
            int k = ga ? stbtt_GetGlyphKernAdvance(&f->info, ga, stbtt_FindGlyphIndex(&f->info, b + 32)) : 0;
            t[a * 95 + b] = (int16_t)(k < -32768 ? -32768 : k > 32767 ? 32767
                                                                      : k);
            any |= k != 0;
        }
    }
    if (any) {
        f->kern_ascii = t;
        f->kern_mode = 1;
    } else {
        free(t);
        f->kern_mode = 3;
    }
}

int dyn_ui_font_add(dyn_ui_t* ui, const uint8_t* ttf, size_t len, int copy)
{
    ui_font* f;
    if (ui->nfonts >= UI_MAX_FONTS) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "more than 64 fonts");
        return -1;
    }
    if (len > (64u << 20) || !ui_font_tables_ok(ttf, len))
        return -1;
    f = &ui->fonts[ui->nfonts];
    memset(f, 0, sizeof(*f));
    if (copy) {
        f->data = malloc(len);
        if (!f->data)
            return -1;
        memcpy(f->data, ttf, len);
        f->owned = 1;
    } else {
        f->data = (uint8_t*)(uintptr_t)ttf;
    }
    f->len = len;
    if (!stbtt_InitFont(&f->info, f->data, 0)) {
        if (f->owned)
            free(f->data);
        memset(f, 0, sizeof(*f));
        return -1;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    ui_kern_summary(f);
    f->has_ellipsis = stbtt_FindGlyphIndex(&f->info, 0x2026) != 0;
    if (f->ascent - f->descent <= 0) {
        if (f->owned)
            free(f->data);
        free(f->kern_ascii);
        memset(f, 0, sizeof(*f));
        return -1;
    }
    return ui->nfonts++;
}

int dyn_ui_font_count(const dyn_ui_t* ui)
{
    return ui->nfonts;
}

static float ui_scale(const ui_font* f, float px)
{
    return px / (float)(f->ascent - f->descent);
}

float dyn_ui_line_height(dyn_ui_t* ui, int font, float px)
{
    const ui_font* f;
    if (font < 0 || font >= ui->nfonts)
        return 0;
    f = &ui->fonts[font];
    return ceilf((float)(f->ascent - f->descent + f->line_gap) * ui_scale(f, px));
}

float dyn_ui_ascent(dyn_ui_t* ui, int font, float px)
{
    const ui_font* f;
    if (font < 0 || font >= ui->nfonts)
        return 0;
    f = &ui->fonts[font];
    return roundf((float)f->ascent * ui_scale(f, px));
}

static uint32_t ui_utf8(const char* s, size_t n, size_t* i)
{
    const uint8_t* p = (const uint8_t*)s + *i;
    size_t left = n - *i;
    uint32_t c = p[0];
    if (c < 0x80) {
        *i += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && left >= 2 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *i += 2;
        return c >= 0x80 ? c : 0xFFFD;
    }
    if ((c & 0xF0) == 0xE0 && left >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *i += 3;
        return (c >= 0x800 && (c < 0xD800 || c > 0xDFFF)) ? c : 0xFFFD;
    }
    if ((c & 0xF8) == 0xF0 && left >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *i += 4;
        return (c >= 0x10000 && c <= 0x10FFFF) ? c : 0xFFFD;
    }
    *i += 1;
    return 0xFFFD;
}

static uint32_t ui_float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static size_t ui_glyph_slot(const dyn_ui_t* ui, int font, uint32_t px_bits, uint32_t cp)
{
    size_t mask = ui->cglyphs - 1;
    size_t i = ((size_t)cp * 2654435761u ^ (size_t)px_bits * 40503u ^ (size_t)font * 97u) & mask;
    while (ui->glyphs[i].used && !(ui->glyphs[i].font == font && ui->glyphs[i].px_bits == px_bits && ui->glyphs[i].cp == cp))
        i = (i + 1) & mask;
    return i;
}

static int ui_glyphs_grow(dyn_ui_t* ui)
{
    size_t old = ui->cglyphs, i, ncap = old ? old * 2 : 512;
    ui_glyph* prev = ui->glyphs;
    ui->glyphs = calloc(ncap, sizeof(ui_glyph));
    if (!ui->glyphs) {
        ui->glyphs = prev;
        return -1;
    }
    ui->gstamp++;
    ui->cglyphs = ncap;
    for (i = 0; i < old; i++)
        if (prev[i].used)
            ui->glyphs[ui_glyph_slot(ui, prev[i].font, prev[i].px_bits, prev[i].cp)] = prev[i];
    free(prev);
    return 0;
}

static int ui_atlas_alloc(dyn_ui_t* ui, int w, int h, int* ox, int* oy)
{
    if (w > UI_ATLAS_W - 2)
        return -1;
    if (ui->shelf_x + w + 1 > UI_ATLAS_W) {
        ui->shelf_y += ui->shelf_h + 1;
        ui->shelf_x = 1;
        ui->shelf_h = 0;
    }
    if (ui->shelf_y + h + 1 > ui->atlas_h) {
        int nh = ui->atlas_h ? ui->atlas_h : 128;
        uint8_t* p;
        while (ui->shelf_y + h + 1 > nh)
            nh *= 2;
        if (nh > UI_ATLAS_MAX_H)
            return -1;
        p = realloc(ui->atlas, (size_t)UI_ATLAS_W * (size_t)nh);
        if (!p)
            return -1;
        memset(p + (size_t)UI_ATLAS_W * (size_t)ui->atlas_h, 0, (size_t)UI_ATLAS_W * (size_t)(nh - ui->atlas_h));
        ui->atlas = p;
        ui->atlas_h = nh;
    }
    *ox = ui->shelf_x;
    *oy = ui->shelf_y;
    ui->shelf_x += w + 1;
    if (h > ui->shelf_h)
        ui->shelf_h = h;
    return 0;
}

static const ui_glyph* ui_glyph_slow(dyn_ui_t* ui, int font, float px, uint32_t cp);

static const ui_glyph* ui_glyph_get(dyn_ui_t* ui, int font, float px, uint32_t cp)
{
    uint32_t pb = ui_float_bits(px);
    int c = (int)((pb ^ (uint32_t)font * 0x9E3779B1u) >> 7) & 3;
    const ui_glyph* g;
    if (cp >= 128)
        return ui_glyph_slow(ui, font, px, cp);
    if (ui->gcache[c].stamp != ui->gstamp + 1 || ui->gcache[c].font != font || ui->gcache[c].px_bits != pb) {
        memset(ui->gcache[c].idx, 0xFF, sizeof(ui->gcache[c].idx));
        ui->gcache[c].font = font;
        ui->gcache[c].px_bits = pb;
        ui->gcache[c].stamp = ui->gstamp + 1;
    }
    if (ui->gcache[c].idx[cp] >= 0)
        return &ui->glyphs[ui->gcache[c].idx[cp]];
    g = ui_glyph_slow(ui, font, px, cp);
    if (g && ui->gcache[c].stamp == ui->gstamp + 1)
        ui->gcache[c].idx[cp] = (int32_t)(g - ui->glyphs);
    return g;
}

static const ui_glyph* ui_glyph_slow(dyn_ui_t* ui, int font, float px, uint32_t cp)
{
    uint32_t pb = ui_float_bits(px);
    ui_font* f = &ui->fonts[font];
    ui_glyph* g;
    size_t i;
    int x0, y0, x1, y1, adv, lsb, ax = 0, ay = 0, gi;
    float sc;
    if ((ui->nglyphs + 1) * 4 >= ui->cglyphs * 3 && ui_glyphs_grow(ui)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for the glyph cache");
        return NULL;
    }
    i = ui_glyph_slot(ui, font, pb, cp);
    g = &ui->glyphs[i];
    if (g->used)
        return g;
    sc = ui_scale(f, px);
    gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
    if (gi == 0 && cp != 0xFFFD && cp != '?')
        gi = stbtt_FindGlyphIndex(&f->info, '?');
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&f->info, gi, sc, sc, &x0, &y0, &x1, &y1);
    if (x1 - x0 > 0 && y1 - y0 > 0) {
        if (ui_atlas_alloc(ui, x1 - x0, y1 - y0, &ax, &ay)) {
            ui->atlas_full = 1;
            ui_fail(ui, DYN_UI_ERR_LIMIT, "the glyph atlas is full; it is rebuilt at the next begin()");
            return NULL;
        }
        stbtt_MakeGlyphBitmap(&f->info, ui->atlas + (size_t)ay * UI_ATLAS_W + (size_t)ax, x1 - x0, y1 - y0, UI_ATLAS_W, sc, sc, gi);
        ui->atlas_version++;
    }
    g->font = (int16_t)font;
    g->px_bits = pb;
    g->cp = cp;
    g->x0 = (float)x0;
    g->y0 = (float)y0;
    g->x1 = (float)x1;
    g->y1 = (float)y1;
    g->u0 = (float)ax;
    g->v0 = (float)ay;
    g->u1 = (float)(ax + (x1 - x0));
    g->v1 = (float)(ay + (y1 - y0));
    g->advance = (float)adv * sc;
    g->used = 1;
    ui->nglyphs++;
    return g;
}

static float ui_kern(dyn_ui_t* ui, int font, float px, uint32_t a, uint32_t b)
{
    ui_font* f = &ui->fonts[font];
    if (!a || f->kern_mode == 0)
        return 0;
    if (a >= 32 && a < 127 && b >= 32 && b < 127) {
        if (f->kern_mode != 1)
            return 0;
        return (float)f->kern_ascii[(a - 32) * 95 + (b - 32)] * ui_scale(f, px);
    }
    return (float)stbtt_GetCodepointKernAdvance(&f->info, (int)a, (int)b) * ui_scale(f, px);
}

float dyn_ui_text_width(dyn_ui_t* ui, int font, float px, const char* s, size_t n)
{
    float pen = 0;
    uint32_t prev = 0;
    size_t i = 0;
    if (font < 0 || font >= ui->nfonts)
        return 0;
    while (i < n) {
        uint32_t cp = ui_utf8(s, n, &i);
        const ui_glyph* g;
        if (cp == '\n')
            break;
        g = ui_glyph_get(ui, font, px, cp);
        if (!g)
            break;
        pen += ui_kern(ui, font, px, prev, cp) + g->advance;
        prev = cp;
    }
    return ceilf(pen);
}

dyn_ui_atlas_view dyn_ui_atlas(const dyn_ui_t* ui)
{
    dyn_ui_atlas_view v;
    v.width = UI_ATLAS_W;
    v.height = ui->atlas_h;
    v.pixels = ui->atlas;
    v.version = ui->atlas_version;
    return v;
}

dyn_ui_t* dyn_ui_new(void)
{
    dyn_ui_t* ui = calloc(1, sizeof(*ui));
    if (!ui)
        return NULL;
    ui->els.esz = sizeof(ui_el);
    ui->els_prev.esz = sizeof(ui_el);
    ui->text.esz = 1;
    ui->text_prev.esz = 1;
    ui->tris_prev.esz = sizeof(dyn_ui_vertex);
    ui->lines.esz = sizeof(ui_line);
    ui->tris.esz = sizeof(dyn_ui_vertex);
    ui->cmds.esz = sizeof(dyn_ui_cmd);
    ui->quads.esz = sizeof(dyn_ui_quad);
    ui->verts.esz = sizeof(dyn_ui_vertex);
    ui->hits_prev.esz = sizeof(ui_hit);
    ui->hits_cur.esz = sizeof(ui_hit);
    ui->boxes_prev.esz = sizeof(ui_named);
    ui->boxes_cur.esz = sizeof(ui_named);
    ui->dnd_src_prev.esz = ui->dnd_src_cur.esz = ui->dnd_tgt_prev.esz = ui->dnd_tgt_cur.esz = sizeof(dyn_ui_id);
    ui->floats.esz = sizeof(int32_t);
    ui->focus_prev.esz = sizeof(dyn_ui_id);
    ui->focus_cur.esz = sizeof(dyn_ui_id);
    ui->ev_pending.esz = sizeof(ui_event);
    ui->ev_frame.esz = sizeof(ui_event);
    ui->tverts.esz = sizeof(dyn_ui_gvertex);
    ui->tidx.esz = sizeof(uint32_t);
    ui->tbatch.esz = sizeof(dyn_ui_batch);
    ui->runs.esz = sizeof(ui_run);
    ui->recs.esz = sizeof(ui_rec);
    ui->rbytes.esz = 1;
    ui->wake = (double)INFINITY;
    ui->scale = 1;
    dyn_ui_theme_default(&ui->theme);
    ui->rt = ui->theme;
    ui->shelf_x = 1;
    ui->shelf_y = 1;
    ui->atlas = calloc((size_t)UI_ATLAS_W * 128, 1);
    if (ui->atlas) {
        ui->atlas[0] = 255;
        ui->atlas_h = 128;
    }
    if (!ui->atlas || ui_slots_grow(ui) || ui_glyphs_grow(ui)) {
        dyn_ui_free(ui);
        return NULL;
    }
    return ui;
}

void dyn_ui_free(dyn_ui_t* ui)
{
    size_t i;
    int k;
    if (!ui)
        return;
    ui_vec_free(&ui->els);
    ui_vec_free(&ui->els_prev);
    ui_vec_free(&ui->text);
    ui_vec_free(&ui->text_prev);
    ui_vec_free(&ui->tris_prev);
    ui_vec_free(&ui->lines);
    ui_vec_free(&ui->tris);
    ui_vec_free(&ui->cmds);
    ui_vec_free(&ui->quads);
    ui_vec_free(&ui->verts);
    ui_vec_free(&ui->hits_prev);
    ui_vec_free(&ui->hits_cur);
    ui_vec_free(&ui->boxes_prev);
    ui_vec_free(&ui->boxes_cur);
    ui_vec_free(&ui->dnd_src_prev);
    ui_vec_free(&ui->dnd_src_cur);
    ui_vec_free(&ui->dnd_tgt_prev);
    ui_vec_free(&ui->dnd_tgt_cur);
    ui_vec_free(&ui->floats);
    ui_vec_free(&ui->focus_prev);
    ui_vec_free(&ui->focus_cur);
    ui_vec_free(&ui->ev_pending);
    ui_vec_free(&ui->ev_frame);
    ui_vec_free(&ui->tverts);
    ui_vec_free(&ui->tidx);
    ui_vec_free(&ui->tbatch);
    ui_vec_free(&ui->runs);
    ui_vec_free(&ui->recs);
    ui_vec_free(&ui->rbytes);
    free(ui->rtab);
    uw_free(ui);
    free(ui->clip);
    for (i = 0; i < ui->cslots; i++)
        if (ui->slots[i].id)
            free(ui->slots[i].data);
    free(ui->slots);
    for (k = 0; k < UI_MAX_IMAGES; k++)
        if (ui->images[k].used)
            free(ui->images[k].rgba);
    for (k = 0; k < ui->nfonts; k++) {
        if (ui->fonts[k].owned)
            free(ui->fonts[k].data);
        free(ui->fonts[k].kern_ascii);
    }
    free(ui->glyphs);
    free(ui->atlas);
    free(ui);
}

void dyn_ui_input_pointer(dyn_ui_t* ui, const dyn_ui_pointer* p)
{
    uint32_t was = ui->ptr.buttons;
    if (!isfinite(p->x) || !isfinite(p->y))
        return;
    ui->pressed |= p->buttons & ~was;
    ui->released |= was & ~p->buttons;
    if ((was & 1u) && (p->buttons & 1u) && isfinite(p->time) && p->time > ui->last_t) {
        float dt = (float)(p->time - ui->last_t);
        ui->vel_x = ui->vel_x * 0.2f + (p->x - ui->last_x) / dt * 0.8f;
        ui->vel_y = ui->vel_y * 0.2f + (p->y - ui->last_y) / dt * 0.8f;
    } else if (!(was & 1u)) {
        ui->vel_x = ui->vel_y = 0;
    }
    ui->last_x = p->x;
    ui->last_y = p->y;
    if (isfinite(p->time))
        ui->last_t = p->time;
    ui->ptr.x = p->x;
    ui->ptr.y = p->y;
    ui->ptr.buttons = p->buttons;
    ui->ptr.kind = p->kind <= DYN_UI_POINTER_PEN ? p->kind : DYN_UI_POINTER_MOUSE;
    if (isfinite(p->wheel_x) && isfinite(p->wheel_y)) {
        ui->ptr.wheel_x += p->wheel_x;
        ui->ptr.wheel_y += p->wheel_y;
    }
    if (isfinite(p->time))
        ui->ptr.time = p->time;
}

void dyn_ui_input_key(dyn_ui_t* ui, int key, int down, int mods)
{
    ui->mods = mods;
    if (key <= 0 || key >= DYN_UI_KEY_COUNT)
        return;
    if (down) {
        ui_event* e = ui->ev_pending.n < UI_MAX_EVENTS ? ui_vec_push(&ui->ev_pending) : NULL;
        ui->key_hit[key]++;
        if (e) {
            e->kind = 1;
            e->key = key;
            e->mods = mods;
            e->cp = 0;
        }
    }
    ui->key_down[key] = down;
}

void dyn_ui_input_text(dyn_ui_t* ui, const char* utf8, size_t n)
{
    size_t room = UI_TYPED_MAX - 1 - ui->ntyped, i = 0;
    while (i < n && ui->ev_pending.n < UI_MAX_EVENTS) {
        uint32_t cp = ui_utf8(utf8, n, &i);
        ui_event* e = ui_vec_push(&ui->ev_pending);
        if (!e)
            break;
        e->kind = 2;
        e->key = 0;
        e->mods = ui->mods;
        e->cp = cp;
    }
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(ui->typed + ui->ntyped, utf8, n);
    ui->ntyped += n;
    ui->typed[ui->ntyped] = 0;
}

static int ui_inside(const dyn_ui_rect* r, float x, float y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static dyn_ui_rect ui_intersect(dyn_ui_rect a, dyn_ui_rect b)
{
    float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    dyn_ui_rect r;
    r.x = x0;
    r.y = y0;
    r.w = x1 > x0 ? x1 - x0 : 0;
    r.h = y1 > y0 ? y1 - y0 : 0;
    return r;
}

static ui_el* ui_el_at(dyn_ui_t* ui, int32_t i)
{
    return (ui_el*)ui->els.p + i;
}

static float* ui_scroll_state(dyn_ui_t* ui, dyn_ui_id id)
{
    dyn_ui_id k = id ^ 0x5C5C5C5Cu;
    float* s;
    if (!id)
        return NULL;
    k = k ? k : 0x5C5C5C5Du;
    s = dyn_ui_state(ui, k, 6 * sizeof(float), NULL);
    if (s)
        ui->slots[ui_slot_find(ui, k)].layout = 1;
    return s;
}

static void ui_atlas_reset(dyn_ui_t* ui)
{
    uint8_t* a = realloc(ui->atlas, (size_t)UI_ATLAS_W * 128);
    memset(ui->glyphs, 0, ui->cglyphs * sizeof(ui_glyph));
    ui->nglyphs = 0;
    ui->gstamp++;
    if (a) {
        ui->atlas = a;
        ui->atlas_h = 128;
    }
    memset(ui->atlas, 0, (size_t)UI_ATLAS_W * (size_t)ui->atlas_h);
    ui->atlas[0] = 255;
    ui->shelf_x = ui->shelf_y = 1;
    ui->shelf_h = 0;
    ui->atlas_version++;
    ui->atlas_full = 0;
}

static float ui_round_px(float v, float s)
{
    return roundf(v * s);
}

static void ui_resolve(dyn_ui_t* ui)
{
    const dyn_ui_theme* t = &ui->theme;
    dyn_ui_theme* r = &ui->rt;
    float s = ui->scale;
    *r = *t;
    r->font_px = t->font_px * s * (t->text_scale > 0 ? t->text_scale : 1);
    if (r->font_px < 1)
        r->font_px = 1;
    if (r->font_px > 512)
        r->font_px = 512;
    r->pad_x = ui_round_px(t->pad_x, s);
    r->pad_y = ui_round_px(t->pad_y, s);
    r->spacing = ui_round_px(t->spacing, s);
    r->window_pad = ui_round_px(t->window_pad, s);
    r->indent = ui_round_px(t->indent, s);
    r->scrollbar_w = t->scrollbar_w > 0 ? fmaxf(1, ui_round_px(t->scrollbar_w, s)) : 0;
    r->radius = t->radius * s;
    r->border_w = t->border_w > 0 ? fmaxf(1, ui_round_px(t->border_w, s)) : 0;
    r->label_w = ui_round_px(t->label_w, s);
    r->min_target = ui_round_px(t->min_target, s);
    r->drag_slop = t->drag_slop * s;
    if (t->font < 0 || t->font >= ui->nfonts)
        r->font = fabsf(r->font_px - 13) < 0.01f || ui->nfonts < 2 ? 0 : 1;
}

static int ui_dnd_is_src(const dyn_ui_t* ui, dyn_ui_id id)
{
    const dyn_ui_id* p = ui->dnd_src_prev.p;
    size_t i;
    if (!id)
        return 0;
    for (i = 0; i < ui->dnd_src_prev.n; i++)
        if (p[i] == id)
            return 1;
    return 0;
}

static void ui_dnd_start(dyn_ui_t* ui)
{
    ui->dnd = 1;
    ui->dnd_src = ui->active;
    ui->dnd_over = 0;
    ui->dnd_drop = 0;
}

static int ui_prev_box(const dyn_ui_t* ui, dyn_ui_id id, dyn_ui_rect* out)
{
    const ui_hit* h = ui->hits_prev.p;
    const ui_named* b = ui->boxes_prev.p;
    size_t i;
    for (i = 0; i < ui->hits_prev.n; i++)
        if (h[i].id == id && !h[i].scroll) {
            *out = ui_intersect(h[i].box, h[i].clip);
            return 1;
        }
    for (i = 0; i < ui->boxes_prev.n; i++)
        if (b[i].id == id) {
            *out = b[i].box;
            return 1;
        }
    return 0;
}

static void ui_dnd_track(dyn_ui_t* ui, const ui_frame_input* fi)
{
    const dyn_ui_id* tg = ui->dnd_tgt_prev.p;
    float best = INFINITY;
    size_t i;
    if (fi->key_hit[DYN_UI_KEY_ESCAPE] && ui->dnd == 1)
        ui->dnd = 2;
    ui->dnd_over = 0;
    if (ui->dnd != 1)
        return;
    for (i = 0; i < ui->dnd_tgt_prev.n; i++) {
        dyn_ui_rect r;
        float a;
        if (!ui_prev_box(ui, tg[i], &r) || !ui_inside(&r, ui->ptr.x, ui->ptr.y))
            continue;
        a = r.w * r.h;
        if (a < best) {
            best = a;
            ui->dnd_over = tg[i];
        }
    }
    if (fi->released & 1u)
        ui->dnd_drop = 1;
}

int dyn_ui_begin(dyn_ui_t* ui, float width, float height)
{
    ui_frame_input* fi;
    ui_hit* h;
    size_t i;
    ui_vec t;
    ui_el* root;
    if (ui->frame_open)
        return DYN_UI_ERR_NESTING;
    if (!(width >= 1 && width <= 16384) || !(height >= 1 && height <= 16384))
        return DYN_UI_ERR_LIMIT;
    ui->error = 0;
    ui->error_text[0] = 0;
    ui->frame++;
    ui->width = width;
    ui->height = height;
    t = ui->els_prev;
    ui->els_prev = ui->els;
    ui->els = t;
    t = ui->text_prev;
    ui->text_prev = ui->text;
    ui->text = t;
    t = ui->tris_prev;
    ui->tris_prev = ui->tris;
    ui->tris = t;
    ui->els.n = ui->text.n = ui->lines.n = ui->tris.n = 0;
    t = ui->hits_prev;
    ui->hits_prev = ui->hits_cur;
    ui->hits_cur = t;
    ui->hits_cur.n = 0;
    t = ui->boxes_prev;
    ui->boxes_prev = ui->boxes_cur;
    ui->boxes_cur = t;
    ui->boxes_cur.n = 0;
    t = ui->dnd_src_prev;
    ui->dnd_src_prev = ui->dnd_src_cur;
    ui->dnd_src_cur = t;
    ui->dnd_src_cur.n = 0;
    t = ui->dnd_tgt_prev;
    ui->dnd_tgt_prev = ui->dnd_tgt_cur;
    ui->dnd_tgt_cur = t;
    ui->dnd_tgt_cur.n = 0;
    ui->depth = 0;
    ui->nids = 0;
    ui_slots_sweep(ui);
    if (ui->atlas_full)
        ui_atlas_reset(ui);
    fi = &ui->fin;
    fi->pressed = ui->pressed;
    fi->released = ui->released;
    memcpy(fi->key_hit, ui->key_hit, sizeof(fi->key_hit));
    memcpy(fi->typed, ui->typed, ui->ntyped + 1);
    fi->ntyped = ui->ntyped;
    fi->wheel_x = ui->ptr.wheel_x;
    fi->wheel_y = ui->ptr.wheel_y;
    ui->pressed = ui->released = 0;
    memset(ui->key_hit, 0, sizeof(ui->key_hit));
    ui->ntyped = 0;
    ui->typed[0] = 0;
    ui->ptr.wheel_x = ui->ptr.wheel_y = 0;
    ui_resolve(ui);
    ui->wake = (double)INFINITY;
    ui->hot = 0;
    ui->hot_root = 0;
    ui->context_id = 0;
    ui->editing = 0;
    h = ui->hits_prev.p;
    {
        uint32_t layer = 0;
        int held = (ui->ptr.buttons & 1u) != 0, touch = ui->ptr.kind != DYN_UI_POINTER_MOUSE;
        int absent = touch && !held && !(fi->released & 1u) && !(fi->pressed & 1u);
        dyn_ui_id scroll_target = 0;
        float slop = ui->rt.drag_slop;
        for (i = 0; i < ui->hits_prev.n && !absent && !ui->drag_scroll; i++) {
            dyn_ui_rect r = ui_intersect(h[i].box, h[i].clip);
            if (h[i].scroll == 1 || !ui_inside(&r, ui->ptr.x, ui->ptr.y))
                continue;
            ui->hot = h[i].id;
            ui->hot_root = h[i].root;
            layer = h[i].layer;
        }
        if (fi->pressed & 1u) {
            ui->press_x = ui->ptr.x;
            ui->press_y = ui->ptr.y;
            ui->press_time = ui->now;
            ui->long_fired = 0;
            ui->drag_scroll = 0;
            ui->fling = 0;
            ui->thumb = 0;
            ui->pressing = 1;
            for (i = 0; i < ui->hits_prev.n; i++)
                if (h[i].scroll >= 2 && h[i].id == ui->hot) {
                    ui->thumb = h[i].owner;
                    ui->thumb_axis = h[i].scroll - 2;
                    ui->thumb_grab = ui->thumb_axis ? ui->ptr.y - h[i].box.y : ui->ptr.x - h[i].box.x;
                    ui->thumb_track = h[i].track;
                    ui->thumb_len = ui->thumb_axis ? h[i].box.h : h[i].box.w;
                }
        }
        if (held && ui->thumb) {
            float* s = ui_scroll_state(ui, ui->thumb);
            if (s) {
                int a = ui->thumb_axis;
                float span = (a ? ui->thumb_track.h : ui->thumb_track.w) - ui->thumb_len;
                float pos = (a ? ui->ptr.y - ui->thumb_track.y : ui->ptr.x - ui->thumb_track.x) - ui->thumb_grab;
                s[a] = span > 0 ? pos / span * (s[2 + a] - s[4 + a]) : 0;
                ui->scroll_gen++;
            }
        } else if (held && ui->pressing) {
            float dx = ui->ptr.x - ui->press_x, dy = ui->ptr.y - ui->press_y, dist = sqrtf(dx * dx + dy * dy);
            if (ui->dnd_src_prev.n && !ui->dnd && dist > slop && ui_dnd_is_src(ui, ui->active) && !touch)
                ui_dnd_start(ui);
            if (!ui->drag_scroll && !ui->dnd && touch && dist > slop) {
                int ax = fabsf(dx) >= fabsf(dy) ? DYN_UI_DRAG_X : DYN_UI_DRAG_Y;
                int consumed = ui->active && ui->flags_id == ui->active ? ui->active_flags : 0;
                if (!(consumed & ax)) {
                    dyn_ui_id target = 0;
                    for (i = 0; i < ui->hits_prev.n; i++) {
                        dyn_ui_rect r = ui_intersect(h[i].box, h[i].clip);
                        float* s;
                        if (h[i].scroll != 1 || !ui_inside(&r, ui->press_x, ui->press_y))
                            continue;
                        s = ui_scroll_state(ui, h[i].id);
                        if (s && (ax == DYN_UI_DRAG_X ? s[2] > s[4] : s[3] > s[5]))
                            target = h[i].id;
                    }
                    if (!target && ui->dnd_src_prev.n && ui_dnd_is_src(ui, ui->active))
                        ui_dnd_start(ui);
                    else if (target) {
                        float* s = ui_scroll_state(ui, target);
                        ui->drag_scroll = target;
                        ui->drag_axes = (s[2] > s[4] ? 1 : 0) | (s[3] > s[5] ? 2 : 0);
                        ui->drag_sx = s[0];
                        ui->drag_sy = s[1];
                        ui->active = 0;
                        ui->long_fired = 1;
                        ui->hot = 0;
                    }
                }
            }
            if (ui->drag_scroll) {
                float* s = ui_scroll_state(ui, ui->drag_scroll);
                if (s) {
                    ui->scroll_gen++;
                    if (ui->drag_axes & 1)
                        s[0] = ui->drag_sx - dx;
                    if (ui->drag_axes & 2)
                        s[1] = ui->drag_sy - dy;
                }
            } else if (touch && !ui->long_fired && !ui->dnd && dist <= slop) {
                if (ui->now - ui->press_time >= (double)ui->rt.long_press) {
                    ui->long_fired = 1;
                    if (ui->dnd_src_prev.n && ui_dnd_is_src(ui, ui->active) && ui->active == ui->hot)
                        ui_dnd_start(ui);
                    else {
                        ui->context_id = ui->hot;
                        ui->active = 0;
                    }
                } else
                    dyn_ui_wake_at(ui, ui->press_time + (double)ui->rt.long_press);
            }
        }
        if (ui->dnd)
            ui_dnd_track(ui, fi);
        if (fi->released & 1u) {
            if (ui->drag_scroll) {
                float vx = (ui->drag_axes & 1) ? -ui->vel_x : 0, vy = (ui->drag_axes & 2) ? -ui->vel_y : 0;
                if (sqrtf(vx * vx + vy * vy) > 100 * ui->scale) {
                    ui->fling = ui->drag_scroll;
                    ui->fling_vx = vx;
                    ui->fling_vy = vy;
                    ui->fling_t = ui->now;
                }
                ui->drag_scroll = 0;
            }
            ui->thumb = 0;
            ui->pressing = 0;
        }
        if (ui->fling && !held) {
            float* s = ui_scroll_state(ui, ui->fling);
            double dt = ui->now - ui->fling_t;
            if (!s)
                ui->fling = 0;
            else if (dt > 0) {
                float d = (float)exp(-(dt > 0.25 ? 0.25 : dt) / 0.325);
                int a;
                ui->fling_t = ui->now;
                if (dt > 0.25)
                    dt = 0.25;
                ui->scroll_gen++;
                s[0] += ui->fling_vx * (float)dt;
                s[1] += ui->fling_vy * (float)dt;
                ui->fling_vx *= d;
                ui->fling_vy *= d;
                for (a = 0; a < 2; a++) {
                    float mx = s[2 + a] - s[4 + a];
                    if (mx < 0)
                        mx = 0;
                    if (s[a] < 0 || s[a] > mx) {
                        s[a] = s[a] < 0 ? 0 : mx;
                        if (a)
                            ui->fling_vy = 0;
                        else
                            ui->fling_vx = 0;
                    }
                }
                if (sqrtf(ui->fling_vx * ui->fling_vx + ui->fling_vy * ui->fling_vy) < 20 * ui->scale)
                    ui->fling = 0;
            }
            if (ui->fling)
                dyn_ui_wake_at(ui, ui->now + 1.0 / 120);
        }
        if ((fi->pressed & 2u) && !touch)
            ui->context_id = ui->hot;
        if (fi->wheel_x != 0 || fi->wheel_y != 0) {
            float step = 3 * dyn_ui_line_height(ui, ui->rt.font, ui->rt.font_px);
            if (step <= 0)
                step = 24 * ui->scale;
            for (i = 0; i < ui->hits_prev.n; i++) {
                dyn_ui_rect r = ui_intersect(h[i].box, h[i].clip);
                float* s;
                if (h[i].scroll != 1 || h[i].layer != layer || !ui_inside(&r, ui->ptr.x, ui->ptr.y))
                    continue;
                s = ui_scroll_state(ui, h[i].id);
                if (s && ((fi->wheel_y != 0 && s[3] > s[5]) || (fi->wheel_x != 0 && s[2] > s[4])))
                    scroll_target = h[i].id;
            }
            if (scroll_target) {
                float* s = ui_scroll_state(ui, scroll_target);
                if (s) {
                    ui->scroll_gen++;
                    s[0] -= fi->wheel_x * step;
                    s[1] -= fi->wheel_y * step;
                }
            }
        }
    }
    if (ui->active && !(ui->ptr.buttons & 1u) && !(fi->released & 1u))
        ui->active = 0;
    if ((fi->pressed & 1u) && !ui->hot)
        ui->focus = 0;
    memcpy(ui->prev_popups, ui->popups, sizeof(ui->popups));
    ui->nprev_popups = ui->npopups;
    {
        ui_vec ev = ui->ev_frame;
        ui->ev_frame = ui->ev_pending;
        ui->ev_pending = ev;
        ui->ev_pending.n = 0;
    }
    if (fi->pressed & 7u) {
        int k = ui->hot_root ? dyn_ui_popup_level(ui, ui->hot_root) : -1;
        memcpy(ui->press_popups, ui->popups, sizeof(ui->popups));
        ui->npress_popups = ui->npopups;
        ui->npopups = k + 1;
        ui->focus_visible = 0;
    }
    if (fi->key_hit[DYN_UI_KEY_ESCAPE] && ui->npopups > 0)
        ui->npopups--;
    if (fi->key_hit[DYN_UI_KEY_TAB] && ui->focus_prev.n > 0) {
        const dyn_ui_id* fo = ui->focus_prev.p;
        size_t k, n = ui->focus_prev.n, at = n;
        for (k = 0; k < n; k++)
            if (fo[k] == ui->focus)
                at = k;
        if (ui->mods & DYN_UI_MOD_SHIFT)
            at = at == n || at == 0 ? n - 1 : at - 1;
        else
            at = at == n ? 0 : (at + 1) % n;
        ui->focus = fo[at];
        ui->focus_visible = 1;
    }
    {
        ui_vec fv = ui->focus_prev;
        ui->focus_prev = ui->focus_cur;
        ui->focus_cur = fv;
        ui->focus_cur.n = 0;
    }
    ui->right_pressed = fi->pressed & 2u;
    ui->last_id = 0;
    ui->last_items = 0;
    ui->last_cols_n = 0;
    ui->last_interact = 0;
    root = ui_vec_push(&ui->els);
    if (!root)
        return DYN_UI_ERR_NOMEM;
    memset(root, 0, sizeof(*root));
    root->parent = root->first = root->last = root->next = -1;
    root->kind = UI_EL_BOX;
    root->layout.w.kind = DYN_UI_FIXED;
    root->layout.w.value = width;
    root->layout.h.kind = DYN_UI_FIXED;
    root->layout.h.value = height;
    root->layout.dir = DYN_UI_COLUMN;
    root->font = -1;
    ui->stack[0] = 0;
    ui->depth = 1;
    ui->frame_open = 1;
    return DYN_UI_OK;
}

uint64_t dyn_ui_frame(const dyn_ui_t* ui)
{
    return ui->frame;
}

int dyn_ui_depth(const dyn_ui_t* ui)
{
    return ui->depth;
}

static ui_el* ui_append(dyn_ui_t* ui, dyn_ui_id id, int kind)
{
    ui_el* e;
    ui_el* parent;
    int32_t idx;
    if (!ui->frame_open) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "an element outside begin()/end()");
        return NULL;
    }
    if (ui->els.n >= UI_MAX_ELEMENTS) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "more than 2^20 elements in one frame");
        return NULL;
    }
    e = ui_vec_push(&ui->els);
    if (!e) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for elements");
        return NULL;
    }
    idx = (int32_t)(ui->els.n - 1);
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->kind = (uint8_t)kind;
    e->font = -1;
    e->first = e->last = e->next = -1;
    e->parent = ui->stack[ui->depth - 1];
    parent = ui_el_at(ui, e->parent);
    if (parent->last >= 0)
        ui_el_at(ui, parent->last)->next = idx;
    else
        parent->first = idx;
    parent->last = idx;
    return ui_el_at(ui, idx);
}

static float ui_finite(float v, float lo, float hi)
{
    return v >= lo && v <= hi ? v : v > hi ? hi
                                           : lo;
}

static void ui_size_sane(dyn_ui_size* s)
{
    if (s->kind > DYN_UI_PERCENT)
        s->kind = DYN_UI_FIT;
    s->value = ui_finite(s->value, 0, s->kind == DYN_UI_PERCENT ? 100.0f : UI_MAX_COORD);
    s->min = ui_finite(s->min, 0, UI_MAX_COORD);
    s->max = ui_finite(s->max, 0, UI_MAX_COORD);
}

static void ui_layout_sane(dyn_ui_layout* l)
{
    memset((char*)&l->w + 1, 0, offsetof(dyn_ui_size, value) - 1);
    memset((char*)&l->h + 1, 0, offsetof(dyn_ui_size, value) - 1);
    memset((char*)l + offsetof(dyn_ui_layout, wrap) + 1, 0, offsetof(dyn_ui_layout, z) - offsetof(dyn_ui_layout, wrap) - 1);
    ui_size_sane(&l->w);
    ui_size_sane(&l->h);
    l->pad_l = ui_finite(l->pad_l, 0, UI_MAX_COORD);
    l->pad_t = ui_finite(l->pad_t, 0, UI_MAX_COORD);
    l->pad_r = ui_finite(l->pad_r, 0, UI_MAX_COORD);
    l->pad_b = ui_finite(l->pad_b, 0, UI_MAX_COORD);
    l->gap = ui_finite(l->gap, 0, UI_MAX_COORD);
    l->float_x = ui_finite(l->float_x, -UI_MAX_COORD, UI_MAX_COORD);
    l->float_y = ui_finite(l->float_y, -UI_MAX_COORD, UI_MAX_COORD);
    if (l->dir > DYN_UI_COLUMN)
        l->dir = DYN_UI_ROW;
    if (l->align_x > DYN_UI_END)
        l->align_x = DYN_UI_START;
    if (l->align_y > DYN_UI_END)
        l->align_y = DYN_UI_START;
    if (l->floating > DYN_UI_FLOAT_SAFE)
        l->floating = DYN_UI_FLOAT_NONE;
    if (l->anchor > DYN_UI_ANCHOR_TOP_RIGHT)
        l->anchor = DYN_UI_ANCHOR_TOP_LEFT;
}

int dyn_ui_open(dyn_ui_t* ui, dyn_ui_id id, const dyn_ui_layout* layout, const dyn_ui_style* style)
{
    ui_el* e;
    if (ui->depth >= UI_MAX_DEPTH) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "elements nest deeper than 256");
        return ui->error;
    }
    e = ui_append(ui, id, UI_EL_BOX);
    if (!e)
        return ui->error;
    if (layout)
        e->layout = *layout;
    if (style)
        e->style = *style;
    ui_layout_sane(&e->layout);
    e->style.border_w = ui_finite(e->style.border_w, 0, 1e4f);
    e->style.radius = ui_finite(e->style.radius, 0, UI_MAX_COORD);
    ui->stack[ui->depth++] = (int32_t)(ui->els.n - 1);
    return DYN_UI_OK;
}

int dyn_ui_close(dyn_ui_t* ui)
{
    if (!ui->frame_open || ui->depth <= 1) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "close() without a matching open()");
        return ui->error;
    }
    ui->depth--;
    return DYN_UI_OK;
}

int dyn_ui_text(dyn_ui_t* ui, dyn_ui_id id, const char* s, size_t n, int font, float px, uint32_t color, int wrap, uint8_t align)
{
    ui_el* e;
    char* dst;
    if (font < 0 || font >= ui->nfonts || !(px >= 1 && px <= 512)) {
        ui_fail(ui, DYN_UI_ERR_FONT, "text needs a loaded font and a size in 1..512");
        return ui->error;
    }
    if (ui->text.n + n > UI_MAX_TEXT) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "more than 64 MiB of text in one frame");
        return ui->error;
    }
    e = ui_append(ui, id, UI_EL_TEXT);
    if (!e)
        return ui->error;
    if (ui_vec_reserve(&ui->text, ui->text.n + n)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for text");
        return ui->error;
    }
    dst = (char*)ui->text.p + ui->text.n;
    if (n)
        memcpy(dst, s, n);
    e->text_off = (uint32_t)ui->text.n;
    e->text_len = (uint32_t)n;
    ui->text.n += n;
    e->font = (int16_t)font;
    e->font_px = px;
    e->color = color;
    e->wrap = (uint8_t)(wrap == DYN_UI_TEXT_ELLIPSIS ? 2 : wrap ? 1
                                                                : 0);
    e->text_align = align;
    e->layout.w.kind = DYN_UI_FIT;
    e->layout.h.kind = DYN_UI_FIT;
    return DYN_UI_OK;
}

int dyn_ui_image(dyn_ui_t* ui, dyn_ui_id id, uint32_t image, const dyn_ui_layout* layout, uint32_t tint)
{
    ui_el* e;
    int w, h;
    if (dyn_ui_image_size(ui, image, &w, &h)) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "an image handle that is not registered");
        return ui->error;
    }
    e = ui_append(ui, id, UI_EL_IMAGE);
    if (!e)
        return ui->error;
    if (layout)
        e->layout = *layout;
    ui_layout_sane(&e->layout);
    e->image = image;
    e->color = tint;
    return DYN_UI_OK;
}

int dyn_ui_triangles(dyn_ui_t* ui, const dyn_ui_vertex* v, uint32_t count, uint32_t color)
{
    ui_el* e;
    uint32_t i;
    if (count % 3 || count > (1u << 20)) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "triangles need a multiple of 3 vertices, at most 2^20");
        return ui->error;
    }
    if (ui->depth < 2) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "triangles are drawn inside an open element");
        return ui->error;
    }
    if (ui_vec_reserve(&ui->tris, ui->tris.n + count)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for triangles");
        return ui->error;
    }
    e = ui_append(ui, 0, UI_EL_TRI);
    if (!e)
        return ui->error;
    e->tri_first = (uint32_t)ui->tris.n;
    e->tri_count = count;
    e->color = color;
    e->layout.w.kind = DYN_UI_GROW;
    e->layout.w.value = 1;
    e->layout.h.kind = DYN_UI_GROW;
    e->layout.h.value = 1;
    e->layout.floating = DYN_UI_FLOAT_PARENT;
    for (i = 0; i < count; i++) {
        dyn_ui_vertex* d = (dyn_ui_vertex*)ui->tris.p + ui->tris.n++;
        d->x = ui_finite(v[i].x, -UI_MAX_COORD, UI_MAX_COORD);
        d->y = ui_finite(v[i].y, -UI_MAX_COORD, UI_MAX_COORD);
    }
    return DYN_UI_OK;
}

static dyn_ui_vertex* ui_polyline_points(dyn_ui_t* ui, uint32_t count, uint32_t color, float thickness)
{
    ui_el* e;
    dyn_ui_vertex* d;
    if (count > (1u << 20)) {
        ui_fail(ui, DYN_UI_ERR_LIMIT, "a polyline has at most 2^20 points");
        return NULL;
    }
    if (ui->depth < 2) {
        ui_fail(ui, DYN_UI_ERR_NESTING, "a polyline is drawn inside an open element");
        return NULL;
    }
    if (ui_vec_reserve(&ui->tris, ui->tris.n + count)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for a polyline");
        return NULL;
    }
    e = ui_append(ui, 0, UI_EL_LINE);
    if (!e)
        return NULL;
    e->tri_first = (uint32_t)ui->tris.n;
    e->tri_count = count;
    e->color = color;
    e->stroke = ui_finite(thickness, 0, 64);
    e->layout.floating = DYN_UI_FLOAT_PARENT;
    d = (dyn_ui_vertex*)ui->tris.p + ui->tris.n;
    ui->tris.n += count;
    return d;
}

int dyn_ui_polyline(dyn_ui_t* ui, const dyn_ui_vertex* norm, uint32_t count, uint32_t color, float thickness)
{
    dyn_ui_vertex* d = ui_polyline_points(ui, count, color, thickness);
    uint32_t i;
    if (!d)
        return ui->error;
    for (i = 0; i < count; i++) {
        d[i].x = ui_finite(norm[i].x, 0, 1);
        d[i].y = ui_finite(norm[i].y, 0, 1);
    }
    return DYN_UI_OK;
}

static int ui_flow(const ui_el* e)
{
    return e->layout.floating == DYN_UI_FLOAT_NONE && e->kind != UI_EL_TRI && e->kind != UI_EL_LINE;
}

static int ui_clips(const ui_el* e, int axis)
{
    return e->kind == UI_EL_BOX && (axis ? e->layout.clip_y : e->layout.clip_x);
}

static float ui_clamp(float v, const dyn_ui_size* s)
{
    if (s->max > 0 && v > s->max)
        v = s->max;
    if (s->min > 0 && v < s->min)
        v = s->min;
    return v < 0 ? 0 : v;
}

static float ui_align(uint8_t a)
{
    return a == DYN_UI_CENTER ? 0.5f : a == DYN_UI_END ? 1.0f
                                                       : 0.0f;
}

static void ui_runs_clear(dyn_ui_t* ui)
{
    ui->runs.n = ui->recs.n = ui->rbytes.n = 0;
    ui->run_gen++;
    if (ui->rtab)
        memset(ui->rtab, 0xFF, ui->rcap * sizeof(int32_t));
    ui->run_gstamp = ui->gstamp;
}

static int ui_runs_rehash(dyn_ui_t* ui, size_t ncap)
{
    int32_t* t = malloc(ncap * sizeof(int32_t));
    size_t k;
    if (!t)
        return -1;
    memset(t, 0xFF, ncap * sizeof(int32_t));
    for (k = 0; k < ui->runs.n; k++) {
        size_t j = (size_t)((ui_run*)ui->runs.p)[k].key & (ncap - 1);
        while (t[j] >= 0)
            j = (j + 1) & (ncap - 1);
        t[j] = (int32_t)k;
    }
    free(ui->rtab);
    ui->rtab = t;
    ui->rcap = ncap;
    return 0;
}

static uint64_t ui_hash64(uint64_t h, const void* p, size_t n);

static const ui_run* ui_run_get(dyn_ui_t* ui, int font, float px, const char* s, size_t n)
{
    uint32_t pb = ui_float_bits(px);
    uint64_t key = ui_hash64(0x51A7E5ull ^ ((uint64_t)pb << 20) ^ (uint64_t)(unsigned)font, s, n) | 1;
    size_t j, i = 0, first;
    ui_run* r;
    float word = 0, widest = 0, wpen = 0, lpen = 0;
    uint32_t prev = 0;
    if (ui->run_gstamp != ui->gstamp || ui->recs.n + n > UI_RUN_MAX_RECS)
        ui_runs_clear(ui);
    if ((ui->runs.n + 1) * 2 > ui->rcap && ui_runs_rehash(ui, ui->rcap ? ui->rcap * 2 : 256))
        return NULL;
    j = (size_t)key & (ui->rcap - 1);
    while (ui->rtab[j] >= 0) {
        r = (ui_run*)ui->runs.p + ui->rtab[j];
        if (r->key == key && r->font == font && r->px_bits == pb && r->nbytes == n && !memcmp((const char*)ui->rbytes.p + r->bytes, s, n))
            return r;
        j = (j + 1) & (ui->rcap - 1);
    }
    if (ui_vec_reserve(&ui->rbytes, ui->rbytes.n + n) || ui_vec_reserve(&ui->recs, ui->recs.n + n) || ui_vec_reserve(&ui->runs, ui->runs.n + 1))
        return NULL;
    first = ui->recs.n;
    while (i < n) {
        uint32_t cp = ui_utf8(s, n, &i);
        ui_rec* rec = (ui_rec*)ui->recs.p + ui->recs.n++;
        const ui_glyph* g;
        float adv;
        rec->cp = cp;
        rec->kern = 0;
        rec->glyph = -1;
        if (cp == '\n') {
            if (wpen > word)
                word = wpen;
            if (lpen > widest)
                widest = lpen;
            wpen = lpen = 0;
            prev = 0;
            continue;
        }
        g = ui_glyph_get(ui, font, px, cp);
        if (ui->run_gstamp != ui->gstamp) {
            ui_runs_clear(ui);
            return NULL;
        }
        rec->kern = ui_kern(ui, font, px, prev, cp);
        if (!g) {
            prev = cp;
            continue;
        }
        rec->glyph = (int32_t)(g - ui->glyphs);
        adv = rec->kern + g->advance;
        prev = cp;
        lpen += adv;
        if (cp == ' ') {
            if (wpen > word)
                word = wpen;
            wpen = 0;
        } else
            wpen += adv;
    }
    if (wpen > word)
        word = wpen;
    if (lpen > widest)
        widest = lpen;
    r = (ui_run*)ui->runs.p + ui->runs.n;
    r->key = key;
    r->font = font;
    r->px_bits = pb;
    r->bytes = (uint32_t)ui->rbytes.n;
    r->nbytes = (uint32_t)n;
    r->first = (uint32_t)first;
    r->count = (uint32_t)(ui->recs.n - first);
    r->full = ceilf(widest);
    r->word = ceilf(word);
    memcpy((char*)ui->rbytes.p + ui->rbytes.n, s, n);
    ui->rbytes.n += n;
    ui->rtab[j] = (int32_t)ui->runs.n++;
    return r;
}

static const char* ui_ellipsis_str(dyn_ui_t* ui, int font, size_t* n)
{
    if (font >= 0 && font < ui->nfonts && ui->fonts[font].has_ellipsis) {
        *n = 3;
        return "\xE2\x80\xA6";
    }
    *n = 3;
    return "...";
}

static float ui_ellipsis_w(dyn_ui_t* ui, const ui_el* e)
{
    size_t n;
    const char* s = ui_ellipsis_str(ui, e->font, &n);
    const ui_run* r = ui_run_get(ui, e->font, e->font_px, s, n);
    return r ? r->full : dyn_ui_text_width(ui, e->font, e->font_px, s, n);
}

static const ui_run* ui_el_run(dyn_ui_t* ui, ui_el* e)
{
    const ui_run* r;
    if (e->run >= 0 && e->run_gen == ui->run_gen && ui->run_gstamp == ui->gstamp && (size_t)e->run < ui->runs.n)
        return (const ui_run*)ui->runs.p + e->run;
    r = ui_run_get(ui, e->font, e->font_px, (const char*)ui->text.p + e->text_off, e->text_len);
    if (!r) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for text runs");
        e->run = -1;
        return NULL;
    }
    e->run = (int32_t)(r - (const ui_run*)ui->runs.p);
    e->run_gen = ui->run_gen;
    return r;
}

static float ui_word_widths(dyn_ui_t* ui, ui_el* e, float* full)
{
    const ui_run* r;
    e->run = -1;
    r = ui_el_run(ui, e);
    *full = r ? r->full : 0;
    return r ? r->word : 0;
}

static int ui_push_line(dyn_ui_t* ui, uint32_t off, uint32_t len, float w)
{
    ui_line* l = ui_vec_push(&ui->lines);
    if (!l) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for text lines");
        return -1;
    }
    l->off = off;
    l->len = len;
    l->w = w;
    return 0;
}

static void ui_wrap(dyn_ui_t* ui, ui_el* e, float max_w)
{
    const ui_run* r;
    const ui_rec* rc;
    size_t i, end, line = 0, brk = (size_t)-1;
    float pen = 0, pen_at_brk = 0, ell = 0;
    int fresh = 1;
    e->line_first = (uint32_t)ui->lines.n;
    e->line_count = 0;
    e->ellipsis = 0;
    if (e->wrap == 2) {
        ell = ui_ellipsis_w(ui, e);
        if (e->text_w <= max_w + 0.01f)
            e->wrap = 3;
    }
    r = ui_el_run(ui, e);
    if (!r)
        return;
    rc = (const ui_rec*)ui->recs.p + r->first;
    end = r->count;
    for (i = 0; i < end; i++) {
        uint32_t cp = rc[i].cp;
        const ui_glyph* g = rc[i].glyph >= 0 ? &ui->glyphs[rc[i].glyph] : NULL;
        float adv;
        if (cp == '\n' && e->wrap >= 2) {
            if (ui_push_line(ui, (uint32_t)line, (uint32_t)(i - line), ceilf(pen + (e->wrap == 2 ? ell : 0))))
                return;
            e->ellipsis = e->wrap == 2;
            if (e->wrap == 3)
                e->wrap = 2;
            e->line_count = 1;
            return;
        }
        if (cp == '\n') {
            if (ui_push_line(ui, (uint32_t)line, (uint32_t)(i - line), ceilf(pen)))
                return;
            line = i + 1;
            pen = 0;
            fresh = 1;
            brk = (size_t)-1;
            continue;
        }
        adv = (g ? g->advance : 0) + (fresh ? 0 : rc[i].kern);
        fresh = 0;
        if (e->wrap == 2 && pen + adv + ell > max_w + 0.01f) {
            if (ui_push_line(ui, (uint32_t)line, (uint32_t)(i - line), ceilf(pen + ell)))
                return;
            e->ellipsis = 1;
            e->line_count = 1;
            return;
        }
        if (e->wrap == 1 && cp == ' ') {
            brk = i;
            pen_at_brk = pen;
        }
        if (e->wrap == 1 && pen + adv > max_w && brk != (size_t)-1 && brk > line) {
            if (ui_push_line(ui, (uint32_t)line, (uint32_t)(brk - line), ceilf(pen_at_brk)))
                return;
            line = brk + 1;
            i = line - 1;
            pen = 0;
            fresh = 1;
            brk = (size_t)-1;
            continue;
        }
        pen += adv;
    }
    if (e->wrap == 3)
        e->wrap = 2;
    if (ui_push_line(ui, (uint32_t)line, (uint32_t)(end - line), ceilf(pen)))
        return;
    e->line_count = (uint32_t)ui->lines.n - e->line_first;
}

static int ui_wraps(const ui_el* e)
{
    return e->kind == UI_EL_BOX && e->layout.wrap && e->layout.dir == DYN_UI_ROW;
}

static void ui_wrap_lines(dyn_ui_t* ui, const ui_el* e, float* total, float* min_total)
{
    int32_t c;
    int line = -1;
    float lh = 0, lm = 0, sum = 0, msum = 0;
    for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
        const ui_el* ch = ui_el_at(ui, c);
        if (!ui_flow(ch))
            continue;
        if (ch->line != line) {
            if (line >= 0) {
                sum += lh + e->layout.gap;
                msum += lm + e->layout.gap;
            }
            line = ch->line;
            lh = lm = 0;
        }
        if (ch->fit_h > lh)
            lh = ch->fit_h;
        if (ch->min_h > lm)
            lm = ch->min_h;
    }
    *total = line >= 0 ? sum + lh : 0;
    *min_total = line >= 0 ? msum + lm : 0;
}

static float ui_line_height_of(dyn_ui_t* ui, const ui_el* e, int line)
{
    int32_t c;
    float h = 0;
    for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
        const ui_el* ch = ui_el_at(ui, c);
        if (ui_flow(ch) && ch->line == line && ch->fit_h > h)
            h = ch->fit_h;
    }
    return h;
}

static void ui_wrap_grow_line(dyn_ui_t* ui, int32_t from, int32_t to, float rem)
{
    int32_t c;
    float total = 0;
    int iter;
    for (iter = 0; iter < 8 && rem > 0.01f; iter++) {
        float given = 0;
        total = 0;
        for (c = from; c >= 0 && c != to; c = ui_el_at(ui, c)->next) {
            const ui_el* ch = ui_el_at(ui, c);
            if (ui_flow(ch) && ch->layout.w.kind == DYN_UI_GROW && ch->layout.w.value > 0 && !(ch->layout.w.max > 0 && ch->box.w >= ch->layout.w.max))
                total += ch->layout.w.value;
        }
        if (total <= 0)
            return;
        for (c = from; c >= 0 && c != to; c = ui_el_at(ui, c)->next) {
            ui_el* ch = ui_el_at(ui, c);
            float before, after;
            if (!ui_flow(ch) || ch->layout.w.kind != DYN_UI_GROW || !(ch->layout.w.value > 0) || (ch->layout.w.max > 0 && ch->box.w >= ch->layout.w.max))
                continue;
            before = ch->box.w;
            after = ui_clamp(before + rem * ch->layout.w.value / total, &ch->layout.w);
            ch->box.w = after;
            given += after - before;
        }
        if (given <= 0)
            return;
        rem -= given;
    }
}

static void ui_wrap_assign(dyn_ui_t* ui, ui_el* e, float inner)
{
    int32_t c, start = -1;
    int line = 0, count = 0;
    float used = 0;
    for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
        ui_el* ch = ui_el_at(ui, c);
        float w;
        if (!ui_flow(ch))
            continue;
        if (ch->box.w > inner)
            ch->box.w = ui_clamp(inner > ch->min_w ? inner : ch->min_w, &ch->layout.w);
        w = ch->box.w;
        if (count > 0 && used + e->layout.gap + w > inner + 0.01f) {
            ui_wrap_grow_line(ui, start, c, inner - used);
            line++;
            used = 0;
            count = 0;
        }
        if (count == 0)
            start = c;
        ch->line = line;
        used += (count ? e->layout.gap : 0) + w;
        count++;
    }
    if (count)
        ui_wrap_grow_line(ui, start, -1, inner - used);
}

static void ui_fit_axis(dyn_ui_t* ui, int axis)
{
    size_t n = ui->els.n, k;
    for (k = n; k-- > 0;) {
        ui_el* e = ui_el_at(ui, (int32_t)k);
        const dyn_ui_size* sz = axis ? &e->layout.h : &e->layout.w;
        float fit = 0, mn = 0;
        if (e->kind == UI_EL_TRI || e->kind == UI_EL_LINE)
            continue;
        if (e->kind == UI_EL_TEXT) {
            if (axis == 0) {
                float full;
                float word = ui_word_widths(ui, e, &full);
                e->text_w = full;
                fit = full;
                mn = e->wrap == 1 ? word : e->wrap == 2 ? ui_ellipsis_w(ui, e)
                                                        : full;
            } else {
                fit = mn = (float)e->line_count * dyn_ui_line_height(ui, e->font, e->font_px);
            }
        } else if (e->kind == UI_EL_IMAGE) {
            int iw = 0, ih = 0;
            dyn_ui_image_size(ui, e->image, &iw, &ih);
            if (axis == 0)
                fit = (float)iw;
            else
                fit = iw > 0 && e->box.w > 0 && e->layout.h.kind != DYN_UI_FIXED ? roundf(e->box.w * (float)ih / (float)iw) : (float)ih;
            mn = 0;
        } else {
            int32_t c;
            int count = 0;
            float sum = 0, mx = 0, msum = 0, mmx = 0, pads;
            int main = (e->layout.dir == DYN_UI_ROW) == (axis == 0);
            for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
                ui_el* ch = ui_el_at(ui, c);
                float cf, cm;
                if (!ui_flow(ch))
                    continue;
                cf = axis ? ch->fit_h : ch->fit_w;
                cm = axis ? ch->min_h : ch->min_w;
                count++;
                sum += cf;
                msum += cm;
                if (cf > mx)
                    mx = cf;
                if (cm > mmx)
                    mmx = cm;
            }
            pads = axis ? e->layout.pad_t + e->layout.pad_b : e->layout.pad_l + e->layout.pad_r;
            if (ui_wraps(e) && axis == 0) {
                float gaps = count > 1 ? e->layout.gap * (float)(count - 1) : 0;
                fit = sum + gaps + pads;
                mn = mmx + pads;
            } else if (ui_wraps(e)) {
                ui_wrap_lines(ui, e, &fit, &mn);
                fit += pads;
                mn += pads;
            } else if (main) {
                float gaps = count > 1 ? e->layout.gap * (float)(count - 1) : 0;
                fit = sum + gaps + pads;
                mn = msum + gaps + pads;
            } else {
                fit = mx + pads;
                mn = mmx + pads;
            }
            if (ui_clips(e, axis))
                mn = pads;
        }
        if (sz->kind == DYN_UI_FIXED)
            fit = mn = sz->value;
        fit = ui_clamp(fit, sz);
        mn = ui_clamp(mn, sz);
        if (mn > fit)
            mn = fit;
        if (axis) {
            e->fit_h = fit;
            e->min_h = mn;
        } else {
            e->fit_w = fit;
            e->min_w = mn;
        }
    }
}

static float ui_size_of(const ui_el* e, int axis)
{
    return axis ? e->box.h : e->box.w;
}

static void ui_set_size(ui_el* e, int axis, float v)
{
    if (axis)
        e->box.h = v;
    else
        e->box.w = v;
}

static void ui_grow_axis(dyn_ui_t* ui, int axis)
{
    size_t n = ui->els.n, k;
    ui_el* root = ui_el_at(ui, 0);
    ui_set_size(root, axis, axis ? ui->height : ui->width);
    for (k = 0; k < n; k++) {
        ui_el* e = ui_el_at(ui, (int32_t)k);
        int32_t c;
        int main;
        float inner, used = 0, rem, pads;
        int count = 0, iter;
        if (e->kind != UI_EL_BOX || e->first < 0)
            continue;
        pads = axis ? e->layout.pad_t + e->layout.pad_b : e->layout.pad_l + e->layout.pad_r;
        inner = ui_size_of(e, axis) - pads;
        if (inner < 0)
            inner = 0;
        main = (e->layout.dir == DYN_UI_ROW) == (axis == 0);
        for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
            ui_el* ch = ui_el_at(ui, c);
            const dyn_ui_size* sz = axis ? &ch->layout.h : &ch->layout.w;
            float fit = axis ? ch->fit_h : ch->fit_w, v;
            if (ch->kind == UI_EL_TRI || ch->kind == UI_EL_LINE)
                continue;
            if (!ui_flow(ch)) {
                float ref = ch->layout.floating >= DYN_UI_FLOAT_ROOT ? (axis ? ui->height : ui->width) : inner;
                v = sz->kind == DYN_UI_PERCENT ? ref * sz->value : sz->kind == DYN_UI_GROW ? ref
                                                                                           : fit;
                ui_set_size(ch, axis, ui_clamp(v, sz));
                continue;
            }
            if (sz->kind == DYN_UI_PERCENT)
                v = inner * sz->value;
            else if (!main && sz->kind == DYN_UI_GROW)
                v = ui_wraps(e) ? ui_line_height_of(ui, e, ch->line) : inner;
            else
                v = fit;
            if (!main && sz->kind != DYN_UI_FIXED && v > inner && !ui_clips(e, axis) && !ui_wraps(e)) {
                float mn = axis ? ch->min_h : ch->min_w;
                v = inner > mn ? inner : mn;
            }
            ui_set_size(ch, axis, ui_clamp(v, sz));
            used += ui_size_of(ch, axis);
            count++;
        }
        if (main && ui_wraps(e)) {
            ui_wrap_assign(ui, e, inner);
            continue;
        }
        if (!main || count == 0)
            continue;
        used += count > 1 ? e->layout.gap * (float)(count - 1) : 0;
        rem = inner - used;
        for (iter = 0; iter < 8 && rem > 0.01f; iter++) {
            float total = 0, given = 0;
            for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
                ui_el* ch = ui_el_at(ui, c);
                const dyn_ui_size* sz = axis ? &ch->layout.h : &ch->layout.w;
                if (ui_flow(ch) && sz->kind == DYN_UI_GROW && sz->value > 0 && !(sz->max > 0 && ui_size_of(ch, axis) >= sz->max))
                    total += sz->value;
            }
            if (total <= 0)
                break;
            for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
                ui_el* ch = ui_el_at(ui, c);
                const dyn_ui_size* sz = axis ? &ch->layout.h : &ch->layout.w;
                float before, after;
                if (!ui_flow(ch) || sz->kind != DYN_UI_GROW || !(sz->value > 0) || (sz->max > 0 && ui_size_of(ch, axis) >= sz->max))
                    continue;
                before = ui_size_of(ch, axis);
                after = ui_clamp(before + rem * sz->value / total, sz);
                ui_set_size(ch, axis, after);
                given += after - before;
            }
            if (given <= 0)
                break;
            rem -= given;
        }
        if (rem < -0.01f && !ui_clips(e, axis)) {
            float cap = 0;
            for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
                ui_el* ch = ui_el_at(ui, c);
                const dyn_ui_size* sz = axis ? &ch->layout.h : &ch->layout.w;
                float mn = axis ? ch->min_h : ch->min_w;
                if (ui_flow(ch) && sz->kind != DYN_UI_FIXED && sz->kind != DYN_UI_PERCENT && ui_size_of(ch, axis) > mn)
                    cap += ui_size_of(ch, axis) - mn;
            }
            if (cap > 0) {
                float ratio = -rem / cap;
                if (ratio > 1)
                    ratio = 1;
                for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
                    ui_el* ch = ui_el_at(ui, c);
                    const dyn_ui_size* sz = axis ? &ch->layout.h : &ch->layout.w;
                    float mn = axis ? ch->min_h : ch->min_w, s = ui_size_of(ch, axis);
                    if (ui_flow(ch) && sz->kind != DYN_UI_FIXED && sz->kind != DYN_UI_PERCENT && s > mn)
                        ui_set_size(ch, axis, s - (s - mn) * ratio);
                }
            }
        }
    }
}

void dyn_ui_scroll(dyn_ui_t* ui, dyn_ui_id id, float* x, float* y)
{
    float* s = ui_scroll_state(ui, id);
    if (!s)
        return;
    ui->scroll_gen++;
    if (x)
        s[0] = *x;
    if (y)
        s[1] = *y;
}

int dyn_ui_scroll_info(dyn_ui_t* ui, dyn_ui_id id, dyn_ui_scroll_view* out)
{
    float* s = ui_scroll_state(ui, id);
    if (!s)
        return -1;
    out->x = s[0];
    out->y = s[1];
    out->content_w = s[2];
    out->content_h = s[3];
    out->view_w = s[4];
    out->view_h = s[5];
    return 0;
}

static void ui_line_extent(dyn_ui_t* ui, int32_t from, float gap, float* w, float* h, int* grows)
{
    int32_t c;
    int line = ui_el_at(ui, from)->line, n = 0;
    *w = *h = 0;
    *grows = 0;
    for (c = from; c >= 0; c = ui_el_at(ui, c)->next) {
        const ui_el* ch = ui_el_at(ui, c);
        if (!ui_flow(ch))
            continue;
        if (ch->line != line)
            break;
        *w += (n ? gap : 0) + ch->box.w;
        if (ch->box.h > *h)
            *h = ch->box.h;
        if (ch->layout.w.kind == DYN_UI_GROW && ch->layout.w.value > 0)
            (*grows)++;
        n++;
    }
}

static void ui_wrap_content(dyn_ui_t* ui, const ui_el* e, float* w, float* h)
{
    int32_t c;
    int line = -1, lines = 0;
    float lw = 0, lh = 0;
    *w = *h = 0;
    for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
        const ui_el* ch = ui_el_at(ui, c);
        if (!ui_flow(ch))
            continue;
        if (ch->line != line) {
            if (line >= 0) {
                *h += lh;
                if (lw > *w)
                    *w = lw;
            }
            line = ch->line;
            lw = 0;
            lh = 0;
            lines++;
        } else
            lw += e->layout.gap;
        lw += ch->box.w;
        if (ch->box.h > lh)
            lh = ch->box.h;
    }
    if (line >= 0) {
        *h += lh + e->layout.gap * (float)(lines - 1);
        if (lw > *w)
            *w = lw;
    }
}

static void ui_position(dyn_ui_t* ui)
{
    size_t n = ui->els.n, k;
    ui_el* root = ui_el_at(ui, 0);
    root->box.x = 0;
    root->box.y = 0;
    for (k = 0; k < n; k++) {
        ui_el* e = ui_el_at(ui, (int32_t)k);
        int32_t c;
        int row = e->layout.dir == DYN_UI_ROW, count = 0, grows = 0, line;
        float ix, iy, iw, ih, used = 0, cross_max = 0, cur, sx = 0, sy = 0, free_main, line_y, line_w, line_h, wx;
        float* sc = NULL;
        if (e->kind != UI_EL_BOX || e->first < 0)
            continue;
        ix = e->box.x + e->layout.pad_l;
        iy = e->box.y + e->layout.pad_t;
        iw = e->box.w - e->layout.pad_l - e->layout.pad_r;
        ih = e->box.h - e->layout.pad_t - e->layout.pad_b;
        for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
            ui_el* ch = ui_el_at(ui, c);
            if (!ui_flow(ch))
                continue;
            used += row ? ch->box.w : ch->box.h;
            if ((row ? ch->box.h : ch->box.w) > cross_max)
                cross_max = row ? ch->box.h : ch->box.w;
            if ((row ? ch->layout.w.kind : ch->layout.h.kind) == DYN_UI_GROW && (row ? ch->layout.w.value : ch->layout.h.value) > 0)
                grows++;
            count++;
        }
        used += count > 1 ? e->layout.gap * (float)(count - 1) : 0;
        if (ui_wraps(e)) {
            ui_wrap_content(ui, e, &used, &cross_max);
            grows = 1;
        }
        e->content_w = (row ? used : cross_max) + e->layout.pad_l + e->layout.pad_r;
        e->content_h = (row ? cross_max : used) + e->layout.pad_t + e->layout.pad_b;
        if ((e->layout.clip_x || e->layout.clip_y) && e->id) {
            sc = ui_scroll_state(ui, e->id);
            if (sc) {
                float mx = e->content_w - e->box.w, my = e->content_h - e->box.h;
                sc[0] = e->layout.clip_x ? (sc[0]<0 ? 0 : sc[0]>(mx > 0 ? mx : 0) ? (mx > 0 ? mx : 0) : sc[0]) : 0;
                sc[1] = e->layout.clip_y ? (sc[1]<0 ? 0 : sc[1]>(my > 0 ? my : 0) ? (my > 0 ? my : 0) : sc[1]) : 0;
                sx = sc[0];
                sy = sc[1];
                sc[2] = e->content_w;
                sc[3] = e->content_h;
                sc[4] = e->box.w;
                sc[5] = e->box.h;
            }
        }
        free_main = (row ? iw : ih) - used;
        cur = (row ? ix : iy) + (grows == 0 && free_main > 0 ? free_main * ui_align(row ? e->layout.align_x : e->layout.align_y) : 0);
        line = -1;
        line_y = iy - sy;
        line_w = line_h = wx = 0;
        for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next) {
            ui_el* ch = ui_el_at(ui, c);
            if (ui_wraps(e) && ui_flow(ch)) {
                if (ch->line != line) {
                    int lg;
                    if (line >= 0)
                        line_y += line_h + e->layout.gap;
                    line = ch->line;
                    ui_line_extent(ui, c, e->layout.gap, &line_w, &line_h, &lg);
                    wx = ix - sx + (lg == 0 && iw > line_w ? (iw - line_w) * ui_align(e->layout.align_x) : 0);
                }
                ch->box.x = wx;
                ch->box.y = line_y + (line_h > ch->box.h ? (line_h - ch->box.h) * ui_align(e->layout.align_y) : 0);
                wx += ch->box.w + e->layout.gap;
                continue;
            }
            if (ch->kind == UI_EL_TRI || ch->kind == UI_EL_LINE) {
                ch->box = e->box;
                continue;
            }
            if (!ui_flow(ch)) {
                if (ch->layout.floating >= DYN_UI_FLOAT_ROOT) {
                    ch->box.x = ch->layout.float_x - (ch->layout.anchor == DYN_UI_ANCHOR_TOP_RIGHT ? ch->box.w : 0);
                    ch->box.y = ch->layout.float_y - (ch->layout.anchor == DYN_UI_ANCHOR_BOTTOM_LEFT ? ch->box.h : 0);
                } else {
                    ch->box.x = e->box.x + ch->layout.float_x + (ch->layout.anchor == DYN_UI_ANCHOR_TOP_RIGHT ? e->box.w : 0);
                    ch->box.y = e->box.y + ch->layout.float_y + (ch->layout.anchor == DYN_UI_ANCHOR_BOTTOM_LEFT ? e->box.h : 0);
                }
                if (ch->layout.floating == DYN_UI_FLOAT_POPUP || ch->layout.floating == DYN_UI_FLOAT_SAFE) {
                    float l = ui->insets[0], t = ui->insets[1], r = ui->width - ui->insets[2], b = ui->height - ui->insets[3];
                    if (ch->box.x + ch->box.w > r)
                        ch->box.x = r - ch->box.w;
                    if (ch->box.y + ch->box.h > b)
                        ch->box.y = b - ch->box.h;
                    if (ch->box.x < l)
                        ch->box.x = l;
                    if (ch->box.y < t)
                        ch->box.y = t;
                }
                continue;
            }
            if (row) {
                float fc = ih - ch->box.h;
                ch->box.x = cur - sx;
                ch->box.y = iy + (fc > 0 ? fc * ui_align(e->layout.align_y) : 0) - sy;
                cur += ch->box.w + e->layout.gap;
            } else {
                float fc = iw - ch->box.w;
                ch->box.y = cur - sy;
                ch->box.x = ix + (fc > 0 ? fc * ui_align(e->layout.align_x) : 0) - sx;
                cur += ch->box.h + e->layout.gap;
            }
        }
    }
    for (k = 0; k < n; k++) {
        ui_el* e = ui_el_at(ui, (int32_t)k);
        float x0 = roundf(e->box.x), y0 = roundf(e->box.y);
        float x1 = roundf(e->box.x + e->box.w), y1 = roundf(e->box.y + e->box.h);
        e->box.x = x0;
        e->box.y = y0;
        e->box.w = x1 - x0;
        e->box.h = y1 - y0;
    }
}

static void ui_layout(dyn_ui_t* ui)
{
    size_t k;
    ui_fit_axis(ui, 0);
    ui_grow_axis(ui, 0);
    ui->lines.n = 0;
    for (k = 0; k < ui->els.n; k++) {
        ui_el* e = ui_el_at(ui, (int32_t)k);
        if (e->kind == UI_EL_TEXT)
            ui_wrap(ui, e, e->box.w);
    }
    ui_fit_axis(ui, 1);
    ui_grow_axis(ui, 1);
    ui_position(ui);
}

static dyn_ui_cmd* ui_cmd(dyn_ui_t* ui, uint8_t type)
{
    dyn_ui_cmd* c = ui_vec_push(&ui->cmds);
    if (!c) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for draw commands");
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->type = type;
    return c;
}

static int ui_emit_recs(dyn_ui_t* ui, const ui_rec* rc, size_t n, float* pen, float base_y)
{
    size_t k;
    dyn_ui_quad* q;
    if (ui_vec_reserve(&ui->quads, ui->quads.n + n)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for glyphs");
        return -1;
    }
    q = (dyn_ui_quad*)ui->quads.p + ui->quads.n;
    for (k = 0; k < n; k++) {
        const ui_glyph* g = rc[k].glyph >= 0 ? &ui->glyphs[rc[k].glyph] : NULL;
        if (!g)
            break;
        *pen += k ? rc[k].kern : 0;
        if (g->x1 > g->x0) {
            float x = roundf(*pen);
            q->x0 = x + g->x0;
            q->y0 = base_y + g->y0;
            q->x1 = x + g->x1;
            q->y1 = base_y + g->y1;
            q->u0 = g->u0;
            q->v0 = g->v0;
            q->u1 = g->u1;
            q->v1 = g->v1;
            q++;
        }
        *pen += g->advance;
    }
    ui->quads.n = (size_t)(q - (dyn_ui_quad*)ui->quads.p);
    return 0;
}

static void ui_emit_text(dyn_ui_t* ui, ui_el* e)
{
    float lh = dyn_ui_line_height(ui, e->font, e->font_px), asc = dyn_ui_ascent(ui, e->font, e->font_px);
    uint32_t l;
    dyn_ui_cmd* c;
    size_t first = ui->quads.n, en = 0;
    const ui_run* er = NULL;
    const ui_run* r = NULL;
    const char* es = e->ellipsis ? ui_ellipsis_str(ui, e->font, &en) : NULL;
    int tries;
    for (tries = 0; tries < 3; tries++) {
        uint32_t gen = ui->run_gen;
        er = es ? ui_run_get(ui, e->font, e->font_px, es, en) : NULL;
        r = ui_el_run(ui, e);
        if (gen == ui->run_gen || tries == 2)
            break;
    }
    if (!r || (es && !er)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for text runs");
        return;
    }
    for (l = 0; l < e->line_count; l++) {
        const ui_line* ln = (const ui_line*)ui->lines.p + e->line_first + l;
        float pen = e->box.x + roundf((e->box.w - ln->w) * ui_align(e->text_align));
        float base_y = e->box.y + (float)l * lh + asc;
        if (ui_emit_recs(ui, (const ui_rec*)ui->recs.p + r->first + ln->off, ln->len, &pen, base_y))
            return;
        if (e->ellipsis && l == 0 && ui_emit_recs(ui, (const ui_rec*)ui->recs.p + er->first, er->count, &pen, base_y))
            return;
    }
    if (ui->quads.n == first)
        return;
    c = ui_cmd(ui, DYN_UI_CMD_GLYPHS);
    if (!c)
        return;
    c->color = e->color;
    c->rect = e->box;
    c->first = (uint32_t)first;
    c->count = (uint32_t)(ui->quads.n - first);
}

static void ui_hit_push(dyn_ui_t* ui, dyn_ui_id id, const dyn_ui_rect* box, dyn_ui_rect clip, uint32_t layer, dyn_ui_id root, int scroll)
{
    ui_hit* h = ui_vec_push(&ui->hits_cur);
    if (!h) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for hit boxes");
        return;
    }
    h->id = id;
    h->box = *box;
    h->clip = clip;
    h->layer = layer;
    h->root = root;
    h->owner = 0;
    h->track = *box;
    h->scroll = (uint8_t)scroll;
}

static void ui_emit_line(dyn_ui_t* ui, ui_el* e)
{
    const dyn_ui_vertex* pt = (const dyn_ui_vertex*)ui->tris.p + e->tri_first;
    size_t base = ui->verts.n;
    uint32_t i;
    float h = e->stroke / 2;
    dyn_ui_cmd* cmd;
    if (e->tri_count < 2 || h <= 0)
        return;
    if (ui_vec_reserve(&ui->verts, base + (size_t)(e->tri_count - 1) * 6)) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for a polyline");
        return;
    }
    for (i = 0; i + 1 < e->tri_count; i++) {
        float x0 = e->box.x + pt[i].x * e->box.w, y0 = e->box.y + pt[i].y * e->box.h;
        float x1 = e->box.x + pt[i + 1].x * e->box.w, y1 = e->box.y + pt[i + 1].y * e->box.h;
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy), nx, ny;
        dyn_ui_vertex* v = (dyn_ui_vertex*)ui->verts.p + ui->verts.n;
        if (len <= 0)
            continue;
        nx = -dy / len * h;
        ny = dx / len * h;
        v[0].x = x0 + nx, v[0].y = y0 + ny;
        v[1].x = x1 + nx, v[1].y = y1 + ny;
        v[2].x = x1 - nx, v[2].y = y1 - ny;
        v[3] = v[0];
        v[4] = v[2];
        v[5].x = x0 - nx, v[5].y = y0 - ny;
        ui->verts.n += 6;
    }
    if (ui->verts.n == base)
        return;
    cmd = ui_cmd(ui, DYN_UI_CMD_TRIANGLES);
    if (cmd) {
        cmd->color = e->color;
        cmd->first = (uint32_t)base;
        cmd->count = (uint32_t)(ui->verts.n - base);
        cmd->rect = e->box;
    }
}

static void ui_emit_scrollbars(dyn_ui_t* ui, const ui_el* e, dyn_ui_rect clip, uint32_t layer, dyn_ui_id root)
{
    float* s = ui_scroll_state(ui, e->id);
    int a, touch = ui->ptr.kind != DYN_UI_POINTER_MOUSE;
    float bw = ui->rt.scrollbar_w;
    if (!s || bw <= 0)
        return;
    if (touch)
        bw = fmaxf(2, roundf(bw / 2));
    for (a = 0; a < 2; a++) {
        float content = s[2 + a], view = s[4 + a], off = s[a], track_len, len, pos;
        dyn_ui_rect track, thumb;
        dyn_ui_id tid = (e->id ^ 0x7A7A7A7Au) + (dyn_ui_id)a;
        dyn_ui_cmd* cmd;
        int hot;
        if (!(a ? e->layout.clip_y : e->layout.clip_x) || content <= view + 0.5f || view <= 0)
            continue;
        if (a) {
            track.x = e->box.x + e->box.w - bw - 1;
            track.y = e->box.y + 1;
            track.w = bw;
            track.h = e->box.h - 2;
        } else {
            track.x = e->box.x + 1;
            track.y = e->box.y + e->box.h - bw - 1;
            track.w = e->box.w - 2;
            track.h = bw;
        }
        track_len = a ? track.h : track.w;
        if (track_len < 2 * bw || (a ? e->box.w : e->box.h) < bw + 2 || !isfinite(content) || !isfinite(off))
            continue;
        len = fmaxf(track_len * view / content, fminf(track_len, 3 * bw));
        pos = content > view ? (track_len - len) * off / (content - view) : 0;
        thumb = track;
        if (a) {
            thumb.y = roundf(track.y + pos);
            thumb.h = roundf(len);
        } else {
            thumb.x = roundf(track.x + pos);
            thumb.w = roundf(len);
        }
        hot = ui->hot == tid || ui->thumb == e->id;
        cmd = ui_cmd(ui, DYN_UI_CMD_RECT);
        if (cmd) {
            cmd->rect = thumb;
            cmd->color = hot ? ui->rt.scrollbar_hot : ui->rt.scrollbar;
            cmd->radius = bw / 2;
        }
        if (!touch) {
            ui_hit* h = ui_vec_push(&ui->hits_cur);
            if (h) {
                h->id = tid;
                h->box = thumb;
                h->clip = clip;
                h->layer = layer;
                h->root = root;
                h->owner = e->id;
                h->track = track;
                h->scroll = (uint8_t)(2 + a);
            }
        }
    }
}

static void ui_emit_el(dyn_ui_t* ui, int32_t idx, dyn_ui_rect clip, uint32_t layer, dyn_ui_id root, int top)
{
    ui_el* e = ui_el_at(ui, idx);
    int32_t c;
    int clips = (e->layout.clip_x || e->layout.clip_y) && e->kind == UI_EL_BOX;
    int deferred = e->layout.floating >= DYN_UI_FLOAT_POPUP && e->kind != UI_EL_TRI && e->kind != UI_EL_LINE;
    dyn_ui_rect inner = clip;
    dyn_ui_cmd* cmd;
    if (deferred && !top) {
        int32_t* f = ui_vec_push(&ui->floats);
        if (f)
            *f = idx;
        else
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for floating elements");
        return;
    }
    if ((e->layout.interactive && e->id) || deferred)
        ui_hit_push(ui, e->layout.interactive ? e->id : 0, &e->box, clip, layer, root, 0);
    if (e->id && !e->layout.interactive) {
        ui_named* nb = ui_vec_push(&ui->boxes_cur);
        if (!nb)
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for element boxes");
        else {
            nb->id = e->id;
            nb->box = e->box;
        }
    }
    if (e->style.fill & 0xFFu) {
        cmd = ui_cmd(ui, DYN_UI_CMD_RECT);
        if (cmd) {
            cmd->rect = e->box;
            cmd->color = e->style.fill;
            cmd->radius = e->style.radius;
        }
    }
    if (e->kind == UI_EL_TEXT) {
        if (e->color & 0xFFu)
            ui_emit_text(ui, e);
    } else if (e->kind == UI_EL_IMAGE) {
        cmd = ui_cmd(ui, DYN_UI_CMD_IMAGE);
        if (cmd) {
            int iw = 0, ih = 0;
            dyn_ui_image_size(ui, e->image, &iw, &ih);
            cmd->rect = e->box;
            cmd->image = e->image;
            cmd->color = e->color;
            cmd->src.w = (float)iw;
            cmd->src.h = (float)ih;
        }
    } else if (e->kind == UI_EL_LINE) {
        ui_emit_line(ui, e);
    } else if (e->kind == UI_EL_TRI) {
        size_t base = ui->verts.n;
        uint32_t i;
        if (ui_vec_reserve(&ui->verts, base + e->tri_count)) {
            ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for triangles");
        } else {
            for (i = 0; i < e->tri_count; i++) {
                dyn_ui_vertex v = ((dyn_ui_vertex*)ui->tris.p)[e->tri_first + i];
                v.x += e->box.x;
                v.y += e->box.y;
                ((dyn_ui_vertex*)ui->verts.p)[ui->verts.n++] = v;
            }
            cmd = ui_cmd(ui, DYN_UI_CMD_TRIANGLES);
            if (cmd) {
                cmd->color = e->color;
                cmd->first = (uint32_t)base;
                cmd->count = e->tri_count;
                cmd->rect = e->box;
            }
        }
    }
    if (clips) {
        inner = ui_intersect(clip, e->box);
        cmd = ui_cmd(ui, DYN_UI_CMD_CLIP);
        if (cmd)
            cmd->rect = inner;
        if (e->id)
            ui_hit_push(ui, e->id, &e->box, clip, layer, root, 1);
    }
    for (c = e->first; c >= 0; c = ui_el_at(ui, c)->next)
        ui_emit_el(ui, c, inner, layer, root, 0);
    if (clips && e->id)
        ui_emit_scrollbars(ui, e, inner, layer, root);
    if (clips)
        ui_cmd(ui, DYN_UI_CMD_UNCLIP);
    if (e->style.border_w > 0 && (e->style.border & 0xFFu)) {
        cmd = ui_cmd(ui, DYN_UI_CMD_BORDER);
        if (cmd) {
            cmd->rect = e->box;
            cmd->color = e->style.border;
            cmd->width = e->style.border_w;
            cmd->radius = e->style.radius;
        }
    }
}

static uint64_t ui_hash64(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = p;
    size_t i = 0;
    uint64_t w0, w1, w2, w3, l0 = h, l1 = h ^ 0x9E3779B97F4A7C15ull, l2 = h ^ 0xC2B2AE3D27D4EB4Full, l3 = h ^ 0x165667B19E3779F9ull;
    for (; i + 32 <= n; i += 32) {
        memcpy(&w0, b + i, 8);
        memcpy(&w1, b + i + 8, 8);
        memcpy(&w2, b + i + 16, 8);
        memcpy(&w3, b + i + 24, 8);
        l0 = (l0 ^ w0) * 0x9E3779B97F4A7C15ull;
        l1 = (l1 ^ w1) * 0x9E3779B97F4A7C15ull;
        l2 = (l2 ^ w2) * 0x9E3779B97F4A7C15ull;
        l3 = (l3 ^ w3) * 0x9E3779B97F4A7C15ull;
        l0 ^= l0 >> 29;
        l1 ^= l1 >> 29;
        l2 ^= l2 >> 29;
        l3 ^= l3 >> 29;
    }
    h = l0 ^ (l1 * 0xC2B2AE3D27D4EB4Full) ^ (l2 * 0x165667B19E3779F9ull) ^ (l3 * 0x27D4EB2F165667C5ull) ^ (uint64_t)n;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w0, b + i, 8);
        h = (h ^ w0) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

static void ui_input_state(const dyn_ui_t* ui, ui_input* st)
{
    memset(st, 0, sizeof(*st));
    st->w = ui->width;
    st->h = ui->height;
    st->scale = ui->scale;
    memcpy(st->insets, ui->insets, sizeof(st->insets));
    if (ui->hot) {
        const ui_hit* hp = ui->hits_prev.p;
        size_t k;
        for (k = 0; k < ui->hits_prev.n; k++)
            if (hp[k].scroll >= 2 && hp[k].id == ui->hot)
                st->hot = ui->hot;
    }
    st->thumb = ui->thumb;
    st->kind = ui->ptr.kind;
    st->scroll_gen = ui->scroll_gen;
    st->atlas_version = ui->atlas_version;
    st->nfonts = (uint32_t)ui->nfonts;
}

static int ui_decl_eq(const ui_el* a, const ui_el* b)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    uint64_t d = 0, x, y;
    size_t i;
    for (i = 0; i < UI_EL_INPUT; i += 8) {
        memcpy(&x, p + i, 8);
        memcpy(&y, q + i, 8);
        d |= x ^ y;
    }
    return d == 0;
}

static int ui_same_input(const dyn_ui_t* ui, const ui_input* st)
{
    const ui_el* a = ui->els.p;
    const ui_el* b = ui->els_prev.p;
    size_t k;
    if (memcmp(st, &ui->last_in, sizeof(*st)) || memcmp(&ui->rt, &ui->last_rt, sizeof(ui->rt)))
        return 0;
    if (ui->els.n != ui->els_prev.n || ui->text.n != ui->text_prev.n || ui->tris.n != ui->tris_prev.n)
        return 0;
    if ((ui->text.n && memcmp(ui->text.p, ui->text_prev.p, ui->text.n))
        || (ui->tris.n && memcmp(ui->tris.p, ui->tris_prev.p, ui->tris.n * sizeof(dyn_ui_vertex))))
        return 0;
    for (k = 0; k < ui->els.n; k++)
        if (!ui_decl_eq(a + k, b + k))
            return 0;
    return 1;
}

int dyn_ui_end(dyn_ui_t* ui, int* changed)
{
    dyn_ui_rect all;
    size_t done = 0;
    ui_input in;
    int same;
    int32_t* fl;
    if (!ui->frame_open)
        return DYN_UI_ERR_NESTING;
    ui->frame_open = 0;
    if (ui->depth != 1)
        ui_fail(ui, DYN_UI_ERR_NESTING, "end() with elements still open");
    if (ui->nids != 0)
        ui_fail(ui, DYN_UI_ERR_NESTING, "end() with a pushId still open");
    ui->depth = 0;
    ui_input_state(ui, &in);
    same = ui->frame > 1 && ui_same_input(ui, &in);
    if (!ui->error && ui->have_input && same && !ui_vec_reserve(&ui->hits_cur, ui->hits_prev.n)
        && !ui_vec_reserve(&ui->boxes_cur, ui->boxes_prev.n)) {
        if (ui->hits_prev.n)
            memcpy(ui->hits_cur.p, ui->hits_prev.p, ui->hits_prev.n * sizeof(ui_hit));
        ui->hits_cur.n = ui->hits_prev.n;
        if (ui->boxes_prev.n)
            memcpy(ui->boxes_cur.p, ui->boxes_prev.p, ui->boxes_prev.n * sizeof(ui_named));
        ui->boxes_cur.n = ui->boxes_prev.n;
        ui->reused++;
        {
            size_t k;
            for (k = 0; k < ui->cslots; k++)
                if (ui->slots[k].id && ui->slots[k].layout && ui->slots[k].touched + 1 >= ui->frame)
                    ui->slots[k].touched = ui->frame;
        }
    } else {
        ui->cmds.n = ui->quads.n = ui->verts.n = ui->floats.n = 0;
        ui->layers = 0;
        ui_layout(ui);
        all.x = 0;
        all.y = 0;
        all.w = ui->width;
        all.h = ui->height;
        ui_emit_el(ui, 0, all, 0, 0, 1);
        while (done < ui->floats.n) {
            size_t best = done, j;
            int32_t t;
            fl = ui->floats.p;
            for (j = done + 1; j < ui->floats.n; j++)
                if (ui_el_at(ui, fl[j])->layout.z < ui_el_at(ui, fl[best])->layout.z)
                    best = j;
            t = fl[best];
            memmove(fl + done + 1, fl + done, (best - done) * sizeof(*fl));
            fl[done] = t;
            ui_emit_el(ui, t, all, ++ui->layers, ui_el_at(ui, t)->id, 1);
            done++;
        }
        ui->have_input = !ui->error;
    }
    if (changed)
        *changed = ui->error || ui->frame == 1 || !same || ui->atlas_version != ui->last_atlas || ui->image_version != ui->last_image;
    ui->last_in = in;
    ui->last_rt = ui->rt;
    ui->last_atlas = ui->atlas_version;
    ui->last_image = ui->image_version;
    if (ui->fin.released & 1u)
        ui->active = 0;
    if (ui->dnd && ((ui->fin.released & 1u) || !(ui->ptr.buttons & 1u))) {
        ui->dnd = ui->dnd_drop = 0;
        ui->dnd_src = ui->dnd_over = 0;
    }
    return ui->error;
}

uint32_t dyn_ui_interact(dyn_ui_t* ui, dyn_ui_id id, int flags)
{
    uint32_t bits = 0;
    int hot, focusable = flags & DYN_UI_FOCUSABLE, direct = ui->ptr.kind != DYN_UI_POINTER_MOUSE;
    if (!id)
        return 0;
    ui->last_interact = id;
    hot = ui->hot == id && (ui->active == 0 || ui->active == id);
    if (focusable) {
        dyn_ui_id* f = ui_vec_push(&ui->focus_cur);
        if (f)
            *f = id;
    }
    if (hot)
        bits |= DYN_UI_HOT;
    if ((ui->fin.pressed & 1u) && hot) {
        ui->active = id;
        ui->flags_id = id;
        ui->active_flags = flags;
        bits |= DYN_UI_PRESSED;
        if (focusable && !direct)
            ui->focus = id;
        ui->focus_visible = 0;
        if (ui->ptr.time > 0 && ui->last_click == id && ui->ptr.time - ui->last_click_time < 0.35) {
            bits |= DYN_UI_DOUBLE;
            ui->last_click = 0;
        } else {
            ui->last_click = id;
            ui->last_click_time = ui->ptr.time;
        }
    }
    if ((ui->fin.released & 1u) && ui->active == id) {
        bits |= DYN_UI_RELEASED;
        if (ui->hot == id && !(ui->dnd && ui->dnd_src == id)) {
            bits |= DYN_UI_CLICKED;
            if (focusable && direct)
                ui->focus = id;
        }
        ui->active = 0;
    } else if (ui->active == id)
        bits |= DYN_UI_ACTIVE;
    if (ui->focus == id)
        bits |= DYN_UI_FOCUSED;
    if (ui->context_id == id)
        bits |= DYN_UI_CONTEXT;
    return bits;
}

int dyn_ui_box(const dyn_ui_t* ui, dyn_ui_id id, dyn_ui_rect* out)
{
    const ui_vec* v = ui->frame_open ? &ui->hits_prev : &ui->hits_cur;
    const ui_vec* bv = ui->frame_open ? &ui->boxes_prev : &ui->boxes_cur;
    const ui_hit* h = v->p;
    const ui_named* b = bv->p;
    size_t i;
    for (i = 0; i < v->n; i++)
        if (h[i].id == id && !h[i].scroll) {
            *out = h[i].box;
            return 1;
        }
    for (i = 0; i < bv->n; i++)
        if (b[i].id == id) {
            *out = b[i].box;
            return 1;
        }
    return 0;
}

int dyn_ui_last_box(const dyn_ui_t* ui, dyn_ui_rect* out)
{
    return ui->last_id ? dyn_ui_box(ui, ui->last_id, out) : 0;
}

int dyn_ui_last_item_box(const dyn_ui_t* ui, int item, int column, dyn_ui_rect* out)
{
    uint32_t k = ui->last_items_off + (uint32_t)item;
    dyn_ui_id id;
    if (!ui->last_items || item < -1)
        return 0;
    if (item == -1) {
        k = 0x10000u + (uint32_t)column;
        return column >= 0 && column < ui->last_cols_n && dyn_ui_box(ui, dyn_ui_hash(ui->last_items, &k, sizeof(k)), out);
    }
    id = dyn_ui_hash(ui->last_items, &k, sizeof(k));
    if (column >= 0) {
        uint32_t c = (uint32_t)column;
        dyn_ui_rect row;
        int j;
        if (dyn_ui_box(ui, dyn_ui_hash(id, &c, sizeof(c)), out))
            return 1;
        if (column >= ui->last_cols_n || !dyn_ui_box(ui, id, &row))
            return 0;
        out->x = row.x;
        for (j = 0; j < column; j++)
            out->x += ui->last_cols[j];
        out->y = row.y;
        out->w = ui->last_cols[column];
        out->h = row.h;
        return 1;
    }
    return dyn_ui_box(ui, id, out);
}

int dyn_ui_has_last(const dyn_ui_t* ui)
{
    return ui->last_id != 0;
}

void dyn_ui_set_last(dyn_ui_t* ui, dyn_ui_id id)
{
    ui->last_id = id;
    ui->last_items = 0;
}

int dyn_ui_drag_source(dyn_ui_t* ui)
{
    dyn_ui_id id = ui->last_id, *p;
    if (!id || !ui->frame_open)
        return 0;
    p = ui_vec_push(&ui->dnd_src_cur);
    if (!p) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for drag sources");
        return 0;
    }
    *p = id;
    if (ui->last_interact != id)
        dyn_ui_interact(ui, id, 0);
    return ui->dnd == 1 && ui->dnd_src == id;
}

int dyn_ui_drop_target(dyn_ui_t* ui)
{
    dyn_ui_id id = ui->last_id, *p;
    if (!id || !ui->frame_open)
        return 0;
    p = ui_vec_push(&ui->dnd_tgt_cur);
    if (!p) {
        ui_fail(ui, DYN_UI_ERR_NOMEM, "out of memory for drop targets");
        return 0;
    }
    *p = id;
    if (ui->dnd != 1 || ui->dnd_over != id)
        return 0;
    return ui->dnd_drop ? 2 : 1;
}

dyn_ui_id dyn_ui_dragging(const dyn_ui_t* ui)
{
    return ui->dnd == 1 ? ui->dnd_src : 0;
}

dyn_ui_id dyn_ui_hot(const dyn_ui_t* ui)
{
    return ui->hot;
}

dyn_ui_id dyn_ui_active(const dyn_ui_t* ui)
{
    return ui->active;
}

dyn_ui_id dyn_ui_focus(const dyn_ui_t* ui)
{
    return ui->focus;
}

void dyn_ui_set_focus(dyn_ui_t* ui, dyn_ui_id id)
{
    ui->focus = id;
}

const dyn_ui_pointer* dyn_ui_pointer_state(const dyn_ui_t* ui)
{
    return &ui->ptr;
}

void dyn_ui_pointer_edges(const dyn_ui_t* ui, uint32_t* pressed, uint32_t* released, float* wheel_x, float* wheel_y)
{
    if (pressed)
        *pressed = ui->fin.pressed;
    if (released)
        *released = ui->fin.released;
    if (wheel_x)
        *wheel_x = ui->fin.wheel_x;
    if (wheel_y)
        *wheel_y = ui->fin.wheel_y;
}

int dyn_ui_key_pressed(const dyn_ui_t* ui, int key)
{
    return key > 0 && key < DYN_UI_KEY_COUNT ? ui->fin.key_hit[key] : 0;
}

int dyn_ui_key_down(const dyn_ui_t* ui, int key)
{
    return key > 0 && key < DYN_UI_KEY_COUNT ? ui->key_down[key] : 0;
}

int dyn_ui_mods(const dyn_ui_t* ui)
{
    return ui->mods;
}

const char* dyn_ui_typed(const dyn_ui_t* ui, size_t* n)
{
    *n = ui->fin.ntyped;
    return ui->fin.typed;
}

const dyn_ui_cmd* dyn_ui_commands(const dyn_ui_t* ui, size_t* n)
{
    *n = ui->cmds.n;
    return ui->cmds.p;
}

const dyn_ui_quad* dyn_ui_quads(const dyn_ui_t* ui, size_t* n)
{
    *n = ui->quads.n;
    return ui->quads.p;
}

const dyn_ui_vertex* dyn_ui_vertices(const dyn_ui_t* ui, size_t* n)
{
    *n = ui->verts.n;
    return ui->verts.p;
}

typedef struct {
    uint8_t* px;
    int w, h;
    size_t stride;
    int cx0, cy0, cx1, cy1;
} ui_canvas;

static int ui_ipx(float v)
{
    return v >= -16777216.0f && v <= 16777216.0f ? (int)floorf(v) : v > 0 ? 16777216
                                                                          : -16777216;
}

static uint32_t ui_ld32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ui_st32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t ui_px_word(uint32_t color)
{
    return (color >> 24) | (color >> 8 & 0xFF00u) | (color << 8 & 0xFF0000u) | 0xFF000000u;
}

static uint32_t ui_mix(uint32_t s, uint32_t d, unsigned a)
{
    uint32_t ia = 255 - a;
    uint32_t rb = (s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * ia + 0x00800080u;
    uint32_t ga = ((s >> 8) & 0x00FF00FFu) * a + ((d >> 8) & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    ga = (ga + ((ga >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
    return rb | ga;
}

static void ui_px_put(uint8_t* p, uint32_t s, unsigned a)
{
    if (!a)
        return;
    ui_st32(p, a == 255 ? s : ui_mix(s, ui_ld32(p), a));
}

static uint8_t ui_chan(uint16_t sa, uint8_t d, uint16_t ia)
{
    uint16_t t = (uint16_t)(sa + d * ia);
    return (uint8_t)((t + (t >> 8)) >> 8);
}

static void ui_px_blend(uint8_t* p, int n, uint32_t s, unsigned a)
{
    uint16_t ia = (uint16_t)(255 - a), s0 = (uint16_t)((s & 0xFFu) * a + 128), s1 = (uint16_t)((s >> 8 & 0xFFu) * a + 128);
    uint16_t s2 = (uint16_t)((s >> 16 & 0xFFu) * a + 128), s3 = (uint16_t)((s >> 24) * a + 128);
    int i;
    for (i = 0; i < n; i++) {
        uint8_t* q = p + (size_t)i * 4;
        q[0] = ui_chan(s0, q[0], ia);
        q[1] = ui_chan(s1, q[1], ia);
        q[2] = ui_chan(s2, q[2], ia);
        q[3] = ui_chan(s3, q[3], ia);
    }
}

#define UI_RUN_LOOP 640

static void ui_px_run(uint8_t* p, int n, uint32_t s, unsigned a)
{
    int i;
    if (!a || n <= 0)
        return;
    if (a == 255) {
        int m = n <= UI_RUN_LOOP ? n : 16;
        for (i = 0; i < m; i++)
            ui_st32(p + (size_t)i * 4, s);
        for (; i < n; i += i)
            memcpy(p + (size_t)i * 4, p, (size_t)(i < n - i ? i : n - i) * 4);
        return;
    }
    ui_px_blend(p, n, s, a);
}

static unsigned ui_alpha(uint32_t color, unsigned cov)
{
    return ((color & 0xFFu) * cov + 127) / 255;
}

static void ui_span(const ui_canvas* cv, float x, float y, float w, float h, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = ui_ipx(x);
    *y0 = ui_ipx(y);
    *x1 = ui_ipx(x + w);
    *y1 = ui_ipx(y + h);
    if (*x0 < cv->cx0)
        *x0 = cv->cx0;
    if (*y0 < cv->cy0)
        *y0 = cv->cy0;
    if (*x1 > cv->cx1)
        *x1 = cv->cx1;
    if (*y1 > cv->cy1)
        *y1 = cv->cy1;
}

typedef struct {
    float rad, l, r, t, b;
} ui_rshape;

static void ui_rshape_init(ui_rshape* s, const dyn_ui_rect* r, float radius)
{
    float hw = r->w * 0.5f, hh = r->h * 0.5f, rad = radius;
    if (rad > hw)
        rad = hw;
    if (rad > hh)
        rad = hh;
    s->rad = rad;
    s->l = r->x + rad;
    s->r = r->x + r->w - rad;
    s->t = r->y + rad;
    s->b = r->y + r->h - rad;
}

static unsigned ui_rshape_cov(const ui_rshape* s, float px, float py)
{
    float cx, cy, dx, dy, d;
    if (s->rad <= 0)
        return 255;
    cx = px < s->l ? s->l : px > s->r ? s->r
                                      : px;
    cy = py < s->t ? s->t : py > s->b ? s->b
                                      : py;
    dx = px - cx;
    dy = py - cy;
    if (dx == 0 && dy == 0)
        return 255;
    d = sqrtf(dx * dx + dy * dy) - s->rad;
    if (d <= -0.5f)
        return 255;
    if (d >= 0.5f)
        return 0;
    return (unsigned)lroundf((0.5f - d) * 255.0f);
}

enum { UI_P_NLT,
    UI_P_GT,
    UI_P_GE };

static int ui_pred(int k, float px, float v)
{
    return k == UI_P_NLT ? !(px < v) : k == UI_P_GT ? px > v
                                                    : px >= v;
}

static int ui_first_x(int k, float v, int lo, int hi)
{
    float t = ceilf(v - 0.5f);
    int e = !(t >= (float)lo) ? lo : !(t <= (float)hi) ? hi
                                                       : (int)t;
    while (e > lo && ui_pred(k, (float)(e - 1) + 0.5f, v))
        e--;
    while (e < hi && !ui_pred(k, (float)e + 0.5f, v))
        e++;
    return e;
}

static void ui_cut(int* bp, int* nb, int v, int lo, int hi)
{
    int i, j;
    if (v <= lo || v >= hi)
        return;
    for (i = 0; bp[i] < v; i++)
        ;
    if (bp[i] == v)
        return;
    for (j = *nb; j > i; j--)
        bp[j] = bp[j - 1];
    bp[i] = v;
    (*nb)++;
}

typedef struct {
    ui_rshape o, i;
    int ring, ob0, ob1, ii0, ii1, ib0, ib1;
} ui_rfill;

static unsigned ui_rfill_cov(const ui_rfill* f, int inside, float px, float py)
{
    unsigned c = ui_rshape_cov(&f->o, px, py), ic;
    if (!f->ring)
        return c;
    ic = inside ? ui_rshape_cov(&f->i, px, py) : 0;
    return c > ic ? c - ic : 0;
}

static void ui_round_fill(ui_canvas* cv, const dyn_ui_rect* r, const dyn_ui_rect* in, uint32_t color, float radius, float hole, int x0, int y0, int x1, int y1)
{
    ui_rfill f;
    uint32_t s = ui_px_word(color);
    int y, bp[10], nb, k, x;
    if (x0 >= x1)
        return;
    memset(&f, 0, sizeof(f));
    ui_rshape_init(&f.o, r, radius);
    f.ring = hole > 0 && in->w > 0 && in->h > 0;
    f.ob0 = x0;
    f.ob1 = x1;
    if (f.o.rad > 0) {
        f.ob0 = ui_first_x(UI_P_NLT, f.o.l, x0, x1);
        f.ob1 = ui_first_x(UI_P_GT, f.o.r, x0, x1);
    }
    if (f.ring) {
        ui_rshape_init(&f.i, in, radius - hole > 0 ? radius - hole : 0);
        f.ii0 = ui_first_x(UI_P_GE, in->x, x0, x1);
        f.ii1 = ui_first_x(UI_P_NLT, in->x + in->w, x0, x1);
        f.ib0 = x0;
        f.ib1 = x1;
        if (f.i.rad > 0) {
            f.ib0 = ui_first_x(UI_P_NLT, f.i.l, x0, x1);
            f.ib1 = ui_first_x(UI_P_GT, f.i.r, x0, x1);
        }
    }
    for (y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        int rin = f.ring && py >= in->y && py < in->y + in->h;
        uint8_t* row = cv->px + (size_t)y * cv->stride;
        nb = 2;
        bp[0] = x0;
        bp[1] = x1;
        ui_cut(bp, &nb, f.ob0, x0, x1);
        ui_cut(bp, &nb, f.ob1, x0, x1);
        if (rin) {
            ui_cut(bp, &nb, f.ii0, x0, x1);
            ui_cut(bp, &nb, f.ii1, x0, x1);
            ui_cut(bp, &nb, f.ib0, x0, x1);
            ui_cut(bp, &nb, f.ib1, x0, x1);
        }
        for (k = 0; k + 1 < nb; k++) {
            int a = bp[k], b = bp[k + 1];
            int inside = rin && a >= f.ii0 && a < f.ii1;
            int oc = f.o.rad <= 0 || (a >= f.ob0 && a < f.ob1);
            int ic = !inside || f.i.rad <= 0 || (a >= f.ib0 && a < f.ib1);
            if (oc && ic) {
                ui_px_run(row + (size_t)a * 4, b - a, s, ui_alpha(color, ui_rfill_cov(&f, inside, (float)a + 0.5f, py)));
                continue;
            }
            for (x = a; x < b; x++) {
                unsigned cov = ui_rfill_cov(&f, inside, (float)x + 0.5f, py);
                if (cov)
                    ui_px_put(row + (size_t)x * 4, s, ui_alpha(color, cov));
            }
        }
    }
}

static void ui_span_fill(ui_canvas* cv, int y, int x0, int x1, uint32_t color)
{
    if (x0 < cv->cx0)
        x0 = cv->cx0;
    if (x1 > cv->cx1)
        x1 = cv->cx1;
    if (x0 >= x1 || y < cv->cy0 || y >= cv->cy1)
        return;
    ui_px_run(cv->px + (size_t)y * cv->stride + (size_t)x0 * 4, x1 - x0, ui_px_word(color), color & 0xFFu);
}

static int ui_center_lo(float v)
{
    float c = ceilf(v - 0.5f);
    return c >= -16777216.0f && c <= 16777216.0f ? (int)c : c > 0 ? 16777216
                                                                  : -16777216;
}

static void ui_fill(ui_canvas* cv, const dyn_ui_rect* r, uint32_t color, float radius, float hole)
{
    int x0, y0, x1, y1, y;
    dyn_ui_rect in;
    ui_span(cv, r->x, r->y, r->w, r->h, &x0, &y0, &x1, &y1);
    in.x = r->x + hole;
    in.y = r->y + hole;
    in.w = r->w - 2 * hole;
    in.h = r->h - 2 * hole;
    if (!(color & 0xFFu))
        return;
    if (radius <= 0) {
        int has_in = hole > 0 && in.w > 0 && in.h > 0;
        int ix0 = has_in ? ui_center_lo(in.x) : 0, ix1 = has_in ? ui_center_lo(in.x + in.w) : 0;
        int iy0 = has_in ? ui_center_lo(in.y) : 0, iy1 = has_in ? ui_center_lo(in.y + in.h) : 0;
        if (!has_in && x0 == 0 && x1 == cv->w && cv->stride == (size_t)cv->w * 4 && y0 < y1) {
            ui_px_run(cv->px + (size_t)y0 * cv->stride, (y1 - y0) * cv->w, ui_px_word(color), color & 0xFFu);
            return;
        }
        for (y = y0; y < y1; y++) {
            if (has_in && y >= iy0 && y < iy1 && ix0 < ix1) {
                ui_span_fill(cv, y, x0, ix0 < x1 ? ix0 : x1, color);
                ui_span_fill(cv, y, ix1 > x0 ? ix1 : x0, x1, color);
            } else
                ui_span_fill(cv, y, x0, x1, color);
        }
        return;
    }
    ui_round_fill(cv, r, &in, color, radius, hole, x0, y0, x1, y1);
}

static float ui_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

typedef struct {
    float ax, ay, dx, dy;
} ui_tedge;

static const float ui_sub_x[16] = { 0.125f, 0.375f, 0.625f, 0.875f, 0.125f, 0.375f, 0.625f, 0.875f, 0.125f, 0.375f, 0.625f, 0.875f, 0.125f, 0.375f, 0.625f,
    0.875f };
static const float ui_sub_y[16] = { 0.125f, 0.125f, 0.125f, 0.125f, 0.375f, 0.375f, 0.375f, 0.375f, 0.625f, 0.625f, 0.625f, 0.625f, 0.875f, 0.875f, 0.875f,
    0.875f };

static void ui_tedge_set(ui_tedge* e, const dyn_ui_vertex* a, const dyn_ui_vertex* b)
{
    e->ax = a->x;
    e->ay = a->y;
    e->dx = b->x - a->x;
    e->dy = b->y - a->y;
}

static unsigned ui_tri_hits(const ui_tedge* e, float sg, int x, int y)
{
    unsigned hits = 0;
    float fx = (float)x, fy = (float)y;
    int k;
    for (k = 0; k < 16; k++) {
        float px = fx + ui_sub_x[k], py = fy + ui_sub_y[k];
        float w0 = (e[0].dx * (py - e[0].ay) - e[0].dy * (px - e[0].ax)) * sg;
        float w1 = (e[1].dx * (py - e[1].ay) - e[1].dy * (px - e[1].ax)) * sg;
        float w2 = (e[2].dx * (py - e[2].ay) - e[2].dy * (px - e[2].ax)) * sg;
        hits += (unsigned)((w0 >= 0) & (w1 >= 0) & (w2 >= 0));
    }
    return hits;
}

static void ui_tri(ui_canvas* cv, const dyn_ui_vertex* v, uint32_t color)
{
    float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)), maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
    float miny = fminf(v[0].y, fminf(v[1].y, v[2].y)), maxy = fmaxf(v[0].y, fmaxf(v[1].y, v[2].y));
    float area = ui_edge(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    uint32_t s = ui_px_word(color);
    unsigned full = ui_alpha(color, 255);
    int x0, x1, y0, y1, x, y;
    if (area == 0)
        return;
    ui_span(cv, minx, miny, maxx - minx + 1, maxy - miny + 1, &x0, &y0, &x1, &y1);
    {
        float l0 = sqrtf((v[2].x - v[1].x) * (v[2].x - v[1].x) + (v[2].y - v[1].y) * (v[2].y - v[1].y));
        float l1 = sqrtf((v[0].x - v[2].x) * (v[0].x - v[2].x) + (v[0].y - v[2].y) * (v[0].y - v[2].y));
        float l2 = sqrtf((v[1].x - v[0].x) * (v[1].x - v[0].x) + (v[1].y - v[0].y) * (v[1].y - v[0].y));
        float sg = area < 0 ? -1.0f : 1.0f, i0, i1, i2, s0, s1, s2;
        ui_tedge te[3];
        if (!(l0 > 0) || !(l1 > 0) || !(l2 > 0))
            return;
        ui_tedge_set(&te[0], &v[1], &v[2]);
        ui_tedge_set(&te[1], &v[2], &v[0]);
        ui_tedge_set(&te[2], &v[0], &v[1]);
        i0 = sg / l0;
        i1 = sg / l1;
        i2 = sg / l2;
        s0 = -(v[2].y - v[1].y) * i0;
        s1 = -(v[0].y - v[2].y) * i1;
        s2 = -(v[1].y - v[0].y) * i2;
        for (y = y0; y < y1; y++) {
            float cy = (float)y + 0.5f, cx = (float)x0 + 0.5f;
            float d0 = ui_edge(v[1].x, v[1].y, v[2].x, v[2].y, cx, cy) * i0;
            float d1 = ui_edge(v[2].x, v[2].y, v[0].x, v[0].y, cx, cy) * i1;
            float d2 = ui_edge(v[0].x, v[0].y, v[1].x, v[1].y, cx, cy) * i2;
            uint8_t* row = cv->px + (size_t)y * cv->stride;
            for (x = x0; x < x1; x++, d0 += s0, d1 += s1, d2 += s2) {
                unsigned hits;
                if (d0 < -0.6f || d1 < -0.6f || d2 < -0.6f)
                    continue;
                if (d0 > 0.6f && d1 > 0.6f && d2 > 0.6f) {
                    ui_px_put(row + (size_t)x * 4, s, full);
                    continue;
                }
                hits = ui_tri_hits(te, sg, x, y);
                if (hits)
                    ui_px_put(row + (size_t)x * 4, s, ui_alpha(color, (hits * 255 + 8) / 16));
            }
        }
    }
}

static void ui_glyphs_raster(ui_canvas* cv, const dyn_ui_t* ui, const dyn_ui_cmd* c)
{
    const dyn_ui_quad* q = (const dyn_ui_quad*)ui->quads.p + c->first;
    uint32_t s = ui_px_word(c->color), k;
    for (k = 0; k < c->count; k++) {
        int x0, y0, x1, y1, x, y, ox = ui_ipx(q[k].x0), oy = ui_ipx(q[k].y0);
        size_t u0 = (size_t)q[k].u0, v0 = (size_t)q[k].v0;
        ui_span(cv, q[k].x0, q[k].y0, q[k].x1 - q[k].x0, q[k].y1 - q[k].y0, &x0, &y0, &x1, &y1);
        for (y = y0; y < y1; y++) {
            const uint8_t* src = ui->atlas + (v0 + (size_t)(y - oy)) * UI_ATLAS_W + u0;
            uint8_t* row = cv->px + (size_t)y * cv->stride;
            for (x = x0; x < x1; x++) {
                unsigned cov = src[(size_t)(x - ox)];
                if (cov)
                    ui_px_put(row + (size_t)x * 4, s, ui_alpha(c->color, cov));
            }
        }
    }
}

#define UI_IMAGE_COLS 256

static void ui_image_raster(ui_canvas* cv, const dyn_ui_t* ui, const dyn_ui_cmd* c)
{
    int iw, ih, x0, y0, x1, y1, x, y, xc, cols[UI_IMAGE_COLS];
    const uint8_t* src = dyn_ui_image_pixels(ui, c->image, &iw, &ih);
    uint32_t tr = c->color >> 24, tg = (c->color >> 16) & 0xFFu, tb = (c->color >> 8) & 0xFFu, ta = c->color & 0xFFu;
    if (!src || !(c->rect.w > 0) || !(c->rect.h > 0))
        return;
    ui_span(cv, c->rect.x, c->rect.y, c->rect.w, c->rect.h, &x0, &y0, &x1, &y1);
    for (xc = x0; xc < x1; xc += UI_IMAGE_COLS) {
        int xe = x1 - xc > UI_IMAGE_COLS ? xc + UI_IMAGE_COLS : x1;
        for (x = xc; x < xe; x++) {
            float fx = ((float)x + 0.5f - c->rect.x) * (float)iw / c->rect.w;
            cols[x - xc] = fx < 0 ? 0 : fx >= (float)iw ? iw - 1
                                                        : (int)fx;
        }
        for (y = y0; y < y1; y++) {
            float fy = ((float)y + 0.5f - c->rect.y) * (float)ih / c->rect.h;
            int sy = fy < 0 ? 0 : fy >= (float)ih ? ih - 1
                                                  : (int)fy;
            const uint8_t* srow = src + (size_t)sy * (size_t)iw * 4;
            uint8_t* row = cv->px + (size_t)y * cv->stride;
            for (x = xc; x < xe; x++) {
                const uint8_t* p = srow + (size_t)cols[x - xc] * 4;
                uint32_t w = (p[0] * tr + 127) / 255 | (p[1] * tg + 127) / 255 << 8 | (p[2] * tb + 127) / 255 << 16 | 0xFF000000u;
                ui_px_put(row + (size_t)x * 4, w, (p[3] * ta + 127) / 255);
            }
        }
    }
}

int dyn_ui_raster(const dyn_ui_t* ui, uint8_t* rgba, int width, int height, size_t stride, uint32_t clear)
{
    ui_canvas cv;
    const dyn_ui_cmd* cmds = ui->cmds.p;
    int clips[UI_MAX_DEPTH][4];
    int nclip = 0, x, y;
    size_t i;
    if (ui->frame_open)
        return DYN_UI_ERR_NESTING;
    if (width < 1 || height < 1 || width > 16384 || height > 16384 || stride < (size_t)width * 4)
        return DYN_UI_ERR_LIMIT;
    cv.px = rgba;
    cv.w = width;
    cv.h = height;
    cv.stride = stride;
    cv.cx0 = 0;
    cv.cy0 = 0;
    cv.cx1 = width;
    cv.cy1 = height;
    if (!(ui->cmds.n && cmds[0].type == DYN_UI_CMD_RECT && cmds[0].radius <= 0 && (cmds[0].color & 0xFFu) == 0xFFu && cmds[0].rect.x <= 0
            && cmds[0].rect.y <= 0 && cmds[0].rect.x + cmds[0].rect.w >= (float)width && cmds[0].rect.y + cmds[0].rect.h >= (float)height)) {
        uint8_t px[4];
        px[0] = (uint8_t)(clear >> 24);
        px[1] = (uint8_t)(clear >> 16);
        px[2] = (uint8_t)(clear >> 8);
        px[3] = (uint8_t)clear;
        for (x = 0; x < width; x++)
            memcpy(rgba + (size_t)x * 4, px, 4);
        for (y = 1; y < height; y++)
            memcpy(rgba + (size_t)y * stride, rgba, (size_t)width * 4);
    }
    for (i = 0; i < ui->cmds.n; i++) {
        const dyn_ui_cmd* c = &cmds[i];
        switch (c->type) {
        case DYN_UI_CMD_RECT:
            ui_fill(&cv, &c->rect, c->color, c->radius, 0);
            break;
        case DYN_UI_CMD_BORDER:
            ui_fill(&cv, &c->rect, c->color, c->radius, c->width);
            break;
        case DYN_UI_CMD_GLYPHS:
            ui_glyphs_raster(&cv, ui, c);
            break;
        case DYN_UI_CMD_IMAGE:
            ui_image_raster(&cv, ui, c);
            break;
        case DYN_UI_CMD_TRIANGLES: {
            const dyn_ui_vertex* v = (const dyn_ui_vertex*)ui->verts.p + c->first;
            uint32_t k;
            for (k = 0; k + 2 < c->count; k += 3)
                ui_tri(&cv, v + k, c->color);
            break;
        }
        case DYN_UI_CMD_CLIP: {
            int x0, y0, x1, y1;
            if (nclip < UI_MAX_DEPTH) {
                clips[nclip][0] = cv.cx0;
                clips[nclip][1] = cv.cy0;
                clips[nclip][2] = cv.cx1;
                clips[nclip][3] = cv.cy1;
                nclip++;
            }
            ui_span(&cv, c->rect.x, c->rect.y, c->rect.w, c->rect.h, &x0, &y0, &x1, &y1);
            cv.cx0 = x0;
            cv.cy0 = y0;
            cv.cx1 = x1 > x0 ? x1 : x0;
            cv.cy1 = y1 > y0 ? y1 : y0;
            break;
        }
        case DYN_UI_CMD_UNCLIP:
            if (nclip > 0) {
                nclip--;
                cv.cx0 = clips[nclip][0];
                cv.cy0 = clips[nclip][1];
                cv.cx1 = clips[nclip][2];
                cv.cy1 = clips[nclip][3];
            }
            break;
        default:
            break;
        }
    }
    return DYN_UI_OK;
}

void dyn_ui_theme_default(dyn_ui_theme* t)
{
    memset(t, 0, sizeof(*t));
    t->font = -1;
    t->font_px = 13;
    t->pad_x = 6;
    t->pad_y = 4;
    t->spacing = 4;
    t->window_pad = 8;
    t->indent = 14;
    t->scrollbar_w = 8;
    t->radius = 0;
    t->border_w = 1;
    t->label_w = 120;
    t->min_target = 0;
    t->drag_slop = 8;
    t->long_press = 0.5f;
    t->text_scale = 1;
    t->text = 0xE6E6E6FFu;
    t->text_dim = 0x9AA0A6FFu;
    t->on_accent = 0xFFFFFFFFu;
    t->bg = 0x202428FFu;
    t->panel = 0x262B30FFu;
    t->frame = 0x15181BFFu;
    t->frame_hot = 0x1B1F23FFu;
    t->frame_active = 0x101214FFu;
    t->button = 0x3A4048FFu;
    t->button_hot = 0x4A525CFFu;
    t->button_active = 0x2F343AFFu;
    t->accent = 0x3D7EFFFFu;
    t->accent_hot = 0x5A92FFFFu;
    t->border = 0x50565EFFu;
    t->title = 0x2C3238FFu;
    t->title_active = 0x34506EFFu;
    t->selection = 0x3D7EFF66u;
    t->popup = 0x2A2F35FFu;
    t->shade = 0x00000080u;
    t->tooltip = 0x111315F0u;
    t->link = 0x6FA8FFFFu;
    t->scrollbar = 0x5A616A99u;
    t->scrollbar_hot = 0x7A828CCCu;
    t->error = 0xF85149FFu;
}

dyn_ui_theme* dyn_ui_theme_of(dyn_ui_t* ui)
{
    return &ui->theme;
}

void dyn_ui_set_time(dyn_ui_t* ui, double seconds)
{
    if (isfinite(seconds))
        ui->now = seconds;
}

double dyn_ui_time(const dyn_ui_t* ui)
{
    return ui->now;
}

void dyn_ui_wake_at(dyn_ui_t* ui, double seconds)
{
    if (seconds < ui->wake)
        ui->wake = seconds;
}

double dyn_ui_next_wake(const dyn_ui_t* ui)
{
    return ui->wake;
}

void dyn_ui_set_clipboard(dyn_ui_t* ui, dyn_ui_clipboard_get get, dyn_ui_clipboard_set set, void* ud)
{
    ui->clip_get = get;
    ui->clip_set = set;
    ui->clip_ud = ud;
}

dyn_ui_id dyn_ui_hot_root(const dyn_ui_t* ui)
{
    return ui->hot_root;
}

int dyn_ui_popup_level(const dyn_ui_t* ui, dyn_ui_id id)
{
    int i;
    for (i = 0; i < ui->npopups; i++)
        if (ui->popups[i] == id)
            return i;
    return -1;
}

void dyn_ui_open_popup(dyn_ui_t* ui, dyn_ui_id id, int level)
{
    if (!id || level < 0 || level >= UI_MAX_POPUPS)
        return;
    if (level > ui->npopups)
        level = ui->npopups;
    ui->popups[level] = id;
    ui->npopups = level + 1;
}

int dyn_ui_popup_was_open(const dyn_ui_t* ui, dyn_ui_id id)
{
    int i;
    for (i = 0; i < ui->npress_popups; i++)
        if (ui->press_popups[i] == id)
            return 1;
    return 0;
}

void dyn_ui_close_popups(dyn_ui_t* ui, int level)
{
    if (level < 0)
        level = 0;
    if (level < ui->npopups)
        ui->npopups = level;
}

void dyn_ui_set_scale(dyn_ui_t* ui, float scale)
{
    if (scale >= 0.25f && scale <= 16)
        ui->scale = scale;
}

float dyn_ui_scale(const dyn_ui_t* ui)
{
    return ui->scale;
}

void dyn_ui_set_insets(dyn_ui_t* ui, float left, float top, float right, float bottom)
{
    ui->insets[0] = ui_finite(left, 0, 4096);
    ui->insets[1] = ui_finite(top, 0, 4096);
    ui->insets[2] = ui_finite(right, 0, 4096);
    ui->insets[3] = ui_finite(bottom, 0, 4096);
}

const dyn_ui_theme* dyn_ui_resolved(const dyn_ui_t* ui)
{
    return &ui->rt;
}

void dyn_ui_input_composition(dyn_ui_t* ui, const char* utf8, size_t n, int cursor)
{
    if (n > sizeof(ui->compose) - 1)
        n = sizeof(ui->compose) - 1;
    while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80 && n < sizeof(ui->compose) - 1)
        n--;
    if (n)
        memcpy(ui->compose, utf8, n);
    ui->compose[n] = 0;
    ui->ncompose = n;
    ui->compose_cursor = cursor < 0 ? 0 : cursor;
}

int dyn_ui_text_input(const dyn_ui_t* ui, dyn_ui_rect* caret)
{
    if (caret)
        *caret = ui->edit_rect;
    return ui->editing;
}

typedef struct {
    float x, y;
} ui_pt;

#define UI_T_MAXP 72

_Static_assert(sizeof(((dyn_ui_t*)0)->tpath[0]) == UI_T_MAXP * sizeof(ui_pt), "tessellator scratch holds UI_T_MAXP points per path");

static int ui_t_batch(dyn_ui_t* ui, uint32_t tex, const dyn_ui_rect* clip)
{
    dyn_ui_batch* b = ui->tbatch.n ? (dyn_ui_batch*)ui->tbatch.p + ui->tbatch.n - 1 : NULL;
    if (b && b->texture == tex && !memcmp(&b->clip, clip, sizeof(*clip)))
        return 0;
    if (b && b->count == 0) {
        b->texture = tex;
        b->clip = *clip;
        return 0;
    }
    b = ui_vec_push(&ui->tbatch);
    if (!b)
        return -1;
    b->texture = tex;
    b->clip = *clip;
    b->first = (uint32_t)ui->tidx.n;
    b->count = 0;
    return 0;
}

static dyn_ui_gvertex* ui_t_verts(dyn_ui_t* ui, size_t n, uint32_t* base)
{
    dyn_ui_gvertex* v;
    if (ui_vec_reserve(&ui->tverts, ui->tverts.n + n))
        return NULL;
    *base = (uint32_t)ui->tverts.n;
    v = (dyn_ui_gvertex*)ui->tverts.p + ui->tverts.n;
    ui->tverts.n += n;
    return v;
}

static uint32_t* ui_t_reserve(dyn_ui_t* ui, size_t n)
{
    if (ui_vec_reserve(&ui->tidx, ui->tidx.n + n))
        return NULL;
    return (uint32_t*)ui->tidx.p + ui->tidx.n;
}

static void ui_t_commit(dyn_ui_t* ui, const uint32_t* end)
{
    size_t n = (size_t)(end - ((uint32_t*)ui->tidx.p + ui->tidx.n));
    ui->tidx.n += n;
    ((dyn_ui_batch*)ui->tbatch.p)[ui->tbatch.n - 1].count += (uint32_t)n;
}

static int ui_t_idx(dyn_ui_t* ui, const uint32_t* ix, size_t n, uint32_t base)
{
    size_t k;
    uint32_t* o = ui_t_reserve(ui, n);
    if (!o)
        return -1;
    for (k = 0; k < n; k++)
        o[k] = base + ix[k];
    ui_t_commit(ui, o + n);
    return 0;
}

static int ui_t_quad(dyn_ui_t* ui, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c)
{
    static const uint32_t ix[6] = { 0, 1, 2, 0, 2, 3 };
    uint32_t base;
    dyn_ui_gvertex* v = ui_t_verts(ui, 4, &base);
    if (!v)
        return -1;
    v[0].x = x0, v[0].y = y0, v[0].u = u0, v[0].v = v0, v[0].color = c;
    v[1].x = x1, v[1].y = y0, v[1].u = u1, v[1].v = v0, v[1].color = c;
    v[2].x = x1, v[2].y = y1, v[2].u = u1, v[2].v = v1, v[2].color = c;
    v[3].x = x0, v[3].y = y1, v[3].u = u0, v[3].v = v1, v[3].color = c;
    return ui_t_idx(ui, ix, 6, base);
}

static int ui_t_glyphs(dyn_ui_t* ui, const dyn_ui_quad* q, uint32_t count, uint32_t c, float aw, float ah)
{
    uint32_t base, k, *o;
    dyn_ui_gvertex* v;
    if (!count)
        return 0;
    v = ui_t_verts(ui, (size_t)count * 4, &base);
    o = v ? ui_t_reserve(ui, (size_t)count * 6) : NULL;
    if (!o)
        return -1;
    for (k = 0; k < count; k++, v += 4, o += 6, base += 4) {
        float u0 = q[k].u0 / aw, v0 = q[k].v0 / ah, u1 = q[k].u1 / aw, v1 = q[k].v1 / ah;
        v[0].x = q[k].x0, v[0].y = q[k].y0, v[0].u = u0, v[0].v = v0, v[0].color = c;
        v[1].x = q[k].x1, v[1].y = q[k].y0, v[1].u = u1, v[1].v = v0, v[1].color = c;
        v[2].x = q[k].x1, v[2].y = q[k].y1, v[2].u = u1, v[2].v = v1, v[2].color = c;
        v[3].x = q[k].x0, v[3].y = q[k].y1, v[3].u = u0, v[3].v = v1, v[3].color = c;
        o[0] = base, o[1] = base + 1, o[2] = base + 2, o[3] = base, o[4] = base + 2, o[5] = base + 3;
    }
    ui_t_commit(ui, o);
    return 0;
}

static float ui_t_clamp_rad(const dyn_ui_rect* r, float rad)
{
    if (rad > r->w / 2)
        rad = r->w / 2;
    if (rad > r->h / 2)
        rad = r->h / 2;
    return rad < 0 ? 0 : rad;
}

static int ui_t_segs(float rad)
{
    return rad < 0.5f ? 0 : (int)fminf(16, fmaxf(3, rad * 0.6f + 2));
}

static int ui_t_round_path(const dyn_ui_rect* r, float rad, int segs, ui_pt* out)
{
    float cx[4], cy[4];
    int k, s, n = 0;
    rad = ui_t_clamp_rad(r, rad);
    cx[0] = r->x + rad, cy[0] = r->y + rad;
    cx[1] = r->x + r->w - rad, cy[1] = r->y + rad;
    cx[2] = r->x + r->w - rad, cy[2] = r->y + r->h - rad;
    cx[3] = r->x + rad, cy[3] = r->y + r->h - rad;
    for (k = 0; k < 4; k++) {
        float a0 = 3.14159265f * (1.0f + 0.5f * (float)k);
        if (segs == 0) {
            out[n].x = cx[k];
            out[n].y = cy[k];
            n++;
            continue;
        }
        for (s = 0; s <= segs; s++) {
            float a = a0 + 1.57079633f * (float)s / (float)segs;
            out[n].x = cx[k] + cosf(a) * rad;
            out[n].y = cy[k] + sinf(a) * rad;
            n++;
        }
    }
    return n;
}

static void ui_t_normals(const ui_pt* p, int n, ui_pt* nm)
{
    int k;
    for (k = 0; k < n; k++) {
        const ui_pt* a = &p[(k + n - 1) % n];
        const ui_pt* b = &p[k];
        const ui_pt* c = &p[(k + 1) % n];
        float dx0 = b->x - a->x, dy0 = b->y - a->y, dx1 = c->x - b->x, dy1 = c->y - b->y;
        float l0 = sqrtf(dx0 * dx0 + dy0 * dy0), l1 = sqrtf(dx1 * dx1 + dy1 * dy1), mx, my, d2;
        if (l0 > 1e-4f)
            dx0 /= l0, dy0 /= l0;
        if (l1 > 1e-4f)
            dx1 /= l1, dy1 /= l1;
        if (l0 <= 1e-4f && l1 > 1e-4f)
            dx0 = dx1, dy0 = dy1;
        else if (l1 <= 1e-4f && l0 > 1e-4f)
            dx1 = dx0, dy1 = dy0;
        else if (l0 <= 1e-4f && l1 <= 1e-4f) {
            int j;
            for (j = 2; j < n; j++) {
                const ui_pt* q = &p[(k + j) % n];
                float ex = q->x - b->x, ey = q->y - b->y, el = sqrtf(ex * ex + ey * ey);
                if (el > 1e-4f) {
                    dx0 = dx1 = ex / el;
                    dy0 = dy1 = ey / el;
                    break;
                }
            }
        }
        mx = (dy0 + dy1) * 0.5f;
        my = (-dx0 - dx1) * 0.5f;
        d2 = mx * mx + my * my;
        if (d2 > 1e-6f) {
            float inv = 1.0f / d2;
            if (inv > 100)
                inv = 100;
            mx *= inv;
            my *= inv;
        }
        nm[k].x = mx;
        nm[k].y = my;
    }
}

static int ui_t_fill(dyn_ui_t* ui, const dyn_ui_rect* r, float rad, uint32_t c, float wu, float wv)
{
    ui_pt* pt = (ui_pt*)ui->tpath[0];
    ui_pt* nm = (ui_pt*)ui->tpath[1];
    int n, k;
    uint32_t base, c0 = c & 0xFFFFFF00u, *o;
    dyn_ui_gvertex* v;
    if (rad < 0.5f)
        return ui_t_quad(ui, r->x, r->y, r->x + r->w, r->y + r->h, wu, wv, wu, wv, c);
    rad = ui_t_clamp_rad(r, rad);
    n = ui_t_round_path(r, rad, ui_t_segs(rad), pt);
    ui_t_normals(pt, n, nm);
    v = ui_t_verts(ui, (size_t)n * 2, &base);
    if (!v)
        return -1;
    for (k = 0; k < n; k++) {
        v[2 * k].x = pt[k].x - nm[k].x * 0.5f, v[2 * k].y = pt[k].y - nm[k].y * 0.5f, v[2 * k].color = c;
        v[2 * k + 1].x = pt[k].x + nm[k].x * 0.5f, v[2 * k + 1].y = pt[k].y + nm[k].y * 0.5f, v[2 * k + 1].color = c0;
        v[2 * k].u = v[2 * k + 1].u = wu;
        v[2 * k].v = v[2 * k + 1].v = wv;
    }
    o = ui_t_reserve(ui, (size_t)(n > 2 ? n - 2 : 0) * 3 + (size_t)n * 6);
    if (!o)
        return -1;
    for (k = 2; k < n; k++) {
        o[0] = base;
        o[1] = base + (uint32_t)(2 * (k - 1));
        o[2] = base + (uint32_t)(2 * k);
        o += 3;
    }
    for (k = 0; k < n; k++) {
        uint32_t a = base + (uint32_t)(2 * k), b = base + (uint32_t)(2 * ((k + 1) % n));
        o[0] = a, o[1] = b, o[2] = b + 1, o[3] = a, o[4] = b + 1, o[5] = a + 1;
        o += 6;
    }
    ui_t_commit(ui, o);
    return 0;
}

static int ui_t_ring(dyn_ui_t* ui, const dyn_ui_rect* r, float rad, float w, uint32_t c, float wu, float wv)
{
    ui_pt* po = (ui_pt*)ui->tpath[0];
    ui_pt* pi = (ui_pt*)ui->tpath[1];
    ui_pt* no = (ui_pt*)ui->tpath[2];
    ui_pt* ni = (ui_pt*)ui->tpath[3];
    dyn_ui_rect in;
    int n, k;
    uint32_t base, c0 = c & 0xFFFFFF00u, *o;
    dyn_ui_gvertex* v;
    if (rad < 0.5f) {
        if (ui_t_quad(ui, r->x, r->y, r->x + r->w, r->y + w, wu, wv, wu, wv, c) || ui_t_quad(ui, r->x, r->y + r->h - w, r->x + r->w, r->y + r->h, wu, wv, wu, wv, c))
            return -1;
        if (ui_t_quad(ui, r->x, r->y + w, r->x + w, r->y + r->h - w, wu, wv, wu, wv, c))
            return -1;
        return ui_t_quad(ui, r->x + r->w - w, r->y + w, r->x + r->w, r->y + r->h - w, wu, wv, wu, wv, c);
    }
    in.x = r->x + w;
    in.y = r->y + w;
    in.w = r->w - 2 * w;
    in.h = r->h - 2 * w;
    if (in.w <= 0 || in.h <= 0)
        return ui_t_fill(ui, r, rad, c, wu, wv);
    rad = ui_t_clamp_rad(r, rad);
    n = ui_t_round_path(r, rad, ui_t_segs(rad), po);
    if (ui_t_round_path(&in, rad - w, ui_t_segs(rad) ? ui_t_segs(rad) : 1, pi) != n)
        return ui_t_fill(ui, r, rad, c, wu, wv);
    ui_t_normals(po, n, no);
    ui_t_normals(pi, n, ni);
    v = ui_t_verts(ui, (size_t)n * 4, &base);
    if (!v)
        return -1;
    for (k = 0; k < n; k++) {
        dyn_ui_gvertex* q = v + 4 * k;
        q[0].x = po[k].x + no[k].x * 0.5f, q[0].y = po[k].y + no[k].y * 0.5f, q[0].color = c0;
        q[1].x = po[k].x - no[k].x * 0.5f, q[1].y = po[k].y - no[k].y * 0.5f, q[1].color = c;
        q[2].x = pi[k].x + ni[k].x * 0.5f, q[2].y = pi[k].y + ni[k].y * 0.5f, q[2].color = c;
        q[3].x = pi[k].x - ni[k].x * 0.5f, q[3].y = pi[k].y - ni[k].y * 0.5f, q[3].color = c0;
        q[0].u = q[1].u = q[2].u = q[3].u = wu;
        q[0].v = q[1].v = q[2].v = q[3].v = wv;
    }
    o = ui_t_reserve(ui, (size_t)n * 18);
    if (!o)
        return -1;
    for (k = 0; k < n; k++) {
        uint32_t a = base + (uint32_t)(4 * k), b = base + (uint32_t)(4 * ((k + 1) % n)), s;
        for (s = 0; s < 3; s++) {
            o[0] = a + s, o[1] = b + s, o[2] = b + s + 1;
            o[3] = a + s, o[4] = b + s + 1, o[5] = a + s + 1;
            o += 6;
        }
    }
    ui_t_commit(ui, o);
    return 0;
}

int dyn_ui_tessellate(dyn_ui_t* ui, const dyn_ui_gvertex** vo, size_t* nv, const uint32_t** io, size_t* ni, const dyn_ui_batch** bo, size_t* nb)
{
    const dyn_ui_cmd* cmds = ui->cmds.p;
    dyn_ui_rect* clips = ui->tclips;
    dyn_ui_rect cur;
    int nclip = 0, rc = 0;
    size_t i;
    float aw = (float)UI_ATLAS_W, ah = (float)ui->atlas_h, wu = 0.5f / aw, wv = 0.5f / ah;
    if (ui->frame_open)
        return DYN_UI_ERR_NESTING;
    ui->tverts.n = ui->tidx.n = ui->tbatch.n = 0;
    cur.x = 0;
    cur.y = 0;
    cur.w = ui->width;
    cur.h = ui->height;
    if (ui_t_batch(ui, DYN_UI_TEX_ATLAS, &cur))
        return DYN_UI_ERR_NOMEM;
    for (i = 0; i < ui->cmds.n && !rc; i++) {
        const dyn_ui_cmd* c = &cmds[i];
        switch (c->type) {
        case DYN_UI_CMD_RECT:
            rc = ui_t_batch(ui, DYN_UI_TEX_ATLAS, &cur) || ui_t_fill(ui, &c->rect, c->radius, c->color, wu, wv);
            break;
        case DYN_UI_CMD_BORDER:
            rc = ui_t_batch(ui, DYN_UI_TEX_ATLAS, &cur) || ui_t_ring(ui, &c->rect, c->radius, c->width, c->color, wu, wv);
            break;
        case DYN_UI_CMD_GLYPHS:
            rc = ui_t_batch(ui, DYN_UI_TEX_ATLAS, &cur) || ui_t_glyphs(ui, (const dyn_ui_quad*)ui->quads.p + c->first, c->count, c->color, aw, ah);
            break;
        case DYN_UI_CMD_IMAGE:
            rc = ui_t_batch(ui, c->image, &cur)
                || ui_t_quad(ui, c->rect.x, c->rect.y, c->rect.x + c->rect.w, c->rect.y + c->rect.h, 0, 0, 1, 1, c->color);
            break;
        case DYN_UI_CMD_TRIANGLES: {
            const dyn_ui_vertex* tv = (const dyn_ui_vertex*)ui->verts.p + c->first;
            uint32_t k, base;
            dyn_ui_gvertex* g;
            rc = ui_t_batch(ui, DYN_UI_TEX_ATLAS, &cur);
            if (rc || !c->count)
                break;
            g = ui_t_verts(ui, c->count, &base);
            if (!g) {
                rc = -1;
                break;
            }
            for (k = 0; k < c->count; k++) {
                g[k].x = tv[k].x;
                g[k].y = tv[k].y;
                g[k].u = wu;
                g[k].v = wv;
                g[k].color = c->color;
            }
            {
                uint32_t n3 = c->count - c->count % 3, *o = ui_t_reserve(ui, n3);
                if (!o) {
                    rc = -1;
                    break;
                }
                for (k = 0; k < n3; k++)
                    o[k] = base + k;
                ui_t_commit(ui, o + n3);
            }
            break;
        }
        case DYN_UI_CMD_CLIP:
            if (nclip < UI_MAX_DEPTH)
                clips[nclip++] = cur;
            cur = ui_intersect(cur, c->rect);
            break;
        case DYN_UI_CMD_UNCLIP:
            if (nclip > 0)
                cur = clips[--nclip];
            break;
        default:
            break;
        }
    }
    if (rc)
        return DYN_UI_ERR_NOMEM;
    if (ui->tbatch.n && ((dyn_ui_batch*)ui->tbatch.p)[ui->tbatch.n - 1].count == 0)
        ui->tbatch.n--;
    *vo = ui->tverts.p;
    *nv = ui->tverts.n;
    *io = ui->tidx.p;
    *ni = ui->tidx.n;
    *bo = ui->tbatch.p;
    *nb = ui->tbatch.n;
    return DYN_UI_OK;
}

void dyn_ui_stats(const dyn_ui_t* ui, dyn_ui_stats_t* s)
{
    s->elements = (uint32_t)ui->els.n;
    s->text_bytes = (uint32_t)ui->text.n;
    s->commands = (uint32_t)ui->cmds.n;
    s->quads = (uint32_t)ui->quads.n;
    s->vertices = (uint32_t)ui->verts.n;
    s->hits = (uint32_t)(ui->frame_open ? ui->hits_prev.n : ui->hits_cur.n);
    s->runs = (uint32_t)ui->runs.n;
    s->run_records = (uint32_t)ui->recs.n;
    s->glyphs = (uint32_t)ui->nglyphs;
    s->atlas_height = (uint32_t)ui->atlas_h;
    s->frames = ui->frame;
    s->reused = ui->reused;
}

#include "dyn-ui-widgets.inc.c"
