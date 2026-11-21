#ifndef DYN_MARKDOWN_MARKDOWN_H
#define DYN_MARKDOWN_MARKDOWN_H

#include "../htmlkit/htmlkit.h"

#define MD_MAX_INPUT (16u << 20)

int dyn_md_render_html(const char* s, size_t n, int allow_html, int heading_ids,
    hb_t* out);

#endif
