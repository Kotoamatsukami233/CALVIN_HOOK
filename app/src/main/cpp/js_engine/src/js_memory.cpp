#include "js_memory.h"
#include "js_ptr.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>

// --- Helpers ---

static void *get_arg_ptr(JSContext *ctx, JSValueConst val) {
    return js_ptr_unwrap(ctx, val);
}

// --- Read functions ---

static JSValue js_read_u8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewInt32(ctx, *reinterpret_cast<uint8_t *>(p));
}

static JSValue js_read_u16(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewInt32(ctx, *reinterpret_cast<uint16_t *>(p));
}

static JSValue js_read_u32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewUint32(ctx, *reinterpret_cast<uint32_t *>(p));
}

static JSValue js_read_u64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, static_cast<int64_t>(*reinterpret_cast<uint64_t *>(p)));
}

static JSValue js_read_s8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewInt32(ctx, *reinterpret_cast<int8_t *>(p));
}

static JSValue js_read_s16(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewInt32(ctx, *reinterpret_cast<int16_t *>(p));
}

static JSValue js_read_s32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewInt32(ctx, *reinterpret_cast<int32_t *>(p));
}

static JSValue js_read_s64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewBigInt64(ctx, *reinterpret_cast<int64_t *>(p));
}

static JSValue js_read_f32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, static_cast<double>(*reinterpret_cast<float *>(p)));
}

static JSValue js_read_f64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, *reinterpret_cast<double *>(p));
}

static JSValue js_read_pointer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    void *val = *reinterpret_cast<void **>(p);
    return js_ptr_new(ctx, val);
}

static JSValue js_read_utf8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int64_t size = -1;
    if (argc >= 2) {
        if (JS_ToInt64(ctx, &size, argv[1]) < 0) return JS_EXCEPTION;
    }
    if (size < 0) {
        return JS_NewString(ctx, reinterpret_cast<const char *>(p));
    }
    return JS_NewStringLen(ctx, reinterpret_cast<const char *>(p), static_cast<size_t>(size));
}

static JSValue js_read_byte_array(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int64_t size;
    if (JS_ToInt64(ctx, &size, argv[1]) < 0) return JS_EXCEPTION;
    if (size < 0) return JS_EXCEPTION;
    return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t *>(p), static_cast<size_t>(size));
}

// --- Write functions ---

static JSValue js_write_u8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<uint8_t *>(p) = static_cast<uint8_t>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_u16(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<uint16_t *>(p) = static_cast<uint16_t>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_u32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    uint32_t v;
    if (JS_ToUint32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<uint32_t *>(p) = v;
    return JS_UNDEFINED;
}

static JSValue js_write_u64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int64_t v;
    if (JS_ToBigInt64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<uint64_t *>(p) = static_cast<uint64_t>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_s8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<int8_t *>(p) = static_cast<int8_t>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_s16(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<int16_t *>(p) = static_cast<int16_t>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_s32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<int32_t *>(p) = v;
    return JS_UNDEFINED;
}

static JSValue js_write_s64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    int64_t v;
    if (JS_ToBigInt64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<int64_t *>(p) = v;
    return JS_UNDEFINED;
}

static JSValue js_write_f32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    double v;
    if (JS_ToFloat64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<float *>(p) = static_cast<float>(v);
    return JS_UNDEFINED;
}

static JSValue js_write_f64(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    double v;
    if (JS_ToFloat64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    *reinterpret_cast<double *>(p) = v;
    return JS_UNDEFINED;
}

static JSValue js_write_pointer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    void *val = get_arg_ptr(ctx, argv[1]);
    *reinterpret_cast<void **>(p) = val;
    return JS_UNDEFINED;
}

static JSValue js_write_utf8(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    const char *str = JS_ToCString(ctx, argv[1]);
    if (!str) return JS_EXCEPTION;
    size_t len = strlen(str);
    memcpy(p, str, len + 1);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue js_write_byte_array(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    auto *p = get_arg_ptr(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;
    size_t size;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf) return JS_EXCEPTION;
    memcpy(p, buf, size);
    return JS_UNDEFINED;
}

// --- Alloc ---

static JSValue js_alloc(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    int64_t size = 1;
    if (argc >= 1) {
        if (JS_ToInt64(ctx, &size, argv[0]) < 0) return JS_EXCEPTION;
    }
    if (size <= 0) return JS_EXCEPTION;
    void *mem = calloc(1, static_cast<size_t>(size));
    if (!mem) return JS_EXCEPTION;
    return js_ptr_new(ctx, mem);
}

// --- Registration ---

static const JSCFunctionListEntry memory_funcs[] = {
    JS_CFUNC_DEF("readU8", 1, js_read_u8),
    JS_CFUNC_DEF("readU16", 1, js_read_u16),
    JS_CFUNC_DEF("readU32", 1, js_read_u32),
    JS_CFUNC_DEF("readU64", 1, js_read_u64),
    JS_CFUNC_DEF("readS8", 1, js_read_s8),
    JS_CFUNC_DEF("readS16", 1, js_read_s16),
    JS_CFUNC_DEF("readS32", 1, js_read_s32),
    JS_CFUNC_DEF("readS64", 1, js_read_s64),
    JS_CFUNC_DEF("readFloat", 1, js_read_f32),
    JS_CFUNC_DEF("readDouble", 1, js_read_f64),
    JS_CFUNC_DEF("readPointer", 1, js_read_pointer),
    JS_CFUNC_DEF("readUtf8String", 1, js_read_utf8),
    JS_CFUNC_DEF("readByteArray", 2, js_read_byte_array),
    JS_CFUNC_DEF("writeU8", 2, js_write_u8),
    JS_CFUNC_DEF("writeU16", 2, js_write_u16),
    JS_CFUNC_DEF("writeU32", 2, js_write_u32),
    JS_CFUNC_DEF("writeU64", 2, js_write_u64),
    JS_CFUNC_DEF("writeS8", 2, js_write_s8),
    JS_CFUNC_DEF("writeS16", 2, js_write_s16),
    JS_CFUNC_DEF("writeS32", 2, js_write_s32),
    JS_CFUNC_DEF("writeS64", 2, js_write_s64),
    JS_CFUNC_DEF("writeFloat", 2, js_write_f32),
    JS_CFUNC_DEF("writeDouble", 2, js_write_f64),
    JS_CFUNC_DEF("writePointer", 2, js_write_pointer),
    JS_CFUNC_DEF("writeUtf8String", 2, js_write_utf8),
    JS_CFUNC_DEF("writeByteArray", 2, js_write_byte_array),
    JS_CFUNC_DEF("alloc", 1, js_alloc),
};

void js_memory_init(JSContext *ctx, JSValue ns) {
    JSValue mem = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, mem, memory_funcs,
                               sizeof(memory_funcs) / sizeof(memory_funcs[0]));
    JS_DefinePropertyValueStr(ctx, ns, "Memory", mem, JS_PROP_C_W_E);
}
