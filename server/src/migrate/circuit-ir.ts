// ============================================================================
// circuit-ir.ts —— 统一量子电路中间表示（CircuitIR）
//
// 各量子编程语言前端（OpenQASM 2/3、Q#、Quil、Silq）把源程序解析并 lowering 到
// 本中间表示，再由 emitter.ts 统一发射为 .rvdl 源。
// 这样「多前端 + 单后端」，新增一门语言只需写一个前端 parser。
//
// 地址模型采用**全局平坦 qubit / bit 索引**（对齐 OpenQASM 的 qreg/creg 展开），
// 使各语言各异的寄存器 / 数组 / 单比特声明统一到同一张地址表。
// ============================================================================

/** 全局平坦 qubit 索引（0..qubitCount-1） */
export type QubitRef = number;

/** 全局平坦经典 bit 索引（0..bitCount-1） */
export type BitRef = number;

/**
 * 门参数：OpenQASM 的角度参数（如 rz(0.1)），MVP 阶段仅支持常量数值；
 * 保留 `'pi'`/`'-pi'` 之类的符号常量供后续参数化门展开使用。
 */
export type Param = number | string;

/**
 * 一条量子电路指令。
 *
 * - `gate`：内建门（或自定义门）作用于若干 qubit，带常量参数。
 * - `measure`：测第 `qubit` 个 qubit，结果写到第 `bit` 个经典 bit。
 * - `reset`：把 qubit 重置为 |0⟩（迁移为「测后条件翻转」）。
 * - `barrier`：电路屏障（仅语义提示，rvdl 侧忽略为注释）。
 * - `if`：经典条件分支（`bit == value` 时执行 body）。
 * - `for`：经典有界循环（`var in [lo, hi)`，步长 1）。
 */
/** 门指令（内建/自定义门作用于若干 qubit）。 */
export interface GateOp {
    kind: 'gate';
    name: string;
    qubits: QubitRef[];
    params: Param[];
}

/**
 * 经典条件分支：支持简单条件（`bit == value`）或复杂布尔条件（`condition` 字符串），
 * 以及可选的 `alternate`（else 分支）。
 */
export interface IfOp {
    kind: 'if';
    /** 简单条件：经典 bit 与常量比较 */
    bit?: BitRef;
    value?: number;
    /** 复杂条件：布尔表达式字符串（如 `c0 == 1 && x < 5`） */
    condition?: string;
    body: Op[];
    alternate?: Op[];
}

/** 经典变量（OpenQASM 3 `int[32]` / `bool` / `float`），迁移为 rvdl 标量局部变量。 */
export interface ClassicalVar {
    name: string;
    type: string; // 'int32' | 'double' | 'bool'
    init: string; // 初始值表达式字符串（如 '0'、'false'、'0.5'）
}

export type Op =
    | GateOp
    | { kind: 'measure'; qubit: QubitRef; bit: BitRef }
    | { kind: 'reset'; qubit: QubitRef }
    | { kind: 'barrier'; qubits: QubitRef[] }
    | IfOp
    | { kind: 'while'; condition: string; body: Op[] }
    | { kind: 'for'; varName: string; lo: number; hi: number; body: Op[] }
    | { kind: 'classicalAssign'; name: string; value: string };

/** 自定义门定义（OpenQASM `gate` / Q# `operation`），形参为 qubit 名与角度参数名。 */
export interface GateDef {
    name: string;
    /** 形式 qubit 参数名（顺序对应调用时的 qubits） */
    qubitParams: string[];
    /** 形式角度参数名（顺序对应调用时的 params） */
    angleParams: string[];
    body: Op[];
}

/**
 * 顶层电路：qubitCount / bitCount 为展开后的全局规模，ops 为有序指令流，
 * gateDefs 为自定义门表（emitter 会把它映射为 rvdl `@[gate]` 函数）。
 */
export interface CircuitIR {
    name: string;
    qubitCount: number;
    bitCount: number;
    /** 经典变量声明（OpenQASM 3 int/bool/float） */
    classicalVars: ClassicalVar[];
    ops: Op[];
    gateDefs: GateDef[];
}

/** 构造一个空的 CircuitIR */
export function emptyCircuit(name: string): CircuitIR {
    return { name, qubitCount: 0, bitCount: 0, classicalVars: [], ops: [], gateDefs: [] };
}

/** 便捷：构造一条内建门指令 */
export function gate(name: string, qubits: QubitRef[], params: Param[] = []): GateOp {
    return { kind: 'gate', name, qubits, params };
}