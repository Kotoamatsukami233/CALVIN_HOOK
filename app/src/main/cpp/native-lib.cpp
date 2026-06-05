#include <jni.h>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include "hook_sdk.h"
#include "elf_symbol_resolver.h"
#include "lsplant.hpp"
#include "js_engine.h"
#include "js_socket_server.h"
#include <android/log.h>

#define LOG_TAG "HookDemo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// ============================================================
// LSPlant initialization state
// ============================================================
static bool g_lsplant_initialized = false;

// ============================================================
// Anti-detection: randomized name pool for generated classes
// Avoids the default "LSPHooker_" pattern that detectors scan for.
// These look like proguard-renamed class names.
// ============================================================
static const char* kGenClassName = "a";
static const char* kGenFieldName = "b";
static const char* kGenSourceName = "";

// ============================================================
// Test 1: Simple function hook (existing)
// ============================================================

static int secret_value = 42;

__attribute__((noinline))
int get_secret_value() {
    return secret_value;
}

int (*orig_get_secret_value)() = nullptr;

int fake_get_secret_value() {
    int original = orig_get_secret_value();
    LOGI("fake_get_secret_value() called! original=%d", original);
    return original + 1000;
}

// ============================================================
// Test 2: 16-parameter function hook (existing)
// ============================================================

__attribute__((noinline))
int sum16_params(int a1,  int a2,  int a3,  int a4,
                 int a5,  int a6,  int a7,  int a8,
                 int a9,  int a10, int a11, int a12,
                 int a13, int a14, int a15, int a16) {
    return a1*1  + a2*2  + a3*3  + a4*4
         + a5*5  + a6*6  + a7*7  + a8*8
         + a9*9  + a10*10 + a11*11 + a12*12
         + a13*13 + a14*14 + a15*15 + a16*16;
}

typedef int (*sum16_fn)(int, int, int, int, int, int, int, int,
                        int, int, int, int, int, int, int, int);

sum16_fn orig_sum16 = nullptr;

int fake_sum16(int a1,  int a2,  int a3,  int a4,
               int a5,  int a6,  int a7,  int a8,
               int a9,  int a10, int a11, int a12,
               int a13, int a14, int a15, int a16) {
    int original = orig_sum16(a1, a2, a3, a4, a5, a6, a7, a8,
                              a9, a10, a11, a12, a13, a14, a15, a16);
    LOGI("fake_sum16() original=%d  args=(%d,%d,%d,%d,%d,%d,%d,%d,"
         "%d,%d,%d,%d,%d,%d,%d,%d)",
         original,
         a1, a2, a3, a4, a5, a6, a7, a8,
         a9, a10, a11, a12, a13, a14, a15, a16);
    return original + 99999;
}

static int expected_sum16() {
    return 1*1  + 2*2  + 3*3  + 4*4
         + 5*5  + 6*6  + 7*7  + 8*8
         + 9*9  + 10*10 + 11*11 + 12*12
         + 13*13 + 14*14 + 15*15 + 16*16;
}

// ============================================================
// HookBridge native methods (registered via RegisterNatives)
// ============================================================

static jobject nativeHook(JNIEnv *env, jclass, jobject entry, jobject original, jobject callback) {
    if (!g_lsplant_initialized) {
        LOGE("HookBridge.nHook: LSPlant not initialized");
        return nullptr;
    }
    return lsplant::Hook(env, original, entry, callback);
}

static jboolean nativeUnhook(JNIEnv *env, jclass, jobject target) {
    if (!g_lsplant_initialized) return JNI_FALSE;
    return lsplant::UnHook(env, target) ? JNI_TRUE : JNI_FALSE;
}

static jboolean nativeDeoptimize(JNIEnv *env, jclass, jobject method) {
    if (!g_lsplant_initialized) return JNI_FALSE;
    return lsplant::Deoptimize(env, method) ? JNI_TRUE : JNI_FALSE;
}

