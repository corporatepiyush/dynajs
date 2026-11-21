#ifndef DYN_XMLSCAN_XMLSCAN_H
#define DYN_XMLSCAN_XMLSCAN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-common.h"
#include "../dyn-sb.h"
#include "../dyn-simdk.h"

#define XML_MAX_DEPTH 256
#define XML_MAX_ATTRS 1024
#define XML_MAX_INPUT (256u << 20)

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} xb_t;

static inline void xb_init(xb_t* b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
    b->oom = 0;
}

static inline void xb_free(xb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static inline void xb_write(xb_t* b, const void* p, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static inline void xb_put(xb_t* b, uint8_t c) { xb_write(b, &c, 1); }

static inline void xb_puts(xb_t* b, const char* s) { xb_write(b, s, strlen(s)); }

static inline void xb_reserve(xb_t* b, size_t need)
{
    if (b->oom || need <= b->cap)
        return;
    if (!dyn_sb_reserve((void**)&b->p, &b->cap, need, 64))
        b->oom = 1;
}

extern const uint8_t XML_NAME[256];

static inline int xml_name_start(unsigned char c) { return (XML_NAME[c] & 1) || c >= 0x80; }

static inline int xml_name_char(unsigned char c) { return XML_NAME[c] != 0 || c >= 0x80; }

static inline int xml_space(unsigned char c)
{
    return dyn_in_set64(c, DYN_WS_XML);
}

enum { XEV_OPEN,
    XEV_CLOSE,
    XEV_TEXT,
    XEV_CDATA,
    XEV_COMMENT,
    XEV_PI,
    XEV_SKIP };

enum { XS_EVENT,
    XS_NEED,
    XS_END,
    XS_ERR };

typedef struct {
    const char* name;
    size_t nlen;
    const char* val;
    size_t vlen;
    xb_t text;
    struct {
        const char* n;
        size_t nlen;
        size_t voff, vlen;
    } attr[XML_MAX_ATTRS];
    xb_t abuf;
    uint32_t nattr;
    uint8_t kind, selfclose;
} xml_ev_t;

typedef struct {
    xb_t buf;
    size_t pos;
    uint64_t total;
    int final, done;
    int tok;
    size_t tscan;
    int keep_entities;
    int bom_done;
    int seen_root, seen_doctype;
    char err[128];
    uint32_t line;
} xml_scan_t;

void xml_scan_bom(xml_scan_t* s);

int xml_scan_open(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev);

int xml_scan_text(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev);

int xml_scan_close(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev);

int xml_scan_bang(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev);

int xml_scan_pi(xml_scan_t* s, const char* p, size_t n, size_t* i,
    xml_ev_t* ev);

int xml_step(xml_scan_t* s, xml_ev_t* ev);

static inline void xml_compact(xml_scan_t* s)
{
    if (s->pos == 0 || s->pos * 2 < s->buf.n)
        return;
    memmove(s->buf.p, s->buf.p + s->pos, s->buf.n - s->pos);
    s->buf.n -= s->pos;
    s->pos = 0;
}

static inline int xml_all_space(const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!xml_space(p[i]))
            return 0;
    return 1;
}

static inline int xml_ok_chars(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r')
            return 0;
    }
    return 1;
}

void xml_escape(xb_t* b, const char* s, size_t n, int in_attr);

#endif
