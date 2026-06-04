# LSPlant 剖析：ART 方法 Hook 实现原理

## 一、项目概述

LSPlant 是一个用于 ART (Android Runtime) 的 Java 方法 Hook 框架。它的核心能力是：**在运行时替换任意 Java 方法的执行入口，将调用重定向到用户提供的回调函数，同时保留调用原始方法的能力（backup）**。

与 inline hook（如 Dobby）直接修改机器码不同，LSPlant 的 Hook 机制基于 ART 虚拟机内部的数据结构操作——通过替换 `ArtMethod` 的 `entry_point` 字段来实现方法劫持。

### 设计目标
- **不依赖任何 inline hook 库**：通过 `InitInfo` 回调由用户提供 inline hook 能力
- **跨 Android 版本兼容**：支持 Android L (5.0) ~ 最新版本
- **多架构支持**：ARM、AArch64、x86、x86_64、RISC-V 64
- **线程安全**：Hook/UnHook 操作自动挂起所有线程

---

## 二、整体架构

```
┌───────────────────────────────────────────────────────────────┐
│                        用户代码 (Java/C++)                      │
│                                                               │
│  HookSDK.Init(env, info)    ──→  初始化                         │
│  HookSDK.Hook(target, obj, cb) ──→  Hook 一个 Java 方法          │
│  HookSDK.UnHook(target)     ──→  取消 Hook                      │
└───────────────────────┬───────────────────────────────────────┘
                        │
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                      LSPlant 公开 API 层                        │
│                   (lsplant.hpp / lsplant.cc)                   │
│                                                               │
│  Init()  → InitConfig() + InitJNI() + InitNative()            │
│  Hook()  → BuildDex() + DoHook()                              │
│  UnHook() → DoUnHook()                                        │
└───────────────────────┬───────────────────────────────────────┘
                        │
          ┌─────────────┼─────────────┐
          ▼             ▼             ▼
┌─────────────┐ ┌──────────────┐ ┌──────────────────┐
│  ART 内部模块  │ │  DEX 动态生成   │ │  Trampoline 管理  │
│              │ │              │ │                  │
│ ArtMethod    │ │ dex_builder  │ │ shellcode 模板     │
│ ClassLinker  │ │ (slicer)     │ │ 内存页池           │
│ Runtime      │ │              │ │ 原子分配           │
│ Thread       │ └──────────────┘ └──────────────────┘
│ JitCodeCache │
│ Instrum.     │
│ JniIdManager │
│ DexFile      │
└──────────────┘
```

---

## 三、核心原理

### 3.1 ART 方法调用机制

在 ART 中，每个 Java 方法在运行时都对应一个 C++ 对象 `art::ArtMethod`，其内存布局（简化）为：

```
ArtMethod 结构体（以 AArch64 为例，大小约 40~48 字节）：
┌─────────────────────────────┐
│ declaring_class_ (4 bytes)  │  ← 声明该方法的类（GcRoot）
├─────────────────────────────┤
│ access_flags_   (4 bytes)  │  ← 访问标志（public/native/static 等）
├─────────────────────────────┤
│ dex_method_index_ (4 bytes)│  ← DEX 文件中的方法索引
├─────────────────────────────┤
│ ...                         │  ← 其他字段（dex_cache_resolves 等）
├─────────────────────────────┤
│ data_           (8 bytes)  │  ← 原生函数指针（JNI）或 ProfilingInfo
├─────────────────────────────┤
│ entry_point_    (8 bytes)  │  ← ★ 方法的机器码入口地址（quick code）
└─────────────────────────────┘
```

**关键点**：当 ART 调用一个 Java 方法时，最终会跳转到 `entry_point_` 指向的机器码地址。LSPlant 通过替换这个字段来劫持方法调用。

### 3.2 Hook 流程详解

