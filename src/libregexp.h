#ifndef LIBREGEXP_H
#define LIBREGEXP_H

#include <stddef.h>
#include <stdint.h>

#define LRE_FLAG_GLOBAL (1 << 0)
#define LRE_FLAG_IGNORECASE (1 << 1)
#define LRE_FLAG_MULTILINE (1 << 2)
#define LRE_FLAG_DOTALL (1 << 3)
#define LRE_FLAG_UNICODE (1 << 4)
#define LRE_FLAG_STICKY (1 << 5)
#define LRE_FLAG_INDICES (1 << 6)
#define LRE_FLAG_NAMED_GROUPS (1 << 7)
#define LRE_FLAG_UNICODE_SETS (1 << 8)

#define LRE_RET_MEMORY_ERROR (-1)
#define LRE_RET_TIMEOUT (-2)

#ifndef LRE_DEFAULT_EXEC_STEPS
#define LRE_DEFAULT_EXEC_STEPS 100000000ull
#endif

#define LRE_GROUP_NAME_TRAILER_LEN 2

uint8_t* lre_compile(int* plen, char* error_msg, int error_msg_size,
    const char* buf, size_t buf_len, int re_flags,
    void* opaque);
int lre_get_alloc_count(const uint8_t* bc_buf);
int lre_get_capture_count(const uint8_t* bc_buf);
int lre_get_flags(const uint8_t* bc_buf);
const char* lre_get_groupnames(const uint8_t* bc_buf);
int lre_exec(uint8_t** capture,
    const uint8_t* bc_buf, const uint8_t* cbuf, int cindex, int clen,
    int cbuf_type, void* opaque);

void lre_set_exec_step_limit(uint64_t steps);

int lre_parse_escape(const uint8_t** pp, int allow_utf16);

int lre_check_stack_overflow(void* opaque, size_t alloca_size);
int lre_check_timeout(void* opaque);
void* lre_realloc(void* opaque, void* ptr, size_t size);

#endif
