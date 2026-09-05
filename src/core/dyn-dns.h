/*
 * dyn-dns -- RFC 1035 message codec. Pure C: no sockets, no JS, so the parser
 * that reads attacker bytes can be fuzzed on its own.
 *
 * THE PARSER IS AN UNTRUSTED-INPUT SURFACE. A DNS message is whatever a peer
 * sends, and name compression makes it a pointer-chasing format, which is where
 * the historical bugs live:
 *
 *   - a compression pointer may form a LOOP (a -> b -> a), so a naive decoder
 *     never terminates. Defence here: a pointer must point STRICTLY BACKWARD,
 *     which makes a cycle unrepresentable rather than merely detected;
 *   - a chain of pointers can expand a small message into a huge name, so the
 *     255-octet total and 63-octet label caps are enforced while decoding, not
 *     after;
 *   - RDLENGTH is attacker-controlled and must be bounded by the bytes that
 *     actually remain before it is used to skip or copy.
 *
 * Decoding is ITERATIVE. A recursive decoder is a stack-exhaustion DoS on a
 * message a peer chooses (CLAUDE.md section 6), and there is no depth here that
 * needs a stack.
 */
#ifndef DYN_DNS_H
#define DYN_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_DNS_MAX_NAME   255   /* RFC 1035 4.1.4: total name octets */
#define DYN_DNS_MAX_LABEL   63   /* and per label */
#define DYN_DNS_HDR_LEN     12

/* Types and classes we name. AAAA is RFC 3596, not 1035. */
#define DYN_DNS_T_A      1
#define DYN_DNS_T_NS     2
#define DYN_DNS_T_CNAME  5
#define DYN_DNS_T_SOA    6
#define DYN_DNS_T_PTR   12
#define DYN_DNS_T_MX    15
#define DYN_DNS_T_TXT   16
#define DYN_DNS_T_AAAA  28
#define DYN_DNS_C_IN     1

typedef struct {
    uint16_t id, flags;
    uint16_t qdcount, ancount, nscount, arcount;
} dyn_dns_hdr_t;

typedef struct {
    char name[DYN_DNS_MAX_NAME + 1];  /* dotted, NUL-terminated, no trailing dot */
    uint16_t type, cls;
    uint32_t ttl;
    const uint8_t *rdata;             /* borrowed: points into the message */
    uint16_t rdlen;
} dyn_dns_rr_t;

/* Errors are negative; 0 or a positive length is success. */
#define DYN_DNS_OK          0
#define DYN_DNS_E_SHORT    -1   /* ran off the end of the message */
#define DYN_DNS_E_LOOP     -2   /* pointer did not point strictly backward */
#define DYN_DNS_E_NAME     -3   /* label or name over its RFC cap */
#define DYN_DNS_E_FORMAT   -4   /* reserved label bits, or a malformed field */

/* Decode the name at `off` into `out` (dotted, no trailing dot). Returns the
 * offset of the first byte AFTER the name in the message -- which, when the
 * name ended in a pointer, is NOT where the name's content came from. Negative
 * on error. */
int dyn_dns_name_decode(const uint8_t *msg, size_t len, size_t off,
                        char *out, size_t outcap);

/* Encode a dotted name into `out`. Returns bytes written, or negative. Emits no
 * compression pointers: a query we generate has nothing to point at. */
int dyn_dns_name_encode(const char *name, uint8_t *out, size_t outcap);

int dyn_dns_hdr_decode(const uint8_t *msg, size_t len, dyn_dns_hdr_t *h);
int dyn_dns_hdr_encode(const dyn_dns_hdr_t *h, uint8_t *out, size_t outcap);

/* Build a standard recursive query. Returns the message length or negative. */
int dyn_dns_build_query(uint16_t id, const char *name, uint16_t type,
                        uint8_t *out, size_t outcap);

/* Walk the question section, leaving `*off` after it. */
int dyn_dns_skip_questions(const uint8_t *msg, size_t len, size_t *off,
                           uint16_t qdcount);

/* Decode one resource record at `*off`, advancing it. `rr->rdata` borrows the
 * message and is valid only while `msg` is. */
int dyn_dns_rr_decode(const uint8_t *msg, size_t len, size_t *off,
                      dyn_dns_rr_t *rr);

/* Start a response to `query`: copies the header with QR set, echoes the
 * question, and returns the write offset. `rcode` 0 for a normal answer. */
int dyn_dns_begin_response(const uint8_t *query, size_t qlen, int rcode,
                           uint8_t *out, size_t outcap);

/* Append one A/AAAA answer whose name is a pointer to the question at offset
 * 12. `addr` is 4 or 16 octets. Returns the new offset, or negative. */
int dyn_dns_add_answer(uint8_t *out, size_t outcap, size_t off, uint16_t type,
                       uint32_t ttl, const uint8_t *addr, uint16_t addrlen);

/* Set ANCOUNT on a message being built. */
void dyn_dns_set_ancount(uint8_t *out, uint16_t n);

const char *dyn_dns_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* DYN_DNS_H */
