module;

#include <jni.h>
#include <parallel_hashmap/phmap.h>
#include <sys/system_properties.h>

#include <list>
#include <shared_mutex>
#include <string_view>

#include "logging.hpp"

export module lsplant:common;
export import jni_helper;
export import hook_helper;
export import type_traits;

export namespace lsplant {

namespace art {
class ArtMethod;
namespace mirror {
class Class;
}
namespace dex {
class ClassDef {};
}  // namespace dex

}  // namespace art

// 线程安全的哈希表类型别名，内部使用读写锁（shared_mutex）支持并发访问
template <class K, class V, class Hash = phmap::priv::hash_default_hash<K>,
          class Eq = phmap::priv::hash_default_eq<K>,
          class Alloc = phmap::priv::Allocator<phmap::priv::Pair<const K, V>>, size_t N = 4>
using SharedHashMap = phmap::parallel_flat_hash_map<K, V, Hash, Eq, Alloc, N, std::shared_mutex>;

// 线程安全的哈希集合类型别名，与 SharedHashMap 类似但只存储键
template <class T, class Hash = phmap::priv::hash_default_hash<T>,
          class Eq = phmap::priv::hash_default_eq<T>, class Alloc = phmap::priv::Allocator<T>,
          size_t N = 4>
using SharedHashSet = phmap::parallel_flat_hash_set<T, Hash, Eq, Alloc, N, std::shared_mutex>;

// 将值 v 向上对齐到 size 的整数倍，利用位运算避免除法
template <typename T>
constexpr inline auto RoundUpTo(T v, size_t size) {
    return v + size - 1 - ((v + size - 1) & (size - 1));
}

// 获取当前 Android 系统的 API 级别（SDK 版本号），通过读取系统属性 ro.build.version.sdk 实现，结果仅初始化一次
[[gnu::const]] inline auto GetAndroidApiLevel() {
    static auto kApiLevel = [] {
        std::array<char, PROP_VALUE_MAX> prop_value;
        __system_property_get("ro.build.version.sdk", prop_value.data());
        return atoi(prop_value.data());
    }();
    [[assume(kApiLevel >= __ANDROID_API__)]];
    return kApiLevel;
}

// 检测当前 Java 运行时是否处于可调试模式（isJavaDebuggable），仅在 Android P 及以上版本有效
inline auto IsJavaDebuggable(JNIEnv * env) {
    static auto kDebuggable = [&env]() {
        auto sdk_int = GetAndroidApiLevel();
        if (sdk_int < __ANDROID_API_P__) {
            return false;
        }
        auto runtime_class = JNI_FindClass(env, "dalvik/system/VMRuntime");
        if (!runtime_class) {
            LOGE("Failed to find VMRuntime");
            return false;
        }
        auto get_runtime_method = JNI_GetStaticMethodID(env, runtime_class, "getRuntime",
                                                        "()Ldalvik/system/VMRuntime;");
        if (!get_runtime_method) {
            LOGE("Failed to find VMRuntime.getRuntime()");
            return false;
        }
        auto is_debuggable_method =
            JNI_GetMethodID(env, runtime_class, "isJavaDebuggable", "()Z");
        if (!is_debuggable_method) {
            LOGE("Failed to find VMRuntime.isJavaDebuggable()");
            return false;
        }
        auto runtime = JNI_CallStaticObjectMethod(env, runtime_class, get_runtime_method);
        if (!runtime) {
            LOGE("Failed to get VMRuntime");
            return false;
        }
        bool is_debuggable = JNI_CallBooleanMethod(env, runtime, is_debuggable_method);
        LOGD("java runtime debuggable %s", is_debuggable ? "true" : "false");
        return is_debuggable;
    }();
    return kDebuggable;
}

// 当前平台指针大小（32 位为 4，64 位为 8）
constexpr auto kPointerSize = sizeof(void *);

// 记录所有已 hook 的方法：键为目标方法，值为 (Java 层备份引用, ART 层备份方法)
SharedHashMap<art::ArtMethod *, std::pair<jobject, art::ArtMethod *>> hooked_methods_;

// 按类（ClassDef）分组记录已 hook 的方法，用于后续按类批量管理
SharedHashMap<const art::dex::ClassDef *, phmap::flat_hash_set<art::ArtMethod *>>
    hooked_classes_;

// 记录所有已去优化（deoptimize）的方法集合
SharedHashSet<art::ArtMethod *> deoptimized_methods_set_;

// 按类（ClassDef）分组记录已去优化的方法
SharedHashMap<const art::dex::ClassDef *, phmap::flat_hash_set<art::ArtMethod *>>
    deoptimized_classes_;

// 记录代理类的备份方法，代理类需要特殊处理
SharedHashSet<art::ArtMethod *> backuped_proxy_methods_;

// 记录 JIT 编译导致的方法地址移动，以便 hook 后能正确跟踪原始方法
std::list<std::pair<art::ArtMethod *, art::ArtMethod *>> jit_movements_;
// 保护 jit_movements_ 列表的读写锁
std::shared_mutex jit_movements_lock_;

// 检查指定方法是否已被 hook，若是则返回其备份方法指针；including_backup 为 true 时即使只有备份记录也返回
inline art::ArtMethod *IsHooked(art::ArtMethod * art_method, bool including_backup = false) {
    art::ArtMethod *backup = nullptr;
    hooked_methods_.if_contains(art_method, [&backup, &including_backup](const auto &it) {
        if (including_backup || it.second.first) backup = it.second.second;
    });
    return backup;
}

// 检查指定方法是否是某个 hook 的备份方法，若是则返回其原始目标方法指针
inline art::ArtMethod *IsBackup(art::ArtMethod * art_method) {
    art::ArtMethod *backup = nullptr;
    hooked_methods_.if_contains(art_method, [&backup](const auto &it) {
        if (!it.second.first) backup = it.second.second;
    });
    return backup;
}

// 检查指定方法是否已被去优化（强制走解释执行路径）
inline bool IsDeoptimized(art::ArtMethod * art_method) {
    return deoptimized_methods_set_.contains(art_method);
}

// 获取并清空 JIT 方法移动记录列表，返回所有已记录的 (原始方法, 备份方法) 对
inline std::list<std::pair<art::ArtMethod *, art::ArtMethod *>> GetJitMovements() {
    std::unique_lock lk(jit_movements_lock_);
    return std::move(jit_movements_);
}

// 记录一个已 hook 的方法：将目标方法和备份方法的映射关系存入 hooked_methods_ 和 hooked_classes_
inline void RecordHooked(art::ArtMethod * target, const art::dex::ClassDef *class_def,
                         jobject reflected_backup, art::ArtMethod *backup) {
    hooked_classes_.lazy_emplace_l(
        class_def, [&target](auto &it) { it.second.emplace(target); },
        [&class_def, &target](const auto &ctor) {
            ctor(class_def, phmap::flat_hash_set<art::ArtMethod *>{target});
        });
    hooked_methods_.insert({std::make_pair(target, std::make_pair(reflected_backup, backup)),
                            std::make_pair(backup, std::make_pair(nullptr, target))});
}

// 记录一个已去优化的方法，同时存入方法集合和按类分组的映射中
inline void RecordDeoptimized(const art::dex::ClassDef *class_def, art::ArtMethod *art_method) {
    { deoptimized_classes_[class_def].emplace(art_method); }
    deoptimized_methods_set_.insert(art_method);
}

// 记录一次 JIT 编译导致的方法地址移动（目标方法和备份方法的对应关系）
inline void RecordJitMovement(art::ArtMethod * target, art::ArtMethod * backup) {
    std::unique_lock lk(jit_movements_lock_);
    jit_movements_.emplace_back(target, backup);
}
}  // namespace lsplant
