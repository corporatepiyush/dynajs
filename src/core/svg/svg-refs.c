#include "svg-doc.h"
#include <stdint.h>

static void paint_copy(Paint* dst, const Paint* src) {
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

static void resolve_paint(Paint* p, IdMap* ids) {
    if (p->type == PAINT_REF && p->ref) {
        Node* g = idmap_get(ids, p->ref);
        if (g && g->type == NODE_PATTERNDEF && g->pat_w > 0.0f && g->pat_h > 0.0f) {
            svg_mfree(p->stops); p->stops = NULL; p->nstops = 0;
            p->type = PAINT_PATTERN;
            p->pat = g;
        } else if (g && g->type == NODE_GRADDEF && g->fill.nstops > 0) {
            paint_copy(p, &g->fill);
        } else {
            svg_mfree(p->stops); p->stops = NULL; p->nstops = 0;
            svg_mfree(p->ref);   p->ref = NULL;

            int is_server = g && (g->type == NODE_PATTERNDEF || g->type == NODE_GRADDEF);
            if (!is_server && (p->fb_type == PAINT_COLOR || p->fb_type == PAINT_CURRENT_COLOR)) {
                p->type = PAINT_COLOR;
                p->color = p->fb_color;
            } else {
                p->type = PAINT_NONE;
            }
        }
    }
}

static void resolve_paint_refs(Node* n, IdMap* ids) {
    if (!n) return;
    resolve_paint(&n->fill, ids);
    resolve_paint(&n->stroke, ids);

    for (int i = 0; i < n->nchunks && n->chunks; i++) {
        resolve_paint(&n->chunks[i].fill, ids);
        resolve_paint(&n->chunks[i].stroke, ids);
    }
    for (int i = 0; i < n->nkids; i++) resolve_paint_refs(n->kids[i], ids);
}

static int grad_ref_pending(const Node* n) {
    return n->type == NODE_GRADDEF && n->fill.ref != NULL;
}

static void grad_clear_ref(Node* n) {
    svg_mfree(n->fill.ref);
    n->fill.ref = NULL;
}

static void grad_copy_stops(Paint* dst, const Paint* src) {
    svg_mfree(dst->stops);
    dst->stops = NULL;
    dst->nstops = 0;
    if (src->nstops <= 0 || !src->stops) return;
    Stop* s = (Stop*)svg_malloc((size_t)src->nstops * sizeof(Stop));
    if (!s) return;
    memcpy(s, src->stops, (size_t)src->nstops * sizeof(Stop));
    dst->stops = s;
    dst->nstops = src->nstops;
}

static void grad_copy_coord(Node* n, float* dst, float v, unsigned bit,
                            unsigned src_spec, unsigned src_pct) {
    if (n->grad_spec & bit) return;
    if (!(src_spec & bit)) return;
    *dst = v;
    n->grad_spec |= bit;
    if (src_pct & bit) n->grad_pct |= bit;
    else n->grad_pct &= ~bit;
}

static void grad_inherit(Node* n, const Node* g) {
    Paint* d = &n->fill;
    const Paint* s = &g->fill;

    if (!(n->grad_spec & GRAD_SPEC_UNITS)) {
        d->units = s->units;
        n->grad_spec |= GRAD_SPEC_UNITS;
    }
    if (!(n->grad_spec & GRAD_SPEC_SPREAD)) {
        d->spread = s->spread;
        n->grad_spec |= GRAD_SPEC_SPREAD;
    }
    if (!(n->grad_spec & GRAD_SPEC_TM)) {
        d->tm = s->tm;
        n->grad_spec |= GRAD_SPEC_TM;
    }
    if (!(n->grad_spec & GRAD_SPEC_CI)) {
        d->linear = s->linear;
        n->grad_spec |= GRAD_SPEC_CI;
    }

    if (d->type == s->type && d->type == PAINT_LINEAR) {
        grad_copy_coord(n, &d->x1, s->x1, GRAD_SPEC_X1,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->y1, s->y1, GRAD_SPEC_Y1,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->x2, s->x2, GRAD_SPEC_X2,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->y2, s->y2, GRAD_SPEC_Y2,
                        g->grad_spec, g->grad_pct);
    } else if (d->type == s->type && d->type == PAINT_RADIAL) {
        grad_copy_coord(n, &d->cx, s->cx, GRAD_SPEC_CX,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->cy, s->cy, GRAD_SPEC_CY,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->r, s->r, GRAD_SPEC_R,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->fx, s->fx, GRAD_SPEC_FX,
                        g->grad_spec, g->grad_pct);
        grad_copy_coord(n, &d->fy, s->fy, GRAD_SPEC_FY,
                        g->grad_spec, g->grad_pct);
    }

    if (!(n->grad_spec & GRAD_SPEC_STOPS)) {
        grad_copy_stops(d, s);
        if (d->nstops > 0) n->grad_spec |= GRAD_SPEC_STOPS;
    }
}

static void resolve_gradient_node(Node* n, IdMap* ids, Node** stack, int depth) {
    if (!grad_ref_pending(n)) return;
    if (depth >= 256) { grad_clear_ref(n); return; }
    for (int i = 0; i < depth; i++) {
        if (stack[i] == n) { grad_clear_ref(n); return; }
    }
    Node* g = idmap_get(ids, n->fill.ref);
    if (!g || g->type != NODE_GRADDEF || g == n) {
        grad_clear_ref(n);
        return;
    }
    stack[depth] = n;
    resolve_gradient_node(g, ids, stack, depth + 1);
    if (g->type == NODE_GRADDEF && g != n) grad_inherit(n, g);
    grad_clear_ref(n);
}

static void resolve_gradient_tree(Node* n, IdMap* ids, Node** stack) {
    if (!n) return;
    if (n->type == NODE_GRADDEF && n->fill.ref)
        resolve_gradient_node(n, ids, stack, 0);
    for (int i = 0; i < n->nkids; i++)
        resolve_gradient_tree(n->kids[i], ids, stack);
}

static void resolve_gradient_hrefs(Node* n, IdMap* ids) {
    Node* stack[256];
    resolve_gradient_tree(n, ids, stack);

    grad_finalize_tree(n);
}

static void resolve_clip_refs(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->clip_ref) {
        Node* c = idmap_get(ids, n->clip_ref);
        if (c && c->type == NODE_CLIPDEF) n->clip = c;
        svg_mfree(n->clip_ref);
        n->clip_ref = NULL;
    }
    if (n->mask_ref) {
        Node* m = idmap_get(ids, n->mask_ref);
        if (m && m->type == NODE_MASKDEF) n->mask = m;
        svg_mfree(n->mask_ref);
        n->mask_ref = NULL;
    }
    if (n->filter_ref) {
        Node* f = idmap_get(ids, n->filter_ref);
        if (f && f->type == NODE_FILTERDEF) n->filter = f;
        svg_mfree(n->filter_ref);
        n->filter_ref = NULL;
    }
    if (n->marker_start_ref) {
        Node* m = idmap_get(ids, n->marker_start_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_start = m;
        svg_mfree(n->marker_start_ref); n->marker_start_ref = NULL;
    }
    if (n->marker_mid_ref) {
        Node* m = idmap_get(ids, n->marker_mid_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_mid = m;
        svg_mfree(n->marker_mid_ref); n->marker_mid_ref = NULL;
    }
    if (n->marker_end_ref) {
        Node* m = idmap_get(ids, n->marker_end_ref);
        if (m && m->type == NODE_MARKERDEF) n->marker_end = m;
        svg_mfree(n->marker_end_ref); n->marker_end_ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_clip_refs(n->kids[i], ids);
}

typedef struct {
    IdMap* ids;
    Node*  root;
    Node*  grave;
    int    max_nodes;
    int    made;
    int    failed;
    int    truncated;
    int    max_depth;
    float  vw, vh;
} UseCtx;

static Node* clone_node(const Node* n, UseCtx* uc, int depth) {
    if (!n || uc->failed) return NULL;
    if (uc->made >= uc->max_nodes) { uc->truncated = 1; return NULL; }

    if (depth > uc->max_depth) { uc->truncated = 1; return NULL; }
    Node* c = (Node*)svg_malloc(sizeof(Node));
    if (!c) { uc->failed = 1; return NULL; }
    *c = *n;
    uc->made++;

    c->id = NULL;
    c->kids = NULL;
    c->nkids = 0;
    c->capkids = 0;
    c->font_owned = NULL;
    c->path = n->path ? path_copy(n->path) : NULL;
    if (n->path && !c->path) uc->failed = 1;

    c->image_href = n->image_href ? svg_strdup(n->image_href) : NULL;
    c->filt_href = n->filt_href ? svg_strdup(n->filt_href) : NULL;
    c->mask_ref = n->mask_ref ? svg_strdup(n->mask_ref) : NULL;
    c->filter_ref = n->filter_ref ? svg_strdup(n->filter_ref) : NULL;
    c->marker_start_ref = n->marker_start_ref ? svg_strdup(n->marker_start_ref) : NULL;
    c->marker_mid_ref = n->marker_mid_ref ? svg_strdup(n->marker_mid_ref) : NULL;
    c->marker_end_ref = n->marker_end_ref ? svg_strdup(n->marker_end_ref) : NULL;
    if ((n->image_href && !c->image_href) || (n->filt_href && !c->filt_href) ||
        (n->mask_ref && !c->mask_ref) || (n->filter_ref && !c->filter_ref) ||
        (n->marker_start_ref && !c->marker_start_ref) ||
        (n->marker_mid_ref && !c->marker_mid_ref) ||
        (n->marker_end_ref && !c->marker_end_ref))
        uc->failed = 1;

    c->img_px = img_buf_retain(n->img_px);

    for (int k = 0; k < 2; k++) {
        Paint* d = (k == 0) ? &c->fill : &c->stroke;
        const Paint* s = (k == 0) ? &n->fill : &n->stroke;
        d->stops = NULL; d->nstops = 0;
        d->ref = s->ref ? svg_strdup(s->ref) : NULL;
        if (s->ref && !d->ref) uc->failed = 1;
        if (s->stops && s->nstops > 0) {
            d->stops = (Stop*)svg_malloc((size_t)s->nstops * sizeof(Stop));
            if (d->stops) {
                memcpy(d->stops, s->stops, (size_t)s->nstops * sizeof(Stop));
                d->nstops = s->nstops;
            } else {
                uc->failed = 1;
            }
        }
    }

    c->dash = NULL;
    c->ndash = 0;
    if (n->dash && n->ndash > 0) {
        c->dash = (float*)svg_malloc((size_t)n->ndash * sizeof(float));
        if (c->dash) {
            memcpy(c->dash, n->dash, (size_t)n->ndash * sizeof(float));
            c->ndash = n->ndash;
        } else {
            uc->failed = 1;
        }
    }

    c->text = n->text ? svg_strdup(n->text) : NULL;
    if (n->text && !c->text) uc->failed = 1;

    c->chunks = NULL;
    c->nchunks = 0;
    c->capchunks = 0;
    if (n->nchunks > 0 && n->chunks) {
        c->chunks = (TextChunk*)svg_malloc((size_t)n->nchunks * sizeof(TextChunk));
        if (!c->chunks) {
            uc->failed = 1;
        } else {
            memcpy(c->chunks, n->chunks, (size_t)n->nchunks * sizeof(TextChunk));
            for (int i = 0; i < n->nchunks; i++) {
                TextChunk* d = &c->chunks[i];
                TextChunk* sc = &n->chunks[i];
                d->text = sc->text ? svg_strdup(sc->text) : NULL;

                d->fill.stops = NULL;
                d->fill.ref = NULL;
                d->stroke.stops = NULL;
                d->stroke.ref = NULL;
                paint_copy(&d->fill, &sc->fill);
                paint_copy(&d->stroke, &sc->stroke);
                if ((sc->fill.nstops > 0 && !d->fill.stops) ||
                    (sc->stroke.nstops > 0 && !d->stroke.stops))
                    uc->failed = 1;
                d->dash = NULL;
                d->ndash = 0;
                if (sc->dash && sc->ndash > 0) {
                    d->dash = (float*)svg_malloc(
                        (size_t)sc->ndash * sizeof(float));
                    if (d->dash) {
                        memcpy(d->dash, sc->dash,
                               (size_t)sc->ndash * sizeof(float));
                        d->ndash = sc->ndash;
                    } else {
                        uc->failed = 1;
                    }
                }
                d->gx = d->gy = d->gdx = d->gdy = NULL;
                d->ngpos = 0;
                if (sc->ngpos > 0) {
                    size_t nb = (size_t)sc->ngpos * sizeof(float);
                    d->gx = (float*)svg_malloc(nb);
                    d->gy = (float*)svg_malloc(nb);
                    d->gdx = (float*)svg_malloc(nb);
                    d->gdy = (float*)svg_malloc(nb);
                    if (d->gx && d->gy && d->gdx && d->gdy) {
                        memcpy(d->gx, sc->gx, nb);
                        memcpy(d->gy, sc->gy, nb);
                        memcpy(d->gdx, sc->gdx, nb);
                        memcpy(d->gdy, sc->gdy, nb);
                        d->ngpos = sc->ngpos;
                    } else {
                        svg_mfree(d->gx); svg_mfree(d->gy);
                        svg_mfree(d->gdx); svg_mfree(d->gdy);
                        d->gx = d->gy = d->gdx = d->gdy = NULL;
                        uc->failed = 1;
                    }
                }
                d->rot = NULL;
                d->nrot = 0;
                if (sc->rot && sc->nrot > 0) {
                    d->rot = (float*)svg_malloc((size_t)sc->nrot * sizeof(float));
                    if (d->rot) {
                        memcpy(d->rot, sc->rot, (size_t)sc->nrot * sizeof(float));
                        d->nrot = sc->nrot;
                    }
                }
                d->path = sc->path ? svg_strdup(sc->path) : NULL;
                d->path_ref = sc->path_ref ? svg_strdup(sc->path_ref) : NULL;
                d->refpath = sc->refpath ? path_copy(sc->refpath) : NULL;

                if ((sc->text && !d->text) || (sc->fill.ref && !d->fill.ref) ||
                    (sc->stroke.ref && !d->stroke.ref))
                    uc->failed = 1;
            }
            c->nchunks = n->nchunks;
            c->capchunks = n->nchunks;
        }
    }

    c->prims = NULL;
    c->nprims = 0;
    if (n->nprims > 0 && n->prims) {
        c->prims = (FilterPrim*)svg_malloc((size_t)n->nprims * sizeof(FilterPrim));
        if (!c->prims) {
            uc->failed = 1;
        } else {
            memcpy(c->prims, n->prims, (size_t)n->nprims * sizeof(FilterPrim));
            for (int i = 0; i < n->nprims; i++) {
                FilterPrim* d = &c->prims[i];
                const FilterPrim* sc = &n->prims[i];
                d->in = sc->in ? svg_strdup(sc->in) : NULL;
                d->in2 = sc->in2 ? svg_strdup(sc->in2) : NULL;
                d->result = sc->result ? svg_strdup(sc->result) : NULL;
                d->href = sc->href ? svg_strdup(sc->href) : NULL;
                d->px = img_buf_retain(sc->px);
                d->nmerge = 0;
                for (int m = 0; m < sc->nmerge && m < 8; m++)
                    d->merge_in[d->nmerge++] =
                        sc->merge_in[m] ? svg_strdup(sc->merge_in[m]) : NULL;
                if ((sc->in && !d->in) || (sc->in2 && !d->in2) ||
                    (sc->result && !d->result) || (sc->href && !d->href))
                    uc->failed = 1;
                for (int m = 0; m < sc->nmerge && m < 8; m++)
                    if (sc->merge_in[m] && !d->merge_in[m]) uc->failed = 1;
            }
            c->nprims = n->nprims;
        }
    }

    c->clip_ref = n->clip_ref ? svg_strdup(n->clip_ref) : NULL;
    if (n->clip_ref && !c->clip_ref) uc->failed = 1;
    c->use_ref = n->use_ref ? svg_strdup(n->use_ref) : NULL;
    if (n->use_ref && !c->use_ref) uc->failed = 1;

    if (c->type == NODE_SYMBOL) c->type = NODE_GROUP;

    for (int i = 0; i < n->nkids; i++) {
        Node* kid = clone_node(n->kids[i], uc, depth + 1);
        if (kid && !node_add_child(c, kid)) svg_free(kid);
        if (uc->failed) break;
    }
    return c;
}

static void paint_inherit_copy(Paint* dst, const Paint* src) {
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

static void use_inherit_paint(Paint* dst, const Paint* src, unsigned spec_bit,
                              unsigned* dst_spec) {
    if (*dst_spec & spec_bit) return;
    if (src->type == PAINT_NONE && !src->ref && src->nstops == 0) return;
    paint_inherit_copy(dst, src);
    *dst_spec |= spec_bit;
}

static void dash_inherit(Node* n, const Node* parent) {
    svg_mfree(n->dash);
    n->dash = NULL;
    n->ndash = 0;
    if (parent->dash && parent->ndash > 0) {
        n->dash = (float*)svg_malloc(sizeof(float) * (size_t)parent->ndash);
        if (n->dash) {
            memcpy(n->dash, parent->dash, sizeof(float) * (size_t)parent->ndash);
            n->ndash = parent->ndash;
        }
    }
    n->dashoff = parent->dashoff;
}

static void push_anchor(Node* n) {
    if (!n || !n->chunks) return;
    for (int i = 0; i < n->nchunks; i++) n->chunks[i].anchor = n->anchor;
}

static void use_apply_child(Node* n, const Node* parent) {
    if (!n || !parent) return;
    if (!(n->spec & SPEC_FILL))   paint_inherit_copy(&n->fill, &parent->fill);
    if (!(n->spec & SPEC_STROKE)) paint_inherit_copy(&n->stroke, &parent->stroke);
    if (!(n->spec & SPEC_FONT_SIZE))     n->fontsize = parent->fontsize;
    if (!(n->spec & SPEC_FONT_WEIGHT))   n->bold = parent->bold;
    if (!(n->spec & SPEC_FONT_STYLE))    n->italic = parent->italic;
    if (!(n->spec & SPEC_TEXT_ANCHOR))   n->anchor = parent->anchor;
    if (!(n->spec & SPEC_LETTER_SP))     n->letterspacing = parent->letterspacing;
    if (!(n->spec & SPEC_VISIBILITY))    n->visible = parent->visible;
    if (!(n->spec & SPEC_STROKE_WIDTH))  n->sw = parent->sw;
    if (!(n->spec & SPEC_LINECAP))       n->linecap = parent->linecap;
    if (!(n->spec & SPEC_LINEJOIN))      n->linejoin = parent->linejoin;
    if (!(n->spec & SPEC_MITERLIMIT))    n->miterlimit = parent->miterlimit;
    if (!(n->spec & SPEC_FILL_RULE))     n->fillrule = parent->fillrule;
    if (!(n->spec & SPEC_CLIP_RULE))     n->clip_rule = parent->clip_rule;
    if (!(n->spec & SPEC_FILL_OPACITY))  n->fillop = parent->fillop;
    if (!(n->spec & SPEC_STROKE_OPACITY)) n->strokeop = parent->strokeop;
    if (!(n->spec & SPEC_DASH))          dash_inherit(n, parent);
    if (!(n->spec & SPEC_DASH_OFFSET))   n->dashoff = parent->dashoff;
    push_anchor(n);
    for (int i = 0; i < n->nkids; i++) use_apply_child(n->kids[i], n);
}

static void use_apply_style(Node* n, const Node* use) {
    if (!n || !use) return;
    use_inherit_paint(&n->fill, &use->fill, (unsigned)SPEC_FILL, &n->spec);
    use_inherit_paint(&n->stroke, &use->stroke, (unsigned)SPEC_STROKE, &n->spec);

    if (!(n->spec & SPEC_FONT_SIZE))      n->fontsize = use->fontsize;
    if (!(n->spec & SPEC_FONT_WEIGHT))    n->bold = use->bold;
    if (!(n->spec & SPEC_FONT_STYLE))     n->italic = use->italic;
    if (!(n->spec & SPEC_TEXT_ANCHOR))    n->anchor = use->anchor;
    if (!(n->spec & SPEC_LETTER_SP))      n->letterspacing = use->letterspacing;
    if (!(n->spec & SPEC_VISIBILITY))     n->visible = use->visible;
    if (!(n->spec & SPEC_STROKE_WIDTH))   n->sw = use->sw;
    if (!(n->spec & SPEC_LINECAP))        n->linecap = use->linecap;
    if (!(n->spec & SPEC_LINEJOIN))       n->linejoin = use->linejoin;
    if (!(n->spec & SPEC_MITERLIMIT))     n->miterlimit = use->miterlimit;
    if (!(n->spec & SPEC_FILL_RULE))      n->fillrule = use->fillrule;
    if (!(n->spec & SPEC_CLIP_RULE))      n->clip_rule = use->clip_rule;
    if (!(n->spec & SPEC_FILL_OPACITY))   n->fillop = use->fillop;
    if (!(n->spec & SPEC_STROKE_OPACITY)) n->strokeop = use->strokeop;
    if (!(n->spec & SPEC_DASH))           dash_inherit(n, use);
    if (!(n->spec & SPEC_DASH_OFFSET))    n->dashoff = use->dashoff;
    push_anchor(n);

    for (int i = 0; i < n->nkids; i++) use_apply_child(n->kids[i], n);
}

static void resolve_use_clear(Node* n) {
    if (!n) return;
    if (n->use_ref) {
        svg_mfree(n->use_ref);
        n->use_ref = NULL;
    }
    for (int i = 0; i < n->nkids; i++) resolve_use_clear(n->kids[i]);
}

static void unregister_ids(Node* n, IdMap* ids) {
    if (!n) return;
    if (ids && n->id) idmap_remove(ids, n->id, n);
    for (int i = 0; i < n->nkids; i++) unregister_ids(n->kids[i], ids);
}

static void grave_adopt(UseCtx* uc, Node* n) {
    if (!n) return;
    if (n->nkids <= 0) {
        svg_mfree(n->kids);
        n->kids = NULL;
        n->nkids = 0;
        n->capkids = 0;
        return;
    }
    if (!uc->grave) {
        Node* g = svg_node_new(NODE_IGNORE);
        if (!g || !node_add_child(uc->root, g)) {
            svg_free(g);
            uc->failed = 1;
            return;
        }
        g->hidden_subtree = 1;
        uc->grave = g;
    }
    int moved = 0;
    while (moved < n->nkids &&
           node_add_child(uc->grave, n->kids[moved])) {
        unregister_ids(n->kids[moved], uc->ids);
        moved++;
    }
    if (moved == n->nkids) {
        svg_mfree(n->kids);
        n->kids = NULL;
        n->nkids = 0;
        n->capkids = 0;
        return;
    }

    memmove(n->kids, n->kids + moved,
            (size_t)(n->nkids - moved) * sizeof(Node*));
    n->nkids -= moved;
    uc->failed = 1;
}

static void use_viewport_size(const Node* use, const Node* target,
                              const UseCtx* uc, float* w, float* h) {
    *w = use->use_w > 0.0f ? use->use_w
        : (target->symbol_has_w ? target->symbol_w : uc->vw);
    *h = use->use_h > 0.0f ? use->use_h
        : (target->symbol_has_h ? target->symbol_h : uc->vh);
}

static int use_size_disabled(const Node* use) {
    return (isfinite(use->use_w) && use->use_w == 0.0f) ||
           (isfinite(use->use_h) && use->use_h == 0.0f);
}

static int resolve_use(Node* n, IdMap* ids, UseCtx* uc, int depth) {
    if (!n || uc->failed) return 0;
    if (n == uc->grave) return 0;
    int used = 0;
    if (n->use_ref) {
        char* ref = n->use_ref;
        n->use_ref = NULL;
        Node* target = idmap_get(ids, ref);
        if (use_size_disabled(n)) {

            svg_mfree(ref);
            used = 1;
            for (int i = 0; i < n->nkids; i++)
                if (resolve_use(n->kids[i], ids, uc, depth + 1)) used = 1;
            return used;
        }
        if (target && target != n) {

            Node* clone = clone_node(target, uc, depth + 1);
            if (clone) {
                use_apply_style(clone, n);

                Mat use_tm = mat_translate(n->use_x, n->use_y);
                if (target->type == NODE_GROUP && target->use_vp_has_size) {

                    float vw, vh;
                    int sized = n->use_w > 0.0f || n->use_h > 0.0f;
                    use_viewport_size(n, target, uc, &vw, &vh);

                    clone->vp_clip = sized ? 1 : target->vp_clip;
                    clone->vp_x = sized ? 0.0f : target->vp_x;
                    clone->vp_y = sized ? 0.0f : target->vp_y;
                    clone->vp_w = sized ? vw : target->vp_w;
                    clone->vp_h = sized ? vh : target->vp_h;
                    if (sized && clone->use_vb_valid) {
                        float sx = vw / clone->use_vb[2];
                        float sy = vh / clone->use_vb[3];
                        if (clone->use_par_none) {
                            clone->tm = mat_mul(mat_scale(sx, sy),
                                        mat_translate(-clone->use_vb[0],
                                                      -clone->use_vb[1]));
                        } else {
                            float s = clone->use_par_slice
                                ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
                            float ex = (float)vw - clone->use_vb[2] * s;
                            float ey = (float)vh - clone->use_vb[3] * s;
                            float ox = (clone->use_par_xa == 1) ? ex * 0.5f
                                 : (clone->use_par_xa == 2 ? ex : 0.0f);
                            float oy = (clone->use_par_ya == 1) ? ey * 0.5f
                                 : (clone->use_par_ya == 2 ? ey : 0.0f);
                            clone->tm = mat_mul(mat_translate(ox, oy),
                                        mat_mul(mat_scale(s, s),
                                        mat_translate(-clone->use_vb[0],
                                                      -clone->use_vb[1])));
                        }
                    }
                } else if (target->type == NODE_SYMBOL) {
                    float vw, vh;
                    use_viewport_size(n, target, uc, &vw, &vh);
                    clone->vp_clip = 1;
                    clone->vp_x = 0.0f; clone->vp_y = 0.0f;
                    clone->vp_w = vw; clone->vp_h = vh;
                    if (clone->symbol_vb_valid) {
                        float sx = vw / clone->symbol_vb[2];
                        float sy = vh / clone->symbol_vb[3];
                        if (clone->symbol_par_none) {
                            clone->tm = mat_mul(mat_scale(sx, sy),
                                        mat_translate(-clone->symbol_vb[0],
                                                      -clone->symbol_vb[1]));
                        } else {
                            float s = clone->symbol_par_slice ? (sx > sy ? sx : sy)
                                                              : (sx < sy ? sx : sy);
                            float ex = (float)vw - clone->symbol_vb[2] * s;
                            float ey = (float)vh - clone->symbol_vb[3] * s;
                            float ox = (clone->symbol_par_xa == 1) ? ex * 0.5f
                                 : (clone->symbol_par_xa == 2 ? ex : 0.0f);
                            float oy = (clone->symbol_par_ya == 1) ? ey * 0.5f
                                 : (clone->symbol_par_ya == 2 ? ey : 0.0f);
                            clone->tm = mat_mul(mat_translate(ox, oy),
                                        mat_mul(mat_scale(s, s),
                                        mat_translate(-clone->symbol_vb[0],
                                                      -clone->symbol_vb[1])));
                        }
                    }
                }
                clone->tm = mat_mul(use_tm, clone->tm);
                resolve_clip_refs(clone, ids);

                grave_adopt(uc, n);
                if (uc->failed || !node_add_child(n, clone)) svg_free(clone);
            }
        }
        svg_mfree(ref);
        used = 1;
    }
    for (int i = 0; i < n->nkids; i++)
        if (resolve_use(n->kids[i], ids, uc, depth + 1)) used = 1;
    return used;
}

static int pattern_ref_pending(const Node* n) {
    return n->type == NODE_PATTERNDEF && n->fill.ref != NULL;
}

static void pat_clear_ref(Node* n) {
    svg_mfree(n->fill.ref);
    n->fill.ref = NULL;
}

static void pat_copy_coord(Node* n, float* dst, float v, unsigned bit,
                           unsigned src_spec, unsigned src_pct) {
    if (n->pat_spec & bit) return;
    if (!(src_spec & bit)) return;
    *dst = v;
    n->pat_spec |= bit;
    if (src_pct & bit) n->pat_pct |= bit;
    else n->pat_pct &= ~bit;
}

static void pattern_inherit(Node* n, Node* t) {
    if (!(n->pat_spec & PAT_SPEC_UNITS)) {
        n->pat_units = t->pat_units;
        n->pat_spec |= PAT_SPEC_UNITS;
    }
    if (!(n->pat_spec & PAT_SPEC_CONTENT_UNITS)) {
        n->pat_content_units = t->pat_content_units;
        n->pat_spec |= PAT_SPEC_CONTENT_UNITS;
    }
    if (!(n->pat_spec & PAT_SPEC_TM)) {
        n->pat_tm = t->pat_tm;
        n->pat_spec |= PAT_SPEC_TM;
    }
    pat_copy_coord(n, &n->pat_x, t->pat_x, PAT_SPEC_X,
                   t->pat_spec, t->pat_pct);
    pat_copy_coord(n, &n->pat_y, t->pat_y, PAT_SPEC_Y,
                   t->pat_spec, t->pat_pct);
    pat_copy_coord(n, &n->pat_w, t->pat_w, PAT_SPEC_W,
                   t->pat_spec, t->pat_pct);
    pat_copy_coord(n, &n->pat_h, t->pat_h, PAT_SPEC_H,
                   t->pat_spec, t->pat_pct);
    if (!(n->pat_spec & PAT_SPEC_VB)) {
        for (int i = 0; i < 4; i++) n->pat_vb[i] = t->pat_vb[i];
        n->pat_vb_valid = t->pat_vb_valid;
        n->pat_spec |= PAT_SPEC_VB;
    }
    if (!(n->pat_spec & PAT_SPEC_PAR)) {
        n->pat_par_none = t->pat_par_none;
        n->pat_par_slice = t->pat_par_slice;
        n->pat_par_xa = t->pat_par_xa;
        n->pat_par_ya = t->pat_par_ya;
        n->pat_spec |= PAT_SPEC_PAR;
    }

    if (n->nkids == 0 && !n->pat_content)
        n->pat_content = t->pat_content ? t->pat_content : t;
}

static void resolve_pattern_node(Node* n, IdMap* ids, Node** stack, int depth) {
    if (!pattern_ref_pending(n)) return;
    if (depth >= 256) { pat_clear_ref(n); return; }
    for (int i = 0; i < depth; i++) {
        if (stack[i] == n) { pat_clear_ref(n); return; }
    }
    Node* t = idmap_get(ids, n->fill.ref);
    if (!t || t->type != NODE_PATTERNDEF || t == n) {
        pat_clear_ref(n);
        return;
    }
    stack[depth] = n;
    resolve_pattern_node(t, ids, stack, depth + 1);
    if (t->type == NODE_PATTERNDEF && t != n) pattern_inherit(n, t);
    pat_clear_ref(n);
}

static void resolve_pattern_tree(Node* n, IdMap* ids, Node** stack) {
    if (!n) return;
    if (n->type == NODE_PATTERNDEF && n->fill.ref)
        resolve_pattern_node(n, ids, stack, 0);
    for (int i = 0; i < n->nkids; i++)
        resolve_pattern_tree(n->kids[i], ids, stack);
}

static void resolve_pattern_hrefs(Node* n, IdMap* ids) {
    Node* stack[256];
    resolve_pattern_tree(n, ids, stack);
    pat_finalize_tree(n);
}

static void resolve_text_paths(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->chunks) {
        for (int i = 0; i < n->nchunks; i++) {
            TextChunk* ch = &n->chunks[i];
            if (!ch->path_ref) continue;
            Node* ref = idmap_get(ids, ch->path_ref);
            if (ref && ref->type == NODE_SHAPE && ref->path && ref->path->n > 0) {
                path_free(ch->refpath);
                ch->refpath = path_copy(ref->path);
            }
            svg_mfree(ch->path_ref);
            ch->path_ref = NULL;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_text_paths(n->kids[i], ids);
}

static void resolve_filter_image_refs(Node* n, IdMap* ids) {
    if (!n) return;
    if (n->type == NODE_FILTERDEF && n->prims) {
        for (int i = 0; i < n->nprims; i++) {
            FilterPrim* p = &n->prims[i];
            if (p->type != FE_IMAGE || !p->href || p->href[0] != '#') continue;
            Node* t = idmap_get(ids, p->href + 1);
            if (t && t != n) p->ref_node = t;
        }
    }
    for (int i = 0; i < n->nkids; i++) resolve_filter_image_refs(n->kids[i], ids);
}

int svg_resolve(Node* n, IdMap* ids, const svg2img_limits* lim) {
    if (!n) return 0;
    resolve_pattern_hrefs(n, ids);
    resolve_gradient_hrefs(n, ids);
    resolve_paint_refs(n, ids);
    resolve_text_paths(n, ids);
    resolve_filter_image_refs(n, ids);
    resolve_clip_refs(n, ids);

    UseCtx uc;
    uc.ids = ids;
    uc.root = n;
    uc.grave = NULL;
    uc.max_nodes = lim ? lim->max_use_expansion : SVG2IMG_LIMIT_MAX_USE_EXPANSION;
    uc.made = 0;
    uc.failed = 0;
    uc.truncated = 0;

    uc.vw = n->vw > 0.0f ? n->vw : 300.0f;
    uc.vh = n->vh > 0.0f ? n->vh : 150.0f;
    uc.max_depth = lim ? lim->max_depth : SVG2IMG_LIMIT_MAX_DEPTH;
    if (uc.max_depth > SVG2IMG_MAX_DEPTH) uc.max_depth = SVG2IMG_MAX_DEPTH;
    if (uc.max_depth < 1) uc.max_depth = 1;

    for (int pass = 0; pass < 64; pass++) {
        int before = uc.made;
        if (!resolve_use(n, ids, &uc, 0)) break;
        if (uc.failed || uc.made == before) break;
    }

    resolve_use_clear(n);
    return uc.truncated || uc.failed;
}
