export type QubitRef = number;

export type BitRef = number;




export type Param = number | string;










export interface GateOp {
    kind: 'gate';
    name: string;
    qubits: QubitRef[];
    params: Param[];
}




export interface IfOp {
    kind: 'if';
    bit?: BitRef;
    value?: number;
    condition?: string;
    body: Op[];
    alternate?: Op[];
}

export interface ClassicalVar {
    name: string;
    type: string;
    init: string;
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

export interface GateDef {
    name: string;
    qubitParams: string[];
    angleParams: string[];
    body: Op[];
}




export interface CircuitIR {
    name: string;
    qubitCount: number;
    bitCount: number;
    classicalVars: ClassicalVar[];
    ops: Op[];
    gateDefs: GateDef[];
}

export function emptyCircuit(name: string): CircuitIR {
    return { name, qubitCount: 0, bitCount: 0, classicalVars: [], ops: [], gateDefs: [] };
}

export function gate(name: string, qubits: QubitRef[], params: Param[] = []): GateOp {
    return { kind: 'gate', name, qubits, params };
}