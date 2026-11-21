#include "svg-doc.h"

static float mask_len(Ctx* ctx, const XMLNode* xn, const char* name, int axis,
                      int units, float def) {
    const char* s = xml_attr(xn, name);
    if (!s) return def;
    if (units == UNITS_BBOX) {
        float v = strtof(s, NULL);
        if (strchr(s, '%')) v /= 100.0f;
        return v;
    }
    return len_attr(ctx, xn, name, axis, def);
}

Node* build_mask(Ctx* ctx, const XMLNode* xn) {
    Node* n = svg_node_new(NODE_MASKDEF);
    if (!n) return NULL;

    n->mask_units = UNITS_BBOX;
    const char* mu = xml_attr(xn, "maskUnits");
    if (mu && !strcmp(mu, "userSpaceOnUse")) n->mask_units = UNITS_USER;

    n->mask_content_units = UNITS_USER;
    const char* mcu = xml_attr(xn, "maskContentUnits");
    if (mcu && !strcmp(mcu, "objectBoundingBox")) n->mask_content_units = UNITS_BBOX;

    float dx = (n->mask_units == UNITS_BBOX) ? -0.1f : -0.1f * ctx->vw;
    float dy = (n->mask_units == UNITS_BBOX) ? -0.1f : -0.1f * ctx->vh;
    float dw = (n->mask_units == UNITS_BBOX) ? 1.2f : 1.2f * ctx->vw;
    float dh = (n->mask_units == UNITS_BBOX) ? 1.2f : 1.2f * ctx->vh;
    n->mask_x = mask_len(ctx, xn, "x", 0, n->mask_units, dx);
    n->mask_y = mask_len(ctx, xn, "y", 1, n->mask_units, dy);
    n->mask_w = mask_len(ctx, xn, "width", 0, n->mask_units, dw);
    n->mask_h = mask_len(ctx, xn, "height", 1, n->mask_units, dh);
    return n;
}
