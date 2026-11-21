#ifndef DKIM_INTERNAL_H
#define DKIM_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DKIM) && defined(CONFIG_OPENSSL)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-dkim.h"

#define DKIM_SIGS_MAX 64
#define DKIM_TXT_MAX 65536

enum { VS_PENDING = 0, VS_PASS, VS_FAIL, VS_NEUTRAL, VS_TEMPERROR, VS_PERMERROR };

typedef struct {
    uint8_t* msg;
    size_t n;
    char* pem;
    size_t pemn;
    uint8_t seed[32];
    int has_seed, algo_req;
    char *d, *s, *i;
    char** heads;
    size_t nheads;
    dkim_sign_opts_t o;
    dyn_sb_t out;
    const char* err;
    char ebuf[128];
} dkim_sign_job_t;

typedef struct {
    dkim_sig_t s;
    int parsed, status, testing, keyst;
    const char* reason;
    char rbuf[128];
    uint8_t hh[32];
    dyn_sb_t txt;
} dkim_vsig_t;

typedef struct {
    uint8_t* msg;
    size_t n;
    dkim_msg_t* m;
    size_t ns, max;
    int64_t now;
    int min_bits, bad_msg;
    JSValue lookup, lthis;
    dkim_vsig_t* sig;
} dkim_verify_job_t;

int dkim_sign_work(dkim_sign_job_t* j);
int dkim_verify_stage1(dkim_verify_job_t* j);
int dkim_verify_stage2(dkim_verify_job_t* j);

#endif
#endif
