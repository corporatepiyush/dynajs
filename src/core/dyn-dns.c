/* dyn-dns -- see dyn-dns.h. */
#include "dyn-dns.h"

#include <string.h>

/* A label length octet's top two bits: 00 = label, 11 = pointer. 01 and 10 are
 * reserved and have never been assigned -- rejecting them is what stops a
 * parser from inventing a meaning for them. */
#define LBL_MASK  0xc0u
#define LBL_PTR   0xc0u
#define LBL_PLAIN 0x00u

const char *dyn_dns_strerror(int code)
{
    switch (code) {
    case DYN_DNS_OK:       return "ok";
    case DYN_DNS_E_SHORT:  return "message truncated";
    case DYN_DNS_E_LOOP:   return "compression pointer did not point backward";
    case DYN_DNS_E_NAME:   return "name or label exceeds its RFC 1035 limit";
    case DYN_DNS_E_FORMAT: return "malformed message";
    default:               return "unknown error";
    }
}

int dyn_dns_name_decode(const uint8_t *msg, size_t len, size_t off,
                        char *out, size_t outcap)
{
    size_t pos = off, total = 0;
    int ret = -1;            /* offset after the name, set at the first pointer */
    size_t limit = off;      /* a pointer must land strictly before this */

    if (!msg || !out || outcap == 0)
        return DYN_DNS_E_FORMAT;
    out[0] = '\0';

    for (;;) {
        uint8_t c;
        if (pos >= len)
            return DYN_DNS_E_SHORT;
        c = msg[pos];

        if ((c & LBL_MASK) == LBL_PTR) {
            size_t target;
            if (pos + 1 >= len)
                return DYN_DNS_E_SHORT;
            target = (size_t)((c & 0x3fu) << 8) | msg[pos + 1];
            /* STRICTLY BACKWARD. A cycle needs some pointer to jump forward or
             * to itself, so forbidding that makes a loop unrepresentable --
             * stronger than counting jumps, which still walks the whole chain. */
            if (target >= limit)
                return DYN_DNS_E_LOOP;
            if (ret < 0)
                ret = (int)(pos + 2);   /* the name ends here in the wire stream */
            limit = target;             /* each hop must go strictly further back */
            pos = target;
            continue;
        }
        if ((c & LBL_MASK) != LBL_PLAIN)
            return DYN_DNS_E_FORMAT;    /* 01/10 are reserved, never assigned */

        if (c == 0) {                   /* root label: the name is complete */
            if (ret < 0)
                ret = (int)(pos + 1);
            return ret;
        }
        if (c > DYN_DNS_MAX_LABEL)
            return DYN_DNS_E_NAME;
        if (pos + 1 + c > len)
            return DYN_DNS_E_SHORT;

        /* +1 for the dot this label will need. Checked BEFORE copying, because
         * the cap is what bounds a pointer chain's expansion. */
        if (total + c + 1 > DYN_DNS_MAX_NAME)
            return DYN_DNS_E_NAME;
        if (total + c + 2 > outcap)
            return DYN_DNS_E_NAME;

        if (total)
            out[total++] = '.';
        memcpy(out + total, msg + pos + 1, c);
        total += c;
        out[total] = '\0';
        pos += 1 + c;
    }
}

int dyn_dns_name_encode(const char *name, uint8_t *out, size_t outcap)
{
    size_t w = 0, i = 0;

    if (!name || !out)
        return DYN_DNS_E_FORMAT;
    /* A single trailing dot is the root and is not a label. */
    while (name[i]) {
        size_t start = i, n;
        while (name[i] && name[i] != '.')
            i++;
        n = i - start;
        if (n == 0)
            return DYN_DNS_E_FORMAT;    /* empty label: ".." or a leading dot */
        if (n > DYN_DNS_MAX_LABEL)
            return DYN_DNS_E_NAME;
        if (w + 1 + n + 1 > outcap)
            return DYN_DNS_E_SHORT;
        if (w + 1 + n > DYN_DNS_MAX_NAME)
            return DYN_DNS_E_NAME;
        out[w++] = (uint8_t)n;
        memcpy(out + w, name + start, n);
        w += n;
        if (name[i] == '.')
            i++;
    }
    if (w + 1 > outcap)
        return DYN_DNS_E_SHORT;
    out[w++] = 0;                       /* root */
    return (int)w;
}

