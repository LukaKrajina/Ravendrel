// ============================================================================
// emitter.ts —— CircuitIR → .rvdl 源发射器
//
// 把统一电路中间表示发射为可读、可编辑、可编译的 rvdl 源文件，分两种模式：
//
//   1. Qubit 层（默认，无「中间 reset」）：
//      全局 qubit → 逐个 `Qubit q<i> = alloc();`
//      门 → `h(q0)` / `rz(q0, θ)` / `cx(q0, q1)` ...（Qubit 层门，门集完整）
//      测量 → `c<i> = measure(q<j>);`
//
//   2. QObject 层（含「中间 reset」，即测后复用 qubit）：
//      Qubit 层消费式 measure 无法表达「测后条件翻转」，故改用 QObject：
//      全局 qubit → `auto q = new QuantumRegister(N);`
//      门 → `qgate_h(q, i)` / `qgate_rz(q, i, θ)` / `qgate_cnot(q, c, t)` ...
//      测量 → `c<i> = qmeasure(q, j);`（部分坍缩）
//      reset → `int32 _r = qmeasure(q, j); if (_r == 1) { qgate_x(q, j); }`
// ============================================================================

import { CircuitIR, GateDef, Op, IfOp } from './circuit-ir';
import { lowerGate, lowerToBasic } from './gates';

const INDENT = '    ';

/** 数字参数格式化：保留 8 位小数，去掉末尾 0（π/2 等显示为 1.57079633） */
function fmtParam(p: number | string): string {
    if (typeof p === 'string') return p;
    return String(Math.round(p * 1e8) / 1e8);
}

/** 顶层 qubit 引用名：q<i>（Qubit 层） */
function topQubit(i: number): string {
    return `q${i}`;
}

/** 顶层 bit 引用名：c<i> */
function topBit(i: number): string {
    return `c${i}`;
}

/**
 * 检测电路是否含任何 `reset` 指令（含嵌套 if/while/for body）。
 * 只要有 reset 就切换到 QObject 层：Qubit 层消费式 measure 无法表达
 * 「测后条件翻转」，QObject 层（qmeasure + qgate_x）则语义正确。
 */
function hasReset(ir: CircuitIR): boolean {
    const walk = (ops: Op[]): boolean => {
        for (const op of ops) {
            if (op.kind === 'reset') return true;
            if (op.kind === 'if' || op.kind === 'for' || op.kind === 'while') {
                if (walk(op.body)) return true;
                if (op.kind === 'if' && op.alternate && walk(op.alternate)) return true;
            }
        }
        return false;
    };
    return walk(ir.ops);
}

// ─── Qubit 层发射 ───────────────────────────────────────────────────────────

function emitGateLine(name: string, qubits: number[], params: (number | string)[], qn: (i: number) => string): string {
    const args = qubits.map(qn).join(', ');
    if (params.length > 0) {
        return `${name}(${args}, ${fmtParam(params[0])});`;
    }
    return `${name}(${args});`;
}

function emitQubitOp(
    op: Op,
    qn: (i: number) => string,
    bn: (i: number) => string,
    depth: number,
    customGates: Set<string>,
): string[] {
    const pad = INDENT.repeat(depth);
    switch (op.kind) {
        case 'gate': {
            if (customGates.has(op.name)) {
                const args = [...op.qubits.map(qn), ...op.params.map(fmtParam)].join(', ');
                return [`${pad}${op.name}(${args});`];
            }
            const lowered = lowerGate(op.name, op.qubits, op.params);
            return lowered.map(g => pad + emitGateLine(g.name, g.qubits, g.params, qn));
        }
        case 'measure':
            return [`${pad}${bn(op.bit)} = measure(${qn(op.qubit)});`];
        case 'reset':
            return [`${pad}// reset ${qn(op.qubit)}（rvdl 末尾隐式释放）`];
        case 'barrier':
            return [`${pad}// barrier`];
        case 'if': {
            const out: string[] = [];
            out.push(`${pad}if (${ifCondition(op, bn)}) {`);
            for (const inner of op.body) out.push(...emitQubitOp(inner, qn, bn, depth + 1, customGates));
            if (op.alternate && op.alternate.length > 0) {
                out.push(`${pad}} else {`);
                for (const inner of op.alternate) out.push(...emitQubitOp(inner, qn, bn, depth + 1, customGates));
            }
            out.push(`${pad}}`);
            return out;
        }
        case 'while': {
            const out: string[] = [];
            out.push(`${pad}while (${op.condition}) {`);
            for (const inner of op.body) out.push(...emitQubitOp(inner, qn, bn, depth + 1, customGates));
            out.push(`${pad}}`);
            return out;
        }
        case 'for': {
            const out: string[] = [];
            out.push(`${pad}for (int32 ${op.varName} = ${op.lo}; ${op.varName} < ${op.hi}; ${op.varName} = ${op.varName} + 1) {`);
            for (const inner of op.body) out.push(...emitQubitOp(inner, qn, bn, depth + 1, customGates));
            out.push(`${pad}}`);
            return out;
        }
        case 'classicalAssign':
            return [`${pad}${op.name} = ${op.value};`];
        default:
            return [];
    }
}

