// ============================================================================
// qsharp.ts —— Q#（Microsoft）前端：源码 → CircuitIR
//
// 覆盖 Q# 核心量子电路子集：
//   namespace / operation / use (q0, q1) = (Qubit(), Qubit());
//   using (qs = Qubit[n]) { ... } / H(q) / CNOT(c, t) / Rx(θ, q) / M(q) / Reset(q)
//
// 注意：Q# 的旋转门参数顺序为「角度在前、qubit 在后」（Rx(θ, q)），
// 此处归一为 qelib1 风格（qubit 在前、角度在后），交 emitter 统一发射。
// Q# 的 M(q) 不消费 qubit（需 Reset 复用），迁移到 rvdl 的消费式 measure，
// 随后的 Reset 降级为注释。
// ============================================================================

import { CircuitIR, Op, emptyCircuit, gate } from './circuit-ir';

// Q# 门名 → qelib1 风格门名；带角度（θ 在前）的门在此列出
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

// 带角度参数（θ 在第一个参数）的门
const ANGLE_GATES = new Set(['Rx', 'Ry', 'Rz']);

// ─── 词法分析 ──────────────────────────────────────────────────────────────

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

// ─── 语法分析 ──────────────────────────────────────────────────────────────

/** 一个 operation 的签名与 body 的 token 区间（供内联时重放）。 */
interface OperationInfo {
    name: string;
    /** qubit 形参名（顺序对应调用实参） */
    params: string[];
    /** body 第一个 token 的下标 */
    bodyStart: number;
    /** body 收尾 '}' 的下标 */
    bodyEnd: number;
}

class QSharpParser {
    private toks: Tok[];
    private pos = 0;
    private ir: CircuitIR = emptyCircuit('qsharp');

    // qubit 变量名 → 全局索引（标量）或索引数组（using 数组）
    private qubits = new Map<string, number | number[]>();

    /** 经典名（let 绑定的测量结果）→ bit 索引，供 if/while 条件引用 */
    private classical = new Map<string, number>();

    /** 全部 operation（名字 → 签名与 body 区间） */
    private operations = new Map<string, OperationInfo>();
    /** operation 的源顺序（入口发射顺序） */
    private operationOrder: string[] = [];
    /** 被其它 operation 调用的名字（非入口） */
    private calledOps = new Set<string>();
    /** 内联栈（递归环检测 + 深度上限） */
    private inlineStack: string[] = [];

    /**
     * 指令汇栈：嵌套 body（if / while / for / using）的指令先收进自己的数组，
     * 再包成 IfOp / WhileOp / ForOp 挂回父级。曾经所有语句直接 push 进
     * ir.ops，嵌套控制流因此无法表达。
     */
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

    /** 当前指令汇（嵌套 body 的收集目标） */
    private get sink(): Op[] {
        return this.opSinks[this.opSinks.length - 1];
    }

    private pushOp(op: Op): void {
        this.sink.push(op);
    }