static jboolean nativeIsHooked(JNIEnv *env, jclass, jobject method) {
    if (!g_lsplant_initialized) return JNI_FALSE;
    return lsplant::IsHooked(env, method) ? JNI_TRUE : JNI_FALSE;
}

static jboolean nativeIsInitialized(JNIEnv *, jclass) {
    return g_lsplant_initialized ? JNI_TRUE : JNI_FALSE;
}

// ============================================================
// Cached JNI references for send callback (initialized in JNI_OnLoad)
// ============================================================
static jclass g_scriptEngineClass = nullptr;
static jmethodID g_onNativeMessageMethod = nullptr;

// ============================================================
// ScriptEngine native methods (JS Engine bridge)
// ============================================================

static jstring nativeLoadScript(JNIEnv *env, jclass, jstring jsource) {
    if (!jsource) {
        return env->NewStringUTF("Error: null source");
    }
    const char *source = env->GetStringUTFChars(jsource, nullptr);
    std::string result = JSEngine::LoadScript(source);
    env->ReleaseStringUTFChars(jsource, source);
    return env->NewStringUTF(result.c_str());
}

// ============================================================
// MainActivity native methods
// ============================================================

static jstring nativeTestOpen(JNIEnv *env, jobject) {
    // Diagnostic: check if open() is hooked by reading its first instruction
    using open_fn_t = int(*)(const char*, int, ...);
    auto open_fn = (open_fn_t)dlsym(RTLD_DEFAULT, "open");
    uint32_t first_inst = *reinterpret_cast<uint32_t *>(open_fn);
    LOGI("nativeTestOpen: open=%p, first_inst=0x%08X", open_fn, first_inst);

    int fd = open_fn("/proc/self/maps", O_RDONLY);
    LOGI("nativeTestOpen: fd=%d", fd);
    if (fd >= 0) {
        close(fd);
        return env->NewStringUTF("open(\"/proc/self/maps\") -> fd closed");
    }
    return env->NewStringUTF("open(\"/proc/self/maps\") failed");
}

