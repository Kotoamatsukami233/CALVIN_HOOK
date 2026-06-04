#include "js_ptr.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>

static JSClassID np_class_id = 0;

// --- Finalizer ---
static void np_finalize(JSRuntime *rt, JSValue val) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(val, np_class_id));
    if (np) {
        delete np;
    }
}

// --- Constructor: new NativePointer(value) ---
static JSValue np_constructor(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    void *ptr = nullptr;

    if (argc >= 1) {
        if (JS_IsNumber(argv[0])) {
            int64_t v;
            if (JS_ToInt64(ctx, &v, argv[0]) < 0)
                return JS_EXCEPTION;
            ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(v));
        } else if (JS_IsString(argv[0])) {
            const char *s = JS_ToCString(ctx, argv[0]);
            if (!s) return JS_EXCEPTION;
            char *end;
            uint64_t v = strtoull(s, &end, 0);
            JS_FreeCString(ctx, s);
            if (end == s) {
                ptr = nullptr;
            } else {
                ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(v));
            }
        } else {
            // Check if argument is already a NativePointer
            auto *existing = static_cast<NativePointer *>(
                JS_GetOpaque(argv[0], np_class_id));
            if (existing) {
                ptr = existing->value;
            }
        }
    }

    auto *np = new NativePointer{ptr};
    JS_SetOpaque(this_val, np);
    return JS_UNDEFINED;
}

// --- Methods ---
static JSValue np_is_null(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np) return JS_EXCEPTION;
    return JS_NewBool(ctx, np->value == nullptr);
}

static JSValue np_to_string(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np) return JS_EXCEPTION;
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%" PRIxPTR, reinterpret_cast<uintptr_t>(np->value));
    return JS_NewString(ctx, buf);
}

static JSValue np_to_int32(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np) return JS_EXCEPTION;
    return JS_NewInt32(ctx, static_cast<int32_t>(reinterpret_cast<uintptr_t>(np->value)));
}

static JSValue np_compare(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *a = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    void *b_raw;
    auto *b_np = static_cast<NativePointer *>(JS_GetOpaque(argv[0], np_class_id));
    if (b_np) {
        b_raw = b_np->value;
    } else {
        int64_t v;
        if (JS_ToInt64(ctx, &v, argv[0]) < 0)
            return JS_EXCEPTION;
        b_raw = reinterpret_cast<void *>(static_cast<uintptr_t>(v));
    }
    if (!a) return JS_EXCEPTION;
    auto va = reinterpret_cast<uintptr_t>(a->value);
    auto vb = reinterpret_cast<uintptr_t>(b_raw);
    if (va < vb) return JS_NewInt32(ctx, -1);
    if (va > vb) return JS_NewInt32(ctx, 1);
    return JS_NewInt32(ctx, 0);
}

// Binary ops helper
static JSValue np_binop(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv, char op) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np || argc < 1) return JS_EXCEPTION;

    uintptr_t rhs;
    auto *rhs_np = static_cast<NativePointer *>(JS_GetOpaque(argv[0], np_class_id));
    if (rhs_np) {
        rhs = reinterpret_cast<uintptr_t>(rhs_np->value);
    } else {
        int64_t v;
        if (JS_ToInt64(ctx, &v, argv[0]) < 0)
            return JS_EXCEPTION;
        rhs = static_cast<uintptr_t>(v);
    }

    uintptr_t result;
    auto lhs = reinterpret_cast<uintptr_t>(np->value);
    switch (op) {
        case '+': result = lhs + rhs; break;
        case '-': result = lhs - rhs; break;
        case '&': result = lhs & rhs; break;
        case '|': result = lhs | rhs; break;
        case '^': result = lhs ^ rhs; break;
        default: return JS_EXCEPTION;
    }
    return js_ptr_new(ctx, reinterpret_cast<void *>(result));
}

