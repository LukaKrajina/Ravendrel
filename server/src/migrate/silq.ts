import { CircuitIR, emptyCircuit, gate } from './circuit-ir';

const SILQ_GATE_MAP: Record<string, string> = {
    H: 'h',
    X: 'x',
    Y: 'y',
    Z: 'z',
    S: 's',
    T: 't',
    rotX: 'rx',
    rotY: 'ry',
    rotZ: 'rz',
    CNOT: 'cx',
    SWAP: 'swap',
};

const SILQ_ANGLE_GATES = new Set(['rotX', 'rotY', 'rotZ']);


interface Tok {
    kind: 'ident' | 'number' | 'punct' | 'eof';
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
        if (/[0-9]/.test(ch)) {
            let j = i;
            while (j < n && /[0-9.eE+-]/.test(src[j])) j++;
            toks.push({ kind: 'number', value: src.slice(i, j), line });
            i = j;
            continue;
        }
        if (ch.codePointAt(0)! > 127) {
            const cp = src.codePointAt(i)!;
            const s = String.fromCodePoint(cp);
            toks.push({ kind: 'ident', value: s, line });
            i += s.length;
            continue;
        }
        if (/[\p{L}_]/u.test(ch)) {
            let j = i;
            while (j < n && /[\p{L}\p{N}_!]/u.test(src[j])) j++;
            toks.push({ kind: 'ident', value: src.slice(i, j), line });
            i = j;
            continue;
        }
        if (ch === ':' && src[i + 1] === '=') {
            toks.push({ kind: 'punct', value: ':=', line });
            i += 2;
            continue;
        }
        if (ch === ':' && src[i + 1] === ':') {
            toks.push({ kind: 'punct', value: '::', line });
            i += 2;
            continue;
        }
        if ('[]();,{}:=<>+-*/%!'.includes(ch)) {
            toks.push({ kind: 'punct', value: ch, line });
            i++;
            continue;
        }
        throw new Error(`[rvdl migrate:silq] Unexpected character '${ch}' at line ${line}`);
    }
    toks.push({ kind: 'eof', value: '', line });
    return toks;
}


class SilqParser {
    private toks: Tok[];
    private pos = 0;
    private ir: CircuitIR = emptyCircuit('silq');
    private qubits = new Map<string, number>();

    constructor(src: string) {
        this.toks = tokenize(src);
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
            throw new Error(`[silq] Expected '${v}' at line ${t.line}, got '${t.value}'`);
        }
    }

    private expectIdent(): string {
        const t = this.next();
        if (t.kind !== 'ident') throw new Error(`[silq] Expected identifier at line ${t.line}`);
        return t.value;
    }

    parse(): CircuitIR {
        while (this.peek().kind !== 'eof' && !this.isIdent('def')) this.next();
        if (this.isIdent('def')) {
            this.next();
            this.expectIdent();
            this.eatPunct('(');
            this.eatPunct(')');
            this.eatPunct('{');
            this.parseBody();
            this.eatPunct('}');
        }
        return this.ir;
    }

    private parseBody(): void {
        while (this.peek().kind !== 'eof' && !(this.peek().kind === 'punct' && this.peek().value === '}')) {
            this.parseStatement();
        }
    }

    private parseStatement(): void {
        if (this.isIdent('return')) {
            this.next();
            this.skipToSemicolon();
            return;
        }

        const lhs = this.expectIdent();
        this.eatPunct(':=');

        if (this.peek().kind === 'number' && (this.peek(1).kind === 'punct' && this.peek(1).value === ':')) {
            this.next();
            this.eatPunct(':');
            if (this.peek().kind === 'punct' && this.peek().value === '!') this.next();
            this.next();
            this.eatPunct(';');
            this.qubits.set(lhs, this.ir.qubitCount++);
            return;
        }

        if (this.isIdent('measure')) {
            this.next();
            this.eatPunct('(');
            const q = this.resolveQubit(this.expectIdent());
            this.eatPunct(')');
            this.eatPunct(';');
            this.ir.ops.push({ kind: 'measure', qubit: q, bit: this.ir.bitCount++ });
            return;
        }

        const gateName = this.expectIdent();
        const mapped = SILQ_GATE_MAP[gateName];
        if (!mapped) {
            this.skipToSemicolon();
            return;
        }
        this.eatPunct('(');
        const params: (number | string)[] = [];
        const qubits: number[] = [];
        let idx = 0;
        while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
            const t = this.next();
            if (SILQ_ANGLE_GATES.has(gateName) && idx === 0) {
                if (t.kind === 'number') params.push(Number(t.value));
                else if (t.kind === 'ident') params.push(t.value);
            } else {
                if (t.kind === 'ident') qubits.push(this.resolveQubit(t.value));
            }
            idx++;
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');

        this.ir.ops.push(gate(mapped, qubits, params));
    }

    private resolveQubit(name: string): number {
        const v = this.qubits.get(name);
        if (typeof v === 'number') return v;
        throw new Error(`[silq] Undefined qubit '${name}'`);
    }

    private skipToSemicolon(): void {
        while (this.peek().kind !== 'eof' && !(this.peek().kind === 'punct' && this.peek().value === ';')) {
            this.next();
        }
        if (this.peek().kind === 'punct' && this.peek().value === ';') this.next();
    }
}

export function parseSilq(src: string): CircuitIR {
    return new SilqParser(src).parse();
}