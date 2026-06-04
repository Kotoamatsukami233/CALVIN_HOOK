module;

#include "logging.hpp"

export module lsplant:jit;

import :art_method;
import :common;
import :thread;
import hook_helper;

namespace lsplant::art::jit {
// JIT 编译类型枚举，用于标识不同的编译模式。
// 实际值由 ART 内部定义，这里只需要知道 kOptimized 对应的具体值。
// kUnknown 作为初始值，在 Init 中根据符号解析结果确定 kOptimized 的真实值。
enum class CompilationKind {
    kUnknown = -1,
};

export class Jit {
    // 表示 "优化编译" 的 CompilationKind 值，不同 ART 版本可能为 2 或 3，在 Init 中动态确定
    inline static auto kOptimized{CompilationKind::kUnknown};

    // 解析 EnqueueBaselineCompilation 符号（不 hook），仅用于检测该符号是否存在。
    // 如果存在，说明 ART 有三种编译类型，kOptimized = 3；否则只有两种，kOptimized = 2。
    inline static auto EnqueueBaselineCompilation_ =
        "_ZN3art3jit3Jit26EnqueueBaselineCompilationEPNS_9ArtMethodEPNS_6ThreadE"_sym
            .as<void (Jit::*)(ArtMethod *, Thread *)>;

    // Hook EnqueueOptimizedCompilation：当请求优化编译一个 backup 方法时，
    // 自动重定向到其 target 方法进行编译，确保编译的是原始方法而非 backup 副本。
    // 否则 JIT 会为 backup 方法生成不必要的机器码。
    inline static auto EnqueueOptimizedCompilation_ =
        "_ZN3art3jit3Jit27EnqueueOptimizedCompilationEPNS_9ArtMethodEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (Jit *thiz, ArtMethod *method, Thread *self) static -> void {
            if (auto target = IsBackup(method); target) [[unlikely]] {
                LOGD("Propagate enqueue compilation: %p -> %p", method, target);
                method = target;
            }
            return backup(thiz, method, self);
        };

    // Hook AddCompileTask：当编译类型为 optimized 且非预编译时，
    // 如果方法被 hook，将编译任务重定向到 target 方法。
    // 这处理了更通用的编译入口，与 EnqueueOptimizedCompilation_ 互补。
    inline static auto AddCompileTask_ =
        "_ZN3art3jit3Jit14AddCompileTaskEPNS_6ThreadEPNS_9ArtMethodENS_15CompilationKindEb"_sym.hook->*[]
        <MemBackup auto backup>
        (Jit *thiz, Thread *self, ArtMethod *method, CompilationKind compilation_kind, bool precompile) static -> void {
            if (compilation_kind == kOptimized && !precompile) {
                if (auto b = IsHooked(method); b) [[unlikely]] {
                    LOGD("Propagate compile task: %p -> %p", method, b);
                    method = b;
                }
            }
            return backup(thiz, self, method, compilation_kind, precompile);
        };

public:
    // 初始化 JIT hook：通过检测 EnqueueBaselineCompilation 符号是否存在来确定 kOptimized 的值，
    // 然后在 Android U 及以下版本安装编译重定向 hook，防止 backup 方法被 JIT 编译。
    static bool Init(const HookHandler &handler) {
        auto sdk_int = GetAndroidApiLevel();

        // 如果存在 EnqueueBaselineCompilation 符号，说明有 baseline/optimized 两种以上编译类型，
        // kOptimized = 3（baseline=2, optimized=3）；否则 kOptimized = 2
        if (handler(EnqueueBaselineCompilation_)) [[likely]] {
            kOptimized = static_cast<CompilationKind>(3);
        } else {
            kOptimized = static_cast<CompilationKind>(2);
        }

        // Android U 及以下版本需要 hook 编译任务，防止 backup 方法被编译
        if (sdk_int <= __ANDROID_API_U__) [[likely]] {
            handler(EnqueueOptimizedCompilation_);
            handler(AddCompileTask_);
        }
        return true;
    }
};
}  // namespace lsplant::art::jit
