#include "js_native.h"
#include "js_ptr.h"
#include "js_scope.h"
#include "js_engine.h"
#include "assembler.h"
#include "memory_allocator.h"

#include <android/log.h>
#include <cstring>
#include <cstdlib>
#include <vector>

#define LOG_TAG "JSNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
// Type system
// ============================================================

enum ArgType {
    kTypeVoid,
    kTypeInt,
    kTypeUInt8,
    kTypeInt8,
    kTypeUInt16,
    kTypeInt16,
    kTypeUInt32,
    kTypeInt32,
    kTypeUInt64,
    kTypeInt64,
    kTypeFloat,
    kTypeDouble,
    kTypePointer,
    kTypeSizeT,
};

static bool parse_type(const char *s, ArgType *out) {
    if (!s) return false;
    if (strcmp(s, "void") == 0) { *out = kTypeVoid; return true; }
    if (strcmp(s, "int") == 0) { *out = kTypeInt; return true; }
    if (strcmp(s, "uint8") == 0 || strcmp(s, "uchar") == 0) { *out = kTypeUInt8; return true; }
    if (strcmp(s, "int8") == 0 || strcmp(s, "char") == 0) { *out = kTypeInt8; return true; }
    if (strcmp(s, "uint16") == 0 || strcmp(s, "ushort") == 0) { *out = kTypeUInt16; return true; }
    if (strcmp(s, "int16") == 0 || strcmp(s, "short") == 0) { *out = kTypeInt16; return true; }
    if (strcmp(s, "uint32") == 0) { *out = kTypeUInt32; return true; }
    if (strcmp(s, "int32") == 0) { *out = kTypeInt32; return true; }
    if (strcmp(s, "uint64") == 0) { *out = kTypeUInt64; return true; }
    if (strcmp(s, "int64") == 0 || strcmp(s, "long") == 0) { *out = kTypeInt64; return true; }
    if (strcmp(s, "float") == 0) { *out = kTypeFloat; return true; }
    if (strcmp(s, "double") == 0) { *out = kTypeDouble; return true; }
    if (strcmp(s, "pointer") == 0 || strcmp(s, "ptr") == 0) { *out = kTypePointer; return true; }
    if (strcmp(s, "size_t") == 0) { *out = kTypeSizeT; return true; }
    return false;
}

static bool is_float_type(ArgType t) {
    return t == kTypeFloat || t == kTypeDouble;
}

static bool is_void_type(ArgType t) {
    return t == kTypeVoid;
}

static size_t type_size(ArgType t) {
    switch (t) {
        case kTypeVoid: return 0;
        case kTypeFloat: return 4;
        case kTypeDouble: return 8;
        default: return 8;  // all integer/pointer types are 8 bytes on ARM64
    }
}

// ============================================================
// ARM64 calling convention dispatch
// ============================================================
//
// ffi_call_arm64(fn, nargs, arg_types, arg_values, ret_type, ret_value)
//   x0 = function pointer
//   x1 = nargs
//   x2 = arg_types (ArgType*)
//   x3 = arg_values (uint64_t*)
//   x4 = ret_type (ArgType*)
//   x5 = ret_value (uint64_t*)
//
// For each arg:
//   If float/double → put in vN (up to v7), then stack
//   If integer/ptr  → put in xN (up to x19+x0..x7), then stack
//   Counters: int_reg_idx, float_reg_idx
//
// We implement this as a C function that uses inline assembly.
// Actually, the simplest approach: generate a per-signature assembly stub
// using the Assembler from hook_sdk, similar to what Interceptor does.

// Alternative simpler approach: use a variadic C dispatch function.
// On ARM64, all integer args come in x0-x7, all float args in v0-v7.
// We can pass arg values in a union array and use a switch-dispatch.

// Simplest correct approach for our limited type set:
// We marshal all args as uint64_t, then use inline asm to place them.
// For float args, we reinterpret the uint64_t as double/float.

extern "C" void ffi_call_dispatch(
    void *fn,
    int nargs,
    const ArgType *types,
    const uint64_t *values,
    ArgType ret_type,
    uint64_t *ret_value);

// Assembly implementation in a separate .S file, or use inline asm.
// For now, implement in C with a fixed-max-args approach.

// ARM64 calling convention:
// Integer args: x0-x7 (up to 8)
// Float args: d0-d7 (up to 8)
// Stack args: beyond that
// Return: x0 (integer), d0 (float/double)

