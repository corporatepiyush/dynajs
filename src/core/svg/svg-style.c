#include "svg-int.h"
#include <ctype.h>
#include <strings.h>

Style style_default(void) {
    Style s;
    memset(&s, 0, sizeof(s));
    s.fill.type = PAINT_COLOR;
    s.fill.color = (Color){0, 0, 0, 1};
    s.fill.tm = mat_identity();
    s.stroke.type = PAINT_NONE;
    s.stroke.tm = mat_identity();
    s.stroke_width = 1.0f;
    s.miterlimit = 4.0f;
    s.cliprule = -1;
    s.fillop = s.strokeop = 1.0f;
    s.color = (Color){0, 0, 0, 1};
    s.stop_color = (Color){0, 0, 0, 1};
    s.stop_opacity = 1.0f;
    s.flood_color = (Color){0, 0, 0, 1};
    s.flood_opacity = 1.0f;
    s.lighting_color = (Color){1, 1, 1, 1};
    s.fontsize = 16.0f;
    s.marker_start = s.marker_mid = s.marker_end = NULL;
    s.filter = NULL;
    s.textlen = 0.0f;
    s.length_kind = 0;
    s.writing = 0;
    s.baseline = 0.0f;
    s.rot = NULL;
    s.nrot = 0;
    return s;
}

void style_copy(Style* dst, const Style* src) {
    *dst = *src;

    dst->saw_fill = 0;
    dst->saw_stroke = 0;

    dst->spec = 0;

    dst->fill = src->fill;
    dst->stroke = src->stroke;
    dst->fill.stops = NULL;
    dst->fill.nstops = 0;
    dst->fill.ref = src->fill.ref ? svg_strdup(src->fill.ref) : NULL;
    dst->stroke.stops = NULL;
    dst->stroke.nstops = 0;
    dst->stroke.ref = src->stroke.ref ? svg_strdup(src->stroke.ref) : NULL;
    if (src->fill.stops && src->fill.nstops > 0) {
        dst->fill.stops = (Stop*)svg_malloc(
            (size_t)src->fill.nstops * sizeof(Stop));
        if (dst->fill.stops) {
            memcpy(dst->fill.stops, src->fill.stops,
                   (size_t)src->fill.nstops * sizeof(Stop));
            dst->fill.nstops = src->fill.nstops;
        }
    }
    if (src->stroke.stops && src->stroke.nstops > 0) {
        dst->stroke.stops = (Stop*)svg_malloc(
            (size_t)src->stroke.nstops * sizeof(Stop));
        if (dst->stroke.stops) {
            memcpy(dst->stroke.stops, src->stroke.stops,
                   (size_t)src->stroke.nstops * sizeof(Stop));
            dst->stroke.nstops = src->stroke.nstops;
        }
    }

    dst->dash = NULL;
    dst->ndash = 0;
    svg_mfree(dst->marker_start); svg_mfree(dst->marker_mid); svg_mfree(dst->marker_end);
    dst->marker_start = src->marker_start ? svg_strdup(src->marker_start) : NULL;
    dst->marker_mid   = src->marker_mid   ? svg_strdup(src->marker_mid)   : NULL;
    dst->marker_end   = src->marker_end   ? svg_strdup(src->marker_end)   : NULL;
    svg_mfree(dst->filter);
    dst->filter = src->filter ? svg_strdup(src->filter) : NULL;
    svg_mfree(dst->rot);
    dst->rot = NULL;
    dst->nrot = 0;
    if (src->rot && src->nrot > 0) {
        dst->rot = (float*)svg_malloc((size_t)src->nrot * sizeof(float));
        if (dst->rot) {
            memcpy(dst->rot, src->rot, (size_t)src->nrot * sizeof(float));
            dst->nrot = src->nrot;
        }
    }
    if (src->dash && src->ndash > 0) {
        dst->dash = (float*)svg_malloc((size_t)src->ndash * sizeof(float));
        if (dst->dash) {
            memcpy(dst->dash, src->dash, (size_t)src->ndash * sizeof(float));
            dst->ndash = src->ndash;
        }
    }
}

