import { Program, Statement, Expression, FunctionDeclaration, FunctionCall, Param } from './ast';

const INTRINSIC_GATES = new Set(['h', 'x', 'y', 'z', 'rz', 'rx', 'ry', 's', 't',
    'cnot', 'toffoli', 'swap', 'qft', 'braid']);

const SELF_INVERSE = new Set(['h', 'x', 'y', 'z', 'cnot', 'toffoli', 'swap', 'braid']);

const STEER_MAP: Record<string, string> = {
    x: 'cx',
    h: 'ch',
    rz: 'crz',
    cnot: 'toffoli',
    swap: 'cswap',
    toffoli: 'c_toffoli',
    qft: 'cqft',
    braid: 'cbraid',
};

function makeExprStmt(fn: FunctionDeclaration, expr: Expression): Statement {
    return {
        type: 'ExpressionStatement',
        expression: expr,
        line: fn.line,
        column: fn.column,
        length: fn.name.length,
    };
}

function makeFn(
    name: string,
    src: FunctionDeclaration,
    body: Statement[],
    extraParams: Param[],
    returnType: string,
): FunctionDeclaration {
    return {
        type: 'FunctionDeclaration',
        returnType,
        name,
        params: [...extraParams, ...src.params],
        receiver: null,
        isPub: false,
        isExport: false,
        requires: [],
        ensures: [],
        body,
        quantum: { isGate: true, undo: false, steer: false, unitary: false, measure: false },
        synthetic: true,
        line: src.line,
        column: src.column,
        length: src.name.length,
    };
}




function extractGateSequence(body: Statement[], userGates: Set<string>): FunctionCall[] {
    const gates: FunctionCall[] = [];
    for (const stmt of body) {
        if (stmt.type === 'ExpressionStatement') {
            const e = stmt.expression;
            if (e.type === 'FunctionCall' && (INTRINSIC_GATES.has(e.name) || userGates.has(e.name))) {
                gates.push(e);
            }
        }
    }
    return gates;
}

const CONTROL_FLOW = new Set(['IfStatement', 'WhileStatement', 'ForStatement', 'SpinStatement']);

function hasControlFlow(body: Statement[]): boolean {
    return body.some(s => CONTROL_FLOW.has(s.type));
}

function daggerGate(g: FunctionCall, userGates: Set<string>): FunctionCall {
    if (g.name === 'rz' || g.name === 'rx' || g.name === 'ry') {
        const negTheta: Expression = {
            type: 'UnaryExpression',
            operator: '-',
            argument: g.arguments[1],
            line: g.line,
            column: g.column,
            length: 1,
        };
        return { ...g, arguments: [g.arguments[0], negTheta] };
    }
    if (g.name === 's' || g.name === 't') {
        const angle = g.name === 's' ? Math.PI / 2 : Math.PI / 4;
        const negTheta: Expression = {
            type: 'NumberLiteral',
            value: -angle,
            isFloat: true,
            line: g.line,
            column: g.column,
            length: 1,
        };
        return { ...g, name: 'rz', arguments: [g.arguments[0], negTheta] };
    }
    if (g.name === 'qft') {
        return { ...g, name: 'iqft' };
    }
    if (userGates.has(g.name)) {
        return { ...g, name: g.name + '_undo' };
    }
    if (SELF_INVERSE.has(g.name)) {
        return g;
    }
    return g;
}

function makeCall(src: { line: number; column: number }, name: string, args: Expression[]): FunctionCall {
    return { type: 'FunctionCall', name, arguments: args, line: src.line, column: src.column, length: name.length };
}

function makeNum(src: { line: number; column: number }, value: number): Expression {
    return { type: 'NumberLiteral', value, isFloat: true, line: src.line, column: src.column, length: 1 };
}

type SteerDecomp = (ctrl: Expression, g: FunctionCall) => FunctionCall[];

