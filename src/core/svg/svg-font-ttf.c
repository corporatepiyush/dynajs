#include "svg-int.h"

typedef struct {
    const unsigned char* d;
    size_t n;
    size_t base;
    unsigned char* owned;
} TTFont;

struct FontFace {
    TTFont f;
    unsigned numGlyphs;
    unsigned unitsPerEm;
    int indexToLocFormat;
    int numberOfHMetrics;
    float ascent, descent;
    const unsigned char* loca;
    unsigned loca_len;
    const unsigned char* glyf;
    unsigned glyf_len;
    const unsigned char* hmtx;
    unsigned hmtx_len;
    const unsigned char* cmap;
    unsigned cmap_len;
};

static unsigned rd_u16(const unsigned char* p) {
    return ((unsigned)p[0] << 8) | p[1];
}

static int rd_i16(const unsigned char* p) {
    return (int)(short)rd_u16(p);
}

static unsigned rd_u32(const unsigned char* p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}

static const unsigned char* find_table(const TTFont* f, const char* tag,
                                       unsigned* len) {

    if (f->base + 12 > f->n) return NULL;
    unsigned num = rd_u16(f->d + f->base + 4);
    if (f->base + 12 + (size_t)num * 16 > f->n) return NULL;
    for (unsigned i = 0; i < num; i++) {
        const unsigned char* rec = f->d + f->base + 12 + (size_t)i * 16;
        if (memcmp(rec, tag, 4)) continue;
        unsigned off = rd_u32(rec + 8);
        unsigned l = rd_u32(rec + 12);
        if ((size_t)off + l > f->n) return NULL;
        if (len) *len = l;
        return f->d + off;
    }
    return NULL;
}

static int cmap_lookup_subtable(const unsigned char* t, unsigned len,
                                int want_format12) {
    if (len < 4) return -1;
    unsigned n = rd_u16(t + 2);
    int fallback = -1;
    for (unsigned i = 0; i < n; i++) {
        if (4 + (size_t)i * 8 + 8 > len) break;
        const unsigned char* rec = t + 4 + (size_t)i * 8;
        unsigned plat = rd_u16(rec);
        unsigned enc = rd_u16(rec + 2);
        unsigned off = rd_u32(rec + 4);
        if ((size_t)off + 2 > len) continue;
        int fmt = (int)rd_u16(t + off);
        int preferred = (plat == 3 && enc == 10) || (plat == 0 && enc >= 4);
        int ok = (fmt == 12 && want_format12) || (fmt == 4 && !want_format12);
        if (!ok) continue;
        if (preferred) return (int)off;
        if (fallback < 0) fallback = (int)off;
    }
    return fallback;
}

static int cmap_lookup4(const unsigned char* t, unsigned len, unsigned c) {
    if (len < 14) return 0;
    unsigned segX2 = rd_u16(t + 6);
    unsigned seg = segX2 / 2;
    if (seg == 0 || 16 + (size_t)seg * 8 > len) return 0;
    const unsigned char* end = t + 14;
    const unsigned char* start = end + segX2 + 2;
    const unsigned char* delta = start + segX2;
    const unsigned char* range = delta + segX2;
    for (unsigned i = 0; i < seg; i++) {
        unsigned e = rd_u16(end + i * 2);
        if (c > e) continue;
        unsigned s = rd_u16(start + i * 2);
        if (c < s) return 0;
        int d = rd_i16(delta + i * 2);
        unsigned ro = rd_u16(range + i * 2);
        if (ro == 0) return (int)((c + (unsigned)d) & 0xFFFFu);

        size_t off = (size_t)(range - t) + (size_t)i * 2 + (size_t)ro +
                     (size_t)(c - s) * 2;
        if (off + 2 > len) return 0;
        unsigned g = rd_u16(t + off);
        return g ? (int)((g + (unsigned)d) & 0xFFFFu) : 0;
    }
    return 0;
}