void style_free(Style* s) {
    svg_mfree(s->dash);
    s->dash = NULL;
    s->ndash = 0;
    for (int k = 0; k < 2; k++) {
        Paint* p = (k == 0) ? &s->fill : &s->stroke;
        svg_mfree(p->stops); p->stops = NULL; p->nstops = 0;
        svg_mfree(p->ref);   p->ref = NULL;
    }
    svg_mfree(s->marker_start); s->marker_start = NULL;
    svg_mfree(s->marker_mid);   s->marker_mid = NULL;
    svg_mfree(s->marker_end);   s->marker_end = NULL;
    svg_mfree(s->filter);       s->filter = NULL;
    svg_mfree(s->rot);          s->rot = NULL; s->nrot = 0;
}
static void for_each_decl(const char* decls,
                          void (*fn)(void* ud, const char* k, const char* v),
                          void* ud) {
    if (!decls) return;
    size_t mark = svg_scratch_mark();
    size_t n = strlen(decls) + 1;
    char* copy = (char*)svg_scratch_alloc(n);
    if (!copy) { svg_scratch_release(mark); return; }
    memcpy(copy, decls, n);
    char* p = copy;
    while (p && *p) {
        char* semi = strchr(p, ';');
        if (semi) *semi = 0;
        char* colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            char* key = p;
            char* val = colon + 1;
            while (*key && isspace((unsigned char)*key)) key++;
            char* ke = key + strlen(key);
            while (ke > key && isspace((unsigned char)ke[-1])) *--ke = 0;
            while (*val && isspace((unsigned char)*val)) val++;
            char* ve = val + strlen(val);
            while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
            if (*key && *val) fn(ud, key, val);
        }
        p = semi ? semi + 1 : NULL;
    }
    svg_scratch_release(mark);
}
struct ApplyCtx {
    Ctx*   ctx;
    Style* st;
};

static int paint_apply(Paint* p, const char* v) {
    Paint np = *p;
    np.stops = NULL; np.nstops = 0;
    np.ref = NULL;
    np.pat = NULL;

    const char* q = v;
    while (isspace((unsigned char)*q)) q++;
    if (strncmp(q, "url(", 4) == 0) {
        const char* e = strchr(q + 4, ')');
        if (!e) return 0;
        char* ref = svg_strndup(q + 4, (size_t)(e - (q + 4)));
        if (!ref) return 0;
        char* t = ref;
        while (*t && isspace((unsigned char)*t)) t++;
        char* te = t + strlen(t);
        while (te > t && isspace((unsigned char)te[-1])) *--te = 0;
        if (*t == '#') t++;
        if (!*t) { svg_mfree(ref); return 0; }

        const char* f = e + 1;
        while (isspace((unsigned char)*f)) f++;
        np.fb_type = 0;
        if (*f) {
            if (!strcmp(f, "none")) np.fb_type = PAINT_NONE;
            else if (!strcasecmp(f, "currentcolor")) np.fb_type = PAINT_CURRENT_COLOR;
            else if (color_parse(f, &np.fb_color)) np.fb_type = PAINT_COLOR;
            else { svg_mfree(ref); return 0; }
        }
        np.type = PAINT_REF;
        np.ref = svg_strdup(t);
        svg_mfree(ref);
        if (!np.ref) return 0;
    } else if (!strcmp(q, "none")) {
        np.type = PAINT_NONE;
    } else if (!strcasecmp(q, "currentcolor")) {
        np.type = PAINT_CURRENT_COLOR;
    } else {
        Color c;
        if (!color_parse(q, &c)) return 0;
        np.type = PAINT_COLOR;
        np.color = c;
    }
    svg_mfree(p->stops);
    svg_mfree(p->ref);
    *p = np;
    return 1;
}

