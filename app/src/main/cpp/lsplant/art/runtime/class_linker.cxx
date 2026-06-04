module;

#include <sys/types.h>

#include "logging.hpp"

export module lsplant:class_linker;

import :art_method;
import :thread;
import :common;
import :clazz;
import :handle;
import :runtime;
import hook_helper;

namespace lsplant::art {
export class ClassLinker {
private:
    // 用于将方法的 entry point 设置为解释器入口的原始函数指针，用于去优化（deoptimize）方法
    inline static auto SetEntryPointsToInterpreter_ =
        "_ZNK3art11ClassLinker27SetEntryPointsToInterpreterEPNS_9ArtMethodE"_sym.as<void(ClassLinker::*)(ArtMethod *)>;

    // Hook ShouldUseInterpreterEntrypoint：当方法已被 hook 且有 quick_code 时，强制返回 false，
    // 阻止 ART 使用解释器入口，从而确保被 hook 的方法继续走 quick_code 路径
    inline static auto ShouldUseInterpreterEntrypoint_ =
        "_ZN3art11ClassLinker30ShouldUseInterpreterEntrypointEPNS_9ArtMethodEPKv"_sym.hook->*[]
        <Backup auto backup>
        (ArtMethod *art_method, const void *quick_code)static -> bool {
            if (quick_code != nullptr && IsHooked(art_method)) [[unlikely]] {
                return false;
            }
            return backup(art_method, quick_code);
        };

    // ART 的 quick-to-interpreter 桥接函数指针，作为没有 SetEntryPointsToInterpreter_ 时的 fallback 入口
    inline static auto art_quick_to_interpreter_bridge_ =
            "art_quick_to_interpreter_bridge"_sym.as<void(void *)>;

    // Instrumentation::GetOptimizedCodeFor 的函数指针（命名空间为 instrumentation），用于获取方法的优化代码入口
    inline static auto GetOptimizedCodeFor_ =
        "_ZN3art15instrumentation15Instrumentation19GetOptimizedCodeForEPNS_9ArtMethodE"_sym
            .as<void *(ArtMethod *)>;

    // GetOptimizedCodeFor 的另一个版本（命名空间为 instrumentationL 的局部符号版本），
    // 某些 Android 版本的符号命名不同，需要同时尝试两个
    inline static auto GetOptimizedCodeForL_ =
        "_ZN3art15instrumentationL19GetOptimizedCodeForEPNS_9ArtMethodE"_sym
            .as<void *(ArtMethod *)>;

    // 如果方法已被 hook，则返回其 backup 方法；否则返回原方法。
    // 用于 RegisterNative/UnregisterNative 系列 hook 中，确保操作作用于 backup 而非被 hook 的原始方法
    inline static art::ArtMethod *MayGetBackup(art::ArtMethod *method) {
        if (auto backup = IsHooked(method); backup) [[unlikely]] {
            method = backup;
            LOGV("propagate native method: %s", method->PrettyMethod(true).data());
        }
        return method;
    }

    // Hook ArtMethod::RegisterNative（带 Thread 参数的旧版本）：
    // 将注册 native 方法的操作重定向到 backup 方法，防止覆盖 hook 的 trampoline
    inline static auto RegisterNativeThread_ =
        "_ZN3art6mirror9ArtMethod14RegisterNativeEPNS_6ThreadEPKvb"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method, Thread *thread, const void *native_method, bool is_fast) static -> void {
            return backup(thiz, MayGetBackup(method), thread, native_method, is_fast);
        };

