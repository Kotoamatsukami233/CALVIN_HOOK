module;

#include <array>
#include <atomic>

#include <jni.h>

#include "logging.hpp"

export module lsplant:runtime;

import :common;
import hook_helper;

namespace lsplant::art {

export class Runtime {
public:
    // ART 运行时的调试状态枚举，控制运行时是否支持调试特性（如方法追踪、热替换等）。
    // kNonJavaDebuggable: 正常状态，不支持任何调试特性
    // kJavaDebuggable: 支持方法追踪和部分调试特性（如调试器附加时）
    // kJavaDebuggableAtInit: 运行时启动时即为可调试状态，支持完整的调试特性集
    enum class RuntimeDebugState {
        // This doesn't support any debug features / method tracing. This is the expected state
        // usually.
        kNonJavaDebuggable,
        // This supports method tracing and a restricted set of debug features (for ex: redefinition
        // isn't supported). We transition to this state when method tracing has started or when the
        // debugger was attached and transition back to NonDebuggable once the tracing has stopped /
        // the debugger agent has detached..
        kJavaDebuggable,
        // The runtime was started as a debuggable runtime. This allows us to support the extended
        // set
        // of debug features (for ex: redefinition). We never transition out of this state.
        kJavaDebuggableAtInit
    };

private:
    // ART Runtime 单例指针，指向全局唯一的 Runtime 实例
    inline static auto instance_ = "_ZN3art7Runtime9instance_E"_sym.as<Runtime *>;
    // Android 16+ fallback: 通过 JavaVM→JavaVMExt::runtime_ 获取的 Runtime 指针
    inline static Runtime *runtime_fallback_ = nullptr;

    // Runtime::SetJavaDebuggable 函数指针，用于设置运行时的可调试状态
    inline static auto SetJavaDebuggable_ =
            "_ZN3art7Runtime17SetJavaDebuggableEb"_sym.as<void (Runtime::*)(bool)>;

    // Runtime::SetRuntimeDebugState 函数指针，Android O+ 使用此函数设置调试状态
    inline static auto SetRuntimeDebugState_ =
            "_ZN3art7Runtime20SetRuntimeDebugStateENS0_17RuntimeDebugStateE"_sym.as<void (Runtime::*)(RuntimeDebugState)>;

    // debug_state_ 字段在 Runtime 对象中的偏移量，通过探测法在 Init 中计算得到
    inline static size_t debug_state_offset = 0U;

public:
    // 获取 ART Runtime 单例指针
    inline static Runtime *Current() { return *instance_; }

    // 设置运行时的调试状态：优先使用 SetJavaDebuggable_ 函数，
    // 若不可用则通过已知的偏移量直接写入 debug_state_ 字段
    void SetJavaDebuggable(RuntimeDebugState value) {
        if (SetJavaDebuggable_) {
            SetJavaDebuggable_(this, value != RuntimeDebugState::kNonJavaDebuggable);
        } else if (debug_state_offset > 0) {
            *reinterpret_cast<RuntimeDebugState *>(reinterpret_cast<uintptr_t>(*instance_) +
                                                   debug_state_offset) = value;
        }
    }

