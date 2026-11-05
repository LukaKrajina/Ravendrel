// ============================================================================
// gates.ts —— 门归一与分解：OpenQASM 标准门库（qelib1.inc）→ rvdl 原生门
//
// rvdl 原生门（语言层）：h x y z s t rz rx ry cnot toffoli swap qft iqft braid
//                      cx ch crz cswap c_toffoli cqft cbraid
//                      measure measure_x measure_y
//
// OpenQASM 标准门库中「无 rvdl 原生对应」的门在此被分解为 rvdl 原生门序列，
// 保证迁移是损失无关的（到全局相位，物理可观测等价）。
// ============================================================================

import { GateOp, gate } from './circuit-ir';

const PI = Math.PI;

/** 直接重命名映射：OpenQASM 门名 → rvdl 原生门名（同构，仅改名）。 */
const DIRECT: Record<string, string> = {
    h: 'h',
    x: 'x',
    y: 'y',
    z: 'z',
    s: 's',
    t: 't',
    rx: 'rx',
    ry: 'ry',
    rz: 'rz',
    cx: 'cx',
    ch: 'ch',
    crz: 'crz',
    swap: 'swap',
    ccx: 'toffoli',
    cswap: 'cswap',
};

/**
 * 把一条门指令归一为 rvdl 原生门序列。
 *
 * - 直接映射的门返回单元素数组（重命名）。
 * - 需分解的门返回多门序列。
 * - 无操作的门（id/barrier）返回空数组。
 * - 未知门抛错（由调用方决定降级策略）。
 */
export function lowerGate(name: string, qubits: number[], params: (number | string)[]): GateOp[] {
    const q0 = qubits[0];
    const q1 = qubits[1];

    // 直接映射
    if (DIRECT[name] !== undefined) {
        return [gate(DIRECT[name], qubits, params)];
    }

    // 单比特相位门（S† / T† 无 rvdl 原生，展开为 rz 取负角）
    if (name === 'sdg') return [gate('rz', [q0], [-PI / 2])];
    if (name === 'tdg') return [gate('rz', [q0], [-PI / 4])];

    // 通用旋转 u1/u2/u3（OpenQASM qelib1 标准分解）
    if (name === 'u1') return [gate('rz', [q0], [params[0]])];
    if (name === 'u2') {
        // u2(φ, λ) = u3(π/2, φ, λ) = rz(λ)·ry(π/2)·rz(φ)
        const phi = params[0] ?? 0;
        const lambda = params[1] ?? 0;
        return [
            gate('rz', [q0], [lambda]),
            gate('ry', [q0], [PI / 2]),
            gate('rz', [q0], [phi]),
        ];
    }
    if (name === 'u3' || name === 'u') {
        // u3(θ, φ, λ) = rz(φ)·ry(θ)·rz(λ)（电路顺序：rz(λ) → ry(θ) → rz(φ)）
        const theta = params[0] ?? 0;
        const phi = params[1] ?? 0;
        const lambda = params[2] ?? 0;
        return [
            gate('rz', [q0], [lambda]),
            gate('ry', [q0], [theta]),
            gate('rz', [q0], [phi]),
        ];
    }

    // 受控门（无 rvdl 原生受控 Y / Z / Rx / Ry 版本，用基础门分解）
    if (name === 'cz') {
        // CZ = H(t)·CX·H(t)
        return [gate('h', [q1]), gate('cx', [q0, q1]), gate('h', [q1])];
    }
    if (name === 'cy') {
        // CY = S(t)·CX·S†(t)（到全局相位）
        return [gate('rz', [q1], [PI / 2]), gate('cx', [q0, q1]), gate('rz', [q1], [-PI / 2])];
    }
    if (name === 'crx') {
        // CRX(θ) = H(t)·CRZ(θ)·H(t)
        return [gate('h', [q1]), gate('crz', [q0, q1], [params[0]]), gate('h', [q1])];
    }
    if (name === 'cry') {
        // CRY(θ) = S(t)·CRX(θ)·S†(t)
        return [
            gate('rz', [q1], [PI / 2]),
            gate('h', [q1]),
            gate('crz', [q0, q1], [params[0]]),
            gate('h', [q1]),
            gate('rz', [q1], [-PI / 2]),
        ];
    }
    if (name === 'cu1') {
        // CU1(λ) = CRZ(λ) 到全局相位
        return [gate('crz', [q0, q1], [params[0]])];
    }

    // 无操作门
    if (name === 'id') return [];

    // 未知门：交给调用方处理（保守抛出，便于前端给出清晰报错）
    throw new Error(`[rvdl migrate] Unsupported gate '${name}'`);
}