```
Hook(target_method, hooker_object, callback_method)
     │
     ▼
① 从 Java 反射对象获取 ArtMethod 指针
     │  ArtMethod::FromReflectedMethod(env, target_method)
     ▼
② 获取方法签名（shorty），判断是否 static
     │  ArtMethod::GetMethodShorty(env, target_method)
     ▼
③ 动态生成 DEX（BuildDex）
     │  使用 dex_builder 构建一个内存 DEX，包含：
     │  - hook 方法：将参数打包为 Object[]，调用 hooker_object.callback_method()
     │  - backup 方法：返回默认值的空方法
     │  - hooker 字段：存储 hooker_object 引用
     ▼
④ 加载生成的 DEX 到新的 ClassLoader
     │  通过 DexFile + PathClassLoader 加载生成的类
     ▼
⑤ 获取 hook 方法和 backup 方法的 ArtMethod 指针
     │  FromReflectedMethod(env, reflected_hook)
     │  FromReflectedMethod(env, reflected_backup)
     ▼
⑥ 将 hooker_object 存入生成类的静态字段
     │  SetStaticObjectField(built_class, hooker_field, hooker_object)
     ▼
⑦ 执行实际 Hook（DoHook）
     │
     ├── 挂起所有线程（ScopedSuspendAll）
     ├── 进入 GC 安全区（ScopedGCCriticalSection）
     │
     ├── 为 hook 方法生成 trampoline
     │    GenerateTrampolineFor(hook)
     │    → 分配可执行内存，写入 shellcode + ArtMethod 指针
     │
     ├── 标记 target 为非 intrinsic
     │    target->SetNonIntrinsic()
     │
     ├── 将 target 备份到 backup
     │    target->BackupTo(backup)
     │    → CopyFrom + SetNonCompilable + ClearFastInterpretFlag
     │
     ├── 标记 target 为不可编译
     │    target->SetNonCompilable()
     │
     └── 替换 target 的 entry_point
          target->SetEntryPoint(trampoline)
          同时设置解释器入口为 artInterpreterToCompiledCodeBridge
```

### 3.3 Trampoline 机制

LSPlant 使用预编译的 shellcode 模板作为 trampoline。以 AArch64 为例：

```asm
// AArch64 trampoline shellcode（20 字节）：
// 机器码：60 00 00 58  10 00 40 F8  00 02 1F D6  [ArtMethod*]
LDR X16, [PC+12]       // 从末尾加载 ArtMethod* 到 X16
LDR X16, [X16, #offset] // 从 ArtMethod 加载 entry_point（偏移量在初始化时写入）
BR  X16                 // 跳转到 entry_point
.quad 0x12345678        // ← ArtMethod* 指针（运行时填充为 hook 方法的指针）
```

**执行流程**：
```
调用 target 方法
  → 跳转到 trampoline
    → 加载 hook ArtMethod*
      → 从 hook ArtMethod* 读取 entry_point（即 hook 方法的 quick code）
        → 跳转执行 hook 方法
          → hook 方法中读取 hooker 字段，调用 callback_method(Object[] args)
            → 用户回调被执行
```

#### Trampoline 内存池

```cpp
// 使用原子变量管理 trampoline 内存页
union Trampoline {
    uintptr_t address;     // 页面基地址
    unsigned count4k : 12; // 4K 页面的已分配计数
    unsigned count16k : 14;// 16K 页面的已分配计数
};
std::atomic_uintptr_t trampoline_pool{0};

// 无锁分配：
// 1. fetch_add(1) 原子递增计数
// 2. 如果页未满，直接使用对应槽位
// 3. 如果页满了或未分配，通过自旋锁 mmap 新页
```

### 3.4 UnHook 流程

```
UnHook(target_method)
     │
     ├── 从 hooked_methods_ 查找并移除记录
     ├── 获取 backup ArtMethod 指针
     │
     └── DoUnHook(target, backup)
          ├── 挂起所有线程
          ├── 保存当前 access_flags
          ├── 将 backup 的内容复制回 target（CopyFrom）
          │    → 恢复原始 entry_point、data_ 等所有字段
          └── 恢复 access_flags（保留可能的运行时修改）
```

---

## 四、ART 内部组件初始化

### 4.1 ArtMethod 偏移量探测

LSPlant 不硬编码 ArtMethod 的字段偏移，而是在运行时通过 JNI 反射动态探测：

