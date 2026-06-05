#include "js_interceptor.h"
#include "js_scope.h"
#include "js_ptr.h"
#include "js_engine.h"
#include "hook_sdk.h"
#include "assembler.h"
#include "memory_allocator.h"
#include "arm64_constants.h"

#include <android/log.h>
#include <cstring>
#include <vector>
#include <mutex>

#define LOG_TAG "JSInterceptor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
// AttachEntry: per-hook state
// ============================================================

struct AttachEntry {
    void *target;
    void *orig_func;
    JSValue on_enter;
    JSValue on_leave;
    JSContext *ctx;
    void *trampoline_mem;
    size_t trampoline_size;
    uint32_t orig_func_patch_offset;  // offset of MOV X16,#0 that we patch
};

static std::vector<AttachEntry *> g_entries;
static std::mutex g_entries_mutex;

// ============================================================
// ARM64 context save layout on stack
// Layout: [AttachEntry*, x0..x18, v0..v7]
// ============================================================

constexpr int kOffEntry = 0;        // 8 bytes
constexpr int kOffX0   = 8;         // 19 * 8 = 152 bytes
constexpr int kOffV0   = 8 + 19*8;  // 8 * 16 = 128 bytes
constexpr int kSaveSize = 8 + 19*8 + 8*16;  // 288 bytes, 16-aligned

// Helper to emit ARM64 instructions
static inline void emit(arm64::Assembler &a, uint32_t inst) {
    a.EmitInst(inst);
}

static inline uint32_t sub_sp(uint32_t imm) {
    return 0xD1000000u | (imm << 10) | (31 << 5) | 31;
}
static inline uint32_t add_sp(uint32_t imm) {
    return 0x91000000u | (imm << 10) | (31 << 5) | 31;
}
static inline uint32_t str_x(int rt, int rn, uint32_t imm8) {
    return 0xF9000000u | (imm8 << 10) | (rn << 5) | rt;
}
static inline uint32_t ldr_x(int rt, int rn, uint32_t imm8) {
    return 0xF9400000u | (imm8 << 10) | (rn << 5) | rt;
}
static inline uint32_t str_q(int rt, int rn, uint32_t imm16) {
    return 0x3D800000u | (imm16 << 10) | (rn << 5) | rt;
}
static inline uint32_t ldr_q(int rt, int rn, uint32_t imm16) {
    return 0x3D400000u | (imm16 << 10) | (rn << 5) | rt;
}

// ============================================================
// C dispatch: called from trampoline with SP pointing to saved context
// ============================================================

extern "C" void dispatch_on_enter(void *ctx_raw) {
    auto *base = static_cast<uint8_t *>(ctx_raw);
    auto *entry = *reinterpret_cast<AttachEntry **>(base + kOffEntry);
    if (!entry || JS_IsUndefined(entry->on_enter)) return;

    LOGI("dispatch_on_enter: entry=%p, x0=%p",
         entry, reinterpret_cast<void*>(*reinterpret_cast<uint64_t*>(base + kOffX0)));

    JSContext *ctx = entry->ctx;
    JSScope scope;

    JSValue obj = JS_NewObject(ctx);
    for (int i = 0; i < 8; i++) {
        auto val = *reinterpret_cast<uint64_t *>(base + kOffX0 + i * 8);
        char name[8];
        snprintf(name, sizeof(name), "x%d", i);
        JS_SetPropertyStr(ctx, obj, name,
                          js_ptr_new(ctx, reinterpret_cast<void *>(val)));
    }

    JSValue ret = JS_Call(ctx, entry->on_enter, JS_UNDEFINED, 1, &obj);

    if (!JS_IsException(ret) && JS_IsObject(ret)) {
        for (int i = 0; i < 8; i++) {
            char name[8];
            snprintf(name, sizeof(name), "x%d", i);
            JSValue prop = JS_GetPropertyStr(ctx, ret, name);
            if (!JS_IsUndefined(prop)) {
                void *p = js_ptr_unwrap(ctx, prop);
                if (p) *reinterpret_cast<uint64_t *>(base + kOffX0 + i * 8) =
                    reinterpret_cast<uintptr_t>(p);
            }
            JS_FreeValue(ctx, prop);
        }
    }

    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, obj);
}

