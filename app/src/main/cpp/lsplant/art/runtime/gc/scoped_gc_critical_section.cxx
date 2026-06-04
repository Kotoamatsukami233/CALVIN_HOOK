module;

#include <android/api-level.h>

#include "logging.hpp"

export module lsplant:scope_gc_critical_section;

import :thread;
import :common;
import hook_helper;

namespace lsplant::art::gc {
// CollectorType: GC 收集器类型枚举，定义 ART 支持的所有垃圾回收器种类
// 包括真实的收集器和用于实现临界区互斥的"伪收集器"
enum CollectorType {
    // No collector selected.
    kCollectorTypeNone,
    // Non concurrent mark-sweep.
    kCollectorTypeMS,
    // Concurrent mark-sweep.
    kCollectorTypeCMS,
    // Semi-space / mark-sweep hybrid, enables compaction.
    kCollectorTypeSS,
    // Heap trimming collector, doesn't do any actual collecting.
    kCollectorTypeHeapTrim,
    // A (mostly) concurrent copying collector.
    kCollectorTypeCC,
    // The background compaction of the concurrent copying collector.
    kCollectorTypeCCBackground,
    // Instrumentation critical section fake collector.
    kCollectorTypeInstrumentation,
    // Fake collector for adding or removing application image spaces.
    kCollectorTypeAddRemoveAppImageSpace,
    // Fake collector used to implement exclusion between GC and debugger.
    kCollectorTypeDebugger,
    // A homogeneous space compaction collector used in background transition
    // when both foreground and background collector are CMS.
    kCollectorTypeHomogeneousSpaceCompact,
    // Class linker fake collector.
    kCollectorTypeClassLinker,
    // JIT Code cache fake collector.
    kCollectorTypeJitCodeCache,
    // Hprof fake collector.
    kCollectorTypeHprof,
    // Fake collector for installing/removing a system-weak holder.
    kCollectorTypeAddRemoveSystemWeakHolder,
    // Fake collector type for GetObjectsAllocated
    kCollectorTypeGetObjectsAllocated,
    // Fake collector type for ScopedGCCriticalSection
    kCollectorTypeCriticalSection,
};

// GcCause: 触发 GC 的原因枚举，用于 GC 日志、策略选择和临界区互斥
// 许多"原因"实际上是伪原因，仅用于在特定操作期间阻止 GC 运行
enum GcCause {
    // Invalid GC cause used as a placeholder.
    kGcCauseNone,
    // GC triggered by a failed allocation. Thread doing allocation is blocked waiting for GC before
    // retrying allocation.
    kGcCauseForAlloc,
    // A background GC trying to ensure there is free memory ahead of allocations.
    kGcCauseBackground,
    // An explicit System.gc() call.
    kGcCauseExplicit,
    // GC triggered for a native allocation when NativeAllocationGcWatermark is exceeded.
    // (This may be a blocking GC depending on whether we run a non-concurrent collector).
    kGcCauseForNativeAlloc,
    // GC triggered for a collector transition.
    kGcCauseCollectorTransition,
    // Not a real GC cause, used when we disable moving GC (currently for
    // GetPrimitiveArrayCritical).
    kGcCauseDisableMovingGc,
    // Not a real GC cause, used when we trim the heap.
    kGcCauseTrim,
    // Not a real GC cause, used to implement exclusion between GC and instrumentation.
    kGcCauseInstrumentation,
    // Not a real GC cause, used to add or remove app image spaces.
    kGcCauseAddRemoveAppImageSpace,
    // Not a real GC cause, used to implement exclusion between GC and debugger.
    kGcCauseDebugger,
    // GC triggered for background transition when both foreground and background collector are CMS.
    kGcCauseHomogeneousSpaceCompact,
    // Class linker cause, used to guard filling art methods with special values.
    kGcCauseClassLinker,
    // Not a real GC cause, used to implement exclusion between code cache metadata and GC.
    kGcCauseJitCodeCache,
    // Not a real GC cause, used to add or remove system-weak holders.
    kGcCauseAddRemoveSystemWeakHolder,
    // Not a real GC cause, used to prevent hprof running in the middle of GC.
    kGcCauseHprof,
    // Not a real GC cause, used to prevent GetObjectsAllocated running in the middle of GC.
    kGcCauseGetObjectsAllocated,
    // GC cause for the profile saver.
    kGcCauseProfileSaver,
};

// GCCriticalSection: 对应 ART 内部的 GC 临界区结构，用于声明一段不可被 GC 中断的代码区域
// 实际字段由 ART 运行时管理，此处仅作内存布局占位
export class GCCriticalSection {
private:
    [[maybe_unused]] void *self_;            // 持有该临界区的线程指针
    [[maybe_unused]] const char *section_name_;  // 临界区名称，用于调试和日志
};

// ScopedGCCriticalSection: RAII 包装类，进入 GC 临界区以阻止 GC 在此期间运行
// 构造时进入临界区，析构时自动退出，确保 GC 不会在修改 ART 数据结构时介入
export class ScopedGCCriticalSection {
    // 符号引用：ScopedGCCriticalSection 构造函数
    inline static auto constructor_ =
            "_ZN3art2gc23ScopedGCCriticalSectionC2EPNS_6ThreadENS0_7GcCauseENS0_13CollectorTypeE"_sym.as<void(ScopedGCCriticalSection::*)(Thread *, GcCause, CollectorType)>;
    // 符号引用：ScopedGCCriticalSection 析构函数
    inline static auto destructor_ =
            "_ZN3art2gc23ScopedGCCriticalSectionD2Ev"_sym.as<void(ScopedGCCriticalSection::*)()>;

public:
    // 构造时进入 GC 临界区，需要传入当前线程、GC 原因和收集器类型
    ScopedGCCriticalSection(Thread *self, GcCause cause, CollectorType collector_type) {
        if (constructor_) {
            constructor_(this, self, cause, collector_type);
        }
    }

    // 析构时退出 GC 临界区
    ~ScopedGCCriticalSection() {
        if (destructor_) destructor_(this);
    }

    // 初始化 GC 临界区模块，Android N+ 必须解析到构造/析构符号
    // Android 16+ 这些符号可能已移除，不影响基本 hook 功能
    static bool Init(const HookHandler &handler) {
        auto sdk_int = GetAndroidApiLevel();
        if (sdk_int >= __ANDROID_API_N__) [[likely]] {
            if (!handler(constructor_) || !handler(destructor_)) {
                LOGW("ScopedGCCriticalSection not found, GC critical section disabled (Android 16+)");
            }
        }
        return true;
    }

private:
    [[maybe_unused]] GCCriticalSection critical_section_;   // 底层临界区结构
    [[maybe_unused]] const char *old_no_suspend_reason_;    // 进入临界区前保存的暂停原因，析构时恢复
};
}  // namespace lsplant::art::gc