/** 解析 if 条件：简单（bit == value）或复杂布尔表达式。 */
function ifCondition(op: IfOp, bn: (i: number) => string): string {
    if (op.condition !== undefined) return op.condition;
    return `${bn(op.bit!)} == ${op.value}`;
}

/** 发射经典变量声明为 rvdl 标量局部变量。 */
function emitClassicalVars(ir: CircuitIR): string[] {
    const lines: string[] = [];
    for (const v of ir.classicalVars) {
        lines.push(`${INDENT}${v.type} ${v.name} = ${v.init};`);
    }
    return lines;
}

function emitGateDef(def: GateDef, customGates: Set<string>): string {
    const params: string[] = [];
    def.qubitParams.forEach(p => params.push(`Qubit ${p}`));
    def.angleParams.forEach(p => params.push(`double ${p}`));
    const sig = params.join(', ');

    const qn = (i: number) => def.qubitParams[i];
    const bn = (_i: number) => `__bit_${_i}`;

    const body: string[] = [];
    for (const op of def.body) {
        body.push(...emitQubitOp(op, qn, bn, 1, customGates));
    }

    return ['@[gate]', `void ${def.name}(${sig}) {`, ...body, '}'].join('\n');
}

function emitQubitCircuit(ir: CircuitIR, customGates: Set<string>): string {
    const lines: string[] = [];

    lines.push(`// 迁移自 ${ir.name}（由 rvdl migrate 生成）`);
    lines.push('');

    for (const def of ir.gateDefs) {
        lines.push(emitGateDef(def, customGates));
        lines.push('');
    }

    lines.push('@layer(time=0, thread=0, coord=(0))');
    lines.push('int32 rvdl_main() {');

    for (let i = 0; i < ir.qubitCount; i++) {
        lines.push(`${INDENT}Qubit ${topQubit(i)} = alloc();`);
    }
    if (ir.qubitCount > 0 && ir.bitCount > 0) lines.push('');

    for (let i = 0; i < ir.bitCount; i++) {
        lines.push(`${INDENT}int32 ${topBit(i)} = 0;`);
    }
    lines.push(...emitClassicalVars(ir));
    if (ir.bitCount > 0 || ir.classicalVars.length > 0) lines.push('');

    for (const op of ir.ops) {
        lines.push(...emitQubitOp(op, topQubit, topBit, 1, customGates));
    }

    lines.push(`${INDENT}return 0;`);
    lines.push('}');
    return lines.join('\n') + '\n';
}

// ─── QObject 层发射（含中间 reset）─────────────────────────────────────────

/** QObject 层门名：lowerToBasic 输出 h/x/rz/cnot → qgate_<name> */
const QGATE_MAP: Record<string, string> = {
    h: 'qgate_h',
    x: 'qgate_x',
    rz: 'qgate_rz',
    cnot: 'qgate_cnot',
};

