module;

#include "logging.hpp"

export module lsplant:thread_list;

import hook_helper;

namespace lsplant::art::thread_list {

// ScopedSuspendAll: RAII 包装类，在构造时暂停所有线程，析构时恢复
// 用于在修改 ART 内部数据结构时获得独占访问权，避免并发问题
export class ScopedSuspendAll {
    // 符号引用：ScopedSuspendAll 构造函数，优先使用此方式暂停所有线程
    inline static auto constructor_ =
            "_ZN3art16ScopedSuspendAllC2EPKcb"_sym.as<void(ScopedSuspendAll::*)(const char *, bool)>;

    // 符号引用：ScopedSuspendAll 析构函数，恢复所有线程
    inline static auto destructor_ =
            "_ZN3art16ScopedSuspendAllD2Ev"_sym.as<void(ScopedSuspendAll::*)()>;

    // 回退方案：Dbg::SuspendVM，在旧版本（无 ScopedSuspendAll）中使用
    inline static auto SuspendVM_ = "_ZN3art3Dbg9SuspendVMEv"_sym.as<void()>;
    // 回退方案：Dbg::ResumeVM，配合 SuspendVM 使用
    inline static auto ResumeVM_ = "_ZN3art3Dbg8ResumeVMEv"_sym.as<void()>;

public:
    // 构造时暂停所有线程。优先使用 ScopedSuspendAll，不可用时回退到 SuspendVM
    ScopedSuspendAll(const char *cause, bool long_suspend) {
        if (constructor_) {
            constructor_(this, cause, long_suspend);
        } else if (SuspendVM_) {
            SuspendVM_();
        }
    }

    // 析构时恢复所有线程。与构造函数配对使用，确保线程必定被恢复
    ~ScopedSuspendAll() {
        if (destructor_) {
            destructor_(this);
        } else if (ResumeVM_) {
            ResumeVM_();
        }
    }

    // 初始化暂停/恢复机制，至少需要一种可用方案（ScopedSuspendAll 或 SuspendVM/ResumeVM）
    // Android 16+ 移除了这些符号，但不影响基本 hook 功能
    static bool Init(const HookHandler &handler) {
        if (!handler(constructor_, SuspendVM_)) [[unlikely]] {
            LOGW("ScopedSuspendAll/SuspendVM not found, thread suspension disabled (Android 16+)");
        }
        if (!handler(destructor_, ResumeVM_)) [[unlikely]] {
            LOGW("ScopedSuspendAll destructor/ResumeVM not found (Android 16+)");
        }
        return true;
    }
};

}  // namespace lsplant::art::thread_list