```cpp
static bool Init(JNIEnv *env, const HookHandler handler) {
    // 1. 通过 Throwable 的两个构造函数的 ArtMethod 指针差值计算 ArtMethod 大小
    auto *first = FromReflectedMethod(env, first_ctor);
    auto *second = FromReflectedMethod(env, second_ctor);
    art_method_size = (uintptr_t)second - (uintptr_t)first;

    // 2. entry_point 位于结构体末尾
    entry_point_offset = art_method_size - kPointerSize;
    data_offset = entry_point_offset - kPointerSize;

    // 3. access_flags 通过扫描匹配 Java 反射得到的值来定位
    for (size_t i = 0; i < art_method_size; i += sizeof(uint32_t)) {
        if (*(uint32_t*)((uintptr_t)first + i) == real_flags) {
            access_flags_offset = i;
            break;
        }
    }
}
```

### 4.2 线程暂停机制

Hook/UnHook 操作必须在世界暂停（Stop-The-World）状态下执行：

```cpp
class ScopedSuspendAll {
    // 优先使用 ART 内部的 ScopedSuspendAll（构造/析构函数）
    // 回退到 Dbg::SuspendVM / Dbg::ResumeVM
    ScopedSuspendAll(const char *cause, bool long_suspend) {
        constructor_(this, cause, long_suspend);  // 暂停所有线程
    }
    ~ScopedSuspendAll() {
        destructor_(this);  // 恢复所有线程
    }
};
```

### 4.3 GC 安全区

```cpp
class ScopedGCCriticalSection {
    // 在 Hook 期间阻止 GC 回收相关 ArtMethod 数据
    ScopedGCCriticalSection(Thread::Current(), kGcCauseDebugger, kCollectorTypeDebugger);
};
```

---

## 五、保护机制（防止 Hook 被覆盖）

LSPlant 需要防止 ART 运行时的多种行为覆盖已设置的 hook：

### 5.1 防止 JIT 编译覆盖

```
问题：JIT 编译器可能重新编译 target 方法，更新其 entry_point
解决：
  - target->SetNonCompilable()  // 设置 kAccCompileDontBother 标记
  - target->SetNonIntrinsic()    // 清除 intrinsic 标记
```

### 5.2 防止 FixupStaticTrampolines 覆盖

```
问题：ART 在类初始化时会调用 FixupStaticTrampolines，可能修改 entry_point
解决：Hook FixupStaticTrampolines 系列函数
  → 在原始函数执行后调用 RestoreBackup()
  → 将被修改的 entry_point 传播到 backup 方法，并恢复 target 的 hook entry_point
```

### 5.3 防止 Instrumentation 覆盖

```
问题：可调试模式下 ART 的 Instrumentation 会更新方法的 entry_point
解决：Hook 三个关键函数
  - UpdateMethodsCodeToInterpreterEntryPoint
  - InitializeMethodsCode
  - ReinitializeMethodsCode
  → 所有更新操作重定向到 backup 方法
```

### 5.4 防止 RegisterNative 覆盖

```
问题：RegisterNative 会修改 data_ 字段（JNI 函数指针）
解决：Hook RegisterNative/UnregisterNative 的多个版本
  → 所有操作重定向到 backup 方法
```

### 5.5 JIT Code Cache GC 保护

```
问题：JIT GC 可能回收 target 方法的 JIT 数据
解决：Hook GarbageCollectCache / DoCollection
  → 在 GC 前调用 MoveObsoleteMethod 将 JIT 数据迁移到 backup 方法
```

### 5.6 解释器入口保护

```
问题：ShouldUseInterpreterEntrypoint 可能强制使用解释器入口
解决：Hook ShouldUseInterpreterEntrypoint
  → 当方法已 hook 且有 quick_code 时返回 false
```

### 5.7 可调试模式处理

```
问题：Android P+ 的可调试模式下，ART 会调用更多检查路径
解决：
  - 初始化完成后临时将 Runtime 设为 kNonJavaDebuggable
  - 需要时通过 JavaDebuggableGuard 临时恢复可调试状态
```

---

## 六、DEX 动态生成

### 6.1 生成的 DEX 结构

LSPlant 使用 `dex_builder`（基于 Google 的 slicer 库）在内存中动态构建一个 DEX 文件：