/** 判断一个门名是否属于 rvdl 原生门（供 emitter 决定直接发射还是走 lowerGate）。 */
export function isNativeGate(name: string): boolean {
    return name in DIRECT;
}

// ============================================================================
// QObject 层降级：完整门集 → 基础四门（h / x / rz / cnot）
//
// 当电路含「中间 reset」（测后复用 qubit）时，Qubit 层（消费式 measure）无法表达
// 「测后条件翻转」，需改用 QObject 层：QuantumRegister + qgate_h/x/rz/cnot +
// qmeasure（部分坍缩）+ 条件翻转。qgate_* 仅有 h/x/rz/cnot 四门，故此处把
// 其余门分解为这四门（到全局相位，物理可观测等价）。
// ============================================================================

/** 把单个「完整门集」门分解为 h/x/rz/cnot 四门序列（QObject 层可用）。 */
function basicizeGate(g: GateOp): GateOp[] {
    const q = g.qubits[0];
    switch (g.name) {
        case 'h':
        case 'x':
        case 'rz':
        case 'cnot':
            return [g]; // 原生四门
        case 'z':
            return [gate('h', [q]), gate('x', [q]), gate('h', [q])];
        case 'y':
            // Y = X·Z（到全局相位），Z = H·X·H
            return [gate('x', [q]), gate('h', [q]), gate('x', [q]), gate('h', [q])];
        case 's':
            return [gate('rz', [q], [PI / 2])];
        case 't':
            return [gate('rz', [q], [PI / 4])];
        case 'rx':
            return [gate('h', [q]), gate('rz', [q], [g.params[0]]), gate('h', [q])];
        case 'ry':
            return [
                gate('rz', [q], [-PI / 2]),
                gate('h', [q]),
                gate('rz', [q], [g.params[0]]),
                gate('h', [q]),
                gate('rz', [q], [PI / 2]),
            ];
        case 'cx':
            return [gate('cnot', g.qubits)];
        case 'swap': {
            const a = g.qubits[0];
            const b = g.qubits[1];
            return [gate('cnot', [a, b]), gate('cnot', [b, a]), gate('cnot', [a, b])];
        }
        case 'cz': {
            const c = g.qubits[0];
            const t = g.qubits[1];
            return [gate('h', [t]), gate('cnot', [c, t]), gate('h', [t])];
        }
        case 'cy': {
            const c = g.qubits[0];
            const t = g.qubits[1];
            return [gate('rz', [t], [PI / 2]), gate('cnot', [c, t]), gate('rz', [t], [-PI / 2])];
        }
        case 'crz': {
            const c = g.qubits[0];
            const t = g.qubits[1];
            const th = g.params[0];
            return [
                gate('rz', [t], [th]),
                gate('cnot', [c, t]),
                gate('rz', [t], [typeof th === 'number' ? -th : th]),
                gate('cnot', [c, t]),
            ];
        }
        default:
            // toffoli/ch/cswap 等无 ancilla 分解的门：QObject 层降级为不可支持
            throw new Error(`[rvdl migrate] Gate '${g.name}' cannot be lowered to QObject primitives (h/x/rz/cnot)`);
    }
}

/**
 * 把一条门指令分解为 QObject 层基础四门（h/x/rz/cnot）序列。
 * 先走 lowerGate 归一（u1/u2/u3/sdg/tdg 等），再 basicize 到四门。
 */
export function lowerToBasic(name: string, qubits: number[], params: (number | string)[]): GateOp[] {
    const lowered = lowerGate(name, qubits, params);
    const out: GateOp[] = [];
    for (const g of lowered) out.push(...basicizeGate(g));
    return out;
}