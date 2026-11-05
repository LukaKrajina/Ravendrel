// ============================================================================
// qasm2.ts —— OpenQASM 2.0 前端：源码 → CircuitIR
//
// 覆盖 OpenQASM 2.0 的核心子集：
//   OPENQASM 2.0; / include "qelib1.inc";
//   qreg / creg 声明、内建门调用、参数化门（u1/u2/u3/rx/ry/rz/crx/cry/...）、
//   measure / barrier / reset / if（单条量子操作条件执行）、自定义 gate 定义。
//
// 由于 Qiskit / Cirq / pyQuil / ProjectQ 都能导出 OpenQASM 2.0，
// 前端即可传递覆盖绝大多数 Python 系量子框架。
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

        // 空白
        if (ch === ' ' || ch === '\t' || ch === '\r') { i++; continue; }
        if (ch === '\n') { line++; i++; continue; }

        // 注释
        if (ch === '/' && src[i + 1] === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }

        // 字符串
        if (ch === '"') {
            let j = i + 1;
            while (j < n && src[j] !== '"') j++;
            toks.push({ kind: 'string', value: src.slice(i + 1, j), line });
            i = j + 1;
            continue;
        }

        // 数字（含小数点、指数）
        if (/[0-9]/.test(ch)) {
            let j = i;
            while (j < n && /[0-9.eE+-]/.test(src[j])) j++;
            toks.push({ kind: 'number', value: src.slice(i, j), line });
            i = j;
            continue;
        }

        // 标识符 / 关键字（含 pi 常量）
        if (/[A-Za-z_]/.test(ch)) {
            let j = i;
            while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
            toks.push({ kind: 'ident', value: src.slice(i, j), line });
            i = j;
            continue;
        }

        // 标点
        if ('[]();,{}->=+*/'.includes(ch)) {
            toks.push({ kind: 'punct', value: ch, line });
            i++;
            continue;
        }

        throw new Error(`[rvdl migrate:qasm2] Unexpected character '${ch}' at line ${line}`);
    }

    toks.push({ kind: 'eof', value: '', line });
    return toks;
}

// ─── 语法分析 ──────────────────────────────────────────────────────────────

