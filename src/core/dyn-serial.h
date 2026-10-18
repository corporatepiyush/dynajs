#ifndef DYN_SERIAL_H
#define DYN_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_SER_MAGIC0 'D'
#define DYN_SER_MAGIC1 'Y'
#define DYN_SER_MAGIC2 'N'
#define DYN_SER_MAGIC3 'S'
#define DYN_SER_VERSION 1u
#define DYN_SER_HEADER 20u
#define DYN_SER_TRAILER 4u

#define DYN_SER_DEFAULT_MAX (1u << 28)

typedef struct {
    uint8_t* buf;
    size_t len, cap;
    int err;
} dyn_ser_t;

void dyn_ser_init(dyn_ser_t* w);
void dyn_ser_free(dyn_ser_t* w);
uint8_t* dyn_ser_take(dyn_ser_t* w, size_t* len);

int dyn_ser_raw(dyn_ser_t* w, const void* p, size_t n);
int dyn_ser_u8(dyn_ser_t* w, uint8_t v);
int dyn_ser_u16(dyn_ser_t* w, uint16_t v);
int dyn_ser_u32(dyn_ser_t* w, uint32_t v);
int dyn_ser_u64(dyn_ser_t* w, uint64_t v);
int dyn_ser_i64(dyn_ser_t* w, int64_t v);
int dyn_ser_f64(dyn_ser_t* w, double v);
int dyn_ser_blob(dyn_ser_t* w, const void* p, size_t n);
int dyn_ser_uvarint(dyn_ser_t* w, uint64_t v);
int dyn_ser_svarint(dyn_ser_t* w, int64_t v);
size_t dyn_varint_len(uint64_t v);

int dyn_ser_begin(dyn_ser_t* w, uint16_t type_id, uint32_t flags);
int dyn_ser_finish(dyn_ser_t* w);

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t pos;
    int err;
    uint64_t budget;
} dyn_de_t;

#define DYN_DE_OK 0
#define DYN_DE_BAD_MAGIC -1
#define DYN_DE_BAD_VERSION -2
#define DYN_DE_TRUNCATED -3
#define DYN_DE_BAD_CRC -4
#define DYN_DE_TOO_LARGE -5

#define DYN_DE_BUDGET_MULT 64u
#define DYN_DE_BUDGET_BASE (512u << 20)

int dyn_de_open(dyn_de_t* r, const uint8_t* buf, size_t len,
    uint16_t* type_id, uint32_t* flags, size_t max_payload);
const char* dyn_de_strerror(int code);

uint8_t dyn_de_u8(dyn_de_t* r);
uint16_t dyn_de_u16(dyn_de_t* r);
uint32_t dyn_de_u32(dyn_de_t* r);
uint64_t dyn_de_u64(dyn_de_t* r);
int64_t dyn_de_i64(dyn_de_t* r);
double dyn_de_f64(dyn_de_t* r);
const uint8_t* dyn_de_raw(dyn_de_t* r, size_t n);
int dyn_de_uvarint(dyn_de_t* r, uint64_t* out);
int dyn_de_svarint(dyn_de_t* r, int64_t* out);
const char* dyn_de_blob(dyn_de_t* r, size_t* n);

int dyn_de_count(dyn_de_t* r, uint32_t* out, size_t elem_size);

static inline int dyn_de_charge(dyn_de_t* r, uint64_t bytes)
{
    if (bytes > r->budget) {
        r->err = 1;
        return -1;
    }
    r->budget -= bytes;
    return 0;
}

static inline int dyn_de_ok(const dyn_de_t* r) { return !r->err; }
static inline size_t dyn_de_left(const dyn_de_t* r)
{
    return r->err ? 0 : r->len - r->pos;
}

#ifdef __cplusplus
}
#endif

#endif
