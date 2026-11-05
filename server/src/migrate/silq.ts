// ============================================================================
// silq.ts —— Silq（ETH Zürich）前端：源码 → CircuitIR
//
// 覆盖 Silq 核心量子子集（研究型语言，聚焦门调用 + 测量）：
//   def main() { ... }
//   q := 0:𝔹;            —— 分配 qubit（|0⟩）
//   q := H(q);           —— 门调用（左值重新绑定忽略）
//   x := measure(q);     —— 测量
//
// Silq 门名：H/X/Y/Z/S/T/rotX(θ,q)/rotY(θ,q)/rotZ(θ,q)/CNOT/SWAP/phase(θ)
// 此处归一为 qelib1 风格，交 emitter 的 lowerGate 统一发射。
// ============================================================================

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

// 角度在前（θ, q）的旋转门
const SILQ_ANGLE_GATES = new Set(['rotX', 'rotY', 'rotZ']);

// ─── 词法分析 ──────────────────────────────────────────────────────────────

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
        // 非 ASCII 字符（如数学字体 𝔹/ℕ 等，UTF-16 代理对）：作为一个 ident token
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

// ─── 语法分析 ──────────────────────────────────────────────────────────────

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
        // 定位 def（跳过 import 等）
        while (this.peek().kind !== 'eof' && !this.isIdent('def')) this.next();
        if (this.isIdent('def')) {
            this.next();
            this.expectIdent(); // 函数名
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
        // return 语句：跳过（迁移后的 rvdl_main 统一 return 0）
        if (this.isIdent('return')) {
            this.next();
            this.skipToSemicolon();
            return;
        }

        // 赋值：name := expr;
        const lhs = this.expectIdent();
        this.eatPunct(':=');

        // qubit 分配：q := 0:𝔹 或 q := 0:!𝔹
        if (this.peek().kind === 'number' && (this.peek(1).kind === 'punct' && this.peek(1).value === ':')) {
            this.next(); // 0
            this.eatPunct(':');
            // 类型（𝔹 或 !𝔹）
            if (this.peek().kind === 'punct' && this.peek().value === '!') this.next();
            this.next(); // 𝔹
            this.eatPunct(';');
            this.qubits.set(lhs, this.ir.qubitCount++);
            return;
        }

        // measure：x := measure(q);
        if (this.isIdent('measure')) {
            this.next();
            this.eatPunct('(');
            const q = this.resolveQubit(this.expectIdent());
            this.eatPunct(')');
            this.eatPunct(';');
            this.ir.ops.push({ kind: 'measure', qubit: q, bit: this.ir.bitCount++ });
            return;
        }

        // 门调用：H(q) / rotZ(θ, q) / CNOT(c, t)
        const gateName = this.expectIdent();
        const mapped = SILQ_GATE_MAP[gateName];
        if (!mapped) {
            // 未识别：跳过到分号
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
        // 若左值是 qubit 门结果，保持 qubit 引用（就地门语义，忽略重新绑定）
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

/** 解析 Silq 源码为 CircuitIR */
export function parseSilq(src: string): CircuitIR {
    return new SilqParser(src).parse();
}