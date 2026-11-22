import { GateOp, gate } from './circuit-ir';

const PI = Math.PI;

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








export function lowerGate(name: string, qubits: number[], params: (number | string)[]): GateOp[] {
    const q0 = qubits[0];
    const q1 = qubits[1];

    if (DIRECT[name] !== undefined) {
        return [gate(DIRECT[name], qubits, params)];
    }

    if (name === 'sdg') return [gate('rz', [q0], [-PI / 2])];
    if (name === 'tdg') return [gate('rz', [q0], [-PI / 4])];

    if (name === 'u1') return [gate('rz', [q0], [params[0]])];
    if (name === 'u2') {
        const phi = params[0] ?? 0;
        const lambda = params[1] ?? 0;
        return [
            gate('rz', [q0], [lambda]),
            gate('ry', [q0], [PI / 2]),
            gate('rz', [q0], [phi]),
        ];
    }
    if (name === 'u3' || name === 'u') {
        const theta = params[0] ?? 0;
        const phi = params[1] ?? 0;
        const lambda = params[2] ?? 0;
        return [
            gate('rz', [q0], [lambda]),
            gate('ry', [q0], [theta]),
            gate('rz', [q0], [phi]),
        ];
    }

    if (name === 'cz') {
        return [gate('h', [q1]), gate('cx', [q0, q1]), gate('h', [q1])];
    }
    if (name === 'cy') {
        return [gate('rz', [q1], [PI / 2]), gate('cx', [q0, q1]), gate('rz', [q1], [-PI / 2])];
    }
    if (name === 'crx') {
        return [gate('h', [q1]), gate('crz', [q0, q1], [params[0]]), gate('h', [q1])];
    }
    if (name === 'cry') {
        return [
            gate('rz', [q1], [PI / 2]),
            gate('h', [q1]),
            gate('crz', [q0, q1], [params[0]]),
            gate('h', [q1]),
            gate('rz', [q1], [-PI / 2]),
        ];
    }
    if (name === 'cu1') {
        return [gate('crz', [q0, q1], [params[0]])];
    }

    if (name === 'id') return [];

    throw new Error(`[rvdl migrate] Unsupported gate '${name}'`);
}

export function isNativeGate(name: string): boolean {
    return name in DIRECT;
}


function basicizeGate(g: GateOp): GateOp[] {
    const q = g.qubits[0];
    switch (g.name) {
        case 'h':
        case 'x':
        case 'rz':
        case 'cnot':
            return [g];
        case 'z':
            return [gate('h', [q]), gate('x', [q]), gate('h', [q])];
        case 'y':
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
            throw new Error(`[rvdl migrate] Gate '${g.name}' cannot be lowered to QObject primitives (h/x/rz/cnot)`);
    }
}




export function lowerToBasic(name: string, qubits: number[], params: (number | string)[]): GateOp[] {
    const lowered = lowerGate(name, qubits, params);
    const out: GateOp[] = [];
    for (const g of lowered) out.push(...basicizeGate(g));
    return out;
}