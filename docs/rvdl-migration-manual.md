# rvdl 迁移手册

把现有量子编程源码（OpenQASM、Q#、Quil、Silq）转成 `.rvdl`。迁移是**纯文本转译**，不连接 daemon，结果回显到 Ravendrel Console。

## 支持的源语言

| 源语言 | `--from` 值 | 覆盖子集 |
| --- | --- | --- |
| OpenQASM 2.0 | `openqasm2` | `qreg`/`creg`、内建门、参数化门、`measure`、`barrier`、`reset`、`if`、自定义 `gate` |
| OpenQASM 3.0 | `openqasm3` | `qubit[N]`/`bit[N]`、`c = measure q`、`for` 展开、`if`/`while`、经典变量、自定义 `gate` |
| Q# | `qsharp` | `namespace`/`operation`、`use`/`using`、`H`/`CNOT`/`Rx`、`M`、`Reset` |
| Quil | `quil` | `DECLARE`、`H`/`CNOT`/`RX`、`MEASURE` |
| Silq | `silq` | `def`、`q := 0:𝔹`、`H(q)`/`rotZ(θ,q)`、`measure(q)` |

## 使用方法

```bash
rvdl migrate circuit.qasm --from openqasm2              # 显式源语言，默认生成 circuit.rvdl
rvdl migrate circuit.qasm --from openqasm3 -o out.rvdl  # 指定输出
rvdl migrate program.qs --from qsharp
rvdl migrate program.quil --from quil
rvdl migrate program.slq --from silq
rvdl migrate circuit.qasm                               # 按内容自动探测
```

迁移后：

```bash
rvdl run circuit.rvdl              # 运行
rvdl ir circuit.rvdl               # 输出 LLVM IR
rvdl compile x64 -e circuit.rvdl   # AOT 编译为原生二进制
```

VS Code 中右键 `.qasm` / `.qs` / `.quil` / `.slq` → **Migrate to rvdl**，同目录生成 `.rvdl`。

## 门映射表

| 源门 | rvdl 结果 | 方式 |
| --- | --- | --- |
| `h` `x` `y` `z` `s` `t` | 同名 | 原生 |
| `rx(θ)` `ry(θ)` `rz(θ)` | `rx(q,θ)` 等 | 原生 |
| `cx` `ccx` `swap` `ch` `crz` `cswap` | `cx` `toffoli` `swap` `ch` `crz` `cswap` | 原生 |
| `sdg` / `tdg` | `rz(q,-π/2)` / `rz(q,-π/4)` | 分解 |
| `u1(λ)` | `rz(q,λ)` | 分解 |
| `u2(φ,λ)` | `rz(q,λ)·ry(q,π/2)·rz(q,φ)` | 分解 |
| `u3(θ,φ,λ)` / `u` | `rz(q,λ)·ry(q,θ)·rz(q,φ)` | 分解 |
| `cz` | `h(t)·cx(c,t)·h(t)` | 分解 |
| `cy` | `rz(t,π/2)·cx(c,t)·rz(t,-π/2)` | 分解 |
| `crx(θ)` | `h(t)·crz(c,t,θ)·h(t)` | 分解 |
| `cry(θ)` | `rz(t,π/2)·crx(θ)·rz(t,-π/2)` | 分解 |
| `id` / `barrier` | 忽略 / 保留为注释 | — |
| `measure` | `measure(q)` | 原生 |
| `reset` | 注释（rvdl 函数末尾隐式释放） | 降级 |

## 迁移示例

**OpenQASM 2.0 → rvdl（Bell 态）**

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    Qubit q0 = alloc();
    Qubit q1 = alloc();
    h(q0);
    cx(q0, q1);
    int32 c0 = measure(q0);
    int32 c1 = measure(q1);
    return 0;
}
```

**OpenQASM 3.0 自定义门 + 循环**：`gate my_rot(angle a) q0 { rz(a) q0; }` → `@[gate] void my_rot(Qubit q0, double a) { rz(q0, a); }`；`for i in [0:3]` 展开为 3 次调用。

其他语言（Q# / Quil / Silq）迁移后都是同样的 `alloc → h/cx → measure` 结构；Q# 的 `Reset` 降级为注释。

## 已支持的进阶语义

| 场景 | 支持方式 |
| --- | --- |
| `reset` 后条件翻转 | 自动切到 QObject：`QuantumRegister` + `qgate_*` + `qmeasure` + 条件 `qgate_x` |
| 经典控制流 | `int[32]→int32`、`bool→int32`、`float[64]→double`；赋值 / `while` / 布尔 `if` |
| OpenQASM 3 for 步长 | `[lo:step:hi]`，含负步长 `[3:-1:0]` |
| Q# 多 operation | 全部迁移，helper 内联；递归报错 |
| Q# 常量 | `One/Zero→1/0`，`and/or/not→&&/||/!` |

## 已知限制

| 限制 | 行为 |
| --- | --- |
| OpenQASM 3 `array[...]` 经典数组 | 未支持，降级 / 忽略 |
| Q# `M` 后 `Reset` 再复用 qubit | `Reset` 降级为注释；典型测后释放不受影响 |
| 符号角度在顶层 | 仅常量角度，`pi` 运行时无此常量 |
| 复杂算术表达式 | 仅简单算术与比较，其余原样透传 |
| Q# 条件里的函数调用（如 `Length(qs)`） | 明确报错，不产出无效 rvdl |

语言本身用法见 [rvdl 语言参考手册](rvdl-language-manual.md)。