# rvdl 语言参考手册

> Ravendrel（`.rvdl`）是一门面向量子计算、量子机器学习与量子机器人的实验性语言。本手册按「怎么写、是什么意思」组织，像查 C++/Rust 手册一样按需检索。理论背景见文末延伸阅读。

## 目录

- [快速上手](#快速上手)
- [词法](#词法)
- [类型](#类型)
- [变量与表达式](#变量与表达式)
- [控制流](#控制流)
- [函数与入口](#函数与入口)
- [量子编程](#量子编程)
- [内置函数速查](#内置函数速查)
- [类、模块与类型定义](#类模块与类型定义)
- [系统级编程](#系统级编程)
- [并发与契约](#并发与契约)
- [编译与运行](#编译与运行)
- [诊断码](#诊断码)
- [延伸阅读](#延伸阅读)

---

## 快速上手

```rvdl
// hello.rvdl —— 脚本模式：直接写语句，自动包成 rvdl_main
let q = alloc();
h(q);
int32 r = measure(q);
```

显式入口（`@layer` 多维标签函数）：

```rvdl
@layer(time=0, thread=0, coord=(0,0))
int32 main() {
    int32 x = 42;
    int32 y = x + 8;
    return y;
}
```

```bash
rvdl run hello.rvdl    # 连接 daemon 执行
rvdl ir hello.rvdl     # 只输出 LLVM IR，不连接 daemon
```

---

## 词法

| 项目 | 规则 |
| --- | --- |
| 注释 | 仅行注释 `// ...` |
| 标识符 | 字母（含 Unicode）或 `_` 开头，后续可为字母、数字、`_`；`let 变量 = 3;` 合法 |
| 整数 | `0` / `42` / `50000`，默认推导为 `int32` |
| 浮点 | `0.1` / `3.14` / `1.0`，默认推导为 `double` |
| 字符串 | `'...'` 与 `"..."` |
| 转义 | `\n` `\t` `\r` `\\` `\"` `\'` `\0` |

---

## 类型

### 标量

| 类型 | 含义 | LLVM 映射 |
| --- | --- | --- |
| `int8` / `uint8` | 8 位有符号 / 无符号整数 | `i8` |
| `int16` / `uint16` | 16 位 | `i16` |
| `int` / `int32` / `uint32` | 32 位（`int` = `int32`） | `i32` |
| `int64` / `uint64` | 64 位（`uint64` 无符号，承载 48 位 LBA/GPT 字段） | `i64` |
| `float` / `double` | 单 / 双精度浮点 | `float` / `double` |
| `string` / `char` | 字符串 / 字符 | `i8*` / `i8` |
| `bool` | 比较结果 | `i1` |
| `void` | 无返回值 | — |
| `complex64` / `complex128` | 单 / 双精度复数 | `{float,float}` / `{double,double}` |

### 量子类型

| 类型 | 含义 |
| --- | --- |
| `Qubit` | 单个量子比特，不可克隆，函数末尾自动释放 |
| `QObject` | 量子对象，容纳多 qubit 状态 |
| `QModel` | 量子语言模型（`.rvdlm`） |
| `QReservoir` | 量子储备池，由 `qrc_new` 创建 |

### 复合类型

| 类型 | 含义与用法 |
| --- | --- |
| `cap<T>` | 能力指针（CHERI 式地址+边界+权限），解引用须在 `unsafe` 中 |
| `arr<T, N>` | 定长数组，字面量 `[e0, e1, ...]`，索引 `a[i]`，映射 `[N x T]` |
| `fn<ret(params)>` | 函数指针，取地址用 `&` |
| `fixed` | 编译期常量，如 `fixed int32 MAX = 100;` / `fixed arr<int32,4> SCANCODES = [...]` |
| `null` | 空指针 / 空能力值 |

类型推导：`let` 与 `auto` 等价，均按初值推导。

```rvdl
auto q = alloc();  let n = 42;  let f = 3.14;  let s = "ravendrel";
auto bell = new BellState();
complex128 z = complex(1.0, 2.0);
double re = real(z), im = imag(z), m = cabs(z);  // 实部/虚部/模长
```

复数运算：`+` `-` `*` 与 `real` / `imag` / `cabs` / `conj`。

---

## 变量与表达式

```rvdl
int32 count = 0; double lr = 0.01; Qubit q = alloc();
auto bell = new BellState();
count = count + 1;

// 一行多变量，后声明的可用先声明的
int32 a = 1, b = 2, c = a + b;
let s = "hi", t = "yo";
```

类型必须一致：`int32 a = "hello";` 报 `Type Error`。

**运算符**：`+ - * /`（算术，整数用有符号除法）、`< <= > >=`、`== !=`、`&& || !`（短路）、一元 `-` / `!`、`a[i]`（索引）、`&fn`（取函数地址）、`*p`（解引用，须 `unsafe`）、`T field : N`（位域）。

---

## 控制流

```rvdl
// if / else
if (a > 0 && b < 10) { ... } else { ... }

// while（可带 invariant 与 else：循环体零次执行时进入 else）
while (i < 10) invariant i >= 0 {
    i = i + 1;
} else { ... }

// for（三段式）
for (int32 i = 0; i < 10; i = i + 1) { ... }

// break / continue
while (true) { if (done) { break; } ... continue; }

// route：switch 的量子化命名
route(code) {
    path 0: { ... }
    path 1: { ... }
    fallback: { ... }
}

// spin：do-while，至少执行一次
spin { ... } while (cond);
```

---

## 函数与入口

```rvdl
int32 add(int32 a, int32 b) { return a + b; }
void do_nothing() { return; }

// lambda / 闭包
let twice = fn(int32 x) -> int32 { return x * 2; };
```

### `@layer` 多维标签函数

程序入口不再单一，而用 `@layer` 把执行映射到「时间 × 线程 × 运行层坐标」拓扑：

```rvdl
@layer(time=0, thread=0, coord=(0,0))
int32 producer() { return 42; }

@layer(time=0, thread=1, coord=(0,1))   // 与 producer 平行
int32 observer() { return 7; }

@layer(time=1, thread=0, coord=(0,0))   // 与 producer 叠加
int32 consumer() { return 0; }
```

常用键：

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `time` | 根块必填 | 锚点时间（≥0，可省略以继承调用者时钟） |
| `thread` | 必填 | 逻辑线程（同线程串行、异线程可并行） |
| `coord` | 必填 | N 维格点坐标（可为负，是地址不是大小） |
| `cost` | `1` | 执行成本，功能周期 `[time, time+cost)` |
| `deadline` | 无 | 时间上限 |
| `dual` | `0` | `1` 标记 T-对偶反转空间 |
| `shuttle` / `track` | 无 | 穿梭 / 追踪，见 [延伸阅读](#延伸阅读) |

块间关系自动推导为五种拓扑边：同 coord 相邻 time → 叠加、异 coord → 平行、维度不同 → 投影、同 coord + `dual` → 反转、同 coord 异 thread → 交叉。

### 属性标签 `@[...]`

| 类别 | 标签 | 含义 |
| --- | --- | --- |
| 内联 | `@[inline]` / `@[noinline]` | 强制 / 禁止内联 |
| 副作用 | `@[pure]` / `@[readonly]` | 无副作用 / 只读 |
| 路径 | `@[hot]` / `@[cold]` | 热 / 冷路径提示 |
| 返回 / 符号 | `@[noreturn]` / `@[export]` | 不返回 / 导出符号 |
| 裸机 | `@[naked]` / `@[raw]` / `@[section("...")]` / `@[no_gc]` | 无栈帧、段放置、禁用 GC |
| 布局 | `@[packed]` | 精确字节布局（无填充，字段从偏移 0 起） |
| 可逆编织 | `@[gate]` `@[undo]` `@[steer]` `@[unitary]` | 标记门、自动合成逆门 / 受控门、要求酉性 |
| 物理 | `@[coherence(t1,t2)]` `@[noise("...")]` `@[basis(X|Y|Z)]` `@[decoherence_free]` | 相干时间、噪声模型、测量基、无退相干子空间 |

```rvdl
@[gate] @[undo] @[steer]
void U(Qubit q) { h(q); rz(q, 0.5); }   // 自动合成 U_undo / U_steer

@[coherence(100, 50)] @[noise("depolarizing")] @[basis("X")]
void noisy_gate(Qubit q) { x(q); }       // 要求 0 < T2 ≤ T1
```

噪声模型取值：`depolarizing` / `amplitude_damping` / `phase_damping` / `bit_flip`。

---

## 量子编程

### 分配与测量

```rvdl
Qubit q = alloc();
h(q);
int32 r = measure(q);      // Z 基测量并坍缩，返回 0/1
int32 rx = measure_x(q);   // X 基
int32 ry = measure_y(q);   // Y 基
```

### 量子门

| 门 | 签名 | 含义 |
| --- | --- | --- |
| `x` `y` `z` | `(Qubit)` | Pauli 门 |
| `h` | `(Qubit)` | Hadamard |
| `s` / `t` | `(Qubit)` | π/2 / π/4 相位门 |
| `rx` `ry` `rz` | `(Qubit, double)` | 绕轴旋转 |
| `cnot` / `cx` | `(Qubit ctrl, Qubit target)` | 受控非 |
| `toffoli` | `(Qubit, Qubit, Qubit)` | 两控制一目标 |
| `swap` | `(Qubit, Qubit)` | 交换 |
| `ch` `crz` `cswap` `c_toffoli` `cqft` `cbraid` | 受控版本 | 受控 H / Rz / SWAP / Toffoli / QFT / 编织 |
| `qft` / `iqft` | `(int)` | 量子傅里叶变换 / 逆变换 |
| `braid` | `(Qubit, Qubit)` | Yang-Baxter 编织 |

任意基单比特态：`basis_state(theta, phi, value)`，返回 `QObject`，`value=0/1` 选 +/− 本征态。

### QObject 与内置类

```rvdl
auto bell = new BellState();          // Bell 态
auto reg  = new QuantumRegister(15); // 15 比特寄存器
auto psi  = new DiracState(4);       // 4 维狄拉克态
int32 m = bell.measure();            // 消费并测量整个对象
int32 n = qobj_num_qubits(reg);      // 查询 qubit 数
```

QObject 层原语（借用不消费，除非 `qmeasure`）：

| 函数 | 含义 |
| --- | --- |
| `qgate_h` `qgate_x` `qgate_rz` `qgate_cnot` | 对第 i 个 qubit 施加门 |
| `qgate_cnot_pair(a, ia, b, ib)` | 跨对象受控非（POVM 弱测量） |
| `qexpect_z(q, i)` | 非破坏读取 ⟨Z⟩ |
| `qmeasure(q, i)` | 部分坍缩第 i 个 qubit，其余存活 |
| `qattention(q, k)` | SWAP test 态重叠 \|⟨ψq\|ψk⟩\|² |
| `qstate_entropy(q)` / `qstate_fidelity(a,b)` | 约化态熵 / 保真度（无态矢量或 >10 qubit 返回 -1） |
| `dla_dim("X,Z", n)` | 动力学李代数维数（可训练性诊断） |
| `shannon4(...)` / `shannon8(...)` | 4 / 8 态测量香农熵 |

```rvdl
// POVM 弱测量：辅助位不完全坍缩主态
QObject sys = new BellState(), anc = alloc_qobject();
qgate_cnot_pair(sys, 0, anc, 0);
qmeasure(anc, 0);
```

---

## 内置函数速查

### 文本 / 图像编码

| 函数 | 含义 |
| --- | --- |
| `encode_text(string)` / `encode_image(string)` | 文本 / 图像 → `QObject` |
| `encode_pixel(string, int32)` | 像素振幅编码 |
| `rvdl_encode_string(string)` / `rvdl_decode_string(QObject)` | 字符串 ↔ 量子对象 |

### 量子语言模型（QLM）

| 函数 | 含义 |
| --- | --- |
| `qlm_load(string) -> QModel` | 加载 `.rvdlm` |
| `qlm_forward(QModel, QObject)` | 前向推理 |
| `qlm_invoke(QObject, int32, double) -> QModel` | 训练并返回模型，`.export(path)` 导出 |

### 脑机接口 / VedaROS

`mind_read("eeg")`（模态 `stream/spike/lfp/eeg/sensor`）、`mind_train(QObject,int,double)`、`mind_feedback(QObject)`、`veda_qlm_train(QObject,int,double)`。

### QRC 量子储备池

| 函数 | 含义 |
| --- | --- |
| `qrc_new(qubits, layers[, out_dim])` | 创建储备池 |
| `qrc_train(res, epochs, lr)` / `qrc_train_ex(res, epochs, lr, inputs, targets, n_samples, n_features, n_outputs)` | 合成 / 真实数据训练线性读出 |
| `qrc_probe(res, data)` / `qrc_predict(res, data)` | 特征编码 / 读出预测 |
| `qrc_release(res)` | 释放 |

### 数学 / 神经 / 几何原语

| 函数 | 含义 |
| --- | --- |
| `surrogate` `tanh_quantize` `lif_step` `mellowmax2` `logsumexp2` `boltzmann2` | 标量神经原语 |
| `tnorm_luk` `tnorm_prod` `tnorm_godel` `polymer_weight` `polymer_mix_bound` | 模糊逻辑 / polymer |
| `real` `imag` `cabs` `conj` `complex` | 复数运算 |
| `geodesic_distance(a,b)` / `inversion(R)` / `hyperbolic_metric(x)` / `hyperbolic_distance(x,y)` | Fubini-Study 测地距离 / T-对偶反转 / Poincaré 双曲度规与距离 |

### 逆因果 / CTC 容量

| 函数 | 含义 |
| --- | --- |
| `retrocausal_imax(kind,p)` `retrocausal_idoe(kind,p)` | max-information / Doeblin information |
| `retrocausal_q_capacity` `retrocausal_c_capacity` `retrocausal_gain` | 渐近量子 / 经典容量、回程增益 |
| `retrocausal_q_one_shot(kind,p,eps)` `retrocausal_deformed(kind,p,q)` | 单次 / q-变形容量 |
| `retrocausal_ctc_q_capacity(n,theta,mu2,lambda)` 等 | 快子 KK 双空间 CTC 容量 |
| `retrocausal_ctc_dephasing(theta)` | Wilson line 退相映射 |

`kind`：`0` 去极化、`1` 退相、`2` 比特翻转、`3` 振幅阻尼、`4` 幺正 Hadamard、`5` 比特-相位翻转。

### 系统内建

| 类别 | 内建 |
| --- | --- |
| 原子 / 自旋锁 | `sync_load` `sync_store` `sync_add` `sync_cas` `sync_lock` `sync_unlock` |
| volatile | `volatile_load` `volatile_store` |
| 端口 I/O | `outb/outw/outl` `inb/inw/inl` |
| 控制寄存器 | `read_cr0/2/3/4` `write_cr0/3` `invlpg` |
| MSR / CPU | `rdmsr` `wrmsr` `cpuid` `rdtsc` `read_rflags` `xgetbv` |
| 堆 / 地址 | `rvdl_gc_alloc` `rvdl_gc_free` `addr` |
| 汇编 | `native("hlt")`（单条）、`asm { "..." }`（多指令，配 `@[naked]`） |
| syscall | `rvdl_sys_call` `rvdl_sys_log` 等（`SYS_LOG=1` `YIELD=2` `EXIT=3` `TIME=10`） |
| QMS 数值 | `rvdl_qms_gap` `rvdl_mix_bound` `rvdl_qms_conc` |

---

## 类、模块与类型定义

### `form` / `trait` / `impl`

```rvdl
form Point { double x; double y; }

trait Shape { double area(); }
form Circle { double r; }
impl Shape for Circle {
    double area() { return 3.14159 * r * r; }
}

template<T> form Box { T value; }   // 泛型：new Box<int32>()
```

`flavor`（枚举）、`fuse`（模式匹配）、`morph`（卫生宏）：

```rvdl
flavor Channel { DEPOLARIZING, DEPHASING, AMPLITUDE_DAMPING, PAULI }
int32 code = Channel.PAULI;         // == 3

int32 f(int32 m) {
    return fuse(m) { 0:10, 1:20, 2:30, _:0 };   // _ 通配
}

morph twice($e: expr) { ($e) + ($e) }
int32 y = twice!(x * 3);            // 展开为 (x*3)+(x*3)，卫生不覆盖外部变量
```

### 模块（`.rvdli`）

```rvdl
// 定义并导出
mod math {
    pub int32 add(int32 a, int32 b) { return a + b; }
    export int32 square(int32 x) { return x * x; }
}

// 导入（.rvdli 为 QOBF v2 加密二进制：序列化 + ChaCha20 + HMAC）
import math from "./math.rvdli";
requires io.network;                 // 权限声明
let result = math.add(1, 2);
```

关键字：`mod` `use` `pub` `import` `from` `export` `requires`。

---

## 系统级编程

危险操作须置于 `unsafe { ... }` 中；能力指针 `cap<T>` 与裸汇编配合可写裸机内核：

```rvdl
unsafe {
    cap<int32> p = null;
    *p = 5;
    int32 x = *p;
}

@[naked] void irq_stub() {
    asm { "push rax;" "iretq;" }
}

uint64 cr3 = read_cr3();
```

位域：`uint8 readonly : 1; uint8 hidden : 1; uint8 reserved : 6;`

可启动内核见 `runtime/qcos/`，用 `scripts/build/qcos/` 下的脚本构建 ELF/ISO。

---

## 并发与契约

```rvdl
// spawn：真实线程（std::thread），支持闭包捕获
spawn {
    Qubit q = alloc();
    h(q);
    int32 r = measure(q);
}

// entangle：声明纠缠，供 Q-Digest 构建纠缠闭包
entangle(q1, q2);
```

契约（配 `rvdl verify` 静态验证，可导出 SMT-LIB）：

```rvdl
int32 sqrt_abs(int32 x)
    requires x >= 0;
    ensures result >= 0;
{ ... }

rvdl verify prog.rvdl
rvdl verify prog.rvdl --smt out.smt2
```

量子线性类型（QLT）：Qubit 不可克隆、必须恰好消费一次。`let q2 = q;` 报 `E-Q001`，测量后使用报 `Qubit used after measurement`。

---

## 编译与运行

```bash
rvdl run <script.rvdl>                                   # 运行
rvdl ir <script.rvdl>                                    # 输出 LLVM IR
rvdl compile <x32|x64|arm64|android> <-e|-m> <file> [-o out]  # AOT 编译
rvdl verify <script.rvdl> [--smt out.smt2]               # 静态验证
rvdl build apk <script.rvdl> [--release]                 # Android APK
rvdl serve <model.rvdlm> [--port 9080]                   # 推理服务
```

---

## 诊断码

| 类别 | 含义 |
| --- | --- |
| `Type Error` | 类型不匹配 |
| `Quantum Violation` | 克隆 qubit / 测量后使用 / 线性泄漏 |
| `Borrow Error` | `E-Q001` 不可克隆、`E-Q002` 线性泄漏、`E0499` 借用冲突、`E0506` 共享借用写入 |
| `Race Error` | Q-Digest 检测到的数据 / 量子竞争 |
| `Topology Error` | `E-TOP005` 叠加链空槽、`E-TOP006` 超 deadline、`E-TOP008` 未知 `@layer` 键、`E-TOP009` 负值 |
| `Shuttle Error` | `E-SHU001..008`：目标无匹配 / 无关联边 / λ≤0 / phase/strategy 非法 / 超 deadline 等 |
| `Tracking Error` | `E-TRK001..004`：track 空 / 无量子源 / 观测量未知 / mode 非法 |
| `Quantum Attr Error` | `E-QUNI`（带测量）、`E-QSYN`（`@[undo]` 缺 `@[gate]`） |

完整前缀：`E-Q001`（不可克隆）、`E-Q002`（线性泄漏）、`E-TOP*`（拓扑）、`E-SHU*`（穿梭）、`E-TRK*`（追踪）。

---

## 延伸阅读

本手册只讲「怎么用」。各子系统的深入说明：

- [量子学习手册](rvdl-quantum-learning-manual.md) — QLM / QML / Numqk / QbNS 训练与推理
- [TQNF 拓扑量子神经场](rvdl-topological-quantum-learning.md) — 几何/拓扑/耗散视角
- [穿梭与追踪](rvdl-shuttle-tracking.md) — `@layer` 的 `shuttle` / `track` 键详解
- [BAT 批量伴随](quantum-batching-theory.md) — minibatch / tile 加速
- [迁移手册](rvdl-migration-manual.md) — OpenQASM / Q# / Quil / Silq → rvdl
- [qchain 量子区块链](rvdl-qchain-manual.md) — 后量子密码与量子账本
- [rvdlRSP 仿真平台](rsp-manual.md) — 量子机器人仿真