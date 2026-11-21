#ifndef DYN_VALIDK_VALIDK_H
#define DYN_VALIDK_VALIDK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int dyn_v_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int dyn_v_digit(unsigned char c) { return c >= '0' && c <= '9'; }

int dyn_v_email(const char* s, size_t n);

int dyn_v_iban(const char* s, size_t n);

int dyn_v_ipv4(const char* s, size_t n);

int dyn_v_ipv6(const char* s, size_t n);

int dyn_v_datestring(const char* s, size_t n);

int dyn_v_rfc3339(const char* s, size_t n);

int dyn_v_mime(const char* s, size_t n);

int dyn_v_luhn(const char* s, size_t n);

int dyn_v_port(const char* s, size_t n);

int dyn_v_base64(const char* s, size_t n);

int dyn_v_base64url(const char* s, size_t n);

int dyn_v_hex(const char* s, size_t n);

int dyn_v_hexcolor(const char* s, size_t n);

int dyn_v_numeric(const char* s, size_t n);

int dyn_v_slug(const char* s, size_t n);

int dyn_v_e164(const char* s, size_t n);

int dyn_v_domain_grammar(const char* s, size_t n);

#endif