extern "C" void dispatch_on_leave(void *ctx_raw) {
    auto *base = static_cast<uint8_t *>(ctx_raw);
    auto *entry = *reinterpret_cast<AttachEntry **>(base + kOffEntry);
    if (!entry || JS_IsUndefined(entry->on_leave)) return;

    LOGI("dispatch_on_leave: entry=%p, retval=%p",
         entry, reinterpret_cast<void*>(*reinterpret_cast<uint64_t*>(base + kOffX0)));

    JSContext *ctx = entry->ctx;
    JSScope scope;

    auto retval = *reinterpret_cast<uint64_t *>(base + kOffX0);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "retval",
                      js_ptr_new(ctx, reinterpret_cast<void *>(retval)));

    JSValue ret = JS_Call(ctx, entry->on_leave, JS_UNDEFINED, 1, &obj);

    if (!JS_IsException(ret)) {
        void *p = js_ptr_unwrap(ctx, ret);
        if (p) *reinterpret_cast<uint64_t *>(base + kOffX0) =
            reinterpret_cast<uintptr_t>(p);
    }

    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, obj);
}

// ============================================================
// Trampoline generation
// ============================================================
//
// The trampoline is ONE code block that does:
//   1. SUB SP, SP, #kSaveSize          ; allocate save area
//   2. Store AttachEntry* at [SP]
//   3. STR x0-x18 at [SP, #kOffX0+i*8]
//   4. STR q0-q7 at [SP, #kOffV0+i*16]
//   5. MOV x0, SP; BLR dispatch_on_enter
//   6. LDR x0-x18 (restore)
//   7. LDR q0-q7 (restore)
//   8. ADD SP, SP, #kSaveSize          ; pop save area
//   9. SUB SP, SP, #16; STR LR,[SP,#8] ; save caller LR
//  10. MOV X16, #0; BLR X16            ; call orig_func (patched later)
//  11. SUB SP, SP, #64                 ; save return regs
//      Store AttachEntry*, x0, x1, q0, q1
//  12. MOV x0, SP; BLR dispatch_on_leave
//  13. Restore x0, x1, q0, q1
//  14. ADD SP, SP, #64
//  15. LDR LR,[SP,#8]; ADD SP,SP,#16   ; restore LR
//  16. RET

