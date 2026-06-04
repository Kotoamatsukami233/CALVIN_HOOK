module;

#include <memory>
#include <string>
#include <vector>

#include "logging.hpp"

export module lsplant:dex_file;

import :common;
import hook_helper;

namespace lsplant::art {
export class DexFile {
    // DEX 文件头部的精简版本，仅包含 magic 和 checksum 字段，
    // 用于从 DEX 数据中提取校验和传给 OpenMemory
    struct Header {
        [[maybe_unused]] uint8_t magic_[8];
        uint32_t checksum_;  // See also location_checksum_
    };

    // OpenMemory 的标准版本（返回 unique_ptr<DexFile>），用于 Android M 之前
    inline static auto OpenMemory_ =
        ("_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_10OatDexFileEPS9_"_sym |
         "_ZN3art7DexFile10OpenMemoryEPKhmRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_10OatDexFileEPS9_"_sym).as<
        std::unique_ptr<DexFile>(const uint8_t* dex_file, size_t size, const std::string& location,
                                 uint32_t location_checksum, void* mem_map,
                                 const void* oat_dex_file, std::string* error_msg)>;

    // OpenMemory 的原始指针版本（返回 const DexFile*，参数为 OatFile），
    // 用于某些 Android 版本中 OatDexFile 参数被替换为 OatFile 的情况
    inline static auto OpenMemoryRaw_ =
        ("_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_7OatFileEPS9_"_sym |
         "_ZN3art7DexFile10OpenMemoryEPKhmRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_7OatFileEPS9_"_sym).as<
        const DexFile*(const uint8_t* dex_file, size_t size, const std::string& location,
                       uint32_t location_checksum, void* mem_map, const void* oat_dex_file,
                       std::string* error_msg)>;

    // OpenMemory 的无 OAT 关联版本（无 oat_dex_file/oat_file 参数），
    // 用于没有预编译 OAT 文件的场景
    inline static auto OpenMemoryWithoutOdex_ =
            ("_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_"_sym |
         "_ZN3art7DexFile10OpenMemoryEPKhmRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_"_sym).as<
        const DexFile*(const uint8_t* dex_file, size_t size, const std::string& location,
                       uint32_t location_checksum, void* mem_map, std::string* error_msg)>;

    // ART 内部的 DexFile_setTrusted 函数指针，用于在 Android P+ 上将 DexFile 标记为可信，
    // 以绕过隐藏 API 限制和类加载限制
    inline static auto DexFile_setTrusted_ =
            "_ZN3artL18DexFile_setTrustedEP7_JNIEnvP7_jclassP8_jobject"_sym.as<void(JNIEnv* env, jclass clazz, jobject j_cookie)>;

public:
    // 从内存中打开 DEX 文件，依次尝试不同版本的 OpenMemory 符号。
    // 从 DEX 文件头部提取 checksum，适配不同 Android 版本的符号签名
    static const DexFile* OpenMemory(const uint8_t* dex_file, size_t size, std::string location,
                                     std::string* error_msg) {
        if (OpenMemory_) [[likely]] {
            return OpenMemory_(dex_file, size, location,
                               reinterpret_cast<const Header*>(dex_file)->checksum_, nullptr,
                               nullptr, error_msg)
                .release();
        }
        if (OpenMemoryRaw_) [[likely]] {
            return OpenMemoryRaw_(dex_file, size, location,
                                  reinterpret_cast<const Header*>(dex_file)->checksum_, nullptr,
                                  nullptr, error_msg);
        }
        if (OpenMemoryWithoutOdex_) [[likely]] {
            return OpenMemoryWithoutOdex_(dex_file, size, location,
                                          reinterpret_cast<const Header*>(dex_file)->checksum_,
                                          nullptr, error_msg);
        }
        if (error_msg) *error_msg = "null sym";
        return nullptr;
    }

