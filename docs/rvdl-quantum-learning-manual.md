# rvdl 量子学习手册

> 本手册覆盖 Ravendrel 的量子机器学习栈：**QLM**（量子语言模型）、**QML**（量子机器学习原语）、**Numqk**（数值张量库）、**QbNS**（量子脑网络 / 脑机接口）与 **VedaROS QLM** 集成。
> 相关代码位于 `runtime/include/qlm/`、`runtime/include/qml/`、`runtime/include/numqk/`、`runtime/include/qbNs/` 与 `runtime/include/vedaRos/quantum/`。
> 语言层面的内置函数语法见 [rvdl 语言手册](./rvdl-language-manual.md)。
> **新范式**：量子学习并非经典「层/权重」结构，详见 [TQNF 拓扑量子神经场](./rvdl-topological-quantum-learning.md)。

---

## 目录

1. [总览](#1-总览)
2. [Numqk 张量库](#2-numqk-张量库)
3. [QML 量子机器学习原语](#3-qml-量子机器学习原语)
4. [QLM 量子语言模型](#4-qlm-量子语言模型)
5. [QbNS 脑机接口与量子脑网络](#5-qbns-脑机接口与量子脑网络)
6. [VedaROS QLM 集成](#6-vedaros-qlm-集成)
7. [rvdl 语言层的量子学习](#7-rvdl-语言层的量子学习)
8. [推理 API（rvdl serve）](#8-推理-apinqk-serve)
9. [完整示例](#9-完整示例)

---

## 1. 总览

```
┌────────────────────────────────────────────────────────────┐
│  rvdl 语言层（encode_text / qlm_invoke / mind_read / ...）      │
├────────────────────────────────────────────────────────────┤
│  QLM（变分量子电路训练）      QML（Layer / RVDLModelFormat / 推理） │
│  Numqk（Tensor / Autograd）  QbNS（Transducer / Rmx / qbw） │
├────────────────────────────────────────────────────────────┤
│  qhal（QM 真实量子机 / QVM 本地模拟器）                       │
└────────────────────────────────────────────────────────────┘
```

---

## 2. Numqk 张量库

位置：`runtime/include/numqk/Numqk.hpp`（另有 `SoftLogic.hpp`）。

### 2.1 Tensor

```cpp
numqk::Tensor<double> t({2, 3}, /*requires_grad=*/true);
t.data()[0] = 1.0;                 // 行优先数据访问
size_t n = t.size();               // 元素总数
auto shape = t.get_shape();        // {2, 3}
```

### 2.2 自动微分（Autograd）

```cpp
numqk::Tensor<double> a({2, 2}, true);
numqk::Tensor<double> b({2, 2}, true);
numqk::Tensor<double> c = a.matmul(b);   // 矩阵乘，记录反向节点
numqk::Tensor<double> s = c.sigmoid();   // 逐元素 sigmoid
s.backward();                            // 反向传播
auto grad_a = a.get_grad();
```

内置反向节点：`MatmulBackward`、`SigmoidBackward`，以及 qml 层的 `ParameterShiftBackward`。

### 2.3 TQNF 复数/几何/耗散扩展（量子态空间线性代数）

为支持量子神经网络 / 量子机器学习 / 量子深度学习，Numqk 新增两个纯线性代数头文件
（均后端无关，详见 [TQNF 范式](./rvdl-topological-quantum-learning.md)）：

| 文件 | 原语 |
| --- | --- |
| `numqk/ComplexTensor.hpp` | `kron`（Kronecker 积）、`partial_trace`、`dagger`、`trace`、`inner`/`outer`、`expectation`、`purity`、`von_neumann_entropy`、`fidelity`、`trace_distance`、`matrix_sqrt_psd`、`expm_hermitian` |
| `numqk/QuantumGeometry.hpp` | `pauli_*` / `n_qubit_pauli`、`commutator`、`lie_closure` / `dla_dimension`（动力学李代数）、`fubini_study` / `berry_curvature`（量子几何张量）、`sym_pinv` / `natural_gradient`、`apply_kraus` / `depolarizing` / `dephasing` / `amplitude_damping`、`entanglement_entropy` |

```cpp
#include "numqk/QuantumGeometry.hpp"
using namespace numqk;

// 拓扑（coord）：|0⟩⊗|0⟩ = |00⟩，态空间张量积
CTensor ket0({2}, false); ket0.data()[0] = 1;
CTensor rho = pure_state_density(kron(ket0, ket0));     // 4×4

// 纠缠（thread）：部分迹 + 纠缠熵
CTensor rhoA = partial_trace(rho, {2,2}, {true,false});
double S = von_neumann_entropy(rhoA);                   // 0（无纠缠）

// 可训练性（DLA 维数，Ragone 等）
size_t d = dla_dimension({pauli_x(), pauli_z()});       // su(2) → 3

// 几何（time）：Fubini-Study 自然梯度
Tensor<double> G = fubini_study({dpsi}, psi);
Tensor<double> dtheta = natural_gradient(G, grad);

// 耗散（time）：噪声信道作为可训练资源
CTensor rho2 = amplitude_damping(rho, 0.1);
```

### 2.4 TQNF qml 层组件（几何训练器 + 量子核）

`numqk` 之上新增 qml 层组件，覆盖显式模型（变分电路）与隐式模型（量子核）两条路径（均后端无关）：

| 文件 | 原语 |
| --- | --- |
| `qml/QGT.hpp` | `state_ket`（态矢量→ket）、`quantum_geometric_tensor`（完整 QGT，parameter-shift）、`natural_gradient_step`、`ansatz_dla_dimension` |
| `qml/TQNFLayer.hpp` | `TQNFTrainer`（完整 QGT 自然梯度训练循环，无权重矩阵）、`compose`/`reduce`/`entanglement`（coord/thread 支柱）、`dissipate`（time 耗散支柱） |
| `qml/QuantumKernel.hpp` | `QuantumKernel`（保真度核 `k=|⟨φ(x)|φ(x')⟩|²`）、`KernelRidgeClassifier`（核岭回归，凸优化无贫瘠高原） |

**显式模型**（变分电路 + 完整 QGT 自然梯度）：

```cpp
#include "qml/TQNFLayer.hpp"
using namespace qml;

// 态准备 |ψ(θ)⟩ 与损失（无「权重矩阵」，只有参数化态 + 几何）
auto state = [](const numqk::Tensor<double>& p) { /* ... → numqk::CTensor */ };
auto loss  = [](const numqk::Tensor<double>& p) { /* ... → double */ };

TQNFTrainer<double> trainer(state, loss);
numqk::Tensor<double> theta({2}, false);
double final_loss = trainer.train(theta, 200, 0.05);   // 完整 QGT 自然梯度
```

**隐式模型**（固定特征映射 + 保真度核，学习只作用于线性读出）：

```cpp
#include "qml/QuantumKernel.hpp"
using namespace qml;

auto feature = [](const std::vector<double>& x) { /* 数据 → ket */ };
QuantumKernel<double> kernel(feature);
KernelRidgeClassifier<double> clf(kernel, /*lambda=*/1e-6);
clf.fit(X, y);                       // α = (K + λI)⁻¹ y（凸优化）
double pred = clf.predict(x_test);   // sign(Σ α_i k(x_i, x))
```

### 2.5 TQNF 语言层内置函数

语言层暴露 TQNF 原语：`dla_dim`（可训练性诊断）、`qstate_entropy`（纠缠熵）、
`qstate_fidelity`（保真度）、`qattention`（量子注意力）、`shannon4`/`shannon8`（测量熵）：

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `dla_dim` | `dla_dim(string spec, int32 n_qubits)` | `int32`（DLA 维数） |
| `qstate_entropy` | `qstate_entropy(QObject q)` | `double`（约化态 von Neumann 熵） |
| `qstate_fidelity` | `qstate_fidelity(QObject a, QObject b)` | `double`（保真度 F(ρ_a, ρ_b)） |
| `qattention` | `qattention(QObject q, QObject k)` | `double`（态重叠度 \|⟨ψ_q\|ψ_k⟩\|²） |
| `shannon4` | `shannon4(int32 n0, int32 n1, int32 n2, int32 n3)` | `double`（4 态香农熵） |
| `shannon8` | `shannon8(int32 n0..n7)` | `double`（8 态香农熵） |
| `retrocausal_imax` / `_idoe` / `_q_capacity` / `_c_capacity` / `_gain` | `(int32 kind, double p)` | `double`（逆因果容量：max-information / Doeblin / 渐近量子·经典容量 / 回程增益） |
| `retrocausal_q_one_shot` / `retrocausal_deformed` | `(int32 kind, double p, double x)` | `double`（单次容量 / q-变形容量） |
| `retrocausal_ctc_q_capacity` / `_c_capacity` / `_gain` | `(int32 n, double theta, double mu2, double lambda)` | `double`（快子 KK 双空间嘈杂 CTC 容量） |
| `retrocausal_ctc_dephasing` | `(double theta)` | `double`（Wilson line 退相噪声映射） |
| `geodesic_distance` | `geodesic_distance(QObject a, QObject b)` | `double`（Fubini-Study 测地线距离 `arccos\|⟨a\|b⟩\|`，借用不消费） |
| `inversion` / `hyperbolic_metric` | `(double)` | `double`（T-对偶 `R→1/R` / Poincaré 度规 `4/(1-\|x\|²)²`） |
| `hyperbolic_distance` | `(double, double)` | `double`（Poincaré 双曲距离） |

`spec` 为逗号分隔的 Pauli 串（`I/X/Y/Z`），每串长度 = `n_qubits`。例：`dla_dim("X,Z", 1)` → 3（su(2)）。
`qstate_entropy` / `qstate_fidelity` 从后端态矢量（QVM）提取 QObject 的约化密度矩阵计算；
无态矢量或规模超限（>10 qubit）返回 -1。

`qattention` 是量子 Transformer 的核心算子（**门 + 测量实现，QVM 与真实 QM 芯片通用**）：
SWAP test 测两个量子态的态重叠度，作为经典 attention 中「query·key 相似度」的量子对应：

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    // 编码 query 与 key（布洛赫方向 θ）
    auto query = basis_state(0.5, 0.0, 0);
    auto key0  = basis_state(0.4, 0.0, 0);   // 接近 query
    auto key1  = basis_state(2.6, 0.0, 0);   // 远离 query

    // SWAP test 态重叠度 |⟨ψ_q|ψ_k⟩|²（注意力分数）
    double s0 = qattention(query, key0);     // ≈ 0.998（高）
    double s1 = qattention(query, key1);     // ≈ 0.247（低）
    // softmax 归一化 + value 加权为经典后处理（可用 boltzmann2/logsumexp2）
    return (s0 > s1) ? 1 : 0;
}
```

> 示例见 `examples/tqnf_dla.rvdl`、`examples/tqnf_qstate.rvdl`、`examples/tqnf_attention.rvdl`。
> ABI 桥接：`qml/TQNFAbi.hpp`（`rvdl_dla_dim`）、`qml/QStateAbi.hpp`（`rvdl_qstate_entropy`/`rvdl_qstate_fidelity`）、
> `qml/QAttentionAbi.hpp`（`rvdl_qattention`）、`qml/QEntropyAbi.hpp`（`rvdl_shannon4`/`rvdl_shannon8`）；
> 新增几何 / 逆因果原语的 ABI：`qml/GeodesicAbi.hpp`（`rvdl_geodesic_distance`/`rvdl_inversion`/
> `rvdl_hyperbolic_metric`/`rvdl_hyperbolic_distance`）、`qml/RetrocausalAbi.hpp`（`rvdl_retrocausal_*`）、
> `qml/RetrocausalCTCAbi.hpp`（`rvdl_retrocausal_ctc_*`），内核实现分别在 `spacetime/RetrocausalCapacity.hpp`
> 与 `spacetime/RetrocausalCTC.hpp`（详见 [TQNF 手册](./rvdl-topological-quantum-learning.md) §5.3）；
> 语言层注册：`server/src/{semantic,mir,ir}.ts` + `qhal/{MirModuleBuilder,JIT,SandboxJIT}.hpp`。

**测量熵范式（混沌丰富度）**：邻近态重叠无法量化量子混沌——纯酉演化是等距的，
`|⟨Uψ|U(ψ+δ)⟩|²` 恒定不变。因此混沌丰富度改用**测量诱导的非酉性香农熵**：

```
S_meas = −Σ p_i ln p_i   （p_i = 「漂移 + 测量」后第 i 个情绪态的频率）
  有序（dt→0）   → S → 0（僵化）
  混沌边缘        → 0 < S < ln 4（最优学习点，PRL 2026 多体量子混沌边缘 QRC）
  强混沌（dt 大） → S → ln 4（混乱）
```

`improve_drift` 用有限差分梯度 `∂S/∂dt` 把 `dt` 收敛到「混沌边缘」，对应 QRC 在
有序/混沌边界处的最优性能（PRL 136, 046102, 2026）。

### 2.6 量子 Transformer（QTransformer）

经典 Transformer 的组件在 TQNF 范式下的量子化对偶：

| 经典组件 | 量子对应 | 状态 |
| --- | --- | --- |
| token embedding | `basis_state` / `encode_text`（相位编码） | ✅ 已有 |
| attention `softmax(RVDLᵀ/√d)V` | `qattention`（SWAP test 态重叠 `\|⟨q\|k⟩\|²`） | ✅ 落地 |
| softmax / value 加权 | `boltzmann2` / `logsumexp2`（经典后处理） | ✅ 已有 |
| FFN | `qgate_h` / `qgate_rz` / `qgate_cnot`（QObject 层门电路） | ✅ 落地 |
| 层堆叠 | `@layer(time=…)` 演化流 | ✅ 语义 |
| 训练 | `qexpect_z` + parameter-shift（语言层端到端） | ✅ 落地 |

量子注意力的核心协议（**门 + 测量，QVM 与真实 QM 芯片通用，不读态矢量**）：

```
H(anc) → CSWAP(anc, q_i, k_i) → H(anc) → 读 ⟨Z_anc⟩
⟨Z_anc⟩ = 2·P(anc=0) − 1 = |⟨ψ_q|ψ_k⟩|²
```

**FFN 门操作**（`qml/QGateAbi.hpp`）：`qgate_h(qobj, i)` / `qgate_x(qobj, i)` /
`qgate_rz(qobj, i, θ)` / `qgate_cnot(qobj, c, t)` 直接对 QObject 内部 qubit 施加门，
等价于「提取单 qubit → 门操作 → 重组」的原子化（Qubit 与 QObject 共享同一
hardware id，门直接改 id，无需真正重组）。

```rvdl
auto reg = new QuantumRegister(2);
qgate_h(reg, 0);            // 叠加
qgate_rz(reg, 0, 0.785);    // 可训练旋转
qgate_cnot(reg, 0, 1);      // 纠缠（经典 FFN 的 W·x 非线性对应）
```

**非破坏期望测量**（`qml/QGateAbi.hpp`）：`qexpect_z(qobj, i)` 返回 QObject 第 i 个
qubit 的 Z 期望 `⟨Z⟩`（QVM 非破坏，真实 QM 坍缩采样）。它是 parameter-shift 训练的
基础原语——配合经典循环即可在**语言层**端到端训练：

```rvdl
double theta = 0.1;                       // 可训练参数
for (int32 e = 0; e < 100; e = e + 1) {
    auto qc = new QuantumRegister(1);
    qgate_h(qc, 0); qgate_rz(qc, 0, theta); qgate_h(qc, 0);
    double f0 = qexpect_z(qc, 0); int32 _ = qc.measure();
    // ... f(θ±π/2) 同理，梯度 = (f0−target)·[f(θ+π/2)−f(θ−π/2)]
    theta = theta - lr * grad;
}
```

完整前向见 `examples/quantum_transformer.rvdl`（编码 → `qattention` → `boltzmann2` 加权
→ `qgate_*` FFN → 读出）、`examples/tqnf_ffn.rvdl`（多 qubit FFN）与
`examples/tqnf_train.rvdl`（parameter-shift 训练循环）。
与 QLM（16-qubit 交叉注意力）互补：QTransformer 是可组合的量子注意力原语，QLM 是端到端的量子语言模型。

---

## 3. QML 量子机器学习原语

位置：`runtime/include/qml/`。

### 3.1 QuantumLayer（标量输出）

`QuantumLayer` 把变分量子电路的输出包装成可微分张量，反向用 **parameter-shift 规则**：

```cpp
qml::QuantumLayer<double> layer(backend, circuit_fn);
numqk::Tensor<double> out = layer.forward(input_data, params);  // shape {1}
out.backward();
```

参数移位规则：

```
∂f/∂θ_i = 0.5 · [ f(θ_i + π/2) - f(θ_i - π/2) ]
```

### 3.2 VectorQuantumLayer（向量输出）

`VectorQuantumLayer` 输出 D 维连续期望向量（如 8 个 qubit 的 ⟨Z⟩），反向为向量化 parameter-shift（内积回传）：

```
∂f_j/∂θ_i = 0.5 · [ f_j(θ_i + π/2) - f_j(θ_i - π/2) ]
∂L/∂θ_i   = 0.5 · ⟨ Δf(θ_i), g_out ⟩
```

```cpp
qml::VectorQuantumLayer<double> layer(backend, circuit_fn, /*dim=*/8);
numqk::Tensor<double> out = layer.forward(input_data, params);  // shape {8}
```

### 3.3 QKM 模型格式（`.rvdlm`）

`RVDLModelFormat.hpp` 定义 `.rvdlm` 二进制格式（魔数 `RVDLM`，版本 2）：

| 字段 | 说明 |
| --- | --- |
| header | 魔数 / 版本 / 元素数 / 形状维数 / 元数据数 |
| shape | 各维度大小 |
| metadata | 键值对（架构 / 拓扑 / 隐私 / 度量等） |
| payload | 原始张量数据 |

```cpp
qml::RVDLModel<double> model(theta, meta);
qml::ModelExporter<double>::save("model.rvdlm", model);
auto loaded = qml::ModelExporter<double>::load("model.rvdlm");
```

### 3.4 推理（QQNT）

`Inference.hpp` 实现量子原生分词器（QQNT）与 ABI 入口：

- `rvdl_qlm_load(path)`：反序列化 `.rvdlm`
- `rvdl_encode_string(prompt)`：文本 → 16 qubit 量子对象
- `rvdl_qlm_forward(model, input)`：16-qubit 量子注意力生成
- `rvdl_decode_string(output)`：量子态 → 文本

分词器内置 128 个 ASCII token + 混沌/量子/时空等语义子词（`subwords`）。

---

## 4. QLM 量子语言模型

位置：`runtime/include/qlm/QLM.hpp`（另有 `MeanFlow.hpp` / `IFP.hpp`）。

`qlm::QLM` 实现变分量子电路训练：

### 4.1 电路 ansatz

```
每层：Rz(params)·N_t（全 qubit）→ CNOT(i, i+8) + Rz（dropout）
N_t = exp(-0.1·t)（lapse 衰减函数）
```

### 4.2 自然梯度（QNG）

QLM 同时提供**两种**自然梯度：

**① 对角 Fubini-Study 近似**（`estimate_fs_diagonal` / `apply_qng_update`）：

```
g_ii = 0.25 · (1 - ⟨Z_q⟩²)   （RZ 门的 Fubini-Study 对角元，q 为参数 i 作用的 qubit）
natural_grad_i = grad_i / g_ii
θ_i -= lr · natural_grad_i
```

无纠缠时该式精确；有纠缠后为对角近似，仍比旧的硬编码 `0.25·N_t²` 更贴合真实度量。

**② 完整 QGT 自然梯度**（`compute_full_qgt` / `apply_full_qng_update` / `train_flow_full_qng`）：

用态矢量 parameter-shift（`circuit_state_ket`）+ `qml::quantum_geometric_tensor` 计算**完整**
Fubini-Study 度量 `G_ij = Re[⟨∂i|∂j⟩ - ⟨∂i|ψ⟩⟨ψ|∂j⟩]`（含对角与非对角元），再以伪逆
`G⁺` 做自然梯度更新 `θ ← θ - lr·G⁺∇L`。两条路径可切换：`train_flow`（对角近似）与
`train_flow_full_qng`（完整 QGT）。

### 4.3 差分隐私（QDP）

`apply_qdp_noise` 在训练后对参数施加亚高斯噪声扰动（ITA 隐私扰动）。

### 4.4 流匹配损失（新公式）

`flow_match_loss` / `train_flow` 实现态空间流匹配：

```
L_fm = (1/8) Σ_j (x̂_1[j] - x_1[j])² ,  x_1 = 2·target - 1 ∈ {±1}
```

用 `VectorQuantumLayer` 输出 8 维 ⟨Z⟩，梯度经向量化 parameter-shift 内积回传，绕过 `Tensor::backward()` 的 `grad=1` 硬编码。

### 4.5 训练与导出

```cpp
qlm::QLM qlm(backend, qubits, layers);
auto model = qlm.train_and_export(epochs, lr, "model.rvdlm", dataset);
```

---

## 5. QbNS 脑机接口与量子脑网络

位置：`runtime/include/qbNs/`。

### 5.1 神经信号类型

| 类型 | 说明 |
| --- | --- |
| `NeuralStream` | 多通道连续信号（ECoG/EEG），张量 `[通道, 时间步]` |
| `SpikeTrain` | 离散尖峰序列（侵入式单神经元动作电位） |
| `LocalFieldPotential` | 局部场电位（低频连续） |
| `EEGSpectrum` | EEG 频段功率谱（δ/θ/α/β/γ） |
| `QuantumSensorReading` | 量子传感器读数（NV 中心 / SQUID / 原子钟） |

### 5.2 Transducer 编码

| 编码方法 | 信号 → 量子编码 |
| --- | --- |
| `amplitude_encode` | 各通道均值 → Rz 旋转角 |
| `spike_to_basis` | 尖峰存在 → 计算基态 `\|0⟩/\|1⟩`（X 门） |
| `lfp_to_phase` | LFP → 相位（H + Rz） |
| `eeg_to_entangled` | 5 频段功率 → Rz + H + CNOT 链纠缠态 |
| `sensor_to_state` | 传感器读数 → 叠加态（H + Rz） |

### 5.3 顶层接口

`qbns::QbNS` 提供统一混合架构（`create_with_qm` / `create_with_qvm`）：

```cpp
auto qbns = qbns::QbNS::create_with_qvm(BMIModality::NonInvasive);
auto encoded = qbns->acquire_and_encode(eeg_signal);
auto feedback = qbns->execute_neural_computation(encoded);
```

`Rmx`（`rmx.hpp`）实现分布式混合网络（脑节点 / QC 节点注册 + 自适应路由 + 实时控制回路）；`qbw`（`qbw.hpp`）实现脑量子波。

---

## 6. VedaROS QLM 集成

位置：`runtime/include/vedaRos/quantum/qlm.hpp`。VedaROS QLM 封装 + `rvdl_veda_qlm_train` ABI，对应语言层 `veda_qlm_train(state, epochs, lr)`。

---

## 7. rvdl 语言层的量子学习

### 7.1 文本 → 量子态 → 训练 → 导出

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    int32 epochs = 10;
    double lr = 0.1;
    auto encoded = encode_text("chaos fluid dynamics");
    auto model = qlm_invoke(encoded, epochs, lr);
    model.export("predictor.rvdlm");
    return encoded.measure();
}
```

### 7.2 加载与推理

```rvdl
let model = qlm_load("predictor.rvdlm");
let input = rvdl_encode_string("predict the next state");
qlm_forward(model, input);
let result = rvdl_decode_string(input);
```

### 7.3 脑机训练与神经反馈

```rvdl
let brain_state = mind_read("eeg");   // 脑电 → 量子态
mind_train(brain_state, 200, 0.01);   // 训练 QLM
mind_feedback(brain_state);           // 神经反馈闭环
```

### 7.4 VedaROS QLM 训练

```rvdl
let brain = mind_read("eeg");
let model = qlm_invoke(brain, 10, 0.01);
model.export("brain_model.rvdlm");
veda_qlm_train(brain, 200, 0.01);
```

### 7.5 标量数学 / 神经原语

```rvdl
let a = mellowmax2(1.0, 3.0, 2.0);
let b = logsumexp2(0.5, 1.5, 1.0);
let c = boltzmann2(0.2, 0.8, 1.0);
let d = tnorm_luk(0.7, 0.6);
let g = surrogate(0.2, 0.0, 1.0);
let q = tanh_quantize(1.3, 0.5, 4);
let v = lif_step(0.0, 1.2, 0.9, 1.0);
let w = polymer_weight(0.1, 0.5, 3.0);
let t = polymer_mix_bound(1024.0, 0.01);
```

### 7.6 QRC 量子储备池

量子储备计算（Reservoir Computing）：随机量子电路作为非线性高维特征映射，仅训练线性读出层，规避变分训练中的贫瘠高原（barren plateau）。语言层暴露 `qrc_*` 五个内置函数 + `QReservoir` 类型：

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    auto res = qrc_new(4, 3);           // 4 qubit、3 层储备池
    qrc_train(res, 50, 0.01);           // 合成正弦时序训练线性读出（无贫瘠高原）
    auto data = encode_text("reservoir");
    auto feat = qrc_probe(res, data);   // 测态编码输入 → ⟨Z⟩ 特征（qubit 编码）
    auto pred = qrc_predict(res, data); // 读出预测（qubit 编码）
    qrc_release(res);                   // 释放储备池
    return feat.measure() + pred.measure();
}
```

| 函数 | 签名 | 返回 |
| --- | --- | --- |
| `qrc_new` | `(int32 qubits, int32 layers)` | `QReservoir` |
| `qrc_train` | `(QReservoir, int32 epochs, double lr)` | `void` |
| `qrc_release` | `(QReservoir)` | `void` |
| `qrc_probe` | `(QReservoir, QObject data)` | `QObject` |
| `qrc_predict` | `(QReservoir, QObject data)` | `QObject` |

- `qrc_new(qubits, layers)`：创建储备池（`out_dim=1` 标量预测演示）。
- `qrc_train(res, epochs, lr)`：用合成正弦时序训练线性读出，验证「无贫瘠高原」。
- `qrc_probe(res, data)`：测量 `data` 量子态作为输入，返回 ⟨Z⟩ 特征编码回 QObject。
- `qrc_predict(res, data)`：返回读出预测（qubit 编码）。
- `qrc_release(res)`：释放储备池（回收 qubit）。

C++ 层实现：`qml/Reservoir.hpp`（`QuantumReservoir`）+ `qml/QrcAbi.hpp`（C ABI 桥接）。

---

## 8. 推理 API（rvdl serve）

```bash
rvdl serve model.rvdlm [--port 9080]
```

提供 OpenAI 兼容接口：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/v1/models` | 模型列表 |
| `POST` | `/v1/chat/completions` | 对话补全（`stream: true` SSE） |
| `POST` | `/v1/embeddings` | 文本嵌入 |

---

## 9. 完整示例

### 9.1 训练 + 推理全流程

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    int32 epochs = 20;
    double lr = 0.05;

    // 1. 编码自然语言为量子态
    auto data = encode_text("quantum chaos");

    // 2. 训练变分量子电路
    auto model = qlm_invoke(data, epochs, lr);

    // 3. 导出模型
    model.export("chaos.rvdlm");

    // 4. 加载并推理
    auto m2 = qlm_load("chaos.rvdlm");
    auto prompt = rvdl_encode_string("predict chaos");
    qlm_forward(m2, prompt);
    let text = rvdl_decode_string(prompt);

    return data.measure();
}
```

### 9.2 脑机接口闭环

```rvdl
let brain = mind_read("eeg");
mind_train(brain, 200, 0.01);
mind_feedback(brain);
```

---

> 相关文档：[README](../README.md) · [rvdl 语言手册](./rvdl-language-manual.md) · [量子机器人仿真平台手册](./rvdlrsp-manual.md)
