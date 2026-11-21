#include "svg-doc.h"
#include <ctype.h>

#define TEXT_MAX_DEPTH 512

static int collect_text(const XMLNode* xn, char** buf, size_t* len, size_t* cap,
                        int max_len, int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) return depth > TEXT_MAX_DEPTH;
    int truncated = 0;
    if (xn->text && *xn->text) {
        size_t tl = strlen(xn->text);
        if (*len < (size_t)max_len) {
            if (*len + tl > (size_t)max_len) tl = (size_t)max_len - *len;
            if (*len + tl + 1 > *cap) {
                size_t nc = (*len + tl + 1) * 2;
                char* nb = (char*)svg_realloc(*buf, nc);
                if (!nb) return 1;
                *buf = nb;
                *cap = nc;
            }
            memcpy(*buf + *len, xn->text, tl);
            *len += tl;
            (*buf)[*len] = 0;
            if (*len >= (size_t)max_len) truncated = 1;
        }
    }
    for (int i = 0; i < xn->nkids; i++)
        if (collect_text(xn->kids[i], buf, len, cap, max_len, depth + 1))
            truncated = 1;
    return truncated;
}

char* gather_text(const XMLNode* xn, int max_len, int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    char* buf = NULL;
    size_t len = 0, cap = 0;
    if (collect_text(xn, &buf, &len, &cap, max_len, 0) && limit_hit) *limit_hit = 1;
    if (!buf) return svg_strdup("");
    return buf;
}

static void push_chunk(Node* out, const char* s, float x, float y,
                       const Style* st, int max_len, int* limit_hit,
                       const FontFace* font) {
    if (!s || !*s) return;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    if (*limit_hit) return;
    if (out->nchunks == out->capchunks) {
        int ncap = out->capchunks ? out->capchunks * 2 : 1;
        TextChunk* nc = (TextChunk*)svg_realloc(
            out->chunks, (size_t)ncap * sizeof(TextChunk));
        if (!nc) return;
        out->chunks = nc;
        out->capchunks = ncap;
    }
    TextChunk* ch = &out->chunks[out->nchunks];
    memset(ch, 0, sizeof(*ch));
    size_t len = strlen(s);
    if ((int)len > max_len) { len = (size_t)max_len; *limit_hit = 1; }
    ch->text = (char*)svg_malloc(len + 1);
    if (!ch->text) return;
    memcpy(ch->text, s, len);
    ch->text[len] = 0;
    ch->x = x;
    ch->y = y;
    ch->fontsize = st->fontsize;
    ch->letterspacing = st->letterspacing;
    ch->bold = st->bold;
    ch->italic = st->italic;
    ch->anchor = st->anchor;
    ch->fill = st->fill;
    ch->stroke = st->stroke;
    ch->stroke_width = st->stroke_width;
    ch->linecap = st->linecap;
    ch->linejoin = st->linejoin;
    ch->miterlimit = st->miterlimit;
    ch->dash = NULL;
    ch->ndash = 0;
    if (st->dash && st->ndash > 0) {
        ch->dash = (float*)svg_malloc((size_t)st->ndash * sizeof(float));
        if (ch->dash) {
            memcpy(ch->dash, st->dash, (size_t)st->ndash * sizeof(float));
            ch->ndash = st->ndash;
        }
    }
    ch->dashoff = st->dashoff;
    ch->textlen = st->textlen;
    ch->length_kind = st->length_kind;
    ch->writing = st->writing;
    ch->baseline = st->baseline;
    ch->font = font;
    if (st->rot && st->nrot > 0) {
        ch->rot = (float*)svg_malloc((size_t)st->nrot * sizeof(float));
        if (ch->rot) {
            memcpy(ch->rot, st->rot, (size_t)st->nrot * sizeof(float));
            ch->nrot = st->nrot;
        }
    }

    for (int k = 0; k < 2; k++) {
        Paint* d = (k == 0) ? &ch->fill : &ch->stroke;
        const Paint* src = (k == 0) ? &st->fill : &st->stroke;
        d->stops = NULL;
        d->nstops = 0;
        d->ref = src->ref ? svg_strdup(src->ref) : NULL;
        if (src->stops && src->nstops > 0) {
            d->stops = (Stop*)svg_malloc((size_t)src->nstops * sizeof(Stop));
            if (d->stops) {
                memcpy(d->stops, src->stops, (size_t)src->nstops * sizeof(Stop));
                d->nstops = src->nstops;
            }
        }
    }
    out->nchunks++;
}