class Qasm2Parser {
    private toks: Tok[];
    private pos = 0;

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
            throw new Error(`[rvdl migrate:qasm2] Expected '${v}' at line ${t.line}, got '${t.value}'`);
        }
    }

    private expectIdent(): string {
        const t = this.next();
        if (t.kind !== 'ident') throw new Error(`[rvdl migrate:qasm2] Expected identifier at line ${t.line}`);
        return t.value;
    }

    parse(): CircuitIR {
        const ir = emptyCircuit('qasm2');

        // 寄存器名 → { offset, size }
        const qregs = new Map<string, { offset: number; size: number }>();
        const cregs = new Map<string, { offset: number; size: number }>();

        const resolveQ = (name: string, idx: number): number => {
            const r = qregs.get(name);
            if (!r) throw new Error(`[rvdl migrate:qasm2] Undefined qreg '${name}'`);
            if (idx >= r.size) throw new Error(`[rvdl migrate:qasm2] Qubit index ${idx} out of range for qreg '${name}'`);
            return r.offset + idx;
        };
        const resolveC = (name: string, idx: number): number => {
            const r = cregs.get(name);
            if (!r) throw new Error(`[rvdl migrate:qasm2] Undefined creg '${name}'`);
            if (idx >= r.size) throw new Error(`[rvdl migrate:qasm2] Bit index ${idx} out of range for creg '${name}'`);
            return r.offset + idx;
        };

        while (this.peek().kind !== 'eof') {
            const t = this.peek();

            if (t.kind === 'ident' && t.value === 'OPENQASM') {
                this.next();
                if (this.peek().kind === 'number') this.next();
                this.eatPunct(';');
                continue;
            }
            if (this.isIdent('include')) {
                this.next();
                this.next(); // 字符串路径
                this.eatPunct(';');
                continue;
            }
            if (this.isIdent('opaque')) {
                // 不透明声明（无定义体的门）：跳过到分号
                while (this.peek().kind !== 'punct' || this.peek().value !== ';') this.next();
                this.eatPunct(';');
                continue;
            }
            if (this.isIdent('qreg')) {
                this.next();
                const name = this.expectIdent();
                this.eatPunct('[');
                const size = Number(this.next().value);
                this.eatPunct(']');
                this.eatPunct(';');
                qregs.set(name, { offset: ir.qubitCount, size });
                ir.qubitCount += size;
                continue;
            }
            if (this.isIdent('creg')) {
                this.next();
                const name = this.expectIdent();
                this.eatPunct('[');
                const size = Number(this.next().value);
                this.eatPunct(']');
                this.eatPunct(';');
                cregs.set(name, { offset: ir.bitCount, size });
                ir.bitCount += size;
                continue;
            }
            if (this.isIdent('gate')) {
                ir.gateDefs.push(this.parseGateDef());
                continue;
            }
            if (this.isIdent('measure')) {
                this.next();
                const q = this.parseArgRef();
                this.eatPunct('-');
                this.eatPunct('>');
                const c = this.parseArgRef();
                this.eatPunct(';');
                ir.ops.push({ kind: 'measure', qubit: resolveQ(q.name, q.idx), bit: resolveC(c.name, c.idx) });
                continue;
            }
            if (this.isIdent('barrier')) {
                this.next();
                const qs: number[] = [];
                while (this.peek().kind !== 'punct' || this.peek().value !== ';') {
                    const q = this.parseArgRef();
                    qs.push(resolveQ(q.name, q.idx));
                    if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
                }
                this.eatPunct(';');
                ir.ops.push({ kind: 'barrier', qubits: qs });
                continue;
            }
            if (this.isIdent('reset')) {
                this.next();
                const q = this.parseArgRef();
                this.eatPunct(';');
                ir.ops.push({ kind: 'reset', qubit: resolveQ(q.name, q.idx) });
                continue;
            }
            if (this.isIdent('if')) {
                this.next();
                this.eatPunct('(');
                const c = this.parseArgRef();
                this.eatPunct('=');
                this.eatPunct('=');
                const value = Number(this.next().value);
                this.eatPunct(')');
                const body: Op[] = [this.parseQuantumOp(resolveQ)];
                ir.ops.push({ kind: 'if', bit: resolveC(c.name, c.idx), value, body });
                continue;
            }

            // 内建门调用（h/x/rz(...)/cx/ccx/u3(...) 等）
            ir.ops.push(this.parseQuantumOp(resolveQ));
        }

        return ir;
    }

    /** 解析一个 qubit/bit 引用（qreg 名 + 下标） */
    private parseArgRef(): { name: string; idx: number } {
        const name = this.expectIdent();
        this.eatPunct('[');
        const idx = Number(this.next().value);
        this.eatPunct(']');
        return { name, idx };
    }

    /** 解析一条量子操作（门调用），可含 if 条件（OpenQASM 2 的 if 只作用于单条 qop）。
     *  顶层 qubit 实参为 `name[idx]`；`localQubits` 非空时（gate 定义体），实参为纯形参名。 */
    private parseQuantumOp(
        resolveQ: (n: string, i: number) => number,
        localQubits: string[] | null = null,
    ): Op {
        const name = this.expectIdent();

        // 参数化门：name(params)
        const params: (number | string)[] = [];
        if (this.peek().kind === 'punct' && this.peek().value === '(') {
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                const p = this.next();
                if (p.kind === 'number') params.push(Number(p.value));
                else if (p.kind === 'ident') params.push(p.value); // pi / 参数名
                else throw new Error(`[rvdl migrate:qasm2] Unexpected gate parameter at line ${p.line}`);
                if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
            }
            this.eatPunct(')');
        }

        // qubit 实参列表
        const qubits: number[] = [];
        while (this.peek().kind !== 'punct' || this.peek().value !== ';') {
            if (localQubits) {
                const qname = this.expectIdent();
                qubits.push(localQubits.indexOf(qname));
            } else {
                const q = this.parseArgRef();
                qubits.push(resolveQ(q.name, q.idx));
            }
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(';');

        return gate(name, qubits, params);
    }

    /** 解析自定义 gate 定义（`gate name(params) qargs { body }`） */
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

        // body：qubit 引用为形参名，角度参数为形参名（string）
        const body: Op[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === '}')) {
            if (this.isIdent('barrier')) {
                this.next();
                const qs: number[] = [];
                while (this.peek().kind === 'ident') {
                    qs.push(qubitParams.indexOf(this.expectIdent()));
                    if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
                }
                this.eatPunct(';');
                body.push({ kind: 'barrier', qubits: qs });
                continue;
            }
            body.push(this.parseQuantumOp(() => 0, qubitParams));
        }
        this.eatPunct('}');

        return { name, qubitParams, angleParams, body };
    }
}

/** 解析 OpenQASM 2.0 源码为 CircuitIR */
export function parseQasm2(src: string): CircuitIR {
    return new Qasm2Parser(src).parse();
}