function emitQObjectOp(
    op: Op,
    bn: (i: number) => string,
    depth: number,
    resetCounter: { n: number },
): string[] {
    const pad = INDENT.repeat(depth);
    switch (op.kind) {
        case 'gate': {
            const lowered = lowerToBasic(op.name, op.qubits, op.params);
            return lowered.map(g => {
                const qg = QGATE_MAP[g.name];
                const args = g.qubits.join(', ');
                if (g.name === 'rz') {
                    return `${pad}${qg}(q, ${args}, ${fmtParam(g.params[0])});`;
                }
                return `${pad}${qg}(q, ${args});`;
            });
        }
        case 'measure':
            return [`${pad}${bn(op.bit)} = qmeasure(q, ${op.qubit});`];
        case 'reset': {
            // 测后条件翻转：测量 qubit，若为 1 则 X 翻转回 |0⟩（部分坍缩语义）
            const tmp = `_r${resetCounter.n++}`;
            return [
                `${pad}int32 ${tmp} = qmeasure(q, ${op.qubit});`,
                `${pad}if (${tmp} == 1) {`,
                `${pad}${INDENT}qgate_x(q, ${op.qubit});`,
                `${pad}}`,
            ];
        }
        case 'barrier':
            return [`${pad}// barrier`];
        case 'if': {
            const out: string[] = [];
            out.push(`${pad}if (${ifCondition(op, bn)}) {`);
            for (const inner of op.body) out.push(...emitQObjectOp(inner, bn, depth + 1, resetCounter));
            if (op.alternate && op.alternate.length > 0) {
                out.push(`${pad}} else {`);
                for (const inner of op.alternate) out.push(...emitQObjectOp(inner, bn, depth + 1, resetCounter));
            }
            out.push(`${pad}}`);
            return out;
        }
        case 'while': {
            const out: string[] = [];
            out.push(`${pad}while (${op.condition}) {`);
            for (const inner of op.body) out.push(...emitQObjectOp(inner, bn, depth + 1, resetCounter));
            out.push(`${pad}}`);
            return out;
        }
        case 'for': {
            const out: string[] = [];
            out.push(`${pad}for (int32 ${op.varName} = ${op.lo}; ${op.varName} < ${op.hi}; ${op.varName} = ${op.varName} + 1) {`);
            for (const inner of op.body) out.push(...emitQObjectOp(inner, bn, depth + 1, resetCounter));
            out.push(`${pad}}`);
            return out;
        }
        case 'classicalAssign':
            return [`${pad}${op.name} = ${op.value};`];
        default:
            return [];
    }
}

function emitQObjectCircuit(ir: CircuitIR): string {
    const lines: string[] = [];

    lines.push(`// 迁移自 ${ir.name}（由 rvdl migrate 生成）`);
    lines.push('');

    lines.push('@layer(time=0, thread=0, coord=(0))');
    lines.push('int32 rvdl_main() {');

    if (ir.qubitCount > 0) {
        lines.push(`${INDENT}auto q = new QuantumRegister(${ir.qubitCount});`);
    }

    for (let i = 0; i < ir.bitCount; i++) {
        lines.push(`${INDENT}int32 ${topBit(i)} = 0;`);
    }
    lines.push(...emitClassicalVars(ir));
    if (ir.bitCount > 0 || ir.qubitCount > 0 || ir.classicalVars.length > 0) lines.push('');

    const resetCounter = { n: 0 };
    for (const op of ir.ops) {
        lines.push(...emitQObjectOp(op, topBit, 1, resetCounter));
    }

    lines.push(`${INDENT}return 0;`);
    lines.push('}');
    return lines.join('\n') + '\n';
}

/**
 * 把 CircuitIR 发射为完整 .rvdl 源文件。
 * 自动选择 Qubit 层（默认）或 QObject 层（含中间 reset 时）。
 */
export function emitCircuit(ir: CircuitIR): string {
    const customGates = new Set(ir.gateDefs.map(d => d.name));
    if (hasReset(ir)) {
        if (ir.gateDefs.length > 0) {
            // QObject 层暂不支持自定义门（需提取单 qubit 传 Qubit 形参）
            throw new Error('[rvdl migrate] Circuits with reset + custom gate definitions are not yet supported');
        }
        return emitQObjectCircuit(ir);
    }
    return emitQubitCircuit(ir, customGates);
}