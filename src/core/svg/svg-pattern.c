#include "svg-doc.h"
#include <ctype.h>

static int par_align(const char* s) {
    if (!s) return 1;
    if (strstr(s, "none")) return 1;
    if (strstr(s, "xMin")) return 0;
    if (strstr(s, "xMax")) return 2;
    return 1;
}

static int par_valign(const char* s) {
    if (!s) return 1;
    if (strstr(s, "YMin")) return 0;
    if (strstr(s, "YMax")) return 2;
    return 1;
}

static void pat_coord(const XMLNode* xn, const char* name, float* out,
                      unsigned bit, Node* n) {
    const char* s = xml_attr(xn, name);
    if (!s) return;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return;
    while (isspace((unsigned char)*end)) end++;
    n->pat_spec |= bit;
    n->pat_pct &= ~bit;
    if (*end == '%') {
        *out = v / 100.0f;
        n->pat_pct |= bit;
    } else {
        *out = v;
    }
}

Node* build_pattern(Ctx* ctx, const XMLNode* xn) {
    Node* n = svg_node_new(NODE_PATTERNDEF);
    if (!n) return NULL;
    n->pat_vw = ctx->vw;
    n->pat_vh = ctx->vh;

    n->pat_units = UNITS_BBOX;
    const char* pu = xml_attr(xn, "patternUnits");
    if (pu) {
        n->pat_spec |= PAT_SPEC_UNITS;
        if (!strcmp(pu, "userSpaceOnUse")) n->pat_units = UNITS_USER;
    }

    n->pat_content_units = UNITS_USER;
    const char* pc = xml_attr(xn, "patternContentUnits");
    if (pc) {
        n->pat_spec |= PAT_SPEC_CONTENT_UNITS;
        if (!strcmp(pc, "objectBoundingBox")) n->pat_content_units = UNITS_BBOX;
    }

    pat_coord(xn, "x", &n->pat_x, PAT_SPEC_X, n);
    pat_coord(xn, "y", &n->pat_y, PAT_SPEC_Y, n);
    pat_coord(xn, "width", &n->pat_w, PAT_SPEC_W, n);
    pat_coord(xn, "height", &n->pat_h, PAT_SPEC_H, n);

    const char* pt = xml_attr(xn, "patternTransform");
    if (pt) {
        n->pat_spec |= PAT_SPEC_TM;
        n->pat_tm = parse_transform(pt);
    } else {
        n->pat_tm = mat_identity();
    }

    const char* vb = xml_attr(xn, "viewBox");
    if (vb) {
        n->pat_spec |= PAT_SPEC_VB;
        if (svg_parse_viewbox(vb, &n->pat_vb[0], &n->pat_vb[1],
                              &n->pat_vb[2], &n->pat_vb[3]) &&
            n->pat_vb[2] > 0.0f && n->pat_vb[3] > 0.0f)
            n->pat_vb_valid = 1;
    }

    const char* par = xml_attr(xn, "preserveAspectRatio");
    if (par) n->pat_spec |= PAT_SPEC_PAR;
    n->pat_par_none = 0;
    n->pat_par_slice = 0;
    n->pat_par_xa = par_align(par);
    n->pat_par_ya = par_valign(par);
    if (par && strstr(par, "none")) n->pat_par_none = 1;
    if (par && strstr(par, "slice")) n->pat_par_slice = 1;

    const char* href = xml_attr(xn, "href");
    if (!href) href = xml_attr(xn, "xlink:href");
    if (href) {
        const char* h = href;
        if (*h == '#') h++;
        n->fill.ref = svg_strdup(h);
    }
    return n;
}

void pat_finalize_tree(Node* n) {
    if (!n) return;
    if (n->type == NODE_PATTERNDEF && n->pat_pct) {
        if (n->pat_units == UNITS_USER) {
            if (n->pat_pct & PAT_SPEC_X) n->pat_x *= n->pat_vw;
            if (n->pat_pct & PAT_SPEC_Y) n->pat_y *= n->pat_vh;
            if (n->pat_pct & PAT_SPEC_W) n->pat_w *= n->pat_vw;
            if (n->pat_pct & PAT_SPEC_H) n->pat_h *= n->pat_vh;
        }
        n->pat_pct = 0;
    }
    for (int i = 0; i < n->nkids; i++) pat_finalize_tree(n->kids[i]);
}
