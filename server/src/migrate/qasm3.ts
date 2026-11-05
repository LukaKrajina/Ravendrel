// ============================================================================
// qasm3.ts —— OpenQASM 3.0 前端：源码 → CircuitIR
//
// 覆盖 OpenQASM 3.0 的核心子集：
//   OPENQASM 3.0; / qubit[N] / bit[N] 声明（兼容 qreg/creg）、内建门、
//   c = measure q; / measure q[i] -> c[i];、reset / barrier、
//   for i in [lo:hi]（循环展开）、if (c[i] == v) { ... }、自定义 gate 定义。
//
// 与 qasm2 的关键差异：经典控制流（for/if 完整语句块）与数组式声明。
// ============================================================================

import { CircuitIR, Op, GateDef, emptyCircuit, gate } from './circuit-ir';

// ─── 词法分析 ──────────────────────────────────────────────────────────────

type TokKind = 'ident' | 'number' | 'string' | 'punct' | 'eof';

interface Tok {
    kind: TokKind;
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
            while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
            toks.push({ kind: 'ident', value: src.slice(i, j), line });
            i = j;
            continue;
        }
        // 双字符运算符
        if ((ch === '=' && src[i + 1] === '=') || (ch === '!' && src[i + 1] === '=') ||
            (ch === '<' && src[i + 1] === '=') || (ch === '>' && src[i + 1] === '=') ||
            (ch === '-' && src[i + 1] === '>') || (ch === '&' && src[i + 1] === '&') ||
            (ch === '|' && src[i + 1] === '|')) {
            toks.push({ kind: 'punct', value: src.slice(i, i + 2), line });
            i += 2;
            continue;
        }
        if ('[]();,{}:->=+*/%@^<>!&|'.includes(ch)) {
            toks.push({ kind: 'punct', value: ch, line });
            i++;
            continue;
        }
        throw new Error(`[rvdl migrate:qasm3] Unexpected character '${ch}' at line ${line}`);
    }

    toks.push({ kind: 'eof', value: '', line });
    return toks;
}

// ─── 语法分析 ──────────────────────────────────────────────────────────────

class Qasm3Parser {
    private toks: Tok[];
    private pos = 0;

    // 寄存器名 → { offset, size }
    private qregs = new Map<string, { offset: number; size: number }>();
    private cregs = new Map<string, { offset: number; size: number }>();
    private ir: CircuitIR = emptyCircuit('qasm3');

