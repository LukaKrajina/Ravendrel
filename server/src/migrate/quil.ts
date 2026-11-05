// ============================================================================
// quil.ts —— Quil（Rigetti）前端：源码 → CircuitIR
//
// 覆盖 Quil 核心子集：
//   DECLARE ro BIT[N] / H 0 / CNOT 0 1 / RX(pi/2) 0 / MEASURE 0 ro[0]
//
// Quil 的 qubit 用整数索引（无需声明），经典 bit 用 DECLARE 声明寄存器。
// 门名在此归一为 OpenQASM qelib1 风格，交给 emitter 的 lowerGate 统一分解。
// ============================================================================

import { CircuitIR, Op, emptyCircuit, gate } from './circuit-ir';

// Quil 门名 → qelib1 风格门名（lowerGate 可识别）
const QUIL_GATE_MAP: Record<string, string> = {
    H: 'h',
    X: 'x',
    Y: 'y',
    Z: 'z',
    S: 's',
    T: 't',
    RX: 'rx',
    RY: 'ry',
    RZ: 'rz',
    CNOT: 'cx',
    CZ: 'cz',
    CY: 'cy',
    SWAP: 'swap',
    CCNOT: 'ccx',
    CSWAP: 'cswap',
    CH: 'ch',
    PHASE: 'u1',
    CPHASE: 'cu1',
};

// ─── 词法分析 ──────────────────────────────────────────────────────────────

interface Tok {
    kind: 'ident' | 'number' | 'string' | 'punct' | 'eof';
    value: string;
    line: number;
    /** 1 基列号。DEFGATE 的矩阵体靠「缩进」定界，没有列号就无法把
     *  续行与顶格新语句区分开（曾因此把矩阵行当门调用解析）。 */
    col: number;
}

function tokenize(src: string): Tok[] {
    const toks: Tok[] = [];
    let i = 0;
    let line = 1;
    let col = 1;
    const n = src.length;
    while (i < n) {
        const ch = src[i];
        if (ch === ' ' || ch === '\t' || ch === '\r') { i++; col++; continue; }
        if (ch === '\n') { line++; col = 1; i++; continue; }
        if (ch === '#' || (ch === '/' && src[i + 1] === '/')) {
            while (i < n && src[i] !== '\n') { i++; col++; }
            continue;
        }
        if (ch === '"') {
            let j = i + 1;
            while (j < n && src[j] !== '"') j++;
            toks.push({ kind: 'string', value: src.slice(i + 1, j), line, col });
            const len = j + 1 - i; // 含两端引号
            i = j + 1;
            col += len;
            continue;
        }
        if (/[0-9]/.test(ch)) {
            let j = i;
            while (j < n && /[0-9.eE+-]/.test(src[j])) j++;
            toks.push({ kind: 'number', value: src.slice(i, j), line, col });
            col += j - i;
            i = j;
            continue;
        }
        if (/[A-Za-z_]/.test(ch)) {
            let j = i;
            while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
            toks.push({ kind: 'ident', value: src.slice(i, j), line, col });
            col += j - i;
            i = j;
            continue;
        }
        if ('[]();,(){}*+-/%:'.includes(ch)) {
            toks.push({ kind: 'punct', value: ch, line, col });
            i++; col++;
            continue;
        }
        throw new Error(`[rvdl migrate:quil] Unexpected character '${ch}' at line ${line}`);
    }
    toks.push({ kind: 'eof', value: '', line, col });
    return toks;
}

// ─── 语法分析 ──────────────────────────────────────────────────────────────

