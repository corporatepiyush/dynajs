#include "asn1-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASN1)

static int asn1_enc_int(JSContext* ctx, JSValueConst v, uint8_t* content,
    int* clen)
{
    int64_t iv;
    if (JS_IsBigInt(ctx, v)) {
        JSValue back;
        int same;
        if (JS_ToBigInt64(ctx, &iv, v) < 0)
            return -1;
        back = JS_NewBigInt64(ctx, iv);
        if (JS_IsException(back))
            return -1;
        same = JS_SameValue(ctx, back, v);
        JS_FreeValue(ctx, back);
        if (same < 0)
            return -1;
        if (same) {
            *clen = der_int_bytes(iv, content);
            return 0;
        }
        {
            size_t sl;
            const char* s = JS_ToCStringLen(ctx, &sl, v);
            int rc;
            if (!s)
                return -1;
            rc = der_int_from_decimal(ctx, s, content, clen);
            JS_FreeCString(ctx, s);
            return rc;
        }
    } else if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (d != floor(d) || isinf(d)) {
            JS_ThrowTypeError(ctx, "ASN.1: INTEGER value must be an integer");
            return -1;
        }
        if (d < -9.223372036854775808e18 || d >= 9.223372036854775808e18) {
            JS_ThrowRangeError(ctx, "ASN.1: INTEGER out of the int64 range");
            return -1;
        }
        iv = (int64_t)d;
    } else {
        JS_ThrowTypeError(ctx, "ASN.1: INTEGER value must be a Number or "
                               "BigInt");
        return -1;
    }
    *clen = der_int_bytes(iv, content);
    return 0;
}

static int asn1_enc_bits(JSContext* ctx, JSValueConst vval, ab_t* out)
{
    const uint8_t* p;
    size_t n;
    JSValue hold = JS_UNDEFINED, vu, vb;
    int unused = 0;
    int br;
    uint8_t h;

    vu = JS_GetPropertyStr(ctx, vval, "unused");
    vb = JS_GetPropertyStr(ctx, vval, "bytes");
    if (JS_IsException(vu) || JS_IsException(vb)) {
        JS_FreeValue(ctx, vu);
        JS_FreeValue(ctx, vb);
        return -1;
    }
    if (!JS_IsUndefined(vu)) {
        if (!JS_IsNumber(vu)) {
            JS_FreeValue(ctx, vu);
            JS_FreeValue(ctx, vb);
            JS_ThrowTypeError(ctx, "ASN.1: BIT STRING 'unused' must be a "
                                   "number");
            return -1;
        }
        if (JS_ToInt32(ctx, &unused, vu) < 0) {
            JS_FreeValue(ctx, vu);
            JS_FreeValue(ctx, vb);
            return -1;
        }
    }
    JS_FreeValue(ctx, vu);
    if (unused < 0 || unused > 7) {
        JS_FreeValue(ctx, vb);
        JS_ThrowRangeError(ctx, "ASN.1: BIT STRING unused-bits count %d is "
                                "not 0..7",
            unused);
        return -1;
    }
    br = asn1_bytes(ctx, vb, &p, &n, &hold);
    JS_FreeValue(ctx, vb);
    if (br < 0)
        return -1;
    if (br == 0) {
        JS_ThrowTypeError(ctx, "ASN.1: BIT STRING 'bytes' must be a "
                               "Uint8Array");
        return -1;
    }
    if (n == 0 && unused > 0) {
        JS_FreeValue(ctx, hold);
        JS_ThrowSyntaxError(ctx, "ASN.1: BIT STRING has unused bits but no "
                                 "payload octet");
        return -1;
    }
    if (n > 0 && unused > 0 && (p[n - 1] & ((1u << unused) - 1)) != 0) {
        JS_FreeValue(ctx, hold);
        JS_ThrowSyntaxError(ctx, "ASN.1: BIT STRING trailing unused bits must "
                                 "be zero in DER");
        return -1;
    }
    h = (uint8_t)unused;
    if (ab_write(out, &h, 1) < 0 || ab_write(out, p, n) < 0) {
        JS_FreeValue(ctx, hold);
        return -1;
    }
    JS_FreeValue(ctx, hold);
    return 0;
}

