module;

#include <cstdint>
#include <type_traits>

export module lsplant:handle;

import :art_method;

namespace lsplant::art {

export {
    // ObjPtr: ART 中用于包裹 mirror 对象指针的轻量智能指针，模拟 art::ObjPtr 的行为
    template <typename MirrorType>
    class ObjPtr {
    public:
        inline MirrorType *operator->() const { return Ptr(); }

        inline MirrorType *Ptr() const { return reference_; }

        inline operator MirrorType *() const { return Ptr(); }

    private:
        MirrorType *reference_;
    };

    // ObjectReference: ART 对象引用的基类模板，支持引用压缩（32 位存储指针）
    // kPoisonReferences 为 true 时对引用值取反（用于调试检测悬空引用）
    template <bool kPoisonReferences, class MirrorType>
    class alignas(4) [[gnu::packed]] ObjectReference {
        // 将 32 位压缩引用解压为完整指针，根据 kPoisonReferences 决定是否反转比特
        static MirrorType *Decompress(uint32_t ref) {
            uintptr_t as_bits = kPoisonReferences ? -ref : ref;
            return reinterpret_cast<MirrorType *>(as_bits);
        }

        uint32_t reference_;  // 压缩后的 32 位引用值

    public:
        // 将压缩引用解压为 mirror 对象指针
        MirrorType *AsMirrorPtr() const { return Decompress(reference_); }
    };

    // CompressedReference: 非毒化的压缩对象引用，用于 GC 堆上的对象引用以节省内存
    template <class MirrorType>
    class alignas(4) [[gnu::packed]] CompressedReference
        : public ObjectReference<false, MirrorType> {};

    // StackReference: 栈上的压缩对象引用，本质与 CompressedReference 相同，语义上表示栈帧中持有
    template <class MirrorType>
    class alignas(4) [[gnu::packed]] StackReference : public CompressedReference<MirrorType> {};

    // down_cast: 安全的向下转型工具，编译期检查 To 是否为 From 的子类型，避免不安全的 static_cast
    template <typename To, typename From>  // use like this: down_cast<T*>(foo);
    inline To down_cast(From * f) {        // so we only accept pointers
        static_assert(std::is_base_of_v<From, std::remove_pointer_t<To>>,
                      "down_cast unsafe as To is not a subtype of From");

        return static_cast<To>(f);
    }

    // ValueObject: 空基类标记，表示按值语义管理的对象（ART 中用于禁用默认拷贝等）
    class ValueObject {};

    // ReflectiveReference: 对 ArtMethod 的裸指针封装，提供 Ptr/Assign 访问接口
    template <class ReflectiveType>
    class ReflectiveReference {
    public:
        static_assert(std::is_same_v<ReflectiveType, ArtMethod>, "Unknown type!");

        // 获取内部保存的 ArtMethod 指针
        ReflectiveType *Ptr() { return val_; }

        // 设置内部保存的 ArtMethod 指针
        void Assign(ReflectiveType *r) { val_ = r; }

    private:
        ReflectiveType *val_;
    };

    // ReflectiveHandle: 对 ArtMethod 的间接引用句柄，通过 ReflectiveReference 间接操作指针
    // 继承 ValueObject 以表达值语义，ART 中用于在 Handle 作用域中持有反射方法
    template <typename T>
    class ReflectiveHandle : public ValueObject {
    public:
        static_assert(std::is_same_v<T, ArtMethod>, "Expected ArtField or ArtMethod");

        // 获取底层 ArtMethod 指针
        T *Get() { return reference_->Ptr(); }

        // 设置底层 ArtMethod 指针
        void Set(T *val) { reference_->Assign(val); }

    protected:
        ReflectiveReference<T> *reference_;
    };

    // Handle: ART 中用于在 HandleScope 中安全持有 mirror 对象的句柄
    // 通过 StackReference 间接引用对象，支持 GC 移动后自动更新（由 HandleScope 管理）
    template <typename T>
    class Handle : public ValueObject {
    public:
        Handle(const Handle<T> &handle) : reference_(handle.reference_) {}

        Handle<T> &operator=(const Handle<T> &handle) {
            reference_ = handle.reference_;
            return *this;
        }
        //    static_assert(std::is_same_v<T, mirror::Class>, "Expected mirror::Class");

        auto operator->() { return Get(); }

        // 从 StackReference 中解压并向下转型获取实际对象指针
        T *Get() { return down_cast<T *>(reference_->AsMirrorPtr()); }

    protected:
        StackReference<T> *reference_;
    };

    // static_assert(!std::is_trivially_copyable_v<Handle<mirror::Class>>);

    // TrivialHandle: 可平凡拷贝的 Handle 变体，与 Handle 功能相同但不禁止默认拷贝
    // 参见 ART 源码提交 38cea84b，在某些场景下 Handle 需要可平凡拷贝
    // https://cs.android.com/android/_/android/platform/art/+/38cea84b362a10859580e788e984324f36272817
    template <typename T>
    class TrivialHandle : public ValueObject {
    public:
        //    static_assert(std::is_same_v<T, mirror::Class>, "Expected mirror::Class");

        auto operator->() { return Get(); }

        // 从 StackReference 中解压并向下转型获取实际对象指针
        T *Get() { return down_cast<T *>(reference_->AsMirrorPtr()); }

    protected:
        StackReference<T> *reference_;
    };
}
}  // namespace lsplant::art
