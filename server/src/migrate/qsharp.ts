import { CircuitIR, Op, emptyCircuit, gate } from './circuit-ir';

const QSHARP_GATE_MAP: Record<string, string> = {
    H: 'h',
    X: 'x',
    Y: 'y',
    Z: 'z',
    S: 's',
    T: 't',
    Rx: 'rx',
    Ry: 'ry',
    Rz: 'rz',
    CNOT: 'cx',
    CCNOT: 'ccx',
    SWAP: 'swap',
};

const ANGLE_GATES = new Set(['Rx', 'Ry', 'Rz']);


interface Tok {
    kind: 'ident' | 'number' | 'string' | 'punct' | 'eof';
    value: string;
    line: number;
}

function tokenize(src: string): Tok[] {
    const toks: Tok[] = [];
    let i = 0;
    let line = 1;
    const n = src.length;
    while (i < n) {
        const ch = src[i];
        if (ch === ' ' || ch === '\t' || ch === '\r') { i++; continue; }
        if (ch === '\n') { line++; i++; continue; }
        if (ch === '/' && src[i + 1] === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }
        if (ch === '"') {
            let j = i + 1;
            while (j < n && src[j] !== '"') j++;
            toks.push({ kind: 'string', value: src.slice(i + 1, j), line });
            i = j + 1;
            continue;
        }
        if (/[0-9]/.test(ch)) {
            let j = i;
            while (j < n && /[0-9.eE+-]/.test(src[j])) j++;
            toks.push({ kind: 'number', value: src.slice(i, j), line });
            i = j;
            continue;
        }
        if (/[A-Za-z_]/.test(ch)) {
            let j = i;
            while (j < n && /[A-Za-z0-9_.]/.test(src[j])) j++;
            toks.push({ kind: 'ident', value: src.slice(i, j), line });
            i = j;
            continue;
        }
        if ((ch === '=' && src[i + 1] === '=') || (ch === '!' && src[i + 1] === '=') ||
            (ch === '&' && src[i + 1] === '&') || (ch === '|' && src[i + 1] === '|') ||
            (ch === '.' && src[i + 1] === '.') ||
            (ch === '<' && src[i + 1] === '=') || (ch === '>' && src[i + 1] === '=')) {
            toks.push({ kind: 'punct', value: src.slice(i, i + 2), line });
            i += 2;
            continue;
        }
        if ('[]();,{}:()=<>+-*/%'.includes(ch)) {
            toks.push({ kind: 'punct', value: ch, line });
            i++;
            continue;
        }
        throw new Error(`[rvdl migrate:qsharp] Unexpected character '${ch}' at line ${line}`);
    }
    toks.push({ kind: 'eof', value: '', line });
    return toks;
}


interface OperationInfo {
    name: string;
    params: string[];
    bodyStart: number;
    bodyEnd: number;
}

class QSharpParser {
    private toks: Tok[];
    private pos = 0;
    private ir: CircuitIR = emptyCircuit('qsharp');

    private qubits = new Map<string, number | number[]>();

    private classical = new Map<string, number>();

    private operations = new Map<string, OperationInfo>();
    private operationOrder: string[] = [];
    private calledOps = new Set<string>();
    private inlineStack: string[] = [];





    private opSinks: Op[][] = [];

    constructor(src: string) {
        this.toks = tokenize(src);
        this.opSinks.push(this.ir.ops);
    }

    private peek(offset = 0): Tok {
        return this.toks[Math.min(this.pos + offset, this.toks.length - 1)];
    }

    private next(): Tok {
        return this.toks[this.pos++];
    }

    private isIdent(v?: string): boolean {
        const t = this.peek();
        return t.kind === 'ident' && (v === undefined || t.value === v);
    }

    private eatPunct(v: string): void {
        const t = this.next();
        if (t.kind !== 'punct' || t.value !== v) {
            throw new Error(`[qsharp] Expected '${v}' at line ${t.line}, got '${t.value}'`);
        }
    }

    private expectIdent(): string {
        const t = this.next();
        if (t.kind !== 'ident') throw new Error(`[qsharp] Expected identifier at line ${t.line}`);
        return t.value;
    }

    private get sink(): Op[] {
        return this.opSinks[this.opSinks.length - 1];
    }

    private pushOp(op: Op): void {
        this.sink.push(op);
    }

    parse(): CircuitIR {
        this.scanOperations();
        this.scanCalls();
        const entries = this.operationOrder.filter(n => !this.calledOps.has(n));
        const targets = entries.length > 0 ? entries : this.operationOrder;
        for (const name of targets) {
            const info = this.operations.get(name)!;
            this.pos = info.bodyStart;
            this.qubits.clear();
            this.classical.clear();
            this.parseBody();
        }
        return this.ir;
    }

