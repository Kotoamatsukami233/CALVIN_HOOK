#include "js_engine.h"
#include "js_scope.h"
#include "js_ptr.h"
#include "js_memory.h"
#include "js_module.h"
#include "js_interceptor.h"
#include "js_native.h"
#include "js_java_bridge.h"
#include "js_socket_server.h"
#include "quickjs.h"

#include <android/log.h>
#include <cstdio>

// Anti-detection: avoid identifiable log tags
#define LOG_TAG "Core"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Anti-detection: compile-time XOR string obfuscation
// Prevents "QuickJS", "NativePointer", etc. from appearing as plaintext in the binary
constexpr char kXorKey = 0x55;

#define OBFUSCATE(str) ([]() -> const char* { \
    constexpr auto encrypted = []() { \
        struct { char data[sizeof(str)]; } s; \
        for (size_t i = 0; i < sizeof(str); i++) s.data[i] = str[i] ^ kXorKey; \
        return s; \
    }(); \
    static char decrypted[sizeof(str)]; \
    static bool done = false; \
    if (!done) { for (size_t i = 0; i < sizeof(str); i++) decrypted[i] = encrypted.data[i] ^ kXorKey; done = true; } \
    return decrypted; \
}())

// Embedded runtime JS
extern const char js_runtime_source[];

// --- Global state ---
static JSRuntime  *g_rt  = nullptr;
static JSContext   *g_ctx = nullptr;
static JSSendCallback g_send_cb;
static JavaVM *g_jvm = nullptr;

// --- send() JS function ---
static JSValue js_send(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) return JS_EXCEPTION;

    if (g_send_cb) {
        g_send_cb(std::string(msg));
    }
    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

// --- Global C functions exposed to JS ---
static const JSCFunctionListEntry global_funcs[] = {
    JS_CFUNC_DEF("send", 1, js_send),
};

// ============================================================

bool JSEngine::Init() {
    if (g_rt) return true;

    JSScope::InitMutex();

    g_rt = JS_NewRuntime();
    if (!g_rt) {
        LOGE("JS_NewRuntime failed");
        return false;
    }

    // 8 MB stack, 64 MB memory limit
    JS_SetMaxStackSize(g_rt, 8 * 1024 * 1024);
    JS_SetMemoryLimit(g_rt, 64 * 1024 * 1024);

    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) {
        LOGE("JS_NewContext failed");
        JS_FreeRuntime(g_rt);
        g_rt = nullptr;
        return false;
    }

    // Register global functions
    JSValue global = JS_GetGlobalObject(g_ctx);
    JS_SetPropertyFunctionList(g_ctx, global, global_funcs,
                               sizeof(global_funcs) / sizeof(global_funcs[0]));

    // Create namespace object for our APIs
    JSValue ns = JS_NewObject(g_ctx);
    JS_DefinePropertyValueStr(g_ctx, global, "_hook", ns, JS_PROP_C_W_E);

    // Register NativePointer class
    js_ptr_init(g_ctx, ns);

    // Register Phase 3 modules
    js_memory_init(g_ctx, ns);
    js_module_init(g_ctx, ns);
    js_interceptor_init(g_ctx, ns);

    // Register Phase 4 modules
    js_native_init(g_ctx, ns);

    // Register Phase 5 modules
    js_java_bridge_init(g_ctx, ns);

    // Register Phase 6 modules
    js_socket_server_init(g_ctx, ns);

    JS_FreeValue(g_ctx, global);

    // Run the bootstrap script
    {
        JSScope scope;
        JSValue ret = JS_Eval(g_ctx, js_runtime_source,
                              strlen(js_runtime_source),
                              "<runtime>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(g_ctx);
            const char *err = JS_ToCString(g_ctx, exc);
            LOGE("Runtime script error: %s", err ? err : "(unknown)");
            JS_FreeCString(g_ctx, err);
            JS_FreeValue(g_ctx, exc);
            JS_FreeValue(g_ctx, ret);
            // Non-fatal: engine still usable for basic eval
        } else {
            JS_FreeValue(g_ctx, ret);
        }
    }

    LOGI("JSEngine initialized");
    return true;
}

void JSEngine::Shutdown() {
    if (!g_rt) return;
    {
        JSScope scope;
    }
    js_interceptor_cleanup();
    js_java_bridge_cleanup();
    js_socket_server_stop();
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = nullptr;
    g_rt = nullptr;
    g_send_cb = nullptr;
    JSScope::DestroyMutex();
    LOGI("JSEngine shutdown");
}

std::string JSEngine::LoadScript(const std::string &source,
                                  const std::string &filename) {
    if (!g_ctx) return "Error: engine not initialized";

    JSScope scope;

    JSValue ret = JS_Eval(g_ctx, source.c_str(), source.size(),
                          filename.c_str(),
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_BACKTRACE_BARRIER);

    std::string result;

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(g_ctx);
        const char *err = JS_ToCString(g_ctx, exc);
        result = err ? std::string("Error: ") + err : "Error: unknown";
        JS_FreeCString(g_ctx, err);
        JS_FreeValue(g_ctx, exc);
    } else {
        const char *str = JS_ToCString(g_ctx, ret);
        result = str ? str : "(no result)";
        JS_FreeCString(g_ctx, str);
    }

    JS_FreeValue(g_ctx, ret);
    return result;
}

void JSEngine::SetSendCallback(JSSendCallback cb) {
    g_send_cb = std::move(cb);
}

JSRuntime *JSEngine::GetRuntime() { return g_rt; }
JSContext *JSEngine::GetContext() { return g_ctx; }
JavaVM *JSEngine::GetJavaVM() { return g_jvm; }

void JSEngine::SetJavaVM(JavaVM *vm) { g_jvm = vm; }

// Static mutex definition for JSScope
pthread_mutex_t JSScope::mutex_;
