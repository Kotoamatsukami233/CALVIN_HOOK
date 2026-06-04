module;

#include "logging.hpp"

export module lsplant:thread;

import hook_helper;

namespace lsplant::art {
// Thread: 对应 ART 虚拟机内部的 art::Thread 类，用于获取当前线程的 ART Thread 对象
export class Thread {
    // 符号引用：Thread::CurrentFromGdb()，获取当前线程的 art::Thread 指针
    // Android 16+ 移除了此符号，需要回退到 TLS 方式获取
    inline static auto CurrentFromGdb_ = "_ZN3art6Thread14CurrentFromGdbEv"_sym.as<Thread *()>;
    inline static bool use_tls_fallback_ = false;

    // 通过 TLS 插槽 7 (TLS_SLOT_ART_THREAD_SELF) 获取当前 ART Thread 指针
    static Thread *GetCurrentFromTLS() {
#if defined(__aarch64__)
        uintptr_t tls_base;
        __asm__("mrs %0, tpidr_el0" : "=r"(tls_base));
        return *reinterpret_cast<Thread **>(tls_base + 7 * sizeof(void *));
#elif defined(__arm__)
        uintptr_t tls_base;
        __asm__("mrc p15, 0, %0, c13, c0, 2" : "=r"(tls_base));
        return *reinterpret_cast<Thread **>(tls_base + 7 * sizeof(void *));
#elif defined(__x86_64__)
        uintptr_t tls_base;
        __asm__("mov %%fs:0, %0" : "=r"(tls_base));
        return *reinterpret_cast<Thread **>(tls_base + 7 * sizeof(void *));
#elif defined(__i386__)
        uintptr_t tls_base;
        __asm__("mov %%gs:0, %0" : "=r"(tls_base));
        return *reinterpret_cast<Thread **>(tls_base + 7 * sizeof(void *));
#else
        return nullptr;
#endif
    }

public:
    static Thread *Current() {
        if (CurrentFromGdb_) [[likely]]
            return CurrentFromGdb_();
        if (use_tls_fallback_)
            return GetCurrentFromTLS();
        return nullptr;
    }

    static bool Init(const HookHandler &handler) {
        if (handler(CurrentFromGdb_)) {
            return true;
        }
        LOGI("CurrentFromGdb not found, using TLS slot 7 fallback (Android 16+)");
        use_tls_fallback_ = true;
        return true;
    }
};
}  // namespace lsplant::art
