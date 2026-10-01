#include "libregexp.h"
#include "dyna-libc.h"

static int nbinterrupts = 0;

int lre_check_stack_overflow(void* opaque, size_t alloca_size) { return 0; }

void* lre_realloc(void* opaque, void* ptr, size_t size)
{
    return realloc(ptr, size);
}

int lre_check_timeout(void* opaque)
{
    nbinterrupts++;
    return (nbinterrupts > 100);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    int len, ret, i;
    uint8_t* bc;
    char error_msg[64];
    const uint8_t* input;
    uint8_t* capture[255 * 2];
    size_t size1 = size;

    nbinterrupts = 0;

    for (i = 0; i < size; i++) {
        if (data[i] == 0) {
            size1 = i;
            break;
        }
    }
    if (size1 == size) {
        return 0;
    }
    bc = lre_compile(&len, error_msg, sizeof(error_msg), (const char*)data,
        size1, 0, NULL);
    if (!bc) {
        return 0;
    }
    input = data + size1 + 1;
    ret = lre_exec(capture, bc, input, 0, size - (size1 + 1), 0, NULL);
    if (ret == 1) {
        lre_get_capture_count(bc);
    }
    free(bc);

    return 0;
}
