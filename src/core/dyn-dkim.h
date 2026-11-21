#ifndef DYN_DKIM_H
#define DYN_DKIM_H

#include <stddef.h>
#include <stdint.h>
#include "dyn-hash.h"
#include "dyn-sb.h"

enum { DKIM_OK = 0, DKIM_EBAD = -1, DKIM_ELONG = -2, DKIM_ENOMEM = -3 };

#define DKIM_RSA 1
#define DKIM_ED25519 2
#define DKIM_HDR_MAX 1000u
#define DKIM_H_MAX 128u
#define DKIM_NO_LIMIT UINT64_MAX

typedef struct {
    size_t off, len, nlen;
} dkim_field_t;

typedef struct {
    const uint8_t* p;
    size_t n, body, nf, cap;
    dkim_field_t* f;
} dkim_msg_t;

typedef struct {
    int relaxed, cr, wsp, text;
    uint64_t blank, limit, n;
    size_t on;
    dyn_hash_ctx_t h;
    uint8_t ob[512];
} dkim_body_t;

typedef struct {
    int algo, hrel, brel;
    int64_t t, x;
    uint64_t l;
    size_t nh;
    dyn_sb_t d, s, i, b, h;
    uint8_t bh[32];
} dkim_sig_t;

typedef struct {
    int k, testing, strict, revoked;
    dyn_sb_t p;
} dkim_key_t;

typedef struct {
    int algo, hrel, brel, use_l, oversign;
    const char *d, *s, *i;
    const char* const* heads;
    size_t nheads;
    int64_t t, x;
} dkim_sign_opts_t;

int dkim_msg_parse(dkim_msg_t* m, const uint8_t* p, size_t n);
void dkim_msg_free(dkim_msg_t* m);
int dkim_name_is(const dkim_msg_t* m, size_t fi, const char* name, size_t nn);

void dkim_body_init(dkim_body_t* b, int relaxed, uint64_t limit);
void dkim_body_feed(dkim_body_t* b, const uint8_t* p, size_t n);
void dkim_body_final(dkim_body_t* b, uint8_t out[32]);

int dkim_hdr_canon(dyn_sb_t* out, const uint8_t* p, size_t n, size_t nlen, int relaxed);
int dkim_hdr_hash_raw(const dkim_msg_t* m, const dkim_sig_t* s, const uint8_t* f, size_t fn, size_t nlen, uint8_t out[32]);
int dkim_hdr_hash(const dkim_msg_t* m, const dkim_sig_t* s, size_t sigfield, uint8_t out[32]);

void dkim_sig_init(dkim_sig_t* s);
void dkim_sig_free(dkim_sig_t* s);
const char* dkim_sig_parse(dkim_sig_t* s, const uint8_t* v, size_t n);

void dkim_key_init(dkim_key_t* k);
void dkim_key_free(dkim_key_t* k);
const char* dkim_key_parse(dkim_key_t* k, const uint8_t* txt, size_t n);

const char* dkim_sign_field(dyn_sb_t* out, const dkim_msg_t* m, const dkim_sign_opts_t* o, uint8_t hh[32]);
int dkim_fold_b64(dyn_sb_t* out, const uint8_t* p, size_t n);

int dkim_b64_decode(const uint8_t* s, size_t n, dyn_sb_t* out);
int dkim_b64_encode(dyn_sb_t* out, const uint8_t* p, size_t n);
int dkim_tag_span(const uint8_t* v, size_t n, char tag, size_t* a, size_t* z);

#endif
