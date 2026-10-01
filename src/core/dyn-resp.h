#ifndef DYN_RESP_H
#define DYN_RESP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_RESP_SIMPLE '+'
#define DYN_RESP_ERROR '-'
#define DYN_RESP_INT ':'
#define DYN_RESP_BULK '$'
#define DYN_RESP_ARRAY '*'
#define DYN_RESP_NULL '_'
#define DYN_RESP_DOUBLE ','
#define DYN_RESP_BOOL '#'
#define DYN_RESP_BLOBERR '!'
#define DYN_RESP_VERB '='
#define DYN_RESP_BIGNUM '('
#define DYN_RESP_MAP '%'
#define DYN_RESP_SET '~'
#define DYN_RESP_ATTR '|'
#define DYN_RESP_PUSH '>'

#define DYN_RESP_MAX_DEPTH 32
#define DYN_RESP_MAX_LINE 65536

#define DYN_RESP_OK 0
#define DYN_RESP_INCOMPLETE -1
#define DYN_RESP_E_TYPE -2
#define DYN_RESP_E_SYNTAX -3
#define DYN_RESP_E_DEPTH -4
#define DYN_RESP_E_TOOBIG -5
#define DYN_RESP_E_COUNT -6
#define DYN_RESP_E_INT -7

int dyn_resp_scan(const uint8_t* buf, size_t len, size_t maxbulk,
    size_t* consumed);

typedef struct {
    const uint8_t* buf;
    size_t len, pos;
} dyn_resp_reader_t;

typedef struct {
    const uint8_t* str;
    size_t slen;
    int64_t ival;
    double dval;
    int64_t count;
    int type;
    int isnull;
} dyn_resp_item_t;

_Static_assert(sizeof(dyn_resp_item_t) == 5 * sizeof(void*) + 8,
    "dyn_resp_item_t regained padding: keep the two ints adjacent");

void dyn_resp_reader_init(dyn_resp_reader_t* r, const uint8_t* buf, size_t len);

int dyn_resp_next(dyn_resp_reader_t* r, dyn_resp_item_t* it);

int dyn_resp_cmd_encode(uint8_t* out, size_t outcap, int argc,
    const char* const* argv, const size_t* lens,
    size_t* need);

size_t dyn_resp_cmd_size(int argc, const char* const* argv, const size_t* lens);

const char* dyn_resp_strerror(int code);

int dyn_resp_looks_like_tls(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
