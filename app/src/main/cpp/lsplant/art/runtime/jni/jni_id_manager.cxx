module;

#include "logging.hpp"

export module lsplant:jni_id_manager;

import :art_method;
import :common;
import :handle;
import hook_helper;

namespace lsplant::art::jni {

export class JniIdManager {
private:
    // Hook EncodeGenericId：当 JVMTI/调试器请求一个 backup 方法的通用 ID 时，
    // 自动将请求重定向到 target 方法，使得外部工具无法感知 backup 方法的存在。
    // 如果不这样做，JVMTI 会为 backup 方法分配独立 ID，导致调试器看到重复方法，
    // 可能引发混淆或崩溃。
    inline static auto EncodeGenericId_ =
        "_ZN3art3jni12JniIdManager15EncodeGenericIdINS_9ArtMethodEEEmNS_16ReflectiveHandleIT_EE"_sym.hook->*[]
        <MemBackup auto backup>
        (JniIdManager *thiz, ReflectiveHandle<ArtMethod> method) static -> uintptr_t {
        if (auto target = IsBackup(method.Get()); target) {
            LOGD("get generic id for %s", method.Get()->PrettyMethod().c_str());
            method.Set(target);
        }
        return backup(thiz, method);
    };

public:
    // 初始化 JniIdManager hook：仅在 debuggable runtime（API 31+/Android R+）上才 hook。
    // 因为只有在 debuggable 模式下，ART 才会使用 JniIdManager 来编码方法 ID，
    // 非 debuggable 模式不需要此 hook。如果 hook 失败仅打印警告，不影响主流程。
    static bool Init(JNIEnv *env, const HookHandler &handler) {
        int sdk_int = GetAndroidApiLevel();
        if (sdk_int >= __ANDROID_API_R__) {
            if (IsJavaDebuggable(env) && !handler(EncodeGenericId_)) {
                LOGW("Failed to hook EncodeGenericId, attaching debugger may crash the process");
            }
        }
        return true;
    }
};

}  // namespace lsplant::art::jni
