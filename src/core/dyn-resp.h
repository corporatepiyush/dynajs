/*
 * dyn-resp -- the Redis serialization protocol (RESP2 + RESP3) codec. Pure C:
 * no sockets, no JS, so the parser that reads a peer's bytes can be fuzzed and
 * unit-tested on its own.
 *
 * THE PARSER IS AN UNTRUSTED-INPUT SURFACE. "The server" is whatever answered
 * on that port, and every length in a reply is a number it chose:
 *
 *   - a BULK length is an instruction to buffer until that many bytes arrive,
 *     so it needs an explicit cap: that is what `maxbulk` is;
 *   - an aggregate's element count is then bounded by that same cap divided by
 *     three, the smallest complete value ("_\r\n"). Bounding it against the
 *     bytes ALREADY here would be wrong -- a scanner sees partial replies, and
 *     "*2\r\n" with nothing after it is incomplete, not a lie;
 *   - nesting is bounded so the JS-side decoder, which does recurse, cannot be
 *     driven off its stack.
 *
 * Scanning is ITERATIVE and works in two phases. `dyn_resp_scan` establishes
 * that ONE complete, well-formed reply is present and enforces every limit;
 * only then does the reader walk it. Splitting them is what lets the decoder
 * recurse safely and lets a partial TCP read cost nothing but a rescan.
 */
#ifndef DYN_RESP_H
#define DYN_RESP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RESP2 type bytes. */
#define DYN_RESP_SIMPLE   '+'
#define DYN_RESP_ERROR    '-'
#define DYN_RESP_INT      ':'
#define DYN_RESP_BULK     '$'
#define DYN_RESP_ARRAY    '*'
/* RESP3 additions. */
#define DYN_RESP_NULL     '_'
#define DYN_RESP_DOUBLE   ','
#define DYN_RESP_BOOL     '#'
#define DYN_RESP_BLOBERR  '!'
#define DYN_RESP_VERB     '='
#define DYN_RESP_BIGNUM   '('
#define DYN_RESP_MAP      '%'
#define DYN_RESP_SET      '~'
#define DYN_RESP_ATTR     '|'
#define DYN_RESP_PUSH     '>'

#define DYN_RESP_MAX_DEPTH  32
/* A header line is a type byte and a number; nothing legitimate is long. This
 * bounds how far we hunt for a CRLF, so a peer cannot make us buffer without
 * limit by simply never sending one. */
#define DYN_RESP_MAX_LINE   65536

#define DYN_RESP_OK           0
#define DYN_RESP_INCOMPLETE  -1   /* well-formed so far; more bytes needed */
#define DYN_RESP_E_TYPE      -2   /* unknown type byte */
#define DYN_RESP_E_SYNTAX    -3   /* malformed number, missing or bare CRLF */
#define DYN_RESP_E_DEPTH     -4   /* nesting past DYN_RESP_MAX_DEPTH */
#define DYN_RESP_E_TOOBIG    -5   /* a length past its cap, or an overflow */
#define DYN_RESP_E_COUNT     -6   /* an element count the message cannot hold */

/* Is one complete reply present at the head of `buf`? On DYN_RESP_OK, sets
 * `*consumed` to its length in octets. `maxbulk` caps any single bulk/verbatim
 * payload; 0 means the built-in default. */
int dyn_resp_scan(const uint8_t *buf, size_t len, size_t maxbulk,
                  size_t *consumed);

typedef struct {
    const uint8_t *buf;
    size_t len, pos;
} dyn_resp_reader_t;

typedef struct {
    const uint8_t *str;      /* borrowed: points into the reader's buffer */
    size_t slen;
    int64_t ival;            /* INT, and 0/1 for BOOL */
    double dval;             /* DOUBLE */
    int64_t count;           /* aggregates: elements that follow (map: pairs) */
    int type;                /* one of the DYN_RESP_* type bytes */
    int isnull;              /* RESP2 `$-1`/`*-1`, or RESP3 `_` */
} dyn_resp_item_t;

_Static_assert(sizeof(dyn_resp_item_t) == 5 * sizeof(void *) + 8,
               "dyn_resp_item_t regained padding: keep the two ints adjacent");

void dyn_resp_reader_init(dyn_resp_reader_t *r, const uint8_t *buf, size_t len);

/* Read the next item in prefix order. Aggregates report `count` and their
 * elements follow as subsequent items. Only ever called on bytes `dyn_resp_scan`
 * has already accepted. */
int dyn_resp_next(dyn_resp_reader_t *r, dyn_resp_item_t *it);

/* Append one command to `out` in the array-of-bulk-strings form -- the ONLY
 * form we ever send. Arguments are length-prefixed, so a CRLF inside one is
 * data and cannot start a second command; the inline form has no such
 * property and is why command injection exists in clients that use it.
 * `lens[i] == (size_t)-1` means strlen(argv[i]). Returns bytes appended or
 * negative; `*need` is set to the required capacity on DYN_RESP_E_TOOBIG. */
int dyn_resp_cmd_encode(uint8_t *out, size_t outcap, int argc,
                        const char *const *argv, const size_t *lens,
                        size_t *need);

/* Bytes `dyn_resp_cmd_encode` would write, so a caller can size once. */
size_t dyn_resp_cmd_size(int argc, const char *const *argv, const size_t *lens);

const char *dyn_resp_strerror(int code);

/* A peer that answers a plaintext connection with a TLS record starts with a
 * handshake (0x16) or an alert (0x15) and a plausible version. Naming that is
 * the difference between "protocol error" and an actionable message. */
int dyn_resp_looks_like_tls(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DYN_RESP_H */
