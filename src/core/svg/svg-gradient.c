#include "svg-doc.h"
#include <ctype.h>

int parse_stops(Ctx* ctx, const XMLNode* xn, Paint* p, int max_stops) {
    if (max_stops <= 0) max_stops = SVG2IMG_LIMIT_MAX_GRADIENT_STOPS;
    int truncated = 0;
    int cap = 0;

    Color current = {0, 0, 0, 1};
    char gsval[128];
    const char* gsdecl = xml_attr(xn, "style");
    const char* gc = xml_attr(xn, "color");
    if (style_get_decl(gsdecl, "color", gsval, sizeof gsval)) gc = gsval;
    if (gc) {
        Color cc;
        if (color_parse(gc, &cc)) current = cc;
    }

    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* c = xn->kids[i];
        if (strcmp(c->name, "stop")) continue;
        if (p->nstops >= max_stops) { truncated = 1; break; }

        float off = 0.0f;
        const char* so = xml_attr(c, "offset");
        if (so) {
            off = strtof(so, NULL);
            if (strchr(so, '%')) off /= 100.0f;
        }

        Style sst = style_default();
        sst.color = current;
        const Style* saved_inherit = ctx->inherit_from;
        ctx->inherit_from = NULL;
        char decl[512];
        const char* owncolor = xml_attr(c, "color");
        if (owncolor) {
            snprintf(decl, sizeof decl, "color:%s", owncolor);
            style_apply_decls(ctx, &sst, decl);
        }
        for (int a = 0; a < c->nattrs; a++) {
            snprintf(decl, sizeof decl, "%s:%s",
                     c->attrs[a * 2], c->attrs[a * 2 + 1]);
            style_apply_decls(ctx, &sst, decl);
        }
        css_apply(ctx, c, &sst);
        style_apply_decls(ctx, &sst, xml_attr(c, "style"));
        css_apply_important(ctx, c, &sst);
        ctx->inherit_from = saved_inherit;

        Color col = sst.stop_color;
        col.a = sst.stop_opacity;
        style_free(&sst);

        if (p->nstops == cap) {
            int ncap = cap ? cap * 2 : 4;
            if (ncap > max_stops) ncap = max_stops;
            Stop* ns = (Stop*)svg_realloc(p->stops, (size_t)ncap * sizeof(Stop));
            if (!ns) { p->nstops = 0; return 1; }
            p->stops = ns;
            cap = ncap;
        }
        p->stops[p->nstops].off = off;
        p->stops[p->nstops].c = col;
        p->nstops++;
    }

    for (int i = 0; i < p->nstops; i++) {
        float off = p->stops[i].off;
        if (!(off > 0.0f)) off = 0.0f;
        if (off > 1.0f) off = 1.0f;
        if (i > 0 && off < p->stops[i - 1].off) off = p->stops[i - 1].off;
        p->stops[i].off = off;
    }
    return truncated;
}

