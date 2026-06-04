module;

#include <atomic>
#include <memory>
#include <string>

#include "logging.hpp"

export module lsplant:art_method;

import :common;
import hook_helper;

export namespace lsplant::art {

// QuickMethodFrameInfo: 描述一个 Quick ABI 栈帧的布局信息，包括帧大小和寄存器掩码
struct alignas(4) [[gnu::packed]] QuickMethodFrameInfo {
    [[maybe_unused]] uint32_t frame_size_in_bytes;  // 栈帧大小（字节）
    [[maybe_unused]] uint32_t core_spill_mask;      // 核心寄存器（整数寄存器）的保存掩码
    [[maybe_unused]] uint32_t fp_spill_mask;        // 浮点寄存器的保存掩码
};

// ArtMethod: 对应 ART 虚拟机内部的 art::ArtMethod 结构，用于操作方法的元数据与入口点
class ArtMethod {
    inline static ArtMethod *abstract_method_{};  // Android M 上用于获取抽象方法的指针，用于 GetQuickFrameInfo hook 回退

    // 符号引用：ArtMethod::SetNotIntrinsic()，用于清除方法的内联缓存标记
    inline static auto SetNotIntrinsic_ =
        "_ZN3art9ArtMethod15SetNotIntrinsicEv"_sym.as<void (ArtMethod::*)()>;

    // 符号引用：成员版 PrettyMethod，将 ArtMethod 转为可读字符串（如 "Class.method()"）
    inline static auto PrettyMethod_ =
            "_ZN3art9ArtMethod12PrettyMethodEPS0_b"_sym.as<std::string(ArtMethod::*)(bool)>;

    // 符号引用：静态版 PrettyMethod，用于部分 Android 版本的兼容
    inline static auto PrettyMethodStatic_ =
            "_ZN3art12PrettyMethodEPNS_9ArtMethodEb"_sym.as<std::string(ArtMethod *thiz, bool with_signature)>;

    // 符号引用：mirror 版 PrettyMethod，用于另一些 Android 版本的兼容
    inline static auto PrettyMethodMirror_ =
            "_ZN3art12PrettyMethodEPNS_6mirror9ArtMethodEb"_sym.as<std::string(ArtMethod *thiz, bool with_signature)>;

    // 符号引用：L 版命名空间下的 GetMethodShorty，获取方法短签名（如 "VIL"）
    inline static auto GetMethodShortyL_ =
            "_ZN3artL15GetMethodShortyEP7_JNIEnvP10_jmethodID"_sym.as<const char *(JNIEnv *env, jmethodID method)>;

    // 符号引用：标准 GetMethodShorty，功能同上但符号名不同
    inline static auto GetMethodShorty_ =
            "_ZN3art15GetMethodShortyEP7_JNIEnvP10_jmethodID"_sym.as<const char *(JNIEnv *env, jmethodID mid)>;

    // 符号引用：ThrowInvocationTimeError，在 O 版本上用于触发运行时调用错误
    inline static auto ThrowInvocationTimeError_ =
            "_ZN3art9ArtMethod24ThrowInvocationTimeErrorEv"_sym.as<void(ArtMethod::*)()>;

    // 符号引用：解释器到编译代码的桥接函数，设置解释器入口点时重定向到编译代码
    inline static auto art_interpreter_to_compiled_code_bridge_ =
            "artInterpreterToCompiledCodeBridge"_sym.as<void()>;

    // Hook: GetQuickFrameInfo，对代理方法的栈帧查询回退到抽象方法以避免崩溃
    inline static auto GetQuickFrameInfo_ =
        "_ZN3art9ArtMethod17GetQuickFrameInfoEv"_sym.hook->*
        []<MemBackup auto backup>(ArtMethod *thiz) static -> QuickMethodFrameInfo {
        if (backuped_proxy_methods_.contains(thiz)) [[unlikely]] {
            return backup(abstract_method_);
        }
        return backup(thiz);
    };

    // 调用 ART 内部的 ThrowInvocationTimeError，用于 O 版本探测 kAccCompileDontBother 标记
    inline void ThrowInvocationTimeError() {
        if (ThrowInvocationTimeError_) {
            [[likely]] ThrowInvocationTimeError_(this);
        }
    }

public:
    // 获取方法的短类型签名（如 "VIL" 表示 void(int, long)），兼容 L 版和非 L 版符号
    inline static const char *GetMethodShorty(JNIEnv *env, jobject method) {
        if (GetMethodShortyL_) {
            return GetMethodShortyL_(env, env->FromReflectedMethod(method));
        }
        return GetMethodShorty_(env, env->FromReflectedMethod(method));
    }

