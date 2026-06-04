# HOOK — ARM64 Inline Hook Framework + JavaScript Scripting Engine

Android ARM64 inline hook 框架，内置 Java 方法 hook 和 QuickJS 脚本引擎，兼容 Frida 核心 API。

## 项目结构

```
app/src/main/
├── cpp/
│   ├── hook_sdk/          # ARM64 inline hook 引擎
│   │   ├── include/hook_sdk.h
│   │   └── src/
│   │       ├── assembler.h/cpp      # ARM64 指令生成器
│   │       ├── relocator.h/cpp      # 指令重定位
│   │       ├── trampoline.h/cpp     # 跳板代码生成
│   │       ├── code_patch.h/cpp     # 代码段读写
│   │       ├── memory_allocator.h   # 近地址可执行内存分配
│   │       ├── hook_registry.h/cpp  # Hook 注册表
│   │       ├── hook_api.cpp         # HookInstall/HookUninstall
│   │       └── elf_symbol_resolver  # libart.so 符号解析
│   ├── lsplant/            # ART Java 方法 hook (LSPlant)
│   ├── quickjs/            # QuickJS 引擎 (Frida fork)
│   └── js_engine/          # JavaScript 脚本层
│       ├── include/js_engine.h
│       └── src/
│           ├── js_engine.cpp        # 引擎生命周期
│           ├── js_scope.h           # RAII 线程安全守卫
│           ├── js_ptr.h/cpp         # NativePointer 类
│           ├── js_memory.h/cpp      # Memory 读写 API
│           ├── js_module.h/cpp      # Module 查询 API
│           ├── js_interceptor.h/cpp # Interceptor hook API
│           ├── js_native.h/cpp      # NativeFunction/NativeCallback
│           ├── js_java_bridge.h/cpp # Java.perform/Java.use
│           ├── js_socket_server.h/cpp # TCP 脚本下发
│           └── js_runtime.js        # ES2020 引导脚本
├── java/com/io/hook/
│   ├── HookBridge.java     # Java hook 入口
│   ├── MethodHook.java     # before/after 回调基类
│   ├── MethodReplacement.java  # 方法替换工具
│   ├── JavaHookManager.java    # 低层 hook 管理
│   ├── ScriptEngine.java   # JS 引擎 JNI 桥接
│   └── MainActivity.java   # 测试入口
```

---

## 一、ARM64 Inline Hook (hook_sdk)

### 概述

纯 C++ 实现的 ARM64 inline hook 引擎，无任何外部依赖。支持 hook 和 unhook 任意函数（包括 16+ 参数的函数），自动处理指令重定位。

### C API

```c
#include "hook_sdk.h"

// 安装 hook
// target:    目标函数地址
// replace:   替换函数地址
// orig_func: [out] 原函数跳板，可用来调用原始实现
// 返回 0 成功，负数失败
int HookInstall(void *target, void *replace, void **orig_func);

// 卸载 hook，恢复原始代码
// 返回 0 成功，负数失败
int HookUninstall(void *target);
```

### 使用示例

```c
#include "hook_sdk.h"

__attribute__((noinline))
int get_secret_value() { return 42; }

int (*orig_get_secret_value)() = nullptr;

int fake_get_secret_value() {
    int original = orig_get_secret_value();
    return original + 1000;
}

void install_hook() {
    HookInstall(
        (void *)get_secret_value,
        (void *)fake_get_secret_value,
        (void **)&orig_get_secret_value);
    // get_secret_value() 现在返回 1042
}

void remove_hook() {
    HookUninstall((void *)get_secret_value);
    // get_secret_value() 恢复返回 42
}
```

### 技术细节

- **跳转编码**：优先使用 4 字节 B 指令（±128MB），否则使用 LDR+BR 字面量（20 字节，任意距离）
- **指令重定位**：自动处理 B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/ADR/ADRP/LDR literal 等指令
- **近地址分配**：三级策略 — 已有页面空闲空间 → /proc/self/maps 间隙 → 已有区域 code cave
- **线程安全**：使用 `mmap` + `mprotect` 管理可执行内存，代码写入期间临时切换权限

### 实测结果

| 测试 | 说明 | 结果 |
|------|------|------|
| 简单函数 hook | hook `get_secret_value()`，修改返回值 | PASS |
| 16 参数函数 hook | hook `sum16_params(1..16)`，验证所有参数传递正确 | PASS |
| Unhook 恢复 | unhook 后函数行为完全恢复 | PASS |

---

## 二、Java 方法 Hook (HookBridge + LSPlant)

### 概述