int dyn_dns_hdr_decode(const uint8_t *msg, size_t len, dyn_dns_hdr_t *h)
{
    if (!msg || !h || len < DYN_DNS_HDR_LEN)
        return DYN_DNS_E_SHORT;
    h->id      = (uint16_t)((msg[0] << 8) | msg[1]);
    h->flags   = (uint16_t)((msg[2] << 8) | msg[3]);
    h->qdcount = (uint16_t)((msg[4] << 8) | msg[5]);
    h->ancount = (uint16_t)((msg[6] << 8) | msg[7]);
    h->nscount = (uint16_t)((msg[8] << 8) | msg[9]);
    h->arcount = (uint16_t)((msg[10] << 8) | msg[11]);
    return DYN_DNS_OK;
}

int dyn_dns_hdr_encode(const dyn_dns_hdr_t *h, uint8_t *out, size_t outcap)
{
    if (!h || !out || outcap < DYN_DNS_HDR_LEN)
        return DYN_DNS_E_SHORT;
    out[0] = (uint8_t)(h->id >> 8);      out[1]  = (uint8_t)h->id;
    out[2] = (uint8_t)(h->flags >> 8);   out[3]  = (uint8_t)h->flags;
    out[4] = (uint8_t)(h->qdcount >> 8); out[5]  = (uint8_t)h->qdcount;
    out[6] = (uint8_t)(h->ancount >> 8); out[7]  = (uint8_t)h->ancount;
    out[8] = (uint8_t)(h->nscount >> 8); out[9]  = (uint8_t)h->nscount;
    out[10] = (uint8_t)(h->arcount >> 8); out[11] = (uint8_t)h->arcount;
    return DYN_DNS_HDR_LEN;
}

int dyn_dns_build_query(uint16_t id, const char *name, uint16_t type,
                        uint8_t *out, size_t outcap)
{
    dyn_dns_hdr_t h;
    int n;
    size_t w;

    memset(&h, 0, sizeof(h));
    h.id = id;
    h.flags = 0x0100;                   /* RD */
    h.qdcount = 1;
    if (dyn_dns_hdr_encode(&h, out, outcap) < 0)
        return DYN_DNS_E_SHORT;
    w = DYN_DNS_HDR_LEN;
    n = dyn_dns_name_encode(name, out + w, outcap - w);
    if (n < 0)
        return n;
    w += (size_t)n;
    if (w + 4 > outcap)
        return DYN_DNS_E_SHORT;
    out[w++] = (uint8_t)(type >> 8); out[w++] = (uint8_t)type;
    out[w++] = 0;                    out[w++] = DYN_DNS_C_IN;
    return (int)w;
}

int dyn_dns_skip_questions(const uint8_t *msg, size_t len, size_t *off,
                           uint16_t qdcount)
{
    char scratch[DYN_DNS_MAX_NAME + 1];
    uint16_t i;

    if (!msg || !off)
        return DYN_DNS_E_FORMAT;
    for (i = 0; i < qdcount; i++) {
        int after = dyn_dns_name_decode(msg, len, *off, scratch, sizeof(scratch));
        if (after < 0)
            return after;
        *off = (size_t)after;
        if (*off + 4 > len)
            return DYN_DNS_E_SHORT;
        *off += 4;                      /* QTYPE + QCLASS */
    }
    return DYN_DNS_OK;
}

