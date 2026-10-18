#ifndef DYN_CODEC_H
#define DYN_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_CODEC_BAD ((size_t)-1)

typedef enum {
    DYN_BASE32_STD = 0,
    DYN_BASE32_HEX = 1
} dyn_base32_alphabet;

static inline size_t dyn_codec_hex_encode_len(size_t n) { return n * 2; }
static inline size_t dyn_codec_hex_decode_cap(size_t n) { return n / 2; }
static inline size_t dyn_codec_base64_encode_cap(size_t n) { return 4 * ((n + 2) / 3); }
static inline size_t dyn_codec_base64_decode_cap(size_t n) { return 3 * (n / 4); }
static inline size_t dyn_codec_base32_encode_cap(size_t n) { return ((n + 4) / 5) * 8; }
static inline size_t dyn_codec_base32_decode_cap(size_t n) { return ((n + 7) / 8) * 5; }
static inline size_t dyn_codec_base85_encode_cap(size_t n) { return ((n + 3) / 4) * 5; }
static inline size_t dyn_codec_base85_decode_cap(size_t n) { return ((n + 4) / 5) * 4; }
#define DYN_CODEC_VARINT_MAX 10

void dyn_codec_hex_encode(const uint8_t* data, size_t n, char* out);

size_t dyn_codec_hex_decode(const char* s, size_t n, uint8_t* out);

size_t dyn_codec_base64_encode(const uint8_t* data, size_t n, char* out);

size_t dyn_codec_base64_decode(const char* s, size_t n, uint8_t* out);

size_t dyn_codec_base64url_encode(const uint8_t* data, size_t n, char* out);

size_t dyn_codec_base64url_decode(const char* s, size_t n, uint8_t* out,
    char* scratch);

size_t dyn_codec_base32_encode(const uint8_t* restrict data, size_t n,
    char* restrict out, dyn_base32_alphabet alpha);
size_t dyn_codec_base32_decode(const char* restrict s, size_t n,
    uint8_t* restrict out, dyn_base32_alphabet alpha);

size_t dyn_codec_base85_encode(const uint8_t* data, size_t n, char* out);
size_t dyn_codec_base85_decode(const char* s, size_t n, uint8_t* out);

size_t dyn_codec_put_uvarint(uint64_t x, uint8_t* out);
size_t dyn_codec_put_varint(int64_t x, uint8_t* out);

int dyn_codec_uvarint(const uint8_t* buf, size_t n, uint64_t* out);
int dyn_codec_varint(const uint8_t* buf, size_t n, int64_t* out);

#ifdef __cplusplus
}
#endif

#endif