基于 LSPlant 实现的 Java/ART 方法 hook 框架，提供 Xposed 风格的 before/after 回调，支持优先级排序和多个回调同时注册。

### Java API

#### HookBridge

```java
public final class HookBridge {

    // Hook 一个方法
    public static MethodHook.Unhook hookMethod(Member method, MethodHook callback);

    // Hook 类中所有同名方法
    public static Set<MethodHook.Unhook> hookAllMethods(Class<?> clazz, String name, MethodHook callback);

    // Hook 类的所有构造函数
    public static Set<MethodHook.Unhook> hookAllConstructors(Class<?> clazz, MethodHook callback);

    // 移除 hook
    public static void unhookMethod(Member method, MethodHook callback);

    // 调用原始方法（绕过 hook）
    public static Object invokeOriginalMethod(Member method, Object thisObj, Object[] args);

    // 检查方法是否被 hook
    public static boolean isHooked(Member method);

    // 反优化方法（防止 ART 内联）
    public static boolean deoptimizeMethod(Member method);

    // 检查 LSPlant 是否已初始化
    public static native boolean isInitialized();
}
```

#### MethodHook — before/after 回调

```java
public abstract class MethodHook {
    public static final int PRIORITY_DEFAULT  = 50;
    public static final int PRIORITY_HIGHEST  = 10000;
    public static final int PRIORITY_LOWEST   = -10000;

    protected void before(HookParam param) throws Throwable {}
    protected void after(HookParam param) throws Throwable {}

    public class HookParam {
        public Member method;        // 被 hook 的方法
        public Object thisObject;    // this 对象（静态方法为 null）
        public Object[] args;        // 参数数组（可修改）

        public Object getResult();
        public void setResult(Object result);      // 修改返回值，阻止原方法执行
        public Throwable getThrowable();
        public void setThrowable(Throwable t);     // 让原方法抛出异常
    }
}
```

#### MethodReplacement — 完全替换方法

```java
public abstract class MethodReplacement extends MethodHook {
    protected abstract Object replace(HookParam param) throws Throwable;

    // 跳过原方法，返回 null
    public static final MethodReplacement DO_NOTHING;

    // 跳过原方法，返回固定值
    public static MethodReplacement returnConstant(Object value);
}
```

### 使用示例

```java
// 1. Before/After hook — 修改参数和返回值
Method targetMethod = MainActivity.class.getMethod("computeValue", int.class, int.class);

MethodHook.Unhook unhook = HookBridge.hookMethod(targetMethod, new MethodHook() {
    @Override
    protected void before(HookParam param) {
        // 修改参数：将 (10, 20) 改为 (100, 200)
        param.args[0] = 100;
        param.args[1] = 200;
    }

    @Override
    protected void after(HookParam param) {
        // 覆盖返回值
        param.setResult(1000);
    }
});

// computeValue(10, 20) → before 修改参数 → 原方法执行 100+200=300 → after 覆盖为 1000

unhook.unhook(); // 恢复

// 2. 方法替换 — 直接返回固定值
HookBridge.hookMethod(targetMethod, MethodReplacement.returnConstant(9999));
// computeValue(任何参数) → 返回 9999，原方法不执行

// 3. 调用原始方法
Object result = HookBridge.invokeOriginalMethod(targetMethod, null, new Object[]{10, 20});
```

### 反检测

- LSPlant 生成的类名使用 proguard 风格的混淆名（`a0`, `a1`...），而非默认的 `LSPHooker_`
- 通过 `RegisterNatives` 注册 native 方法，避免 `Java_com_io_hook_*` 符号模式

### 实测结果

| 测试 | 说明 | 结果 |
|------|------|------|
| before 修改参数 | args[0]=100, args[1]=200 | PASS |
| after 覆盖返回值 | setResult(1000) | PASS |
| 调用原方法 | invokeOriginalMethod 返回真实值 | PASS |
| unhook 恢复 | 卸载后行为完全恢复 | PASS |
| 方法替换 | returnConstant(9999) | PASS |

---

## 三、JavaScript 脚本引擎 (QuickJS + Frida 兼容 API)

### 概述

基于 QuickJS（Frida fork，ES2020）的脚本引擎，提供兼容 Frida 的 JavaScript API。支持通过 JS 脚本动态执行 native hook、内存读写、调用原生函数、hook Java 方法。

### 引导方式

```java
// Java 端执行 JS 脚本
String result = ScriptEngine.loadScript("1 + 1");  // → "2"

// 通过 TCP socket 远程下发脚本
SocketServer.start(0);  // 随机端口
SocketServer.send('{"type":"result","value":"..."}');
```

### JS API 参考