static int cmap_lookup12(const unsigned char* t, unsigned len, unsigned c) {
    if (len < 16) return 0;
    unsigned ng = rd_u32(t + 12);
    if (16 + (size_t)ng * 12 > len) return 0;
    for (unsigned i = 0; i < ng; i++) {
        const unsigned char* g = t + 16 + (size_t)i * 12;
        unsigned s = rd_u32(g), e = rd_u32(g + 4), gid = rd_u32(g + 8);
        if (c < s) return 0;
        if (c <= e) return (int)(gid + (c - s));
    }
    return 0;
}

static int glyph_index(const FontFace* f, unsigned c) {
    if (!f->cmap) return 0;
    int off = cmap_lookup_subtable(f->cmap, f->cmap_len, 1);
    if (off < 0) off = cmap_lookup_subtable(f->cmap, f->cmap_len, 0);
    if (off < 0) return 0;
    const unsigned char* t = f->cmap + off;
    unsigned len = f->cmap_len - (unsigned)off;
    if (len < 2) return 0;
    int fmt = (int)rd_u16(t);
    if (fmt == 12) return cmap_lookup12(t, len, c);
    if (fmt == 4) return cmap_lookup4(t, len, c);
    return 0;
}

static int cont_push(PtList* l, int n) {
    if (l->nc + 1 > l->ccap) {
        int cap = l->ccap ? l->ccap * 2 : 16;
        int* np = (int*)svg_realloc(l->cont, (size_t)cap * sizeof(int));
        if (!np) return 0;
        l->cont = np;
        l->ccap = cap;
    }
    l->cont[l->nc++] = n;
    return 1;
}

#define FONT_MAX_POINTS (1 << 20)

static int pt_push(PtList* l, float x, float y) {

    if (l->n > FONT_MAX_POINTS - 2) return 0;
    if (l->n + 2 > l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        while (cap < l->n + 2) cap *= 2;
        float* np = (float*)svg_realloc(l->xy, (size_t)cap * sizeof(float));
        if (!np) return 0;
        l->xy = np;
        l->cap = cap;
    }
    l->xy[l->n++] = x;
    l->xy[l->n++] = y;
    return 1;
}

static int emit_contour(PtList* out, const float* px, const float* py,
                        const unsigned char* on, int n) {
    if (n <= 0) return 1;
    int s = -1;
    for (int i = 0; i < n; i++)
        if (on[i]) { s = i; break; }

    float cx, cy;
    if (s >= 0) {
        cx = px[s];
        cy = py[s];
    } else {

        cx = (px[0] + px[n - 1]) * 0.5f;
        cy = (py[0] + py[n - 1]) * 0.5f;
        s = 0;
    }
    int before = out->n;
    if (!pt_push(out, cx, cy)) return 0;

    for (int k = 1; k <= n; k++) {
        int i = (s + k) % n;
        if (on[i]) {
            if (!pt_push(out, px[i], py[i])) return 0;
            cx = px[i];
            cy = py[i];
            continue;
        }
        int j = (i + 1) % n;
        float ex, ey;
        if (on[j]) {
            ex = px[j];
            ey = py[j];
        } else {
            ex = (px[i] + px[j]) * 0.5f;
            ey = (py[i] + py[j]) * 0.5f;
        }
        float qx = px[i], qy = py[i];
        for (int t = 1; t <= 6; t++) {
            float u = (float)t / 6.0f;
            float mu = 1.0f - u;
            float x = mu * mu * cx + 2.0f * mu * u * qx + u * u * ex;
            float y = mu * mu * cy + 2.0f * mu * u * qy + u * u * ey;
            if (!pt_push(out, x, y)) return 0;
        }
        cx = ex;
        cy = ey;
    }
    return cont_push(out, (out->n - before) / 2);
}