    // 标记方法为不可编译，防止 JIT/AOT 对该方法进行优化，避免 hook 被覆盖
    void SetNonCompilable() {
        auto access_flags = GetAccessFlags();
        access_flags |= kAccCompileDontBother;
        access_flags &= ~kAccPreCompiled;
        SetAccessFlags(access_flags);
    }

    // 清除快速解释器间调用标记，确保方法调用走标准路径而非快速路径
    void ClearFastInterpretFlag() {
        auto access_flags = GetAccessFlags();
        access_flags &= ~kAccFastInterpreterToInterpreterInvoke;
        SetAccessFlags(access_flags);
    }

    // 将方法的访问权限设为 private
    void SetPrivate() {
        auto access_flags = GetAccessFlags();
        access_flags |= kAccPrivate;
        access_flags &= ~kAccProtected;
        access_flags &= ~kAccPublic;
        SetAccessFlags(access_flags);
    }

    // 将方法的访问权限设为 public
    void SetPublic() {
        auto access_flags = GetAccessFlags();
        access_flags |= kAccPublic;
        access_flags &= ~kAccProtected;
        access_flags &= ~kAccPrivate;
        SetAccessFlags(access_flags);
    }

    // 将方法的访问权限设为 protected
    void SetProtected() {
        auto access_flags = GetAccessFlags();
        access_flags |= kAccProtected;
        access_flags &= ~kAccPrivate;
        access_flags &= ~kAccPublic;
        SetAccessFlags(access_flags);
    }

    // 清除 final 标记，使方法可被继承/重写
    void SetNonFinal() {
        auto access_flags = GetAccessFlags();
        access_flags &= ~kAccFinal;
        SetAccessFlags(access_flags);
    }

    // 将方法标记为 native，表示由本地代码实现
    void SetNative() {
        auto access_flags = GetAccessFlags();
        access_flags |= kAccNative;
        SetAccessFlags(access_flags);
    }

    // 清除 native 标记，恢复为非本地方法
    void SetNonNative() {
        auto access_flags = GetAccessFlags();
        access_flags &= ~kAccNative;
        SetAccessFlags(access_flags);
    }

    // 清除内联缓存（intrinsic）标记，防止 ART 将该方法作为内建函数优化
    void SetNonIntrinsic() {
        if (SetNotIntrinsic_) [[likely]] {
            SetNotIntrinsic_(this);
        } else if (kAccIntrinsic) [[likely]] {
            auto access_flags = GetAccessFlags();
            access_flags &= ~kAccIntrinsic;
            SetAccessFlags(access_flags);
        }
    }

    // 判断方法是否为 private
    bool IsPrivate() { return GetAccessFlags() & kAccPrivate; }
    // 判断方法是否为 protected
    bool IsProtected() { return GetAccessFlags() & kAccProtected; }
    // 判断方法是否为 public
    bool IsPublic() { return GetAccessFlags() & kAccPublic; }
    // 判断方法是否为 final
    bool IsFinal() { return GetAccessFlags() & kAccFinal; }
    // 判断方法是否为 static
    bool IsStatic() { return GetAccessFlags() & kAccStatic; }
    // 判断方法是否为 native
    bool IsNative() { return GetAccessFlags() & kAccNative; }
    // 判断方法是否为 abstract
    bool IsAbstract() { return GetAccessFlags() & kAccAbstract; }
    // 判断方法是否为构造函数（<init> 或 <clinit>）
    bool IsConstructor() { return GetAccessFlags() & kAccConstructor; }

    // 从另一个 ArtMethod 拷贝全部字段到当前对象，用于备份和还原方法
    void CopyFrom(const ArtMethod *other) { memcpy(this, other, art_method_size); }

