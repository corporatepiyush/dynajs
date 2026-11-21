#include <stdio.h>
#include "dd.h"

typedef struct {
    char* p;
    size_t n, cap;
} dd_rbuf;

static void rb_put(dd_rbuf* b, const char* s, size_t n)
{
    size_t room = b->cap - b->n;
    if (room <= 1)
        return;
    if (n > room - 1)
        n = room - 1;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void rb_puts(dd_rbuf* b, const char* s)
{
    rb_put(b, s, strlen(s));
}

static void rb_fmt(dd_rbuf* b, double d)
{
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%.17g", d);
    if (n > 0)
        rb_put(b, tmp, (size_t)n);
}

static void repr_val(dd_rbuf* b, dyn_bson_val v, int depth);

static void repr_doc(dd_rbuf* b, const uint8_t* doc, int arr, int depth)
{
    dyn_bson_iter it;
    int first = 1;
    rb_puts(b, arr ? "[ " : "{ ");
    if (depth > 4) {
        rb_puts(b, "... ");
        rb_puts(b, arr ? "]" : "}");
        return;
    }
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0 && b->n + 8 < b->cap) {
        if (!first)
            rb_puts(b, ", ");
        first = 0;
        if (!arr) {
            rb_put(b, it.key, it.klen);
            rb_puts(b, ": ");
        }
        repr_val(b, dyn_bson_iter_val(&it), depth + 1);
    }
    rb_puts(b, arr ? " ]" : " }");
}

static void repr_val(dd_rbuf* b, dyn_bson_val v, int depth)
{
    char tmp[64];
    switch (v.type) {
    case 0:
        rb_puts(b, "missing");
        break;
    case DYN_BSON_DOUBLE:
        rb_fmt(b, dyn_bson_rdf64(v.val));
        break;
    case DYN_BSON_INT32:
        snprintf(tmp, sizeof tmp, "%d", (int32_t)dyn_bson_rd32(v.val));
        rb_puts(b, tmp);
        break;
    case DYN_BSON_INT64:
        snprintf(tmp, sizeof tmp, "%lld", (long long)(int64_t)dyn_bson_rd64(v.val));
        rb_puts(b, tmp);
        break;
    case DYN_BSON_STRING:
    case DYN_BSON_SYMBOL:
        rb_puts(b, "\"");
        rb_put(b, (const char*)v.val + 4, dyn_bson_rd32(v.val) - 1);
        rb_puts(b, "\"");
        break;
    case DYN_BSON_DOC:
        repr_doc(b, v.val, 0, depth);
        break;
    case DYN_BSON_ARRAY:
        repr_doc(b, v.val, 1, depth);
        break;
    case DYN_BSON_OID: {
        int i;
        rb_puts(b, "ObjectId('");
        for (i = 0; i < 12; i++) {
            snprintf(tmp, sizeof tmp, "%02x", v.val[i]);
            rb_puts(b, tmp);
        }
        rb_puts(b, "')");
        break;
    }
    case DYN_BSON_BOOL:
        rb_puts(b, v.val[0] ? "true" : "false");
        break;
    case DYN_BSON_NULL:
    case DYN_BSON_UNDEFINED:
        rb_puts(b, "null");
        break;
    case DYN_BSON_DATE:
        snprintf(tmp, sizeof tmp, "new Date(%lld)", (long long)(int64_t)dyn_bson_rd64(v.val));
        rb_puts(b, tmp);
        break;
    case DYN_BSON_DEC128: {
        char d[64];
        dyn_bson_dec128_to_string(v.val, d, sizeof d);
        rb_puts(b, "Decimal128(\"");
        rb_puts(b, d);
        rb_puts(b, "\")");
        break;
    }
    case DYN_BSON_REGEX:
        rb_puts(b, "/");
        rb_puts(b, (const char*)v.val);
        rb_puts(b, "/");
        rb_puts(b, (const char*)v.val + strlen((const char*)v.val) + 1);
        break;
    case DYN_BSON_MINKEY:
        rb_puts(b, "MinKey");
        break;
    case DYN_BSON_MAXKEY:
        rb_puts(b, "MaxKey");
        break;
    default:
        snprintf(tmp, sizeof tmp, "<type 0x%02x>", v.type);
        rb_puts(b, tmp);
        break;
    }
}

const char* dd_repr(dyn_bson_val v, char* out, size_t cap)
{
    dd_rbuf b;
    b.p = out;
    b.n = 0;
    b.cap = cap;
    if (cap)
        out[0] = 0;
    repr_val(&b, v, 0);
    return out;
}