static bool generate_trampolines(AttachEntry *entry) {
    NearCodeAllocator &alloc = NearCodeAllocator::Shared();
    uintptr_t target = reinterpret_cast<uintptr_t>(entry->target);

    size_t total_alloc = 1024;
    void *code_mem = alloc.AllocNearCode(total_alloc, target,
                                          static_cast<size_t>(arm64::kMaxBRange));
    if (!code_mem) {
        LOGE("Failed to allocate near code");
        return false;
    }

    // PAC-aware trampoline:
    // Save LR in X19 (callee-saved) instead of the stack,
    // so SP is unchanged when orig_func's PACIASP executes.
    arm64::Assembler a;

    // Flow:
    //   STP X19, X30, [SP, #-16]!   ; save X19 and LR on stack (SP -= 16)
    //   ... save all regs, dispatch_on_enter, restore ...
    //   LDP X19, X30, [SP], #16     ; restore X19 and LR (SP += 16)
    //   STP X19, X30, [SP, #-16]!   ; save X19 and LR again (SP -= 16)
    //   MOV X19, LR                  ; save LR in X19
    //   BLR orig_func                ; call orig (PACIASP sees correct SP)
    //   MOV LR, X19                  ; restore LR from X19
    //   STP X19, X30, [SP, #-16]!   ; save for dispatch_on_leave
    //   ... save all regs, dispatch_on_leave, restore ...
    //   LDP X19, X30, [SP], #16     ; restore
    //   LDP X19, X30, [SP], #16     ; final restore + RET

    // But simpler approach: use STP/LDP to save/restore X19+LR as a pair,
    // keeping SP adjustments balanced around the orig_func call.

    // === ON ENTER ===
    // Save X19 and LR (callee-saved pair), SP -= 16
    // STP X19, X30, [SP, #-16]! = 0xA9BF7BF3
    emit(a, 0xA9BF7BF3u);

    // Save all argument/temp regs + NEON on a separate stack frame
    emit(a, sub_sp(kSaveSize));

    // Save x0-x18
    for (int i = 0; i < 19; i++)
        emit(a, str_x(i, 31, (kOffX0 + i * 8) / 8));

    // Save q0-q7
    for (int i = 0; i < 8; i++)
        emit(a, str_q(i, 31, (kOffV0 + i * 16) / 16));

    // Store AttachEntry*
    a.EmitMovImm64(16, reinterpret_cast<uintptr_t>(entry));
    emit(a, str_x(16, 31, 0));

    // Call dispatch_on_enter(SP)
    a.EmitMovImm64(16, reinterpret_cast<uintptr_t>(&dispatch_on_enter));
    emit(a, 0x910003E0u);  // MOV X0, SP
    a.EmitBLR(16);

    // Restore x0-x18
    for (int i = 0; i < 19; i++)
        emit(a, ldr_x(i, 31, (kOffX0 + i * 8) / 8));

    // Restore q0-q7
    for (int i = 0; i < 8; i++)
        emit(a, ldr_q(i, 31, (kOffV0 + i * 16) / 16));

    emit(a, add_sp(kSaveSize));

    // === CALL ORIGINAL FUNCTION ===
    // Restore X19/LR so SP is back to original, then re-save them
    // so orig_func sees the correct SP for PACIASP.
    // LDP X19, X30, [SP], #16  (SP += 16, back to caller SP)
    emit(a, 0xA8C17BF3u);

    // Now SP = caller SP. Save X19 and LR again.
    // STP X19, X30, [SP, #-16]!
    emit(a, 0xA9BF7BF3u);

    // SP = caller SP - 16. This is what the caller would have done
    // if it called open() normally (with BL setting LR and the caller
    // having SP at caller_SP). Actually, the caller's SP is caller_SP.
    // When open() does PACIASP, it uses SP = caller_SP - 16 (from our STP).
    // When open() does STP X29,X30,[SP,#-16]!, SP = caller_SP - 32.
    // When open() does LDP X29,X30,[SP],#16, SP = caller_SP - 16.
    // When open() does AUTIASP, SP = caller_SP - 16. Matches PACIASP. OK!

    // Save LR into X19 for safekeeping across orig_func call
    // MOV X19, X30
    emit(a, 0xAA1E03F3u);

    // Call orig_func
    uint32_t patch_off = a.Buffer().CurrentOffset();
    a.EmitMovImm64(16, 0);  // placeholder
    a.EmitBLR(16);
    entry->orig_func_patch_offset = patch_off;

    // Restore LR from X19 (orig_func may have clobbered LR via AUTIASP+RET)
    // MOV X30, X19
    emit(a, 0xAA1303FEu);

    // === ON LEAVE ===
    // Save all regs for dispatch_on_leave
    emit(a, sub_sp(kSaveSize));

    for (int i = 0; i < 19; i++)
        emit(a, str_x(i, 31, (kOffX0 + i * 8) / 8));

    for (int i = 0; i < 8; i++)
        emit(a, str_q(i, 31, (kOffV0 + i * 16) / 16));

    a.EmitMovImm64(16, reinterpret_cast<uintptr_t>(entry));
    emit(a, str_x(16, 31, 0));

    a.EmitMovImm64(16, reinterpret_cast<uintptr_t>(&dispatch_on_leave));
    emit(a, 0x910003E0u);  // MOV X0, SP
    a.EmitBLR(16);

    for (int i = 0; i < 19; i++)
        emit(a, ldr_x(i, 31, (kOffX0 + i * 8) / 8));

    for (int i = 0; i < 8; i++)
        emit(a, ldr_q(i, 31, (kOffV0 + i * 16) / 16));

    emit(a, add_sp(kSaveSize));

    // Restore X19 and LR, return to caller
    // LDP X19, X30, [SP], #16
    emit(a, 0xA8C17BF3u);

    // RET
    emit(a, 0xD65F03C0u);  // RET X30

    a.Finalize();

    uint32_t code_size = a.Buffer().size;

    // Write trampoline
    alloc.BeginWrite(code_mem, total_alloc);
    memcpy(code_mem, a.Buffer().data, code_size);
    alloc.EndWrite(code_mem, total_alloc);

    // Install hook
    void *orig_func = nullptr;
    int ret = HookInstall(entry->target, code_mem, &orig_func);
    if (ret != 0) {
        LOGE("HookInstall failed: %d", ret);
        return false;
    }

    entry->orig_func = orig_func;
    entry->trampoline_mem = code_mem;
    entry->trampoline_size = code_size;

    // Patch orig_func address in trampoline
    alloc.BeginWrite(code_mem, total_alloc);
    auto *base = static_cast<uint8_t *>(code_mem);
    uint64_t val = reinterpret_cast<uintptr_t>(orig_func);
    uint32_t off = patch_off;

    *reinterpret_cast<uint32_t *>(base + off) =
        arm64::kOpMovZ | (0u << 21) | (static_cast<uint32_t>(val & 0xFFFF) << 5) | 16;
    off += 4;
    *reinterpret_cast<uint32_t *>(base + off) =
        arm64::kOpMovK | (1u << 21) | (static_cast<uint32_t>((val >> 16) & 0xFFFF) << 5) | 16;
    off += 4;
    *reinterpret_cast<uint32_t *>(base + off) =
        arm64::kOpMovK | (2u << 21) | (static_cast<uint32_t>((val >> 32) & 0xFFFF) << 5) | 16;
    off += 4;
    *reinterpret_cast<uint32_t *>(base + off) =
        arm64::kOpMovK | (3u << 21) | (static_cast<uint32_t>((val >> 48) & 0xFFFF) << 5) | 16;

    alloc.EndWrite(code_mem, total_alloc);

    LOGI("Interceptor.attach: target=%p tramp=%p orig=%p",
         entry->target, code_mem, orig_func);
    return true;
}