    // 设置方法的入口点（Quick 编译代码入口），同时将解释器入口设为桥接函数以保持一致性
    void SetEntryPoint(void *entry_point) {
        *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(this) + entry_point_offset) =
            entry_point;
        if (interpreter_entry_point_offset) [[unlikely]] {
            *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(this) +
                                       interpreter_entry_point_offset) =
                reinterpret_cast<void *>(&art_interpreter_to_compiled_code_bridge_);
        }
    }

    // 获取方法的 Quick 编译代码入口点
    void *GetEntryPoint() {
        return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(this) + entry_point_offset);
    }

    // 获取方法的 data 字段（通常存储 JNI 函数指针或 ProfilingInfo）
    void *GetData() {
        return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(this) + data_offset);
    }

    // 设置方法的 data 字段
    void SetData(void *data) {
        *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(this) + data_offset) = data;
    }

    // 以原子方式读取方法的 access_flags，使用 relaxed 内存序以减少开销
    uint32_t GetAccessFlags() {
        return (reinterpret_cast<const std::atomic<uint32_t> *>(reinterpret_cast<uintptr_t>(this) +
                                                                access_flags_offset))
            ->load(std::memory_order_relaxed);
    }

    // 以原子方式写入方法的 access_flags
    void SetAccessFlags(uint32_t flags) {
        return (reinterpret_cast<std::atomic<uint32_t> *>(reinterpret_cast<uintptr_t>(this) +
                                                          access_flags_offset))
            ->store(flags, std::memory_order_relaxed);
    }

    // 将方法转为人类可读的字符串描述（类名.方法名(签名)），兼容不同 Android 版本的符号
    std::string PrettyMethod(bool with_signature = true) {
        if (PrettyMethod_) [[likely]]
            return PrettyMethod_(this, with_signature);
        if (PrettyMethodStatic_) return PrettyMethodStatic_(this, with_signature);
        if (PrettyMethodMirror_) return PrettyMethodMirror_(this, with_signature);
        return "null sym";
    }

    // 获取声明该方法的类（mirror::Class 指针）
    mirror::Class *GetDeclaringClass() {
        return reinterpret_cast<mirror::Class *>(*reinterpret_cast<uint32_t *>(
            reinterpret_cast<uintptr_t>(this) + declaring_class_offset));
    }

    // 在堆上分配并拷贝一个新的 ArtMethod 副本，调用者获得唯一所有权
    std::unique_ptr<ArtMethod> Clone() {
        auto *method = reinterpret_cast<ArtMethod*>(::operator new(art_method_size));
        method->CopyFrom(this);
        return std::unique_ptr<ArtMethod>(method);
    }

    // 将当前方法备份到指定位置，同时标记为不可编译、清除快速解释标记、非静态方法设为 private
    void BackupTo(ArtMethod *backup) {
        SetNonCompilable();

        // copy after setNonCompilable
        backup->CopyFrom(this);

        ClearFastInterpretFlag();

        if (!backup->IsStatic()) backup->SetPrivate();
    }

    // 从 Java 反射对象（Executable/Method/Constructor）获取对应的 ART 内部 ArtMethod 指针
    // Android M+ 通过 artMethod long 字段获取，旧版本通过 JNI FromReflectedMethod 获取
    static art::ArtMethod *FromReflectedMethod(JNIEnv *env, jobject method) {
        if (art_method_field) [[likely]] {
            return reinterpret_cast<art::ArtMethod *>(
                JNI_GetLongField(env, method, art_method_field));
        } else {
            return reinterpret_cast<art::ArtMethod *>(env->FromReflectedMethod(method));
        }
    }

    // 初始化 ArtMethod 的各种偏移量和符号引用，通过 JNI 反射和符号查找确定运行时布局
    static bool Init(JNIEnv *env, const HookHandler handler) {
        auto sdk_int = GetAndroidApiLevel();
        ScopedLocalRef<jclass> executable{env, nullptr};
        if (sdk_int >= __ANDROID_API_O__) {
            executable = JNI_FindClass(env, "java/lang/reflect/Executable");
        } else if (sdk_int >= __ANDROID_API_M__) {
            executable = JNI_FindClass(env, "java/lang/reflect/AbstractMethod");
        } else {
            executable = JNI_FindClass(env, "java/lang/reflect/ArtMethod");
        }
        if (!executable) {
            LOGE("Failed to find Executable/AbstractMethod/ArtMethod");
            return false;
        }

        if (sdk_int >= __ANDROID_API_M__) [[likely]] {
            if (art_method_field = JNI_GetFieldID(env, executable, "artMethod", "J");
                !art_method_field) {
                LOGE("Failed to find artMethod field");
                return false;
            }
        }

        auto throwable = JNI_FindClass(env, "java/lang/Throwable");
        if (!throwable) {
            LOGE("Failed to find Throwable");
            return false;
        }
        auto clazz = JNI_FindClass(env, "java/lang/Class");
        static_assert(std::is_same_v<decltype(clazz)::BaseType, jclass>);
        jmethodID get_declared_constructors = JNI_GetMethodID(env, clazz, "getDeclaredConstructors",
                                                              "()[Ljava/lang/reflect/Constructor;");
        const auto constructors =
            JNI_Cast<jobjectArray>(JNI_CallObjectMethod(env, throwable, get_declared_constructors));
        if (constructors.size() < 2) {
            LOGE("Throwable has less than 2 constructors");
            return false;
        }
        auto first_ctor = constructors[0];
        auto second_ctor = constructors[1];
        auto *first = FromReflectedMethod(env, first_ctor.get());
        auto *second = FromReflectedMethod(env, second_ctor.get());
        art_method_size = reinterpret_cast<uintptr_t>(second) - reinterpret_cast<uintptr_t>(first);
        LOGD("ArtMethod size: %zu", art_method_size);

        if (RoundUpTo(4 * 9, kPointerSize) + kPointerSize * 3 < art_method_size) [[unlikely]] {
            if (sdk_int >= __ANDROID_API_M__) {
                LOGW("ArtMethod size exceeds maximum assume. There may be something wrong.");
            }
        }

        entry_point_offset = art_method_size - kPointerSize;
        data_offset = entry_point_offset - kPointerSize;

        if (sdk_int >= __ANDROID_API_M__) [[likely]] {
            if (auto access_flags_field = JNI_GetFieldID(env, executable, "accessFlags", "I");
                access_flags_field) {
                uint32_t real_flags = JNI_GetIntField(env, first_ctor, access_flags_field);
                for (size_t i = 0; i < art_method_size; i += sizeof(uint32_t)) {
                    if (*reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(first) + i) ==
                        real_flags) {
                        access_flags_offset = i;
                        break;
                    }
                }
            }
            if (access_flags_offset == 0) {
                LOGW("Failed to find accessFlags field. Fallback to 4.");
                access_flags_offset = 4U;
            }
        } else {
            auto art_field = JNI_FindClass(env, "java/lang/reflect/ArtField");
            auto field = JNI_FindClass(env, "java/lang/reflect/Field");
            auto art_field_field =
                JNI_GetFieldID(env, field, "artField", "Ljava/lang/reflect/ArtField;");
            auto field_offset = JNI_GetFieldID(env, art_field, "offset", "I");
            auto get_offset_from_art_method = [&](const char *name, const char *sig) {
                return JNI_GetIntField(
                    env,
                    JNI_GetObjectField(
                        env,
                        JNI_ToReflectedField(env, executable,
                                             JNI_GetFieldID(env, executable, name, sig), false),
                        art_field_field),
                    field_offset);
            };
            access_flags_offset = get_offset_from_art_method("accessFlags", "I");
            declaring_class_offset =
                get_offset_from_art_method("declaringClass", "Ljava/lang/Class;");
            if (sdk_int == __ANDROID_API_L__) {
                entry_point_offset =
                    get_offset_from_art_method("entryPointFromQuickCompiledCode", "J");
                interpreter_entry_point_offset =
                    get_offset_from_art_method("entryPointFromInterpreter", "J");
                data_offset = get_offset_from_art_method("entryPointFromJni", "J");
            }
        }
        LOGD("ArtMethod::declaring_class offset: %zu", declaring_class_offset);
        LOGD("ArtMethod::entrypoint offset: %zu", entry_point_offset);
        LOGD("ArtMethod::data offset: %zu", data_offset);
        LOGD("ArtMethod::access_flags offset: %zu", access_flags_offset);

        if (sdk_int < __ANDROID_API_R__) {
            kAccPreCompiled = 0;
        } else if (sdk_int >= __ANDROID_API_S__) {
            kAccPreCompiled = 0x00800000;
        }
        if (sdk_int < __ANDROID_API_Q__) kAccFastInterpreterToInterpreterInvoke = 0;
        if (sdk_int < __ANDROID_API_O__) kAccIntrinsic = 0;

        if (sdk_int >= __ANDROID_API_P__ && !handler(SetNotIntrinsic_)) {
            LOGW("Failed to find SetNotIntrinsic, use hard-coded kAccIntrinsic instead");
        }

        if (!handler(GetMethodShortyL_, true, GetMethodShorty_)) {
            LOGE("Failed to find GetMethodShorty");
            return false;
        }

        handler(PrettyMethod_, PrettyMethodStatic_, PrettyMethodMirror_);

        if (sdk_int <= __ANDROID_API_O__) [[unlikely]] {
            auto abstract_method_error = JNI_FindClass(env, "java/lang/AbstractMethodError");
            if (!abstract_method_error) {
                LOGE("Failed to find AbstractMethodError");
                return false;
            }
            if (sdk_int == __ANDROID_API_O__) [[unlikely]] {
                auto executable_get_name =
                    JNI_GetMethodID(env, executable, "getName", "()Ljava/lang/String;");
                if (!executable_get_name) {
                    LOGE("Failed to find Executable.getName");
                    return false;
                }
                handler(ThrowInvocationTimeError_);
                auto abstract_method = FromReflectedMethod(
                    env, JNI_ToReflectedMethod(env, executable, executable_get_name, false).get());
                uint32_t access_flags = abstract_method->GetAccessFlags();
                abstract_method->SetAccessFlags(access_flags | kAccDefaultConflict);
                abstract_method->ThrowInvocationTimeError();
                abstract_method->SetAccessFlags(access_flags);
            }
            if (auto exception = env->ExceptionOccurred();
                env->ExceptionClear(),
                (!exception || JNI_IsInstanceOf(env, exception, abstract_method_error)))
                [[likely]] {
                kAccCompileDontBother = kAccDefaultConflict;
            }
        }
        if (sdk_int == __ANDROID_API_M__) [[unlikely]] {
            auto executable_get_name =
                JNI_GetMethodID(env, executable, "getName", "()Ljava/lang/String;");
            if (!executable_get_name) {
                LOGE("Failed to find Executable.getName");
                return false;
            }
            abstract_method_ = FromReflectedMethod(
                env, JNI_ToReflectedMethod(env, executable, executable_get_name, false).get());
            if (!abstract_method_ || !abstract_method_->IsAbstract()) [[unlikely]] {
                LOGW("Abstract method Executable.getName not found");
            } else if (!handler(GetQuickFrameInfo_)) [[unlikely]] {
                LOGW("Failed to hook GetQuickFrameInfo, hooking proxy method may crash");
            }
        }
        if (sdk_int < __ANDROID_API_N__) {
            kAccCompileDontBother = 0;
        }
        if (sdk_int <= __ANDROID_API_M__) [[unlikely]] {
            if (!handler(art_interpreter_to_compiled_code_bridge_)) {
                return false;
            }
            if (sdk_int >= __ANDROID_API_L_MR1__) {
                interpreter_entry_point_offset = entry_point_offset - 2 * kPointerSize;
            }
        }

        return true;
    }

    // 返回 entry_point 字段在 ArtMethod 结构中的偏移量，供外部模块使用
    static size_t GetEntryPointOffset() { return entry_point_offset; }

    // 公开访问标志常量，对应 ART 内部的 access_flags 位域
    constexpr static uint32_t kAccPublic = 0x0001;           // 方法/字段/类公开访问
    constexpr static uint32_t kAccPrivate = 0x0002;          // 方法/字段私有访问
    constexpr static uint32_t kAccProtected = 0x0004;        // 方法/字段受保护访问
    constexpr static uint32_t kAccStatic = 0x0008;           // 静态成员
    constexpr static uint32_t kAccNative = 0x0100;           // 本地方法（JNI 实现）
    constexpr static uint32_t kAccFinal = 0x0010;            // 不可重写/覆盖
    constexpr static uint32_t kAccAbstract = 0x0400;         // 抽象方法，无方法体
    constexpr static uint32_t kAccConstructor = 0x00010000;  // 构造函数标记（<init>/<clinit>）

