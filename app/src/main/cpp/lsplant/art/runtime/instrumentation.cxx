module;

#include "logging.hpp"

export module lsplant:instrumentation;

import :art_method;
import :common;
import hook_helper;

namespace lsplant::art {

export class Instrumentation {
    // 判断是否应使用 backup 方法：如果方法已被 hook 且当前 entry point 与目标 quick_code 不同，
    // 则将代码更新操作重定向到 backup 方法，保护 hook 的 trampoline 不被覆盖
    inline static ArtMethod *MaybeUseBackupMethod(ArtMethod *art_method, const void *quick_code) {
        if (auto backup = IsHooked(art_method); backup && art_method->GetEntryPoint() != quick_code)
            [[unlikely]] {
            LOGD("Propagate update method code %p for hooked method %p to its backup", quick_code,
                 art_method);
            return backup;
        }
        return art_method;
    }

    // Hook UpdateMethodsCodeToInterpreterEntryPoint：
    // 防止 ART 将已被 hook 且去优化的方法的 entry point 覆盖为解释器入口，
    // 跳过已去优化的方法，将更新操作重定向到 backup 方法
    inline static auto UpdateMethodsCodeToInterpreterEntryPoint_ =
        "_ZN3art15instrumentation15Instrumentation40UpdateMethodsCodeToInterpreterEntryPointEPNS_9ArtMethodE"_sym.hook->*[]
        <MemBackup auto backup>
        (Instrumentation *thiz, ArtMethod *art_method) static -> void {
            if (IsDeoptimized(art_method)) {
                LOGV("skip update entrypoint on deoptimized method %s",
                     art_method->PrettyMethod(true).c_str());
                return;
            }
            backup(thiz, MaybeUseBackupMethod(art_method, nullptr));
        };

    // Hook InitializeMethodsCode：
    // 防止 ART 在初始化方法代码时覆盖被 hook 方法的 entry point，
    // 跳过已去优化的方法，将初始化操作重定向到 backup 方法
    inline static auto InitializeMethodsCode_ =
        "_ZN3art15instrumentation15Instrumentation21InitializeMethodsCodeEPNS_9ArtMethodEPKv"_sym.hook->*[]
         <MemBackup auto backup>
         (Instrumentation *thiz, ArtMethod *art_method, const void *quick_code) static -> void {
            if (IsDeoptimized(art_method)) {
                LOGV("skip update entrypoint on deoptimized method %s",
                     art_method->PrettyMethod(true).c_str());
                return;
            }
            backup(thiz, MaybeUseBackupMethod(art_method, quick_code), quick_code);
        };

    // Hook ReinitializeMethodsCode：
    // 防止 ART 在重新初始化方法代码时覆盖被 hook 方法的 entry point，
    // 跳过已去优化的方法，将重新初始化操作重定向到 backup 方法
    inline static auto ReinitializeMethodsCode_ =
        "_ZN3art15instrumentation15Instrumentation23ReinitializeMethodsCodeEPNS_9ArtMethodE"_sym.hook->*[]
         <MemBackup auto backup>
         (Instrumentation *thiz, ArtMethod *art_method) static -> void {
            if (IsDeoptimized(art_method)) {
                LOGV("skip update entrypoint on deoptimized method %s",
                     art_method->PrettyMethod(true).c_str());
                return;
            }
            backup(thiz, MaybeUseBackupMethod(art_method, nullptr));
        };

public:
    // 初始化 Instrumentation hook：
    // 仅在运行时为 debuggable 模式时才安装 hook（非 debuggable 模式下不需要，
    // 因为 ART 不会调用这些 Instrumentation 方法），
    // Android P+ 上需要 hook 三个方法以防止 hook trampoline 被覆盖
    static bool Init(JNIEnv *env, const HookHandler &handler) {
        if (!IsJavaDebuggable(env)) [[likely]] {
            return true;
        }
        int sdk_int = GetAndroidApiLevel();
        if (sdk_int >= __ANDROID_API_P__) [[likely]] {
            if (!handler(ReinitializeMethodsCode_, InitializeMethodsCode_, UpdateMethodsCodeToInterpreterEntryPoint_)) {
                LOGW("Instrumentation hooks not found, debuggable app hooks may not persist (Android 16+)");
            }
        }
        return true;
    }
};

}  // namespace lsplant::art
