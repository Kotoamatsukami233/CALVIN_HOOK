# Frida Inline Hook 实现细节深度分析

> 本文档基于 `资料/frida-gum` 和 `资料/Dobby` 源码，深度剖析 inline hook 框架的核心实现机制。

---

## 目录

- [1. Inline Hook 基本原理](#1-inline-hook-基本原理)
- [2. Frida-Gum 拦截器架构](#2-frida-gum-拦截器架构)
- [3. Dobby Hook 框架架构](#3-dobby-hook-框架架构)
- [4. Hook 安装完整流程对比](#4-hook-安装完整流程对比)
- [5. Trampoline（跳板）构造](#5-trampoline跳板构造)
- [6. 指令重定位（Instruction Relocation）](#6-指令重定位instruction-relocation)
- [7. 代码补丁机制（Code Patching）](#7-代码补丁机制code-patching)
- [8. 可执行内存管理](#8-可执行内存管理)
- [9. 线程安全](#9-线程安全)
- [10. 各架构差异对比](#10-各架构差异对比)
- [11. 关键数据结构汇总](#11-关键数据结构汇总)
- [12. 执行流示意](#12-执行流示意)

---

## 1. Inline Hook 基本原理

Inline hook（内联钩子）通过**直接修改目标函数入口处的机器码**，将执行流重定向到用户提供的回调函数中。核心步骤：

1. **保存原始指令** — 备份目标函数入口处的 N 字节
2. **构建 Trampoline** — 包含被覆盖指令的重定位副本 + 跳回原函数剩余代码的分支
3. **覆盖入口** — 写入跳转到 hook 函数的指令
4. **回调原始** — 用户通过调用 trampoline 执行原始函数逻辑

```
┌──────────────────────────────────────────────────┐
│  Target Function (被 hook 后)                      │
│  ┌─────────────────────────────┐                 │
│  │  JMP → hook_func           │  ← 被覆盖的入口   │
│  └─────────────────────────────┘                 │
│  │  ... 原始代码继续 ...        │                 │
│  └─────────────────────────────┘                 │
└──────────────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────┐
│  hook_func (用户回调)                              │
│  → 调用 orig_func (即 trampoline)                  │
└──────────────────────────────────────────────────┘
         │
         ▼
┌──────────────────────────────────────────────────┐
│  Trampoline                                       │
│  ┌─────────────────────────────┐                 │
│  │  重定位后的原始指令            │                 │
│  │  JMP → target + overwritten │  ← 跳回原函数     │
│  └─────────────────────────────┘                 │
└──────────────────────────────────────────────────┘
```

---

## 2. Frida-Gum 拦截器架构

### 2.1 目录结构

```
frida-gum/gum/
├── guminterceptor.h          # 公共 API: attach/detach/replace/revert
├── guminterceptor.c           # 核心逻辑: instrument/activate/deactivate
├── guminterceptor-priv.h      # 内部类型: GumFunctionContext
├── arch-arm/                  # ARM 汇编写入器 + 重定位器
│   ├── gumarmwriter.c/h
│   ├── gumarmrelocator.c/h
│   ├── gumthumbwriter.c/h
│   └── gumthumbrelocator.c/h
├── arch-arm64/                # ARM64 汇编写入器 + 重定位器
│   ├── gumarm64writer.c/h
│   └── gumarm64relocator.c/h
├── arch-x86/                  # x86/x64 汇编写入器 + 重定位器
│   ├── gumx86writer.c/h
│   └── gumx86relocator.c/h
├── backend-arm64/             # ARM64 后端: trampoline 构造 + enter/leave thunk
│   ├── guminterceptor-arm64.c
│   └── guminterceptor-arm64-glue.S
├── backend-arm/               # ARM 后端 (ARM + Thumb 双模式)
│   └── guminterceptor-arm.c
├── backend-x86/               # x86/x64 后端
│   └── guminterceptor-x86.c
├── gumcodeallocator.c/h       # 可执行内存池分配器
├── gumcodesegment.c/h         # 平台相关的可执行内存段
├── gummemory.c/h              # 底层内存补丁
├── gumspinlock.c/h            # 自旋锁
├── guminvocationcontext.c/h   # 调用上下文
└── guminvocationlistener.c/h  # 监听器接口
```

### 2.2 核心数据结构

#### `GumInterceptor`（拦截器实例）

```c
struct _GumInterceptor {
  GObject parent;
  GRecMutex mutex;                    // 递归互斥锁
  GHashTable * function_by_address;   // 地址 → GumFunctionContext 映射
  GumInterceptorBackend * backend;    // 架构特定后端
  GumCodeAllocator allocator;         // 可执行内存池
  volatile guint selected_thread_id;  // 用于 ignore_other_threads
  GumInterceptorTransaction current_transaction;
};
```

#### `GumFunctionContext`（每个被 hook 函数的上下文）

```c
struct _GumFunctionContext {
  gpointer function_address;           // 原函数地址
  GumCodeSlice * trampoline_slice;     // 分配的可执行内存
  GumCodeDeflector * trampoline_deflector; // 短距离跳转桥接
  volatile gint trampoline_usage_counter;  // 原子引用计数
  gpointer on_enter_trampoline;        // on-enter trampoline 地址
  guint8 overwritten_prologue[32];     // 保存的原始字节
  guint overwritten_prologue_len;      // 被覆盖字节数
  gpointer on_invoke_trampoline;       // 原函数继续执行的地址
  gpointer on_leave_trampoline;        // on-leave trampoline 地址
  volatile GPtrArray * listener_entries; // RCU 风格监听器列表
  gpointer replacement_function;       // 替换函数地址
  gint scratch_register;               // 使用的临时寄存器
};
```

### 2.3 Hook 安装流程

```
gum_interceptor_attach()
  │
  ├─ 1. 获取互斥锁 (GUM_INTERCEPTOR_LOCK)
  ├─ 2. 开始事务 (begin_transaction)
  ├─ 3. gum_interceptor_resolve() — 解析地址（去 PAC、跟踪 PLT 桩）
  └─ 4. gum_interceptor_instrument()
       │
       ├─ a. 检查是否已被 hook（哈希表查找）
       ├─ b. 懒创建后端: _gum_interceptor_backend_create()
       ├─ c. 分配 GumFunctionContext
       ├─ d. 创建 trampoline: _gum_interceptor_backend_create_trampoline()
       │     │
       │     ├─ Prepare: 确定可安全重定位的字节数
       │     ├─ 分配 GumCodeSlice（可执行内存）
       │     ├─ 写入 on_enter / on_leave / on_invoke trampoline
       │     └─ 重定位原始指令
       │
       ├─ e. 注册到哈希表
       └─ f. 调度激活更新
```

### 2.4 Trampoline 内存布局（ARM64）

```
[on_enter_trampoline]:
  LDR X16, =GumFunctionContext*    ; 加载上下文指针
  STR X16, [SP, #-16]!            ; 压栈
  LDR X16, =enter_thunk_address   ; 加载 enter thunk 地址
  BR X16                           ; 跳转到 enter thunk

[on_leave_trampoline]:
  LDR X16, =GumFunctionContext*
  STR X16, [SP, #-16]!
  LDR X16, =leave_thunk_address
  BR X16

[on_invoke_trampoline]:
  <重定位后的原始指令>
  LDR X16, =function_address + reloc_bytes  ; 跳回原函数
  BR X16
```

### 2.5 Enter/Leave Thunk 机制

**Enter Thunk**（ARM64 汇编，`guminterceptor-arm64-glue.S`）：

```
enter_thunk:
  ; 保存完整 CPU 上下文
  PUSH q0-q31          ; 512 字节 (NEON)
  PUSH FP, LR
  PUSH X1-X28           ; 224 字节
  MRS X1, NZCV
  PUSH X1, X0           ; 状态寄存器 + X0
  PUSH XZR, SP          ; PC 占位 + SP

  ; 调用 C 分发函数
  BL _gum_function_context_begin_invocation

  ; 恢复上下文并跳转到 next_hop
  LDR X16, [SP, #next_hop_offset]
  BR X16
```

`_gum_function_context_begin_invocation()` 核心逻辑：
1. 原子递增 `trampoline_usage_counter`
2. 检查 TLS guard 防止重入
3. 检查 `ignore_level`（当前线程忽略）
4. 检查 `selected_thread_id`（其他线程忽略）
5. 压入调用栈
6. 调用每个 listener 的 `on_enter()`
7. 设置 `*next_hop` 为 `replacement_function` 或 `on_invoke_trampoline`
8. 将返回地址替换为 `on_leave_trampoline`

**Leave Thunk**：函数返回时跳到 `on_leave_trampoline`，调用 `on_leave()` 回调后恢复原始返回地址。

### 2.6 重定向指令写入（ARM64）

| 距离 | 大小 | 指令 |
|------|------|------|
| +/-128MB | 4 字节 | `B on_enter_trampoline` |
| +/-4GB 页 | 8 字节 | `ADRP X16, page; BR X16` |
| 任意距离 | 16 字节 | `LDR X16, =addr; BR X16` |
| 需要中转 | 额外 | 通过 `GumCodeDeflector`（代码洞）中转 |

---

## 3. Dobby Hook 框架架构

### 3.1 目录结构

```
Dobby/
├── include/dobby.h                        # 公共 API
├── source/
│   ├── dobbie.cpp                         # API 入口
│   ├── Interceptor.h                      # 拦截器 (全局单例 gInterceptor)
│   ├── InterceptRouting/
│   │   ├── InterceptRouting.h             # 路由基类
│   │   ├── InlineHookRouting.h            # 内联 hook 路由
│   │   ├── InstrumentRouting.h            # 插桩路由
│   │   └── NearBranchTrampoline/          # 近分支跳板
│   │       └── near_trampoline_arm64.cc
│   ├── TrampolineBridge/
│   │   ├── Trampoline/                    # 各架构 trampoline 构造
│   │   │   ├── trampoline_arm64.cc
│   │   │   ├── trampoline_arm.cc
│   │   │   ├── trampoline_x86.cc
│   │   │   └── trampoline_x64.cc
│   │   └── ClosureTrampolineBridge/       # 闭包 trampoline (用于 Instrument)
│   ├── InstructionRelocation/             # 指令重定位
│   │   ├── arm64/InstructionRelocationARM64.cc
│   │   ├── arm/InstructionRelocationARM.cc
│   │   ├── x86/InstructionRelocationX86Shared.cc
│   │   └── x64/InstructionRelocationX64.cc
│   ├── core/
│   │   ├── assembler/                     # 汇编器
│   │   ├── codegen/                       # 代码生成器
│   │   └── arch/                          # 架构相关常量/寄存器定义
│   ├── MemoryAllocator/                   # 内存管理
│   │   ├── MemoryAllocator.h              # 全局内存分配器
│   │   ├── NearMemoryAllocator.h          # 近距离内存分配器
│   │   └── AssemblerCodeBuilder.h
│   └── Backend/UserMode/ExecMemory/       # 代码补丁工具
│       ├── code-patch-tool-posix.cc
│       ├── code-patch-tool-darwin.cc
│       └── code-patch-tool-windows.cc
```

### 3.2 核心 API

```c
// 内联 hook — 替换函数入口
int DobbyHook(void *address, void *fake_func, void **out_origin_func);

// 二进制插桩 — 在目标地址前插入回调
int DobbyInstrument(void *address, dobby_instrument_callback_t pre_handler);

// 销毁 hook
int DobbyDestroy(void *address);

// 启用/禁用近跳板优化 (ARM64)
void dobby_set_near_trampoline(bool enable);
```

### 3.3 DobbyHook 完整调用链

```
DobbyHook(addr, fake_func, &orig_func)
  │
  ├─ 1. 地址预处理
  │     ├─ ARM64: 去除 PAC (Pointer Authentication Code)
  │     └─ Android: 确保目标内存页可读
  │
  ├─ 2. 重复 hook 检查 (gInterceptor 查找)
  │
  ├─ 3. 创建 Entry + Routing
  │     ├─ Interceptor::Entry { addr, fake_func, patched, relocated, origin_code_ }
  │     └─ InlineHookRouting (TrampolineTarget() = fake_func)
  │
  ├─ 4. BuildRouting()
  │     ├─ GenerateTrampoline()
  │     │     ├─ [近跳板] GenerateNearTrampolineBuffer()
  │     │     └─ [普通跳板] GenerateNormalTrampolineBuffer()
  │     │
  │     ├─ GenerateRelocatedCode()
  │     │     └─ GenRelocateCodeAndBranch() — 重定位 + 回跳
  │     │
  │     └─ BackupOriginCode() — 保存原始字节
  │
  ├─ 5. Active() → DobbyCodePatch() — 原子覆盖目标入口
  │
  └─ 6. 返回 orig_func = entry->relocated.addr()
```

### 3.4 InstrumentRouting 路由

Instrument 模式通过 **ClosureTrampoline** 桥接到 C++ 分发函数：

```
[目标入口] → trampoline → ClosureTrampoline
                                  │
                                  ├─ 保存完整寄存器上下文 (DobbyRegisterContext)
                                  ├─ 调用 instrument_routing_dispatch()
                                  │     └─ 调用用户的 pre_handler(addr, ctx)
                                  ├─ 设置 TMP_REG_0 = relocated.addr()
                                  └─ 恢复寄存器 → BR TMP_REG_0
                                        │
                                        ▼
                                  [重定位后的原始指令]
                                        │
                                        ▼
                                  [跳回原函数剩余代码]
```

### 3.5 Closure Bridge 寄存器保存/恢复（ARM64）

```
closure_bridge:
  ; 保存
  PUSH q0-q7        ; 128 字节
  PUSH X1-X30       ; 240 字节
  PUSH X0            ; 8 字节
  PUSH SP            ; 原始 SP

  ; 调用分发
  BL common_closure_bridge_handler

  ; 恢复 (逆序)
  POP all registers
  RET                ; TMP_REG_0 已被设置为下一跳地址
```

---

## 4. Hook 安装完整流程对比

| 阶段 | Frida-Gum | Dobby |
|------|-----------|-------|
| **入口 API** | `gum_interceptor_attach()` | `DobbyHook()` |
| **线程安全** | 递归互斥锁 + 事务系统 | 无显式锁（依赖指令原子性） |
| **地址解析** | 去 PAC、跟踪 PLT 桩、解析重定向 | 去 PAC、确保内存可读 |
| **内存分配** | `GumCodeAllocator` 池分配 | `gMemoryAllocator` / `gNearMemoryAllocator` |
| **Trampoline 构造** | 后端架构特定代码 | `TrampolineBridge` + `CodeGen` |
| **指令重定位** | `GumArm64Relocator` 等各架构实现 | `relo_ctx_t::relocate()` 各架构实现 |
| **代码写入** | 事务批量提交 `gum_memory_patch_code_pages()` | `DobbyCodePatch()` 直接 mprotect+memcpy |
| **回调机制** | `on_enter` / `on_leave` 监听器 | 直接跳转到 fake_func |
| **原始调用** | `on_invoke_trampoline` | `entry->relocated.addr()` |
| **函数替换** | `gum_interceptor_replace()` | 通过 fake_func 内部调用 orig_func |

---

## 5. Trampoline（跳板）构造

### 5.1 ARM64 跳板类型

| 类型 | Frida-Gum | Dobby | 大小 |
|------|-----------|-------|------|
| **短跳转 (B)** | `B target` (+/-128MB) | `B target` | 4 字节 |
| **ADRP+BR** | `ADRP X16, page; BR X16` | `ADRP X17, page; ADD X17, X17, off; BR X17` | 8 / 12 字节 |
| **LDR+BR** | `LDR X16, =addr; BR X16` | `LDR X17, =addr; BR X17; .quad addr` | 16 / 20 字节 |
| **近跳板+中转** | `GumCodeDeflector`（代码洞） | `ForwardTrampoline`（近距离可执行内存） | 额外 |

### 5.2 ARM (32-bit) 跳板

| 类型 | Frida-Gum | Dobby |
|------|-----------|-------|
| **ARM 模式** | `LDR PC, =target` | `LDR PC, [PC, #-4]; .word target` |
| **Thumb 模式** | `LDR.W PC, =target` | `LDR.W PC, [PC, #0]; .word target` |
| **短跳转** | `B target` (ARM/Thumb) | 类似 |

### 5.3 x86/x64 跳板

| 类型 | Frida-Gum | Dobby |
|------|-----------|-------|
| **x86** | `JMP rel32` (5 字节) | `JMP rel32` (5 字节) |
| **x64 近跳** | `JMP rel32` (5 字节, +/-2GB) | `JMP [RIP+disp32]` (6 字节 + 8 字节 stub) |
| **x64 远跳** | `JMP [RIP+2]; .quad target` (14 字节) | 同上 |

### 5.4 Dobby 近跳板机制（ARM64）

近跳板优化尝试在目标地址 +/-128MB 范围内分配可执行内存：

```
情况1: 目标在 B 范围内 (+/-128MB)
  [目标入口] → B <trampoline>    (4 字节)

情况2: 不在 B 范围内
  [目标入口] → B <forward_tramp>  (4 字节, 跳到近距离中转站)
  [forward_tramp] → LDR X17, =target; BR X17  (中转到远距离)
```

ForwardTrampoline 通过 `gNearMemoryAllocator` 在 B 指令可达范围内分配可执行内存。

---

## 6. 指令重定位（Instruction Relocation）

指令重定位是 inline hook 中**最复杂**的部分。当目标函数入口被覆盖时，被覆盖的指令必须在新地址正确执行，所有 PC-relative 引用都需要修正。

### 6.1 ARM64 指令重定位

| 原始指令 | 重定位策略 | 大小变化 |
|----------|-----------|---------|
| `B target` | `LDR X16, =target; BR X16` | 4→12 字节 |
| `BL target` | `LDR LR, =target; BLR LR` | 4→12 字节 |
| `B.cond target` | `B.cond.inverted skip; LDR X16,=target; BR X16; skip:` | 4→20+ 字节 |
| `CBZ/CBNZ reg, target` | 同 B.cond 模式 | 4→16+ 字节 |
| `TBZ/TBNZ reg, bit, target` | 同 B.cond 模式 | 4→16+ 字节 |
| `LDR Xt, [PC, #off]` | `LDR X16, =abs_addr; LDR Xt, [X16]` | 4→8 字节 |
| `ADR Xd, label` | `LDR Xd, =abs_addr` | 4→8 字节 |
| `ADRP Xd, page` | `LDR Xd, =abs_page_addr` | 4→8 字节 |
| 其他指令 | 直接复制 | 不变 |

**Frida-Gum 额外处理**：
- NEON 浮点 LDR (S/D/Q)：使用临时寄存器保存/恢复
- `dlopen()` 风格 thunk 中 LR 的特殊重写

**Dobby 处理**：
- `Mov` 指令使用 4 条 `movz`/`movk` 序列加载完整 64 位立即数
- 数据标签 (`RelocDataLabel`) 在代码末尾嵌入绝对地址数据

### 6.2 ARM (32-bit) 指令重定位

ARM 32-bit 因同时存在 ARM 和 Thumb 两种指令集，重定位**极为复杂**：

**ARM 模式关键处理**：
- `LDR literal` → 加载地址 + 间接加载
- `ADR` → 替换为 `LDR Rd, [data_label]`
- `B/BL/BLX` → 创建小桩代码，含数据标签

**Thumb-1 (16-bit) 关键处理**：
- `BX PC` → 记录 ARM/Thumb 状态转换到 `execute_state_map`
- `B.cond` → 反转条件跳过修正代码 + 数据标签
- `CBZ/CBNZ` → 同上

**Thumb-2 (32-bit) 关键处理**：
- `B.cond (T3)` → 反转条件 + `b.w` 跳过 + 数据标签
- `BL (T1)` / `BLX (T2)` → 类似模式
- `ADR` / `LDR literal` → 加载绝对地址

**Dobby 特有**：`relo_ctx_t` 维护 `execute_state_map` 跟踪 ARM/Thumb 转换，在转换点切换汇编器。

### 6.3 x86/x64 指令重定位

| 指令 | x86 策略 | x64 策略 |
|------|---------|---------|
| `Jcc rel8` | 升级为 `Jcc rel32` (0x0F 0x80+cond) | 短 Jcc + `JMP [RIP]` + 绝对地址 |
| `JMP rel8` | 升级为 `JMP rel32` (0xE9) | `JMP [RIP]` + 8 字节绝对地址 |
| `CALL rel32` | 重写 rel32 偏移 | `CALL [RIP+2]; JMP +8; .quad target` |
| `JMP rel32` | 重写 rel32 偏移 | `JMP [RIP]; .quad target` |
| RIP-relative (x64) | N/A | **分配近原始目标的可执行块，原指令修正偏移后放入，再加跳回** |
| 其他 | 直接复制 | 直接复制 |

**x64 RIP-relative 修正细节（Dobby）**：

```
原始: mov rax, [rip+offset]  (位于 0x1000)

重定位后:
  [relocated_code]:
    JMP [RIP]; .quad <rip_insn_seq_addr>    ; 跳到修正块

  [rip_insn_seq] (分配在原始指令附近):
    mov rax, [rip+corrected_offset]         ; 修正后的偏移
    JMP [RIP]; .quad <return_to_relo>       ; 跳回重定位代码
```

这种"跳到原处执行再跳回"的策略确保 RIP-relative 内存访问仍然正确。

---

## 7. 代码补丁机制（Code Patching）

### 7.1 Frida-Gum 事务化补丁

Frida 使用**事务系统**批量应用代码修改：

1. 所有修改在事务中累积为 `GumUpdateTask`
2. 事务提交时按目标页地址排序
3. 调用 `gum_memory_patch_code_pages()` 原子修改所有页

**补丁策略**（按平台优先级）：

| 策略 | 平台 | 原理 |
|------|------|------|
| **mmap MAP_FIXED** | Linux/Android | 创建可写别名页，写入后别名替换原始页，对其他线程原子 |
| **mprotect RWX** | 支持 RWX 的系统 | 直接修改权限、写入、恢复 |
| **Code Segment** | 非 RWX 系统 | 使用 memfd_create (Linux) 或 mach-o (macOS) 创建可执行段 |
| **线程暂停** | 非 RWX 回退 | 暂停所有其他线程 → 修改 → 恢复线程 |

### 7.2 Dobby 平台补丁

**POSIX (Linux/Android)**：
```c
DobbyCodePatch(address, buffer, size):
  mprotect(page, size, R|W|X)     // 设为可写
  memcpy(address, buffer, size)    // 写入
  mprotect(page, size, R|X)       // 恢复
  ClearCache(address, size)        // 刷新指令缓存 (ARM 关键!)
```

**Darwin (macOS/iOS)**：
```c
// 使用 vm_protect + Copy-on-Write
vm_protect(task, page, VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY)
memcpy(...)
vm_protect(task, page, VM_PROT_READ|VM_PROT_EXECUTE)
```
动态解析 `vm_protect` 以绕过新版 iOS 共享缓存限制。

**Windows**：
```c
VirtualProtect(page, size, PAGE_EXECUTE_READWRITE, &old)
memcpy(...)
VirtualProtect(page, size, old, &old)
```

### 7.3 指令缓存刷新

ARM/ARM64 有独立的指令缓存和数据缓存。修改代码后**必须**刷新：

- **Frida**: `gum_clear_cache()` 在所有脏页上执行缓存维护
- **Dobby**: `ClearCache()` 调用 `__builtin___clear_cache()` 或平台特定系统调用

---

## 8. 可执行内存管理

### 8.1 Frida-Gum 内存分配

```
GumCodeAllocator (池分配器)
  │
  ├─ Slice 大小: 256 字节
  ├─ 批次分配: 7 页/批次，切分为 slices
  │
  ├─ 分配策略 (按优先级):
  │   1. RWX 页直接分配
  │   2. RX 页 + 可写别名重映射
  │   3. Code Segment (memfd / mach-o)
  │   4. RW 页 → commit 时改 RX
  │
  ├─ 近距离分配: gum_code_allocator_try_alloc_slice_near()
  │   ├─ 扫描现有空闲 slices
  │   └─ gum_try_alloc_n_pages_near() 在范围内分配新页
  │
  └─ Code Deflector (代码洞中转):
      ├─ 在现有模块头部的填充区寻找空洞
      ├─ Darwin: __TEXT+4096-24 处的填充
      ├─ Linux/ELF: module_base+8 处
      └─ 写入跳转到实际 trampoline 的指令
```

### 8.2 Dobby 内存分配

```
gMemoryAllocator (全局单例，池分配)
  │
  ├─ 每页一个 simple_linear_allocator_t (bump allocator)
  ├─ 页满时分配新 OS 页: OSMemory::Allocate() → mprotect(R|X)
  └─ allocExecBlock() / allocDataBlock()

gNearMemoryAllocator (近距离分配，三级搜索)
  │
  ├─ 第1级: 遍历现有页分配器，检查空闲空间是否在范围内
  ├─ 第2级: 查询进程内存布局，在已映射区域间的间隙中分配
  │         ProcessRuntime::getMemoryLayout() → 在 gap 中分配 OS 页
  └─ 第3级: 扫描现有可执行区域中的零字节填充
            (memmem_impl 查找连续零字节)
```

**Dobby 支持自定义分配回调**：`dobby_register_alloc_near_code_callback()` 允许用户覆盖整个近距离分配策略。

---

## 9. 线程安全

### 9.1 Frida-Gum（高度安全）

| 机制 | 说明 |
|------|------|
| **递归互斥锁** | `GRecMutex` 保护所有数据结构修改 |
| **事务系统** | 嵌套事务 + 延迟更新 + 批量原子应用 |
| **线程暂停** | 非 RWX 平台在补丁期间暂停所有其他线程 |
| **TLS 重入保护** | `gum_interceptor_guard_key` 防止回调中递归触发 |
| **原子引用计数** | `trampoline_usage_counter` 防止 trampoline 使用中被释放 |
| **RCU 监听器** | `listener_entries` 原子指针交换，无锁读取 |
| **调用栈** | 每线程 `GumInvocationStack`，最大深度 32 |
| **线程过滤** | `ignore_current_thread` / `ignore_other_threads` |

### 9.2 Dobby（轻量级）

| 机制 | 说明 |
|------|------|
| **指令原子性** | ARM64 的 4 字节 `B` 指令本身是原子的 |
| **TLS 调用栈** | `ThreadSupport::CurrentThreadCallStack()` 管理每线程栈帧 |
| **无全局锁** | 依赖小尺寸 trampoline 的写入原子性 |

> **注意**：Dobby 在 ARM64 上写入超过 4 字节的 trampoline 时，存在短暂的竞态窗口。这是轻量级框架的已知权衡。

---

## 10. 各架构差异对比

### 10.1 临时寄存器约定

| 架构 | Frida-Gum | Dobby | 说明 |
|------|-----------|-------|------|
| ARM64 | X16/X17 (可配置) | X17 (TMP_REG_0) | ARM64 ABI 保留的 IP 寄存器 |
| ARM | R6 | R12 (IP) | ARM ABI 保留的临时寄存器 |
| x86 | 无 | 无 | 使用相对寻址，无需临时寄存器 |
| x64 | 无 | 无 | 使用 RIP-relative 寻址 |

### 10.2 最小覆盖字节数

| 架构 | Frida-Gum | Dobby | 说明 |
|------|-----------|-------|------|
| ARM64 | 4 字节 (B) | 4 字节 (B) | 近跳板时可用 |
| ARM64 远跳 | 16 字节 (LDR+BR) | 12/20 字节 | ADRP+ADD+BR 或 LDR+BR+literal |
| ARM | 4 字节 | 8 字节 | LDR PC 模式 |
| Thumb | 4-8 字节 | 8 字节 | LDR.W PC 模式 |
| x86 | 5 字节 | 5 字节 | JMP rel32 |
| x64 | 5 字节 (近) | 14 字节 | JMP rel32 或 JMP [RIP]+addr |

### 10.3 指令重定位复杂度

| 架构 | 复杂度 | 主要挑战 |
|------|--------|---------|
| **ARM64** | 中等 | ADR/ADRP/LDR literal 等 PC-relative 指令，但指令定长 (4字节) |
| **ARM** | 高 | ARM/Thumb 双指令集 + 互操作切换 + 16/32 位 Thumb 混合 |
| **x86** | 中等 | 变长指令需完整解码，但仅相对跳转需修正 |
| **x64** | 高 | RIP-relative 寻址广泛使用，需"回原处执行"策略 |

---

## 11. 关键数据结构汇总

### Frida-Gum

| 结构 | 文件 | 用途 |
|------|------|------|
| `GumInterceptor` | `guminterceptor.c` | 拦截器实例，持有全局状态 |
| `GumFunctionContext` | `guminterceptor-priv.h` | 每个 hook 函数的完整上下文 |
| `GumCodeSlice` | `gumcodeallocator.h` | 一块可执行内存 (默认 256 字节) |
| `GumCodeDeflector` | `gumcodeallocator.h` | 代码洞中转跳转 |
| `GumArm64Writer` | `arch-arm64/gumarm64writer.h` | ARM64 指令写入器 |
| `GumArm64Relocator` | `arch-arm64/gumarm64relocator.h` | ARM64 指令重定位器 |
| `GumInvocationStack` | `guminterceptor-priv.h` | 每线程调用栈 (最大 32 层) |

### Dobby

| 结构 | 文件 | 用途 |
|------|------|------|
| `Interceptor::Entry` | `source/Interceptor.h` | 被 hook 函数的注册信息 |
| `Interceptor` | `source/Interceptor.h` | 全局单例 hook 注册表 |
| `InterceptRouting` | `InterceptRouting.h` | 路由基类 (trampoline + 重定位 + 激活) |
| `InlineHookRouting` | `InlineHookRouting.h` | 内联 hook 路由 |
| `InstrumentRouting` | `InstrumentRouting.h` | 插桩路由 (通过 ClosureTrampoline) |
| `Trampoline` | `Trampoline.h` | 跳板类型 + 代码缓冲区 |
| `ClosureTrampoline` | `ClosureTrampoline.h` | 带回调数据的闭包跳板 |
| `MemBlock` / `MemRange` | `MemoryAllocator.h` | 内存区域表示 |
| `RelocDataLabel` | `pseudo_label.h` | 代码中嵌入的数据标签 |
| `DobbyRegisterContext` | `include/dobby.h` | CPU 寄存器上下文 |

---

## 12. 执行流示意

### 12.1 Frida-Gum 完整执行流

```
caller 调用 target_func()
  │
  ▼
[target_func 入口] ──────────────────────────────┐
  被覆盖为: B on_enter_trampoline               │
  │                                              │
  ▼                                              │
[on_enter_trampoline]                            │
  加载 GumFunctionContext*                       │
  跳转到 enter_thunk                             │
  │                                              │
  ▼                                              │
[enter_thunk] (汇编)                             │
  保存全部 CPU 上下文 (x0-x28, fp, lr, q0-q31)  │
  调用 _gum_function_context_begin_invocation()  │
  │                                              │
  ├─ 检查 TLS guard (防重入)                     │
  ├─ 检查线程过滤                                │
  ├─ 调用 listener->on_enter()                   │
  ├─ 设置 next_hop                               │
  └─ 替换返回地址 → on_leave_trampoline          │
  │                                              │
  ▼                                              │
[next_hop]                                       │
  ├─ replacement_function (如果 replace)         │
  └─ on_invoke_trampoline (执行原始逻辑)  ◄─────┘
       │
       ├─ 执行重定位后的原始指令
       └─ 跳回 target_func + overwritten_bytes
              │
              ▼
         [原始函数剩余代码执行...]
              │
              ▼ (函数返回)
[on_leave_trampoline]
  加载 GumFunctionContext*
  跳转到 leave_thunk
  │
  ▼
[leave_thunk]
  保存 CPU 上下文
  调用 _gum_function_context_end_invocation()
    ├─ 弹出调用栈
    ├─ 调用 listener->on_leave()
    └─ 恢复原始返回地址
  恢复 CPU 上下文
  跳转到原始返回地址 → caller 继续执行
```

### 12.2 Dobby 完整执行流

```
caller 调用 target_func()
  │
  ▼
[target_func 入口] ────────────────────────────┐
  被覆盖为: 跳转到 fake_func 的 trampoline     │
  │                                            │
  ▼                                            │
[trampoline → fake_func]                       │
  │                                            │
  ▼                                            │
[fake_func (用户回调)]                          │
  │                                            │
  ├─ 可选: 调用 *orig_func (即 trampoline)     │
  │      │                                     │
  │      ▼                             ◄───────┘
  │  [relocated_code]
  │      ├─ 重定位后的原始指令
  │      └─ JMP → target_func + overwritten_size
  │              │
  │              ▼
  │         [原始函数剩余代码执行...]
  │              │
  │              ▼
  │         返回到 fake_func
  │
  └─ fake_func 返回 → caller 继续执行
```

---

## 总结

| 特性 | Frida-Gum | Dobby |
|------|-----------|-------|
| **定位** | 全功能动态二进制插桩框架 | 轻量级 inline hook 框架 |
| **线程安全** | 高（互斥锁+事务+线程暂停+RCU） | 轻量（依赖原子写入） |
| **回调模型** | on_enter / on_leave 监听器 | 直接跳转到替换函数 |
| **内存管理** | 池分配器 + 代码洞中转 | 池分配器 + 三级近距离搜索 |
| **指令重定位** | 架构特定 Relocator 类 | relo_ctx_t 架构特定实现 |
| **跨平台** | Linux/Android/macOS/iOS/Windows | 同上 |
| **支持架构** | ARM/ARM64/x86/x64/MIPS | ARM/ARM64/x86/x64 |
| **代码量** | 大（完整插桩框架） | 小（专注 hook） |
| **适用场景** | 动态分析、逆向工程、安全研究 | 游戏/应用 hook、轻量注入 |
