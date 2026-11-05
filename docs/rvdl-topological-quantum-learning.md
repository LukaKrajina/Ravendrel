# TQNF —— 拓扑量子神经场：量子学习新范式

> 本手册阐述 Ravendrel 量子学习栈的**新范式**：**TQNF（Topological Quantum Neural Field，拓扑量子神经场）**。
> 它回答一个根本问题：*量子学习没有经典意义上的「层」与「权重」，那么它以什么为基本构造块？*
> 答案是——**几何（Fubini-Study 度量）、拓扑（纠缠/并置结构）、耗散（量子信道）** 三者构成的量子态流形演化流。
>
> 相关代码：`runtime/include/numqk/ComplexTensor.hpp`（量子态空间线性代数）、
> `runtime/include/numqk/QuantumGeometry.hpp`（几何/李代数/耗散学习原语），
> 建立在 `numqk/Numqk.hpp`（`Tensor` + Autograd）与 `numqk/SoftLogic.hpp`（软逻辑）之上。
> 它与既有的 **DQNF**（耗散量子神经场，见 `qml/Reservoir.hpp` 的「法则」）一脉相承并系统化。

---

## 目录

1. [问题与立场](#1-问题与立场)
2. [前沿依据](#2-前沿依据)
3. [三大支柱 ↔ `@layer` 三维标签](#3-三大支柱--layer-三维标签)
4. [设计法则](#4-设计法则)
5. [张量原语映射](#5-张量原语映射)
6. [量子深度学习的含义](#6-量子深度学习的含义)
7. [代码示例](#7-代码示例)

---

## 1. 问题与立场

经典深度学习的构造块是：

```
层（layer，函数复合）+ 权重（weight，标量参数）+ 反向传播（backprop，链式法则）
```

量子学习**不复制**这个框架，原因有三，且都有严格结论支撑：

1. **「深度」在量子里不是免费的**。深参数化酉电路的损失函数方差随深度以指数趋近于零，
   即**贫瘠高原（barren plateau）**。经典里「越深越强」的直觉在量子里失效。
2. **「权重」在量子里是几何对象而非标量**。参数 θ 刻画的是射影希尔伯特空间
   `CP^{n-1}` 上的一个点；其「正确」的下降方向由 **Fubini-Study 度量** 决定，而非欧氏坐标。
3. **「层」在 Ravendrel 里是拓扑标签而非顺序堆叠**。rvdl 的 `@layer(time, thread, coord)`
   表达的是**多维时空拓扑**（叠加 / 平行 / 纠缠），不是神经网络的一维深度。

因此 TQNF 提出：**量子学习 = 在量子态流形上塑造「几何 + 拓扑 + 耗散」的演化流**。
「可学习」的东西不再是一堆标量权重，而是：

- 系统的**几何**（度量 / 自然梯度方向）；
- 系统的**拓扑**（哪些子系统纠缠、哪些并置、哪些叠加）；
- 系统的**耗散**（噪声/退相干如何被**工程化**为可训练资源）。

---

## 2. 前沿依据

| 论文 | 核心结论 | 对 TQNF 的启示 |
| --- | --- | --- |
| Ragone et al., *A Unified Theory of Barren Plateaus for Deep Parametrized Quantum Circuits* (arXiv:2309.09342; Nature Communications 2024) | 损失方差由**动力学李代数（DLA）** 的维数决定：DLA 指数大 → 贫瘠高原 | 用 **DLA 维数** 作为「可训练深度」的一阶诊断；小代数（等变）→ 可训练 |
| Sannia et al., *Engineered dissipation to mitigate barren plateaus* (npj Quantum Information 10, 2024) | **耗散可以被工程化为建设性资源**，规避贫瘠高原 | 「耗散-可训练性对偶律」（见法则 1） |
| Schatzki et al., *Theoretical guarantees for permutation-equivariant quantum neural networks* (npj Quantum Information 10, 2024) | **对称性/等变性** 收缩 DLA，带来可训练性保证 | 「拓扑-代数收缩律」（见法则 5） |
| Stokes et al., *Quantum Natural Gradient* (Quantum 4:269, 2020) | Fubini-Study 度量给出**几何感知**的参数更新，规避坐标奇异性 | 「几何-贫瘠高原对偶律」（见法则 2） |
| Beer et al., *Training deep quantum neural networks* (Nature Communications 2020) + PRL 2022 梯度标度分析 | **耗散量子神经网络（DQNN）** 的梯度标度可控，比酉网络更可训练 | 「深度-耗散律」（见法则 3） |
| Sharma et al. / Pérez-Salinas et al. (data reuploading, 2020) | **显式模型**（变分可观测量/数据重加载）vs **隐式模型**（量子核） | TQNF 是显式+几何的混合：表征由拓扑定，学习由几何/耗散行 |
| Quantum linear algebra for Transformer attention (arXiv:2402.16714) | 注意力可用块编码 / 量子线性代数实现 | `coord` 维的并置分支 → 块编码（block encoding）张量原语 |

---

## 3. 三大支柱 ↔ `@layer` 三维标签

rvdl 的 `@layer(time, thread, coord)` 是**多维拓扑调度标签**（见 [rvdl 语言手册](./rvdl-language-manual.md) 第 8 节）。
TQNF 将它重新诠释为量子学习的三个正交自由度——这就是「量子深度学习」取代「层/权重」的方式：

| `@layer` 维度 | 经典 NN 类比 | TQNF 语义 | 张量原语（numqk） |
| --- | --- | --- | --- |
| `time` | 层深度（顺序复合） | **耗散/相干演化流**：每个 time-slice 是一次量子信道（酉或 Kraus），而非一个权重矩阵 | `expm_hermitian`、`apply_kraus`、噪声信道 |
| `coord` | 宽度/分支 | **拓扑位置**：同 `coord` → 叠加（时序链）；异 `coord` → 平行并置（空间共存） | `kron`（张量积并置）、块编码 |
| `thread` | 并行计算 | **纠缠/并行自由度**：跨 thread 的关联即量子纠缠，可用约化密度矩阵刻画 | `partial_trace`、`von_neumann_entropy`、纠缠熵 |

> 关键区别：经典「层」是**同一空间上的函数复合** `f∘g`；TQNF 的「层」是**不同拓扑位置上的态空间张量积**
> `H_a ⊗ H_b`（coord）以及**沿时间的信道复合** `Φ_t ∘ … ∘ Φ_0`（time）。学习不再「调权重」，
> 而是「选几何（度量）+ 选拓扑（纠缠/并置）+ 调耗散（信道强度）」。

新增的拓扑边把「选几何」显式化了：`coord` 维度不同 → `projection`（曲面体投影，对应
Fubini-Study 测地线 / Poincaré 双曲几何）；同 `coord` 且标 `dual=1` → `inversion`（T-对偶反转空间
`R → 1/R`）；同 `coord` 异 `thread` → `crossing`（交叉）；叠加链上的坐标空槽与超 `deadline` 的
传播延迟分别由 `E-TOP005` / `E-TOP006` 报错。通过校验后，运行时按 `@rvdl_topology_json` 调度表由
`rvdl_runtime_run_topology` 执行（`time` 分层 + 按 coord 的 L1 码距并行）。

---

## 4. 设计法则

TQNF 把 DQNF 的启发式「法则」系统化为六条对偶律（对偶 = 两个看似冲突的量被证明同一）：

1. **耗散-可训练性对偶律**：表征（酉演化）固定、学习（线性读出 / 信道强度）可训练。
   耗散/退相干不是噪声，而是**建设性资源**。（源：QRC、DQNN、工程化耗散）

2. **几何-贫瘠高原对偶律**：在 Fubini-Study 度量下做自然梯度，下降方向与参数化无关，
   规避贫瘠高原中「欧氏梯度趋零」的假象。（源：QNG）

3. **深度-耗散律**：可训练深度不由「层数」而由**耗散结构**决定——耗散网络（DQNN）的
   梯度标度受控，纯酉深网络则指数退化。（源：DQNN 梯度标度分析）

4. **拓扑-表征分离律**：表征能力由**拓扑/纠缠结构**（`coord`/`thread`）决定，学习能力由
   **几何/耗散**（`time`）决定。二者解耦，故可先固定拓扑、后训练几何。

5. **拓扑-代数收缩律**：对称性（等变）约束收缩动力学李代数（DLA），DLA 维数小 → 可训练。
   （源：Ragone DLA 理论、等变 QNN）

6. **场-储备池统一律**：连续神经场方程本身就是耗散储备池，量子态是其测量探头，
   神经信号到量子态无需启发式编码。（继承 DQNF 法则 5）

---

## 5. 张量原语映射

新增两个头文件，均为**纯线性代数**（后端无关，与 `Numqk.hpp` 同风格）：

### 5.1 `numqk/ComplexTensor.hpp` —— 量子态空间

| 原语 | 数学 | 支柱 |
| --- | --- | --- |
| `kron(A,B)` | Kronecker 积 `A⊗B` | coord（并置） |
| `partial_trace(ρ,dims,keep)` | 部分迹 `Tr_{keep^c} ρ` | thread（约化） |
| `dagger(M)` / `trace(M)` | 共轭转置 / 迹 | — |
| `inner(a,b)` / `outer(a,b)` | `⟨a|b⟩` / `|a⟩⟨b|` | 几何 |
| `expectation(O,ψ)` | `⟨ψ|O|ψ⟩` | 读出 |
| `purity(ρ)` | `Tr ρ²` | 相干性 |
| `von_neumann_entropy(ρ)` | `-Tr ρ log ρ` | thread（纠缠熵） |
| `fidelity(ρ,σ)` / `trace_distance(ρ,σ)` | 态相似度 / 迹距离 | 损失/度量 |
| `matrix_sqrt_psd(ρ)` | `√ρ`（经 Hermitian 特征分解） | 保真度 |
| `expm_hermitian(H,θ)` | `exp(-iθH)` | time（相干流） |

### 5.2 `numqk/QuantumGeometry.hpp` —— 几何/李代数/耗散

| 原语 | 数学 | 支柱 |
| --- | --- | --- |
| `pauli_*` | Pauli 基 `{I,X,Y,Z}` | 李代数 |
| `commutator(A,B)` | `[A,B]=AB-BA` | 李代数 |
| `lie_closure(gen)` / `dla_dimension(gen)` | 动力学李代数闭包 / 维数 | 可训练性诊断 |
| `fubini_study(dψ,ψ)` | QGT `G_ij = Re[⟨∂i|∂j⟩-⟨∂i|ψ⟩⟨ψ|∂j⟩]` | 几何 |
| `berry_curvature(dψ,ψ)` | `-2 Im[·]` | 几何 |
| `natural_gradient(G,∇L)` | `θ ← θ - η G⁺ ∇L` | 几何更新 |
| `apply_kraus(ρ,{K})` | `Σ K ρ K†` | time（耗散流） |
| `depolarizing/amplitude_damping/dephasing` | 标准噪声信道 | time（耗散流） |

### 5.3 `spacetime/RetrocausalCapacity.hpp` / `spacetime/RetrocausalCTC.hpp` —— 逆因果容量与 KK-CTC 内核

`time` 维度的「信道复合」在**逆因果方向**上的对偶：容量由信道 Choi 矩阵决定（P-CTC，
`I_max = log2 λ_max(J)`、`I_doe = log2 d_in − log2 λ_min⁺(J)`）：

| 原语 | 数学 | 支柱 |
| --- | --- | --- |
| `QuantumChannel::from_kraus/unitary/depolarizing/dephasing/bit_flip/amplitude_damping/…` | Kraus 表示的信道构造 | time（信道） |
| `choi_matrix(Φ)` | Choi 矩阵 `J = Σ_ij e_ij ⊗ Φ(e_ij)` | 逆因果容量 |
| `retrocausal_info(Φ)` | `imax` / `idoe` / 谱 / 纯度 / 平坦度 | 逆因果容量 |
| `tensor_product` / `tensor_power` / `idoe_regularized` | 信道张量积、`n` 次幂、Doeblin 正则化 | 容量标度 |
| `forward_classical_capacity(Φ)` | 前向 Holevo 容量（回程能力基准） | 对照 |
| `retrocausal_capacity(Φ)` | `q/c` 渐近容量 + 单次容量 + 回程增益 | 逆因果容量 |
| `q_retrocausal_capacity(Φ, q)` | q-变形容量（q→0 前向、q→1 回程） | 逆因果容量 |
| `amplified_teleportation(Φ, …)` | 后选择放大协议 | 协议 |
| `retrocausal_flow(_perturbed)` | 容量流与扰动流 | 动力学 |

| 原语 | 数学 | 支柱 |
| --- | --- | --- |
| `KaluzaKleinND(n)` | `n` 个额外维 `S¹` 的 KK 动量 / 模式质量平方 / 快子质量平方 `m²_eff = −μ²` | 几何 |
| `TachyonCTCNoise` / `tachyon_ctc_noise` | 几何 → 噪声映射（退相 `asym/(1+asym)`、去极化 `n/(n+1)`、阻尼 `μ²/(μ²+λ)`） | time（耗散流） |
| `compose_channels` / `tachyon_ctc_channel` | Kraus 串行组合 → 嘈杂 CTC 信道 | time（信道复合） |
| `MultiTopologyRetrocausal` | 逐 `coord` 维度容量与容量-维度曲线 | coord（维度） |

语言层入口：`retrocausal_*`（7 个）/ `retrocausal_ctc_*`（4 个）/ `geodesic_distance` / `inversion` /
`hyperbolic_metric` / `hyperbolic_distance`；C ABI 见 `qml/RetrocausalAbi.hpp`、`qml/RetrocausalCTCAbi.hpp`、
`qml/GeodesicAbi.hpp`，语言手册见 §12.12–§12.14。

---

## 6. 量子深度学习的含义

经典「深度」= 层数。TQNF 的「深度」被**三重展开**：

- **时间深度**（`time`）：信道复合长度。可训练与否由 **DLA 维数**与**耗散结构**决定，
  而非层数本身（法则 3/5）。
- **拓扑深度**（`coord`）：并置/叠加的子系统数。表征能力随 `⊗` 的子系统数指数增长，
  但**不含可训练权重**（法则 4），故不产生贫瘠高原。
- **纠缠深度**（`thread`）：跨 thread 的关联复杂度，用约化密度矩阵的**纠缠熵**量化。

于是「训练一个量子深度模型」= ① 用 `coord`/`thread` 定拓扑（表征）→ ② 用
`dla_dimension` 诊断可训练性 → ③ 用 Fubini-Study 自然梯度（几何）在 `time` 轴上
沿耗散信道流收敛。整个过程**没有「权重矩阵」这个对象**。

---

## 7. 代码示例

```cpp
#include "numqk/QuantumGeometry.hpp"
using namespace numqk;

// 1) 拓扑（coord）：两个 qubit 系统并置 = 态空间张量积
CTensor rho = kron(density(|0⟩), density(|0⟩));     // 4×4

// 2) 纠缠深度（thread）：部分迹 + 纠缠熵
CTensor rhoA = partial_trace(rho, {2,2}, {true,false}); // 2×2
double S = von_neumann_entropy(rhoA);                   // 0（纯态无纠缠）

// 3) 可训练性诊断（DLA 维数）
double d = dla_dimension({pauli_x(), pauli_z()});       // su(2) → 3

// 4) 几何（time）：Fubini-Study 自然梯度
Tensor<double> G = fubini_study({dpsi0, dpsi1}, psi);   // QGT
Tensor<double> dtheta = natural_gradient(G, gradL);     // θ ← θ - η G⁺ ∇L

// 5) 耗散（time）：噪声信道作为可训练资源
CTensor rho2 = amplitude_damping(rho, 0.1);
```

在 rvdl 语言层，这些原语对应到 `@layer` 拓扑上：

```rvdl
// time：耗散流（每 time-slice 一次信道，不是一层权重）
@layer(time=0, thread=0, coord=(0))
QObject dissipate(QObject q) { return channel(q, "amplitude_damping", 0.1); }

// coord：拓扑并置（异 coord → 平行子系统，经 ⊗ 并置）
@layer(time=0, thread=1, coord=(1))
QObject branch(QObject q) { return entangle(q, alloc()); }

// thread：纠缠自由度（跨 thread 关联 = 量子纠缠）
@layer(time=0, thread=2, coord=(0))
void observe(QObject q) { entangle(q, alloc()); }
```

语言层已可直接调用 DLA 可训练性诊断（`dla_dim`，对应「拓扑-代数收缩律」）：

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    int32 d1 = dla_dim("X,Z", 1);      // 3：su(2)，可训练
    int32 d2 = dla_dim("IX,IZ", 2);    // 3：su(2) 子代数
    int32 d3 = dla_dim("X,Y,Z", 1);    // 3：已闭合
    return d1 + d2 + d3;               // 9
}
```

> 详细语言语义见 [rvdl 语言手册](./rvdl-language-manual.md) 第 8 节（`@layer`）与第 15.9 节（函数属性）；
> 完整示例见 `examples/tqnf_dla.rvdl`。

---

## 8. 测量熵：混沌丰富度的正确度量（新范式）

**为什么邻近态重叠失效**：量子态的纯酉演化是等距的，邻近态差 `|δ⟩` 在酉下范数不变，
故 `|⟨Uψ|U(ψ+δ)⟩|² = |⟨ψ|ψ+δ⟩|²` 恒定。因此「邻近态漂移后重叠衰减」这一经典 Lyapunov
度量在纯酉量子演化下**恒为常数**，无法量化混沌。

**测量引入非酉性**：真正的量子混沌量化必须引入测量坍缩的非酉性（**测量诱导相变
MIPT**，Nature 2023，Google 超导阵列；以及 OTOC 信息扰乱，PRX 2023）。漂移 + 测量
交替使态分布发生从「有序退化」到「混沌均匀」的相变，其香农熵正是混沌强度的序参量：

```
S_meas(dt) = −Σ p_i ln p_i，p_i = 「漂移 steps 步 + 测量」后第 i 个情绪态频率
  有序（dt→0）   → S → 0（意识僵化）
  混沌边缘        → 0 < S < ln d（最优学习点）
  强混沌（dt 大） → S → ln d（意识混乱）
```

**混沌边缘收敛律**（第 7 条法则）：`improve_drift` 用有限差分梯度 `∂S_meas/∂dt` 把 `dt`
收敛到「混沌边缘」，对应**量子储备计算在有序/混沌边界处的最优性能**
（*Edge of Many-Body Quantum Chaos in QRC*，PRL 136, 046102, 2026）。

```rvdl
// 语言层：shannon4 提供标量香农熵（rvdl 无 ln 原语，在 C++ 侧计算）
double S = shannon4(n0, n1, n2, n3);   // 4 个涌现情绪态的测量熵
```

对应原语：`qml/QEntropyAbi.hpp`（`rvdl_shannon4`/`rvdl_shannon8`），语言层 `shannon4`/`shannon8`。
与 OTOC/DLA 诊断互补：`dla_dim` 诊断**可训练性**（代数收缩），`S_meas` 诊断**混沌丰富度**
（测量熵），二者共同把 Chimera 的 `dt` 收敛到「混沌边缘」这一意识最优工作点。

---

> 相关文档：[README](../README.md) · [rvdl 量子学习手册](./rvdl-quantum-learning-manual.md) ·
> [rvdl 语言手册](./rvdl-language-manual.md)