```java
// 生成的类结构：
public class LSPHooker_ {
    public static Object hooker;  // 存储用户传入的 hooker_object

    // hook 方法（方法名可配置，默认与目标方法同名）
    // 签名与目标方法完全一致
    public static ReturnType hook_method(ParamType1 p1, ParamType2 p2, ...) {
        Object[] args = new Object[参数数量];
        // 非静态方法：args[0] = this
        // 装箱基本类型参数
        args[0] = p1; args[1] = p2; ...
        // 调用用户回调
        Object result = hooker.callback_method(args);
        // 拆箱返回值
        return (ReturnType) result;
    }

    // backup 方法（固定名为 "backup"）
    public static ReturnType backup(ParamType1 p1, ParamType2 p2, ...) {
        return 默认值;  // void/0/null（实际不会被调用，入口会被 DoHook 替换）
    }
}
```

### 6.2 DEX 加载方式（按 API 级别）

| Android 版本 | 加载方式 |
|---|---|
| Q (10)+ | `DexFile(ByteBuffer[], ClassLoader, Element[])` |
| O (8) ~ Q | `DexFile(ByteBuffer)` |
| L (5) ~ N | mmap + `DexFile::OpenMemory` + 手动调用 `ToJavaDexFile` |

---

## 七、辅助功能

### 7.1 Deoptimize（去优化）

```cpp
bool Deoptimize(JNIEnv *env, jobject method) {
    // 将方法的 entry_point 设置为解释器入口
    // 使方法强制走解释执行路径，避免 JIT 内联导致 hook 失效
    ClassLinker::SetEntryPointsToInterpreter(art_method);
}
```

**使用场景**：如果方法 A 内联了方法 B，而 B 被 hook 了，A 中的 B 调用不会走 hook 路径。对 A 执行 deoptimize 可以强制 A 走解释执行，从而确保 B 的 hook 生效。

### 7.2 GetNativeFunction

```cpp
void *GetNativeFunction(JNIEnv *env, jobject method) {
    // 返回 ArtMethod 的 data_ 字段
    // 即 JNI RegisterNative 注册的函数指针
    // 用户可以备份该指针后重新 RegisterNative 实现 native hook
}
```

### 7.3 MakeClassInheritable

```cpp
bool MakeClassInheritable(JNIEnv *env, jclass target) {
    // 移除类的 final 修饰符
    // 将所有 private 构造函数设为 protected
    // 使类可被继承
}
```

### 7.4 MakeDexFileTrusted

```cpp
bool MakeDexFileTrusted(JNIEnv *env, jobject cookie) {
    // 将 DexFile 标记为可信，绕过 hidden API 访问限制
}
```

---

## 八、数据结构管理

### 8.1 Hook 状态记录

```cpp
// 核心：记录已 hook 的方法
// key = target ArtMethod*, value = (Java backup 引用, backup ArtMethod*)
SharedHashMap<ArtMethod*, pair<jobject, ArtMethod*>> hooked_methods_;

// 反向记录：backup → (nullptr, target)
// 用于检测某个方法是否是某个 hook 的 backup

// 按类分组记录
SharedHashMap<const ClassDef*, flat_hash_set<ArtMethod*>> hooked_classes_;

// 已去优化的方法集合
SharedHashSet<ArtMethod*> deoptimized_methods_set_;
```

### 8.2 线程安全

所有共享数据结构使用 `parallel_flat_hash_map`（基于 parallel-hashmap 库），内置 `std::shared_mutex`，支持并发读写。

---

## 九、关键设计特点

### 9.1 依赖注入而非硬依赖

LSPlant 不自带 inline hook 实现，而是通过 `InitInfo` 让用户注入：
- `inline_hooker`：用于 hook ART 内部函数（如 FixupStaticTrampolines）
- `inline_unhooker`：取消 ART 内部 hook
- `art_symbol_resolver`：解析 libart.so 中的符号
- `art_symbol_prefix_resolver`：按前缀解析符号

### 9.2 编译期 trampoline 生成

trampoline 的 shellcode 模板在编译期通过 `_uarr` 字面量运算符生成，运行时仅需要填充 `entry_point_offset` 和 `ArtMethod*` 指针。

### 9.3 全版本兼容策略