// We use inline assembly for the actual call.
// Strategy: build the call frame in C, then use asm to load registers and call.

static void marshal_and_call(
    void *fn, int nargs, const ArgType *types, const uint64_t *values,
    ArgType ret_type, uint64_t *ret_value)
{
    // Classify each arg as INTEGER or FLOAT
    int int_idx = 0;
    int float_idx = 0;
    uint64_t int_regs[8] = {};
    uint64_t float_regs[8] = {};

    for (int i = 0; i < nargs && i < 24; i++) {
        if (is_float_type(types[i])) {
            if (float_idx < 8) float_regs[float_idx++] = values[i];
        } else {
            if (int_idx < 8) int_regs[int_idx++] = values[i];
        }
    }

    // Use a generated assembly stub to load registers and call.
    // We generate a per-call stub using the Assembler and execute it.
    NearCodeAllocator &alloc = NearCodeAllocator::Shared();
    size_t stub_size = 512;
    void *stub_mem = alloc.AllocPage();
    if (!stub_mem) return;

    arm64::Assembler a;

    // Save callee-saved registers we'll use
    // STP x19, x20, [SP, #-16]!
    a.EmitInst(0xA9BF0000u | (20 << 10) | (31 << 5) | 19);
    // STP x21, x22, [SP, #-16]!  (for float regs pointer)
    a.EmitInst(0xA9BF0000u | (22 << 10) | (31 << 5) | 21);

    // Load integer args from int_regs array into x0-x7
    // x21 = pointer to int_regs (passed as arg)
    a.EmitMovImm64(21, reinterpret_cast<uintptr_t>(int_regs));
    for (int i = 0; i < int_idx; i++) {
        // LDR Xi, [x21, #i*8]
        a.EmitInst(0xF9400000u | (i << 10) | (21 << 5) | i);
    }

    // Load float args from float_regs array into d0-d7
    // x22 = pointer to float_regs
    a.EmitMovImm64(22, reinterpret_cast<uintptr_t>(float_regs));
    for (int i = 0; i < float_idx; i++) {
        // LDR Di, [x22, #i*8]
        a.EmitInst(0xFD400000u | (i << 10) | (22 << 5) | i);
    }

    // Call function
    a.EmitMovImm64(19, reinterpret_cast<uintptr_t>(fn));
    a.EmitBLR(19);

    // Save return value: store x0 to ret_value, d0 to ret_value (for float)
    // x19 = ret_value pointer (reuse after call)
    a.EmitMovImm64(19, reinterpret_cast<uintptr_t>(ret_value));
    // STR X0, [X19, #0]
    a.EmitInst(0xF9000000u | (0 << 10) | (19 << 5) | 0);
    // STR D0, [X19, #0] (overwrites for float case; caller handles this)
    a.EmitInst(0xFD000000u | (0 << 10) | (19 << 5) | 0);

    // Restore
    a.EmitInst(0xA8C10000u | (22 << 10) | (31 << 5) | 21); // LDP x21, x22, [SP], #16
    a.EmitInst(0xA8C10000u | (20 << 10) | (31 << 5) | 19); // LDP x19, x20, [SP], #16

    a.EmitInst(0xD65F03C0u); // RET X30

    a.Finalize();

    alloc.BeginWrite(stub_mem, stub_size);
    memcpy(stub_mem, a.Buffer().data, a.Buffer().size);
    alloc.EndWrite(stub_mem, stub_size);

    // Execute the stub
    auto *stub_fn = reinterpret_cast<void(*)()>(stub_mem);
    stub_fn();

    // Handle float return: ret_value already has the bits from d0 store
    // But for kTypeFloat, we stored 8 bytes of d0 but only need 4
    // The caller handles this based on ret_type
    (void)ret_type;
}

// ============================================================
// JS → Native value marshalling
// ============================================================