    private scanOperations(): void {
        const saved = this.pos;
        this.pos = 0;
        while (this.peek().kind !== 'eof') {
            if (this.isIdent('operation')) {
                this.next();
                const name = this.expectIdent();
                const params: string[] = [];
                if (this.peek().kind === 'punct' && this.peek().value === '(') {
                    this.next();
                    while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                        if (this.peek().kind === 'ident') {
                            const pname = this.next().value;
                            if (this.peek().kind === 'punct' && this.peek().value === ':') {
                                this.next();
                                while (!(this.peek().kind === 'punct' &&
                                         (this.peek().value === ',' || this.peek().value === ')'))) {
                                    this.next();
                                }
                            }
                            params.push(pname);
                        } else {
                            this.next();
                        }
                        if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
                    }
                    this.eatPunct(')');
                }
                while (!(this.peek().kind === 'punct' && this.peek().value === '{') &&
                       this.peek().kind !== 'eof') {
                    this.next();
                }
                if (this.peek().kind === 'eof') break;
                this.eatPunct('{');
                const bodyStart = this.pos;
                let depth = 1;
                while (depth > 0 && this.peek().kind !== 'eof') {
                    const t = this.next();
                    if (t.kind === 'punct' && t.value === '{') depth++;
                    else if (t.kind === 'punct' && t.value === '}') depth--;
                }
                const bodyEnd = this.pos - 1;
                this.operations.set(name, { name, params, bodyStart, bodyEnd });
                this.operationOrder.push(name);
            } else {
                this.next();
            }
        }
        this.pos = saved;
    }

    private scanCalls(): void {
        for (const [name, info] of this.operations) {
            for (let i = info.bodyStart; i < info.bodyEnd; i++) {
                const t = this.toks[i];
                if (t.kind !== 'ident' || t.value === name) continue;
                if (!this.operations.has(t.value)) continue;
                const nx = this.toks[i + 1];
                if (nx && nx.kind === 'punct' && nx.value === '(') {
                    this.calledOps.add(t.value);
                }
            }
        }
    }

    private skipBalancedParens(): void {
        if (!(this.peek().kind === 'punct' && this.peek().value === '(')) return;
        let depth = 0;
        do {
            const t = this.next();
            if (t.kind === 'punct' && t.value === '(') depth++;
            else if (t.kind === 'punct' && t.value === ')') depth--;
        } while (depth > 0 && this.peek().kind !== 'eof');
    }

    private parseBody(): void {
        while (this.peek().kind !== 'eof' && !(this.peek().kind === 'punct' && this.peek().value === '}')) {
            this.parseStatement();
        }
    }

    private parseStatement(): void {
        if (this.isIdent('use')) {
            this.parseUse();
            return;
        }
        if (this.isIdent('using')) {
            this.parseUsing();
            return;
        }
        if (this.isIdent('let')) {
            this.parseLet();
            return;
        }
        if (this.isIdent('mutable')) {
            this.next();
            this.expectIdent();
            this.eatPunct('=');
            this.skipExpression();
            this.eatPunct(';');
            return;
        }
        if (this.isIdent('set')) {
            this.next();
            const name = this.expectIdent();
            this.eatPunct('=');
            const v = this.next();
            const bit = this.classical.get(name);
            if (bit === undefined) {
                throw new Error(`[qsharp] 'set ${name}' targets an unknown classical value at line ${v.line}`);
            }
            const value = v.value === 'One' ? 1 : v.value === 'Zero' ? 0 : Number(v.value);
            this.pushOp({ kind: 'classicalAssign', name: `c${bit}`, value: String(value) });
            this.eatPunct(';');
            return;
        }
        if (this.isIdent('ResetAll')) {
            this.next();
            this.skipBalancedParens();
            this.eatPunct(';');
            return;
        }
        if (this.isIdent('Reset')) {
            this.next();
            this.skipBalancedParens();
            this.eatPunct(';');
            return;
        }
        if (this.isIdent('for')) {
            this.parseFor();
            return;
        }
        if (this.isIdent('while')) {
            this.parseWhile();
            return;
        }
        if (this.isIdent('if')) {
            this.parseIf();
            return;
        }
        if (this.isIdent('return')) {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ';')) this.next();
            this.eatPunct(';');
            return;
        }
        this.parseGateCall();
    }

    private parseUse(): void {
        this.next();
        this.eatPunct('(');
        const names: string[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
            names.push(this.expectIdent());
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct('=');
        this.eatPunct('(');
        for (const name of names) {
            this.expectIdent();
            this.eatPunct('(');
            this.eatPunct(')');
            this.qubits.set(name, this.ir.qubitCount++);
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');
    }

    private parseUsing(): void {
        this.next();
        this.eatPunct('(');
        const name = this.expectIdent();
        this.eatPunct('=');
        this.expectIdent();
        this.eatPunct('[');
        const n = Number(this.next().value);
        this.eatPunct(']');
        this.eatPunct(')');

        const arr: number[] = [];
        for (let i = 0; i < n; i++) arr.push(this.ir.qubitCount++);
        this.qubits.set(name, arr);

        this.eatPunct('{');
        this.parseBody();
        this.eatPunct('}');
    }


    private parseLet(): void {
        this.next();
        const names: string[] = [];
        if (this.peek().kind === 'punct' && this.peek().value === '(') {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                names.push(this.expectIdent());
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(')');
        } else {
            names.push(this.expectIdent());
        }
        this.eatPunct('=');

        const measureQubits: number[] = [];
        if (this.peek().kind === 'punct' && this.peek().value === '(') {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                const fn = this.expectIdent();
                if (fn !== 'M') {
                    throw new Error(`[qsharp] let-tuple only supports M(...) measurements, got '${fn}'`);
                }
                this.eatPunct('(');
                measureQubits.push(this.resolveQubit(this.expectIdent()));
                this.eatPunct(')');
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(')');
        } else {
            const fn = this.expectIdent();
            if (fn === 'M') {
                this.eatPunct('(');
                measureQubits.push(this.resolveQubit(this.expectIdent()));
                this.eatPunct(')');
            } else {
                this.skipExpression();
                this.eatPunct(';');
                return;
            }
        }
        this.eatPunct(';');

        if (measureQubits.length !== names.length) {
            throw new Error(`[qsharp] let binds ${names.length} name(s) but ${measureQubits.length} measurement(s)`);
        }
        for (let i = 0; i < names.length; i++) {
            const bit = this.ir.bitCount++;
            this.pushOp({ kind: 'measure', qubit: measureQubits[i], bit });
            this.classical.set(names[i], bit);
        }
    }

    private parseFor(): void {
        this.next();
        const varName = this.expectIdent();
        this.expectIdent();
        const lo = Number(this.next().value);
        this.eatPunct('..');
        const hiTok = this.next();
        let hi: number;
        if (hiTok.kind === 'number') hi = Number(hiTok.value);
        else if (hiTok.kind === 'ident') {
            const n = this.qubits.get(hiTok.value);
            if (Array.isArray(n)) hi = n.length;
            else hi = Number(hiTok.value);
        } else hi = 0;
        if (this.peek().kind === 'punct' && this.peek().value === '-') {
            this.next();
            const k = Number(this.next().value);
            hi = hi - k;
        }

        this.eatPunct('{');

        const saved = this.loopVar;
        for (let i = lo; i < hi; i++) {
            this.loopVar = { name: varName, value: i };
            this.parseBody();
        }
        this.loopVar = saved;

        this.eatPunct('}');
    }

    private loopVar: { name: string; value: number } | null = null;



    private parseIf(): void {
        this.next();
        this.eatPunct('(');
        const condition = this.parseCondition();
        this.eatPunct(')');
        this.eatPunct('{');
        const body: Op[] = [];
        this.opSinks.push(body);
        this.parseBody();
        this.opSinks.pop();
        this.eatPunct('}');

        let alternate: Op[] | undefined;
        if (this.isIdent('else')) {
            this.next();
            if (this.peek().kind === 'punct' && this.peek().value === '{') {
                this.eatPunct('{');
                const alt: Op[] = [];
                this.opSinks.push(alt);
                this.parseBody();
                this.opSinks.pop();
                this.eatPunct('}');
                alternate = alt;
            } else if (this.isIdent('if')) {
                const nested: Op[] = [];
                this.opSinks.push(nested);
                this.parseIf();
                this.opSinks.pop();
                alternate = nested;
            } else {
                throw new Error('[qsharp] Expected \'{\' or \'if\' after else');
            }
        }
        this.pushOp({ kind: 'if', condition, body, alternate });
    }

    private parseWhile(): void {
        this.next();
        this.eatPunct('(');
        const condition = this.parseCondition();
        this.eatPunct(')');
        this.eatPunct('{');
        const body: Op[] = [];
        this.opSinks.push(body);
        this.parseBody();
        this.opSinks.pop();
        this.eatPunct('}');
        this.pushOp({ kind: 'while', condition, body });
    }


    private parseCondition(): string {
        return this.parseOr();
    }

    private parseOr(): string {
        let left = this.parseAnd();
        while (this.isIdent('or')) {
            this.next();
            left = `(${left} || ${this.parseAnd()})`;
        }
        return left;
    }

    private parseAnd(): string {
        let left = this.parseNot();
        while (this.isIdent('and')) {
            this.next();
            left = `(${left} && ${this.parseNot()})`;
        }
        return left;
    }

    private parseNot(): string {
        if (this.isIdent('not')) {
            this.next();
            return `(!${this.parseNot()})`;
        }
        return this.parseComparison();
    }

    private parseComparison(): string {
        const left = this.parsePrimary();
        const t = this.peek();
        if (t.kind === 'punct' && ['==', '!=', '<', '>', '<=', '>='].includes(t.value)) {
            this.next();
            return `${left} ${t.value} ${this.parsePrimary()}`;
        }
        return left;
    }

    private parsePrimary(): string {
        const t = this.next();
        if (t.kind === 'punct' && t.value === '(') {
            const inner = this.parseOr();
            this.eatPunct(')');
            return `(${inner})`;
        }
        if (t.kind === 'number') return t.value;
        if (t.kind === 'ident') {
            if (t.value === 'One') return '1';
            if (t.value === 'Zero') return '0';
            if (t.value === 'true') return '1';
            if (t.value === 'false') return '0';
            const bit = this.classical.get(t.value);
            if (bit !== undefined) return `c${bit}`;
            const nx = this.toks[this.pos];
            if (nx && nx.kind === 'punct' && nx.value === '(') {
                throw new Error(
                    `[qsharp] function call '${t.value}(...)' in a condition is not supported `
                    + `(line ${t.line}); only classical values, comparisons and and/or/not are`);
            }
            return t.value;
        }
        throw new Error(`[qsharp] Unexpected token '${t.value}' in condition at line ${t.line}`);
    }

    private parseGateCall(): void {
        const gateName = this.expectIdent();
        const mapped = QSHARP_GATE_MAP[gateName];
        if (!mapped) {
            if (this.operations.has(gateName)) {
                this.inlineOperation(gateName);
                return;
            }
            this.skipStatement();
            return;
        }

        this.eatPunct('(');
        const args: (number | string)[] = [];
        const qubits: number[] = [];
        let idx = 0;
        while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
            const t = this.next();
            if (ANGLE_GATES.has(gateName) && idx === 0) {
                if (t.kind === 'number') args.push(Number(t.value));
                else if (t.kind === 'ident') args.push(t.value);
            } else {
                if (t.kind === 'ident') qubits.push(this.resolveQubit(t.value));
            }
            idx++;
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');

        this.pushOp(gate(mapped, qubits, args));
    }













    private inlineOperation(name: string): void {
        if (this.inlineStack.includes(name)) {
            throw new Error(
                `[qsharp] recursive operation '${name}' cannot be inlined into a flat circuit `
                + `(cycle: ${[...this.inlineStack, name].join(' -> ')})`);
        }
        if (this.inlineStack.length >= 32) {
            throw new Error(`[qsharp] operation inlining depth exceeded 32 at '${name}'`);
        }
        const info = this.operations.get(name)!;

        this.eatPunct('(');
        const args: number[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
            const t = this.next();
            if (t.kind === 'ident') args.push(this.resolveQubit(t.value));
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');

        if (args.length !== info.params.length) {
            throw new Error(
                `[qsharp] operation '${name}' expects ${info.params.length} qubit argument(s), `
                + `got ${args.length}`);
        }

        const savedQubits = this.qubits;
        const savedClassical = this.classical;
        const savedPos = this.pos;
        this.qubits = new Map();
        this.classical = new Map();
        info.params.forEach((p, i) => this.qubits.set(p, args[i]));
        this.inlineStack.push(name);
        this.pos = info.bodyStart;
        this.parseBody();
        this.inlineStack.pop();
        this.qubits = savedQubits;
        this.classical = savedClassical;
        this.pos = savedPos;
    }

    private resolveQubit(name: string): number {
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            this.next();
            const idxTok = this.next();
            let idx: number;
            if (idxTok.kind === 'number') idx = Number(idxTok.value);
            else if (idxTok.kind === 'ident' && this.loopVar && this.loopVar.name === idxTok.value) idx = this.loopVar.value;
            else idx = 0;
            this.eatPunct(']');
            const arr = this.qubits.get(name);
            if (Array.isArray(arr)) return arr[idx];
            throw new Error(`[qsharp] '${name}' is not a qubit array`);
        }
        const v = this.qubits.get(name);
        if (typeof v === 'number') return v;
        throw new Error(`[qsharp] Undefined qubit '${name}'`);
    }

    private skipExpression(): void {
        while (!(this.peek().kind === 'punct' && (this.peek().value === ';' || this.peek().value === ')'))) {
            this.next();
        }
    }

    private skipStatement(): void {
        let depth = 0;
        while (this.peek().kind !== 'eof') {
            const t = this.next();
            if (t.kind === 'punct' && t.value === '{') depth++;
            if (t.kind === 'punct' && t.value === '}') {
                if (depth === 0) { this.pos--; return; }
                depth--;
            }
            if (t.kind === 'punct' && t.value === ';' && depth === 0) return;
        }
    }
}

export function parseQsharp(src: string): CircuitIR {
    return new QSharpParser(src).parse();
}