static int parse_simple(const unsigned char* g, unsigned len, PtList* out,
                        int* ncontours, int max_contours, float scale) {
    if (len < 10) return -1;
    int nc = rd_i16(g);
    if (nc < 0) return -1;
    if (nc == 0) { *ncontours = 0; return 0; }
    if (nc > max_contours) nc = max_contours;
    unsigned p = 10;
    if (p + (size_t)nc * 2 + 2 > len) return -1;
    unsigned last = rd_u16(g + p + (size_t)(nc - 1) * 2);
    unsigned npoints = last + 1;
    if (npoints == 0 || npoints > 10000) return -1;
    unsigned instr_len = rd_u16(g + p + (size_t)nc * 2);
    size_t skip = (size_t)nc * 2 + 2 + (size_t)instr_len;
    if ((size_t)p + skip > (size_t)len) return -1;
    p = (unsigned)((size_t)p + skip);

    unsigned char* flags = (unsigned char*)svg_malloc(npoints);
    float* px = (float*)svg_malloc(npoints * sizeof(float));
    float* py = (float*)svg_malloc(npoints * sizeof(float));
    unsigned char* on = (unsigned char*)svg_malloc(npoints);
    if (!flags || !px || !py || !on) {
        svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on);
        return -1;
    }
    for (unsigned i = 0; i < npoints; ) {
        if (p >= len) { svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on); return -1; }
        unsigned char fl = g[p++];
        flags[i++] = fl;
        if (fl & 8) {
            if (p >= len) { svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on); return -1; }
            int rep = g[p++];
            while (rep-- > 0 && i < npoints) flags[i++] = fl;
        }
    }
    unsigned v = 0;
    for (unsigned i = 0; i < npoints; i++) {
        unsigned char fl = flags[i];
        if (fl & 2) {
            if (p >= len) goto bad;
            int d = g[p++];
            v += (fl & 16) ? (unsigned)d : (unsigned)(-(int)d);
        } else if (!(fl & 16)) {
            if (p + 2 > len) goto bad;
            v += (unsigned)rd_i16(g + p);
            p += 2;
        }
        px[i] = (float)(int)v * scale;
    }
    v = 0;
    for (unsigned i = 0; i < npoints; i++) {
        unsigned char fl = flags[i];
        if (fl & 4) {
            if (p >= len) goto bad;
            int d = g[p++];
            v += (fl & 32) ? (unsigned)d : (unsigned)(-(int)d);
        } else if (!(fl & 32)) {
            if (p + 2 > len) goto bad;
            v += (unsigned)rd_i16(g + p);
            p += 2;
        }
        py[i] = (float)(int)v * scale;
    }
    for (unsigned i = 0; i < npoints; i++) on[i] = (flags[i] & 1) != 0;

    unsigned s = 0;
    for (int c = 0; c < nc; c++) {
        unsigned e = rd_u16(g + 10 + (size_t)c * 2);
        if (e < s || e >= npoints) break;
        if (!emit_contour(out, px + s, py + s, on + s, (int)(e - s + 1))) {
            svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on);
            return -1;
        }
        (*ncontours)++;
        s = e + 1;
    }
    svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on);
    return 0;
bad:
    svg_mfree(flags); svg_mfree(px); svg_mfree(py); svg_mfree(on);
    return -1;
}

static int parse_glyph(const FontFace* f, unsigned gid, PtList* out,
                       int* ncontours, int max_contours, int depth,
                       float scale);