    // 循环展开：当前循环变量（for i in [lo:hi] 展开时设置）
    private loopVar: { name: string; value: number } | null = null;

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
            throw new Error(`[rvdl migrate:qasm3] Expected '${v}' at line ${t.line}, got '${t.value}'`);
        }
    }

    private expectIdent(): string {
        const t = this.next();
        if (t.kind !== 'ident') throw new Error(`[rvdl migrate:qasm3] Expected identifier at line ${t.line}`);
        return t.value;
    }

    private resolveQ(name: string, idx: number): number {
        const r = this.qregs.get(name);
        if (!r) throw new Error(`[rvdl migrate:qasm3] Undefined qubit register '${name}'`);
        if (idx >= r.size) throw new Error(`[rvdl migrate:qasm3] Qubit index ${idx} out of range for '${name}'`);
        return r.offset + idx;
    }

    private resolveC(name: string, idx: number): number {
        const r = this.cregs.get(name);
        if (!r) throw new Error(`[rvdl migrate:qasm3] Undefined bit register '${name}'`);
        if (idx >= r.size) throw new Error(`[rvdl migrate:qasm3] Bit index ${idx} out of range for '${name}'`);
        return r.offset + idx;
    }

    parse(): CircuitIR {
        // 头部
        if (this.isIdent('OPENQASM')) {
            this.next();
            if (this.peek().kind === 'number') this.next();
            this.eatPunct(';');
        }

        while (this.peek().kind !== 'eof') {
            this.parseTopLevel();
        }
        return this.ir;
    }

    private parseTopLevel(): void {
        if (this.isIdent('include')) {
            this.next();
            this.next(); // 字符串
            this.eatPunct(';');
            return;
        }
        if (this.isIdent('qubit')) { this.parseQubitDecl(); return; }
        if (this.isIdent('bit')) { this.parseBitDecl(); return; }
        if (this.isIdent('qreg')) { this.parseLegacyQreg(); return; }
        if (this.isIdent('creg')) { this.parseLegacyCreg(); return; }
        if (this.isIdent('gate')) { this.ir.gateDefs.push(this.parseGateDef()); return; }
        if (this.isClassicalTypeStart()) { this.parseClassicalDecl(); return; }
        this.ir.ops.push(...this.parseStatement());
    }

    /** 数组长度：`[N]` 或省略（默认 1） */
    private parseArraySize(): number {
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            this.next();
            const n = Number(this.next().value);
            this.eatPunct(']');
            return n;
        }
        return 1;
    }

    private parseQubitDecl(): void {
        this.next(); // 'qubit'
        const size = this.parseArraySize();
        const name = this.expectIdent();
        this.eatPunct(';');
        this.qregs.set(name, { offset: this.ir.qubitCount, size });
        this.ir.qubitCount += size;
    }

    private parseBitDecl(): void {
        this.next(); // 'bit'
        const size = this.parseArraySize();
        const name = this.expectIdent();
        this.eatPunct(';');
        this.cregs.set(name, { offset: this.ir.bitCount, size });
        this.ir.bitCount += size;
    }

    private parseLegacyQreg(): void {
        this.next(); // 'qreg'
        const name = this.expectIdent();
        this.eatPunct('[');
        const size = Number(this.next().value);
        this.eatPunct(']');
        this.eatPunct(';');
        this.qregs.set(name, { offset: this.ir.qubitCount, size });
        this.ir.qubitCount += size;
    }

    private parseLegacyCreg(): void {
        this.next(); // 'creg'
        const name = this.expectIdent();
        this.eatPunct('[');
        const size = Number(this.next().value);
        this.eatPunct(']');
        this.eatPunct(';');
        this.cregs.set(name, { offset: this.ir.bitCount, size });
        this.ir.bitCount += size;
    }

    /** 解析一条语句（门调用 / measure / reset / barrier / for / if），返回 Op[]。 */
    private parseStatement(): Op[] {
        if (this.isIdent('measure')) {
            this.next();
            return this.parseMeasureArrow();
        }
        if (this.isIdent('reset')) {
            this.next();
            const q = this.parseArgRef();
            this.eatPunct(';');
            return [{ kind: 'reset', qubit: this.resolveQ(q.name, q.idx) }];
        }
        if (this.isIdent('barrier')) {
            this.next();
            const qs: number[] = [];
            while (this.peek().kind !== 'punct' || this.peek().value !== ';') {
                const q = this.parseArgRef();
                qs.push(this.resolveQ(q.name, q.idx));
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(';');
            return [{ kind: 'barrier', qubits: qs }];
        }
        if (this.isIdent('for')) return this.parseFor();
        if (this.isIdent('if')) return [this.parseIf()];
        if (this.isIdent('while')) return [this.parseWhile()];

        // 赋值：lhs = rhs（lhs 为 ident 或 ident[idx]）
        if (this.peek().kind === 'ident' && this.eqAfterLhs() >= 0) {
            const afterEq = this.peek(this.eqAfterLhs() + 1);
            if (afterEq.kind === 'ident' && afterEq.value === 'measure') {
                return this.parseMeasureAssign();
            }
            return this.parseClassicalAssign();
        }

        return [this.parseGateCall((n, i) => this.resolveQ(n, i), null)];
    }

    /** 若当前是赋值（ident = ... 或 ident[idx] = ...），返回 '=' 的位置，否则 -1。 */
    private eqAfterLhs(): number {
        if (this.peek(0).kind !== 'ident') return -1;
        if (this.peek(1).kind === 'punct' && this.peek(1).value === '=') return 1;
        if (this.peek(1).kind === 'punct' && this.peek(1).value === '[' &&
            this.peek(3).kind === 'punct' && this.peek(3).value === ']' &&
            this.peek(4).kind === 'punct' && this.peek(4).value === '=') return 4;
        return -1;
    }

    /** 2.0 风格 measure q[i] -> c[i]; */
    private parseMeasureArrow(): Op[] {
        const q = this.parseArgRef();
        this.eatPunct('-');
        this.eatPunct('>');
        const c = this.parseArgRef();
        this.eatPunct(';');
        return [{ kind: 'measure', qubit: this.resolveQ(q.name, q.idx), bit: this.resolveC(c.name, c.idx) }];
    }

    /** 3.0 风格 c = measure q;（整寄存器，展开为逐 bit 测量） */
    private parseMeasureAssign(): Op[] {
        // 左值：ident 或 ident[idx]
        const cname = this.expectIdent();
        let cidx: number | null = null;
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            this.next();
            cidx = Number(this.next().value);
            this.eatPunct(']');
        }
        this.eatPunct('=');
        this.next(); // 'measure'
        // 右值：ident 或 ident[idx]
        const qname = this.expectIdent();
        let qidx: number | null = null;
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            this.next();
            qidx = Number(this.next().value);
            this.eatPunct(']');
        }
        this.eatPunct(';');

        // 单 bit：c[i] = measure q[j];
        if (cidx !== null || qidx !== null) {
            return [{ kind: 'measure', qubit: this.resolveQ(qname, qidx ?? 0), bit: this.resolveC(cname, cidx ?? 0) }];
        }

        // 整寄存器：c = measure q;
        const qr = this.qregs.get(qname);
        const cr = this.cregs.get(cname);
        if (!qr || !cr) throw new Error('[rvdl migrate:qasm3] measure requires declared qubit/bit registers');
        if (qr.size !== cr.size) throw new Error('[rvdl migrate:qasm3] measure: register sizes differ');

        const ops: Op[] = [];
        for (let i = 0; i < qr.size; i++) {
            ops.push({ kind: 'measure', qubit: qr.offset + i, bit: cr.offset + i });
        }
        return ops;
    }

    /** 解析一个可带负号的整数（词法层把 '-' 独立成 token，范围里的
     *  `[3:-1:0]` 倒序步长依赖它）。 */
    private parseSignedInt(): number {
        let sign = 1;
        if (this.peek().kind === 'punct' && this.peek().value === '-') {
            this.next();
            sign = -1;
        }
        const t = this.next();
        if (t.kind !== 'number') {
            throw new Error(`[rvdl migrate:qasm3] Expected integer at line ${t.line}, got '${t.value}'`);
        }
        return sign * Number(t.value);
    }

    /** for i in [lo:hi] { ... } 或 for i in [lo:step:hi] { ... }（循环展开）
     *
     * OpenQASM 3 的范围语法是 `[start : stop]`（步长 1）与 `[start : step : stop]`
     * —— **中间那个数是步长**。曾经的实现把三段式读成 `lo : hi : step` 并
     * 丢弃步长：`for i in [0:2:10]` 被迁移成 i = 0,1（步长 1、上界 2），
     * 而正确展开是 0,2,4,6,8 —— 静默的错误电路，且没有任何报错。
     */
    private parseFor(): Op[] {
        this.next(); // 'for'
        const varName = this.expectIdent();
        if (!this.isIdent('in')) throw new Error('[rvdl migrate:qasm3] Expected "in" in for loop');
        this.next(); // 'in'

        this.eatPunct('[');
        const start = this.parseSignedInt();
        this.eatPunct(':');
        const second = this.parseSignedInt();
        let stop: number;
        let step = 1;
        if (this.peek().kind === 'punct' && this.peek().value === ':') {
            // 三段式 [start : step : stop]
            this.next(); // ':'
            stop = this.parseSignedInt();
            step = second;
        } else {
            // 两段式 [start : stop]
            stop = second;
        }
        if (step === 0) {
            throw new Error('[rvdl migrate:qasm3] for-loop step must be non-zero');
        }
        this.eatPunct(']');
        this.eatPunct('{');

        const bodyStart = this.pos;

        // 先扫描定位循环体结束 `}`（处理嵌套大括号）
        let depth = 1;
        let closePos = this.pos;
        while (depth > 0 && this.peek().kind !== 'eof') {
            const t = this.next();
            if (t.kind === 'punct' && t.value === '{') depth++;
            else if (t.kind === 'punct' && t.value === '}') { depth--; closePos = this.pos - 1; }
        }

        // 循环展开：每个 i 重新解析一次循环体（loopVar 替换循环变量）。
        // 步长可负（`[9:-1:0]` 倒序），方向由 step 的符号决定。
        const out: Op[] = [];
        for (let i = start; step > 0 ? i < stop : i > stop; i += step) {
            this.pos = bodyStart;
            this.loopVar = { name: varName, value: i };
            out.push(...this.parseBlock());
            this.loopVar = null;
        }
        this.pos = closePos + 1;
        return out;
    }

    /** if (cond) { ... } [else { ... }] —— 支持复杂布尔条件 */
    private parseIf(): Op {
        this.next(); // 'if'
        this.eatPunct('(');
        const condition = this.parseExpressionString();
        this.eatPunct(')');
        this.eatPunct('{');
        const body = this.parseBlock();
        this.eatPunct('}');
        let alternate: Op[] | undefined;
        if (this.isIdent('else')) {
            this.next();
            this.eatPunct('{');
            alternate = this.parseBlock();
            this.eatPunct('}');
        }
        return { kind: 'if', condition, body, alternate };
    }

    /** while (cond) { ... } */
    private parseWhile(): Op {
        this.next(); // 'while'
        this.eatPunct('(');
        const condition = this.parseExpressionString();
        this.eatPunct(')');
        this.eatPunct('{');
        const body = this.parseBlock();
        this.eatPunct('}');
        return { kind: 'while', condition, body };
    }

    /** 经典变量声明：int[32] x = 0; / bool flag = true; / float[64] f = 0.0; */
    private static readonly CLASSICAL_TYPES: Record<string, string> = {
        int: 'int32',
        uint: 'uint32',
        bool: 'int32',   // rvdl 无 bool 类型，映射为 int32（true→1, false→0）
        float: 'double',
        angle: 'double',
    };

    private isClassicalTypeStart(): boolean {
        const t = this.peek();
        return t.kind === 'ident' && ['int', 'uint', 'bool', 'float', 'angle'].includes(t.value);
    }

    private parseClassicalDecl(): void {
        const typeTok = this.expectIdent();
        const type = Qasm3Parser.CLASSICAL_TYPES[typeTok] ?? 'int32';
        // 可选的位宽 [N]
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            this.next();
            this.next(); // N
            this.eatPunct(']');
        }
        const name = this.expectIdent();
        let init = '0';
        if (this.peek().kind === 'punct' && this.peek().value === '=') {
            this.next();
            init = this.parseExpressionString();
        }
        this.eatPunct(';');
        this.ir.classicalVars.push({ name, type, init });
    }

    /** 经典赋值：x = x + 1; 或 c[0] = 1; */
    private parseClassicalAssign(): Op[] {
        let name = this.expectIdent();
        if (this.peek().kind === 'punct' && this.peek().value === '[') {
            // bit 数组元素：c[0] → c<全局索引>
            this.next();
            const idx = Number(this.next().value);
            this.eatPunct(']');
            name = `c${this.resolveC(name, idx)}`;
        }
        this.eatPunct('=');
        const value = this.parseExpressionString();
        this.eatPunct(';');
        return [{ kind: 'classicalAssign', name, value }];
    }

    /**
     * 解析布尔/算术表达式，还原为 rvdl 表达式字符串。
     * bit 引用 c[i] → c<全局索引>，qubit 引用 q[i] → q<全局索引>，true/false → 1/0。
     */
    private parseExpressionString(): string {
        const parts: string[] = [];
        while (this.peek().kind !== 'eof' &&
               !(this.peek().kind === 'punct' && (this.peek().value === ';' || this.peek().value === ')' || this.peek().value === '}'))) {
            const t = this.next();
            if (t.kind === 'ident') {
                if (this.peek().kind === 'punct' && this.peek().value === '[') {
                    this.next(); // '['
                    const idx = this.parseIndex();
                    this.eatPunct(']');
                    if (this.cregs.has(t.value)) {
                        parts.push(`c${this.cregs.get(t.value)!.offset + idx}`);
                    } else if (this.qregs.has(t.value)) {
                        parts.push(`q${this.qregs.get(t.value)!.offset + idx}`);
                    } else {
                        parts.push(`${t.value}[${idx}]`);
                    }
                } else if (t.value === 'true') {
                    parts.push('1');
                } else if (t.value === 'false') {
                    parts.push('0');
                } else {
                    parts.push(t.value);
                }
            } else {
                parts.push(t.value);
            }
        }
        return parts.join(' ');
    }

    /** 解析语句块直到 '}'，返回 Op[]。 */
    private parseBlock(): Op[] {
        const out: Op[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === '}')) {
            out.push(...this.parseStatement());
        }
        return out;
    }

    /** 解析一个 qubit/bit 引用（name + 下标，下标可为循环变量） */
    private parseArgRef(): { name: string; idx: number } {
        const name = this.expectIdent();
        this.eatPunct('[');
        const idx = this.parseIndex();
        this.eatPunct(']');
        return { name, idx };
    }

    /** 解析下标：数字或循环变量名 */
    private parseIndex(): number {
        const t = this.next();
        if (t.kind === 'number') return Number(t.value);
        if (t.kind === 'ident') {
            if (this.loopVar && this.loopVar.name === t.value) return this.loopVar.value;
            throw new Error(`[rvdl migrate:qasm3] Unknown variable '${t.value}' in index`);
        }
        throw new Error(`[rvdl migrate:qasm3] Expected index at line ${t.line}`);
    }

    /** 解析门调用，返回一条 Op。resolveQ 为 qubit 地址解析，localQubits 为 gate 定义体形参名。 */
    private parseGateCall(
        resolveQ: (n: string, i: number) => number,
        localQubits: string[] | null,
    ): Op {
        const name = this.expectIdent();

        const params: (number | string)[] = [];
        if (this.peek().kind === 'punct' && this.peek().value === '(') {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                const p = this.next();
                if (p.kind === 'number') params.push(Number(p.value));
                else if (p.kind === 'ident') params.push(p.value);
                else throw new Error(`[rvdl migrate:qasm3] Unexpected gate parameter at line ${p.line}`);
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(')');
        }

        const qubits: number[] = [];
        while (this.peek().kind !== 'punct' || this.peek().value !== ';') {
            if (localQubits) {
                qubits.push(localQubits.indexOf(this.expectIdent()));
            } else {
                const q = this.parseArgRef();
                qubits.push(resolveQ(q.name, q.idx));
            }
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(';');
        return gate(name, qubits, params);
    }

    /** 自定义 gate 定义 */
    private parseGateDef(): GateDef {
        this.next(); // 'gate'
        const name = this.expectIdent();

        const angleParams: string[] = [];
        if (this.peek().kind === 'punct' && this.peek().value === '(') {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                angleParams.push(this.expectIdent());
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(')');
        }

        const qubitParams: string[] = [];
        while (this.peek().kind === 'ident') {
            qubitParams.push(this.expectIdent());
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }

        this.eatPunct('{');
        const body: Op[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === '}')) {
            body.push(this.parseGateCall(() => 0, qubitParams));
        }
        this.eatPunct('}');

        return { name, qubitParams, angleParams, body };
    }
}

/** 解析 OpenQASM 3.0 源码为 CircuitIR */
export function parseQasm3(src: string): CircuitIR {
    return new Qasm3Parser(src).parse();
}