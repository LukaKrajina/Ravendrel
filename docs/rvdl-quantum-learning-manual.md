# rvdl 量子学习手册

用 rvdl 完成「文本 → 量子态 → 训练变分电路 → 导出 `.rvdlm` → 加载推理」的完整流程，以及调用 Numqk / QML / QLM / QbNS / VedaROS 各层原语。

分三层：**rvdl 语言层**（内置函数，最常用）→ **C++ 层**（Numqk / QML / QLM 库）→ **qhal**（QM 真机 / QVM 模拟器）。

## 目录

- [语言层：训练与推理](#语言层训练与推理)
- [语言层：量子注意力与 TQNF](#语言层量子注意力与-tqnf)
- [语言层：脑机接口与储备池](#语言层脑机接口与储备池)
- [C++ 层：张量与自动微分](#c-层张量与自动微分)
- [C++ 层：模型与训练器](#c-层模型与训练器)
- [推理服务](#推理服务)
- [train_cli 参数速查](#train_cli-参数速查)

---

## 语言层：训练与推理

### 文本 → 训练 → 导出 → 加载 → 推理

```rvdl
// 1. 训练并导出
let encoded = encode_text("quantum chaos");
let model = qlm_invoke(encoded, 20, 0.05);   // (输入, epochs, lr) -> QModel
model.export("chaos.rvdlm");
encoded.measure();

// 2. 加载并推理
QModel m = qlm_load("chaos.rvdlm");
QObject input = rvdl_encode_string("what is quantum?");
qlm_forward(m, input);
string answer = rvdl_decode_string(input);
```

对应内置函数：

| 函数 | 含义 |
| --- | --- |
| `encode_text(string)` / `encode_image(string)` | 文本 / 图像 → 量子态 |
| `qlm_invoke(QObject, epochs, lr) -> QModel` | 训练变分电路，返回模型（`.export(path)` 导出） |
| `qlm_load(string) -> QModel` | 加载 `.rvdlm` |
| `qlm_forward(QModel, QObject)` | 前向推理 |
| `rvdl_encode_string` / `rvdl_decode_string` | 字符串 ↔ 量子态（16 qubit，128 ASCII token + 语义子词） |

### 量子 Transformer 的 FFN 门

```rvdl
auto reg = new QuantumRegister(2);
qgate_h(reg, 0);
qgate_rz(reg, 0, 0.785);
qgate_cnot(reg, 0, 1);
double z = qexpect_z(reg, 0);   // 非破坏读出
```

parameter-shift 训练循环（梯度 `∂f/∂θ = 0.5[f(θ+π/2) − f(θ−π/2)]`）：

```rvdl
double theta = 0.3, lr = 0.01;
for (int32 e = 0; e < 100; e = e + 1) {
    auto qc = new QuantumRegister(1);
    qgate_h(qc, 0); qgate_rz(qc, 0, theta); qgate_h(qc, 0);
    double f0 = qexpect_z(qc, 0);
    double gp = qexpect_z(qc_theta_plus, 0);   // θ + π/2
    double gm = qexpect_z(qc_theta_minus, 0);  // θ − π/2
    theta = theta - lr * 0.5 * (gp - gm);
}
```

示例文件：`examples/quantum_transformer.rvdl`、`examples/tqnf_ffn.rvdl`、`examples/tqnf_train.rvdl`。

---

## 语言层：量子注意力与 TQNF

### 量子注意力（SWAP test）

用 `basis_state(theta, phi, value)` 按布洛赫方向编码 query / key，再用 `qattention` 读重叠度作为注意力分数：

```rvdl
QObject query = basis_state(0.5, 0, 0);
QObject key0  = basis_state(0.5, 0, 0);
QObject key1  = basis_state(2.0, 0, 0);
double s0 = qattention(query, key0);   // ≈ 0.998
double s1 = qattention(query, key1);   // ≈ 0.247
// softmax 用 boltzmann2 / logsumexp2 做后处理
```

协议是「H(anc) → CSWAP → H(anc) → ⟨Z_anc⟩」，真机与 QVM 通用，不读态矢量。

### 可训练性 / 几何诊断

| 函数 | 含义 |
| --- | --- |
| `dla_dim("X,Z", n)` | 动力学李代数维数；维数越高越不易贫瘠高原（例：`dla_dim("X,Z",1)=3`） |
| `qstate_entropy(q)` | 约化态 von Neumann 熵（纠缠度量） |
| `qstate_fidelity(a,b)` | 保真度 |
| `shannon4(n0,n1,n2,n3)` / `shannon8(...)` | 测量香农熵（混沌丰富度：有序→0，混沌边缘→适中，强混沌→ln4） |
| `geodesic_distance(a,b)` | Fubini-Study 测地距离（借用不消费） |
| `inversion(R)` / `hyperbolic_metric(x)` / `hyperbolic_distance(x,y)` | T-对偶反转 / Poincaré 度规与距离 |

---

## 语言层：脑机接口与储备池

### 脑机接口（QbNS）

```rvdl
let brain = mind_read("eeg");          // 模态：stream/spike/lfp/eeg/sensor
mind_train(brain, 200, 0.01);
mind_feedback(brain);                  // 闭合神经反馈环
veda_qlm_train(brain, 200, 0.01);      // VedaROS QLM 训练
```

### QRC 量子储备池

随机量子电路做非线性映射，只训练线性读出，规避贫瘠高原。

```rvdl
QReservoir res = qrc_new(4, 3);            // (qubits, layers)
qrc_train(res, 50, 0.01);                  // 合成正弦时序自检
QObject feats = qrc_probe(res, encode_text("reservoir"));
QObject pred  = qrc_predict(res, feats);
qrc_release(res);

// 真实数据（多维输出）
QReservoir r2 = qrc_new(6, 3, 4);          // out_dim=4
qrc_train_ex(r2, epochs, lr, inputs, targets, n_samples, n_features, n_outputs);
```

---

## C++ 层：张量与自动微分

头文件位于 `runtime/include/`。

### Numqk 张量（`numqk/Numqk.hpp`）

```cpp
#include "numqk/Numqk.hpp"
numqk::Tensor<double> a({2,3}, true), b({3,2}, true);
a.data()[0] = 1.0;                 // 行优先
a.size(); a.get_shape();           // 元素数 / 形状
auto c = a.matmul(b);              // 矩阵乘，记录反向节点
auto s = c.sigmoid();
s.backward();                      // 反向传播
auto grad = a.get_grad();          // 取梯度
```

内置反向节点：`MatmulBackward`、`SigmoidBackward`、`ParameterShiftBackward`。

### 量子态线性代数（`numqk/ComplexTensor.hpp`）

`kron(A,B)`（Kronecker 积）、`partial_trace(ρ, dims, keep)`（部分迹）、`dagger`、`trace`、`inner`、`outer`、`expectation`、`purity`、`von_neumann_entropy`、`fidelity`、`trace_distance`、`matrix_sqrt_psd`、`expm_hermitian`（相干流）、`pure_state_density`。

### 几何 / 李代数 / 耗散（`numqk/QuantumGeometry.hpp`）

`commutator(A,B)`、`lie_closure` / `dla_dimension`（可训练性诊断）、`fubini_study` / `berry_curvature`（量子几何张量）、`sym_pinv` / `natural_gradient`（自然梯度）、`apply_kraus`、`depolarizing` / `dephasing` / `amplitude_damping`、`entanglement_entropy`。

---

## C++ 层：模型与训练器

| 类 | 头文件 | 用法 |
| --- | --- | --- |
| `QuantumLayer<T>` | `qml/` | `layer.forward(input, params)`，标量输出，反向用 parameter-shift |
| `VectorQuantumLayer<T>` | `qml/` | 向量输出（如 8 个 ⟨Z⟩），向量化 parameter-shift |
| `QuantumKernel<T>` + `KernelRidgeClassifier<T>` | `qml/QuantumKernel.hpp` | `clf.fit(X,y); clf.predict(x)`，保真度核 + 核岭回归，无贫瘠高原 |
| `TQNFTrainer<T>` | `qml/TQNFLayer.hpp` | `trainer.train(theta, 200, 0.05)`，无权重矩阵的 QGT 自然梯度 |
| `RVDLModel<T>` / `ModelExporter<T>` | `qml/RVDLModelFormat.hpp` | `.rvdlm` 读写（魔数 `RVDLM`，版本 2） |
| `QLM` | `qlm/QLM.hpp` | `QLM(backend, qubits, layers); qlm.train_and_export(epochs, lr, "model.rvdlm", dataset)` |

推理（`qml/Inference.hpp`）：`rvdl_qlm_load(path)` → `rvdl_encode_string(prompt)` → `rvdl_qlm_forward(model, input)` → `rvdl_decode_string(output)`。

QbNS（`runtime/include/qbNs/`）：`NeuralStream` / `SpikeTrain` / `LocalFieldPotential` / `EEGSpectrum`（δ/θ/α/β/γ）/ `QuantumSensorReading`；`QbNS::create_with_qvm()` → `acquire_and_encode(eeg_signal)` → `execute_neural_computation(encoded)`。

---

## 推理服务

```bash
rvdl serve model.rvdlm [--port 9080]
```

OpenAI 兼容接口：`GET /v1/models`、`POST /v1/chat/completions`（支持 `stream:true` SSE）、`POST /v1/embeddings`。

---

## train_cli 参数速查

（构建产物在 `runtime/build/`，源码 `runtime/tools/train_cli.cpp`）

```
train_cli <train.txt> <out.rvdlm> [epochs] [lr] [layers] [options]
```

| 类别 | 参数 |
| --- | --- |
| 基础 | `--epochs N` `--lr X` `--layers N` `--qubits N\|auto`（默认 16，范围 8..20）`--optimizer adam\|sgd` `--gradient auto\|adjoint\|param-shift` `--device auto\|cpu\|gpu` |
| 续训 | `--continue old.rvdlm` `--checkpoint path` `--checkpoint-every N` |
| 数据 | `--images` `--image-root dir` `--limit N` `--max-samples N` `--shuffle` `--seed N` |
| MoE | `--moe-experts N` `--moe-active-expert N` |
| 批量加速 | `--batch-size N`（默认 1）`--tile-bits N`（默认 0）`--tile-epsilon F`（默认 0=精确，见 [BAT 手册](quantum-batching-theory.md)） |
| 稀疏 / 低秩 | `--sparse-epsilon F` `--sparse-tile-bits N` `--lowrank-epsilon F` `--lowrank-rank N` |
| 训练控制 | `--patience N` `--min-delta X` `--grad-clip X`（默认 10）`--weight-decay X` |
| 运行 | `--dry-run` `--json-log file.jsonl` `--run-id id` `--quiet` `-h` |

---

概念背景见 [TQNF 手册](rvdl-topological-quantum-learning.md)；语言内置函数全表见 [语言参考手册](rvdl-language-manual.md)。