static int xml_space_preserve(const XMLNode* xn) {
    for (const XMLNode* p = xn; p; p = p->parent) {

        const char* v = xml_attr(p, "xml:space");
        if (!v) v = xml_attr(p, "space");
        if (!v) continue;
        if (!strcmp(v, "preserve")) return 1;
        if (!strcmp(v, "default")) return 0;
    }
    return 0;
}

static int is_xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void append_text(char** buf, size_t* len, size_t* cap, const char* s,
                        int preserve, int max_len, int* truncated,
                        int at_start) {
    size_t n = strlen(s);
    if (preserve) {
        if (*len + n > (size_t)max_len) { n = (size_t)max_len - *len; *truncated = 1; }
        if (*len + n + 1 > *cap) {
            size_t nc = (*len + n + 1) * 2;
            char* nb = (char*)svg_realloc(*buf, nc);
            if (!nb) return;
            *buf = nb; *cap = nc;
        }
        memcpy(*buf + *len, s, n);
        *len += n;
        (*buf)[*len] = 0;
        return;
    }

    for (size_t i = 0; i < n; i++) {
        if (is_xml_space(s[i])) {

            while (i + 1 < n && is_xml_space(s[i + 1])) i++;
            if (at_start && *len == 0) continue;
            char sp = ' ';
            if (*len < (size_t)max_len) {
                if (*len + 1 >= *cap) {
                    size_t nc = (*cap ? *cap * 2 : 64);
                    char* nb = (char*)svg_realloc(*buf, nc);
                    if (!nb) return;
                    *buf = nb; *cap = nc;
                }
                (*buf)[(*len)++] = sp;
                (*buf)[*len] = 0;
            } else *truncated = 1;
        } else {
            if (*len < (size_t)max_len) {
                if (*len + 2 > *cap) {
                    size_t nc = (*cap ? *cap * 2 : 64);
                    char* nb = (char*)svg_realloc(*buf, nc);
                    if (!nb) return;
                    *buf = nb; *cap = nc;
                }
                (*buf)[(*len)++] = s[i];
                (*buf)[*len] = 0;
            } else *truncated = 1;
        }
    }
}

static char* text_normalize(const XMLNode* xn, Ctx* ctx, int* limit_hit) {
    const char* src = xn->text;
    if (!src || !*src) return NULL;
    int preserve = xml_space_preserve(xn);
    int ws_only = 1;
    for (const char* q = src; *q; q++)
        if (!is_xml_space(*q)) { ws_only = 0; break; }
    if (ws_only && !preserve) return NULL;
    int max_len = ctx->lim ? ctx->lim->max_text_length
                           : SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    if (max_len <= 0) max_len = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;

    size_t src_len = strlen(src);
    size_t limit = (size_t)max_len;
    size_t cap = (src_len < limit ? src_len : limit) + 1;
    char* out = (char*)svg_malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    size_t len = 0;
    int trunc = 0;
    append_text(&out, &len, &cap, src, preserve, max_len, &trunc, 1);
    if (trunc && limit_hit) *limit_hit = 1;

    if (!preserve) {
        while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
    }
    (void)0;
    if (len == 0) { svg_mfree(out); return NULL; }
    return out;
}

static void collect_all_text(const XMLNode* xn, char** buf, size_t* len,
                             size_t* cap, int max_len, int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) return;
    if (xn->text && *xn->text) {
        int trunc = 0;
        int at_start = (xn->parent == NULL);
        append_text(buf, len, cap, xn->text, xml_space_preserve(xn),
                    max_len, &trunc, at_start);
    }
    for (int i = 0; i < xn->nkids; i++)
        collect_all_text(xn->kids[i], buf, len, cap, max_len, depth + 1);
}

static char* all_text(const XMLNode* xn, int max_len) {
    char* buf = NULL;
    size_t len = 0, cap = 0;
    collect_all_text(xn, &buf, &len, &cap, max_len, 0);
    if (!buf) return svg_strdup("");
    return buf;
}

