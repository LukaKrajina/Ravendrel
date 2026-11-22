# BAT 批量伴随训练

BAT（Batched Amplitude-Tile Adjoint）是 `train_cli` 里加速 QLM 稠密态训练的一组开关：把多个样本打包成一个 minibatch 共享同一次门演化，减少 kernel launch 和同步次数。

> 一句话：B 个样本一起算，门 launch 数从 `O(B·G)` 降到 `O(G)`；代价是显存从 `2·2ⁿ` 涨到 `2·B·2ⁿ`。

## 怎么用

```bash
train_cli corpus.txt model.rvdlm 1 0.02 4 \
    --device auto --batch-size 32 --tile-bits 8
# 可选：--tile-epsilon 0.02 启用误差有界裁剪并报告上界
```

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `--batch-size N` | `1` | 每批样本数 B；`1` 为逐样本精确基线 |
| `--tile-bits N` | `0` | tile 局部归约位数 t，把振幅分成 `2^t` 个 tile |
| `--tile-epsilon F` | `0` | 误差有界裁剪阈值；`0` = 精确，不跳过任何 tile |

跑自检：

```bash
cmake --build runtime/build --target quantum_accel_test
ctest --test-dir runtime/build -R quantum_accel_test --output-on-failure
```

## 三条机制

| 机制 | 通俗解释 |
| --- | --- |
| **SUB A** 共享酉批量 | B 个态排成 `[B][2ⁿ]` 二维 View，一个门一次作用于全部 B 个态 → launch 数 `O(B·G)→O(G)`，fence `O(B)→2` |
| **TLR** tile 局部归约 | 按低 t 位把振幅分 `2^t` 个 tile，寄存器内累加再原子加 → 原子争用 `O(B·2ⁿ)→O(B·2^t)` |
| **EBAT** 误差有界裁剪 | 跳过 L2 质量 `< ε²` 的 tile 对**梯度归约**的贡献（门演化仍全量），并报告误差上界 |

## 语义与约束

- minibatch：B 个样本共享一次 optimizer step，梯度按 `1/B` 平均（缩放早于 `weight_decay` / `grad_clip`）。
- **MoE 与 BAT 互斥**：每样本换专家则无法共享酉；同时指定 `--moe-experts N>1` 和 `--batch-size N>1` 时，`train_cli` 自动把 batch 降为 1 并打印原因。
- 误差上界：跳过质量 δ 时，梯度误差 `≤ 4(2√δ+δ)`，写入 `train.tile_error_bound`；`ε=0` 逐位精确。
- 模型元数据记录 `train.batch_size` / `train.batched_adjoint` / `train.tile_*`。

## 实践建议

| 建议 | 原因 |
| --- | --- |
| `B·2^t ≳ 1024` 才划算 | 线程太少反而更慢（实测 `B=8,t=4` 仅 128 线程 → 0.65×） |
| 显存要够 | 批量工作集是 `2·B·2ⁿ` 复数 |
| B 越大等效步数越少 | `best_loss` 可能上升，可增大 `lr` / `epochs` 补偿 |
| 想精确对照用 `B=1` 或 `ε=0` | 与逐样本训练逐位一致（误差 ~1e-16） |

实测（16 qubit / 4 layers / 512 样本）：`B=1` 7.96s → `B=32,t=8` 2.53s，约 **3.15×**；梯度误差与逐样本 4 次平均在 1e-16 量级。

实现文件：`runtime/include/qlm/BatchAdjoint.hpp`、`runtime/include/qlm/QLM.hpp`、`runtime/tools/train_cli.cpp`。更多训练参数见 [量子学习手册](rvdl-quantum-learning-manual.md#train_cli-参数速查)。