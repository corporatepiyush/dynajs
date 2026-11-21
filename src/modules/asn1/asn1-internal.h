#ifndef ASN1_INTERNAL_H
#define ASN1_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASN1)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define ASN1_MAX_DEPTH 256
#define ASN1_MAX_BIGINT_BYTES 64
#define ASN1_MAX_OID_ARCS 64
#define ASN1_ARC_MAX 0xFFFFFFFFu
#define ASN1_MAX_TAG 0x7FFFFFFF
#define ASN1_MAX_CHILDREN (1u << 16)

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} ab_t;

static inline void ab_init(ab_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

static inline void ab_free(ab_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

int ab_write(ab_t* b, const uint8_t* p, size_t n);

int asn1_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn, JSValue* hold);

JSValue asn1_new_bytes(JSContext* ctx, const uint8_t* p, size_t n);

int der_ident(ab_t* b, int cls, int constructed, uint32_t tag);

int der_length(ab_t* b, size_t n);

int der_int_bytes(int64_t v, uint8_t* out);

int utf8_ok(const uint8_t* p, size_t n);

static inline int printable_ok(const uint8_t* p, size_t n)
{
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                              "0123456789 '()+,-./:=?";
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == 0 || !strchr(set, (int)c))
            return 0;
    }
    return 1;
}

int time_ok(const uint8_t* p, size_t n, int tag);

int utf8_put(uint8_t* q, uint32_t cp);

JSValue asn1_node(JSContext* ctx, int cls, int tag, int constructed,
    JSValue value);

int node_int(JSContext* ctx, JSValueConst v, const char* name,
    int64_t lo, int64_t hi, int64_t* out);

int der_int_from_decimal(JSContext* ctx, const char* s,
    uint8_t* content, int* clen);

int asn1_enc_node(JSContext* ctx, JSValueConst node, ab_t* out,
    int depth);

int asn1_der_set_cmp(const void* a, const void* b);

JSValue asn1_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