    // Hook ArtMethod::UnregisterNative（带 Thread 参数的旧版本）：
    // 将注销 native 方法的操作重定向到 backup 方法
    inline static auto UnregisterNativeThread_ =
        "_ZN3art6mirror9ArtMethod16UnregisterNativeEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method, Thread *thread) static -> void {
            return backup(thiz, MayGetBackup(method), thread);
        };

    // Hook ArtMethod::RegisterNative（带 is_fast 参数的版本）：
    // 将快速注册 native 的操作重定向到 backup 方法
    inline static auto RegisterNativeFast_ =
        "_ZN3art9ArtMethod14RegisterNativeEPKvb"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method, const void *native_method, bool is_fast) static -> void {
            return backup(thiz, MayGetBackup(method), native_method, is_fast);
        };

    // Hook ArtMethod::UnregisterNative（带 is_fast 参数的版本）：
    // 将快速注销 native 的操作重定向到 backup 方法
    inline static auto UnregisterNativeFast_ =
        "_ZN3art9ArtMethod16UnregisterNativeEv"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method) static -> void{
            return backup(thiz, MayGetBackup(method));
        };

    // Hook ArtMethod::RegisterNative（标准版本）：
    // 将注册 native 方法的操作重定向到 backup 方法
    inline static auto RegisterNative_ =
        "_ZN3art9ArtMethod14RegisterNativeEPKv"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method, const void *native_method) static -> const void * {
            return backup(thiz, MayGetBackup(method), native_method);
        };

    // Hook ArtMethod::UnregisterNative（标准版本）：
    // 将注销 native 方法的操作重定向到 backup 方法
    inline static auto UnregisterNative_ =
        "_ZN3art9ArtMethod16UnregisterNativeEv"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ArtMethod *method) static -> const void * {
            return backup(thiz, MayGetBackup(method));
        };

    // Hook ClassLinker::RegisterNative（通过 ClassLinker 注册的版本）：
    // 将注册 native 的操作重定向到 backup 方法
    inline static auto RegisterNativeClassLinker_ =
        "_ZN3art11ClassLinker14RegisterNativeEPNS_6ThreadEPNS_9ArtMethodEPKv"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, Thread *self, ArtMethod *method, const void *native_method) static -> const void *{
            return backup(thiz, self, MayGetBackup(method), native_method);
        };

    // Hook ClassLinker::UnregisterNative（通过 ClassLinker 注销的版本）：
    // 将注销 native 的操作重定向到 backup 方法
    inline static auto UnregisterNativeClassLinker_ =
        "_ZN3art11ClassLinker16UnregisterNativeEPNS_6ThreadEPNS_9ArtMethodE"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, Thread *self, ArtMethod *method) static -> const void * {
            return backup(thiz, self, MayGetBackup(method));
        };

    // 在 FixupStaticTrampolines 执行后恢复 backup：
    // 遍历之前保存的 backup 方法，如果 entry point 被修改则更新 hook 的 backup 方法的入口，
    // 如果方法已被去优化则需要重新去优化
    static void RestoreBackup(const dex::ClassDef *class_def, art::Thread *self) {
        auto methods = mirror::Class::PopBackup(class_def, self);
        for (const auto &[art_method, old_trampoline] : methods) {
            auto new_trampoline = art_method->GetEntryPoint();
            art_method->SetEntryPoint(old_trampoline);
            auto deoptimized = IsDeoptimized(art_method);
            auto backup_method = IsHooked(art_method);
            if (backup_method) {
                // If deoptimized, the backup entrypoint should be already set to interpreter
                if (!deoptimized && new_trampoline != old_trampoline) [[unlikely]] {
                    LOGV("propagate entrypoint for orig %p backup %p", art_method, backup_method);
                    backup_method->SetEntryPoint(new_trampoline);
                }
            } else if (deoptimized) {
                if (new_trampoline != &art_quick_to_interpreter_bridge_ && !art_method->IsNative()) {
                    LOGV("re-deoptimize for %p", art_method);
                    SetEntryPointsToInterpreter(art_method);
                }
            }
        }
    }

    // Hook ClassLinker::FixupStaticTrampolines（不带 Thread 参数的版本）：
    // 在类的静态方法 trampoline 修复完成后，调用 RestoreBackup 恢复被 hook 方法的 entry point
    inline static auto FixupStaticTrampolines_ =
        "_ZN3art11ClassLinker22FixupStaticTrampolinesENS_6ObjPtrINS_6mirror5ClassEEE"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, ObjPtr<mirror::Class> mirror_class) static -> void {
            backup(thiz, mirror_class);
            RestoreBackup(mirror_class->GetClassDef(), nullptr);
        };

    // Hook ClassLinker::FixupStaticTrampolines（带 Thread 参数的版本）：
    // x86 架构使用 naked 函数手动处理调用约定差异（stdcall vs thiscall），
    // 其他架构直接调用原始函数后执行 RestoreBackup
    inline static auto FixupStaticTrampolinesWithThread_ =
        "_ZN3art11ClassLinker22FixupStaticTrampolinesEPNS_6ThreadENS_6ObjPtrINS_6mirror5ClassEEE"_sym
            .hook
            ->*[] consteval {
                    if constexpr (is_arch_v<Arch::kX86>) {
                        return []<MemBackup auto backup> [[gnu::naked]] (
                                   ClassLinker * thiz, Thread * self,
                                   ObjPtr<mirror::Class> mirror_class) static {
                            asm volatile(R"(
                                pushl   %eax
                                pushl   %ebx
                                pushl   %ecx
                                pushl   %edx
                                pushl   %esi
                                pushl   %edi
                                pushl   %ebp

                                calll   1f
                            1:
                                popl    %eax
                                addl    $_GLOBAL_OFFSET_TABLE_+[.-1b], %eax
                                movl    lsplant_bridge_fixup_static_trampolines@GOT(%eax), %eax
                                movl    (%eax), %eax

                                movl    36(%esp), %ebx
                                movl    %gs:0, %edx
                                movl    28(%edx), %edx
                                cmpl    %ebx, %edx
                                je      .L.stdcall

                                movl    32(%esp), %ebx
                                pushl   %ebx
                                calll   *%eax
                                addl    $4, %esp
                                movl    12(%esp), %ebx
                                movl    32(%esp), %ecx
                                jmp     .L.restore_backup

                            .L.stdcall:
                                movl    40(%esp), %ebx
                                pushl   %ebx
                                movl    40(%esp), %ebx
                                pushl   %ebx
                                movl    40(%esp), %ebx
                                pushl   %ebx
                                calll   *%eax
                                addl    $12, %esp
                                movl    36(%esp), %ebx
                                movl    40(%esp), %ecx

                            .L.restore_backup:
                                calll   2f
                            2:
                                popl    %eax
                                addl    $_GLOBAL_OFFSET_TABLE_+[.-2b], %eax
                                movl    lsplant_bridge_restore_backup@GOT(%eax), %eax
                                movl    (%eax), %eax
                                pushl   %ecx
                                pushl   %ebx
                                calll   *%eax
                                addl    $8, %esp

                                popl    %ebp
                                popl    %edi
                                popl    %esi
                                popl    %edx
                                popl    %ecx
                                popl    %ebx
                                popl    %eax
                                retl

                                .bss
                                .global lsplant_bridge_fixup_static_trampolines
                                .hidden lsplant_bridge_fixup_static_trampolines
                                .common lsplant_bridge_fixup_static_trampolines, 4, 4
                                .global lsplant_bridge_restore_backup
                                .hidden lsplant_bridge_restore_backup
                                .common lsplant_bridge_restore_backup, 4, 4
                                .previous
                            )");
                        };
                    } else {
                        return []<MemBackup auto backup>(
                                   ClassLinker *thiz, Thread *self,
                                   ObjPtr<mirror::Class> mirror_class) static -> void {
                            backup(thiz, self, mirror_class);
                            RestoreBackup(mirror_class->GetClassDef(), self);
                        };
                    }
                }();

    // Hook ClassLinker::FixupStaticTrampolines（使用原始 mirror::Class 指针的版本）：
    // 用于更早的 Android 版本，参数为裸指针而非 ObjPtr 包装
    inline static auto FixupStaticTrampolinesRaw_ =
        "_ZN3art11ClassLinker22FixupStaticTrampolinesEPNS_6mirror5ClassE"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, mirror::Class *mirror_class)static -> void {
            backup(thiz, mirror_class);
            RestoreBackup(mirror_class->GetClassDef(), nullptr);
        };

    // Hook AdjustThreadVisibilityCounter：在类可见性计数器调整后恢复 backup，
    // 因为 FixupStaticTrampolines 可能被内联，通过此 hook 确保仍能捕获到 trampoline 修复
    inline static auto AdjustThreadVisibilityCounter_ =
        ("_ZN3art11ClassLinker26VisiblyInitializedCallback29AdjustThreadVisibilityCounterEPNS_6ThreadEi"_sym |
         "_ZN3art11ClassLinker26VisiblyInitializedCallback29AdjustThreadVisibilityCounterEPNS_6ThreadEl"_sym).hook->*[]
         <MemBackup auto backup>
         (ClassLinker *thiz, Thread *self, ssize_t adjustment) static -> void {
            backup(thiz, self, adjustment);
            RestoreBackup(nullptr, self);
        };

    // Hook MarkVisiblyInitialized：在类被标记为可见初始化后恢复 backup，
    // 同样用于 FixupStaticTrampolines 被内联的场景
    inline static auto MarkVisiblyInitialized_ =
        "_ZN3art11ClassLinker26VisiblyInitializedCallback22MarkVisiblyInitializedEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (ClassLinker *thiz, Thread *self) static -> void {
            backup(thiz, self);
            RestoreBackup(nullptr, self);
        };

    // 获取方法的优化代码指针（OAT 中的 quick code）：
    // x86 架构使用 naked 桥接函数处理调用约定差异；
    // 其他架构优先使用 GetOptimizedCodeFor_，失败则回退到 GetOptimizedCodeForL_
    static void *GetOptimizedCodeFor(ArtMethod *method) {
        if constexpr (is_arch_v<Arch::kX86>) {
            extern void *(*get_optimized_code_for)(ArtMethod *)asm(
                "lsplant_bridge_get_optimized_code_for");
            get_optimized_code_for =
                GetOptimizedCodeFor_ ? &GetOptimizedCodeFor_ : &GetOptimizedCodeForL_;
            return [] [[gnu::naked]] (ArtMethod * method) static -> void * {
                asm volatile(R"(
                    pushl   %ebx
                    pushl   %ecx
                    pushl   %edx
                    pushl   %esi
                    pushl   %edi
                    pushl   %ebp

                    calll   1f
                1:
                    popl    %eax
                    addl    $_GLOBAL_OFFSET_TABLE_+[.-1b], %eax
                    movl    lsplant_bridge_get_optimized_code_for@GOT(%eax), %eax
                    movl    (%eax), %eax

                    movl    28(%esp), %ecx
                    pushl   %ecx
                    calll   *%eax
                    addl    $4, %esp

                    popl    %ebp
                    popl    %edi
                    popl    %esi
                    popl    %edx
                    popl    %ecx
                    popl    %ebx
                    retl

                    .bss
                    .global lsplant_bridge_get_optimized_code_for
                    .hidden lsplant_bridge_get_optimized_code_for
                    .common lsplant_bridge_get_optimized_code_for, 4, 4
                    .previous
                )");
            }(method);
        } else if (GetOptimizedCodeFor_) [[likely]] {
            return GetOptimizedCodeFor_(method);
        } else {
            return GetOptimizedCodeForL_(method);
        }
    }

