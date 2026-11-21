#include "bson.h"

static int bson_cstr_len(const uint8_t* p, size_t avail, uint32_t* out)
{
    const uint8_t* z = memchr(p, 0, avail);
    if (!z)
        return DYN_BSON_E_TERM;
    *out = (uint32_t)(z - p);
    return 0;
}

static int bson_str_size(const uint8_t* v, size_t avail, uint32_t* out)
{
    uint32_t l;
    if (avail < 5)
        return DYN_BSON_E_SHORT;
    l = dyn_bson_rd32(v);
    if (l < 1 || l > avail - 4)
        return DYN_BSON_E_LENGTH;
    if (v[4 + l - 1] != 0)
        return DYN_BSON_E_TERM;
    *out = 4 + l;
    return 0;
}

int dyn_bson_type_valid(uint8_t type)
{
    return (type >= DYN_BSON_DOUBLE && type <= DYN_BSON_DEC128) || type == DYN_BSON_MINKEY
        || type == DYN_BSON_MAXKEY;
}

int dyn_bson_value_size(uint8_t type, const uint8_t* v, size_t avail, uint32_t* out)
{
    uint32_t l, a, b;
    int rc;
    switch (type) {
    case DYN_BSON_DOUBLE:
    case DYN_BSON_DATE:
    case DYN_BSON_TIMESTAMP:
    case DYN_BSON_INT64:
        l = 8;
        break;
    case DYN_BSON_INT32:
        l = 4;
        break;
    case DYN_BSON_DEC128:
        l = 16;
        break;
    case DYN_BSON_OID:
        l = 12;
        break;
    case DYN_BSON_BOOL:
        l = 1;
        break;
    case DYN_BSON_UNDEFINED:
    case DYN_BSON_NULL:
    case DYN_BSON_MINKEY:
    case DYN_BSON_MAXKEY:
        l = 0;
        break;
    case DYN_BSON_STRING:
    case DYN_BSON_CODE:
    case DYN_BSON_SYMBOL:
        return bson_str_size(v, avail, out);
    case DYN_BSON_DOC:
    case DYN_BSON_ARRAY:
        if (avail < 5)
            return DYN_BSON_E_SHORT;
        l = dyn_bson_rd32(v);
        if (l < 5 || l > avail)
            return DYN_BSON_E_LENGTH;
        if (v[l - 1] != 0)
            return DYN_BSON_E_TERM;
        *out = l;
        return 0;
    case DYN_BSON_BINARY:
        if (avail < 5)
            return DYN_BSON_E_SHORT;
        l = dyn_bson_rd32(v);
        if (l > avail - 5)
            return DYN_BSON_E_LENGTH;
        if (v[4] == 2) {
            if (l < 4 || dyn_bson_rd32(v + 5) != l - 4)
                return DYN_BSON_E_LENGTH;
        }
        *out = 5 + l;
        return 0;
    case DYN_BSON_REGEX:
        if ((rc = bson_cstr_len(v, avail, &a)) < 0)
            return rc;
        if ((rc = bson_cstr_len(v + a + 1, avail - a - 1, &b)) < 0)
            return rc;
        *out = a + 1 + b + 1;
        return 0;
    case DYN_BSON_DBPOINTER:
        if ((rc = bson_str_size(v, avail, &a)) < 0)
            return rc;
        if (avail - a < 12)
            return DYN_BSON_E_SHORT;
        *out = a + 12;
        return 0;
    case DYN_BSON_CODE_W_SCOPE:
        if (avail < 14)
            return DYN_BSON_E_SHORT;
        l = dyn_bson_rd32(v);
        if (l < 14 || l > avail)
            return DYN_BSON_E_LENGTH;
        if ((rc = bson_str_size(v + 4, l - 4, &a)) < 0)
            return rc;
        if (l - 4 - a < 5)
            return DYN_BSON_E_LENGTH;
        b = dyn_bson_rd32(v + 4 + a);
        if (b != l - 4 - a || v[l - 1] != 0)
            return DYN_BSON_E_LENGTH;
        *out = l;
        return 0;
    default:
        return DYN_BSON_E_TYPE;
    }
    if (l > avail)
        return DYN_BSON_E_SHORT;
    *out = l;
    return 0;
}