static JSValue np_add(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    return np_binop(ctx, this_val, argc, argv, '+');
}
static JSValue np_sub(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    return np_binop(ctx, this_val, argc, argv, '-');
}
static JSValue np_and(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    return np_binop(ctx, this_val, argc, argv, '&');
}
static JSValue np_or(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    return np_binop(ctx, this_val, argc, argv, '|');
}
static JSValue np_xor(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    return np_binop(ctx, this_val, argc, argv, '^');
}

static JSValue np_shr(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np || argc < 1) return JS_EXCEPTION;
    int64_t shift;
    if (JS_ToInt64(ctx, &shift, argv[0]) < 0) return JS_EXCEPTION;
    auto result = reinterpret_cast<uintptr_t>(np->value) >> shift;
    return js_ptr_new(ctx, reinterpret_cast<void *>(result));
}

static JSValue np_shl(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np || argc < 1) return JS_EXCEPTION;
    int64_t shift;
    if (JS_ToInt64(ctx, &shift, argv[0]) < 0) return JS_EXCEPTION;
    auto result = reinterpret_cast<uintptr_t>(np->value) << shift;
    return js_ptr_new(ctx, reinterpret_cast<void *>(result));
}

static JSValue np_not(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(this_val, np_class_id));
    if (!np) return JS_EXCEPTION;
    auto result = ~reinterpret_cast<uintptr_t>(np->value);
    return js_ptr_new(ctx, reinterpret_cast<void *>(result));
}

static const JSCFunctionListEntry np_methods[] = {
    JS_CFUNC_DEF("isNull", 0, np_is_null),
    JS_CFUNC_DEF("toString", 0, np_to_string),
    JS_CFUNC_DEF("toInt32", 0, np_to_int32),
    JS_CFUNC_DEF("compare", 1, np_compare),
    JS_CFUNC_DEF("add", 1, np_add),
    JS_CFUNC_DEF("sub", 1, np_sub),
    JS_CFUNC_DEF("and", 1, np_and),
    JS_CFUNC_DEF("or", 1, np_or),
    JS_CFUNC_DEF("xor", 1, np_xor),
    JS_CFUNC_DEF("shr", 1, np_shr),
    JS_CFUNC_DEF("shl", 1, np_shl),
    JS_CFUNC_DEF("not", 0, np_not),
};

uint32_t js_ptr_init(JSContext *ctx, JSValue ns) {
    static JSClassDef np_class_def = {
        .class_name = "NativePointer",
        .finalizer = np_finalize,
    };

    JS_NewClassID(&np_class_id);
    JS_NewClass(JS_GetRuntime(ctx), np_class_id, &np_class_def);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, np_methods,
                               sizeof(np_methods) / sizeof(np_methods[0]));

    JSValue ctor = JS_NewCFunction2(ctx, np_constructor, "NativePointer",
                                    1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, np_class_id, proto);

    JS_DefinePropertyValueStr(ctx, ns, "NativePointer", ctor,
                              JS_PROP_C_W_E);

    return static_cast<uint32_t>(np_class_id);
}

JSValue js_ptr_new(JSContext *ctx, void *ptr) {
    auto *np = new NativePointer{ptr};
    JSValue obj = JS_NewObjectClass(ctx, np_class_id);
    JS_SetOpaque(obj, np);
    return obj;
}

void *js_ptr_unwrap(JSContext *ctx, JSValueConst val) {
    // Try as NativePointer first
    auto *np = static_cast<NativePointer *>(JS_GetOpaque(val, np_class_id));
    if (np) return np->value;

    // Try as number
    if (JS_IsNumber(val)) {
        int64_t v;
        if (JS_ToInt64(ctx, &v, val) == 0)
            return reinterpret_cast<void *>(static_cast<uintptr_t>(v));
    }

    // Try as string (hex)
    if (JS_IsString(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return nullptr;
        char *end;
        uint64_t v = strtoull(s, &end, 0);
        JS_FreeCString(ctx, s);
        if (end != s)
            return reinterpret_cast<void *>(static_cast<uintptr_t>(v));
    }

    return nullptr;
}

uint32_t js_ptr_class_id() {
    return static_cast<uint32_t>(np_class_id);
}
