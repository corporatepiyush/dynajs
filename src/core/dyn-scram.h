#ifndef DYN_SCRAM_H
#define DYN_SCRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_SCRAM_KEY_LEN 32
#define DYN_SCRAM_RAW_NONCE 18
#define DYN_SCRAM_NONCE_B64 24
#define DYN_SCRAM_MAX_ITERS 1000000
#define DYN_SCRAM_MAX_SALT 1024
#define DYN_SCRAM_MAX_MSG 8192

#define DYN_SCRAM_OK 0
#define DYN_SCRAM_E_SYNTAX -1
#define DYN_SCRAM_E_NONCE -2
#define DYN_SCRAM_E_ITERS -3
#define DYN_SCRAM_E_SALT -4
#define DYN_SCRAM_E_VERIFY -5
#define DYN_SCRAM_E_SERVER -6
#define DYN_SCRAM_E_SHORT -7
#define DYN_SCRAM_E_STATE -8
#define DYN_SCRAM_E_EXT -9
#define DYN_SCRAM_E_ENTROPY -10

typedef struct {
    int step;
    char nonce[DYN_SCRAM_NONCE_B64 + 1];
    char* cfirst_bare;
    char* sfirst;
    uint8_t server_sig[DYN_SCRAM_KEY_LEN];
    char err[128];
} dyn_scram_t;

int dyn_scram_client_first(dyn_scram_t* s, char* out, size_t outcap);

int dyn_scram_server_first(dyn_scram_t* s, const char* msg, size_t len,
    const char* password, char* out, size_t outcap);

int dyn_scram_server_final(dyn_scram_t* s, const char* msg, size_t len);

void dyn_scram_free(dyn_scram_t* s);

const char* dyn_scram_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