class QuilParser {
    private toks: Tok[];
    private pos = 0;
    private ir: CircuitIR = emptyCircuit('quil');
    private cregs = new Map<string, { offset: number; size: number }>();
    /** DEFGATE / DEFCIRCUIT 定义过的名字（矩阵形式，rvdl 无对应物） */
    private defGates = new Set<string>();

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
            throw new Error(`[quil] Expected '${v}' at line ${t.line}, got '${t.value}'`);
        }
    }

    private expectIdent(): string {
        const t = this.next();
        if (t.kind !== 'ident') throw new Error(`[quil] Expected identifier at line ${t.line}`);
        return t.value;
    }

    parse(): CircuitIR {
        while (this.peek().kind !== 'eof') {
            this.parseLine();
        }
        return this.ir;
    }

    private parseLine(): void {
        const t = this.peek();

        if (this.isIdent('DECLARE')) {
            this.next();
            const name = this.expectIdent();
            this.expectIdent(); // BIT
            this.eatPunct('[');
            const size = Number(this.next().value);
            this.eatPunct(']');
            this.cregs.set(name, { offset: this.ir.bitCount, size });
            this.ir.bitCount += size;
            return;
        }

        if (this.isIdent('MEASURE')) {
            this.next();
            const qubit = Number(this.next().value);
            const c = this.parseBitRef();
            this.ir.ops.push({ kind: 'measure', qubit, bit: this.resolveC(c.name, c.idx) });
            return;
        }

        if (this.isIdent('RESET')) {
            this.next();
            const qubit = Number(this.next().value);
            this.ir.ops.push({ kind: 'reset', qubit });
            return;
        }

        // DEFGATE / DEFCIRCUIT：矩阵/电路体定义在**缩进的续行**上。
        // 曾经只跳到行尾 —— 矩阵行（`1, 0` / `0, exp(i*theta)`）随后被当作
        // 门调用逐 token 解析，轻则抛「Unexpected character」，重则把矩阵里的
        // 标识符误认成门名。现在按列号定界：头部行 + 所有缩进续行整体跳过，
        // 并把名字记下来，被调用时给出**指名道姓**的错误（矩阵门无法映射为
        // rvdl 的 @[gate] 函数体，诚实失败优于静默跳过）。
        if (this.isIdent('DEFGATE') || this.isIdent('DEFCIRCUIT')) {
            const headerLine = t.line;
            this.next(); // DEFGATE / DEFCIRCUIT
            const defName = this.expectIdent();
            this.defGates.add(defName);
            while (this.peek().kind !== 'eof' && this.peek().line === headerLine) this.next();
            while (this.peek().kind !== 'eof' &&
                   this.peek().line > headerLine && this.peek().col > 1) {
                this.next();
            }
            return;
        }

        // 门调用：GATE q1 q2 ... 或 GATE(params) q1 q2 ...
        if (t.kind === 'ident') {
            const gateName = this.expectIdent().toUpperCase();
            const params: (number | string)[] = [];
            if (this.peek().kind === 'punct' && this.peek().value === '(') {
                this.next();
                while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                    const p = this.next();
                    if (p.kind === 'number') params.push(Number(p.value));
                    else if (p.kind === 'ident') params.push(p.value); // pi
                    else throw new Error(`[quil] Unexpected param at line ${p.line}`);
                    if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
                }
                this.eatPunct(')');
            }

            const qubits: number[] = [];
            const startLine = this.peek().line;
            while (this.peek().kind === 'number' && this.peek().line === startLine) {
                qubits.push(Number(this.next().value));
            }

            const mapped = QUIL_GATE_MAP[gateName];
            if (!mapped) {
                if (this.defGates.has(gateName)) {
                    throw new Error(
                        `[rvdl migrate:quil] Gate '${gateName}' is defined by DEFGATE (matrix form); `
                        + `matrix-defined gates have no rvdl @[gate] function body to map to. `
                        + `Rewrite it as a composition of standard gates, or inline its action manually.`);
                }
                throw new Error(`[rvdl migrate:quil] Unsupported Quil gate '${gateName}'`);
            }
            this.ir.ops.push(gate(mapped, qubits, params));
            // 更新 qubit 数
            for (const q of qubits) this.ir.qubitCount = Math.max(this.ir.qubitCount, q + 1);
            return;
        }

        // 跳过空行 / 未知
        this.next();
    }

    private parseBitRef(): { name: string; idx: number } {
        const name = this.expectIdent();
        this.eatPunct('[');
        const idx = Number(this.next().value);
        this.eatPunct(']');
        return { name, idx };
    }

    private resolveC(name: string, idx: number): number {
        const r = this.cregs.get(name);
        if (!r) throw new Error(`[quil] Undefined register '${name}'`);
        return r.offset + idx;
    }
}

/** 解析 Quil 源码为 CircuitIR */
export function parseQuil(src: string): CircuitIR {
    return new QuilParser(src).parse();
}