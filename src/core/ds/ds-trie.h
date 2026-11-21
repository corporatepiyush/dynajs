#ifndef DS_TRIE_H
#define DS_TRIE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_trie_node {
    struct dyn_trie_node* child;
    struct dyn_trie_node* sibling;
    uint8_t* tail;
    uint32_t tail_len;
    uint8_t byte;
    uint8_t is_end;
} dyn_trie_node_t;

typedef struct dyn_trie_chunk {
    struct dyn_trie_chunk* next;
    uint32_t used, cap;
} dyn_trie_chunk_t;

typedef struct dyn_trie_bytes {
    struct dyn_trie_bytes* next;
    uint32_t used, cap;
} dyn_trie_bytes_t;

#define DYN_TRIE_BYTES(b) ((uint8_t*)((b) + 1))
#define DYN_TRIE_BYTES_MIN 512
#define DYN_TRIE_CHUNK_NODES(c) ((dyn_trie_node_t*)((c) + 1))
#define DYN_TRIE_CHUNK_MAX 8192

typedef struct {
    dyn_trie_node_t* root;
    dyn_trie_chunk_t* chunks;
    dyn_trie_bytes_t* bytes;
    uint32_t count;
} dyn_trie_t;

int dyn_trie_init(dyn_trie_t* t);

void dyn_trie_free(dyn_trie_t* t);

dyn_trie_node_t* dyn_trie_child(dyn_trie_node_t* node, uint8_t b);

dyn_trie_node_t* dyn_trie_walk(dyn_trie_t* t, const char* key, size_t klen, int* on_tail);

int dyn_trie_insert_at(dyn_trie_t* t, dyn_trie_node_t* node, const char* key, size_t klen, size_t from, dyn_trie_node_t** path, size_t* plen);

int dyn_trie_insert(dyn_trie_t* t, const char* key, size_t klen);

int dyn_trie_has(const dyn_trie_t* t, const char* key, size_t klen);

int dyn_trie_delete(dyn_trie_t* t, const char* key, size_t klen);

int dyn_trie_has_prefix(const dyn_trie_t* t, const char* p, size_t plen);

size_t dyn_trie_longest_prefix(const dyn_trie_t* t, const char* s, size_t slen);

void dyn_trie_order_children(dyn_trie_node_t** v, size_t k);

#ifdef __cplusplus
}
#endif

#endif