static int parse_composite(const FontFace* f, const unsigned char* g,
                           unsigned len, PtList* out, int* ncontours,
                           int max_contours, int depth, float scale) {
    if (depth > 4) return -1;
    unsigned p = 10;
    for (;;) {
        if (p + 4 > len) return -1;
        unsigned flags = rd_u16(g + p);
        unsigned idx = rd_u16(g + p + 2);
        p += 4;
        float dx = 0, dy = 0;
        if (flags & 1) {
            if (p + 4 > len) return -1;
            dx = (float)rd_i16(g + p);
            dy = (float)rd_i16(g + p + 2);
            p += 4;
        } else {
            if (p + 2 > len) return -1;
            dx = (float)(signed char)g[p];
            dy = (float)(signed char)g[p + 1];
            p += 2;
        }
        float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
        if (flags & 8) {
            if (p + 2 > len) return -1;
            a = d = (float)rd_i16(g + p) / 16384.0f;
            p += 2;
        } else if (flags & 0x40) {
            if (p + 4 > len) return -1;
            a = (float)rd_i16(g + p) / 16384.0f;
            d = (float)rd_i16(g + p + 2) / 16384.0f;
            p += 4;
        } else if (flags & 0x80) {
            if (p + 8 > len) return -1;
            a = (float)rd_i16(g + p) / 16384.0f;
            b = (float)rd_i16(g + p + 2) / 16384.0f;
            c = (float)rd_i16(g + p + 4) / 16384.0f;
            d = (float)rd_i16(g + p + 6) / 16384.0f;
            p += 8;
        }
        if (flags & 2) { dx = 0; dy = 0; }

        int before = out->n;
        int added = 0;
        if (parse_glyph(f, idx, out, &added, max_contours, depth + 1, scale) == 0) {
            *ncontours += added;

            for (int i = before; i < out->n; i += 2) {
                float x = out->xy[i], y = out->xy[i + 1];
                out->xy[i] = a * x + c * y + dx * scale;
                out->xy[i + 1] = b * x + d * y + dy * scale;
            }
        }
        if (!(flags & 0x20)) break;
    }
    return 0;
}

static int parse_glyph(const FontFace* f, unsigned gid, PtList* out,
                       int* ncontours, int max_contours, int depth,
                       float scale) {
    if (!f || gid >= f->numGlyphs || depth > 4) return -1;
    if (!f->loca || !f->glyf) return -1;
    unsigned start, end;
    if (f->indexToLocFormat == 0) {
        if ((size_t)gid * 2 + 4 > f->loca_len) return -1;
        start = (unsigned)rd_u16(f->loca + (size_t)gid * 2) * 2u;
        end = (unsigned)rd_u16(f->loca + (size_t)(gid + 1) * 2) * 2u;
    } else {
        if ((size_t)gid * 4 + 8 > f->loca_len) return -1;
        start = rd_u32(f->loca + (size_t)gid * 4);
        end = rd_u32(f->loca + (size_t)(gid + 1) * 4);
    }
    if (end <= start || start >= f->glyf_len) return 0;
    if (end > f->glyf_len) end = f->glyf_len;
    const unsigned char* g = f->glyf + start;
    unsigned len = end - start;
    int nc = rd_i16(g);
    if (nc >= 0) {
        int added = 0;
        if (parse_simple(g, len, out, &added, max_contours, scale)) return -1;
        *ncontours += added;
        return 0;
    }
    return parse_composite(f, g, len, out, ncontours, max_contours, depth + 1,
                           scale);
}