    // 将 native DexFile 指针转换为 Java 层的 dalvik.system.DexFile 对象：
    // 通过设置 mCookie（和 mInternalCookie）字段，使 Java 层能使用此 DexFile 加载类
    jobject ToJavaDexFile(JNIEnv* env) const {
        auto* java_dex_file = env->AllocObject(dex_file_class);
        auto cookie = JNI_NewLongArray(env, dex_file_start_index + 1);
        if (dex_file_start_index != size_t(-1)) [[likely]] {
            cookie[oat_file_index] = 0;
            cookie[dex_file_start_index] = reinterpret_cast<jlong>(this);
            cookie.commit();
            JNI_SetObjectField(env, java_dex_file, cookie_field, cookie);
            if (internal_cookie_field) {
                JNI_SetObjectField(env, java_dex_file, internal_cookie_field, cookie);
            }
        } else {
            JNI_SetLongField(
                env, java_dex_file, cookie_field,
                static_cast<jlong>(reinterpret_cast<uintptr_t>(new std::vector{this})));
        }
        JNI_SetObjectField(env, java_dex_file, file_name_field, JNI_NewStringUTF(env, ""));
        return java_dex_file;
    }

    // 调用 ART 内部 DexFile_setTrusted 将指定的 DexFile 标记为可信，
    // Android P+ 上必须调用此函数才能使注入的 DexFile 被正常使用
    static bool SetTrusted(JNIEnv* env, jobject cookie) {
        if (!DexFile_setTrusted_) return false;
        DexFile_setTrusted_(env, nullptr, cookie);
        return true;
    }

    // 初始化 DexFile 模块：
    // Android O+ 不需要 OpenMemory hook（系统支持直接加载），
    // Android O 以下需要 hook OpenMemory 并缓存 DexFile 类的 JNI 字段 ID
    static bool Init(JNIEnv* env, const HookHandler& handler) {
        auto sdk_int = GetAndroidApiLevel();
        if (sdk_int >= __ANDROID_API_P__) [[likely]] {
            if (!handler(DexFile_setTrusted_, true)) {
                LOGW("DexFile.setTrusted not found, MakeDexFileTrusted will not work.");
            }
        }
        if (sdk_int >= __ANDROID_API_O__) [[likely]] {
            return true;
        }
        if (!handler(OpenMemory_, OpenMemoryRaw_, OpenMemoryWithoutOdex_)) [[unlikely]] {
            LOGE("Failed to find OpenMemory");
            return false;
        }
        dex_file_class = JNI_NewGlobalRef(env, JNI_FindClass(env, "dalvik/system/DexFile"));
        if (!dex_file_class) [[unlikely]] {
            return false;
        }
        if (sdk_int >= __ANDROID_API_M__) [[unlikely]] {
            cookie_field = JNI_GetFieldID(env, dex_file_class, "mCookie", "Ljava/lang/Object;");
        } else {
            cookie_field = JNI_GetFieldID(env, dex_file_class, "mCookie", "J");
            dex_file_start_index = -1;
        }
        if (!cookie_field) [[unlikely]] {
            return false;
        }
        file_name_field = JNI_GetFieldID(env, dex_file_class, "mFileName", "Ljava/lang/String;");
        if (!file_name_field) [[unlikely]] {
            return false;
        }
        if (sdk_int >= __ANDROID_API_N__) [[likely]] {
            internal_cookie_field =
                JNI_GetFieldID(env, dex_file_class, "mInternalCookie", "Ljava/lang/Object;");
            if (!internal_cookie_field) [[unlikely]] {
                return false;
            }
            dex_file_start_index = 1u;
        }
        return true;
    }

private:
    inline static jclass dex_file_class = nullptr;
    inline static jfieldID cookie_field = nullptr;
    inline static jfieldID file_name_field = nullptr;
    inline static jfieldID internal_cookie_field = nullptr;
    inline static size_t oat_file_index = 0u;
    inline static size_t dex_file_start_index = 0u;
};
}  // namespace lsplant::art
