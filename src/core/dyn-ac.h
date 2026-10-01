#ifndef DYN_AC_H
#define DYN_AC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_AC_ALPHABET 256

#define DYN_AC_MAX_STATES (1 << 18)

_Static_assert(sizeof(int32_t) * DYN_AC_ALPHABET == 1024, "goto row is 1 KiB");

#ifndef DYN_AC_DENSE_MAX
#define DYN_AC_DENSE_MAX 2048
#endif

typedef struct {
    uint32_t next;
    uint32_t check;
} dyn_ac_slot;
typedef struct {
    int32_t out;
    int32_t out_link;
} dyn_ac_oo;

typedef struct {
    int32_t* go;

    dyn_ac_slot* tab;
    uint32_t* base;
    dyn_ac_oo* oo;

    int32_t* fail;
    int32_t* out;
    int32_t* out_link;
    uint32_t compact;
    size_t n_states, cap_states;
    size_t tab_cap;
    size_t* plen;
    size_t n_pat;
    int ascii;

    int32_t* child_head;
    uint8_t* e_byte;
    int32_t* e_target;
    int32_t* e_next;
    size_t n_edges, cap_edges;
} dyn_ac_t;

dyn_ac_t* dyn_ac_new(size_t n_pat);

void dyn_ac_free(dyn_ac_t* a);

int dyn_ac_insert(dyn_ac_t* a, const uint8_t* p, size_t len, int idx);

int dyn_ac_build(dyn_ac_t* a);

typedef int (*dyn_ac_emit_t)(void* ud, int pat, size_t end_byte);

int dyn_ac_run(const dyn_ac_t* a, const uint8_t* t, size_t tlen,
    dyn_ac_emit_t emit, void* ud);

#ifdef __cplusplus
}
#endif

#endif
