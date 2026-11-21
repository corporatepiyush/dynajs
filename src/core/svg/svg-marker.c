#include "svg-doc.h"

Node* build_marker(Ctx* ctx, const XMLNode* xn) {
    Node* n = svg_node_new(NODE_MARKERDEF);
    if (!n) return NULL;

    n->mk_units = 0;
    const char* mu = xml_attr(xn, "markerUnits");
    if (mu && !strcmp(mu, "userSpaceOnUse")) n->mk_units = 1;

    n->mk_w = len_attr(ctx, xn, "markerWidth", 0, 3.0f);
    n->mk_h = len_attr(ctx, xn, "markerHeight", 1, 3.0f);
    n->mk_refx = len_attr(ctx, xn, "refX", 0, 0.0f);
    n->mk_refy = len_attr(ctx, xn, "refY", 1, 0.0f);

    n->mk_orient = 0;
    const char* or = xml_attr(xn, "orient");
    if (or) {
        if (!strcmp(or, "auto-start-reverse")) n->mk_orient = 2;
        else if (strcmp(or, "auto")) {
            char* end = NULL;
            float a = strtof(or, &end);
            if (end != or) { n->mk_orient = 1; n->mk_angle = a; }
        }
    }

    if (svg_parse_viewbox(xml_attr(xn, "viewBox"),
                          &n->mk_vb[0], &n->mk_vb[1], &n->mk_vb[2], &n->mk_vb[3]) &&
        n->mk_vb[2] > 0.0f && n->mk_vb[3] > 0.0f)
        n->mk_vb_valid = 1;
    n->mk_par_xa = n->mk_par_ya = 1;
    n->mk_par_slice = 0;
    n->mk_par_none = 0;
    const char* par = xml_attr(xn, "preserveAspectRatio");
    if (par) {
        if (strstr(par, "none")) n->mk_par_none = 1;
        else {
            if (strstr(par, "xMin")) n->mk_par_xa = 0;
            else if (strstr(par, "xMax")) n->mk_par_xa = 2;
            if (strstr(par, "YMin")) n->mk_par_ya = 0;
            else if (strstr(par, "YMax")) n->mk_par_ya = 2;
        }
        if (strstr(par, "slice")) n->mk_par_slice = 1;
    }
    return n;
}