static bool js_to_native(JSContext *ctx, JSValueConst val, ArgType type, uint64_t *out) {
    switch (type) {
        case kTypePointer: {
            void *p = js_ptr_unwrap(ctx, val);
            if (!p && !JS_IsNull(val)) {
                // Allow null for pointers
                return false;
            }
            *out = reinterpret_cast<uintptr_t>(p);
            return true;
        }
        case kTypeInt:
        case kTypeInt32: {
            int32_t v;
            if (JS_ToInt32(ctx, &v, val) < 0) return false;
            int64_t ext = v;
            memcpy(out, &ext, 8);
            return true;
        }
        case kTypeUInt8: {
            int32_t v;
            if (JS_ToInt32(ctx, &v, val) < 0) return false;
            uint64_t u = static_cast<uint8_t>(v);
            memcpy(out, &u, 8);
            return true;
        }
        case kTypeInt8: {
            int32_t v;
            if (JS_ToInt32(ctx, &v, val) < 0) return false;
            int64_t ext = static_cast<int8_t>(v);
            memcpy(out, &ext, 8);
            return true;
        }
        case kTypeUInt16: {
            int32_t v;
            if (JS_ToInt32(ctx, &v, val) < 0) return false;
            uint64_t u = static_cast<uint16_t>(v);
            memcpy(out, &u, 8);
            return true;
        }
        case kTypeInt16: {
            int32_t v;
            if (JS_ToInt32(ctx, &v, val) < 0) return false;
            int64_t ext = static_cast<int16_t>(v);
            memcpy(out, &ext, 8);
            return true;
        }
        case kTypeUInt32: {
            uint32_t v;
            if (JS_ToUint32(ctx, &v, val) < 0) return false;
            uint64_t u = v;
            memcpy(out, &u, 8);
            return true;
        }
        case kTypeUInt64:
        case kTypeInt64:
        case kTypeSizeT: {
            int64_t v;
            if (JS_ToBigInt64(ctx, &v, val) < 0) {
                // Fallback: try as number
                double d;
                if (JS_ToFloat64(ctx, &d, val) < 0) return false;
                v = static_cast<int64_t>(d);
            }
            memcpy(out, &v, 8);
            return true;
        }
        case kTypeFloat: {
            double d;
            if (JS_ToFloat64(ctx, &d, val) < 0) return false;
            float f = static_cast<float>(d);
            uint64_t u = 0;
            memcpy(&u, &f, 4);
            *out = u;
            return true;
        }
        case kTypeDouble: {
            double d;
            if (JS_ToFloat64(ctx, &d, val) < 0) return false;
            memcpy(out, &d, 8);
            return true;
        }
        default:
            return false;
    }
}

static JSValue native_to_js(JSContext *ctx, ArgType type, const uint64_t *val) {
    switch (type) {
        case kTypeVoid: return JS_UNDEFINED;
        case kTypePointer:
            return js_ptr_new(ctx, reinterpret_cast<void *>(*val));
        case kTypeInt:
        case kTypeInt32: {
            int32_t v;
            memcpy(&v, val, 4);
            return JS_NewInt32(ctx, v);
        }
        case kTypeUInt8: return JS_NewUint32(ctx, static_cast<uint8_t>(*val));
        case kTypeInt8: return JS_NewInt32(ctx, static_cast<int8_t>(*val));
        case kTypeUInt16: return JS_NewUint32(ctx, static_cast<uint16_t>(*val));
        case kTypeInt16: return JS_NewInt32(ctx, static_cast<int16_t>(*val));
        case kTypeUInt32: return JS_NewUint32(ctx, static_cast<uint32_t>(*val));
        case kTypeUInt64: return JS_NewBigUint64(ctx, *val);
        case kTypeInt64: {
            int64_t v;
            memcpy(&v, val, 8);
            return JS_NewBigInt64(ctx, v);
        }
        case kTypeSizeT: {
            size_t v;
            memcpy(&v, val, sizeof(size_t));
            return JS_NewBigInt64(ctx, static_cast<int64_t>(v));
        }
        case kTypeFloat: {
            float f;
            memcpy(&f, val, 4);
            return JS_NewFloat64(ctx, static_cast<double>(f));
        }
        case kTypeDouble: {
            double d;
            memcpy(&d, val, 8);
            return JS_NewFloat64(ctx, d);
        }
        default: return JS_UNDEFINED;
    }
}

// ============================================================
// NativeFunction JS class
// ============================================================

struct NativeFuncData {
    void *address;
    ArgType ret_type;
    ArgType *arg_types;
    int nargs;
};

static JSClassID nf_class_id = 0;

static void nf_finalize(JSRuntime *rt, JSValue val) {
    auto *nf = static_cast<NativeFuncData *>(JS_GetOpaque(val, nf_class_id));
    if (nf) {
        delete[] nf->arg_types;
        delete nf;
    }
}

