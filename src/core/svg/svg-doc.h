#ifndef SVG_INTERNAL_H
#define SVG_INTERNAL_H

#include "svg-int.h"

#define SVG2IMG_MAX_DEPTH 1024

struct CssRule {
    char* sel;
    char* decls;
    char* decls_imp;
    int   spec;

    struct SelTok* toks;
    int   ntoks;

    char  key_kind;
    char* key;
};
enum { CSS_KEY_NONE = 0, CSS_KEY_ID, CSS_KEY_CLASS, CSS_KEY_TAG };

typedef struct { char* key; Node* val; } IdEntry;
struct IdMap { IdEntry* e; int n, cap; };

void  idmap_put(IdMap* m, const char* k, Node* v);
Node* idmap_get(IdMap* m, const char* k);
void  idmap_remove(IdMap* m, const char* k, const Node* v);
void  idmap_free(IdMap* m);
void  svg_free_detached(Node* n, IdMap* ids);

Mat   parse_transform(const char* s);
Mat   transform_attr(const XMLNode* xn);
float len_attr(Ctx* ctx, const XMLNode* xn, const char* name, int axis, float def);

Path* path_from_rect(float x, float y, float w, float h, float rx, float ry);
Path* path_from_ellipse(float cx, float cy, float rx, float ry);
Path* path_from_line(float x1, float y1, float x2, float y2);
Path* path_from_points(const char* s, int close);
Path* path_from_points_lim(const char* s, int close, const svg2img_limits* lim,
                           int* limit_hit);

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial);
Node* build_pattern(Ctx* ctx, const XMLNode* xn);
void  pat_finalize_tree(Node* n);
Node* build_mask(Ctx* ctx, const XMLNode* xn);
Node* build_marker(Ctx* ctx, const XMLNode* xn);
Node* build_filter(Ctx* ctx, const XMLNode* xn, Color current);
void  filter_prims_free(FilterPrim* p, int n);
int   parse_stops(Ctx* ctx, const XMLNode* xn, Paint* p, int max_stops);
void  grad_finalize_tree(Node* n);
char* gather_text(const XMLNode* xn, int max_len, int* limit_hit);
void  gather_chunks(const XMLNode* xn, Ctx* ctx, Node* out, int* limit_hit,
                    const Style* parent);

int   node_add_child(Node* parent, Node* child);
Node* build_node(Ctx* ctx, const XMLNode* xn, const Style* parent, int depth);

int   svg_parse_viewbox(const char* s, float* x, float* y, float* w, float* h);

void  css_collect(Ctx* ctx, const XMLNode* root);
void  css_apply(Ctx* ctx, const XMLNode* xn, Style* st);
void  css_apply_important(Ctx* ctx, const XMLNode* xn, Style* st);
void  css_free(Ctx* ctx);

void  compute_viewport(const XMLNode* svg, const svg2img_options* opts,
                       int* W, int* H, Mat* tm, const svg2img_limits* lim,
                       int* clamped);

#endif
