#include "svg-doc.h"

static void text_chunks_free(Node* n) {
    if (!n || !n->chunks) return;
    for (int i = 0; i < n->nchunks; i++) {
        svg_mfree(n->chunks[i].text);
        svg_mfree(n->chunks[i].fill.stops);
        svg_mfree(n->chunks[i].fill.ref);
        svg_mfree(n->chunks[i].stroke.stops);
        svg_mfree(n->chunks[i].stroke.ref);
        svg_mfree(n->chunks[i].dash);
        svg_mfree(n->chunks[i].gx);
        svg_mfree(n->chunks[i].gy);
        svg_mfree(n->chunks[i].gdx);
        svg_mfree(n->chunks[i].gdy);
        svg_mfree(n->chunks[i].rot);
        path_free(n->chunks[i].refpath);
        svg_mfree(n->chunks[i].path);
        svg_mfree(n->chunks[i].path_ref);
    }
    svg_mfree(n->chunks);
    n->chunks = NULL;
    n->nchunks = 0;
    n->capchunks = 0;
}

void svg_free(Node* n) {
    if (!n) return;
    if (n->font_owned) font_face_free(n->font_owned);
    for (int i = 0; i < n->nkids; i++) svg_free(n->kids[i]);
    svg_mfree(n->kids);
    path_free(n->path);
    svg_mfree(n->fill.stops);
    svg_mfree(n->fill.ref);
    svg_mfree(n->stroke.stops);
    svg_mfree(n->stroke.ref);
    svg_mfree(n->dash);
    text_chunks_free(n);
    svg_mfree(n->text);
    svg_mfree(n->id);
    svg_mfree(n->clip_ref);
    svg_mfree(n->mask_ref);
    svg_mfree(n->image_href);
    img_buf_release(n->img_px);
    n->img_px = NULL;
    svg_mfree(n->marker_start_ref);
    svg_mfree(n->marker_mid_ref);
    svg_mfree(n->marker_end_ref);
    svg_mfree(n->filter_ref);
    svg_mfree(n->filt_href);
    filter_prims_free(n->prims, n->nprims);
    svg_mfree(n->use_ref);
    svg_mfree(n);
}

void svg_free_detached(Node* n, IdMap* ids) {
    if (!n) return;
    if (ids && n->id) idmap_remove(ids, n->id, n);
    if (n->font_owned) font_face_free(n->font_owned);
    for (int i = 0; i < n->nkids; i++) svg_free_detached(n->kids[i], ids);
    svg_mfree(n->kids);
    path_free(n->path);
    svg_mfree(n->fill.stops); svg_mfree(n->fill.ref);
    svg_mfree(n->stroke.stops); svg_mfree(n->stroke.ref);
    svg_mfree(n->dash); svg_mfree(n->text); svg_mfree(n->id); svg_mfree(n->clip_ref);
    svg_mfree(n->mask_ref);
    svg_mfree(n->image_href);
    img_buf_release(n->img_px);
    n->img_px = NULL;
    text_chunks_free(n);
    svg_mfree(n->marker_start_ref);
    svg_mfree(n->marker_mid_ref);
    svg_mfree(n->marker_end_ref); svg_mfree(n->use_ref);
    svg_mfree(n->filter_ref); svg_mfree(n->filt_href);
    filter_prims_free(n->prims, n->nprims);
    svg_mfree(n);
}

void svg2img_default_limits(svg2img_limits* out) {
    if (!out) return;
    out->max_nodes         = SVG2IMG_LIMIT_MAX_NODES;
    out->max_depth         = SVG2IMG_LIMIT_MAX_DEPTH;
    out->max_path_points   = SVG2IMG_LIMIT_MAX_PATH_POINTS;
    out->max_path_subpaths = SVG2IMG_LIMIT_MAX_PATH_SUBPATHS;
    out->max_gradient_stops= SVG2IMG_LIMIT_MAX_GRADIENT_STOPS;
    out->max_text_length   = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    out->max_clip_depth    = SVG2IMG_LIMIT_MAX_CLIP_DEPTH;
    out->max_layers        = SVG2IMG_LIMIT_MAX_LAYERS;
    out->max_use_expansion = SVG2IMG_LIMIT_MAX_USE_EXPANSION;
    out->max_pixels        = SVG2IMG_LIMIT_MAX_PIXELS;
}

static int lim_or(int v, int def) { return v > 0 ? v : def; }

void limits_resolve(const svg2img_limits* in, svg2img_limits* out) {
    svg2img_limits d;
    svg2img_default_limits(&d);
    if (!in) { *out = d; return; }
    out->max_nodes         = lim_or(in->max_nodes, d.max_nodes);
    out->max_depth         = lim_or(in->max_depth, d.max_depth);
    out->max_path_points   = lim_or(in->max_path_points, d.max_path_points);
    out->max_path_subpaths = lim_or(in->max_path_subpaths, d.max_path_subpaths);
    out->max_gradient_stops= lim_or(in->max_gradient_stops, d.max_gradient_stops);
    out->max_text_length   = lim_or(in->max_text_length, d.max_text_length);
    out->max_clip_depth    = lim_or(in->max_clip_depth, d.max_clip_depth);
    out->max_layers        = lim_or(in->max_layers, d.max_layers);
    out->max_use_expansion = lim_or(in->max_use_expansion, d.max_use_expansion);
    out->max_pixels        = lim_or(in->max_pixels, d.max_pixels);
}

static void options_normalize(svg2img_options* o) {
    if (o->aa < 1) o->aa = 1;
    if (o->aa > 8) o->aa = 8;
    if (!(o->scale > 0.0f)) o->scale = 1.0f;
}