private:
    // 以下为运行时通过符号查找和 JNI 反射确定的偏移量和标记值
    inline static jfieldID art_method_field = nullptr;        // Java 层 Executable.artMethod 字段 ID
    inline static size_t art_method_size = 0;                 // ArtMethod 结构的实际大小（字节）
    inline static size_t entry_point_offset = 0;              // entry_point 字段偏移
    inline static size_t interpreter_entry_point_offset = 0;  // 解释器入口点偏移（仅旧版本有）
    inline static size_t data_offset = 0;                     // data 字段偏移（JNI/native 数据）
    inline static size_t access_flags_offset = 0;             // access_flags 字段偏移
    inline static size_t declaring_class_offset = 0;          // declaring_class 字段偏移
    inline static uint32_t kAccFastInterpreterToInterpreterInvoke = 0x40000000;  // 快速解释器间调用优化标记
    inline static uint32_t kAccPreCompiled = 0x00200000;       // 预编译标记，Android R+ 有效
    inline static uint32_t kAccCompileDontBother = 0x02000000; // 禁止编译标记，阻止 JIT/AOT 编译
    inline static uint32_t kAccDefaultConflict = 0x01000000;   // 默认方法冲突标记
    inline static uint32_t kAccIntrinsic = 0x80000000;         // 内建函数标记，ART 会特殊优化
};

}  // namespace lsplant::art