static int apply_text_style(Ctx* ctx, const XMLNode* xn, Style* st,
                            const Style* parent) {
    const Style* saved_inherit = ctx->inherit_from;
    ctx->inherit_from = parent;
    for (int a = 0; a < xn->nattrs; a++) {
        if (!style_is_presentation_attr(xn->attrs[a * 2])) continue;
        char decl[512];
        snprintf(decl, sizeof decl, "%s:%s", xn->attrs[a * 2], xn->attrs[a * 2 + 1]);
        style_apply_decls(ctx, st, decl);
    }

    css_apply(ctx, xn, st);
    style_apply_decls(ctx, st, xml_attr(xn, "style"));
    css_apply_important(ctx, xn, st);
    ctx->inherit_from = saved_inherit;
    if (st->fill.fb_type == PAINT_CURRENT_COLOR) { st->fill.fb_type = PAINT_COLOR; st->fill.fb_color = st->color; }
    if (st->stroke.fb_type == PAINT_CURRENT_COLOR) { st->stroke.fb_type = PAINT_COLOR; st->stroke.fb_color = st->color; }
    if (st->fill.type == PAINT_CURRENT_COLOR) {
        st->fill.type = PAINT_COLOR;
        st->fill.color = st->color;
    }
    return 1;
}

static void apply_rotate(Ctx* ctx, const XMLNode* xn, Style* st) {
    const char* r = xml_attr(xn, "rotate");
    if (!r) return;
    svg_mfree(st->rot);
    st->rot = NULL;
    st->nrot = 0;
    const char* p = r;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char* end = NULL;
        float v = strtof(p, &end);
        if (end == p) break;
        float* nr = (float*)svg_realloc(st->rot, (size_t)(st->nrot + 1) * sizeof(float));
        if (!nr) return;
        st->rot = nr;
        st->rot[st->nrot++] = v;
        p = end;
    }
    (void)ctx;
}

static void walk_chunks(const XMLNode* xn, Ctx* ctx, Style* st,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth);

static int len_list(const char* s, float vw, float vh, float em, int axis,
                    float* out, int max) {
    int n = 0;
    if (!s) return 0;
    const char* p = s;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        const char* start = p;
        char* end = NULL;
        (void)strtof(p, &end);
        if (end == start) break;
        char tok[64];
        size_t tl = (size_t)(end - start);
        while (tl > 0 && isspace((unsigned char)start[tl - 1])) tl--;
        if (tl >= sizeof tok) tl = sizeof tok - 1;
        memcpy(tok, start, tl);
        tok[tl] = 0;
        out[n++] = length_parse(tok, vw, vh, em, axis, 0.0f);
        p = end;
    }
    return n;
}

static void push_positions(Node* out, const float* gx, const float* gy,
                           const float* gdx, const float* gdy,
                           int ngx, int ngy, int ngdx, int ngdy, int ng) {
    if (!out || out->nchunks <= 0 || ng <= 1) return;
    TextChunk* ch = &out->chunks[out->nchunks - 1];
    int npos = ng - 1;
    svg_mfree(ch->gx); svg_mfree(ch->gy);
    svg_mfree(ch->gdx); svg_mfree(ch->gdy);
    ch->gx = (float*)svg_malloc((size_t)npos * sizeof(float));
    ch->gy = (float*)svg_malloc((size_t)npos * sizeof(float));
    ch->gdx = (float*)svg_malloc((size_t)npos * sizeof(float));
    ch->gdy = (float*)svg_malloc((size_t)npos * sizeof(float));
    if (!ch->gx || !ch->gy || !ch->gdx || !ch->gdy) {
        svg_mfree(ch->gx); svg_mfree(ch->gy);
        svg_mfree(ch->gdx); svg_mfree(ch->gdy);
        ch->gx = ch->gy = ch->gdx = ch->gdy = NULL;
        ch->ngpos = 0;
        return;
    }
    for (int i = 0; i < npos; i++) {
        ch->gx[i]  = (ngx  > i + 1) ? gx[i + 1]  : 0.0f;
        ch->gy[i]  = (ngy  > i + 1) ? gy[i + 1]  : 0.0f;
        ch->gdx[i] = (ngdx > i + 1) ? gdx[i + 1] : 0.0f;
        ch->gdy[i] = (ngdy > i + 1) ? gdy[i + 1] : 0.0f;
    }
    ch->ngpos = npos;
}

