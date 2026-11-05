# rvdl 语言手册

> Ravendrel（`.rvdl`）是一门面向「量子计算 + 神经接口 + 量子语言模型 + 量子机器人」的实验性编程语言。
> 本手册是 rvdl 语言的完整参考，覆盖词法、类型、控制流、函数与契约、量子操作、模块系统、类型定义与静态验证。
> 运行环境与工具链见 [README.md](../README.md)，量子学习见 [rvdl 量子学习手册](./rvdl-quantum-learning-manual.md)。

---

## 目录

1. [概述](#1-概述)
2. [快速上手](#2-快速上手)
3. [词法结构](#3-词法结构)
4. [类型系统](#4-类型系统)
5. [变量与赋值](#5-变量与赋值)
6. [运算符](#6-运算符)
7. [控制流](#7-控制流)
8. [函数](#8-函数)
9. [契约与静态验证](#9-契约与静态验证)
10. [量子类型与操作](#10-量子类型与操作)
11. [内置类](#11-内置类)
12. [内置函数](#12-内置函数)
13. [模块系统（`.rvdli`）](#13-模块系统mmi)
14. [类型定义（form / trait / impl / template）](#14-类型定义form--trait--impl--template)
15. [系统级编程（cap / unsafe / native）](#15-系统级编程cap--unsafe--native)
16. [并发与纠缠（spawn / entangle）](#16-并发与纠缠spawn--entangle)
17. [借用检查与线性类型](#17-借用检查与线性类型)
18. [编译管线与运行](#18-编译管线与运行)
19. [附录：关键字与错误类型](#19-附录关键字与错误类型)

> 专题文档：[穿梭与追踪：`@layer` 的两个可选机制](./rvdl-shuttle-tracking.md)

---

## 1. 概述

rvdl 的编译管线为：

```
Lexer（词法）→ Parser（语法）→ SemanticAnalyzer（语义）
    → 门合成 gate-synth（@[undo]/@[steer] 可逆编织 → <name>_undo/_steer）
    → TopologyBuilder（@layer 拓扑聚合 + 平行/叠加推导 + 调用传播延迟）
    → MIR lowering（中间表示）→ BorrowChecker（借用检查 + 量子线性类型 QLT）
    → Q-Digest（静态竞争检测）→ IRGenerator（LLVM IR）
    → TCP Daemon（:50052）→ LLVM ORC JIT → TopoScheduler（分层拓扑调度）
    → 硬件探测 → QM（真实量子机）/ QVM（本地模拟器）
```

- 源文件扩展名：`.rvdl`
- 入口函数：多维标签函数 `@layer(time, thread, coord)`（无单一 `main`；脚本模式下顶层语句仍隐式包裹进 `rvdl_main`）
- 类型系统：静态类型 + `let`/`auto` 局部类型推导
- 量子语义：静态强制「不可克隆定理」「测量后坍缩」等量子约束（量子线性类型 QLT）
- 所有权：借用检查，杜绝借用冲突与悬垂借用
- 系统级：`cap` 能力指针 / `unsafe` / 内联汇编，支持裸机与内核编程

---

## 2. 快速上手

```rvdl
// hello.rvdl
@layer(time=0, thread=0, coord=(0))
int32 main() {
    int32 x = 42;
    int32 y = x + 8;
    return y;                     // 返回 50
}
```

```bash
rvdl run hello.rvdl        # 运行脚本
rvdl ir  hello.rvdl        # 仅输出 LLVM IR
```

任何没有显式函数定义的顶层脚本也可运行（脚本模式，隐式包裹为 `rvdl_main`）：

```rvdl
let q = alloc();
h(q);
int32 r = measure(q);
```

---

## 3. 词法结构

### 3.1 注释

仅支持行注释：

```rvdl
// 这是注释
int32 a = 1;   // 行尾注释
```

### 3.2 标识符

标识符以字母（含 Unicode 字母）或下划线 `_` 开头，后续可含字母、数字、下划线。

```rvdl
let foo = 1;
let _bar = 2;
let 变量 = 3;    // 支持 Unicode 字母
```

### 3.3 数字

- 整数：`0` `42` `50000`
- 浮点数：`0.1` `3.14` `1.0`

整数字面量在语义层推导为 `int32`，含小数点的推导为 `double`。

### 3.4 字符串与转义

字符串支持单引号 `'...'` 与双引号 `"..."`，并支持常见转义序列：

| 转义 | 含义 |
| --- | --- |
| `\n` | 换行 |
| `\t` | 制表符 |
| `\r` | 回车 |
| `\\` | 反斜杠 |
| `\"` `\'` | 引号 |
| `\0` | 空字符 |

---

## 4. 类型系统

### 4.1 标量类型

| 类型 | LLVM 映射 | 说明 |
| --- | --- | --- |
| `int8` / `uint8` | `i8` | 8 位整数 |
| `int16` / `uint16` | `i16` | 16 位整数 |
| `int` / `int32` / `uint32` | `i32` | 32 位整数（`int` 是 `int32` 别名） |
| `int64` / `uint64` | `i64` | 64 位整数（`uint64` 走 `udiv`/`urem`/`ult`/`lshr` 无符号语义，承载 48 位 LBA / GPT 64 位字段） |
| `float` | `float` | 单精度浮点 |
| `double` | `double` | 双精度浮点 |
| `complex64` | `{ float, float }` | 单精度复数（量子态矢量运算） |
| `complex128` | `{ double, double }` | 双精度复数 |
| `string` | `i8*` | 字符串 |
| `char` | `i8` | 字符 |

### 4.2 量子类型

| 类型 | LLVM 映射 | 说明 |
| --- | --- | --- |
| `Qubit` | `%Qubit*`（opaque） | 单个量子比特，不可克隆 |
| `QObject` | `%QObject*`（opaque） | 量子对象（可容纳多 qubit 状态） |
| `QModel` | `%QModel*`（opaque） | 量子语言模型 |
| `QReservoir` | `%QReservoir*`（opaque） | 量子储备池（QRC，经 `qrc_new` 创建） |

### 4.3 类型推导

`let` 与 `auto` 等价，均触发局部类型推导：

```rvdl
auto q = alloc();          // Qubit
let n = 42;                // int32
let f = 3.14;              // double
let s = "ravendrel";           // string
let m = new BellState();   // QObject
```

### 4.4 能力指针（cap）

`cap<T>` 是 CHERI 式「能力」指针——地址 + 边界 + 权限 + 有效性的统一封装，用于系统级
编程（裸机 / 内核）。它在 LLVM 层降为普通指针 `T*`，语义层受借用检查与 `unsafe` 约束：

```rvdl
cap<int32> p = null;       // 等价于 int32*，初始为空
*p = 5;                    // 解引用写入
int32 x = *p;              // 解引用读取
```

> 裸指针解引用必须在 `unsafe { ... }` 块内（见 [第 15 节](#15-系统级编程cap--unsafe--native)）。

### 4.5 编译期常量（fixed）

`fixed` 声明编译期常量（`const` 的量子化命名，确定型范式）：

```rvdl
fixed int32 MAX = 100;
int32 x = MAX;             // 编译期求值
```

### 4.6 空值（null）

`null` 表示空指针 / 空能力，用于初始化 `cap<T>`，或作为 `fuse` 的通配模式（`_`）。

### 4.7 定长数组（arr<T, N>）

`arr<T, N>` 是定长数组类型，映射到 LLVM `[N x T]`，用于静态查找表（键盘扫描码表 /
异常名表）与磁盘结构内嵌保留区。数组字面量 `[e0, e1, ...]`，索引 `a[i]`：

```rvdl
fixed arr<int32, 4> SCANCODES = [0x1E, 0x30, 0x2E, 0x20];  // 全局查找表
arr<int32, 3> local = [10, 20, 30];                        // 局部定长数组
int32 code = SCANCODES[0];
int32 v = local[1];
```

### 4.8 函数指针（fn<ret(params)>）

`fn<ret(params)>` 是函数指针类型（与 `(params)->ret` 等价），映射到 LLVM `ret (params)*`，
支撑 blkdev 读写回调表、irq 中断回调、线程入口、QPU 后端虚表。取函数地址用 `&`，间接调用
直接 `cb(args)`：

```rvdl
fn<int32(int32)> cb = &handler;   // 取函数地址 → 函数指针
int32 r = cb(41);                 // 间接调用
cb = null;                        // 空函数指针
```

> 函数指针可降为原始字节指针 `cap<uint8>`（`cap<uint8> h = &handler`），供 IDT / 中断表等
> 直接存函数地址的场景；`addr(&fn)` 仍返回低 32 位整数地址。

### 4.9 复数类型（complex64 / complex128）

`complex64` / `complex128` 是复数类型（分量 `float` / `double`），映射到 LLVM `{ float, float }` /
`{ double, double }`，支撑量子态矢量运算。支持 `+` / `-` / `*`，以及构造与访问内建：

```rvdl
complex128 a = complex(1.0, 2.0);   // 构造（实部, 虚部）
complex128 b = complex(3.0, 4.0);
complex128 c = a + b;               // 复数加法
complex128 d = a * b;               // 复数乘法（4 次 fmul + 组合）
double re = real(c);                // 实部
double im = imag(c);                // 虚部
double m  = cabs(d);                // |z| = sqrt(re² + im²)
complex128 e = conj(a);             // 共轭（虚部取反）
```

---

## 5. 变量与赋值

```rvdl
// 变量声明（带显式类型）
int32 count = 0;
double lr = 0.01;
Qubit q = alloc();

// 类型推导
auto bell = new BellState();

// 重新赋值（类型必须一致）
count = count + 1;
```

类型不匹配会在语义分析阶段报错：

```rvdl
int32 a = "hello";   // Type Error: Cannot assign expression of type 'string' ...
```

### 5.1 一行多变量定义

支持单行声明多个同类型变量，后声明的变量可引用先声明的变量：

```rvdl
int32 a = 1, b = 2, c = a + b;   // a=1, b=2, c=3
double x = 0.1, y = x * 2;       // x=0.1, y=0.2
let s = "hi", t = "yo";          // let/auto 类型推导同样支持
```

顶层显式类型多定义降为多个全局变量（`int32 a = 1, b = 2;` → `@a`/`@b`）；
`let`/`auto` 多定义作为脚本模式局部变量。类型不匹配同样在语义分析阶段报错：

```rvdl
int32 a = 1, b = "hello";   // Type Error: Cannot assign expression of type 'string' ...
```

---

## 6. 运算符

### 6.1 算术

`+` `-` `*` `/`（整数除法为 `sdiv`，浮点为 `fdiv`）

### 6.2 比较

`<` `<=` `>` `>=` `==` `!=`（返回 `bool`/`i1`）

### 6.3 逻辑

`&&`（短路与）、`||`（短路或）、`!`（非）

```rvdl
if (a > 0 && b < 10) { /* ... */ }
```

### 6.4 一元

`-`（取负）、`!`（逻辑非）

---

## 7. 控制流

### 7.1 if / else

```rvdl
if (cond) {
    // ...
} else {
    // ...
}
```

### 7.2 while（含不变量与 else）

`while` 支持可选的 `invariant`（循环不变量，用于静态验证）与 `else` 分支：

```rvdl
int32 i = 0;
while (i < 100) {
    invariant i >= 0;
    i = i + 1;
} else {
    // 循环体一次都未执行时进入
}
```

### 7.3 for

标准三段式 `for`：

```rvdl
for (int32 i = 0; i < 10; i = i + 1) {
    // ...
}
```

### 7.4 break / continue

```rvdl
while (true) {
    if (done) break;
    continue;
}
```

> 语义分析器会检查 `break` / `continue` 是否位于循环之外。

### 7.5 route（路由分支）

`route` 是 `switch` 的量子化命名，把判别值路由到若干路径之一；`path` 列出取值分支，
`fallback` 提供兜底分支：

```rvdl
route (code) {
    path 0: {
        // code == 0
    }
    path 1: {
        // code == 1
    }
    fallback: {
        // 其余情况
    }
}
```

### 7.6 spin（自旋循环）

`spin` 是 `do-while` 的量子化命名，先执行循环体再判断条件，至少执行一次：

```rvdl
spin {
    // 循环体（至少执行一次）
} while (cond);
```

---

## 8. 函数

### 8.1 多维标签函数（入口）

rvdl 没有单一的 `main` 入口，而是用**多维标签函数**（`@layer`）声明可调度单元。每个显式函数必须
携带 `@layer(time, thread, coord)` 标签，写在函数上一行：

```rvdl
@layer(time=0, thread=0, coord=(0,0))
int32 main() {
    return 0;
}
```

三个维度的语义：

| 字段 | 必填 | 含义 |
| --- | --- | --- |
| `time` | 根块必填，子函数可省略 | 锚点时间：块在其 `coord` 时序链上的启动槽位；省略则继承调用者时钟 + Δt |
| `thread` | 必填 | 逻辑线程 id：同线程串行、异线程可并行 |
| `coord` | 必填 | N 维运行层坐标：块在多维拓扑空间的初始位置 |
| `cost` | 可选（默认 1） | 块自身的执行成本，影响父块逻辑时钟推进 |
| `deadline` | 可选 | 时间束缚上限 |
| `dual` | 可选 | `dual=1` 标记该块位于 T-对偶反转空间；同 coord 且带 `dual` 的块之间推导 `inversion` 边 |
| `shuttle` | 可选 | **穿梭**：目标 coord 列表，把本块数据按关联度在共享点前后双向渗透进关联层（见下） |
| `shuttle_lambda` | 可选（默认 1） | 穿梭关联度核尺度 `λ > 0`：`w = exp(−d_L1/λ)` |
| `shuttle_phase` | 可选（默认 2） | 渗透相对**目标层** time-slot 的时机：`0` 入口 / `1` 出口 / `2` 双向 |
| `shuttle_rounds` | 可选（默认 0 = auto） | 渗透轮次；auto 取 `⌈log₂ span⌉` |
| `shuttle_strategy` | 可选（默认 −1 = auto） | `−1` auto / `0` 蝶式 / `1` 环式 / `2` 树式 / `3` Grover 扩散 / `4` c001 蝶点 |
| `track` | 可选 | **追踪**：可观测量列表，在块的功能周期内转成经典影子再恢复（见下） |
| `track_mode` | 可选（默认 0 = auto） | 保真级别：`0` auto / `1` qnd（L0）/ `2` shadow（L1）/ `3` relabel（L2） |
| `track_slots` | 可选（默认 1） | 经典影子槽数 |

#### 8.1.1 穿梭与追踪（可选机制）

`shuttle` 与 `track` 是两个可选的 `@layer` 键。**不写则完全惰性**——
`@layer(time=0, thread=0, coord=(0))` 的语义与诊断结果与它们不存在时逐字节相同。

```rvdl
@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z, entropy))
int32 producer() { return 42; }

@layer(time=1, thread=0, coord=(1,0), track=(expect_z))
int32 consumer() { return 0; }
```

- **穿梭**按 L1 码距 `d_L1 = Σᵢ|aᵢ−bᵢ|` 定权 `w = exp(−d_L1/λ)`，在共享点前后双向渗透。
  该核**严格为正、不设截断阈值**（与注意力 softmax 根本不同）：
  哪怕关联度极小，对端仍留在调度计划内。
  线格式同时下发 `weight` 与 `logWeight`——`d/λ ≳ 746` 时线性权重会双精度下溢为 0，
  此时路由一律落到 `logWeight`，**数值零不等于语义截断**。
- **追踪**在块的功能周期 `[time, time+cost)` 内把可观测量转成经典影子槽、复制、再恢复原态。
  **单次软克隆在物理上不可能**，故采用三级保真阶梯（L0 精确 / L1 统计 / L2 自由），
  **不做统一精确承诺**。影子槽只承载经典标量，结构上不可能是 `QObject`
  ——这正是保 QLT 线性不变量 `E-Q001`（不可克隆）/ `E-Q002`（线性泄漏）的方式。

`shuttle` 的目标与 `track` 的来源必须是同一拓扑中的**关联块**；跨功能周期时
数据经**渗透载荷**（经典关联标量）通道传递，而非把影子槽的生命周期延长到周期之外。

完整规格（关联核、四策略代价表、auto 启发式、诊断码、线格式）见
[穿梭与追踪](./rvdl-shuttle-tracking.md)。

块间关系从标签**自动推导**（五类拓扑边）：

```text
coord 相同，time 相邻   → 叠加（stack），按 time 组成时序链
coord 不同              → 平行（parallel），空间并置
coord 维度不同          → 投影（projection），曲面体投影（不同 coord 维度共存）
coord 相同且 dual=1     → 反转（inversion），T-对偶反转空间
coord 相同、thread 不同 → 交叉（crossing）
thread 不同             → 可并行
```

块内**调用传播延迟**（逻辑时钟模型）：子函数的执行时刻 = 父函数锚点 `time` + 调用点之前的累计延迟
Δt（Δt = 前面语句的执行成本 δ 与前置子函数 `cost` 之和）。这对应量子电路的门时序与传播延迟。

编译期拓扑校验会报出两类新诊断：`E-TOP005`（叠加链上坐标出现空槽 gap）、`E-TOP006`（调用传播
延迟超出该块的 `deadline`）；`E-TOP002`（坐标维度一致性）在曲面体语义下已放宽为不报错，
coord 维度不同的块改为推导 `projection` 边。

标签键的解析与语义校验另有两条诊断：

- `E-TOP008`：`@layer` 出现未知键。**既有行为是静默丢弃**（`parseLayerTag` 的
  `else if` 链没有 `else` 分支），拼错的 `thread2=1` 会被无声忽略；两个新机制引入 8 个键后，
  这个静默失败会变成真实的用户困惑来源，故改为报错。
- `E-TOP009`：`time` / `thread` / `cost` / `deadline` 取负值。新机制需要 `shuttle_strategy=-1`
  （文档规定的默认值）这类负值，而词法层把负号独立成 `Minus` token、旧的 `parseLayerTag`
  一律 `eat(Number)`——于是负号会抛出**无错误码的 `Parser Error`**。解析层因此统一接受有符号数值
  （`dual=true` / `false` 同样从崩溃改为可解析），语义层再用 `E-TOP009` 拒绝无语义的负数。
  `coord` **允许为负**：coord 是格点**地址**而非计数，L1 距离对负地址同样良定义。

含 `@layer` 的模块在通过校验后额外生成调度表常量
`@rvdl_topology_json` 与入口 `define i32 @rvdl_topology_entry()`，后者调用运行时接口
`rvdl_runtime_run_topology(i8*)`——按 `time` 分层、同层按 coord 的 L1 码距并行调度
（虚拟线程 → 残差并发 → 码距逻辑 qubit 布局，阈值可用环境变量 `RVDL_TOPOLOGY_MIN_DISTANCE` 调整）。

多个标签函数构成一张执行拓扑，运行时按 `time` 分层、同层按 `thread` 并行调度：

```rvdl
@layer(time=0, thread=0, coord=(0,0))
int32 producer() { return 42; }

@layer(time=0, thread=1, coord=(0,1))   // 与 producer 同 time、异 coord → 平行
int32 observer() { return 7; }

@layer(time=1, thread=0, coord=(0,0))   // 与 producer 同 coord、time+1 → 叠加
int32 consumer() { return 0; }
```

### 8.2 函数声明

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 add(int32 a, int32 b) {
    return a + b;
}

@layer(time=0, thread=0, coord=(1))
void do_nothing() {
    return;
}
```

### 8.3 匿名函数（lambda / 闭包）

`fn` 关键字定义匿名函数，支持闭包捕获与高阶调用：

```rvdl
let twice = fn(int32 x) -> int32 {
    return x * 2;
};
```

函数类型的返回类型签名形如 `(params)->ret`，可通过变量间接调用。lambda body 的量子操作
纳入借用检查（QLT 线性类型覆盖闭包内的 qubit 操作）。

### 8.4 方法接收者

`form`/`impl` 的方法可通过 `self` 或 `&self` 作为接收者（见 [第 14 节](#14-类型定义form--trait--impl--template)）。

### 8.5 函数属性（系统级 `@[...]` 与调度标签 `@layer`）

函数可携带系统级属性 `@[name]` / `@[name("value")]`，与调度标签 `@layer(...)` 正交并存：
`@[...]` 控制段放置/裸函数等系统语义，`@layer(...)` 控制多维拓扑调度语义，二者可混排：

```rvdl
@layer(time=0, thread=0, coord=(0)) @[section(".text.boot")] @[naked] @[no_gc]
int32 main() {
    return 0;
}
```

常用系统属性：`section`/`place`（段放置）、`naked`/`raw`（裸函数，无栈帧）、`no_gc`（禁用 GC）。

#### 8.5.1 `@layer` 键参考

`@layer(...)` 的键分四组。**全部键都是可选的**（`time` 对根块必填），
未出现的键用括号中的默认值。

| 组 | 键 | 默认 | 取值域 |
| --- | --- | --- | --- |
| 拓扑 | `time` | — | ≥ 0（根块必填） |
| 拓扑 | `thread` | — | ≥ 0 |
| 拓扑 | `coord` | — | N 维整数；**允许负值**（格点地址） |
| 拓扑 | `cost` | `1` | ≥ 0；功能周期 `[time, time+cost)` |
| 拓扑 | `deadline` | 无 | ≥ 0 |
| 拓扑 | `dual` | `0` | `1`/`0`，也接受 `true`/`false` |
| 穿梭 | `shuttle` | 无 | 目标 coord 列表 |
| 穿梭 | `shuttle_lambda` | `1` | > 0 |
| 穿梭 | `shuttle_phase` | `2` | `{0, 1, 2}` |
| 穿梭 | `shuttle_rounds` | `0`（auto） | ≥ 0；`0` = 按所选策略的解析式定轮次 |
| 穿梭 | `shuttle_strategy` | `-1`（auto） | `{-1, 0, 1, 2, 3, 4}` |
| 追踪 | `track` | 无 | 可观测量标识符列表 |
| 追踪 | `track_mode` | `0`（auto） | `{0, 1, 2, 3}` |
| 追踪 | `track_slots` | `1` | ≥ 1 |

**相位语义（易错点）**：`shuttle_phase` 是相对**目标层** time-slot 的时机，不是相对本块：

| `shuttle_phase` | 执行时刻 | 方向 |
| --- | --- | --- |
| `0` | 目标层**入口** | 载荷投递进目标影子，目标把它当作输入看到 |
| `1` | 目标层**出口** | 目标影子写回，作为返回腿 |
| `2`（默认） | 两个边界 | 双侧恢复算子，覆盖面严格大于任一单侧 |

**为什么需要渗透载荷**：源块与目标块的功能周期通常并不重叠（`sense@t0` → `relay@t1`）。
若在目标边界上直接读源影子，会发现窗口已关闭；而延长影子生命周期又会违反
「影子槽随功能周期释放」的不变量。因此跨窗口传递的是一枚**经典关联标量**
（不是状态副本），它承载关联度而非量子态。

**强制即强制**：非零 `track_mode` **覆盖** auto 选级（`2`→L1、`3`→L2；
`1` 对非 Pauli 族报 `E-TRK003`）。若强制不改变级别，`track_mode` 就是哑参数。

**轮次按策略解析（易错点）**：`shuttle_rounds=0`（auto）时的轮次由**所选策略**决定，
五个式子各不相同：

| 策略 | auto 轮次 |
| --- | --- |
| `0` 蝶式 | `max(1, ⌈log₂ span⌉)` |
| `1` 环式 | `n`（对端数） |
| `2` 树式 | `⌈log₂(n+1)⌉` |
| `3` Grover | `1` |
| `4` c001 蝶点 | `1`（恒 1 轮，与 span / 对端数无关） |

蝶式跟格点跨度 `span` 走、环式跟对端数 `n` 走，二者**没有固定大小关系** ——
因此不能统一套一个式子。`shuttle_rounds > 0` 时用户值是权威值，任一策略都原样执行。
c001 蝶点把全体关联层的主要特征压缩成一个**蝶点**再辐射，多个蝶点经拓扑边互联
即成「DNA 链」多层架构，详见[穿梭与追踪](./rvdl-shuttle-tracking.md) §3.7。

编辑器中悬停到任一键名或枚举取值即可看到该键的语义与取值域；
在 `@layer(` 后（键位）补全只给出键名，不混入关键字与内建函数。

### 8.6 堆分配构造（make）

`new` 与 `make` 都用于构造对象，区别在于分配位置——`new` 栈分配、`make` 堆分配（经 GC 堆）：

```rvdl
auto a = new BellState();   // 栈分配
auto b = make BellState();  // 堆分配
```

---

## 9. 契约与静态验证

函数与循环支持契约注解，用于静态验证（对应 `rvdl verify` 命令）：

| 关键字 | 位置 | 含义 |
| --- | --- | --- |
| `requires` | 函数签名后 | 前置条件 |
| `ensures` | 函数签名后 | 后置条件 |
| `invariant` | `while` 循环内 | 循环不变量 |

```rvdl
int32 square(int32 x)
    requires x >= 0;
    ensures result >= 0;
{
    return x * x;
}
```

- `result` 关键字在 `ensures` 中引用函数返回值。
- 验证器基于最弱前置条件（WP）演算，把契约翻译为证明义务（obligation），可导出 SMT-LIB 交由 Z3 / cvc5 判定：

```bash
rvdl verify prog.rvdl                 # 静态验证契约
rvdl verify prog.rvdl --smt out.smt2  # 导出 SMT-LIB
```

---

## 10. 量子类型与操作

### 10.1 分配与测量

```rvdl
Qubit q = alloc();      // 分配一个量子比特
int32 r = measure(q);   // 测量并坍缩，返回 0/1
```

> **不可克隆定理**：`Qubit` 不能被拷贝，`let q2 = q;` 会报 `Quantum Violation: Cannot copy Qubit`。
> **测量后坍缩**：已测量的 qubit 不能再被使用，会报 `Qubit used after measurement`。

### 10.2 量子门

| 门 | 签名 | 说明 |
| --- | --- | --- |
| `x` | `x(Qubit)` | Pauli-X |
| `y` | `y(Qubit)` | Pauli-Y |
| `z` | `z(Qubit)` | Pauli-Z（相位翻转） |
| `h` | `h(Qubit)` | Hadamard |
| `s` | `s(Qubit)` | S 门（π/2 相位门，= Rz(π/2)） |
| `t` | `t(Qubit)` | T 门（π/4 相位门，= Rz(π/4)） |
| `rz` | `rz(Qubit, double)` | 绕 Z 轴旋转 |
| `rx` | `rx(Qubit, double)` | 绕 X 轴旋转 |
| `ry` | `ry(Qubit, double)` | 绕 Y 轴旋转 |
| `cnot` | `cnot(Qubit, Qubit)` | 受控非（控制, 目标） |
| `toffoli` | `toffoli(Qubit, Qubit, Qubit)` | Toffoli（两控制一目标） |
| `swap` | `swap(Qubit, Qubit)` | 交换 |
| `qft` | `qft(int)` | 量子傅里叶变换（比特数） |
| `braid` | `braid(Qubit, Qubit)` | 编织（Yang-Baxter） |

**受控门**（由 `@[steer]` 可逆编织自动合成，也可直接调用）：

| 门 | 签名 | 说明 |
| --- | --- | --- |
| `cx` | `cx(Qubit, Qubit)` | 受控 X（= CNOT） |
| `ch` | `ch(Qubit, Qubit)` | 受控 Hadamard |
| `crz` | `crz(Qubit, Qubit, double)` | 受控 Rz |
| `cswap` | `cswap(Qubit, Qubit, Qubit)` | 受控 SWAP（Fredkin） |
| `c_toffoli` | `c_toffoli(Qubit, Qubit, Qubit, Qubit)` | 受控 Toffoli（C³X） |
| `cqft` | `cqft(Qubit, int)` | 受控 QFT（控制位 + 比特数） |
| `cbraid` | `cbraid(Qubit, Qubit, Qubit)` | 受控编织（Yang-Baxter） |

逆 QFT：`iqft(int)`（`qft` 的可逆对偶，门序反转 + 角度取负）。

> 门操作在语句位置被识别为函数调用；在表达式位置（如 `x * x`）则识别为普通标识符。

### 10.3 X / Y 基测量

```rvdl
int32 mx = measure_x(q);   // X 基测量
int32 my = measure_y(q);   // Y 基测量
```

### 10.4 任意基构建

`basis_state` 用布洛赫方向 (θ, φ) 构建任意基下的量子态，与固定基的 `DiracState`、
`BellState` 形成互补：

```rvdl
auto zplus  = basis_state(0.0, 0.0, 0);              // Z 基 |0⟩
auto zminus = basis_state(0.0, 0.0, 1);              // Z 基 |1⟩
auto xplus  = basis_state(1.5707963, 0.0, 0);        // X 基 |+⟩（θ=π/2）
auto yplus  = basis_state(1.5707963, 1.5707963, 0);  // Y 基 |+i⟩（θ=π/2, φ=π/2）
auto arbitrary = basis_state(0.8, 2.1, 0);           // 任意方向的 + 本征态
```

其中 `|n̂₊⟩ = cos(θ/2)|0⟩ + e^{iφ} sin(θ/2)|1⟩`，`|n̂₋⟩` 为其正交的 - 本征态。

### 10.5 QObject 部分坍缩与 POVM 弱测量

QObject（多 qubit 容器）支持**部分坍缩**测量：`qmeasure(qobj, i)` 只测量第 i 个 qubit 并坍缩它，其余 qubit 保持存活（`Ref` 借用，不消费整个 QObject）；而 `QObject.measure()` 消费并测量整个 QObject。

```rvdl
auto reg = new QuantumRegister(3);
int32 b0 = qmeasure(reg, 0);        // 测第 0 个 qubit（部分坍缩，其余 2 个存活）
int32 b1 = qmeasure(reg, 1);        // 测第 1 个 qubit
int32 n  = qobj_num_qubits(reg);    // 3
```

POVM（正定算子值）弱测量用跨对象受控门实现：主 qubit 控制辅助 qubit，测量辅助 qubit 时主态仅被弱扰动（不完全坍缩）：

```rvdl
auto sys = basis_state(0.8, 0.0, 0);    // 主态
auto anc = basis_state(0.0, 0.0, 0);    // 辅助 qubit |0⟩
qgate_cnot_pair(sys, 0, anc, 0);        // 主 qubit 控制辅助 qubit（弱测量耦合）
int32 m = qmeasure(anc, 0);             // 测辅助 qubit，主态弱坍缩
```

非破坏期望 `qexpect_z(qobj, i)` 读取 ⟨Z⟩ 而不坍缩态（软测量）；多 qubit 变分 ansatz 可用 `qobj_num_qubits` 判定态维度对齐。

---

## 11. 内置类

内置量子类通过 `new` 实例化，返回 `QObject`：

| 类 | 构造 | 说明 |
| --- | --- | --- |
| `DiracState` | `new DiracState(n)` | 狄拉克态（n 维） |
| `BellState` | `new BellState()` | Bell 态 |
| `QuantumRegister` | `new QuantumRegister(n)` | n 比特量子寄存器 |

`QReservoir`（量子储备池）通过 `qrc_new(qubits, layers)` 创建（而非 `new`），详见 [12.10 QRC 量子储备池](#1210-qrc-量子储备池)。

支持成员访问：

```rvdl
auto bell = new BellState();
int32 m = bell.measure();     // QObject.measure() -> int32
auto reg = new QuantumRegister(15);
```

`QModel` 支持 `.export(path)` 导出 `.rvdlm` 模型：

```rvdl
auto model = qlm_invoke(data, 10, 0.01);
model.export("model.rvdlm");
```

---

## 12. 内置函数

### 12.1 量子核心

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `alloc` | `alloc()` | `Qubit` |
| `measure` | `measure(Qubit)` | `int32` |
| `measure_x` / `measure_y` | `(Qubit)` | `int32` |
| `basis_state` | `(double theta, double phi, int32 value)` | `QObject` |

`basis_state` 在**任意基**下构建单比特量子态：`(θ, φ)` 参数化布洛赫球方向 n̂，
返回该基的 + 本征态（`value=0`）或 - 本征态（`value=1`）：
`|n̂₊⟩ = cos(θ/2)|0⟩ + e^{iφ} sin(θ/2)|1⟩`。标准基特例：Z 基 `(0,0)`、X 基
`(π/2,0)`、Y 基 `(π/2,π/2)`。

### 12.2 文本 / 图像编码

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `encode_text` | `encode_text(string)` | `QObject` |
| `encode_image` | `encode_image(string)` | `QObject` |
| `rvdl_encode_string` | `rvdl_encode_string(string)` | `QObject` |
| `rvdl_decode_string` | `rvdl_decode_string(QObject)` | `string` |

### 12.3 量子语言模型（QLM）

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `qlm_load` | `qlm_load(string)` | `QModel` |
| `qlm_forward` | `qlm_forward(QModel, QObject)` | `void` |
| `qlm_invoke` | `qlm_invoke(QObject, int, double)` | `QModel` |

### 12.4 脑机接口（QbNS）

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `mind_read` | `mind_read(string)` | `QObject`（模态：`stream/spike/lfp/eeg/sensor`） |
| `mind_train` | `mind_train(QObject, int, double)` | `void` |
| `mind_feedback` | `mind_feedback(QObject)` | `void` |

### 12.5 VedaROS

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `veda_qlm_train` | `veda_qlm_train(QObject, int, double)` | `void` |

### 12.6 标量数学 / 神经原语

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `surrogate` | `(double, double, double)` | `double` |
| `tanh_quantize` | `(double, double, int)` | `double` |
| `lif_step` | `(double, double, double, double)` | `double` |
| `mellowmax2` | `(double, double, double)` | `double` |
| `logsumexp2` | `(double, double, double)` | `double` |
| `boltzmann2` | `(double, double, double)` | `double` |
| `tnorm_luk` / `tnorm_prod` / `tnorm_godel` | `(double, double)` | `double` |
| `polymer_weight` | `(double, double, double)` | `double` |
| `polymer_mix_bound` | `(double, double)` | `double` |
| `complex` | `(double re, double im)` | `complex128`（复数构造） |
| `real` / `imag` | `(complex128)` | `double`（实部 / 虚部） |
| `cabs` | `(complex128)` | `double`（模长 \|z\|） |
| `conj` | `(complex128)` | `complex128`（共轭） |

### 12.7 QCOS 系统调用（syscall ABI）

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `rvdl_sys_call` | `(int32 no, int32 a, int32 b, int32 c)` | `int32` |
| `rvdl_sys_calld` | `(int32 no, ...)` | `double` |
| `rvdl_sys_log` | `(int32 level, string)` | `void` |
| `rvdl_sys_logi` | `(int32 level, int32)` | `void` |
| `rvdl_sys_callp` | 指针型入口 | —（待 P1 `ptr<T>` 开放） |

常用系统调用号：`SYS_LOG = 1`、`SYS_YIELD = 2`、`SYS_EXIT = 3`、`SYS_TIME = 10`。

### 12.8 QMS 数值内核（算法 4）

量子 Markov 半群数值服务下沉到裸机内核，提供谱隙 / 混合界 / 方差收缩时间：

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `rvdl_qms_gap` | `(int32 model, double p, double q)` | `double`（谱隙） |
| `rvdl_mix_bound` | `(double gap, double n, double eps)` | `double`（混合界） |
| `rvdl_qms_conc` | `(double gap, double eps)` | `double`（方差收缩时间） |

噪声信道模型：`0` 去极化、`1` 去相位、`2` 振幅阻尼、`3` Pauli。

### 12.9 系统级内建

| 类别 | 函数 | 说明 |
| --- | --- | --- |
| 原子操作 | `sync_load` / `sync_store` / `sync_add` / `sync_cas` | 映射到 LLVM 原子指令（load/store atomic、atomicrmw、cmpxchg） |
| 自旋锁 | `sync_lock(p)` / `sync_unlock(p)` | 原子 TAS 获取（返回旧值）/ 原子释放，供 while 自旋等待 |
| volatile 访问 | `volatile_load(p)` / `volatile_store(p, v)` | `load volatile` / `store volatile`（MMIO 轮询在 `-O2` 下不被优化为死循环） |
| 端口 I/O | `outb/outw/outl(port, val)` / `inb/inw/inl(port)` | x86 8/16/32 位端口读写（16550 UART / ATA / PCI 等） |
| 读寄存器 | `read_cr0/cr2/cr3/cr4()` | 读控制寄存器（`mov %crN, ${0}`，返回 `uint64`） |
| 写寄存器 | `write_cr0(v)` / `write_cr3(v)` / `invlpg(addr)` | 写控制寄存器 / 刷新 TLB 单页（`invlpg`） |
| MSR | `rdmsr(msr)` / `wrmsr(msr, v)` | 读/写模型特定寄存器（EDX:EAX 双输出组合为 `uint64`） |
| CPUID | `cpuid(leaf, subleaf, eax, ebx, ecx, edx)` | 探测 CPU 特性，写 4 个输出指针（`uint32`） |
| 时间戳 | `rdtsc()` / `read_rflags()` / `xgetbv(xcr)` | 读 TSC / RFLAGS / XCR0（`uint64`） |
| 堆分配 | `rvdl_gc_alloc(n)` / `rvdl_gc_free(p)` | 内核堆分配 / 释放 |
| 取地址 | `addr(&fn)` | 取函数地址（低 32 位） |
| 内联汇编 | `native("hlt")` / `native("lidt (${0})", &idtr)` | 内联汇编模板（`asm sideeffect`），支持无/带操作数 |

### 12.10 QRC 量子储备池

量子储备计算（Reservoir Computing）：随机量子电路作为非线性高维特征映射，仅训练线性读出层，规避变分训练中的贫瘠高原（barren plateau）。

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `qrc_new` | `qrc_new(int32 qubits, int32 layers)` | `QReservoir` |
| `qrc_new` | `qrc_new(int32 qubits, int32 layers, int32 out_dim)` | `QReservoir` |
| `qrc_train` | `qrc_train(QReservoir res, int32 epochs, double lr)` | `void` |
| `qrc_train_ex` | `qrc_train_ex(QReservoir res, int32 epochs, double lr, cap<double> inputs, cap<double> targets, int32 n_samples, int32 n_features, int32 n_outputs)` | `void` |
| `qrc_release` | `qrc_release(QReservoir res)` | `void` |
| `qrc_probe` | `qrc_probe(QReservoir res, QObject data)` | `QObject` |
| `qrc_predict` | `qrc_predict(QReservoir res, QObject data)` | `QObject` |

**输出维度 `out_dim`**：`qrc_new` 的第三参指定读出输出的维度（多维回归 / 多类读出）。
两参形式等价于 `out_dim = 1`（标量输出，向后兼容）。

**训练数据**：
- `qrc_train(res, epochs, lr)` —— 用**合成正弦时序**训练线性读出。这是经典 QRC 自检
  任务（用于验证「无贫瘠高原」），**不是**通用训练入口。
- `qrc_train_ex(res, epochs, lr, inputs, targets, n_samples, n_features, n_outputs)` ——
  用**真实训练数据**训练。`inputs` / `targets` 为扁平 `double` 缓冲（行主序，
  长度分别为 `n_samples × n_features` 与 `n_samples × n_outputs`），类型可用
  `cap<double>` 或 `arr<double, N>`（二者在 ABI 层均退化为 double 数组首指针）。

```rvdl
auto res = qrc_new(4, 3);              // 4 qubit、3 层储备池（标量输出，out_dim = 1）
qrc_train(res, 50, 0.01);              // 自检：合成正弦时序训练（演示用）
auto feat = qrc_probe(res, data);      // 测态编码输入 → ⟨Z⟩ 特征（qubit 编码）
auto pred = qrc_predict(res, data);    // 读出预测（qubit 编码）
qrc_release(res);                      // 释放储备池

// 多维输出 + 真实训练数据
auto mr = qrc_new(6, 3, 4);            // 6 qubit、3 层、输出维度 4
qrc_train_ex(mr, 100, 0.01, xs, ys,   // xs: n×in  flat double buffer
             n_samples, n_features, 4);// ys: n×4   flat double buffer
qrc_release(mr);
```

### 12.11 TQNF 拓扑量子神经场

QObject 层量子原语，供量子 Transformer / 多 qubit 变分 ansatz 在 QObject 数据流上串联参数化门电路。这些函数**借用** QObject 实参（不消费整体），`qmeasure` 为部分坍缩测量。

| 类别 | 函数 | 签名 | 返回 |
| --- | --- | --- | --- |
| QObject 门 | `qgate_h` / `qgate_x` | `(QObject q, int i)` | `void` |
| QObject 门 | `qgate_rz` | `(QObject q, int i, double θ)` | `void` |
| QObject 门 | `qgate_cnot` | `(QObject q, int c, int t)` | `void` |
| 跨对象门 | `qgate_cnot_pair` | `(QObject a, int ia, QObject b, int ib)` | `void` |
| 期望读取 | `qexpect_z` | `(QObject q, int i)` | `double` |
| 部分坍缩 | `qmeasure` | `(QObject q, int i)` | `int32` |
| qubit 数 | `qobj_num_qubits` | `(QObject q)` | `int32` |
| 注意力 | `qattention` | `(QObject q, QObject k)` | `double` |
| 态量度 | `qstate_entropy` | `(QObject q)` | `double` |
| 态量度 | `qstate_fidelity` | `(QObject a, QObject b)` | `double` |
| 可训练性 | `dla_dim` | `(string spec, int32 n_qubits)` | `int32` |
| 测量熵 | `shannon4` / `shannon8` | `(int32 n0, ...)` | `double` |

- `qgate_*`：直接作用于 QObject 内部第 i 个 qubit（借用不消费），用于量子 Transformer 前馈层。
- `qgate_cnot_pair`：跨对象受控非门，供 POVM 弱测量（主 qubit 控制辅助 qubit，主态不完全坍缩）。
- `qexpect_z`：非破坏期望读取 ⟨Z⟩（软测量，不坍缩态）。
- `qmeasure`：**部分坍缩**测量（测第 i 个 qubit，其余 qubit 仍存活，`Ref` 借用不消费整个 QObject）。
- `qattention`：SWAP test 测两态重叠度 |⟨ψ_q\|ψ_k⟩|² ∈ [0,1]，作为经典 attention「query·key 相似度」的量子对应。
- `qstate_entropy` / `qstate_fidelity`：从 QVM 态矢量提取约化密度矩阵计算；无态矢量或 >10 qubit 返回 -1。
- `dla_dim`：动力学李代数维数（`spec` 为逗号分隔 Pauli 串，如 `"X,Z"`），维数越小越可训练（规避贫瘠高原）。

```rvdl
auto q = basis_state(0.5, 0.0, 0);     // query
auto k = basis_state(0.4, 0.0, 0);     // key
double s = qattention(q, k);           // |⟨ψ_q|ψ_k⟩|² ≈ 0.998
int32 d = dla_dim("X,Z", 1);           // su(2) → 3
int32 n = qobj_num_qubits(q);          // 1
```

### 12.12 量子通道逆因果容量（回程能力）

量子通道的**逆因果容量**（retrocausal capacity）度量「通过有噪后选择闭合类时曲线（P-CTC）从未来向过去通信」的能力（理论来源 arXiv:2509.08965）。核心量由信道的 Choi 矩阵 J 决定：max-information `I_max = log2 λ_max(J)`、Doeblin information `I_doe = log2 d_in − log2 λ_min⁺(J)`，渐近逆因果容量 `Q_retro = ½(Imax + Idoe^∞)`、`C_retro = Imax + Idoe^∞`。

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| max-information | `retrocausal_imax(kind, p)` | `double` |
| Doeblin information | `retrocausal_idoe(kind, p)` | `double` |
| 渐近量子容量 | `retrocausal_q_capacity(kind, p)` | `double` |
| 渐近经典容量 | `retrocausal_c_capacity(kind, p)` | `double` |
| 单次量子容量 | `retrocausal_q_one_shot(kind, p, eps)` | `double` |
| 回程增益 | `retrocausal_gain(kind, p)` | `double` |
| q-变形容量 | `retrocausal_deformed(kind, p, q)` | `double` |

预置信道 `kind`（`int32`）：`0`=去极化（`p`=去极化概率）、`1`=退相（`p`=退相概率）、`2`=比特翻转（`p`=X 概率）、`3`=振幅阻尼（`p`=γ 衰变率）、`4`=幺正 Hadamard（`p` 忽略）、`5`=比特-相位翻转（`p`=Y 概率）。

```rvdl
double imax = retrocausal_imax(0, 0.1);         // 去极化 p=0.1 的 max-information
double q    = retrocausal_q_capacity(0, 0.1);   // 渐近逆因果量子容量
double c    = retrocausal_c_capacity(1, 0.2);   // 退相 p=0.2 的逆因果经典容量
double g    = retrocausal_gain(0, 0.1);         // 回程增益（时间不对称性）
double cq   = retrocausal_deformed(0, 0.1, 0.5);// q-变形容量（q=0.5 偏回程）
```

### 12.13 快子 KK 双空间嘈杂 CTC 通信（n 个 DD 维度）

把上节的「抽象噪声参数」升级为「由快子场在 KK 额外维（DD 双维度）中传播物理产生」：快子（`m²_eff = −μ² < 0`，超光速）沿类时闭合路径传播形成封闭类时曲线（CTC），其噪声由 KK 额外维的几何（Wilson line θ 破坏 ±n 模式对称）与快子势 `V = −½μ²φ² + ¼λφ⁴` 决定。`@layer(coord=(c0,...,c_{n-1}))` 的 coord 维度即额外维数 n。

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| CTC 量子容量 | `retrocausal_ctc_q_capacity(n, theta, mu2, lambda)` | `double` |
| CTC 经典容量 | `retrocausal_ctc_c_capacity(n, theta, mu2, lambda)` | `double` |
| CTC 回程增益 | `retrocausal_ctc_gain(n, theta, mu2, lambda)` | `double` |
| 退相噪声映射 | `retrocausal_ctc_dephasing(theta)` | `double` |

噪声映射：退相 `p = asym/(1+asym)`（`asym = Σ|θ_i|`，Wilson line 破坏 ±n 对称）、去极化 `p = n/(n+1)`（额外维「维度扩散」）、振幅阻尼 `γ = μ²/(μ²+λ)`（快子真空不稳定）。

```rvdl
@layer(time=0, thread=0, coord=(0,0))
double ctc_capacity() {
    double q = retrocausal_ctc_q_capacity(2, 0.3, 1.0, 1.0);  // 双 DD 维度 CTC 量子容量
    double p = retrocausal_ctc_dephasing(0.3);                // Wilson line 退相噪声
    return q;
}
```

### 12.14 非欧几里德曲面体几何

把「量子态流形」与「弦论 T-对偶双空间」的几何暴露到语言层：`geodesic_distance` 给出两个量子态
在复射影空间 `CP^{d-1}` 上的 **Fubini-Study 测地线距离** `arccos|⟨a|b⟩|`（复用 `rvdl_qattention`
的 SWAP-test 重叠，借用 QObject 不消费）；`inversion` 给出 T-对偶的半径反转 `R → 1/R`；
`hyperbolic_metric` / `hyperbolic_distance` 给出 Poincaré 球的双曲度规 `4/(1-|x|²)²` 与双曲距离。
这组原语对应「曲面体」上 coord 维度不同的块之间的 `projection` 投影关系，以及在
`@layer(dual=1)` 反转空间中生效的 `inversion` 边。

| 函数 | 签名 | 返回 | 说明 |
| --- | --- | --- | --- |
| 测地线距离 | `geodesic_distance(QObject, QObject)` | `double` | Fubini-Study 测地线距离 `arccos\|⟨a\|b⟩\|`（借用不消费） |
| T-对偶反转 | `inversion(double)` | `double` | `R → 1/R`（反转空间，对应 `@layer(dual=1)`） |
| Poincaré 度规 | `hyperbolic_metric(double)` | `double` | `4/(1-\|x\|²)²`（`\|x\| → 1` 时发散） |
| 双曲距离 | `hyperbolic_distance(double, double)` | `double` | Poincaré 球内的双曲距离 |

```rvdl
@layer(time=0, thread=0, coord=(0,0))
double geometry() {
    auto a = basis_state(0.5, 0.0, 0);
    auto b = basis_state(0.4, 0.0, 0);
    double d = geodesic_distance(a, b);   // 两态在 CP^1 上的测地线距离
    double r = inversion(2.0);            // T-对偶：R=2 → 1/R=0.5
    double m = hyperbolic_metric(0.5);    // 4/(1-0.25)² ≈ 7.11
    double h = hyperbolic_distance(0.0, 0.5);
    return d + r + m + h;
}
```

---

## 13. 模块系统（`.rvdli`）

### 13.1 声明与导出

```rvdl
mod math {
    pub int32 add(int32 a, int32 b) {
        return a + b;
    }
    export int32 square(int32 x) {
        return x * x;
    }
}
```

### 13.2 导入与权限

```rvdl
import math from "./math.rvdli";
requires io.network;          // 声明所需权限
let result = math.add(1, 2);
```

### 13.3 关键字

| 关键字 | 说明 |
| --- | --- |
| `mod` | 定义模块 |
| `use` | 路径导入 |
| `pub` | 公开成员 |
| `import` + `from` | 导入 `.rvdli` |
| `export` | 导出 |
| `requires` | 权限声明 |

### 13.4 `.rvdli` 格式（QOBF v2，加密二进制）

`.rvdli` 模块采用 **QOBF v2**（Quantum Obfuscated Binary Format）加密封装，文本编辑器强制打开为
乱码（与加密 DLL 同理），杜绝明文 JSON / LLVM IR 泄露：

```
[magic "RVDLI" 4B][version=2 u32][flags u32][name_len u16][name(明文)][nonce 12B]
[kdf_salt 16B][ciphertext_len u32][ciphertext][tag 32B]
```

**三层防护**：

| 层 | 技术 | 作用 |
| --- | --- | --- |
| ① 二进制序列化 | 类型标签（u8）+ 长度前缀，取代 JSON | 去除可读文本结构 |
| ② 流加密 | ChaCha20（RFC 8439，IETF 96-bit nonce） | 去除明文 |
| ③ 完整性 | HMAC-SHA256（防篡改，常量时间比较） | 篡改即拒载 |

**密钥派生**（自包含混淆，与加密 DLL 同定位）：`K(64B) = HKDF-SHA256(内嵌盐 ‖ 模块名, kdf_salt, "rvdl-mmi-obf-v2")`，
前 32B 加密、后 32B MAC；`nonce` / `kdf_salt` 打包时真随机，每模块独立密钥。

**实现位置**：C++ `qhal/Qcrypt.hpp`（SHA-256 / HMAC / HKDF / ChaCha20）+ `qhal/MMI.hpp`（解密 + 二进制解析），
TS `server/src/mmi.ts`（`packMMI` 加密 / `unpackMMI` 解密）。运行时通过 C ABI `rvdl_runtime_*_mmi` 动态加载：
验 HMAC → ChaCha20 解密 → 二进制 header 解析 → SandboxJIT 加载 IR。

> 安全边界：`.rvdli` 是自包含模块（加载时无用户密钥输入），密钥内嵌属**混淆/白盒**防护——目标是
> 「打开是乱码 + 防篡改 + 防轻易提取」，而非理论不可破（与加密 DLL 同定位）。

---

## 14. 类型定义（form / trait / impl / template）

### 14.1 form

`form` 定义数据结构，支持继承（`inherits`，单继承）与 `rank` 秩：

```rvdl
form Point {
    double x;
    double y;
}
```

字段访问（`p.x` / `p.y`）经 `getelementptr` 编译为结构字段加载；结构布局为
`{ i8* vtable, 字段... }`（字段索引从 1 起）。MIR 携带 form 定义（`forms`），
下沉路径（`--mir`）据此重建结构类型并生成精确 `getelementptr`。

`@[packed] form` 定义**精确字节布局**结构体：无 vtable 指针、无填充、字段从偏移 0 起紧密
排列，映射到 LLVM packed struct `<{ ... }>`，用于精确映射磁盘硬件结构（GPT 头 / 分区表项 /
FAT32 BPB / ATA IDENTIFY）：

```rvdl
@[packed] form GptHeader {
    uint64 signature;       // 偏移 0
    uint32 revision;
    uint32 headerSize;
    arr<uint8, 4> reserved; // 内嵌保留区
}
```

**位域**（`T field : N`）：连续的同类位域打包进同一个存储单元（LSB 优先，超宽自动开新单元），
读为 `lshr + and` 掩码、写为「load + 清位 + 移位 or + store」的读-改-写，用于磁盘结构的位标志：

```rvdl
@[packed] form FatDirAttr {
    uint8 readonly : 1;   // 位 0
    uint8 hidden   : 1;   // 位 1
    uint8 system   : 1;   // 位 2
    uint8 reserved : 5;   // 位 3..7
}
```

### 14.2 trait 与 impl

```rvdl
trait Shape {
    double area();
}

impl Shape for Circle {
    double area() {
        return 3.14;
    }
}
```

### 14.3 template

```rvdl
template<T> form Box {
    T value;
}

auto b = new Box<int32>();
```

### 14.4 关键字汇总

| 关键字 | 说明 |
| --- | --- |
| `form` | 定义数据结构（`inherits` / `rank`） |
| `trait` | 定义可共享的行为接口 |
| `impl` | 为类型实现 trait（`impl <trait> for <type>`） |
| `template` | 泛型声明 |
| `rank` | 秩块 |
| `self` | 方法接收者（`self` / `&self`） |

### 14.5 flavor（味 / 枚举）

`flavor` 是 `enum` 的量子化命名，声明具名常量，按声明顺序自动赋值 `0,1,2,...`：

```rvdl
flavor Channel {
    DEPOLARIZING,
    DEPHASING,
    AMPLITUDE_DAMPING,
    PAULI
}

int32 m = Channel.PAULI;    // 3
```

### 14.6 fuse（融合 / 模式匹配）

`fuse` 是 `match` 模式匹配的量子化命名（Hopf 余乘解构），对判别值做多分支解构；`_` 为通配模式：

```rvdl
int32 label = fuse (m) {
    0: 10,
    1: 20,
    2: 30,
    _: 0
};
```

### 14.7 morph（态射宏 / 全局卫生宏）

`morph` 是 rvdl 的**宏系统**：一种在编译期把「语法模式」变换为「代码」的元编程机制，是函数/内建在**语法层**的对偶。它有三条硬约束，构成与 `#define` 完全不同的新范式：

1. **全局**：`morph` 只能是顶层声明，与内建函数同命名空间，不允许局部宏（函数体里不能 `morph`）。宏展开不依赖任何局部作用域，因此与程序组合可交换（指称透明）。
2. **卫生 = 规范不变性**：模板引入的绑定（`let` / `fn` 名 / 参数 / 类型声明）在每次展开时取**新原子**（α-重命名），使展开结果在「重命名」下不可区分——类比的规范场：名字不可观测，只有 α-等价类（规范不变量）才有意义（Gabbay–Pitts 名义集合）。因此不会出现 `#define SWAP(a,b){int tmp=a;...}` 那种变量捕获 bug。
3. **不使用 `#define`**：`morph` 是正经的顶层语言构造，展开发生在词法层（parse 之前），产物重新进入类型检查 / 借用检查 / 拓扑推导。

#### 声明与调用

```rvdl
// 声明：morph 名字( $元变量: 种类, ... ) { 模板 }
morph twice($e: expr) { ($e) + ($e) }

// 调用：统一 name!(...) 形式（表达式 / 语句 / 顶层均可）
int32 y = twice!(x * 3);       // 展开为 (x * 3) + (x * 3)
```

#### 元变量种类（一期）

| 种类 | 匹配 | 用途 |
| --- | --- | --- |
| `expr` | 一个表达式（直到顶层 `,` 或参数末尾） | 值片段 |
| `stmt` | 一条语句（直到顶层 `,` / `;`） | 控制流片段 |
| `items` | 0+ 顶层项（贪婪到参数末尾） | 生成 form / fn / flavor 一族 |
| `type` | 一个类型名（含 `cap<T>` / `arr<T,N>` / `lattice<T,B>` 泛型后缀） | 类型参数化 |
| `ident` | 一个标识符 | 函数 / 变量名 |
| `literal` | 一个数字或字符串字面量 | 常量 |
| `tt` | 任意一个 token 树（叶子或括号组） | 万能兜底 |

#### 重复（生成一族）

用 `$( ... )` 包裹可重复片段，`*` 表示 0 次或多次、`+` 表示 1 次或多次，分隔符写在 `)` 与 `*`/`+` 之间：

```rvdl
morph make_syscalls($($name: ident),*) {
    $(fn $name() -> int32 { return 0; })*
}
make_syscalls!(do_init, do_tick);   // 生成 fn do_init() 与 fn do_tick()
```

#### 字符串化

模板里的 `#$name` 把捕获片段转成字符串字面量（展开期 `stringify`）：

```rvdl
morph log_call($f: ident) { rvdl_sys_log(#$f); }
log_call!(init);    // → rvdl_sys_log("init");
```

#### 卫生示例

```rvdl
morph swap_into($a: expr, $b: expr) { let tmp = $a; $a = $b; $b = tmp }

@layer(time=0, thread=0, coord=(0)) int32 main() {
    int32 tmp = 42;              // 使用处的 tmp
    int32 x = 1; int32 y = 2;
    swap_into!(x, y);            // 模板里的 tmp 被 α-重命名为新名，不覆盖外面的 tmp
    // x == 2, y == 1, tmp == 42
}
```

#### 递归（限深）

宏可展开出宏，受固定深度上限（64）约束；超限会报「recursion limit」：

```rvdl
morph A($e: expr) { B!($e) }
morph B($e: expr) { ($e) + 1 }
int32 z = A!(2);    // → (2) + 1
```

> 约定：模板是一个**不含末尾分号**的语句/表达式序列，调用点照常写 `;` 作为终结符（形如 Rust `macro_rules!`）。模板若要保证优先级，请自行加括号 `($e)`。

---

## 15. 系统级编程（cap / unsafe / native）

rvdl 面向「量子机器人 + 裸机内核」，提供系统级构造：能力指针、危险操作块、内联汇编、
端口 I/O、原子操作与内核堆分配。这些操作必须显式置于 `unsafe { ... }` 块内，作为
「写内核」的明确危险边界。

### 15.1 unsafe 块

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    unsafe {
        // 危险操作：裸指针解引用 / MMIO / 内联汇编
    }
    return 0;
}
```

### 15.2 能力指针（cap）与解引用

```rvdl
unsafe {
    cap<int32> p = null;   // 能力指针，等价于 int32*
    *p = 5;                // 解引用写入（store）
    int32 x = *p;          // 解引用读取（load）
}
```

`cap<T>` 是 CHERI 式能力（地址 + 边界 + 权限 + 有效性），在 LLVM 层降为 `T*`。

### 15.3 内联汇编（native）

`native("模板", op1, op2, ...)` 逃逸到目标机器原生指令集，支持**无操作数**与**带操作数**两种
形式。模板用 `${0}` / `${1}` 占位引用操作数；操作数默认 `"r"`（通用寄存器）约束，内存间接
寻址在模板里写 `(${N})`。

```rvdl
unsafe {
    native("hlt");           // 无操作数：call void asm sideeffect "hlt"
    native("cli");           // 关中断

    // 上下文切换汇编（iretq / fxsave / 切栈 / lidt）
    native("iretq");                         // 中断返回（无操作数）
    native("fxsave (${0})", fpu_buf);        // 保存浮点状态（内存间接寻址）
    native("fxrstor (${0})", fpu_buf);       // 恢复浮点状态
    native("mov %%rsp, ${0}", new_stack);    // 切栈（寄存器操作数）
    native("lidt (${0})", idtr);             // 加载 IDT（特权指令带操作数）
}
```

### 15.4 裸汇编块（asm {}）

`asm { "..." "..." }` 逃逸到目标机器的**多指令汇编序列**（区别于 `native()` 的单条指令）。
块内为一个或多个字符串字面量，按行拼接为单一汇编模板，**不做 `${}` 占位符替换**——适合
「保存全部通用寄存器 + iretq」「调度器切栈」「gdt 远返回改 CS」这类完整汇编体。

与 `@[naked]` 裸函数配合时，函数体只允许 `asm{}` 块，直接发射为裸汇编体（无栈帧、无
prologue/epilogue）：

```rvdl
@layer(time=0, thread=0, coord=(0)) @[naked]
void irq_stub_common() {
    asm {
        "push rax; push rbx; push rcx; push rdx;"
        "mov rdi, rsp; call irq_dispatch;"
        "pop rdx; pop rcx; pop rbx; pop rax;"
        "iretq;"
    }
}
```

非 `@[naked]` 函数里的 `asm{}` 块则作为 `asm sideeffect` 内联到当前位置。

### 15.5 读寄存器 / MSR / CPUID（x86 架构边界）

把控制寄存器、MSR、CPUID 叶读回 rvdl 变量（此前 `native()` 是 void 副作用，读不回寄存器值）：

```rvdl
unsafe {
    uint64 cr3 = read_cr3();                 // 页表根（mov %cr3）
    uint64 efer = rdmsr(0xC0000080);         // EFER MSR（EDX:EAX 组合为 uint64）
    wrmsr(0xC0000080, efer);                 // 写回

    cap<uint32> eax = rvdl_gc_alloc(1);
    cap<uint32> ebx = rvdl_gc_alloc(1);
    cap<uint32> ecx = rvdl_gc_alloc(1);
    cap<uint32> edx = rvdl_gc_alloc(1);
    cpuid(1, 0, eax, ebx, ecx, edx);         // 探测 CPU 特性

    uint64 tsc = rdtsc();                    // 时间戳
    uint64 rfl = read_rflags();              // RFLAGS
    uint64 xcr = xgetbv(0);                  // XCR0（FPU 状态）
}
```

### 15.6 原子操作（sync_*）

```rvdl
unsafe {
    cap<int32> p = null;
    sync_store(p, 1);       // store atomic
    int32 x = sync_load(p); // load atomic
    int32 y = sync_add(p, 1);// atomicrmw add
    int32 z = sync_cas(p, 1, 2); // cmpxchg

    // 自旋锁：sync_lock 返回旧值（0 成功 / 1 已被持有），供 while 自旋等待
    while (sync_lock(p) != 0) { }
    sync_unlock(p);
}
```

### 15.7 端口 I/O（outb/outw/outl + inb/inw/inl）

8 / 16 / 32 位端口 I/O，覆盖 PCI/ATA/AHCI 底层：

```rvdl
unsafe {
    outb(1016, 81);         // 8 位：16550 UART 写 'Q'
    int32 st = inb(1021);   // 8 位：读 LSR 状态
    outw(1016, 0x4142);     // 16 位：ATA 数据寄存器
    int32 w = inw(1020);    // 16 位
    outl(0xCF8, 0x80000000);// 32 位：PCI 配置地址端口
    int32 l = inl(0xCFC);   // 32 位：PCI 配置数据端口
}
```

### 15.8 内核堆分配与取地址

```rvdl
unsafe {
    cap<int32> buf = rvdl_gc_alloc(16);   // 分配 16 字节
    rvdl_gc_free(buf);                    // 释放
    int32 h = addr(&rvdl_main);        // 取函数地址低 32 位
}
```

### 15.9 函数属性（三类标签）

函数上一行可标注三类正交标签：经典编译属性、量子门属性、量子物理特性。

**经典编译属性**（映射到 LLVM 函数属性）：

```rvdl
@layer(time=0, thread=0, coord=(0))
@[inline] @[pure] @[cold] @[export] @[noreturn]
int32 rvdl_main() { return 0; }
```

| 标签 | 语义 | LLVM |
| --- | --- | --- |
| `@[inline]` / `@[noinline]` | 强制 / 禁止内联 | `alwaysinline` / `noinline` |
| `@[pure]` / `@[readonly]` | 无副作用 / 只读 | `readnone` / `readonly` |
| `@[cold]` / `@[hot]` | 冷 / 热路径提示 | `cold` / `hot` |
| `@[noreturn]` | 不返回 | `noreturn` |
| `@[export]` | 导出符号 | `dllexport` |
| `@[section("...")]` / `@[naked]` | 段放置 / 裸函数 | `section` / `naked` |

**量子门属性**（可逆编织 Reversible Weaving 范式）：

```rvdl
@[gate] @[undo] @[steer]
void U(Qubit q) { h(q); rz(q, 0.5); }
```

| 标签 | 语义 | 自动合成 |
| --- | --- | --- |
| `@[gate]` | 标记为可组合门单元 | — |
| `@[undo]` | 合成可逆对偶 `U†`（门序反转 + 逐门取逆） | `<name>_undo` |
| `@[steer]` | 合成相干控制版本 `Λ(U)`（控制位导引目标门） | `<name>_steer` |
| `@[unitary]` | 酉性验证（无测量 / 无经典分支依赖） | — |
| `@[measure]` | 标记测量点（消费 Qubit） | — |

`@[undo]` 相干控制（coherent control）与 ZX-calculus 图式受控，而非「加一个控制位」。

**量子物理特性**（为 QVM / QM 模拟提供约束元数据）：

```rvdl
@[coherence(100, 50)] @[noise("depolarizing")] @[basis("X")] @[decoherence_free]
void f(Qubit q) { h(q); }
```

| 标签 | 语义 |
| --- | --- |
| `@[coherence(t1, t2)]` | 相干时间（T1 弛豫 / T2 退相，单位 μs；须满足 `0 < T2 ≤ T1`） |
| `@[noise("model")]` | 噪声模型（`depolarizing` / `amplitude_damping` / `phase_damping` / `bit_flip`） |
| `@[basis(X\|Y\|Z)]` | 指定测量基 |
| `@[decoherence_free]` | 无退相干子空间（DFS） |
| `@[error_correction("surface")]` | 纠错码 |

### 15.10 QCOS 可启动内核

rvdl 的系统级构造可直接用于编写裸机内核：`runtime/qcos/` 提供 Multiboot2 引导汇编
（`boot_x86_32.S` / `boot_x86_64.S`）、`kernel_main.cpp` 入口、`rvdl_shim.cpp` /
`rvdl_shim32.cpp` 桥接与 `linker_x86_64.ld` 链接脚本；`qcos_core` 是 freestanding
内核核心库（DragonPmm / Vmm / Spinlock / Sched / Serial / Qms，无 LLVM / Kokkos /
libstdc++ 依赖）。

`rvdl_shim.cpp` / `rvdl_shim32.cpp` 同时桥接完整的 QCOS syscall ABI（`rvdl_sys_call` /
`rvdl_sys_calld` / `rvdl_sys_log` / `rvdl_sys_logi` / `rvdl_sys_callp` / `rvdl_gc_alloc` /
`rvdl_gc_free`，见 12.7 节），串口为唯一输出通道；其 freestanding 实现集中在
`runtime/qcos/qcos_syscall.hpp`，语义与 hosted `qhal/QcosSyscall.hpp` 一致。

> QCOS 内核隶属正式项目 [QuarkOS](https://github.com/LukaKrajina/QuarkOS)。

构建链（RVDL 源码 → LLVM IR → freestanding object → 链接引导 stub → 可启动 ELF / ISO）：

```bash
.\scripts\build-qcos-kernel.ps1 -Source examples\qcos_kernel.rvdl   # RVDL → 可启动 ELF
.\scripts\build-qcos-iso.ps1                                       # → 可启动 ISO
qemu-system-x86_64 -cdrom runtime/build/qcos.iso -display none -serial stdio
```

示例：`examples/qcos_kernel.rvdl`（串口 + 位图分配器 + IDT 打包）、
`examples/qcos_quantum_service.rvdl`（QMS 量子服务下沉到裸机）、
`examples/rvdl_idt.rvdl` / `examples/rvdl_idt2.rvdl`（IDT 条目打包）、
`examples/rvdl_bitmap.rvdl`（位图分配器）、`examples/rvdl_buddy.rvdl`（伙伴系统）。

---

## 16. 并发与纠缠（spawn / entangle）

rvdl 用 `spawn` 派生并发线程、`entangle` 声明量子纠缠，二者为 Q-Digest 静态竞争检测
（digest 框架）提供「线程」与「纠缠闭包」的构造原语。

### 16.1 spawn

```rvdl
spawn {
    // 并发执行体，是竞争检测的"线程"单元
    // 编译为独立线程函数 @rvdl_thread_N，经 rvdl_spawn 内建以 std::thread 启动
    Qubit q = alloc();
    h(q);
    int32 r = measure(q);
}
```

`spawn` 块编译为独立线程函数 `@rvdl_thread_N`，经 `rvdl_spawn` 内建以 `std::thread` 启动（detach），
实现**真实并发**（不再是串行内联降级）。线程函数支持**闭包捕获**外层局部变量——外层变量经
env 闭包结构传递（复用 lambda 的闭包机制）。

### 16.2 entangle

```rvdl
entangle(q1, q2);   // 声明 q1 与 q2 纠缠
```

纠缠具有传递性：对 `q1` 的破坏性操作（测量/释放）会影响与 `q1` 纠缠的 `q3`。竞争检测
据此构建纠缠闭包，跨线程触及同一闭包且无共同锁时报告量子竞争。

---

## 17. 借用检查与线性类型

rvdl 引入 Polonius 风格的借用检查器（`borrow.ts`，刻意比 rustc 保守）与量子线性类型
（QLT），在 MIR 上静态保证内存与量子资源安全。

### 17.1 量子线性类型（QLT）

`Qubit` / `QObject` 受 no-cloning 约束，每个线性变量必须在每条执行路径上**恰好**被消费
一次（消费 = `measure` / `release` / 按值 move）：

```rvdl
Qubit q = alloc();      // 线性变量
// let q2 = q;          // E-Q001：不可克隆
int32 r = measure(q);   // 消费 q（之后不可再用）
// 若 q 从未被消费     // E-Q002：线性泄漏
```

**具体量子类型（`QuantumRegister` / `DiracState` / `BellState`）同样纳入 QLT**：重复 move 报
`E-Q001`。它们在 C++ 侧析构时自动 measure + release_qubit，因此函数末尾未消费时**隐式 Drop**
（IR 层 `emitScopeCleanup` 生成 `rvdl_release_object`），不报 `E-Q002`；仅 `Qubit` / `QObject`
裸类型强制显式消费。

`QObject` 的**部分坍缩测量** `qmeasure(qobj, i)` 用 `Ref` 借用（不消费整个 QObject），因此同一
QObject 可测多个 qubit、测后仍可 move 返回，不会误报 `E-Q001`。

### 17.2 借用冲突

| 错误码 | 含义 |
| --- | --- |
| `E-Q001` | 量子不可克隆（重复消费同一线性变量） |
| `E-Q002` | 线性泄漏（线性变量未被消费） |
| `E0499` | 同一 place 同时存在两个可变借用 / 可变与共享借用并存 |
| `E0506` | 共享借用活跃时对该 place 写入 |

借用检查以「生命周期 = 程序点集合」的子集式（subset）思想实现，不采用 NLL 的约束求解，
以规避如 rustc 在该区域的可靠性缺陷。

---

## 18. 编译管线与运行

| 命令 | 说明 |
| --- | --- |
| `rvdl run <file.rvdl>` | 编译并运行 |
| `rvdl ir <file.rvdl>` | 输出 LLVM IR |
| `rvdl compile <x32\|x64\|arm64\|android> <-e\|-m> <file.rvdl> [-o <out>]` | AOT 编译为原生二进制（`android` = aarch64-linux-android 交叉编译）；默认输出到源文件同目录的 `<name><扩展名>` |
| `rvdl build apk <file.rvdl> [--release]` | 一键把 `.rvdl` 项目构建为 Android APK（AOT 编译 + 轻量运行时 + Gradle 打包） |
| `rvdl verify <file.rvdl> [--smt [out]]` | 静态验证契约 |
| `rvdl serve <model.rvdlm> [--port p]` | 启动推理服务 |

运行依赖 daemon（`./runtime --daemon`，监听 `localhost:50052`）。

### 18.1 Android 交叉编译

`rvdl compile android` 把 `.rvdl` AOT 编译为 `aarch64-linux-android` 目标（`librvdl_main.so`，
默认输出到源文件同目录），经 NDK 交叉编译器链接；`rvdl build apk` 则进一步与轻量运行时
`librvdl_rt.so`（QVM 核心，无 LLVM JIT）一并打入 APK，由 Java JNI 壳加载调用 `rvdl_main`，
ARM64 设备安装即运行。交叉编译所需的工具链路径（`RVDL_ANDROID_CLANGXX` / `RVDL_ANDROID_SYSROOT`
/ `RVDL_ANDROID_RT_DIR` / `RVDL_AOT_EXTRA_LIBS`）会由 CLI 随编译请求转发给 daemon，
无需 daemon 进程预置这些环境变量。

---

## 19. 附录：关键字与错误类型

### 19.1 关键字

```text
// 基础与控制流
let auto int new return if else while for break continue fn
int8 int16 int32 int64 uint8 uint16 uint32 uint64
float double complex64 complex128 string char
// 量子类型与门
Qubit QObject QModel QReservoir DiracState BellState QuantumRegister
alloc measure encode_text encode_image qlm_invoke qlm_load
rvdl_encode_string qlm_forward rvdl_decode_string
mind_read mind_train mind_feedback veda_qlm_train
// QRC 量子储备池 + TQNF 拓扑量子神经场
qrc_new qrc_train qrc_release qrc_probe qrc_predict
qgate_h qgate_x qgate_rz qgate_cnot qgate_cnot_pair
qexpect_z qmeasure qobj_num_qubits qattention
qstate_entropy qstate_fidelity dla_dim shannon4 shannon8
// 量子通道逆因果容量（回程能力）
retrocausal_imax retrocausal_idoe retrocausal_q_capacity retrocausal_c_capacity
retrocausal_q_one_shot retrocausal_gain retrocausal_deformed
// 快子 KK 双空间嘈杂 CTC 通信
retrocausal_ctc_q_capacity retrocausal_ctc_c_capacity
retrocausal_ctc_gain retrocausal_ctc_dephasing
// 非欧几里德曲面体几何
geodesic_distance inversion hyperbolic_metric hyperbolic_distance
// 类型系统与模块
mod use pub form impl trait template rank self flavor fuse morph
export import requires ensures invariant result from
// 神经/软逻辑原语
surrogate tanh_quantize lif_step mellowmax2 logsumexp2 boltzmann2
tnorm_luk tnorm_prod tnorm_godel polymer_weight polymer_mix_bound
complex real imag cabs conj
// 系统级（裸机 / 内核）
make cap unsafe null native fixed
sync_load sync_store sync_add sync_cas sync_lock sync_unlock
volatile_load volatile_store
read_cr0 read_cr2 read_cr3 read_cr4 write_cr0 write_cr3 invlpg
rdmsr wrmsr cpuid rdtsc read_rflags xgetbv
outb inb outw inw outl inl rvdl_gc_alloc rvdl_gc_free addr
asm arr fn
rvdl_sys_call rvdl_sys_calld rvdl_sys_log rvdl_sys_logi rvdl_sys_callp
rvdl_qms_gap rvdl_mix_bound rvdl_qms_conc
// 量子化命名范式与并发
route path fallback spin spawn entangle
// 标签（多维拓扑 + 可逆编织 + 物理特性 + 经典属性）
layer gate undo steer unitary measure
coherence noise basis decoherence_free error_correction
inline noinline pure readonly cold hot noreturn
section naked place raw export
// @layer 键（**不是关键字**：在 @layer(...) 内按标识符解析，见 §8.5.1）
shuttle shuttle_lambda shuttle_phase shuttle_rounds shuttle_strategy
track track_mode track_slots
// 量子门（单比特 + 双比特）
h x y z s t rz rx ry cnot toffoli swap qft iqft braid
// 受控门
cx ch crz cswap c_toffoli cqft cbraid
```

### 19.2 常见错误类型

| 类别 | 示例 |
| --- | --- |
| `Type Error` | 类型不匹配 |
| `Quantum Violation` | 克隆 qubit / 测量后使用 qubit |
| `Borrow Error` | `E-Q001` 不可克隆 / `E-Q002` 线性泄漏 / `E0499` 借用冲突 / `E0506` 共享借用写入 |
| `Race Error` | Q-Digest 检测到的数据竞争 / 量子竞争（纠缠闭包） |
| `Reference Error` | 未定义变量 / `break` 在循环外 |
| `Signature Error` | 参数数量不符 |
| `Inheritance Error` | 重复继承 / 父类型不存在 |
| `Impl Error` | 未实现 trait 方法 / trait 不存在 |
| `Contract Error` | `requires` / `ensures` / `invariant` 类型非布尔 |
| `Ambiguity Warning` | 门名与变量名冲突 |
| `Topology Error` | `E-TOP001` 显式函数缺 `@layer` / `E-TOP003` 坐标占用 / `E-TOP005` 叠加链空槽 / `E-TOP006` 传播延迟超 deadline / `E-TOP007` coord 维度跨度 > 1（跨一个以上维度的投影无良定义路径；**维度差恰为 1 是合法投影**，原 `E-TOP002` 维度不一致检查已放宽为 no-op）/ `E-TOP008` `@layer` 未知键（原为静默丢弃）/ `E-TOP009` `time`·`thread`·`cost`·`deadline` 取负（`coord` 允许为负） |
| `Shuttle Error` | `E-SHU001` 目标 coord 无匹配块 / `E-SHU002` 目标存在但无可用关联对端 / `E-SHU003` `shuttle_lambda ≤ 0` / `E-SHU004` `shuttle_phase ∉ {0,1,2}` / `E-SHU005` `shuttle_strategy ∉ {−1..4}` / `E-SHU006` `shuttle_rounds < 0` / `E-SHU007` 规划轮次超出 `deadline` / `E-SHU008` 对端与本块同 coord 同 time |
| `Tracking Error` | `E-TRK001` `track` 为空或 `track_slots < 1` / `E-TRK002` 追踪源在块作用域内不可解析（无量子参数/构造/`alloc()`/量子态返回调用）/ `E-TRK003` 可观测量不在已知表内（强制 L0 时对非 Pauli 族也报此码）/ `E-TRK004` `track_mode ∉ {0..3}` |
| `Shuttle/Tracking Warning` | `W-SHU001` 声明 `shuttle` 但本块无 `@track` → 量子相干路径无影子供给，退化为经典影子混合（**warning 通道：不阻断执行**）/ `W-TRK001` `strategy=3`（Grover）但缺 ancilla 工作寄存器 → 降级为策略 0 |
| `Quantum Attr Error` | `E-QUNI` `@[unitary]`/`@[gate]` 函数含测量（不可逆）/ `E-QSYN` `@[undo]`/`@[steer]` 缺 `@[gate]`/`@[unitary]` 前提 |
| `Physical Error` | `E-PHY` `@[coherence]` 违反 `0 < T2 ≤ T1` / `@[noise]` 未知噪声模型 |

---

> 相关文档：[README](../README.md) · [穿梭与追踪](./rvdl-shuttle-tracking.md) · [量子机器人仿真平台手册](./rvdlrsp-manual.md) · [rvdl 量子学习手册](./rvdl-quantum-learning-manual.md)