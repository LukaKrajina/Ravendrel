# TQNF 拓扑量子神经场入门

TQNF 把 `@layer(time, thread, coord)` 三个标签当作神经场的「构造块」：用 **coord** 定拓扑位置、用 **thread** 定纠缠并行度、用 **time** 走耗散演化流。它没有传统神经网络的"权重矩阵"——参数本身就是几何对象，训练沿流形做自然梯度。

> 一句话区别：经典层是同空间函数复合 \(f \circ g\)；TQNF 层是不同拓扑位置的张量积 \(H_a \otimes H_b\)（coord）+ 沿时间的信道复合 \(\Phi_t \circ \cdots \circ \Phi_0\)（time）。

## 三维标签怎么用

| `@layer` 维度 | 在 TQNF 里表示 | 对应 C++ 原语 |
| --- | --- | --- |
| `time` | 相干 / 耗散演化流（每 time-slice 一次量子信道，不是权重矩阵） | `expm_hermitian`、`apply_kraus`、噪声信道 |
| `coord` | 拓扑位置（同 coord → 叠加时序链，异 coord → 平行并置） | `kron`、块编码 |
| `thread` | 纠缠 / 并行自由度（跨 thread 关联 = 纠缠） | `partial_trace`、`von_neumann_entropy` |

```rvdl
@layer(time=0, thread=0, coord=(0))
QObject dissipate(QObject q) {
    return channel(q, "amplitude_damping", 0.1);   // time 维：耗散信道
}

@layer(time=0, thread=1, coord=(1))
QObject branch(QObject q) {
    return entangle(q, alloc());                     // thread 维：纠缠扩展
}

@layer(time=0, thread=2, coord=(0))
QObject observe(QObject q) {
    entangle(q, alloc());                            // 同 coord 异 thread → crossing 边
    return q;
}
```

拓扑边（由编译器自动推导）：`projection`（coord 维度不同，走曲面体投影）、`inversion`（同 coord 且 `dual=1`，T-对偶 \(R \to 1/R\)）、`crossing`（同 coord 异 thread）。诊断：`E-TOP005`（坐标空槽）、`E-TOP006`（超 deadline）。

## 可训练性诊断：`dla_dim`

动力学李代数维数越高，越不容易出现贫瘠高原。`spec` 是逗号分隔的 Pauli 串，每串长度 = qubit 数。

```rvdl
int32 d1 = dla_dim("X,Z", 1);    // = 3
int32 d2 = dla_dim("IX,IZ", 2);  // = 3
int32 d3 = dla_dim("X,Y,Z", 1);  // = 3
```

示例：`examples/tqnf_dla.rvdl`、`examples/tqnf_qstate.rvdl`、`examples/tqnf_attention.rvdl`。

C++ 侧：`numqk/QuantumGeometry.hpp` 的 `lie_closure({pauli_x(), pauli_z()})`、`dla_dimension(...)`。

## 张量原语速查

| 头文件 | 代表函数 | 用途 |
| --- | --- | --- |
| `numqk/ComplexTensor.hpp` | `kron` / `partial_trace` / `von_neumann_entropy` / `fidelity` / `expm_hermitian` | 态空间线性代数：并置、约化、纠缠熵、保真度、相干流 |
| `numqk/QuantumGeometry.hpp` | `commutator` / `dla_dimension` / `fubini_study` / `natural_gradient` / `apply_kraus` | 几何度量、可训练性、自然梯度、噪声信道 |
| `spacetime/RetrocausalCapacity.hpp` | `QuantumChannel::from_kraus` / `choi_matrix` / `retrocausal_capacity` | 逆因果容量内核 |
| `spacetime/RetrocausalCTC.hpp` | `KaluzaKleinND(n)` / `tachyon_ctc_channel` / `MultiTopologyRetrocausal` | 快子 KK 额外维 CTC 噪声 |

典型 C++ 流程（从态构造到自然梯度）：

```cpp
auto rho  = kron(pure_state_density(ket0), pure_state_density(ket0));
auto rhoA = partial_trace(rho, {2,2}, {true,false});
double S  = von_neumann_entropy(rhoA);                  // = 0（直积态无纠缠）
int64_t d = dla_dimension({pauli_x(), pauli_z()});      // = 3
auto G = fubini_study({dpsi0, dpsi1}, psi);             // 量子几何张量
auto step = natural_gradient(G, gradL);                 // 沿测地线更新
auto noisy = amplitude_damping(rho, 0.1);               // 耗散
```

## 测量熵：混沌丰富度

纯酉演化是等距的，邻近态重叠恒定，所以用「漂移 + 测量」的非酉香农熵衡量混沌丰富度：有序 \(S \to 0\)，混沌边缘 \(0 < S < \ln 4\)（最优学习点），强混沌 \(S \to \ln 4\)。

```rvdl
double S = shannon4(n0, n1, n2, n3);   // 4 个涌现态的测量熵
```

`improve_drift` 用 \(\partial S / \partial t\) 把系统收敛到混沌边缘。

## 延伸

- 基础语法与 `@layer` 全键见 [语言参考手册](rvdl-language-manual.md)
- `shuttle` / `track` 的拓扑调度机制见 [穿梭与追踪](rvdl-shuttle-tracking.md)
- 完整训练 / 推理流程见 [量子学习手册](rvdl-quantum-learning-manual.md)