int dyn_bson_iter_init(dyn_bson_iter* it, const uint8_t* doc, size_t avail)
{
    uint32_t n;
    memset(it, 0, sizeof *it);
    if (avail < 5)
        return DYN_BSON_E_SHORT;
    n = dyn_bson_rd32(doc);
    if (n < 5 || n > avail)
        return DYN_BSON_E_LENGTH;
    if (doc[n - 1] != 0)
        return DYN_BSON_E_TERM;
    it->p = doc;
    it->n = n;
    it->off = 4;
    return 0;
}

static const int8_t bson_fixed_size[20] = { -1, 8, -1, -1, -1, -1, 0, 12, 1, 8, 0, -1, -1, -1, -1, -1, 4, 8, 8, 16 };

static inline const uint8_t* bson_key_end(const uint8_t* p, size_t avail)
{
    size_t i, lim = avail < 24 ? avail : 24;
    for (i = 0; i < lim; i++)
        if (!p[i])
            return p + i;
    return lim < avail ? memchr(p + lim, 0, avail - lim) : NULL;
}

int dyn_bson_iter_next(dyn_bson_iter* it)
{
    uint32_t off = it->off, klen, vlen;
    size_t avail;
    const uint8_t* z;
    int rc;
    if (off >= it->n)
        return 0;
    it->type = it->p[off];
    if (it->type == 0)
        return off == it->n - 1 ? 0 : DYN_BSON_E_TERM;
    off++;
    avail = (size_t)it->n - 1 - off;
    z = bson_key_end(it->p + off, avail);
    if (!z)
        return DYN_BSON_E_KEY;
    klen = (uint32_t)(z - (it->p + off));
    it->key = (const char*)(it->p + off);
    it->klen = klen;
    off += klen + 1;
    avail = (size_t)it->n - 1 - off;
    if (it->type < 20 && bson_fixed_size[it->type] >= 0) {
        vlen = (uint32_t)bson_fixed_size[it->type];
        if (vlen > avail)
            return DYN_BSON_E_SHORT;
    } else if ((it->type == DYN_BSON_DOC || it->type == DYN_BSON_ARRAY) && avail >= 5) {
        vlen = dyn_bson_rd32(it->p + off);
        if (vlen < 5 || vlen > avail)
            return DYN_BSON_E_LENGTH;
        if (it->p[off + vlen - 1] != 0)
            return DYN_BSON_E_TERM;
    } else if (it->type == DYN_BSON_STRING && avail >= 5) {
        uint32_t l = dyn_bson_rd32(it->p + off);
        if (l < 1 || l > avail - 4)
            return DYN_BSON_E_LENGTH;
        if (it->p[off + 4 + l - 1] != 0)
            return DYN_BSON_E_TERM;
        vlen = 4 + l;
    } else if ((rc = dyn_bson_value_size(it->type, it->p + off, avail, &vlen)) < 0)
        return rc;
    it->val = it->p + off;
    it->vlen = vlen;
    it->off = off + vlen;
    return 1;
}

int dyn_bson_find(const uint8_t* doc, const char* key, size_t klen, dyn_bson_val* out)
{
    dyn_bson_iter it;
    int rc;
    if (dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc)) < 0)
        return 0;
    while ((rc = dyn_bson_iter_next(&it)) > 0) {
        if (it.klen == klen && memcmp(it.key, key, klen) == 0) {
            *out = dyn_bson_iter_val(&it);
            return 1;
        }
    }
    return 0;
}

const char* dyn_bson_strerror(int code)
{
    switch (code) {
    case DYN_BSON_OK:
        return "ok";
    case DYN_BSON_E_SHORT:
        return "truncated input";
    case DYN_BSON_E_LENGTH:
        return "length field out of bounds";
    case DYN_BSON_E_TERM:
        return "missing terminator";
    case DYN_BSON_E_TYPE:
        return "unknown element type";
    case DYN_BSON_E_KEY:
        return "unterminated key";
    case DYN_BSON_E_UTF8:
        return "invalid UTF-8";
    case DYN_BSON_E_DEPTH:
        return "nesting too deep";
    case DYN_BSON_E_VALUE:
        return "invalid value";
    case DYN_BSON_E_TRAILING:
        return "trailing bytes after the document";
    case DYN_BSON_E_OOM:
        return "out of memory";
    }
    return "malformed";
}