static int dash_parse_one(Ctx* ctx, const char* q, const char** next, float* out) {
    char* end = NULL;
    float raw = strtof(q, &end);
    if (end == q || !isfinite(raw)) return 0;
    const char* token_end = end;
    int has_unit = 0;
    if (*end == '%') {
        token_end = end + 1;
        has_unit = 1;
    } else if (isalpha((unsigned char)*end)) {
        const char* ue = end;
        while (isalpha((unsigned char)*ue)) ue++;
        size_t n = (size_t)(ue - end);
        static const char* units[] = {"px", "pt", "pc", "in", "cm", "mm", "em", "ex"};
        int known = 0;
        for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++)
            if (n == strlen(units[i]) && !strncmp(end, units[i], n)) { known = 1; break; }
        if (!known) return 0;
        token_end = ue;
        has_unit = 1;
    }
    if (has_unit && *token_end && !isspace((unsigned char)*token_end) &&
        *token_end != ',')
        return 0;
    size_t len = (size_t)(token_end - q);
    char tok[64];
    if (len >= sizeof(tok)) return 0;
    memcpy(tok, q, len);
    tok[len] = 0;
    float v = length_parse(tok, ctx->vw, ctx->vh, ctx->em, 2, NAN);
    if (!isfinite(v) || v < 0.0f) return 0;
    *out = v;
    *next = token_end;
    return 1;
}

static void paint_inherit(Paint* dst, const Paint* src) {
    if (dst == src) return;
    svg_mfree(dst->stops);
    svg_mfree(dst->ref);
    *dst = *src;
    dst->stops = NULL;
    dst->nstops = 0;
    dst->ref = src->ref ? svg_strdup(src->ref) : NULL;
    if (src->stops && src->nstops > 0) {
        dst->stops = (Stop*)svg_malloc((size_t)src->nstops * sizeof(Stop));
        if (dst->stops) {
            memcpy(dst->stops, src->stops, (size_t)src->nstops * sizeof(Stop));
            dst->nstops = src->nstops;
        }
    }
}

