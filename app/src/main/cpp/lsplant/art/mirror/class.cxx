module;

#include <parallel_hashmap/phmap.h>

#include "logging.hpp"

export module lsplant:clazz;

import :common;
import :art_method;
import :thread;
import :handle;
import hook_helper;

export namespace lsplant::art::mirror {

class Class {
private:
    // 解析 GetClassDef 符号，用于获取 Class 对应的 DEX ClassDef，以便后续按类查找被 hook 的方法
    inline static auto GetClassDef_ =
            "_ZN3art6mirror5Class11GetClassDefEv"_sym.as<const dex::ClassDef *(Class::*)()>;

    using BackupMethods = phmap::flat_hash_map<art::ArtMethod *, void *>;
    // 按线程和 ClassDef 分组保存被 hook/deoptimize 的静态方法的 entry point 备份
    // key 是线程指针和 ClassDef 指针，value 是方法到其原始 entry point 的映射
    inline static phmap::flat_hash_map<const art::Thread *,
                                       phmap::flat_hash_map<const dex::ClassDef *, BackupMethods>>
        backup_methods_;
    inline static std::mutex backup_methods_lock_;

    // 表示 "已初始化" 的类状态值，不同 Android 版本的枚举值不同，在 Init 中根据 API 级别设置
    inline static uint8_t initialized_status = 0;

    // 在类初始化前备份该类中所有被 hook 或 deoptimize 的静态方法的 entry point。
    // 因为类初始化（<clinit>）时 ART 会重新设置静态方法的 entry point，导致 hook 丢失，
    // 所以需要提前保存，以便在类初始化完成后恢复。
    static void BackupClassMethods(const dex::ClassDef *class_def, art::Thread *self) {
        BackupMethods out;
        if (!class_def) return;
        {
            hooked_classes_.if_contains(class_def, [&out](const auto &it) {
                for (auto method : it.second) {
                    if (method->IsStatic()) {
                        LOGV("Backup hooked method %p because of initialization", method);
                        out.emplace(method, method->GetEntryPoint());
                    }
                }
            });
        }
        {
            deoptimized_classes_.if_contains(class_def, [&out](const auto &it) {
                for (auto method : it.second) {
                    if (method->IsStatic()) {
                        LOGV("Backup deoptimized method %p because of initialization", method);
                        out.emplace(method, method->GetEntryPoint());
                    }
                }
            });
        }
        if (!out.empty()) [[unlikely]] {
            std::unique_lock lk(backup_methods_lock_);
            backup_methods_[self].emplace(class_def, std::move(out));
        }
    }

    // Hook SetStatus（Android O+，参数类型为 ClassStatus 枚举）。
    // 当类即将进入 "已初始化" 状态时，先备份该类中被 hook 的静态方法的 entry point。
    // 使用 TrivialHandle，因为此版本 ART 的 Handle 是平凡类型。
    inline static auto SetClassStatus_ =
            "_ZN3art6mirror5Class9SetStatusENS_6HandleIS1_EENS_11ClassStatusEPNS_6ThreadE"_sym.hook->*[]
        <Backup auto backup>
        (TrivialHandle<Class> h, uint8_t new_status, Thread *self) static -> void {
            if (new_status == initialized_status) {
                BackupClassMethods(GetClassDef_(h.Get()), self);
            }
            return backup(h, new_status, self);
        };

    // Hook SetStatus（Android N 及更早版本，参数类型为 int）。
    // 功能同 SetClassStatus_，但适配旧版 ART 的签名，Handle 为非平凡类型。
    inline static auto SetStatus_ =
        "_ZN3art6mirror5Class9SetStatusENS_6HandleIS1_EENS1_6StatusEPNS_6ThreadE"_sym.hook->*[]
        <Backup auto backup>
         (Handle<Class> h, int new_status, Thread *self) static -> void {
            if (new_status == static_cast<int>(initialized_status)) {
                BackupClassMethods(GetClassDef_(h.Get()), self);
            }
            return backup(h, new_status, self);
        };

    // Hook SetStatus（Android O+，参数类型为 uint32_t）。
    // 与 SetClassStatus_ 签名不同：此版本使用 TrivialHandle 且 status 为 uint32_t，
    // 适配部分 ART 版本中 Status 枚举的底层类型差异。
    inline static auto TrivialSetStatus_ =
        "_ZN3art6mirror5Class9SetStatusENS_6HandleIS1_EENS1_6StatusEPNS_6ThreadE"_sym.hook->*[]
        <Backup auto backup>
        (TrivialHandle<Class> h, uint32_t new_status, Thread *self) static -> void {
            if (new_status == initialized_status) {
                BackupClassMethods(GetClassDef_(h.Get()), self);
            }
            return backup(h, new_status, self);
        };