typedef struct {
    float gx[256], gy[256], gdx[256], gdy[256];
    int ngx, ngy, ngdx, ngdy, ng;
} TextPos;

static void text_pos_parse(const XMLNode* xn, Ctx* ctx, TextPos* tp) {
    tp->ngx  = len_list(xml_attr(xn, "x"),  ctx->vw, ctx->vh, ctx->em, 0,
                        tp->gx,  256);
    tp->ngy  = len_list(xml_attr(xn, "y"),  ctx->vw, ctx->vh, ctx->em, 1,
                        tp->gy,  256);
    tp->ngdx = len_list(xml_attr(xn, "dx"), ctx->vw, ctx->vh, ctx->em, 0,
                        tp->gdx, 256);
    tp->ngdy = len_list(xml_attr(xn, "dy"), ctx->vw, ctx->vh, ctx->em, 1,
                        tp->gdy, 256);
    tp->ng = tp->ngx;
    if (tp->ngy  > tp->ng) tp->ng = tp->ngy;
    if (tp->ngdx > tp->ng) tp->ng = tp->ngdx;
    if (tp->ngdy > tp->ng) tp->ng = tp->ngdy;
}

static void text_pos_origin(const TextPos* tp, float cx, float cy,
                            float* x, float* y) {
    *x = tp->ngx > 0 ? tp->gx[0] : cx;
    *y = tp->ngy > 0 ? tp->gy[0] : cy;
    if (tp->ngdx > 0) *x += tp->gdx[0];
    if (tp->ngdy > 0) *y += tp->gdy[0];
}

static void run_element(const XMLNode* xn, Ctx* ctx, Style* parent,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth) {
    if (depth > TEXT_MAX_DEPTH) { *limit_hit = 1; return; }
    Style child;
    style_copy(&child, parent);
    apply_text_style(ctx, xn, &child, parent);
    apply_rotate(ctx, xn, &child);

    TextPos tp;
    text_pos_parse(xn, ctx, &tp);
    float nx, ny;
    text_pos_origin(&tp, *cx, *cy, &nx, &ny);

    char* own = text_normalize(xn, ctx, limit_hit);
    if (own && *own) {
        int before = out->nchunks;
        push_chunk(out, own, nx, ny, &child,
                   ctx->lim ? ctx->lim->max_text_length
                            : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                   ctx->font);
        if (out->nchunks > before) {
            push_positions(out, tp.gx, tp.gy, tp.gdx, tp.gdy,
                           tp.ngx, tp.ngy, tp.ngdx, tp.ngdy, tp.ng);
        }
    }
    float scale = child.fontsize / 7.0f;

    size_t adv_len = xn->text ? strlen(xn->text) : 0;
    if (own) {
        size_t a = strlen(own), b = strlen(xn->text);

        if (b > a) adv_len -= (b - a);
    }
    *cx = nx + (float)adv_len * (6.0f * scale + child.letterspacing);
    svg_mfree(own);
    *cy = ny;
    walk_chunks(xn, ctx, &child, cx, cy, out, limit_hit, depth + 1);
    style_free(&child);
}