// Constructor: new NativeFunction(address, retType, argTypes)
static JSValue nf_constructor(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "NativeFunction(address, retType, argTypes)");

    void *addr = js_ptr_unwrap(ctx, argv[0]);
    if (!addr) return JS_EXCEPTION;

    const char *ret_str = JS_ToCString(ctx, argv[1]);
    if (!ret_str) return JS_EXCEPTION;
    ArgType ret_type;
    if (!parse_type(ret_str, &ret_type)) {
        JS_FreeCString(ctx, ret_str);
        return JS_ThrowTypeError(ctx, "Invalid return type");
    }
    JS_FreeCString(ctx, ret_str);

    // Parse arg types array
    JSValue length_val = JS_GetPropertyStr(ctx, argv[2], "length");
    int32_t nargs;
    if (JS_ToInt32(ctx, &nargs, length_val) < 0) {
        JS_FreeValue(ctx, length_val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, length_val);

    if (nargs < 0 || nargs > 24)
        return JS_ThrowTypeError(ctx, "Too many arguments (max 24)");

    auto *arg_types = nargs > 0 ? new ArgType[nargs] : nullptr;
    for (int i = 0; i < nargs; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[2], i);
        const char *s = JS_ToCString(ctx, elem);
        if (!s || !parse_type(s, &arg_types[i])) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, elem);
            delete[] arg_types;
            return JS_ThrowTypeError(ctx, "Invalid arg type at index %d", i);
        }
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, elem);
    }

    auto *nf = new NativeFuncData{addr, ret_type, arg_types, nargs};
    JS_SetOpaque(this_val, nf);
    return JS_UNDEFINED;
}

// __call__: make NativeFunction directly callable
static JSValue nf_call(JSContext *ctx, JSValueConst this_val,
                        JSValueConst new_target,
                        int argc, JSValueConst *argv, int magic) {
    (void)new_target;
    (void)magic;
    auto *nf = static_cast<NativeFuncData *>(JS_GetOpaque(this_val, nf_class_id));
    if (!nf) return JS_EXCEPTION;

    if (argc < nf->nargs)
        return JS_ThrowTypeError(ctx, "Expected %d args, got %d", nf->nargs, argc);

    // Marshal args
    auto *values = new uint64_t[nf->nargs > 0 ? nf->nargs : 1];
    for (int i = 0; i < nf->nargs; i++) {
        if (!js_to_native(ctx, argv[i], nf->arg_types[i], &values[i])) {
            delete[] values;
            return JS_EXCEPTION;
        }
    }

    // Call
    uint64_t ret_val = 0;
    marshal_and_call(nf->address, nf->nargs, nf->arg_types, values,
                     nf->ret_type, &ret_val);
    delete[] values;

    return native_to_js(ctx, nf->ret_type, &ret_val);
}

// ============================================================
// NativeCallback JS class
// ============================================================

struct NativeCbData {
    JSValue js_func;
    ArgType ret_type;
    ArgType *arg_types;
    int nargs;
    void *code_ptr;        // executable code address (what native code calls)
    void *closure_mem;     // allocated executable memory
    size_t closure_mem_size;
    JSContext *ctx;
};

static JSClassID nc_class_id = 0;

// The callback dispatch: called from assembly stub
extern "C" void native_callback_dispatch(NativeCbData *cb, uint64_t *args, uint64_t *ret_val) {
    if (!cb || JS_IsUndefined(cb->js_func)) return;

    JSScope scope;
    JSContext *ctx = cb->ctx;

    // Build JS args from native args
    auto *js_args = new JSValue[cb->nargs > 0 ? cb->nargs : 1];
    for (int i = 0; i < cb->nargs; i++) {
        js_args[i] = native_to_js(ctx, cb->arg_types[i], &args[i]);
    }

    JSValue result = JS_Call(ctx, cb->js_func, JS_UNDEFINED, cb->nargs, js_args);

    // Marshal return value
    if (!JS_IsException(result) && !is_void_type(cb->ret_type)) {
        js_to_native(ctx, result, cb->ret_type, ret_val);
    }

    JS_FreeValue(ctx, result);
    for (int i = 0; i < cb->nargs; i++) {
        JS_FreeValue(ctx, js_args[i]);
    }
    delete[] js_args;
}