#### ptr(value)

创建 NativePointer 对象。接受数字、十六进制字符串、已有的 NativePointer。

```javascript
var p1 = ptr(0x1234);
var p2 = ptr("0xDEADBEEF");
var p3 = ptr(null);       // NULL 指针
```

#### NativePointer

所有指针操作的基础类。

```javascript
var p = ptr(0x1000);

p.toString()      // "0x1000"
p.isNull()        // false
p.toInt32()       // 4096

// 算术运算
p.add(0x50)       // 0x1050
p.sub(0x100)      // 0xf00

// 位运算
p.and(0xFF)       // 0x0
p.or(0xFF)        // 0x10ff
p.xor(0xFF)       // 0x10ff
p.shr(8)          // 0x10
p.shl(4)          // 0x10000
p.not()           // 按位取反

// 比较
p.compare(ptr(0x2000))  // -1 (小于)
p.compare(ptr(0x1000))  // 0  (等于)
p.compare(ptr(0x500))   // 1  (大于)
```

#### Memory

直接读写进程内存。

```javascript
var buf = Memory.alloc(256);        // 分配 256 字节（calloc，清零）

// 写入
Memory.writeU8(buf, 0x41);
Memory.writeU32(buf, 12345);
Memory.writeU64(buf, 0xDEADBEEFCAFE);
Memory.writePointer(buf, ptr(0x1234));
Memory.writeUtf8String(buf, "Hello");
Memory.writeFloat(buf, 3.14);
Memory.writeDouble(buf, 3.14159265);
Memory.writeByteArray(buf, new Uint8Array([1,2,3,4]));

// 读取
Memory.readU8(buf)              // → 0x41
Memory.readU32(buf)             // → 12345
Memory.readU64(buf)             // → 0xDEADBEEFCAFE (BigInt)
Memory.readPointer(buf)         // → NativePointer(0x1234)
Memory.readUtf8String(buf, 5)   // → "Hello"
Memory.readFloat(buf)           // → 3.14
Memory.readDouble(buf)          // → 3.14159265
Memory.readByteArray(buf, 4)   // → ArrayBuffer

// 完整类型列表：
// readU8/U16/U32/U64/S8/S16/S32/S64/Float/Double/Pointer/Utf8String/ByteArray
// writeU8/U16/U32/U64/S8/S16/S32/S64/Float/Double/Pointer/Utf8String/ByteArray
```

#### Module

查询已加载模块和导出符号。

```javascript
// 查找导出函数地址
var malloc = Module.findExportByName('libc.so', 'malloc');
// malloc → NativePointer(0x7abc...)

var open = Module.findExportByName(null, 'open');  // null = 搜索所有模块

// 获取模块基地址
var base = Module.getBaseAddress('libc.so');
// base → NativePointer(0x7abc...)
```

#### Interceptor

Hook native 函数，支持 onEnter/onLeave 回调。

```javascript
// attach：在目标函数前后插入回调
var listener = Interceptor.attach(ptr(0x1234), {
    onEnter: function(args) {
        // args.x0 ~ args.x7 是前 8 个参数的指针表示
        console.log("arg0 =", args.x0);
        // 可以修改参数：
        // args.x0 = ptr(0x9999);
    },
    onLeave: function(retval) {
        // retval 是返回值的指针表示
        console.log("return =", retval);
        // 可以修改返回值：
        // retval.replace(ptr(42));
    }
});

// replace：完全替换函数实现
Interceptor.replace(ptr(0x1234), new NativeCallback(
    function(a, b) { return a * b; },
    'int', ['int', 'int']
));

// detachAll：卸载所有 hook
Interceptor.detachAll();
```

#### NativeFunction

将原生函数包装为可在 JS 中直接调用的对象。

```javascript
// new NativeFunction(address, returnType, [argTypes])
// 类型: void/int/uint8/int8/uint16/int16/uint32/int32/uint64/int64
//       float/double/pointer/size_t

// 调用 libc 的 open 函数
var open = new NativeFunction(
    Module.findExportByName(null, 'open'),
    'int', ['pointer', 'int']
);
var fd = open(ptr("/proc/self/maps"), 0);

// 调用 malloc
var malloc = new NativeFunction(
    Module.findExportByName(null, 'malloc'),
    'pointer', ['int']
);
var mem = malloc(1024);

// 调用自定义函数
var myFunc = new NativeFunction(ptr(0x1234), 'int', ['int', 'int', 'int']);
var result = myFunc(1, 2, 3);  // 直接调用
```

#### NativeCallback

将 JS 函数转换为原生函数指针，供 native 代码回调。