static void apply_inherit(Ctx* ctx, Style* st, const char* k) {
    const Style* p = ctx->inherit_from;
    if (!p) return;
    if (!strcmp(k, "fill")) {
        paint_inherit(&st->fill, &p->fill);
        st->saw_fill = 1;
    } else if (!strcmp(k, "stroke")) {
        paint_inherit(&st->stroke, &p->stroke);
        st->saw_stroke = 1;
    } else if (!strcmp(k, "stroke-width")) {
        st->stroke_width = p->stroke_width; st->spec |= SPEC_STROKE_WIDTH;
    } else if (!strcmp(k, "stroke-linecap")) {
        st->linecap = p->linecap; st->spec |= SPEC_LINECAP;
    } else if (!strcmp(k, "stroke-linejoin")) {
        st->linejoin = p->linejoin; st->spec |= SPEC_LINEJOIN;
    } else if (!strcmp(k, "stroke-miterlimit")) {
        st->miterlimit = p->miterlimit; st->spec |= SPEC_MITERLIMIT;
    } else if (!strcmp(k, "stroke-dasharray")) {
        svg_mfree(st->dash);
        st->dash = NULL; st->ndash = 0;
        if (p->dash && p->ndash > 0) {
            st->dash = (float*)svg_malloc((size_t)p->ndash * sizeof(float));
            if (st->dash) {
                memcpy(st->dash, p->dash, (size_t)p->ndash * sizeof(float));
                st->ndash = p->ndash;
            }
        }
        st->spec |= SPEC_DASH;
    } else if (!strcmp(k, "stroke-dashoffset")) {
        st->dashoff = p->dashoff; st->spec |= SPEC_DASH_OFFSET;
    } else if (!strcmp(k, "fill-rule")) {
        st->fillrule = p->fillrule; st->spec |= SPEC_FILL_RULE;
    } else if (!strcmp(k, "clip-rule")) {
        st->cliprule = p->cliprule; st->spec |= SPEC_CLIP_RULE;
    } else if (!strcmp(k, "fill-opacity")) {
        st->fillop = p->fillop; st->spec |= SPEC_FILL_OPACITY;
    } else if (!strcmp(k, "stroke-opacity")) {
        st->strokeop = p->strokeop; st->spec |= SPEC_STROKE_OPACITY;
    } else if (!strcmp(k, "color")) {
        st->color = p->color;
    } else if (!strcmp(k, "flood-color")) {
        st->flood_color = p->flood_color;
    } else if (!strcmp(k, "flood-opacity")) {
        st->flood_opacity = p->flood_opacity;
    } else if (!strcmp(k, "lighting-color")) {
        st->lighting_color = p->lighting_color;
    } else if (!strcmp(k, "stop-color")) {
        st->stop_color = p->stop_color;
    } else if (!strcmp(k, "stop-opacity")) {
        st->stop_opacity = p->stop_opacity;
    } else if (!strcmp(k, "font-size")) {
        st->fontsize = p->fontsize; st->spec |= SPEC_FONT_SIZE;
    } else if (!strcmp(k, "font-weight")) {
        st->bold = p->bold; st->spec |= SPEC_FONT_WEIGHT;
    } else if (!strcmp(k, "font-style")) {
        st->italic = p->italic; st->spec |= SPEC_FONT_STYLE;
    } else if (!strcmp(k, "text-anchor")) {
        st->anchor = p->anchor; st->spec |= SPEC_TEXT_ANCHOR;
    } else if (!strcmp(k, "letter-spacing")) {
        st->letterspacing = p->letterspacing; st->spec |= SPEC_LETTER_SP;
    } else if (!strcmp(k, "marker-start") || !strcmp(k, "marker-mid") ||
               !strcmp(k, "marker-end")) {
        char** slot = !strcmp(k, "marker-start") ? &st->marker_start
                      : !strcmp(k, "marker-mid") ? &st->marker_mid
                                                 : &st->marker_end;
        const char* src = !strcmp(k, "marker-start") ? p->marker_start
                          : !strcmp(k, "marker-mid") ? p->marker_mid
                                                     : p->marker_end;
        svg_mfree(*slot);
        *slot = src ? svg_strdup(src) : NULL;
    } else if (!strcmp(k, "textLength")) {
        st->textlen = p->textlen;
    } else if (!strcmp(k, "lengthAdjust")) {
        st->length_kind = p->length_kind;
    } else if (!strcmp(k, "writing-mode")) {
        st->writing = p->writing;
    } else if (!strcmp(k, "dominant-baseline") || !strcmp(k, "alignment-baseline")) {
        st->baseline = p->baseline;
    } else if (!strcmp(k, "shape-rendering")) {
        st->crisp = p->crisp;
    } else if (!strcmp(k, "color-interpolation")) {
        st->fill.linear = p->fill.linear;
        st->stroke.linear = p->stroke.linear;
    } else if (!strcmp(k, "filter")) {
        svg_mfree(st->filter);
        st->filter = p->filter ? svg_strdup(p->filter) : NULL;
    } else if (!strcmp(k, "display")) {
        st->display_none = p->display_none;
    } else if (!strcmp(k, "visibility")) {
        st->visibility_hidden = p->visibility_hidden;
        st->spec |= SPEC_VISIBILITY;
    }
}

static void apply_one(void* ud, const char* k, const char* v);

static int font_size_token(const char* t) {
    static const char* const kw[] = {"xx-small", "x-small", "small", "medium",
                                     "large", "x-large", "xx-large",
                                     "smaller", "larger"};
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++)
        if (!strcmp(t, kw[i])) return 1;
    return (t[0] >= '0' && t[0] <= '9') || (t[0] == '.' && t[1] >= '0' && t[1] <= '9');
}

