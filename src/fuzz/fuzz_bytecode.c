#include "dynajs.h"
#include "list.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int ref_count;
    struct list_head link;
    uint64_t buf[0];
} FuzzSABHeader;

static struct list_head fz_sab_registry = LIST_HEAD_INIT(fz_sab_registry);

static void* fz_sab_alloc(void* opaque, size_t size)
{
    FuzzSABHeader* sab = malloc(sizeof(*sab) + size);
    if (!sab)
        return NULL;
    sab->ref_count = 1;
    list_add_tail(&sab->link, &fz_sab_registry);
    return sab->buf;
}

static void fz_sab_free(void* opaque, void* ptr)
{
    FuzzSABHeader* sab;
    int ref_count;

    sab = (FuzzSABHeader*)((uint8_t*)ptr - sizeof(*sab));
    ref_count = --sab->ref_count;
    if (ref_count == 0) {
        list_del(&sab->link);
        free(sab);
    }
}

static void fz_sab_dup(void* opaque, void* ptr)
{
    FuzzSABHeader* sab;
    sab = (FuzzSABHeader*)((uint8_t*)ptr - sizeof(*sab));
    sab->ref_count++;
}

static int fz_sab_valid_ptr(void* opaque, void* ptr)
{
    struct list_head* el;

    list_for_each(el, &fz_sab_registry)
    {
        if (((FuzzSABHeader*)el)->buf == ptr)
            return 1;
    }
    return 0;
}

static void read_and_drop(JSContext* ctx, const uint8_t* buf, size_t len, int flags)
{
    JSValue v = JS_ReadObject(ctx, buf, len, flags);
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt = JS_NewRuntime();
    if (!rt)
        return 0;
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);
    {
        JSSharedArrayBufferFunctions sf;
        memset(&sf, 0, sizeof(sf));
        sf.sab_alloc = fz_sab_alloc;
        sf.sab_free = fz_sab_free;
        sf.sab_dup = fz_sab_dup;
        sf.sab_valid_ptr = fz_sab_valid_ptr;
        JS_SetSharedArrayBufferFunctions(rt, &sf);
    }

    read_and_drop(ctx, data, size, JS_READ_OBJ_BYTECODE);
    read_and_drop(ctx, data, size, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE);
    read_and_drop(ctx, data, size, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA);
    read_and_drop(ctx, data, size, JS_READ_OBJ_SAB);
    read_and_drop(ctx, data, size, 0);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