```javascript
// new NativeCallback(jsFunc, returnType, [argTypes])

var callback = new NativeCallback(
    function(a, b) {
        return a + b;
    },
    'int', ['int', 'int']
);

// callback.address 是一个 NativePointer，可传给任何需要函数指针的地方
var result_ptr = callback.address;  // 可传给 HookInstall 等

// 用作 Interceptor.replace 的替换函数
Interceptor.replace(target, new NativeCallback(
    function() { return 42; },
    'int', []
));
```

#### Java

在 JS 中调用 JNI 操作，hook Java 方法。

```javascript
// Java.perform：在 JNI 线程中执行代码
Java.perform(function() {
    // JNI 环境已 attach
    var Foo = Java.use("com.example.Foo");
    // ...
});

// Java.use：获取 Java 类的包装器
var Activity = Java.use("com.io.hook.MainActivity");
```

#### send / SocketServer

脚本与外部的消息通信。

```javascript
// 发送 JSON 消息到所有连接的客户端
send(JSON.stringify({ type: "log", message: "hook installed" }));

// TCP 服务器控制
SocketServer.start(0);       // 启动，0 = 随机端口
SocketServer.start(27042);   // 启动，指定端口
SocketServer.stop();         // 停止
SocketServer.getPort();      // 获取当前端口
SocketServer.send(json);     // 广播消息到所有客户端
```

### 线程安全

所有进入 QuickJS 的路径都通过 `JSScope`（RAII 递归互斥锁 + `JS_Enter/Leave`）保护：

- JS 主线程正常执行
- Hook 回调在任意线程触发时，自动获取锁
- 如果 JS 线程正在执行，hook 回调会阻塞等待
- NativeCallback 的 native → JS 调用同样通过 JSScope 保护

### 反检测

- `-fvisibility=hidden`：所有 js_engine 符号不导出
- QuickJS 版本字符串改为 `"6.0.0"`（不暴露 QuickJS 标识）
- JS 运行时脚本不包含 "Frida" 字符串
- Log tag 使用通用名称 `"Core"`
- HookBridge native 方法通过 `RegisterNatives` 注册（无 `Java_com_io_hook_*` 符号）
- LSPlant 生成类名使用 proguard 风格混淆

---

## 四、构建

```bash
# 编译 debug APK
./gradlew assembleDebug

# 安装到设备
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 启动
adb shell am start -n com.io.hook/.MainActivity
```

### 依赖

- Android NDK 29+
- Android 16+ (API 35+)
- CMake 3.22+
- C++23

### 编译选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ENABLE_JS_ENGINE` | ON | 启用 JS 脚本引擎 |

---

## 五、实测结果汇总

所有测试在 Android 16 (Pixel 8 Pro) 上通过：

| # | 测试 | 结果 |
|---|------|------|
| 1 | 简单函数 Inline Hook | PASS |
| 2 | 16 参数函数 Inline Hook | PASS |
| 3a | Java before/after Hook | PASS |
| 3b | Java 方法替换 | PASS |
| 3c | Java unhook 恢复 | PASS |
| 4a | JS 引擎初始化 | PASS |
| 4b | JS 基础表达式 `1+1=2` | PASS |
| 4c | NativePointer 创建和运算 | PASS |
| 4d | Memory alloc/write/read | PASS |
| 4e | Module.findExportByName | PASS |
| 4f | NativeFunction 创建 | PASS |
| 4g | JS 错误处理 | PASS |

---

## 六、技术架构

```
┌─────────────────────────────────────────────────┐
│                  JavaScript 脚本                 │
│  ptr() · Memory · Module · Interceptor           │
│  NativeFunction · NativeCallback · Java           │
├─────────────────────────────────────────────────┤
│              js_runtime.js (ES2020)               │
│              Frida 兼容 API 包装层                 │
├─────────────────────────────────────────────────┤
│              QuickJS 引擎 (Frida fork)            │
│              JSRuntime → JSContext → JSScope      │
├──────────┬──────────┬────────────┬───────────────┤
│ js_ptr   │js_memory │js_module   │js_interceptor │
│js_native │js_java   │js_socket   │               │
├──────────┴──────────┴────────────┴───────────────┤
│              hook_sdk (ARM64 Inline Hook)          │
│  Assembler · Relocator · Trampoline · Allocator   │
├─────────────────────────────────────────────────┤
│              LSPlant (ART Java Hook)              │
│  ELF Symbol Resolver · libart 符号查找            │
├─────────────────────────────────────────────────┤
│                 Android Runtime                   │
│              libart.so · JNI · mmap               │
└─────────────────────────────────────────────────┘
```
