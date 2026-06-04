module;

#include "logging.hpp"

export module lsplant:jit_code_cache;

import :art_method;
import :common;
import :thread;
import hook_helper;

namespace lsplant::art::jit {
export class JitCodeCache {
    // 解析 MoveObsoleteMethod 符号，用于将旧方法的 JIT 数据（如 profiling 信息、code cache 条目）
    // 迁移到新方法。当 LSPlant 创建 backup 方法时，需要将 JIT 数据从 target 迁移到 backup。
    inline static auto MoveObsoleteMethod_ =
            "_ZN3art3jit12JitCodeCache18MoveObsoleteMethodEPNS_9ArtMethodES3_"_sym.as<void(JitCodeCache::*)(ArtMethod *, ArtMethod *)>;

    // 批量执行所有待迁移的 JIT 数据，将每个 target 方法的 JIT 数据迁移到对应的 backup 方法。
    // 如果 MoveObsoleteMethod_ 不可用（理论上不应该），则手动交换 data 指针作为降级方案。
    static void MoveObsoleteMethods(JitCodeCache *thiz) {
        auto movements = GetJitMovements();
        LOGD("Before jit cache collection, moving %zu hooked methods", movements.size());
        for (auto [target, backup] : movements) {
            if (MoveObsoleteMethod_) [[likely]]
                MoveObsoleteMethod_(thiz, target, backup);
            else {
                backup->SetData(backup->GetData());
                target->SetData(nullptr);
            }
        }
    }

    // Hook GarbageCollectCache：在 JIT 代码缓存 GC 执行前，先完成所有待处理的 JIT 数据迁移。
    // 如果不先迁移，GC 可能会回收 target 方法的 JIT 数据，导致 backup 方法失效。
    inline static auto GarbageCollectCache_ =
            "_ZN3art3jit12JitCodeCache19GarbageCollectCacheEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (JitCodeCache *thiz, Thread *self) static -> void {
            MoveObsoleteMethods(thiz);
            backup(thiz, self);
        };

    // Hook DoCollection：功能同 GarbageCollectCache_，这是另一个 JIT GC 入口点。
    // 某些 ART 版本使用 DoCollection 而非 GarbageCollectCache 作为主要 GC 路径，
    // 所以两个都需要 hook 以确保迁移在所有 GC 路径上执行。
    inline static auto DoCollection_ =
            "_ZN3art3jit12JitCodeCache12DoCollectionEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (JitCodeCache *thiz, Thread *self) static -> void {
            MoveObsoleteMethods(thiz);
            backup(thiz, self);
        };

public:
    // 初始化 JIT code cache hook：解析 MoveObsoleteMethod 符号并安装 GC hook。
    // Android 16+ 这些符号可能已移除，不影响基本 hook 功能
    static bool Init(const HookHandler &handler) {
        auto sdk_int = GetAndroidApiLevel();
        if (sdk_int >= __ANDROID_API_O__) [[likely]] {
            if (!handler(MoveObsoleteMethod_)) [[unlikely]] {
                LOGW("JitCodeCache::MoveObsoleteMethod not found, JIT data migration disabled (Android 16+)");
            }
        }
        if (sdk_int >= __ANDROID_API_N__) [[likely]] {
            if (!handler(GarbageCollectCache_, DoCollection_)) [[unlikely]] {
                LOGW("JitCodeCache GC hooks not found, JIT GC protection disabled (Android 16+)");
            }
        }
        return true;
    }
};
}  // namespace lsplant::art::jit