// Generate assembly stub for a callback.
// The stub saves all argument registers (x0-x7, d0-d7) to a buffer,
// calls native_callback_dispatch, restores return register, and returns.
static void *generate_callback_stub(NativeCbData *cb) {
    NearCodeAllocator &alloc = NearCodeAllocator::Shared();

    // We need to:
    // 1. Allocate a save area on stack
    // 2. Save x0-x7 (potential integer args)
    // 3. Save d0-d7 (potential float args)
    // 4. Store NativeCbData pointer
    // 5. Call native_callback_dispatch(cb, args_buf, ret_buf)
    // 6. Load return value from ret_buf
    // 7. Return

    // Save area layout:
    // [NativeCbData*] (8 bytes)
    // [x0-x7]         (64 bytes)
    // [d0-d7]          (128 bytes) — saved as 64-bit doubles
    // [ret_val]        (8 bytes)
    // Total: 208 bytes, pad to 16-aligned = 208

    constexpr uint32_t kCbOffData = 0;
    constexpr uint32_t kCbOffArgs = 8;
    constexpr uint32_t kCbOffFloats = 8 + 8*8;  // 72
    constexpr uint32_t kCbOffRet = 8 + 8*8 + 8*8; // 136
    constexpr uint32_t kCbSaveSize = 144; // rounded to 16

    size_t alloc_size = 512;
    void *mem = alloc.AllocPage();  // use a full page for simplicity
    if (!mem) return nullptr;

    arm64::Assembler a;

    // SUB SP, SP, #kCbSaveSize
    a.EmitInst(0xD1000000u | (kCbSaveSize << 10) | (31 << 5) | 31);

    // Store cb data pointer
    a.EmitMovImm64(9, reinterpret_cast<uintptr_t>(cb));  // x9 as scratch
    a.EmitInst(0xF9000000u | (0 << 10) | (31 << 5) | 9); // STR X9, [SP, #0]

    // Save x0-x7
    for (int i = 0; i < 8; i++) {
        a.EmitInst(0xF9000000u | ((kCbOffArgs/8 + i) << 10) | (31 << 5) | i);
    }

    // Save d0-d7 as 64-bit (FSTR Dn, [SP, #imm])
    // FSTR Dt, [Xn, #imm] → 0xFD000000 | (imm/8 << 10) | (Xn << 5) | Dt
    for (int i = 0; i < 8; i++) {
        uint32_t off = kCbOffFloats + i * 8;
        a.EmitInst(0xFD000000u | ((off/8) << 10) | (31 << 5) | i);
    }

    // Prepare args for native_callback_dispatch(cb, args, ret)
    // x0 = cb (already loaded into x9, move to x0)
    a.EmitMovImm64(0, reinterpret_cast<uintptr_t>(cb));
    // x1 = &args[0] = SP + kCbOffArgs
    a.EmitInst(0x91000000u | (kCbOffArgs << 10) | (31 << 5) | 1); // ADD X1, SP, #kCbOffArgs
    // x2 = &ret = SP + kCbOffRet
    a.EmitInst(0x91000000u | (kCbOffRet << 10) | (31 << 5) | 2); // ADD X2, SP, #kCbOffRet

    // Call dispatch
    a.EmitMovImm64(8, reinterpret_cast<uintptr_t>(&native_callback_dispatch));
    a.EmitBLR(8);

    // Load return value from ret slot
    a.EmitInst(0xF9400000u | ((kCbOffRet/8) << 10) | (31 << 5) | 0); // LDR X0, [SP, #kCbOffRet]
    // Load float return
    a.EmitInst(0xFD400000u | ((kCbOffRet/8) << 10) | (31 << 5) | 0); // LDR D0, [SP, #kCbOffRet]

    // ADD SP, SP, #kCbSaveSize
    a.EmitInst(0x91000000u | (kCbSaveSize << 10) | (31 << 5) | 31);

    // RET
    a.EmitInst(0xD65F0000u);

    a.Finalize();

    alloc.BeginWrite(mem, alloc_size);
    memcpy(mem, a.Buffer().data, a.Buffer().size);
    alloc.EndWrite(mem, alloc_size);

    cb->closure_mem = mem;
    cb->closure_mem_size = alloc_size;
    cb->code_ptr = mem;

    return mem;
}

static void nc_finalize(JSRuntime *rt, JSValue val) {
    auto *cb = static_cast<NativeCbData *>(JS_GetOpaque(val, nc_class_id));
    if (cb) {
        JS_FreeValueRT(rt, cb->js_func);
        delete[] cb->arg_types;
        // Note: closure_mem is leaked here since we can't easily free executable pages
        // In production, you'd track and free these
        delete cb;
    }
}

