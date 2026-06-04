#include "js_java_bridge.h"
#include "js_scope.h"
#include "js_ptr.h"
#include "js_engine.h"
#include "quickjs.h"

#include <android/log.h>
#include <jni.h>
#include <cstring>
#include <mutex>
#include <vector>

#define LOG_TAG "JSJava"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
// JNI helpers
// ============================================================

static JNIEnv *get_jni_env() {
    JavaVM *jvm = JSEngine::GetJavaVM();
    if (!jvm) return nullptr;

    JNIEnv *env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, "JSJavaBridge", nullptr};
    int ret = jvm->AttachCurrentThread(&env, &args);
    if (ret != JNI_OK) {
        LOGE("AttachCurrentThread failed: %d", ret);
        return nullptr;
    }
    return env;
}

static void release_jni_env() {
    JavaVM *jvm = JSEngine::GetJavaVM();
    if (jvm) jvm->DetachCurrentThread();
}

// ============================================================
// Java.perform(fn)
// ============================================================

static JSValue js_java_perform(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Java.perform requires a function argument");

    // Attach JNI thread
    JNIEnv *env = get_jni_env();
    if (!env) return JS_ThrowTypeError(ctx, "Failed to attach JNI thread");

    JSScope scope;

    // Call the JS function
    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        LOGE("Java.perform error: %s", err ? err : "unknown");
        JS_FreeCString(ctx, err);
        JS_Throw(ctx, exc);
        return JS_EXCEPTION;
    }

    JS_FreeValue(ctx, ret);
    // Don't detach — may be called from main thread or repeatedly
    return JS_UNDEFINED;
}

// ============================================================
// Java.use(className) — returns a wrapper with .implementation setter
// ============================================================

// Per-method hook data
struct JavaMethodHook {
    JSValue callback;     // JS callback function
    jmethodID methodId;   // hooked method
    jclass clazz;         // class ref (global)
    std::string methodName;
    std::string methodSig;
    bool isHooked;
};

static std::vector<JavaMethodHook *> g_java_hooks;
static std::mutex g_java_hooks_mutex;

// Store global refs for cleanup
struct JavaClassWrapper {
    jclass clazz;         // global ref
    std::string className;
    JSContext *ctx;
};

static void java_method_hook_finalize(JSRuntime *rt, JSValue val) {
    auto *hook = static_cast<JavaMethodHook *>(JS_GetOpaque(val, 0));
    if (hook) {
        JS_FreeValueRT(rt, hook->callback);
        JNIEnv *env = get_jni_env();
        if (env && hook->clazz) env->DeleteGlobalRef(hook->clazz);
        delete hook;
    }
}

// Create a Java class wrapper that lets JS access methods by name
// Usage: const Foo = Java.use("com.example.Foo");
//        Foo.bar.implementation = function(args) { ... };

static JSValue js_java_use(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Java.use(className)");

    const char *className = JS_ToCString(ctx, argv[0]);
    if (!className) return JS_EXCEPTION;

    JNIEnv *env = get_jni_env();
    if (!env) {
        JS_FreeCString(ctx, className);
        return JS_ThrowTypeError(ctx, "JNI not available");
    }

    // Replace dots with slashes for JNI FindClass
    std::string jniName(className);
    for (auto &c : jniName) if (c == '.') c = '/';

    jclass clazz = env->FindClass(jniName.c_str());
    JS_FreeCString(ctx, className);

    if (!clazz) {
        env->ExceptionClear();
        return JS_ThrowTypeError(ctx, "Class not found");
    }

    // Create a global ref to keep the class alive
    jclass globalClazz = static_cast<jclass>(env->NewGlobalRef(clazz));
    env->DeleteLocalRef(clazz);

    // Return a JS object that acts as a class wrapper
    // We use a Proxy-like approach: return an object with method access
    JSValue wrapper = JS_NewObject(ctx);

    // Store the class reference as opaque data
    auto *cw = new JavaClassWrapper{globalClazz, jniName, ctx};
    JS_SetOpaque(wrapper, cw);

    return wrapper;
}