    // Hook SetStatus（Android N 及更早版本的非成员函数签名，参数类型为 int）。
    // 与 SetStatus_ 的区别在于此版本使用成员指针调用（MemBackup），而非 Handle。
    inline static auto ClassSetStatus_ =
        "_ZN3art6mirror5Class9SetStatusENS1_6StatusEPNS_6ThreadE"_sym.hook->*[]
        <MemBackup auto backup>
        (Class *thiz, int new_status, Thread *self) static -> void {
            if (new_status == static_cast<int>(initialized_status)) {
                BackupClassMethods(GetClassDef_(thiz), self);
            }
            return backup(thiz, new_status, self);
        };

public:
    // 初始化 Class hook：解析 GetClassDef 符号，根据 API 级别选择正确的 SetStatus hook 变体，
    // 并设置对应版本的 initialized_status 枚举值。
    // Android O 之前使用 SetStatus_ + ClassSetStatus_（旧签名），
    // Android O 及之后使用 SetClassStatus_ + TrivialSetStatus_（新签名）。
    static bool Init(const HookHandler &handler) {
        if (!handler(GetClassDef_)) {
            LOGI("GetClassDef not found, using Class pointer as fallback key (Android 16+)");
            // Android 16+ inlined GetClassDef(). Since ClassDef* is only used as a hash map key
            // and never dereferenced, using the Class* itself provides a unique per-class identifier.
            static const auto fake_get_class_def =
                +[](Class *thiz) -> const dex::ClassDef * {
                return reinterpret_cast<const dex::ClassDef *>(thiz);
            };
            GetClassDef_ = reinterpret_cast<void *>(fake_get_class_def);
        }

        int sdk_int = GetAndroidApiLevel();

        if (sdk_int < __ANDROID_API_O__) {
            if (!handler(SetStatus_, ClassSetStatus_)) {
                LOGW("SetStatus hook not installed, static method hooks on uninitialized classes may not persist");
            }
        } else {
            if (!handler(SetClassStatus_, TrivialSetStatus_)) {
                LOGW("SetStatus hook not installed, static method hooks on uninitialized classes may not persist");
            }
        }

        // 不同 Android 版本中 ClassStatus::kInitialized 的枚举值不同：
        // R(11)+: 15, P(9): 14, O-MR1(8.1): 11, 其他: 10
        if (sdk_int >= __ANDROID_API_R__) {
            initialized_status = 15;
        } else if (sdk_int >= __ANDROID_API_P__) {
            initialized_status = 14;
        } else if (sdk_int == __ANDROID_API_O_MR1__) {
            initialized_status = 11;
        } else {
            initialized_status = 10;
        }

        return true;
    }

    const dex::ClassDef *GetClassDef() { return GetClassDef_(this); }

    // 取出并删除之前保存的 entry point 备份。
    // 可以按 class_def 精确取出（类初始化完成后恢复），也可以按 self（线程）取出全部备份。
    // 返回的 BackupMethods 供调用方在类初始化完成后恢复 hook 的 entry point。
    static auto PopBackup(const dex::ClassDef *class_def, art::Thread *self) {
        BackupMethods methods;
        if (!backup_methods_.size()) [[likely]] {
            return methods;
        }
        if (class_def) {
            std::unique_lock lk(backup_methods_lock_);
            for (auto it = backup_methods_.begin(); it != backup_methods_.end();) {
                if (auto found = it->second.find(class_def); found != it->second.end()) {
                    methods.merge(std::move(found->second));
                    it->second.erase(found);
                }
                if (it->second.empty()) {
                    backup_methods_.erase(it++);
                } else {
                    it++;
                }
            }
        } else if (self) {
            std::unique_lock lk(backup_methods_lock_);
            if (auto found = backup_methods_.find(self); found != backup_methods_.end()) {
                for (auto it = found->second.begin(); it != found->second.end();) {
                    methods.merge(std::move(it->second));
                    found->second.erase(it++);
                }
                backup_methods_.erase(found);
            }
        }
        return methods;
    }
};

}  // namespace lsplant::art::mirror
