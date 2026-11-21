#ifndef DYN_ARCK_ARCK_H
#define DYN_ARCK_ARCK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-common.h"
#include "../dyn-hash.h"
#include "../dyn-compress.h"

#define AR_MAX_ENTRIES 100000u

#define ZIP_MAX_ENTRIES 65535u

#define AR_MAX_NAME 4096u

#define AR_MAX_MEMBER (1u << 30)

#define AR_MAX_TOTAL_OUT (1u << 30)

#define TAR_BLOCK 512

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} ab_t;

typedef struct {
    char name[AR_MAX_NAME + 1];
    size_t nlen;
    uint64_t size, mtime, mode;
    char type;
    char link[AR_MAX_NAME + 1];
    size_t linklen;
    size_t body;
} tar_ent_t;

typedef struct {
    void* ud;
    const uint8_t* p;
    size_t n, i;
    int allow_unsafe;
    char err[160];
} tar_rd_t;

typedef struct {
    char name[AR_MAX_NAME + 1];
    size_t nlen;
    uint32_t crc, csize, usize, method, mtime;
    uint32_t mode;
    char comment[1025];
    size_t clen;
    size_t local;
} zip_ent_t;

typedef struct {
    void* ud;
    const uint8_t* p;
    size_t n;
    size_t cd_off, cd_size;
    uint32_t count;
    int allow_unsafe;
    char err[160];
} zip_rd_t;

typedef struct {
    char* name;
    size_t nlen, csize, usize, local;
    uint32_t crc, method;
    uint32_t dos_mtime;
    uint32_t mode;
    char* comment;
    size_t clen;
} zip_out_t;

void arck_ab_free(ab_t* b);

void arck_ab_init(ab_t* b);

void arck_ab_u16(ab_t* b, uint32_t v);

void arck_ab_u32(ab_t* b, uint32_t v);

void arck_ab_write(ab_t* b, const void* p, size_t n);

void arck_ab_zero(ab_t* b, size_t n);

int arck_ar_name_safe(const char* s, size_t n);

uint32_t arck_tar_checksum(const uint8_t* h);

int arck_tar_next(tar_rd_t* r, tar_ent_t* e);

void arck_tar_put_octal(uint8_t* dst, size_t n, uint64_t v);

int arck_zip_deflate_raw(const uint8_t* src, size_t n, uint8_t** out,
    size_t* out_len);

uint32_t arck_zip_dos_from_unix(int64_t t);

int arck_zip_entry_at(zip_rd_t* z, size_t* off, zip_ent_t* e);

int arck_zip_member(zip_rd_t* z, const zip_ent_t* e, dyn_outbuf_t* out);

int arck_zip_open(zip_rd_t* z);

int arck_zip_preflight(zip_rd_t* z);

int64_t arck_zip_unix_from_dos(uint32_t dos);

#endif