static void grad_coord(const XMLNode* xn, const char* name, float* out,
                       unsigned bit, Node* n) {
    const char* s = xml_attr(xn, name);
    if (!s) return;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return;
    while (isspace((unsigned char)*end)) end++;
    n->grad_spec |= bit;
    n->grad_pct &= ~bit;
    if (*end == '%') {
        *out = v / 100.0f;
        n->grad_pct |= bit;
    } else {
        *out = v;
    }
}

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial) {
    Node* n = svg_node_new(NODE_GRADDEF);
    if (!n) return NULL;
    Paint* p = &n->fill;

    n->grad_vw = ctx->vw;
    n->grad_vh = ctx->vh;

    p->type = radial ? PAINT_RADIAL : PAINT_LINEAR;
    p->units = UNITS_BBOX;

    const char* ci = xml_attr(xn, "color-interpolation");
    char cival[64];
    if (style_get_decl(xml_attr(xn, "style"), "color-interpolation", cival,
                       sizeof cival)) {
        ci = cival;
        n->grad_spec |= GRAD_SPEC_CI;
    } else if (ci) {
        n->grad_spec |= GRAD_SPEC_CI;
    }
    p->linear = (ci && !strcmp(ci, "linearRGB"));

    const char* gu = xml_attr(xn, "gradientUnits");
    if (gu) {
        n->grad_spec |= GRAD_SPEC_UNITS;
        if (!strcmp(gu, "userSpaceOnUse")) p->units = UNITS_USER;
    }

    p->spread = SPREAD_PAD;
    const char* sp = xml_attr(xn, "spreadMethod");
    if (sp) {
        n->grad_spec |= GRAD_SPEC_SPREAD;
        if (!strcmp(sp, "reflect")) p->spread = SPREAD_REFLECT;
        else if (!strcmp(sp, "repeat")) p->spread = SPREAD_REPEAT;
    }

    if (radial) {
        p->cx = p->cy = p->r = 0.5f;
        n->grad_pct |= GRAD_SPEC_CX | GRAD_SPEC_CY | GRAD_SPEC_R;
        grad_coord(xn, "cx", &p->cx, GRAD_SPEC_CX, n);
        grad_coord(xn, "cy", &p->cy, GRAD_SPEC_CY, n);
        grad_coord(xn, "r",  &p->r,  GRAD_SPEC_R, n);

        p->fx = 0.0f;
        p->fy = 0.0f;
        grad_coord(xn, "fx", &p->fx, GRAD_SPEC_FX, n);
        grad_coord(xn, "fy", &p->fy, GRAD_SPEC_FY, n);
    } else {
        p->x1 = 0.0f;
        p->y1 = 0.0f;
        p->x2 = 1.0f;
        p->y2 = 0.0f;
        n->grad_pct |= GRAD_SPEC_X2;
        grad_coord(xn, "x1", &p->x1, GRAD_SPEC_X1, n);
        grad_coord(xn, "y1", &p->y1, GRAD_SPEC_Y1, n);
        grad_coord(xn, "x2", &p->x2, GRAD_SPEC_X2, n);
        grad_coord(xn, "y2", &p->y2, GRAD_SPEC_Y2, n);
    }

    p->tm = mat_identity();
    const char* gt = xml_attr(xn, "gradientTransform");
    if (gt) {
        n->grad_spec |= GRAD_SPEC_TM;
        p->tm = parse_transform(gt);
    }

    for (int i = 0; i < xn->nkids; i++) {
        if (!strcmp(xn->kids[i]->name, "stop")) {
            n->grad_spec |= GRAD_SPEC_STOPS;
            break;
        }
    }

    const char* href = xml_attr(xn, "href");
    if (!href) href = xml_attr(xn, "xlink:href");
    if (href) {
        const char* h = href;
        if (*h == '#') h++;
        p->ref = svg_strdup(h);
    }
    return n;
}

void grad_finalize_tree(Node* n) {
    if (!n) return;
    if (n->type == NODE_GRADDEF && n->grad_pct) {
        Paint* p = &n->fill;
        if (p->units == UNITS_USER) {
            float diag = sqrtf(n->grad_vw * n->grad_vw +
                               n->grad_vh * n->grad_vh) / sqrtf(2.0f);
            if (n->grad_pct & GRAD_SPEC_X1) p->x1 *= n->grad_vw;
            if (n->grad_pct & GRAD_SPEC_Y1) p->y1 *= n->grad_vh;
            if (n->grad_pct & GRAD_SPEC_X2) p->x2 *= n->grad_vw;
            if (n->grad_pct & GRAD_SPEC_Y2) p->y2 *= n->grad_vh;
            if (n->grad_pct & GRAD_SPEC_CX) p->cx *= n->grad_vw;
            if (n->grad_pct & GRAD_SPEC_CY) p->cy *= n->grad_vh;
            if (n->grad_pct & GRAD_SPEC_R)  p->r  *= diag;
            if (n->grad_pct & GRAD_SPEC_FX) p->fx *= n->grad_vw;
            if (n->grad_pct & GRAD_SPEC_FY) p->fy *= n->grad_vh;
        }
        n->grad_pct = 0;
    }
    if (n->type == NODE_GRADDEF && n->fill.type == PAINT_RADIAL) {

        if (!(n->grad_spec & GRAD_SPEC_FX)) n->fill.fx = n->fill.cx;
        if (!(n->grad_spec & GRAD_SPEC_FY)) n->fill.fy = n->fill.cy;
    }
    for (int i = 0; i < n->nkids; i++) grad_finalize_tree(n->kids[i]);
}