Node* svg_parse(const char* data, size_t len, const svg2img_options* opts,
                int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    svg2img_options o;
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.scale = 1.0f; o.aa = 4; }
    options_normalize(&o);

#ifdef SVG2IMG_ALLOC_TRACE
    svg_trace_mark("phase:xml");
#endif
    XMLNode* doc = svg_xml_parse(data, len);
    if (!doc) return NULL;
    XMLNode* root = xml_child(doc, "svg");
    if (!root) { xml_free(doc); return NULL; }

    int W = 0, H = 0;
    Mat view = mat_identity();
    svg2img_limits lim;
    limits_resolve(&o.limits, &lim);
    int clamped = 0;

    IdMap ids;
    memset(&ids, 0, sizeof(ids));

    compute_viewport(root, &o, &W, &H, &view, &lim, &clamped);

    if (clamped) {
        if (limit_hit) *limit_hit = 1;
        idmap_free(&ids);
        xml_free(doc);
        return NULL;
    }

    Ctx ctx;

    Mat inv_view = mat_invert(view);
    Pt vp0 = mat_apply(inv_view, (Pt){0.0f, 0.0f});
    Pt vp1 = mat_apply(inv_view, (Pt){(float)W, (float)H});
    ctx.vw = fabsf(vp1.x - vp0.x);
    ctx.vh = fabsf(vp1.y - vp0.y);
    if (!(ctx.vw > 0.0f) || !isfinite(ctx.vw)) ctx.vw = (float)W;
    if (!(ctx.vh > 0.0f) || !isfinite(ctx.vh)) ctx.vh = (float)H;
    ctx.em = 16.0f;
    ctx.ids = &ids;
    ctx.lim = &lim;
    ctx.nodes = 0;
    ctx.limit_hit = 0;
    ctx.base_path = o.base_path;
    ctx.aa = o.aa;
    ctx.inherit_from = NULL;
    FontFace* face = o.font_path ? font_face_load(o.font_path) : NULL;
    ctx.font = face;
    ctx.rules = NULL;
    ctx.css_last = NULL;
    ctx.css_index = NULL;
    ctx.css_nlast = 0;
    ctx.nrules = ctx.caprules = 0;
    ctx.has_root_view = 1;
    ctx.root_view = view;
    ctx.root_w = W;
    ctx.root_h = H;
    css_collect(&ctx, root);

#ifdef SVG2IMG_ALLOC_TRACE
    svg_trace_mark("phase:build");
#endif
    Style parent = style_default();
    Node* root_node = build_node(&ctx, root, &parent, 0);
    style_free(&parent);
    css_free(&ctx);

    if (root_node) {

        root_node->vw = ctx.vw;
        root_node->vh = ctx.vh;
#ifdef SVG2IMG_ALLOC_TRACE
        svg_trace_mark("phase:resolve");
#endif
        if (svg_resolve(root_node, &ids, &lim)) ctx.limit_hit = 1;
        root_node->font = face;
        root_node->font_owned = face;
        root_node->vp.w = W;
        root_node->vp.h = H;
        root_node->vp.tm = view;
        root_node->vp.valid = 1;
    }

    idmap_free(&ids);
#ifdef SVG2IMG_ALLOC_TRACE
    svg_trace_mark("phase:free-xml");
#endif
    xml_free(doc);

    if (ctx.limit_hit) {
        svg_free(root_node);
        if (limit_hit) *limit_hit = 1;
        return NULL;
    }
    if (!root_node) {

        font_face_free(face);
        return NULL;
    }
    return root_node;
}

int svg2img_rasterize(const char* svg_data, size_t len, const svg2img_options* opts,
    svg2img_sink sink, void* ud) {
    if (!svg_data || !sink) return SVG2IMG_ERR_ARG;
    svg2img_options o;
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.scale = 1.0f; o.aa = 4; }
    o.base_path = NULL;
    o.font_path = NULL;
    o.threads = 0;
    options_normalize(&o);
    svg2img_limits lim;
    limits_resolve(&o.limits, &lim);
    int hit_limit = 0;
    image_budget_reset();
    svg_doc_arena_begin();
    Node* root = svg_parse(svg_data, len, &o, &hit_limit);
    svg_doc_arena_pause();
    if (!root || !root->vp.valid) {
        svg_free(root);
        svg_doc_arena_reset();
        return hit_limit ? SVG2IMG_ERR_LIMIT : SVG2IMG_ERR_FORMAT;
    }
    int W = root->vp.w, H = root->vp.h;
    if ((int64_t)W * (int64_t)H > (int64_t)lim.max_pixels) {
        svg_free(root);
        svg_doc_arena_reset();
        return SVG2IMG_ERR_LIMIT;
    }
    Canvas* c = canvas_create(W, H);
    if (!c) {
        svg_free(root);
        svg_doc_arena_reset();
        return SVG2IMG_ERR_NOMEM;
    }
    if (o.has_background) {
        unsigned int v = o.background;
        Color bg;
        bg.r = (float)((v >> 24) & 0xFF) / 255.0f;
        bg.g = (float)((v >> 16) & 0xFF) / 255.0f;
        bg.b = (float)((v >> 8) & 0xFF) / 255.0f;
        bg.a = (float)(v & 0xFF) / 255.0f;
        canvas_clear(c, bg);
    }
    render_document(c, root, mat_identity(), &o, &lim);
    int rc = sink(ud, c->px, W, H);
    canvas_free(c);
    svg_free(root);
    svg_doc_arena_reset();
    return rc;
}