// ============================================================
// JNI_OnLoad: Initialize LSPlant + RegisterNatives
// ============================================================

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: GetEnv failed");
        return JNI_ERR;
    }

    // Store JavaVM for JS Java bridge
    JSEngine::SetJavaVM(vm);

    // --- RegisterNatives for HookBridge (anti-detection: avoids Java_com_io_hook_* symbol pattern) ---
    jclass hookBridgeClass = env->FindClass("com/io/hook/HookBridge");
    if (!hookBridgeClass) {
        LOGE("Failed to find HookBridge class");
        return JNI_ERR;
    }

    static const JNINativeMethod bridgeMethods[] = {
        {"nHook",        "(Ljava/lang/Object;Ljava/lang/reflect/Member;Ljava/lang/reflect/Method;)Ljava/lang/reflect/Method;", (void *) nativeHook},
        {"nUnhook",      "(Ljava/lang/reflect/Member;)Z",   (void *) nativeUnhook},
        {"nDeoptimize",  "(Ljava/lang/reflect/Member;)Z",   (void *) nativeDeoptimize},
        {"nIsHooked",    "(Ljava/lang/reflect/Member;)Z",    (void *) nativeIsHooked},
        {"isInitialized","()Z",                               (void *) nativeIsInitialized},
    };

    if (env->RegisterNatives(hookBridgeClass, bridgeMethods,
                             sizeof(bridgeMethods) / sizeof(bridgeMethods[0])) < 0) {
        LOGE("RegisterNatives failed for HookBridge");
        return JNI_ERR;
    }

    // --- Initialize ELF symbol resolver for libart.so ---
    if (!InitElfSymbolResolver()) {
        LOGE("Failed to initialize ELF symbol resolver");
    }

    // --- Configure LSPlant with anti-detection settings ---
    lsplant::InitInfo info;

    info.inline_hooker = [](void *target, void *hooker) -> void * {
        void *orig = nullptr;
        if (HookInstall(target, hooker, &orig) == 0) {
            return orig;
        }
        return nullptr;
    };

    info.inline_unhooker = [](void *func) -> bool {
        return HookUninstall(func) == 0;
    };

    info.art_symbol_resolver = [](std::string_view symbol) -> void * {
        return ArtSymbolResolver(symbol);
    };

    info.art_symbol_prefix_resolver = [](std::string_view prefix) -> void * {
        return ArtSymbolPrefixResolver(prefix);
    };

    // Anti-detection: change generated class/field/source names
    // Default LSPlant uses "LSPHooker_" which is easily detected.
    // These names look like proguard output: class a0, a1... with field b
    info.generated_class_name  = kGenClassName;   // "a" → generated classes: a0, a1, a2...
    info.generated_source_name = kGenSourceName;    // "" → no identifiable source tag
    info.generated_field_name  = kGenFieldName;     // "b" → short generic field name
    info.generated_method_name = "{target}";         // same as target method name

    // --- Initialize LSPlant ---
    if (lsplant::Init(env, info)) {
        g_lsplant_initialized = true;
        LOGI("LSPlant initialized successfully");
    } else {
        LOGE("LSPlant initialization failed!");
    }

    // --- Initialize JS Engine ---
    if (!JSEngine::Init()) {
        LOGE("JSEngine initialization failed!");
    } else {
        LOGI("JSEngine initialized successfully");

        // Route JS send() to Java ScriptEngine.onNativeMessage()
        JSEngine::SetSendCallback([vm](std::string msg) {
            __android_log_print(ANDROID_LOG_INFO, "JSSend", "%s", msg.c_str());

            if (!g_scriptEngineClass || !g_onNativeMessageMethod) return;

            JNIEnv *env = nullptr;
            if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
                vm->AttachCurrentThread(&env, nullptr);
            }
            if (env) {
                jstring jmsg = env->NewStringUTF(msg.c_str());
                env->CallStaticVoidMethod(g_scriptEngineClass, g_onNativeMessageMethod, jmsg);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(jmsg);
            }
        });

        // Auto-start socket server for remote script injection
        js_socket_server_start();
    }

    // --- RegisterNatives for ScriptEngine ---
    jclass scriptEngineClass = env->FindClass("com/io/hook/ScriptEngine");
    if (scriptEngineClass) {
        static const JNINativeMethod scriptMethods[] = {
            {"nativeLoadScript", "(Ljava/lang/String;)Ljava/lang/String;", (void *) nativeLoadScript},
        };
        if (env->RegisterNatives(scriptEngineClass, scriptMethods,
                                 sizeof(scriptMethods) / sizeof(scriptMethods[0])) < 0) {
            LOGE("RegisterNatives failed for ScriptEngine");
        }

        // Cache global ref and method ID for send() callback
        g_scriptEngineClass = static_cast<jclass>(env->NewGlobalRef(scriptEngineClass));
        g_onNativeMessageMethod = env->GetStaticMethodID(scriptEngineClass, "onNativeMessage",
                                                          "(Ljava/lang/String;)V");
        env->DeleteLocalRef(scriptEngineClass);
    } else {
        LOGE("Failed to find ScriptEngine class");
    }

    // --- RegisterNatives for MainActivity (testOpen) ---
    jclass mainActivityClass = env->FindClass("com/io/hook/MainActivity");
    if (mainActivityClass) {
        static const JNINativeMethod mainMethods[] = {
            {"nativeTestOpen", "()Ljava/lang/String;", (void *) nativeTestOpen},
        };
        if (env->RegisterNatives(mainActivityClass, mainMethods,
                                 sizeof(mainMethods) / sizeof(mainMethods[0])) < 0) {
            LOGE("RegisterNatives failed for MainActivity");
        }
    } else {
        LOGE("Failed to find MainActivity class");
    }

    return JNI_VERSION_1_6;
}

// ============================================================
// JNI: stringFromJNI (existing inline hook tests)
// ============================================================