public:
    // 初始化所有 ClassLinker 相关的 hook：
    // 根据不同 Android API 级别选择性地安装 hook，包括 entry point 保护、
    // FixupStaticTrampolines 恢复、RegisterNative 重定向、可见性变更处理等
    static bool Init(JNIEnv *env, const HookHandler &handler) {
        int sdk_int = GetAndroidApiLevel();

        if (sdk_int >= __ANDROID_API_N__ && sdk_int < __ANDROID_API_T__) {
            handler(ShouldUseInterpreterEntrypoint_);
        }

        if constexpr (is_arch_v<Arch::kX86>) {
            if (handler(FixupStaticTrampolinesWithThread_)) [[likely]] {
                extern void (*fixup_static_trampolines)(
                    ClassLinker *, Thread *,
                    ObjPtr<mirror::Class>) asm("lsplant_bridge_fixup_static_trampolines");
                extern void (*restore_backup)(
                    Thread *, ObjPtr<mirror::Class>) asm("lsplant_bridge_restore_backup");
                fixup_static_trampolines = &FixupStaticTrampolinesWithThread_;
                restore_backup = +[](Thread *self, ObjPtr<mirror::Class> mirror_class) {
                    RestoreBackup(mirror_class->GetClassDef(), self);
                };
            } else {
                handler(FixupStaticTrampolines_, FixupStaticTrampolinesRaw_);
            }
        } else {
            handler(FixupStaticTrampolinesWithThread_, FixupStaticTrampolines_,
                    FixupStaticTrampolinesRaw_);
        }

        if (!handler(RegisterNativeClassLinker_, RegisterNative_, RegisterNativeFast_,
                          RegisterNativeThread_) ||
            !handler(UnregisterNativeClassLinker_, UnregisterNative_, UnregisterNativeFast_,
                          UnregisterNativeThread_)) {
            return false;
        }

        if (sdk_int >= __ANDROID_API_R__) {
            if constexpr (!is_arch_v<Arch::kX86, Arch::kAmd64>) {
                // fixup static trampoline may have been inlined
                handler(AdjustThreadVisibilityCounter_, MarkVisiblyInitialized_);
            }
        }

        if (!handler(SetEntryPointsToInterpreter_)) [[likely]] {
            if (handler(GetOptimizedCodeFor_, GetOptimizedCodeForL_, true)) [[likely]] {
                auto obj = JNI_FindClass(env, "java/lang/Object");
                if (!obj) {
                    return false;
                }
                auto method = JNI_GetMethodID(env, obj, "equals", "(Ljava/lang/Object;)Z");
                if (!method) {
                    return false;
                }
                auto dummy = ArtMethod::FromReflectedMethod(
                        env, JNI_ToReflectedMethod(env, obj, method, false).get())->Clone();
                JavaDebuggableGuard guard;
                // just in case
                dummy->SetNonNative();
                art_quick_to_interpreter_bridge_ = GetOptimizedCodeFor(dummy.get());
            } else if (!handler(art_quick_to_interpreter_bridge_)) [[unlikely]] {
                return false;
            }
            LOGD("art_quick_to_interpreter_bridge = %p", &art_quick_to_interpreter_bridge_);
        }
        return true;
    }

    // 去优化指定方法：将其 entry point 替换为解释器桥接入口，使其强制走解释执行路径。
    // 优先使用 SetEntryPointsToInterpreter_ 原始函数，Android 13+ 回退到手动设置 art_quick_to_interpreter_bridge_
    [[gnu::always_inline]] static bool SetEntryPointsToInterpreter(ArtMethod *art_method) {
        if (art_method->IsNative()) {
            return false;
        }
        if (SetEntryPointsToInterpreter_) [[likely]] {
            SetEntryPointsToInterpreter_(nullptr, art_method);
            return true;
        }
        // Android 13
        if (art_quick_to_interpreter_bridge_) [[likely]] {
            LOGV("deoptimize method %s from %p to %p", art_method->PrettyMethod(true).data(),
                 art_method->GetEntryPoint(), &art_quick_to_interpreter_bridge_);
            art_method->SetEntryPoint(
                reinterpret_cast<void *>(&art_quick_to_interpreter_bridge_));
            return true;
        }
        return false;
    }
};
}  // namespace lsplant::art