// Constructor: new NativeCallback(jsFunc, retType, argTypes)
static JSValue nc_constructor(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "NativeCallback(jsFunc, retType, argTypes)");

    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "First arg must be a function");

    const char *ret_str = JS_ToCString(ctx, argv[1]);
    if (!ret_str) return JS_EXCEPTION;
    ArgType ret_type;
    if (!parse_type(ret_str, &ret_type)) {
        JS_FreeCString(ctx, ret_str);
        return JS_ThrowTypeError(ctx, "Invalid return type");
    }
    JS_FreeCString(ctx, ret_str);

    JSValue length_val = JS_GetPropertyStr(ctx, argv[2], "length");
    int32_t nargs;
    if (JS_ToInt32(ctx, &nargs, length_val) < 0) {
        JS_FreeValue(ctx, length_val);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, length_val);

    if (nargs < 0 || nargs > 8)
        return JS_ThrowTypeError(ctx, "Max 8 args for NativeCallback");

    auto *arg_types = nargs > 0 ? new ArgType[nargs] : nullptr;
    for (int i = 0; i < nargs; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, argv[2], i);
        const char *s = JS_ToCString(ctx, elem);
        if (!s || !parse_type(s, &arg_types[i])) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, elem);
            delete[] arg_types;
            return JS_ThrowTypeError(ctx, "Invalid arg type at index %d", i);
        }
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, elem);
    }

    auto *cb = new NativeCbData{};
    cb->js_func = JS_DupValue(ctx, argv[0]);
    cb->ret_type = ret_type;
    cb->arg_types = arg_types;
    cb->nargs = nargs;
    cb->ctx = ctx;

    if (!generate_callback_stub(cb)) {
        JS_FreeValue(ctx, cb->js_func);
        delete[] cb->arg_types;
        delete cb;
        return JS_ThrowTypeError(ctx, "Failed to generate callback stub");
    }

    JS_SetOpaque(this_val, cb);
    return JS_UNDEFINED;
}

// .address property getter for NativeCallback
static JSValue nc_get_address(JSContext *ctx, JSValueConst this_val) {
    auto *cb = static_cast<NativeCbData *>(JS_GetOpaque(this_val, nc_class_id));
    if (!cb) return JS_EXCEPTION;
    return js_ptr_new(ctx, cb->code_ptr);
}

// ============================================================
// Registration
// ============================================================

static const JSCFunctionListEntry nf_methods[] = {};

static const JSCFunctionListEntry nc_methods[] = {
    JS_CGETSET_DEF("address", nc_get_address, nullptr),
};

void js_native_init(JSContext *ctx, JSValue ns) {
    // --- NativeFunction ---
    static JSClassDef nf_def = {
        .class_name = "NativeFunction",
        .finalizer = nf_finalize,
        .call = nf_call,  // makes instances callable with ()
    };

    JS_NewClassID(&nf_class_id);
    JS_NewClass(JS_GetRuntime(ctx), nf_class_id, &nf_def);

    JSValue nf_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nf_proto, nf_methods,
                               sizeof(nf_methods) / sizeof(nf_methods[0]));

    JSValue nf_ctor = JS_NewCFunction2(ctx, nf_constructor, "NativeFunction",
                                        3, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, nf_ctor, nf_proto);
    JS_SetClassProto(ctx, nf_class_id, nf_proto);
    JS_DefinePropertyValueStr(ctx, ns, "NativeFunction", nf_ctor, JS_PROP_C_W_E);

    // --- NativeCallback ---
    static JSClassDef nc_def = {
        .class_name = "NativeCallback",
        .finalizer = nc_finalize,
    };

    JS_NewClassID(&nc_class_id);
    JS_NewClass(JS_GetRuntime(ctx), nc_class_id, &nc_def);

    JSValue nc_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nc_proto, nc_methods,
                               sizeof(nc_methods) / sizeof(nc_methods[0]));

    JSValue nc_ctor = JS_NewCFunction2(ctx, nc_constructor, "NativeCallback",
                                        3, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, nc_ctor, nc_proto);
    JS_SetClassProto(ctx, nc_class_id, nc_proto);
    JS_DefinePropertyValueStr(ctx, ns, "NativeCallback", nc_ctor, JS_PROP_C_W_E);
}