static void font_shorthand(void* ud, const char* v) {
    const char* style = "normal";
    const char* weight = "normal";
    char tok[64], size[64];
    const char* q = v;
    for (;;) {
        while (isspace((unsigned char)*q)) q++;
        const char* e = q;
        while (*e && !isspace((unsigned char)*e) && *e != '/') e++;
        size_t n = (size_t)(e - q);
        if (n == 0 || n >= sizeof tok) return;
        memcpy(tok, q, n);
        tok[n] = 0;
        q = e;
        if (!strcmp(tok, "normal") || !strcmp(tok, "small-caps")) continue;
        if (!strcmp(tok, "italic") || !strcmp(tok, "oblique")) {
            style = !strcmp(tok, "italic") ? "italic" : "oblique";
            continue;
        }
        if (!strcmp(tok, "bold") || !strcmp(tok, "bolder") || !strcmp(tok, "lighter")) {
            weight = !strcmp(tok, "lighter") ? "lighter" : "bold";
            continue;
        }
        if (n == 3 && tok[0] >= '1' && tok[0] <= '9' && tok[1] == '0' && tok[2] == '0') {
            weight = tok[0] >= '6' ? "bold" : "normal";
            continue;
        }
        if (!font_size_token(tok)) return;
        memcpy(size, tok, n + 1);
        break;
    }

    while (isspace((unsigned char)*q)) q++;
    if (*q == '/') {
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (!*q || isspace((unsigned char)*q)) return;
        while (*q && !isspace((unsigned char)*q)) q++;
        while (isspace((unsigned char)*q)) q++;
    }
    if (!*q) return;
    apply_one(ud, "font-style", style);
    apply_one(ud, "font-weight", weight);
    apply_one(ud, "font-size", size);
}