static int oid_emit_subid(ab_t* b, uint64_t v)
{
    uint8_t t[8];
    int n = 0, i;
    do {
        t[n++] = (uint8_t)(v & 0x7F);
        v >>= 7;
    } while (v);
    for (i = n - 1; i >= 0; i--) {
        uint8_t c = (uint8_t)(t[i] | (i ? 0x80 : 0));
        if (ab_write(b, &c, 1) < 0)
            return -1;
    }
    return 0;
}

static int asn1_enc_oid(JSContext* ctx, JSValueConst v, ab_t* out)
{
    size_t n;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    uint32_t arcs[ASN1_MAX_OID_ARCS];
    int narc = 0;
    size_t i = 0;
    int rc = -1;

    if (!s)
        return -1;
    while (i < n) {
        uint64_t a = 0;
        int digits = 0;
        if (narc >= ASN1_MAX_OID_ARCS) {
            JS_ThrowRangeError(ctx, "ASN.1: more than %d OID arcs",
                ASN1_MAX_OID_ARCS);
            goto done;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            a = a * 10 + (uint64_t)(s[i] - '0');
            if (a > ASN1_ARC_MAX) {
                JS_ThrowRangeError(ctx, "ASN.1: OID arc exceeds 2^32-1");
                goto done;
            }
            i++;
            digits++;
        }
        if (digits == 0) {
            JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string");
            goto done;
        }
        arcs[narc++] = (uint32_t)a;
        if (i == n)
            break;
        if (s[i] != '.') {
            JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string");
            goto done;
        }
        i++;
    }
    if (narc < 2) {
        JS_ThrowSyntaxError(ctx, "ASN.1: an OID needs at least two arcs");
        goto done;
    }
    if (n > 0 && s[n - 1] == '.') {
        JS_ThrowSyntaxError(ctx, "ASN.1: malformed OID string (trailing dot)");
        goto done;
    }
    if (arcs[0] > 2) {
        JS_ThrowRangeError(ctx, "ASN.1: first OID arc must be 0, 1 or 2");
        goto done;
    }
    if (arcs[0] < 2 && arcs[1] >= 40) {
        JS_ThrowRangeError(ctx, "ASN.1: second arc must be < 40 when the "
                                "first arc is 0 or 1");
        goto done;
    }
    if (oid_emit_subid(out, (uint64_t)arcs[0] * 40 + arcs[1]) < 0)
        goto done;
    rc = 0;
    for (i = 2; i < (size_t)narc; i++)
        if (oid_emit_subid(out, arcs[i]) < 0) {
            rc = -1;
            break;
        }
done:
    JS_FreeCString(ctx, s);
    return rc;
}

static int asn1_enc_string(JSContext* ctx, JSValueConst v, int tag, ab_t* out)
{
    size_t n;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    int rc;

    if (!s)
        return -1;
    if (tag == 19) {
        if (!printable_ok((const uint8_t*)s, n)) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "ASN.1: PrintableString contains characters "
                                   "outside the printable set");
            return -1;
        }
    } else if (tag == 23 || tag == 24) {
        if (!time_ok((const uint8_t*)s, n, tag)) {
            JS_FreeCString(ctx, s);
            JS_ThrowSyntaxError(ctx, tag == 23 ? "ASN.1: UTCTime must be canonical YYMMDDHHMMSSZ" : "ASN.1: GeneralizedTime must be canonical "
                                                                                                    "YYYYMMDDHHMMSSZ");
            return -1;
        }
    } else if (tag == 22) {
        size_t i;
        for (i = 0; i < n; i++)
            if (((const uint8_t*)s)[i] > 0x7F) {
                unsigned bad = ((const uint8_t*)s)[i];
                JS_FreeCString(ctx, s);
                JS_ThrowTypeError(ctx, "ASN.1: IA5String is ASCII; byte 0x%02X "
                                       "at offset %u is outside it",
                    bad, (unsigned)i);
                return -1;
            }
    } else if (tag == 12) {
        if (!utf8_ok((const uint8_t*)s, n)) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "ASN.1: UTF8String content would not be "
                                   "well-formed UTF-8 (a lone surrogate?)");
            return -1;
        }
    } else if (tag == 28 || tag == 30) {
        uint8_t* b;
        size_t i = 0, w = 0, unit = (tag == 30) ? 2 : 4, need;
        if (!utf8_ok((const uint8_t*)s, n)) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "ASN.1: %s value would carry an ill-formed "
                                   "sequence (a lone surrogate?)",
                tag == 30 ? "BMPString" : "UniversalString");
            return -1;
        }
        if (n > (SIZE_MAX - 4) / 4) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx, "ASN.1: %s value too large to re-encode",
                tag == 30 ? "BMPString" : "UniversalString");
            return -1;
        }
        need = n * 4 + 4;
        b = (uint8_t*)malloc(need);
        if (!b) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        while (i < n) {
            uint32_t cp;
            size_t k, width;
            uint8_t first = ((const uint8_t*)s)[i];
            if (first < 0x80) {
                width = 1;
                cp = first;
            } else if (first < 0xE0) {
                width = 2;
                cp = first & 0x1F;
            } else if (first < 0xF0) {
                width = 3;
                cp = first & 0x0F;
            } else {
                width = 4;
                cp = first & 0x07;
            }
            for (k = 1; k < width; k++)
                cp = (cp << 6) | (((const uint8_t*)s)[i + k] & 0x3F);
            i += width;
            if (tag == 30 && cp > 0xFFFF) {
                free(b);
                JS_FreeCString(ctx, s);
                JS_ThrowTypeError(ctx, "ASN.1: BMPString cannot represent "
                                       "U+%X (above U+FFFF); use utf8String or "
                                       "universalString",
                    cp);
                return -1;
            }
            for (k = unit; k > 0; k--) {
                b[w + k - 1] = (uint8_t)(cp & 0xFF);
                cp >>= 8;
            }
            w += unit;
        }
        JS_FreeCString(ctx, s);
        rc = ab_write(out, b, w);
        free(b);
        return rc;
    }
    rc = ab_write(out, (const uint8_t*)s, n);
    JS_FreeCString(ctx, s);
    return rc;
}

