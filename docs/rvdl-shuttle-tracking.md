# 穿梭与追踪：`@layer` 的调度机制

`shuttle`（穿梭）和 `track`（追踪）是 `@layer` 上的两组可选键，用来在拓扑块之间传数据、读可观测量。不写它们时，程序语义与以前完全一样（惰性）。

- **穿梭**：把一个块的数据"渗透"到其他 coord 的关联块，距离越远权重越小，不需要调用链一层层传。
- **追踪**：在块的功能周期内读可观测量并存进经典影子槽，不破坏量子态（软克隆）。

## 怎么用：三个例子

```rvdl
// 1. 穿梭：把本块数据渗透给 coord (1,0)
@layer(time=1, thread=0, coord=(0,0),
       shuttle=(1,0), shuttle_lambda=0.5, shuttle_phase=2, shuttle_strategy=-1)
int32 propagate(QObject q) {
    return qstate_fidelity(q, q);
}

// 2. 追踪：记录 expect_z / measure_x / entropy，保留 2 个影子槽
@layer(time=0, thread=0, coord=(0,0),
       track=(expect_z, measure_x, entropy), track_slots=2)
QObject sense(QObject q) {
    return qexpect_z(q, 0);
}

// 3. 组合：一边追踪一边穿梭
@layer(time=0, thread=0, coord=(0,0),
       track=(expect_z), track_slots=2,
       shuttle=(1,0), shuttle_lambda=1.0)
QObject relay(QObject q) { return q; }
```

## 键速查

全部是可选键，默认值保证"不写则不变"。

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `shuttle` | 无 | 目标 coord 列表，可多个（形成多个穿梭群） |
| `shuttle_lambda` | `1.0` | 关联度衰减长度（要求 >0），权重 \(w = \exp(-d_{L1}/\lambda)\)，无截断阈值 |
| `shuttle_phase` | `2` | `0` 仅共享前（目标入口）/ `1` 仅共享后（目标出口）/ `2` 前后都渗透 |
| `shuttle_rounds` | `0`（auto） | 渗透轮数，`0` 按策略解析式自动决定 |
| `shuttle_strategy` | `-1`（auto） | `-1` auto / `0` 蝶式 / `1` 环式 / `2` 树式 / `3` Grover 扩散 / `4` c001 蝶点 |
| `track` | 无 | 要追踪的可观测量（如 `expect_z, measure_x, entropy`） |
| `track_mode` | `0`（auto） | `0` auto / `1` qnd（L0，Pauli 族，精确）/ `2` shadow（L1，经典阴影）/ `3` relabel（L2，已是经典值） |
| `track_slots` | `1` | 影子槽位数（≥1） |

**硬约束**：影子槽必须是经典标量（`float` / `int32` / `double`），不能是 `QObject`，否则违反量子不可克隆（`E-Q001` / `E-Q002`）。

策略经验法则：`N≤2` 用蝶式；有量子载荷且 ancilla 充足用 Grover；长消息用环式；`span≥16 且 N≥4` 用 c001 蝶点；其余蝶式。

## 调度表

含 `@layer` 的模块会生成 `@rvdl_topology_json` 调度表，新增两段：

- `shuttleLinks`：每条含 `from`、`targets[{name, coord, logWeight, weight, distance, via}]`、`lambda`、`phase`、`rounds`、`strategy`、`slotsPerPhase`
- `trackingScopes`：每条含 `block`、`observables`、`mode`、`slots`、`level`

`logWeight = −d_L1/λ` 用于路由排序，`weight = exp(logWeight)` 用于线性混合；两者必须同时下发。原因：\(d/\lambda > 745\) 时线性权重会下溢成 0，但"数值零"不等于"语义截断"，路由要一直用 log 值。

## 诊断码

| 码 | 含义 |
| --- | --- |
| `E-SHU001` / `E-SHU002` | 目标 coord 无匹配块 / 存在但无可用关联对端 |
| `E-SHU003..006` | `lambda≤0` / `phase` 非法 / `strategy` 非法 / `rounds<0` |
| `E-SHU007` / `E-SHU008` | 规划轮次超 deadline / 对端与本块同 coord 同 time |
| `W-SHU001` | 声明了 `shuttle` 但没有 `track` → 退化为经典影子混合（警告，不阻断） |
| `E-TRK001..004` | track 空或 slots<1 / 作用域内无量子源 / 观测量未知 / mode 非法 |
| `W-TRK001` | `strategy=3` 但缺 ancilla → 降级为策略 0 |

示例：`examples/c001_dna_chain.rvdl`（双蝶点链 + 四叶子层 + 自检）。

基础标签语法见 [语言参考手册](rvdl-language-manual.md#函数与入口)；理论背景见 [TQNF 手册](rvdl-topological-quantum-learning.md)。