static void apply_one(void* ud, const char* k, const char* v) {
    struct ApplyCtx* ac = (struct ApplyCtx*)ud;
    Ctx* ctx = ac->ctx;
    Style* st = ac->st;

    if (!strcasecmp(v, "inherit")) {
        if (!strcmp(k, "marker")) {
            apply_inherit(ctx, st, "marker-start");
            apply_inherit(ctx, st, "marker-mid");
            apply_inherit(ctx, st, "marker-end");
        } else if (!strcmp(k, "font")) {
            apply_inherit(ctx, st, "font-style");
            apply_inherit(ctx, st, "font-weight");
            apply_inherit(ctx, st, "font-size");
        } else {
            apply_inherit(ctx, st, k);
        }
        return;
    }

    if (!strcmp(k, "marker")) {
        apply_one(ud, "marker-start", v);
        apply_one(ud, "marker-mid", v);
        apply_one(ud, "marker-end", v);
        return;
    }
    if (!strcmp(k, "font")) { font_shorthand(ud, v); return; }

    if (!strcmp(k, "fill")) {
        if (paint_apply(&st->fill, v)) st->saw_fill = 1;
    } else if (!strcmp(k, "stroke")) {
        if (paint_apply(&st->stroke, v)) st->saw_stroke = 1;
    } else if (!strcmp(k, "stroke-width")) {
        float f = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, -1.0f);
        if (f >= 0.0f && isfinite(f)) {
            st->spec |= SPEC_STROKE_WIDTH;
            st->stroke_width = f;
        }
    } else if (!strcmp(k, "stroke-linecap")) {
        st->spec |= SPEC_LINECAP;
        st->linecap = !strcmp(v, "round") ? 1 : !strcmp(v, "square") ? 2 : 0;
    } else if (!strcmp(k, "stroke-linejoin")) {
        st->spec |= SPEC_LINEJOIN;
        st->linejoin = !strcmp(v, "round") ? 1 : !strcmp(v, "bevel") ? 2 : 0;
    } else if (!strcmp(k, "stroke-miterlimit")) {
        char* end = NULL;
        float f = strtof(v, &end);
        while (end && isspace((unsigned char)*end)) end++;

        if (end != v && *end == 0 && isfinite(f) && f >= 1.0f) {
            st->spec |= SPEC_MITERLIMIT;
            st->miterlimit = f;
        }
    } else if (!strcmp(k, "stroke-dasharray")) {
        float* dash = NULL;
        int ndash = 0, cap = 0, ok = 1, all_zero = 1;
        if (strcmp(v, "none")) {
            const char* q = v;
            while (*q) {
                while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
                if (!*q) break;
                float f = 0.0f;
                if (!dash_parse_one(ctx, q, &q, &f)) { ok = 0; break; }
                if (f != 0.0f) all_zero = 0;
                if (ndash >= 512) { ok = 0; break; }
                if (ndash == cap) {
                    cap = cap ? cap * 2 : 4;
                    float* nd = (float*)svg_realloc(dash, (size_t)cap * sizeof(float));
                    if (!nd) { ok = 0; break; }
                    dash = nd;
                }
                dash[ndash++] = f;
            }
        }
        if (ok) {
            st->spec |= SPEC_DASH;
            svg_mfree(st->dash);
            st->dash = dash;
            st->ndash = ndash;

            if (all_zero) {
                svg_mfree(st->dash);
                st->dash = NULL;
                st->ndash = 0;
            }
        } else {
            svg_mfree(dash);
        }
    } else if (!strcmp(k, "stroke-dashoffset")) {
        st->spec |= SPEC_DASH_OFFSET;
        st->dashoff = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
    } else if (!strcmp(k, "fill-rule")) {
        st->spec |= SPEC_FILL_RULE;
        st->fillrule = !strcmp(v, "evenodd");
    } else if (!strcmp(k, "clip-rule")) {
        st->spec |= SPEC_CLIP_RULE;
        st->cliprule = !strcmp(v, "evenodd") ? 1 : 0;
    } else if (!strcmp(k, "fill-opacity")) {
        st->spec |= SPEC_FILL_OPACITY;
        float f = strtof(v, NULL);
        st->fillop = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    } else if (!strcmp(k, "stroke-opacity")) {
        st->spec |= SPEC_STROKE_OPACITY;
        float f = strtof(v, NULL);
        st->strokeop = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    } else if (!strcmp(k, "color")) {
        Color c;
        if (color_parse(v, &c)) st->color = c;
    } else if (!strcmp(k, "flood-color") || !strcmp(k, "lighting-color")) {
        Color* dst = (k[0] == 'f') ? &st->flood_color : &st->lighting_color;
        if (!strcasecmp(v, "currentcolor")) {
            *dst = st->color;
        } else {
            Color c;
            if (color_parse(v, &c)) *dst = c;
        }
    } else if (!strcmp(k, "flood-opacity")) {
        char* end = NULL;
        float f = strtof(v, &end);
        if (end != v && isfinite(f))
            st->flood_opacity = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    } else if (!strcmp(k, "stop-color")) {
        if (!strcasecmp(v, "currentcolor")) {
            st->stop_color = st->color;
        } else {
            Color c;
            if (color_parse(v, &c)) st->stop_color = c;
        }
    } else if (!strcmp(k, "stop-opacity")) {
        char* end = NULL;
        float f = strtof(v, &end);
        while (end && isspace((unsigned char)*end)) end++;
        if (end != v && end && *end == 0 && isfinite(f)) {
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            st->stop_opacity = f;
        }
    } else if (!strcmp(k, "font-size")) {
        st->spec |= SPEC_FONT_SIZE;
        if (!strcmp(v, "xx-small")) st->fontsize = 8.0f;
        else if (!strcmp(v, "x-small")) st->fontsize = 10.0f;
        else if (!strcmp(v, "small")) st->fontsize = 13.0f;
        else if (!strcmp(v, "medium")) st->fontsize = 16.0f;
        else if (!strcmp(v, "large")) st->fontsize = 18.0f;
        else if (!strcmp(v, "x-large")) st->fontsize = 24.0f;
        else if (!strcmp(v, "xx-large")) st->fontsize = 32.0f;
        else st->fontsize = length_parse(v, ctx->em, ctx->em, ctx->em, 2, 16.0f);
    } else if (!strcmp(k, "font-weight")) {
        st->spec |= SPEC_FONT_WEIGHT;
        if (!strcmp(v, "bold") || !strcmp(v, "bolder")) st->bold = 1;
        else if (!strcmp(v, "normal") || !strcmp(v, "lighter")) st->bold = 0;
        else st->bold = ((int)strtof(v, NULL) >= 600);
    } else if (!strcmp(k, "font-style")) {
        st->spec |= SPEC_FONT_STYLE;
        st->italic = (!strcmp(v, "italic") || !strcmp(v, "oblique"));
    } else if (!strcmp(k, "text-anchor")) {
        st->spec |= SPEC_TEXT_ANCHOR;
        st->anchor = !strcmp(v, "middle") ? 1 : !strcmp(v, "end") ? 2 : 0;
    } else if (!strcmp(k, "letter-spacing")) {
        st->spec |= SPEC_LETTER_SP;
        st->letterspacing = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
    } else if (!strcmp(k, "marker-start") || !strcmp(k, "marker-mid") ||
               !strcmp(k, "marker-end")) {
        char** slot = !strcmp(k, "marker-start") ? &st->marker_start
                      : !strcmp(k, "marker-mid") ? &st->marker_mid
                                                 : &st->marker_end;
        svg_mfree(*slot);
        *slot = NULL;
        if (v && !strcmp(v, "none")) {

        } else if (v && !strncmp(v, "url(#", 5)) {
            const char* e = strchr(v + 5, ')');
            if (e) *slot = svg_strndup(v + 5, (size_t)(e - (v + 5)));
        }
    } else if (!strcmp(k, "textLength")) {
        float f = length_parse(v, ctx->vw, ctx->vh, ctx->em, 2, 0.0f);
        st->textlen = f > 0.0f ? f : 0.0f;
    } else if (!strcmp(k, "lengthAdjust")) {
        st->length_kind = !strcmp(v, "spacingAndGlyphs") ? 1 : 0;
    } else if (!strcmp(k, "writing-mode")) {
        st->writing = (!strcmp(v, "tb") || !strcmp(v, "vertical-rl") ||
                       !strcmp(v, "vertical-lr")) ? 1 : 0;
    } else if (!strcmp(k, "dominant-baseline") || !strcmp(k, "alignment-baseline")) {

        if (!strcmp(v, "central") || !strcmp(v, "middle")) st->baseline = 0.5f;
        else if (!strcmp(v, "hanging")) st->baseline = 0.8f;
        else if (!strcmp(v, "text-before-edge")) st->baseline = 0.8f;
        else if (!strcmp(v, "text-after-edge")) st->baseline = 0.0f;
        else if (v[0] == '-' || (v[0] >= '0' && v[0] <= '9')) {
            float f = length_parse(v, ctx->vw, ctx->vh, ctx->em, 1, 0.0f);
            st->baseline = ctx->em > 0.0f ? f / ctx->em : 0.0f;
        } else st->baseline = 0.0f;
    } else if (!strcmp(k, "shape-rendering")) {
        st->crisp = (!strcmp(v, "crispEdges") || !strcmp(v, "optimizeSpeed"));
    } else if (!strcmp(k, "color-interpolation")) {

        st->fill.linear = !strcmp(v, "linearRGB");
        st->stroke.linear = !strcmp(v, "linearRGB");
    } else if (!strcmp(k, "filter")) {
        svg_mfree(st->filter);
        st->filter = NULL;
        if (v && !strncmp(v, "url(#", 5)) {
            const char* e = strchr(v + 5, ')');
            if (e) st->filter = svg_strndup(v + 5, (size_t)(e - (v + 5)));
        }
    } else if (!strcmp(k, "display")) {

        st->display_none = !strcmp(v, "none");
    } else if (!strcmp(k, "visibility")) {

        st->spec |= SPEC_VISIBILITY;
        st->visibility_hidden = (!strcmp(v, "hidden") || !strcmp(v, "collapse"));
    }
}

int style_get_decl(const char* decls, const char* key, char* out, size_t cap) {
    if (!decls || !key || !out || cap == 0) return 0;
    size_t kl = strlen(key);
    const char* p = decls;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char* ks = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { if (*p == ';') { p++; continue; } break; }
        const char* ke = p;
        while (ke > ks && isspace((unsigned char)ke[-1])) ke--;
        if ((size_t)(ke - ks) == kl && !strncmp(ks, key, kl)) {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            const char* vs = p;
            while (*p && *p != ';') p++;
            size_t vl = (size_t)(p - vs);
            while (vl > 0 && isspace((unsigned char)vs[vl - 1])) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, vs, vl);
            out[vl] = 0;
            return 1;
        }
        if (*p == ';') p++;
        else break;
    }
    return 0;
}

int style_is_presentation_attr(const char* name) {
    return strcmp(name, "marker") != 0 && strcmp(name, "font") != 0;
}

void style_apply_decls(Ctx* ctx, Style* st, const char* decls) {
    struct ApplyCtx ac = { ctx, st };
    for_each_decl(decls, apply_one, &ac);
}