typedef struct {
    JSAtom cls, tag, constructed, value, length;
} asn1_atoms_t;

static int asn1_enc_node_a(JSContext* ctx, const asn1_atoms_t* A, JSValueConst node, ab_t* out,
    int depth);

static int asn1_enc_children(JSContext* ctx, const asn1_atoms_t* A, JSValueConst arr, ab_t* out,
    int depth)
{
    JSValue lv = JS_GetProperty(ctx, arr, A->length);
    int64_t len, k;

    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len < 0 || len > ASN1_MAX_CHILDREN) {
        JS_ThrowRangeError(ctx, "ASN.1: too many children (%lld)",
            (long long)len);
        return -1;
    }
    for (k = 0; k < len; k++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)k);
        int rc;
        if (JS_IsException(el))
            return -1;
        rc = asn1_enc_node_a(ctx, A, el, out, depth + 1);
        JS_FreeValue(ctx, el);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int asn1_enc_set_children(JSContext* ctx, const asn1_atoms_t* A, JSValueConst arr, ab_t* out,
    int depth)
{
    JSValue lv = JS_GetProperty(ctx, arr, A->length);
    int64_t len, k;
    ab_t* kids;
    size_t total = 0;
    int rc = -1;

    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    if (len < 0 || len > ASN1_MAX_CHILDREN) {
        JS_ThrowRangeError(ctx, "ASN.1: too many children (%lld)",
            (long long)len);
        return -1;
    }
    kids = (ab_t*)calloc((size_t)len ? (size_t)len : 1, sizeof(*kids));
    if (!kids) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (k = 0; k < len; k++) {
        JSValue el = JS_GetPropertyUint32(ctx, arr, (uint32_t)k);
        int rc2;
        if (JS_IsException(el))
            goto out;
        ab_init(&kids[k]);
        rc2 = asn1_enc_node_a(ctx, A, el, &kids[k], depth + 1);
        JS_FreeValue(ctx, el);
        if (rc2 < 0) {
            if (kids[k].oom)
                JS_ThrowOutOfMemory(ctx);
            goto out;
        }
        if (kids[k].oom) {
            JS_ThrowOutOfMemory(ctx);
            goto out;
        }
    }
    if (len > 1)
        qsort(kids, (size_t)len, sizeof(*kids), asn1_der_set_cmp);
    for (k = 0; k < len; k++)
        total += kids[k].n;
    rc = der_length(out, total);
    for (k = 0; rc == 0 && k < len; k++)
        rc = ab_write(out, kids[k].p, kids[k].n);
out:
    for (k = 0; k < len; k++)
        ab_free(&kids[k]);
    free(kids);
    return rc;
}