extern "C" JNIEXPORT jstring JNICALL
Java_com_io_hook_MainActivity_stringFromJNI(
        JNIEnv *env, jobject /* this */) {

    std::string result;
    bool all_pass = true;

    // ---- Test 1: Simple hook ----
    LOGI("===== Test 1: Simple function hook =====");

    int t1_before = get_secret_value();
    LOGI("Before hook: %d", t1_before);

    int t1_ret = HookInstall(
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(get_secret_value)),
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fake_get_secret_value)),
        reinterpret_cast<void **>(&orig_get_secret_value));

    int t1_after = -1;
    int t1_restored = -1;
    bool t1_pass = false;

    if (t1_ret == 0) {
        t1_after = get_secret_value();
        LOGI("After hook: %d", t1_after);

        HookUninstall(
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(get_secret_value)));
        t1_restored = get_secret_value();
        LOGI("After unhook: %d", t1_restored);

        t1_pass = (t1_before == 42 && t1_after == 1042 && t1_restored == 42);
    }

    LOGI("Test 1: %s", t1_pass ? "PASS" : "FAIL");
    result += "Test 1: Simple hook\n";
    result += "  Before: " + std::to_string(t1_before) + "\n";
    result += "  Hooked: " + std::to_string(t1_after) + "\n";
    result += "  Restore: " + std::to_string(t1_restored) + "\n";
    result += std::string("  Result: ") + (t1_pass ? "PASS" : "FAIL") + "\n\n";
    if (!t1_pass) all_pass = false;

    // ---- Test 2: 16-parameter hook ----
    LOGI("===== Test 2: 16-parameter function hook =====");

    int t2_before = sum16_params(
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16);
    int t2_expected = expected_sum16();
    LOGI("Before hook: sum16=%d (expected %d)", t2_before, t2_expected);

    int t2_ret = HookInstall(
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(sum16_params)),
        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fake_sum16)),
        reinterpret_cast<void **>(&orig_sum16));

    int t2_after = -1;
    int t2_restored = -1;
    bool t2_pass = false;

    if (t2_ret == 0) {
        t2_after = sum16_params(
            1, 2, 3, 4, 5, 6, 7, 8,
            9, 10, 11, 12, 13, 14, 15, 16);
        LOGI("After hook: sum16=%d (expected %d)", t2_after, t2_expected + 99999);

        HookUninstall(
            reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(sum16_params)));

        t2_restored = sum16_params(
            1, 2, 3, 4, 5, 6, 7, 8,
            9, 10, 11, 12, 13, 14, 15, 16);
        LOGI("After unhook: sum16=%d (expected %d)", t2_restored, t2_expected);

        t2_pass = (t2_before == t2_expected)
               && (t2_after == t2_expected + 99999)
               && (t2_restored == t2_expected);
    }

    LOGI("Test 2: %s", t2_pass ? "PASS" : "FAIL");
    result += "Test 2: 16-param hook\n";
    result += "  Before: " + std::to_string(t2_before) + " (expect " + std::to_string(t2_expected) + ")\n";
    result += "  Hooked: " + std::to_string(t2_after) + " (expect " + std::to_string(t2_expected + 99999) + ")\n";
    result += "  Restore: " + std::to_string(t2_restored) + " (expect " + std::to_string(t2_expected) + ")\n";
    result += std::string("  Result: ") + (t2_pass ? "PASS" : "FAIL") + "\n\n";
    if (!t2_pass) all_pass = false;

    // ---- Test 3: LSPlant status ----
    result += "Test 3: Java Hook (LSPlant)\n";
    result += std::string("  LSPlant: ") + (g_lsplant_initialized ? "INITIALIZED" : "FAILED") + "\n\n";

    // ---- Summary ----
    result += "========================\n";
    result += std::string("All tests: ") + (all_pass ? "PASS" : "FAIL");

    return env->NewStringUTF(result.c_str());
}
