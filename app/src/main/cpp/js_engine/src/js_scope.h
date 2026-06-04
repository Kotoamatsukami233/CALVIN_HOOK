#pragma once

#include <pthread.h>
#include "quickjs.h"
#include "js_engine.h"

// RAII guard: acquires recursive mutex + JS_Enter/Leave.
// All entry into QuickJS must go through this.
class JSScope {
public:
    JSScope() {
        pthread_mutex_lock(&mutex_);
        JSRuntime *rt = JSEngine::GetRuntime();
        if (rt) JS_Enter(rt);
        entered_ = (rt != nullptr);
    }

    ~JSScope() {
        JSRuntime *rt = JSEngine::GetRuntime();
        if (rt && entered_) JS_Leave(rt);
        pthread_mutex_unlock(&mutex_);
    }

    JSScope(const JSScope &) = delete;
    JSScope &operator=(const JSScope &) = delete;

    static void InitMutex() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    static void DestroyMutex() {
        pthread_mutex_destroy(&mutex_);
    }

private:
    bool entered_ = false;
    static pthread_mutex_t mutex_;
};