    // 初始化 Runtime 模块：
    // 获取 Runtime 单例指针，解析 SetJavaDebuggable/SetRuntimeDebugState 符号，
    // 通过探测法（构造假 Runtime 对象并调用 SetRuntimeDebugState）定位 debug_state_ 字段偏移量
    static bool Init(JNIEnv *env, const HookHandler &handler) {
        int sdk_int = GetAndroidApiLevel();
        if (!handler(instance_) || !*instance_) {
            LOGI("Runtime::instance_ not found, using JavaVM fallback (Android 16+)");
            // Get Runtime through JavaVM → JavaVMExt::runtime_
            JavaVM *vm = nullptr;
            if (env->GetJavaVM(&vm) != JNI_OK || !vm) {
                LOGE("Failed to get JavaVM");
                return false;
            }
            // JavaVM::functions at offset 0, JavaVMExt::runtime_ at offset sizeof(void*)
            runtime_fallback_ = *reinterpret_cast<Runtime **>(
                reinterpret_cast<uintptr_t>(vm) + sizeof(void *));
            if (!runtime_fallback_) {
                LOGE("Failed to get Runtime from JavaVM");
                return false;
            }
            instance_ = &runtime_fallback_;
        }
        LOGD("runtime instance = %p", *instance_);
        if (sdk_int >= __ANDROID_API_O__) {
            if (!handler(SetJavaDebuggable_, SetRuntimeDebugState_)) {
                LOGW("SetJavaDebuggable/SetRuntimeDebugState not found, debug state control disabled");
            }
        }
        if (SetRuntimeDebugState_) {
            static constexpr size_t kLargeEnoughSizeForRuntime = 4096;
            std::array<uint8_t, kLargeEnoughSizeForRuntime> code;
            static_assert(static_cast<int>(RuntimeDebugState::kJavaDebuggable) != 0);
            static_assert(static_cast<int>(RuntimeDebugState::kJavaDebuggableAtInit) != 0);
            code.fill(uint8_t{0});
            auto *const fake_runtime = reinterpret_cast<Runtime *>(code.data());
            SetRuntimeDebugState_(fake_runtime, RuntimeDebugState::kJavaDebuggable);
            for (size_t i = 0; i < kLargeEnoughSizeForRuntime; ++i) {
                if (*reinterpret_cast<RuntimeDebugState *>(
                        reinterpret_cast<uintptr_t>(fake_runtime) + i) ==
                    RuntimeDebugState::kJavaDebuggable) {
                    LOGD("found debug_state at offset %zu", i);
                    debug_state_offset = i;
                    break;
                }
            }
            if (debug_state_offset == 0) {
                LOGE("failed to find debug_state");
                return false;
            }
        }
        return true;
    }
};

// RAII 守卫：在构造时将 runtime 设置为可调试状态（kJavaDebuggableAtInit），
// 在析构时恢复为非调试状态（kNonJavaDebuggable）。
// 使用原子计数器确保多个 Guard 嵌套时只有最外层的析构才会真正恢复状态。
// 典型用途：在需要 JIT 编译器生成代码时临时启用 debuggable 模式
export struct JavaDebuggableGuard {
    // 构造函数：通过 CAS 原子操作递增计数器，
    // 第一个进入的线程负责将 runtime 设置为可调试状态
    JavaDebuggableGuard() {
        while (true) {
            size_t expected = 0;
            if (count.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                Runtime::Current()->SetJavaDebuggable(
                        Runtime::RuntimeDebugState::kJavaDebuggableAtInit);
                count.fetch_add(1, std::memory_order_release);
                count.notify_all();
                break;
            }
            if (expected == 1) {
                count.wait(expected, std::memory_order_acquire);
                continue;
            }
            if (count.compare_exchange_strong(expected, expected + 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                break;
            }
        }
    }

    // 析构函数：当计数器从 2 降为 1 时（即最外层 Guard 退出），恢复为非调试状态
    ~JavaDebuggableGuard() {
        while (true) {
            size_t expected = 2;
            if (count.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                Runtime::Current()->SetJavaDebuggable(
                        Runtime::RuntimeDebugState::kNonJavaDebuggable);
                count.fetch_sub(1, std::memory_order_release);
                count.notify_all();
                break;
            }
            if (expected == 1) {
                count.wait(expected, std::memory_order_acquire);
                continue;
            }
            if (count.compare_exchange_strong(expected, expected - 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                break;
            }
        }
    }

private:
    // 原子计数器：0=空闲，1=已设置为可调试，N(>1)=有 N-1 个嵌套的 Guard
    inline static std::atomic_size_t count{0};
    static_assert(std::atomic_size_t::is_always_lock_free, "Unsupported architecture");
    static_assert(std::is_same_v<std::atomic_size_t::value_type, size_t>,
                  "Unsupported architecture");
};
}  // namespace lsplant::art
