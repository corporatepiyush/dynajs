#ifndef PROTOBUF_INTERNAL_H
#define PROTOBUF_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-codec.h"
#include "core/dyn-sb.h"
#include "core/dyn-simdk.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROTOBUF)

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PB_MAX_DEPTH 64
#define PB_MAX_FIELD 536870911u
#define PB_UNKNOWN_KEY "__protoUnknown"

enum {
    PB_T_INT32 = 0,
    PB_T_INT64,
    PB_T_UINT32,
    PB_T_UINT64,
    PB_T_SINT32,
    PB_T_SINT64,
    PB_T_FIXED32,
    PB_T_SFIXED32,
    PB_T_FIXED64,
    PB_T_SFIXED64,
    PB_T_FLOAT,
    PB_T_DOUBLE,
    PB_T_BOOL,
    PB_T_STRING,
    PB_T_BYTES,
    PB_T_MESSAGE,
    PB_T_ENUM,
    PB_T_COUNT
};

#define PB_F_REPEATED 1
#define PB_F_PACKED 2
#define PB_F_MAP 4

extern const char* const pb_type_names[17];

typedef struct pb_schema pb_schema_t;

typedef struct {
    JSAtom name;
    uint32_t number;
    uint8_t type;
    uint8_t flags;
    uint8_t ktype, vtype;
    pb_schema_t* sub;
} pb_field_t;

struct pb_schema {
    pb_field_t* f;
    size_t n;
};

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} pb_w_t;

int pb_wire_of_type(int t);

static inline int pb_is_packable(int t)
{
    int w = pb_wire_of_type(t);
    return w == 0 || w == 1 || w == 5;
}

int pb_schema_parse(JSContext* ctx, JSValueConst schema,
    pb_schema_t* out, int depth);

void pb_schema_free(JSContext* ctx, pb_schema_t* s);

typedef struct {
    JSContext* ctx;
    pb_w_t w;
    int depth;
} pb_enc_t;

int pb_enc_message(pb_enc_t* e, JSValueConst v, const pb_schema_t* s);

int pb_field_wire_ok(const pb_field_t* f, int wire);

JSValue pb_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