static void walk_chunks(const XMLNode* xn, Ctx* ctx, Style* st,
                        float* cx, float* cy, Node* out, int* limit_hit,
                        int depth) {
    if (!xn || depth > TEXT_MAX_DEPTH) {
        if (xn) *limit_hit = 1;
        return;
    }
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* k = xn->kids[i];
        const char* nm = k->name;
        if (!strcmp(nm, "tspan") || !strcmp(nm, "a") ||
            !strcmp(nm, "altGlyph")) {
            run_element(k, ctx, st, cx, cy, out, limit_hit, depth + 1);
        } else if (!strcmp(nm, "tref")) {
            const char* href = xml_attr(k, "href");
            if (!href) href = xml_attr(k, "xlink:href");
            Style child;
            style_copy(&child, st);
            apply_text_style(ctx, k, &child, st);
            if (href && *href) {
                const char* h = (*href == '#') ? href + 1 : href;
                Node* ref = ctx->ids ? idmap_get(ctx->ids, h) : NULL;
                if (ref && ref->text && *ref->text) {
                    push_chunk(out, ref->text, *cx, *cy, &child,
                               ctx->lim ? ctx->lim->max_text_length
                                        : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                               limit_hit, ctx->font);
                }
            }
            style_free(&child);
        } else if (!strcmp(nm, "textPath")) {
            Style child;
            style_copy(&child, st);
            apply_text_style(ctx, k, &child, st);
            float start = 0.0f;
            int start_is_pct = 0;
            const char* so = xml_attr(k, "startOffset");
            if (so) {

                while (*so && isspace((unsigned char)*so)) so++;
                if (*so && strchr(so, '%')) {
                    start = strtof(so, NULL) / 100.0f;
                    start_is_pct = 1;
                } else {
                    start = length_parse(so, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
                }
            }
            char* raw = all_text(k, ctx->lim ? ctx->lim->max_text_length
                                              : SVG2IMG_LIMIT_MAX_TEXT_LENGTH);

            char* text = NULL;
            if (raw && *raw) {
                int preserve = xml_space_preserve(k);
                size_t len = 0, cap = 0;
                text = svg_strdup("");
                if (text) {
                    int trunc = 0;
                    append_text(&text, &len, &cap, raw, preserve,
                                ctx->lim ? ctx->lim->max_text_length
                                         : SVG2IMG_LIMIT_MAX_TEXT_LENGTH,
                                &trunc, 1);
                    if (trunc) *limit_hit = 1;
                }
            }
            svg_mfree(raw);
            if (text && *text) {
                push_chunk(out, text, *cx, *cy, &child,
                           ctx->lim ? ctx->lim->max_text_length
                                    : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                           ctx->font);
                if (out->nchunks > 0) {
                    TextChunk* ch = &out->chunks[out->nchunks - 1];
                    const char* d = xml_attr(k, "path");
                    if (d && *d) ch->path = svg_strdup(d);
                    else {
                        const char* href = xml_attr(k, "href");
                        if (!href) href = xml_attr(k, "xlink:href");
                        if (href && *href) {
                            const char* h = (*href == '#') ? href + 1 : href;
                            ch->path_ref = svg_strdup(h);
                        }
                    }
                    ch->start_offset = start;
                    ch->start_is_pct = start_is_pct;
                }
            }
            svg_mfree(text);
            style_free(&child);
        } else if (!strcmp(nm, "altGlyphDef") || !strcmp(nm, "altGlyphItem") ||
                   !strcmp(nm, "glyphRef")) {

        } else {
            walk_chunks(k, ctx, st, cx, cy, out, limit_hit, depth + 1);
        }
    }
}

void gather_chunks(const XMLNode* xn, Ctx* ctx, Node* out, int* limit_hit,
                   const Style* parent) {
    if (!xn || !out) return;
    Style st;
    if (parent) style_copy(&st, parent);
    else st = style_default();
    apply_text_style(ctx, xn, &st, parent);
    apply_rotate(ctx, xn, &st);

    TextPos tp;
    text_pos_parse(xn, ctx, &tp);
    float x, y;
    text_pos_origin(&tp, len_attr(ctx, xn, "x", 0, 0.0f),
                    len_attr(ctx, xn, "y", 1, 0.0f), &x, &y);

    char* own = text_normalize(xn, ctx, limit_hit);
    if (own && *own) {
        int before = out->nchunks;
        push_chunk(out, own, x, y, &st,
                   ctx->lim ? ctx->lim->max_text_length
                            : SVG2IMG_LIMIT_MAX_TEXT_LENGTH, limit_hit,
                   ctx->font);
        if (out->nchunks > before)
            push_positions(out, tp.gx, tp.gy, tp.gdx, tp.gdy,
                           tp.ngx, tp.ngy, tp.ngdx, tp.ngdy, tp.ng);
    }
    float scale = st.fontsize / 7.0f;

    size_t adv_len = xn->text ? strlen(xn->text) : 0;
    if (own) {
        size_t a = strlen(own), b = strlen(xn->text);
        if (b > a) adv_len -= (b - a);
    }
    svg_mfree(own);
    float cx = x + (float)adv_len * (6.0f * scale + st.letterspacing);
    float cy = y;
    walk_chunks(xn, ctx, &st, &cx, &cy, out, limit_hit, 0);
    style_free(&st);
}