```
┌─────────┬──────────────────────────────────────┐
│ 版本     │ 特殊处理                               │
├─────────┼──────────────────────────────────────┤
│ L (5.0) │ 通过 ArtMethod Java 类获取偏移          │
│         │ art_quick_to_interpreter_bridge        │
│         │ 解释器入口偏移特殊处理                    │
├─────────┼──────────────────────────────────────┤
│ M (6.0) │ 通过 Executable.artMethod 字段         │
│         │ Hook GetQuickFrameInfo 处理代理方法      │
│         │ kAccCompileDontBother = kAccDefaultConflict │
├─────────┼──────────────────────────────────────┤
│ N (7.0) │ JIT code cache GC hook                │
│         │ ShouldUseInterpreterEntrypoint hook    │
├─────────┼──────────────────────────────────────┤
│ O (8.0) │ Executable 类替代 AbstractMethod       │
│         │ MoveObsoleteMethod 迁移 JIT 数据        │
│         │ SetNotIntrinsic 符号 hook              │
│         │ DexFile(ByteBuffer) 加载方式            │
├─────────┼──────────────────────────────────────┤
│ P (9.0) │ Instrumentation 三重 hook              │
│         │ Runtime::SetRuntimeDebugState           │
│         │ 可调试模式检测和绕过                      │
├─────────┼──────────────────────────────────────┤
│ Q (10)  │ DexFile(ByteBuffer[], CL, Elements)    │
│         │ kAccPreCompiled 标记                    │
│         │ kAccFastInterpreterToInterpreterInvoke  │
├─────────┼──────────────────────────────────────┤
│ R (11)  │ AdjustThreadVisibilityCounter hook     │
│         │ MarkVisiblyInitialized hook            │
│         │ kAccPreCompiled 值变化                  │
├─────────┼──────────────────────────────────────┤
│ S (12)  │ kAccPreCompiled = 0x00800000           │
│         │ SetEntryPointsToInterpreter 回退方案     │
├─────────┼──────────────────────────────────────┤
│ T (13)+ │ 不 hook ShouldUseInterpreterEntrypoint │
│         │ art_quick_to_interpreter_bridge 手动设置 │
└─────────┴──────────────────────────────────────┘
```

### 9.4 C++20 模块化设计

LSPlant 使用 C++20 Modules（`module`/`import`/`export`）组织代码，每个 ART 内部组件作为独立模块：

```
lsplant.cc          → 主模块，import 所有子模块
:common             → 共享数据结构和工具函数
:art_method         → ArtMethod 操作
:thread             → Thread::Current()
:thread_list        → ScopedSuspendAll
:class_linker       → ClassLinker hook
:runtime            → Runtime 单例和调试状态
:instrumentation    → Instrumentation hook
:jit_code_cache     → JIT GC hook
:jni_id_manager     → JNI ID 管理
:dex_file           → DexFile 操作
:jit                → JIT 编译器操作
```

---

## 十、与 Inline Hook 的对比

| 维度 | LSPlant | Inline Hook (Dobby 等) |
|------|---------|----------------------|
| **Hook 目标** | Java 方法（ART 层面） | 任意 native 函数 |
| **Hook 机制** | 替换 ArtMethod.entry_point | 修改目标函数的机器码前几条指令 |
| **线程安全** | 自动 Stop-The-World | 需自行处理 |
| **原函数调用** | 通过 backup ArtMethod | 通过 trampoline 跳板 |
| **稳定性** | 需要大量 ART 内部 hook 保护 | 相对简单，但不感知 ART 运行时 |
| **版本兼容** | 需适配各 Android 版本的 ART 内部变化 | 只需适配指令集架构 |
| **依赖关系** | 需要用户提供 inline hook 实现 | 独立使用 |

**LSPlant 本身不实现 inline hook**，它依赖用户通过 `InitInfo` 注入 inline hook 能力来 hook ART 内部函数。这些 hook 是保护性的，确保 LSPlant 对 Java 方法的 hook 不会被 ART 运行时覆盖。

---

## 十一、总结

LSPlant 的核心思想是：**利用 ART 虚拟机的方法分派机制（entry_point），在 ART 层面实现 Java 方法劫持**。它不修改任何机器码指令，而是操作 ART 内部的数据结构（ArtMethod），这使得它对 Java 方法的 Hook 比 inline hook 更加自然和稳定。

但它同时需要大量的保护性 hook（通过 inline hook 实现）来防止 ART 运行时的各种行为（JIT 编译、GC、类初始化、调试器等）破坏已建立的 hook 状态。这些保护机制构成了 LSPlant 代码的绝大部分复杂度，也是它能够跨 Android 版本稳定运行的关键。