FontFace* font_face_load(const char* path) {
    if (!path || !*path) return NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > (long)(64u << 20)) { fclose(fp); return NULL; }
    rewind(fp);
    unsigned char* buf = (unsigned char*)svg_malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { svg_mfree(buf); return NULL; }

    FontFace* f = (FontFace*)svg_malloc(sizeof(FontFace));
    if (!f) { svg_mfree(buf); return NULL; }
    memset(f, 0, sizeof(*f));

    size_t base = 0;
    if (sz >= 16 && !memcmp(buf, "ttcf", 4)) {
        unsigned count = rd_u32(buf + 8);
        if (count > 0) base = rd_u32(buf + 12);
        if (base + 12 > (size_t)sz) { svg_mfree(buf); svg_mfree(f); return NULL; }
    }
    f->f.d = buf;
    f->f.n = (size_t)sz;
    f->f.base = base;
    f->f.owned = buf;
    f->unitsPerEm = 1000.0f;
    f->ascent = 800.0f;
    f->descent = -200.0f;

    unsigned head_len = 0, maxp_len = 0, hhea_len = 0;
    const unsigned char* head = find_table(&f->f, "head", &head_len);
    const unsigned char* maxp = find_table(&f->f, "maxp", &maxp_len);
    const unsigned char* hhea = find_table(&f->f, "hhea", &hhea_len);
    const unsigned char* loca = find_table(&f->f, "loca", &f->loca_len);
    const unsigned char* glyf = find_table(&f->f, "glyf", &f->glyf_len);
    const unsigned char* hmtx = find_table(&f->f, "hmtx", &f->hmtx_len);
    const unsigned char* cmap = find_table(&f->f, "cmap", &f->cmap_len);
    if (!head || !maxp || !loca || !glyf || !hhea) goto bad;

    if (head_len < 54 || maxp_len < 6 || hhea_len < 36) goto bad;
    if (hmtx && f->hmtx_len < 4) goto bad;
    if (cmap && f->cmap_len < 4) goto bad;

    f->unitsPerEm = (unsigned)rd_u16(head + 18);
    f->indexToLocFormat = rd_i16(head + 50);
    if (f->unitsPerEm == 0) f->unitsPerEm = 1000;
    f->numGlyphs = (unsigned)rd_u16(maxp + 4);
    if (f->numGlyphs == 0) goto bad;
    f->numberOfHMetrics = (int)rd_u16(hhea + 34);
    if (f->numberOfHMetrics == 0) f->numberOfHMetrics = 1;
    f->ascent = (float)rd_i16(hhea + 4);
    f->descent = (float)rd_i16(hhea + 6);
    f->loca = loca;
    f->glyf = glyf;
    f->hmtx = hmtx;
    f->cmap = cmap;
    if (getenv("SVG2IMG_DEBUG_FONT"))
        fprintf(stderr, "font: upem=%u glyphs=%u ih=%d asc=%.0f desc=%.0f hm=%d\n",
                (unsigned)f->unitsPerEm, f->numGlyphs, f->indexToLocFormat,
                (double)f->ascent, (double)f->descent, f->numberOfHMetrics);
    return f;
bad:
    svg_mfree(buf);
    svg_mfree(f);
    return NULL;
}

void font_face_free(FontFace* f) {
    if (!f) return;

    svg_mfree(f->f.owned);
    svg_mfree(f);
}

float font_face_advance(const FontFace* f, int gid) {
    if (!f || !f->hmtx) return 0.0f;
    unsigned i = (unsigned)gid;
    if (i >= f->numGlyphs) i = f->numGlyphs ? f->numGlyphs - 1 : 0;
    unsigned idx = i < (unsigned)f->numberOfHMetrics ? i
                                                     : (unsigned)f->numberOfHMetrics - 1;
    if (idx >= (unsigned)f->numberOfHMetrics) return 0.0f;

    if (4u * idx + 2 > f->hmtx_len) return 0.0f;
    return (float)(int)rd_u16(f->hmtx + 4u * idx) / (float)f->unitsPerEm;
}

int font_face_glyph_index(const FontFace* f, unsigned ch) {
    return f ? glyph_index(f, ch) : 0;
}

int font_face_contours(const FontFace* f, int gid, float size, PtList* out,
                       int* ncontours) {
    if (!f || !out) return -1;
    int nc = 0;
    out->nc = 0;
    float scale = size / (float)f->unitsPerEm;
    if (parse_glyph(f, (unsigned)gid, out, &nc, 256, 0, scale)) return -1;
    *ncontours = nc;
    return 0;
}

float font_face_metrics(const FontFace* f, float* ascent, float* descent) {
    if (ascent) *ascent = f->ascent / (float)f->unitsPerEm;
    if (descent) *descent = f->descent / (float)f->unitsPerEm;
    return (float)f->unitsPerEm;
}
