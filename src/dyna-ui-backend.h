#ifndef DYNA_UI_BACKEND_H
#define DYNA_UI_BACKEND_H

#include "dynajs.h"
#include "core/dyn-ui.h"

#define DYN_UIB_KEYBOARD_PROP "dynajs.keyboard.bottom"

typedef struct {
    float w, h, scale, density;
    float insets[4];
    int touch;
} dyn_uib_view;

typedef struct {
    int (*begin)(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, dyn_uib_view* view);
    int (*draw)(void* be, JSContext* ctx, JSValueConst target, dyn_ui_t* ui, int changed, int present, uint32_t clear);
    void (*idle)(void* be, double seconds);
    void (*release)(void* be, JSValueConst target);
    const char* name;
} dyn_uib_ops;

#ifdef CONFIG_SDL3
void* dyn_uib_sdl_attach(JSContext* ctx, JSValueConst target, dyn_ui_t* ui, const dyn_uib_ops** ops);
#endif
#ifdef CONFIG_RAYLIB
void* dyn_uib_raylib_attach(JSContext* ctx, JSValueConst target, dyn_ui_t* ui, const dyn_uib_ops** ops);
#endif

#endif