// Hook a Java method using HookBridge.hookMethod via JNI
static JSValue js_java_hook_method(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    // Expected: hookMethod(className, methodName, paramTypes, callback)
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "hookMethod(className, methodName, paramTypes, callback)");

    JNIEnv *env = get_jni_env();
    if (!env) return JS_ThrowTypeError(ctx, "JNI not available");

    const char *className = JS_ToCString(ctx, argv[0]);
    const char *methodName = JS_ToCString(ctx, argv[1]);

    if (!className || !methodName) {
        JS_FreeCString(ctx, className);
        JS_FreeCString(ctx, methodName);
        return JS_EXCEPTION;
    }

    // Find the HookBridge class
    jclass hbClass = env->FindClass("com/io/hook/HookBridge");
    if (!hbClass) {
        env->ExceptionClear();
        JS_FreeCString(ctx, className);
        JS_FreeCString(ctx, methodName);
        return JS_ThrowTypeError(ctx, "HookBridge class not found");
    }

    // Call HookBridge.isInitialized() to make sure LSPlant is ready
    jmethodID isInitId = env->GetStaticMethodID(hbClass, "isInitialized", "()Z");
    if (!isInitId || !env->CallStaticBooleanMethod(hbClass, isInitId)) {
        JS_FreeCString(ctx, className);
        JS_FreeCString(ctx, methodName);
        return JS_ThrowTypeError(ctx, "HookBridge not initialized");
    }

    // Find target class
    std::string jniName(className);
    for (auto &c : jniName) if (c == '.') c = '/';
    JS_FreeCString(ctx, className);

    jclass targetClass = env->FindClass(jniName.c_str());
    if (!targetClass) {
        env->ExceptionClear();
        JS_FreeCString(ctx, methodName);
        return JS_ThrowTypeError(ctx, "Target class not found: %s", jniName.c_str());
    }

    // Find the method
    const char *mName = methodName;

    // For simplicity, try to find any method with the given name
    // First get all declared methods
    jclass methodClass = env->FindClass("java/lang/reflect/Method");
    jclass memberClass = env->FindClass("java/lang/reflect/Member");
    jmethodID getDeclaredMethodsId = env->GetMethodID(targetClass, "getDeclaredMethods",
                                                      "()[Ljava/lang/reflect/Method;");
    jmethodID getNameId = env->GetMethodID(memberClass, "getName", "()Ljava/lang/String;");

    jobjectArray methods = static_cast<jobjectArray>(
        env->CallObjectMethod(targetClass, getDeclaredMethodsId));

    jmethodID targetMethod = nullptr;
    jsize methodCount = env->GetArrayLength(methods);

    for (jsize i = 0; i < methodCount; i++) {
        jobject method = env->GetObjectArrayElement(methods, i);
        jstring name = static_cast<jstring>(env->CallObjectMethod(method, getNameId));
        const char *n = env->GetStringUTFChars(name, nullptr);
        bool match = (strcmp(n, mName) == 0);
        env->ReleaseStringUTFChars(name, n);
        env->DeleteLocalRef(name);

        if (match) {
            // Get method ID from the Method object
            jclass methodClazz = env->FindClass("java/lang/reflect/Method");
            jmethodID getMethodId = env->GetMethodID(methodClazz, "getName", "()Ljava/lang/String;");

            // Use the Method object directly with HookBridge
            // Convert to Member (it is a Member)
            targetMethod = env->FromReflectedMethod(method);
            env->DeleteLocalRef(method);
            break;
        }
        env->DeleteLocalRef(method);
    }
    env->DeleteLocalRef(methods);
    JS_FreeCString(ctx, methodName);

    if (!targetMethod) {
        return JS_ThrowTypeError(ctx, "Method not found");
    }

    // Store the JS callback
    auto *hook = new JavaMethodHook{};
    hook->callback = JS_DupValue(ctx, argv[3]);
    hook->methodId = targetMethod;
    hook->isHooked = false;

    {
        std::lock_guard<std::mutex> lock(g_java_hooks_mutex);
        g_java_hooks.push_back(hook);
    }

    // Return the hook info
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "methodId", JS_NewInt64(ctx, reinterpret_cast<int64_t>(targetMethod)));
    return result;
}

// Call HookBridge.invokeOriginalMethod via JNI
static JSValue js_java_invoke_original(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "invokeOriginal(className, methodName, thisObj, args...)");

    // This is a simplified version — full implementation would marshal args
    return JS_UNDEFINED;
}

// ============================================================
// Registration
// ============================================================

static const JSCFunctionListEntry java_funcs[] = {
    JS_CFUNC_DEF("perform", 1, js_java_perform),
    JS_CFUNC_DEF("use", 1, js_java_use),
    JS_CFUNC_DEF("hookMethod", 4, js_java_hook_method),
    JS_CFUNC_DEF("invokeOriginal", 3, js_java_invoke_original),
};

void js_java_bridge_init(JSContext *ctx, JSValue ns) {
    JSValue java = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, java, java_funcs,
                               sizeof(java_funcs) / sizeof(java_funcs[0]));
    JS_DefinePropertyValueStr(ctx, ns, "Java", java, JS_PROP_C_W_E);
}

void js_java_bridge_cleanup() {
    JSContext *ctx = JSEngine::GetContext();
    std::lock_guard<std::mutex> lock(g_java_hooks_mutex);
    for (auto *hook : g_java_hooks) {
        if (ctx) JS_FreeValue(ctx, hook->callback);
        delete hook;
    }
    g_java_hooks.clear();
}
