#pragma once

#include <string>
#include <functional>
#include <jni.h>

struct JSRuntime;
struct JSContext;

// Callback for send() from JS
using JSSendCallback = std::function<void(const std::string &json)>;

class JSEngine {
public:
    // Initialize the engine. Call once.
    static bool Init();

    // Shutdown and free resources.
    static void Shutdown();

    // Evaluate a JS script, return result as string.
    static std::string LoadScript(const std::string &source, const std::string &filename = "<script>");

    // Set the callback invoked when JS calls send()
    static void SetSendCallback(JSSendCallback cb);

    // Set the JavaVM pointer (call from JNI_OnLoad)
    static void SetJavaVM(JavaVM *vm);

    // Getters for internal use
    static JSRuntime *GetRuntime();
    static JSContext *GetContext();
    static JavaVM *GetJavaVM();
};