// ============================================================
// JS API
// ============================================================

static JSValue js_attach(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Interceptor.attach(target, callbacks)");

    void *target = js_ptr_unwrap(ctx, argv[0]);
    if (!target) return JS_EXCEPTION;

    JSValue on_enter = JS_UNDEFINED;
    JSValue on_leave = JS_UNDEFINED;

    JSValue ep = JS_GetPropertyStr(ctx, argv[1], "onEnter");
    if (JS_IsFunction(ctx, ep)) on_enter = ep;
    else JS_FreeValue(ctx, ep);

    JSValue lp = JS_GetPropertyStr(ctx, argv[1], "onLeave");
    if (JS_IsFunction(ctx, lp)) on_leave = lp;
    else JS_FreeValue(ctx, lp);

    if (JS_IsUndefined(on_enter) && JS_IsUndefined(on_leave)) {
        JS_ThrowTypeError(ctx, "Need onEnter or onLeave");
        return JS_EXCEPTION;
    }

    auto *entry = new AttachEntry{};
    entry->target = target;
    entry->ctx = ctx;
    entry->on_enter = on_enter;
    entry->on_leave = on_leave;

    if (!generate_trampolines(entry)) {
        JS_FreeValue(ctx, on_enter);
        JS_FreeValue(ctx, on_leave);
        delete entry;
        return JS_ThrowTypeError(ctx, "Trampoline generation failed");
    }

    {
        std::lock_guard<std::mutex> lock(g_entries_mutex);
        g_entries.push_back(entry);
    }
    return JS_NewObject(ctx);
}

static JSValue js_replace(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Interceptor.replace(target, replacement)");

    void *target = js_ptr_unwrap(ctx, argv[0]);
    if (!target) return JS_EXCEPTION;
    void *replacement = js_ptr_unwrap(ctx, argv[1]);
    if (!replacement) return JS_EXCEPTION;

    void *orig = nullptr;
    if (HookInstall(target, replacement, &orig) != 0)
        return JS_ThrowTypeError(ctx, "HookInstall failed");

    auto *entry = new AttachEntry{};
    entry->target = target;
    entry->orig_func = orig;
    entry->ctx = ctx;
    entry->on_enter = JS_UNDEFINED;
    entry->on_leave = JS_UNDEFINED;

    {
        std::lock_guard<std::mutex> lock(g_entries_mutex);
        g_entries.push_back(entry);
    }
    return JS_NewObject(ctx);
}

static JSValue js_detach_all(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    std::lock_guard<std::mutex> lock(g_entries_mutex);
    for (auto *e : g_entries) {
        HookUninstall(e->target);
        JS_FreeValue(ctx, e->on_enter);
        JS_FreeValue(ctx, e->on_leave);
        delete e;
    }
    g_entries.clear();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry interceptor_funcs[] = {
    JS_CFUNC_DEF("attach", 2, js_attach),
    JS_CFUNC_DEF("replace", 2, js_replace),
    JS_CFUNC_DEF("detachAll", 0, js_detach_all),
};

void js_interceptor_init(JSContext *ctx, JSValue ns) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, obj, interceptor_funcs,
                               sizeof(interceptor_funcs) / sizeof(interceptor_funcs[0]));
    JS_DefinePropertyValueStr(ctx, ns, "Interceptor", obj, JS_PROP_C_W_E);
}

void js_interceptor_cleanup() {
    JSContext *ctx = JSEngine::GetContext();
    std::lock_guard<std::mutex> lock(g_entries_mutex);
    for (auto *e : g_entries) {
        HookUninstall(e->target);
        if (ctx) {
            JS_FreeValue(ctx, e->on_enter);
            JS_FreeValue(ctx, e->on_leave);
        }
        delete e;
    }
    g_entries.clear();
}
