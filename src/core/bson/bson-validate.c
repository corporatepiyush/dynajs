#include "bson.h"

size_t dyn_bson_utf8_valid_prefix(const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c;
        if (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (!(w & 0x8080808080808080ull)) {
                i += 8;
                continue;
            }
        }
        c = p[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80)
                return i;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            uint8_t c1;
            if (i + 2 >= n)
                return i;
            c1 = p[i + 1];
            if ((c1 & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                return i;
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F))
                return i;
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            uint8_t c1;
            if (i + 3 >= n)
                return i;
            c1 = p[i + 1];
            if ((c1 & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
                return i;
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
                return i;
            i += 4;
        } else {
            return i;
        }
    }
    return n;
}

static int bson_utf8_ok(const uint8_t* p, size_t n)
{
    return dyn_bson_utf8_valid_prefix(p, n) == n;
}

static int bson_str_ok(const uint8_t* v)
{
    uint32_t l = dyn_bson_rd32(v);
    return bson_utf8_ok(v + 4, l - 1) && !memchr(v + 4, 0, l - 1);
}

static int bson_validate_doc(const uint8_t* doc, size_t avail, int depth, int max_depth,
    dyn_bson_err* err)
{
    dyn_bson_iter it;
    int rc;
    if (depth > max_depth) {
        err->code = DYN_BSON_E_DEPTH;
        err->offset = 0;
        return -1;
    }
    rc = dyn_bson_iter_init(&it, doc, avail);
    if (rc < 0) {
        err->code = rc;
        err->offset = 0;
        return -1;
    }
    for (;;) {
        uint32_t at = it.off;
        rc = dyn_bson_iter_next(&it);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            err->code = rc;
            err->offset = at;
            return -1;
        }
        if (!bson_utf8_ok((const uint8_t*)it.key, it.klen)) {
            err->code = DYN_BSON_E_UTF8;
            err->offset = at;
            return -1;
        }
        switch (it.type) {
        case DYN_BSON_STRING:
        case DYN_BSON_CODE:
        case DYN_BSON_SYMBOL:
            if (!bson_utf8_ok(it.val + 4, dyn_bson_rd32(it.val) - 1)) {
                err->code = DYN_BSON_E_UTF8;
                err->offset = at;
                return -1;
            }
            break;
        case DYN_BSON_DBPOINTER:
            if (!bson_str_ok(it.val)) {
                err->code = DYN_BSON_E_UTF8;
                err->offset = at;
                return -1;
            }
            break;
        case DYN_BSON_BOOL:
            if (it.val[0] > 1) {
                err->code = DYN_BSON_E_VALUE;
                err->offset = at;
                return -1;
            }
            break;
        case DYN_BSON_REGEX: {
            size_t pl = strlen((const char*)it.val);
            const uint8_t* o = it.val + pl + 1;
            if (!bson_utf8_ok(it.val, pl) || !bson_utf8_ok(o, strlen((const char*)o))) {
                err->code = DYN_BSON_E_UTF8;
                err->offset = at;
                return -1;
            }
            break;
        }
        case DYN_BSON_DOC:
        case DYN_BSON_ARRAY: {
            dyn_bson_err sub;
            if (bson_validate_doc(it.val, it.vlen, depth + 1, max_depth, &sub) < 0) {
                err->code = sub.code;
                err->offset = (uint32_t)(it.val - doc) + sub.offset;
                return -1;
            }
            break;
        }
        case DYN_BSON_CODE_W_SCOPE: {
            dyn_bson_err sub;
            uint32_t sl = dyn_bson_rd32(it.val + 4);
            const uint8_t* scope = it.val + 8 + sl;
            if (!bson_utf8_ok(it.val + 8, sl - 1)) {
                err->code = DYN_BSON_E_UTF8;
                err->offset = at;
                return -1;
            }
            if (bson_validate_doc(scope, it.vlen - 8 - sl, depth + 1, max_depth, &sub) < 0) {
                err->code = sub.code;
                err->offset = (uint32_t)(scope - doc) + sub.offset;
                return -1;
            }
            if (dyn_bson_rd32(scope) != it.vlen - 8 - sl) {
                err->code = DYN_BSON_E_LENGTH;
                err->offset = at;
                return -1;
            }
            break;
        }
        default:
            break;
        }
    }
}

static int bson_quick_doc(const uint8_t* p, uint32_t n, int depth, int max_depth)
{
    uint32_t off = 4, end = n - 1, avail, l, k;
    for (;;) {
        uint8_t t = p[off], hi = 0;
        if (t == 0)
            return off == end;
        k = ++off;
        while (off < end && p[off] != 0)
            hi |= p[off++];
        if (off >= end || ((hi & 0x80) && !bson_utf8_ok(p + k, off - k)))
            return 0;
        off++;
        avail = end - off;
        switch (t) {
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
            if (avail < 1 || p[off] > 1)
                return 0;
            l = 1;
            break;
        case DYN_BSON_UNDEFINED:
        case DYN_BSON_NULL:
        case DYN_BSON_MINKEY:
        case DYN_BSON_MAXKEY:
            l = 0;
            break;
        case DYN_BSON_STRING:
            if (avail < 5)
                return 0;
            l = dyn_bson_rd32(p + off);
            if (l < 1 || l > avail - 4 || p[off + 4 + l - 1] != 0 || !bson_utf8_ok(p + off + 4, l - 1))
                return 0;
            l += 4;
            break;
        case DYN_BSON_BINARY:
            if (avail < 5)
                return 0;
            l = dyn_bson_rd32(p + off);
            if (l > avail - 5 || (p[off + 4] == 2 && (l < 4 || dyn_bson_rd32(p + off + 5) != l - 4)))
                return 0;
            l += 5;
            break;
        case DYN_BSON_DOC:
        case DYN_BSON_ARRAY:
            if (avail < 5 || depth >= max_depth)
                return 0;
            l = dyn_bson_rd32(p + off);
            if (l < 5 || l > avail || p[off + l - 1] != 0 || !bson_quick_doc(p + off, l, depth + 1, max_depth))
                return 0;
            break;
        default:
            return 0;
        }
        if (l > avail)
            return 0;
        off += l;
    }
}

int dyn_bson_validate(const uint8_t* doc, size_t n, int max_depth, dyn_bson_err* err)
{
    dyn_bson_err e = { 0, 0 };
    if (n >= 5 && n <= UINT32_MAX && dyn_bson_rd32(doc) == n && doc[n - 1] == 0 && bson_quick_doc(doc, (uint32_t)n, 0, max_depth)) {
        if (err)
            *err = e;
        return 0;
    }
    if (n < 5) {
        e.code = DYN_BSON_E_SHORT;
        if (err)
            *err = e;
        return e.code;
    }
    if (bson_validate_doc(doc, n, 0, max_depth, &e) < 0) {
        if (err)
            *err = e;
        return e.code;
    }
    if (dyn_bson_rd32(doc) != n) {
        e.code = DYN_BSON_E_TRAILING;
        e.offset = dyn_bson_rd32(doc);
        if (err)
            *err = e;
        return e.code;
    }
    if (err)
        *err = e;
    return 0;
}