static int asn1_enc_node_a(JSContext* ctx, const asn1_atoms_t* A, JSValueConst node, ab_t* out,
    int depth)
{
    JSValue vcls, vtag, vcon, vval;
    int cls = 0, constructed = 0, isa;
    int64_t itag;
    uint32_t tag;
    int rc;

    if (depth >= ASN1_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "ASN.1: encode nesting exceeds %d",
            ASN1_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(node) || JS_IsArray(ctx, node) == 1) {
        JS_ThrowTypeError(ctx, "ASN1.encode: a node must be a plain object");
        return -1;
    }
    vcls = JS_GetProperty(ctx, node, A->cls);
    vtag = JS_GetProperty(ctx, node, A->tag);
    vcon = JS_GetProperty(ctx, node, A->constructed);
    vval = JS_GetProperty(ctx, node, A->value);
    if (JS_IsException(vcls) || JS_IsException(vtag) || JS_IsException(vcon) || JS_IsException(vval))
        goto fail;
    if (!JS_IsUndefined(vcls)) {
        if (node_int(ctx, vcls, "cls", 0, 3, &itag) < 0)
            goto fail;
        cls = (int)itag;
    }
    if (node_int(ctx, vtag, "tag", 0, ASN1_MAX_TAG, &itag) < 0)
        goto fail;
    tag = (uint32_t)itag;
    if (!JS_IsUndefined(vcon)) {
        int t = JS_ToBool(ctx, vcon);
        if (t < 0)
            goto fail;
        constructed = t;
    }
    isa = JS_IsArray(ctx, vval);
    if (isa < 0)
        goto fail;
    if (isa == 1) {
        ab_t tmp;
        if (!constructed) {
            JS_ThrowTypeError(ctx, "ASN.1: an array value requires "
                                   "constructed:true");
            goto fail;
        }
        if (cls == 0 && tag != 16 && tag != 17) {
            JS_ThrowTypeError(ctx, "ASN.1: constructed content is not DER for "
                                   "universal tag %u",
                tag);
            goto fail;
        }
        ab_init(&tmp);
        if (cls == 0 && tag == 17) {
            rc = der_ident(out, cls, 1, tag);
            if (rc == 0)
                rc = asn1_enc_set_children(ctx, A, vval, out, depth);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
        } else {
            if (asn1_enc_children(ctx, A, vval, &tmp, depth) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, cls, 1, tag);
            if (rc == 0)
                rc = der_length(out, tmp.n);
            if (rc == 0)
                rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
        }
    } else if (cls != 0) {
        const uint8_t* raw;
        size_t rn;
        JSValue hold = JS_UNDEFINED;
        int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
        if (br < 0)
            goto fail;
        if (br == 0) {
            JS_ThrowTypeError(ctx, "ASN.1: content of a class %d tag %u node "
                                   "must be a Uint8Array",
                cls, tag);
            goto fail;
        }
        rc = der_ident(out, cls, constructed, tag);
        if (rc == 0)
            rc = der_length(out, rn);
        if (rc == 0)
            rc = ab_write(out, raw, rn);
        JS_FreeValue(ctx, hold);
        if (rc < 0)
            goto fail;
    } else {
        if (constructed) {
            JS_ThrowTypeError(ctx,
                "ASN.1: universal tag %u is primitive in DER; constructed "
                "form is not available",
                (unsigned)tag);
            goto fail;
        }
        switch (tag) {
        case 1: {
            int t = JS_ToBool(ctx, vval);
            uint8_t c;
            if (t < 0)
                goto fail;
            c = t ? 0xFF : 0x00;
            rc = der_ident(out, 0, 0, 1);
            if (rc == 0)
                rc = der_length(out, 1);
            if (rc == 0)
                rc = ab_write(out, &c, 1);
            if (rc < 0)
                goto fail;
            break;
        }
        case 2: {
            uint8_t content[ASN1_MAX_BIGINT_BYTES];
            int clen;
            if (asn1_enc_int(ctx, vval, content, &clen) < 0)
                goto fail;
            rc = der_ident(out, 0, 0, 2);
            if (rc == 0)
                rc = der_length(out, (size_t)clen);
            if (rc == 0)
                rc = ab_write(out, content, (size_t)clen);
            if (rc < 0)
                goto fail;
            break;
        }
        case 3: {
            ab_t tmp;
            if (!JS_IsObject(vval) || JS_IsArray(ctx, vval) == 1) {
                JS_ThrowTypeError(ctx, "ASN.1: BIT STRING value must be "
                                       "{unused, bytes}");
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_bits(ctx, vval, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, 3);
            if (rc == 0)
                rc = der_length(out, tmp.n);
            if (rc == 0)
                rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        case 4: {
            const uint8_t* raw;
            size_t rn;
            JSValue hold = JS_UNDEFINED;
            int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
            if (br < 0)
                goto fail;
            if (br == 0) {
                JS_ThrowTypeError(ctx, "ASN.1: OCTET STRING value must be a "
                                       "Uint8Array");
                goto fail;
            }
            rc = der_ident(out, 0, 0, 4);
            if (rc == 0)
                rc = der_length(out, rn);
            if (rc == 0)
                rc = ab_write(out, raw, rn);
            JS_FreeValue(ctx, hold);
            if (rc < 0)
                goto fail;
            break;
        }
        case 5: {
            if (!JS_IsNull(vval) && !JS_IsUndefined(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: NULL value must be null");
                goto fail;
            }
            rc = der_ident(out, 0, 0, 5);
            if (rc == 0)
                rc = der_length(out, 0);
            if (rc < 0)
                goto fail;
            break;
        }
        case 6: {
            ab_t tmp;
            if (!JS_IsString(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: OID value must be a dotted "
                                       "string");
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_oid(ctx, vval, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, 6);
            if (rc == 0)
                rc = der_length(out, tmp.n);
            if (rc == 0)
                rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        case 12:
        case 19:
        case 22:
        case 23:
        case 24:
        case 28:
        case 30: {
            ab_t tmp;
            if (!JS_IsString(vval)) {
                JS_ThrowTypeError(ctx, "ASN.1: tag %u needs a string value",
                    tag);
                goto fail;
            }
            ab_init(&tmp);
            if (asn1_enc_string(ctx, vval, (int)tag, &tmp) < 0) {
                if (tmp.oom)
                    JS_ThrowOutOfMemory(ctx);
                ab_free(&tmp);
                goto fail;
            }
            rc = der_ident(out, 0, 0, tag);
            if (rc == 0)
                rc = der_length(out, tmp.n);
            if (rc == 0)
                rc = ab_write(out, tmp.p, tmp.n);
            ab_free(&tmp);
            if (rc < 0)
                goto fail;
            break;
        }
        default: {
            const uint8_t* raw;
            size_t rn;
            JSValue hold = JS_UNDEFINED;
            int br = asn1_bytes(ctx, vval, &raw, &rn, &hold);
            if (br < 0)
                goto fail;
            if (br == 0) {
                JS_ThrowTypeError(ctx, "ASN.1: universal tag %u needs a "
                                       "Uint8Array value",
                    tag);
                goto fail;
            }
            rc = der_ident(out, 0, 0, tag);
            if (rc == 0)
                rc = der_length(out, rn);
            if (rc == 0)
                rc = ab_write(out, raw, rn);
            JS_FreeValue(ctx, hold);
            if (rc < 0)
                goto fail;
            break;
        }
        }
    }
    JS_FreeValue(ctx, vcls);
    JS_FreeValue(ctx, vtag);
    JS_FreeValue(ctx, vcon);
    JS_FreeValue(ctx, vval);
    return 0;

fail:
    JS_FreeValue(ctx, vcls);
    JS_FreeValue(ctx, vtag);
    JS_FreeValue(ctx, vcon);
    JS_FreeValue(ctx, vval);
    return -1;
}

int asn1_enc_node(JSContext* ctx, JSValueConst node, ab_t* out,
    int depth)
{
    static const char* const names[5] = { "cls", "tag", "constructed", "value", "length" };
    JSAtom a[5];
    asn1_atoms_t A;
    int i, rc;
    for (i = 0; i < 5; i++) {
        a[i] = JS_NewAtom(ctx, names[i]);
        if (a[i] == JS_ATOM_NULL) {
            while (i-- > 0)
                JS_FreeAtom(ctx, a[i]);
            return -1;
        }
    }
    A.cls = a[0];
    A.tag = a[1];
    A.constructed = a[2];
    A.value = a[3];
    A.length = a[4];
    rc = asn1_enc_node_a(ctx, &A, node, out, depth);
    for (i = 0; i < 5; i++)
        JS_FreeAtom(ctx, a[i]);
    return rc;
}

#endif
