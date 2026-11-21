#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static int ai_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (ai_error_register(ctx, m) < 0)
        return -1;
    if (ai_cancel_register(ctx) < 0)
        return -1;
    if (ai_anthropic_register(ctx, m) < 0)
        return -1;
    if (ai_openai_register(ctx, m) < 0)
        return -1;
    if (ai_stream_register(ctx, m) < 0)
        return -1;
    return ai_results_register(ctx, m);
}

int js_nat_init_ai(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:ai", ai_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Anthropic");
    JS_AddModuleExport(ctx, m, "APIError");
    JS_AddModuleExport(ctx, m, "MessageStream");
    JS_AddModuleExport(ctx, m, "MessageAccumulator");
    JS_AddModuleExport(ctx, m, "BatchResults");
    JS_AddModuleExport(ctx, m, "OpenAI");
    JS_AddModuleExport(ctx, m, "ChatCompletionStream");
    JS_AddModuleExport(ctx, m, "ChatCompletionAccumulator");
    return 0;
}
#endif