int dyn_dns_rr_decode(const uint8_t *msg, size_t len, size_t *off,
                      dyn_dns_rr_t *rr)
{
    int after;
    size_t p;

    if (!msg || !off || !rr)
        return DYN_DNS_E_FORMAT;
    after = dyn_dns_name_decode(msg, len, *off, rr->name, sizeof(rr->name));
    if (after < 0)
        return after;
    p = (size_t)after;
    if (p + 10 > len)
        return DYN_DNS_E_SHORT;
    rr->type  = (uint16_t)((msg[p] << 8) | msg[p + 1]);
    rr->cls   = (uint16_t)((msg[p + 2] << 8) | msg[p + 3]);
    rr->ttl   = ((uint32_t)msg[p + 4] << 24) | ((uint32_t)msg[p + 5] << 16) |
                ((uint32_t)msg[p + 6] << 8)  |  (uint32_t)msg[p + 7];
    rr->rdlen = (uint16_t)((msg[p + 8] << 8) | msg[p + 9]);
    p += 10;
    /* RDLENGTH is attacker-controlled: bound it against what REMAINS before
     * using it, or the borrowed rdata pointer runs off the message. */
    if (rr->rdlen > len - p)
        return DYN_DNS_E_SHORT;
    rr->rdata = msg + p;
    *off = p + rr->rdlen;
    return DYN_DNS_OK;
}

int dyn_dns_begin_response(const uint8_t *query, size_t qlen, int rcode,
                           uint8_t *out, size_t outcap)
{
    dyn_dns_hdr_t h;
    size_t off = DYN_DNS_HDR_LEN;
    char scratch[DYN_DNS_MAX_NAME + 1];
    int after;

    if (!query || !out || qlen < DYN_DNS_HDR_LEN)
        return DYN_DNS_E_SHORT;
    if (dyn_dns_hdr_decode(query, qlen, &h) < 0)
        return DYN_DNS_E_SHORT;
    if (h.qdcount != 1)
        return DYN_DNS_E_FORMAT;   /* exactly one question, or we do not answer */

    /* Echo the question VERBATIM. Re-encoding it would change the bytes a
     * client compares against, and a client that checks (as ours does) would
     * then reject its own answer. */
    after = dyn_dns_name_decode(query, qlen, off, scratch, sizeof(scratch));
    if (after < 0)
        return after;
    if ((size_t)after + 4 > qlen)
        return DYN_DNS_E_SHORT;
    {
        size_t qsec = (size_t)after + 4 - DYN_DNS_HDR_LEN;
        if (DYN_DNS_HDR_LEN + qsec > outcap)
            return DYN_DNS_E_SHORT;
        h.flags = (uint16_t)((h.flags & 0x0100) |  /* keep RD */
                             0x8000 |              /* QR: this is a response */
                             0x0080 |              /* RA */
                             (rcode & 0x0f));
        h.ancount = h.nscount = h.arcount = 0;
        if (dyn_dns_hdr_encode(&h, out, outcap) < 0)
            return DYN_DNS_E_SHORT;
        memcpy(out + DYN_DNS_HDR_LEN, query + DYN_DNS_HDR_LEN, qsec);
        off = DYN_DNS_HDR_LEN + qsec;
    }
    return (int)off;
}

int dyn_dns_add_answer(uint8_t *out, size_t outcap, size_t off, uint16_t type,
                       uint32_t ttl, const uint8_t *addr, uint16_t addrlen)
{
    if (!out || !addr)
        return DYN_DNS_E_FORMAT;
    if (off + 12 + addrlen > outcap)
        return DYN_DNS_E_SHORT;
    out[off++] = 0xc0; out[off++] = DYN_DNS_HDR_LEN;   /* name -> the question */
    out[off++] = (uint8_t)(type >> 8);  out[off++] = (uint8_t)type;
    out[off++] = 0;                     out[off++] = DYN_DNS_C_IN;
    out[off++] = (uint8_t)(ttl >> 24);  out[off++] = (uint8_t)(ttl >> 16);
    out[off++] = (uint8_t)(ttl >> 8);   out[off++] = (uint8_t)ttl;
    out[off++] = (uint8_t)(addrlen >> 8); out[off++] = (uint8_t)addrlen;
    memcpy(out + off, addr, addrlen);
    return (int)(off + addrlen);
}

void dyn_dns_set_ancount(uint8_t *out, uint16_t n)
{
    out[6] = (uint8_t)(n >> 8);
    out[7] = (uint8_t)n;
}
