#include "src/engine-internal.h"
#include "src/engine-unity-decls.h"

static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT];
static JSDateFieldsCache* js_date_cache_alloc(JSContext* ctx)
{
    JSRuntime* rt = ctx->rt;
    JSDateFieldsCache* fc = rt->date_cache_free_list;
    if (likely(fc != NULL)) {
        rt->date_cache_free_list = fc->next_free;
        fc->e[0].valid = 0;
        fc->e[1].valid = 0;
        return fc;
    }
    return js_mallocz(ctx, sizeof(*fc));
}

static void js_date_cache_free(JSRuntime* rt, JSDateFieldsCache* fc)
{
    fc->next_free = rt->date_cache_free_list;
    rt->date_cache_free_list = fc;
}
static const char js_atom_init[] =
#define DEF(name, str) str "\0"
#include "dyna-atom.h"
#undef DEF
    ;
static const JSClassExoticMethods js_arguments_exotic_methods;
static const JSClassExoticMethods js_string_exotic_methods;
static const JSClassExoticMethods js_proxy_exotic_methods;
static JSClassID js_class_id_alloc = JS_CLASS_INIT_COUNT;

static int js_no_prototype_extensions;

void JS_SetNoPrototypeExtensions(int disable)
{
    js_no_prototype_extensions = !!disable;
}

int JS_NoPrototypeExtensions(void)
{
    return js_no_prototype_extensions;
}

#include "src/mm/js_malloc.inc.c"
#include "src/value/atoms.inc.c"
#include "src/runtime/class.inc.c"
#include "src/object/shapes_objects_gc.inc.c"
#include "src/object/property_get.inc.c"
#include "src/object/property_set_convert.inc.c"
#include "src/vm/interpreter.inc.c"
#include "src/builtins/iterator.inc.c"
#include "src/builtins/array.inc.c"
#include "src/builtins/number.inc.c"
#include "src/builtins/string_width.inc.c"
#include "src/builtins/string.inc.c"
#include "src/builtins/math.inc.c"
#include "src/builtins/date_timezone.inc.c"
#include "src/builtins/regexp.inc.c"
#include "src/builtins/json.inc.c"
#include "src/builtins/reflect.inc.c"
#include "src/builtins/proxy.inc.c"
#include "src/builtins/symbol.inc.c"
#include "src/builtins/promise_async.inc.c"
#include "src/builtins/date.inc.c"
#include "src/builtins/bigint_number.inc.c"
#include "src/builtins/typedarray_atomics.inc.c"
#include "src/builtins/weakref_init.inc.c"
#include "src/builtins/disposable.inc.c"
#include "src/runtime/using.inc.c"