    parse(): CircuitIR {
        // 第一遍：登记全部 operation 的签名与 body 区间
        this.scanOperations();
        // 调用图：谁被调用 → 非入口
        this.scanCalls();
        // 入口 = 不被任何 operation 调用的 operation（源顺序）。
        // 全互相调用（成环）时退化为全部 operation —— 内联会因环检测报错，
        // 那是诚实失败，好过静默只迁第一个。
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

    /** 第一遍：扫描所有 operation 声明，记录签名与 body 的 token 区间。 */
    private scanOperations(): void {
        const saved = this.pos;
        this.pos = 0;
        while (this.peek().kind !== 'eof') {
            if (this.isIdent('operation')) {
                this.next(); // 'operation'
                const name = this.expectIdent();
                const params: string[] = [];
                if (this.peek().kind === 'punct' && this.peek().value === '(') {
                    this.next();
                    while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
                        if (this.peek().kind === 'ident') {
                            const pname = this.next().value;
                            // 跳过 ': Type'（类型可含 []）
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
                // 返回类型：跳过到 '{'（Unit / Result[] / (Result, Result) 都覆盖）
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

    /** 调用图：body 里 `Name(` 形态且 Name 是已登记的 operation → 被调用。 */
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

    /** 跳过一对平衡括号（Reset(q) / ResetAll() 等无需解析的调用）。 */
    private skipBalancedParens(): void {
        if (!(this.peek().kind === 'punct' && this.peek().value === '(')) return;
        let depth = 0;
        do {
            const t = this.next();
            if (t.kind === 'punct' && t.value === '(') depth++;
            else if (t.kind === 'punct' && t.value === ')') depth--;
        } while (depth > 0 && this.peek().kind !== 'eof');
    }

    /** 解析 operation body 的语句序列（指令进当前汇）。 */
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
            // set r = One / set r = Zero（经典结果赋值）
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
            // 忽略返回值（迁移后的 rvdl_main 统一 return 0）
            this.next();
            while (!(this.peek().kind === 'punct' && this.peek().value === ';')) this.next();
            this.eatPunct(';');
            return;
        }
        // 门调用（H(q) / CNOT(c,t) / Rx(θ,q) 等）或自定义 operation 调用（内联）
        this.parseGateCall();
    }

    /** use (q0, q1) = (Qubit(), Qubit()); */
    private parseUse(): void {
        this.next(); // 'use'
        this.eatPunct('(');
        const names: string[] = [];
        while (!(this.peek().kind === 'punct' && this.peek().value === ')')) {
            names.push(this.expectIdent());
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct('=');
        this.eatPunct('(');
        // 每个 Qubit() 分配一个全局 qubit
        for (const name of names) {
            this.expectIdent(); // 'Qubit'
            this.eatPunct('(');
            this.eatPunct(')');
            this.qubits.set(name, this.ir.qubitCount++);
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');
    }

    /** using (qs = Qubit[n]) { ... } */
    private parseUsing(): void {
        this.next(); // 'using'
        this.eatPunct('(');
        const name = this.expectIdent();
        this.eatPunct('=');
        this.expectIdent(); // 'Qubit'
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

    /** let r = M(q); → 测量（映射为 measure(q)），并把 r 登记为经典别名。
     *  元组形态 let (r0, r1) = (M(q0), M(q1)); 同样支持。 */
    private parseLet(): void {
        this.next(); // 'let'
        // 名字：单个或元组
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

        // 右侧：单个 M(q) 或元组 (M(q0), M(q1))
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
                // 其它 let 表达式：跳过
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

    /** for i in 0..n-1 { ... }（循环展开） */
    private parseFor(): void {
        this.next(); // 'for'
        const varName = this.expectIdent();
        this.expectIdent(); // 'in'
        const lo = Number(this.next().value);
        this.eatPunct('..');
        // 上界可能是 n-1 形式（ident - number）
        const hiTok = this.next();
        let hi: number;
        if (hiTok.kind === 'number') hi = Number(hiTok.value);
        else if (hiTok.kind === 'ident') {
            const n = this.qubits.get(hiTok.value);
            if (Array.isArray(n)) hi = n.length;
            else hi = Number(hiTok.value); // 回退
        } else hi = 0;
        // 处理 `n - 1` 的 `- 1`
        if (this.peek().kind === 'punct' && this.peek().value === '-') {
            this.next();
            const k = Number(this.next().value);
            hi = hi - k;
        }

        this.eatPunct('{');

        // 循环展开：替换 qs[i] 下标里的循环变量
        const saved = this.loopVar;
        for (let i = lo; i < hi; i++) {
            this.loopVar = { name: varName, value: i };
            this.parseBody();
        }
        this.loopVar = saved;

        this.eatPunct('}');
    }

    private loopVar: { name: string; value: number } | null = null;

    /** if (cond) { ... } [else { ... } | else if ...] —— 经典控制流。
     *  条件经 parseCondition 翻译为 rvdl 布尔表达式（One→1、经典名→c<bit>、
     *  and/or/not→&&/||/!），包成 IfOp 交 emitter。 */
    private parseIf(): void {
        this.next(); // 'if'
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
                // else if 链：嵌套 if 整体作为 alternate
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

    /** while (cond) { ... } —— 经典循环（条件同上）。 */
    private parseWhile(): void {
        this.next(); // 'while'
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

    // ── 条件表达式：Q# 布尔式 → rvdl 布尔式 ──────────────────────────────
    // 优先级：or > and > not > 比较 > 初等。One/Zero/true/false 归一为
    // 1/0（rvdl 的 int32 条件语义），经典名映射到 c<bit>。

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
            // 函数调用形态（Length(qs) 等）：明确报错，不要产出无效 rvdl
            const nx = this.toks[this.pos];
            if (nx && nx.kind === 'punct' && nx.value === '(') {
                throw new Error(
                    `[qsharp] function call '${t.value}(...)' in a condition is not supported `
                    + `(line ${t.line}); only classical values, comparisons and and/or/not are`);
            }
            // 未知标识符原样透传（由 rvdl 编译器给出最终诊断）
            return t.value;
        }
        throw new Error(`[qsharp] Unexpected token '${t.value}' in condition at line ${t.line}`);
    }

    /** 解析门调用：H(q) / CNOT(c,t) / Rx(θ,q)；自定义 operation 调用则内联。 */
    private parseGateCall(): void {
        const gateName = this.expectIdent();
        const mapped = QSHARP_GATE_MAP[gateName];
        if (!mapped) {
            // 自定义 operation 调用 → 内联其 body（形参绑定到实参 qubit）
            if (this.operations.has(gateName)) {
                this.inlineOperation(gateName);
                return;
            }
            // 未知标识符（可能是表达式）：跳过语句
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
                // 角度在第一个参数
                if (t.kind === 'number') args.push(Number(t.value));
                else if (t.kind === 'ident') args.push(t.value);
            } else {
                // qubit 引用
                if (t.kind === 'ident') qubits.push(this.resolveQubit(t.value));
            }
            idx++;
            if (this.peek().kind === 'punct' && this.peek().value === ',') this.next();
        }
        this.eatPunct(')');
        this.eatPunct(';');

        this.pushOp(gate(mapped, qubits, args));
    }

    /**
     * 内联一个自定义 operation：形参 qubit 绑定到实参，body 在调用点重放。
     *
     * Q# 的 operation 是过程而非数据流 —— 迁移到 rvdl 的平坦电路必须内联。
     * 曾经的实现只迁**第一个** operation，其余静默丢弃：helper + main 的
     * 常见结构会迁出完全错误的电路（只剩 helper），且无任何报错。
     *
     * 语义要点：
     *   · 形参绑定：调用点的实参 qubit 替换形参名（qs[i] 下标同样解析）；
     *   · 被调 operation 自己的 use/using 分配**新** qubit —— 这与 Q# 语义
     *     一致（每次调用分配新 qubit），不是近似；
     *   · 递归环检测 + 深度上限 32：环无法摊平，诚实报错。
     */
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

        // 保存现场（qubit / 经典名 / 位置），绑定形参后重放 body
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
        // qs[i] 数组下标（含循环变量）
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

/** 解析 Q# 源码为 CircuitIR */
export function parseQsharp(src: string): CircuitIR {
    return new QSharpParser(src).parse();
}