const STEER_DECOMPOSITIONS: Record<string, SteerDecomp> = {
    z: (ctrl, g) => {
        const q = g.arguments[0];
        return [makeCall(g, 'h', [q]), makeCall(g, 'cnot', [ctrl, q]), makeCall(g, 'h', [q])];
    },
    y: (ctrl, g) => {
        const q = g.arguments[0];
        return [
            makeCall(g, 'crz', [ctrl, q, makeNum(g, -Math.PI / 2)]),
            makeCall(g, 'cnot', [ctrl, q]),
            makeCall(g, 'crz', [ctrl, q, makeNum(g, Math.PI / 2)]),
        ];
    },
    s: (ctrl, g) => [makeCall(g, 'crz', [ctrl, g.arguments[0], makeNum(g, Math.PI / 2)])],
    t: (ctrl, g) => [makeCall(g, 'crz', [ctrl, g.arguments[0], makeNum(g, Math.PI / 4)])],
    rx: (ctrl, g) => {
        const q = g.arguments[0];
        return [makeCall(g, 'h', [q]), makeCall(g, 'crz', [ctrl, q, g.arguments[1]]), makeCall(g, 'h', [q])];
    },
    ry: (ctrl, g) => {
        const q = g.arguments[0];
        return [
            makeCall(g, 'rz', [q, makeNum(g, -Math.PI / 2)]),
            makeCall(g, 'h', [q]),
            makeCall(g, 'crz', [ctrl, q, g.arguments[1]]),
            makeCall(g, 'h', [q]),
            makeCall(g, 'rz', [q, makeNum(g, Math.PI / 2)]),
        ];
    },
};

function steerGate(g: FunctionCall, userGates: Set<string>, fnName: string, warnings: string[]): FunctionCall[] {
    const ctrl: Expression = {
        type: 'Identifier',
        name: '__ctrl',
        line: g.line,
        column: g.column,
        length: 1,
    };
    if (userGates.has(g.name)) {
        return [{ ...g, name: g.name + '_steer', arguments: [ctrl, ...g.arguments] }];
    }
    const decomp = STEER_DECOMPOSITIONS[g.name];
    if (decomp) {
        return decomp(ctrl, g);
    }
    const mapped = STEER_MAP[g.name];
    if (mapped) {
        return [{ ...g, name: mapped, arguments: [ctrl, ...g.arguments] }];
    }
    warnings.push(`[E-QSTEER] @[steer] on '${fnName}': gate '${g.name}' has no controlled decomposition`);
    return [g];
}

function synthUndo(fn: FunctionDeclaration, userGates: Set<string>): FunctionDeclaration {
    const gates = extractGateSequence(fn.body, userGates);
    const body = gates
        .reverse()
        .map(g => daggerGate(g, userGates))
        .map(g => makeExprStmt(fn, g));
    return makeFn(fn.name + '_undo', fn, body, [], fn.returnType);
}

function synthSteer(fn: FunctionDeclaration, userGates: Set<string>, warnings: string[]): FunctionDeclaration {
    const gates = extractGateSequence(fn.body, userGates);
    const body: Statement[] = [];
    for (const g of gates) {
        for (const steered of steerGate(g, userGates, fn.name, warnings)) {
            body.push(makeExprStmt(fn, steered));
        }
    }
    const ctrlParam: Param = { name: '__ctrl', type: 'Qubit' };
    return makeFn(fn.name + '_steer', fn, body, [ctrlParam], fn.returnType);
}





export function synthesizeGates(program: Program): string[] {
    const warnings: string[] = [];
    const userGates = new Set<string>();
    for (const n of program.body) {
        if (n.type === 'FunctionDeclaration' && n.quantum?.isGate) {
            userGates.add(n.name);
        }
    }

    const newFns: FunctionDeclaration[] = [];
    for (const n of program.body) {
        if (n.type !== 'FunctionDeclaration') continue;
        const fn = n as FunctionDeclaration;
        if ((fn.quantum?.undo || fn.quantum?.steer) && hasControlFlow(fn.body)) {
            const tag = fn.quantum?.undo ? '@[undo]' : '@[steer]';
            warnings.push(`[E-QCTRL] ${tag} on '${fn.name}' contains control flow; reversible weaving requires a linear gate sequence`);
            continue;
        }
        if (fn.quantum?.undo) newFns.push(synthUndo(fn, userGates));
        if (fn.quantum?.steer) newFns.push(synthSteer(fn, userGates, warnings));
    }
    program.body.push(...newFns);
    return warnings;
}