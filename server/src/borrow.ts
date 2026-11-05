/**
 * 借用检查器+ 量子线性类型（QLT）
 *
 * 设计目标（刻意比 rustc 保守，规避rustc可靠性缺陷）：
 *  - 生命周期不采用 NLL 的约束求解（那是 rustc 出不可靠性 bug 的重灾区），
 *    而是用"借用活跃区间 = 程序点集合"的子集式（subset）思想。
 *  - v1 明确**不支持** implied bounds / trait object 生命周期推断 / 关联类型
 *    上的生命周期绑定，这些在 RVDL 语义层尚未出现，暂不引入。
 *
 * 覆盖两类检查：
 *  1. 量子线性类型 QLT：
 *     - Qubit / QObject 受 no-cloning 约束，不可复制。
 *     - 每个线性局部变量必须在每条执行路径上**恰好**被消费一次
 *       （消费 = measure / release / 按值 move）。未消费 -> E-Q002 泄漏；
 *       重复消费 -> E-Q001 不可克隆。
 *  2. 借用冲突（v2：带 loan 活跃区间的完整检查）：
 *     - 持久借用 `&x`（以及未来的 `&mut x`）产生 loan；loan 从产生点活跃到
 *       被借用的引用（承载它的局部）最后一次使用，由 liveness 刻画。
 *     - 同一 place 的 loan 活跃区间重叠且至少一个为可变借用 -> E0499。
 *     - loan 活跃时对该 place 写入/消费 -> E0506。
 *     - 量子门实参（即用即止的 Ref 操作数）仍做语句内冲突检查。
 */

import {
    MirBody,
    MirBasicBlock,
    MirStatement,
    MirTerminator,
    MirOperand,
    MirPlace,
    MirRvalue,
    LocalId,
    BlockId,
} from './mir';

export interface BorrowError {
    code: string;
    message: string;
    line: number;
    column: number;
}

/**
 * 持久借用 loan：由 `&x`（以及未来的 `&mut x`）产生。
 *   place —— 被借用的根局部变量
 *   temp  —— 承载该借用的临时局部（`tmp = Ref place`）
 *   loan 的活跃区间 = temp 的 liveness 区间（Polonius 子集式：生命周期 = 程序点集合）
 */
interface Loan {
    place: LocalId;
    mut: boolean;
    temp: LocalId;
}

// ---------------------------------------------------------------------------
// CFG 工具
// ---------------------------------------------------------------------------

function successors(b: MirBasicBlock): BlockId[] {
    const t = b.terminator;
    if (!t) return [];
    switch (t.kind) {
        case 'Goto':      return [t.target];
        case 'SwitchInt': return t.targets.map(x => x.target);
        case 'Return':
        case 'Unreachable': return [];
        case 'Call':      return [t.next];
    }
}

function buildPredecessors(body: MirBody): BlockId[][] {
    const preds: BlockId[][] = body.blocks.map(() => []);
    for (const b of body.blocks) {
        for (const s of successors(b)) {
            preds[s].push(b.id);
        }
    }
    return preds;
}

// ---------------------------------------------------------------------------
// 占位收集：一个语句 / 右值 / 操作数分别"读、移动、借用"了哪些 place
// ---------------------------------------------------------------------------

type Access = { place: MirPlace; kind: 'copy' | 'move' | 'borrow'; mut: boolean };

function operandAccess(op: MirOperand): Access[] {
    switch (op.kind) {
        case 'Const': return [];
        case 'Ref':   return [{ place: op.place, kind: 'borrow', mut: op.mut }];
        case 'Copy':  return [{ place: op.place, kind: 'copy', mut: false }];
        case 'Move':  return [{ place: op.place, kind: 'move', mut: false }];
    }
}

function rvalueAccess(rv: MirRvalue): Access[] {
    switch (rv.kind) {
        case 'Use':          return operandAccess(rv.operand);
        case 'Ref':          return [{ place: rv.place, kind: 'borrow', mut: rv.mut }];
        case 'Binary':       return [...operandAccess(rv.lhs), ...operandAccess(rv.rhs)];
        case 'Unary':        return operandAccess(rv.arg);
        case 'Call':         return rv.args.flatMap(operandAccess);
        case 'NewObject':    return rv.args.flatMap(operandAccess);
        case 'Member':       return operandAccess(rv.object);
        case 'Asm':          return [];
        case 'Array':        return rv.elements.flatMap(operandAccess);
        case 'ArrayIndex':   return [...operandAccess(rv.object), ...operandAccess(rv.index)];
        case 'QAlloc':       return [];
        case 'QGate':        return rv.args.flatMap(operandAccess);
        case 'QMeasure':     return operandAccess(rv.arg);
    }
}

function statementAccess(stmt: MirStatement): Access[] {
    switch (stmt.kind) {
        case 'Assign':   return rvalueAccess(stmt.rvalue);
        case 'Drop':     return [{ place: stmt.place, kind: 'move', mut: false }];
        case 'Consume':  return [{ place: stmt.place, kind: 'move', mut: false }];
    }
}

function rootLocal(p: MirPlace): LocalId {
    return p.local;
}

/** 语句"读取"的局部变量集合（Copy/Move/Ref 均为使用点） */
function usesOf(stmt: MirStatement): LocalId[] {
    return statementAccess(stmt).map(a => a.place.local);
}

/** 语句"定义"的局部变量（仅 Assign 的 place） */
function defsOf(stmt: MirStatement): LocalId[] {
    if (stmt.kind === 'Assign') return [stmt.place.local];
    return [];
}

/** 语句"破坏性写入/消费"的局部变量（assign/drop/线性消费都视为破坏） */
function writesOf(stmt: MirStatement): LocalId[] {
    if (stmt.kind === 'Assign' || stmt.kind === 'Drop' || stmt.kind === 'Consume') {
        return [stmt.place.local];
    }
    return [];
}

// ---------------------------------------------------------------------------
// 检查器
// ---------------------------------------------------------------------------

export class BorrowChecker {
    private body: MirBody;
    private errors: BorrowError[] = [];

    constructor(body: MirBody) {
        this.body = body;
    }

    check(): BorrowError[] {
        this.checkLinearity();
        this.checkBorrows();
        return this.errors;
    }

    private err(code: string, message: string, local: LocalId): void {
        const info = this.body.locals[local];
        this.errors.push({
            code,
            message,
            line: info.line,
            column: info.column,
        });
    }

    // ------------------------------------------------------------------
    // 量子线性类型 QLT（前向 may 分析，跨块）
    // ------------------------------------------------------------------
    private checkLinearity(): void {
        const n = this.body.blocks.length;
        const preds = buildPredecessors(this.body);

        // live[b] = 到达块 b 入口时"可能仍存活、未被消费"的线性局部变量集合
        const live: Set<LocalId>[] = this.body.blocks.map(() => new Set());
        const consumed: Set<LocalId>[] = this.body.blocks.map(() => new Set());

        // 初始：所有线性局部都"未消费"？不——局部变量在首次 QAlloc 赋值前并不存在。
        // 用前向数据流：QAlloc 把局部加入 live；消费将其移除。
        // 工作队列迭代到不动点。
        const work = new Set<BlockId>([0]);
        let iterations = 0;
        const maxIter = n * 4 + 16;

        while (work.size > 0 && iterations++ < maxIter) {
            const id = work.values().next().value as BlockId;
            work.delete(id);

            const inLive = new Set<LocalId>();
            const inConsumed = new Set<LocalId>();
            if (id === 0) {
                // 入口：无线性局部存活
            } else {
                for (const p of preds[id]) {
                    for (const l of live[p]) inLive.add(l);
                    for (const l of consumed[p]) inConsumed.add(l);
                }
            }

            const curLive = new Set(inLive);
            const curConsumed = new Set(inConsumed);

            for (const stmt of this.body.blocks[id].statements) {
                for (const acc of statementAccess(stmt)) {
                    if (!this.isLinear(acc.place.local)) continue;
                    if (acc.kind === 'borrow') {
                        // 借用不消费；借用本身由 checkBorrows 处理
                        continue;
                    }
                    // copy / move 都是线性类型的消费点
                    if (curConsumed.has(acc.place.local)) {
                        this.err('E-Q001',
                            `Quantum linearity violation: '${this.name(acc.place.local)}' was already consumed; Qubit/QObject cannot be cloned or measured twice.`,
                            acc.place.local);
                    }
                    curConsumed.add(acc.place.local);
                    curLive.delete(acc.place.local);
                }
                // 写入一个线性局部 = 该局部获得一个（新的或移动来的）值，变为存活。
                // 统一覆盖 QAlloc（tmp = QAlloc）与 move（q = Copy(tmp)）两种形态。
                if (stmt.kind === 'Assign' && this.isLinear(stmt.place.local)) {
                    curLive.add(stmt.place.local);
                    curConsumed.delete(stmt.place.local);
                }
            }

            // 终点检查：Return 时仍有未消费的线性局部 -> 泄漏
            const term = this.body.blocks[id].terminator;
            if (term && term.kind === 'Return') {
                // return 值 = move 出去（消费返回值），从 curLive 移除，不算泄漏
                if (term.value) {
                    for (const acc of operandAccess(term.value)) {
                        if (acc.kind === 'borrow') continue;
                        curLive.delete(acc.place.local);
                        curConsumed.add(acc.place.local);
                    }
                }
                for (const l of curLive) {
                    // 具体量子类型（QuantumRegister/DiracState/BellState）在 C++ 侧析构时
                    // 自动 measure+release_qubit，且 IR 层 emitScopeCleanup 会生成
                    // rvdl_release_object，故允许隐式 Drop（不报 E-Q002 泄漏）。
                    // 仅 Qubit / QObject 裸类型强制显式消费（measure/release/move）。
                    const ty = this.body.locals[l].ty;
                    if (ty === 'QuantumRegister' || ty === 'DiracState' || ty === 'BellState')
                        continue;
                    this.err('E-Q002',
                        `Quantum linearity violation: '${this.name(l)}' is allocated but never measured or released on this path.`,
                        l);
                }
            }

            // 收敛判断：若 out 状态没变，跳过后继
            const outChanged =
                !sameSet(curLive, live[id]) || !sameSet(curConsumed, consumed[id]);
            live[id] = curLive;
            consumed[id] = curConsumed;

            if (outChanged) {
                for (const s of successors(this.body.blocks[id])) {
                    work.add(s);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 借用冲突（loan 活跃区间检查，Polonius 子集式）
    //
    //    生命周期不采用 NLL 的约束求解，而是"借用活跃区间 = 程序点集合"：
    //    一个持久 loan（`tmp = &x`）从产生点开始，活跃到 tmp 的最后一次使用
    //    （由 tmp 的 liveness 刻画）。跨语句、跨基本块。
    // ------------------------------------------------------------------
    private checkBorrows(): void {
        // 收集持久 loans
        const loans = this.collectLoans();
        const loanByTemp = new Map<LocalId, Loan>();
        for (const l of loans) loanByTemp.set(l.temp, l);

        // 块级 liveness（后向 may 分析）
        const { liveOut } = this.computeLiveness();

        // 反向扫描每个块，逐语句维护 live 集合，据此判定活跃 loans 与冲突
        for (const block of this.body.blocks) {
            const live = new Set<LocalId>(liveOut[block.id]);

            for (let i = block.statements.length - 1; i >= 0; --i) {
                const stmt = block.statements[i];

                // 活跃 loans：temp 在 live 中的 loan（即该点之后仍被使用）
                const activeByPlace = new Map<LocalId, { mut: number; shared: number }>();
                for (const l of live) {
                    const loan = loanByTemp.get(l);
                    if (!loan) continue;
                    const e = activeByPlace.get(loan.place) ?? { mut: 0, shared: 0 };
                    if (loan.mut) e.mut++; else e.shared++;
                    activeByPlace.set(loan.place, e);
                }

                this.checkLoanConflicts(stmt, activeByPlace);
                this.checkIntraStatement(stmt);

                // 更新 live（反向 liveness：live = (live - def) ∪ use）
                for (const d of defsOf(stmt)) live.delete(d);
                for (const u of usesOf(stmt)) live.add(u);
            }
        }
    }

    /** 收集持久 loans：`tmp = Ref place`（含未来的 &mut，通过 Ref.mut 区分） */
    private collectLoans(): Loan[] {
        const loans: Loan[] = [];
        for (const block of this.body.blocks) {
            for (const stmt of block.statements) {
                if (stmt.kind === 'Assign' && stmt.rvalue.kind === 'Ref') {
                    loans.push({
                        place: rootLocal(stmt.rvalue.place),
                        mut: stmt.rvalue.mut,
                        temp: stmt.place.local,
                    });
                }
            }
        }
        return loans;
    }

    /** 块级 liveness：liveIn[B] = use[B] ∪ (liveOut[B] - def[B]) */
    private computeLiveness(): { liveIn: Set<LocalId>[]; liveOut: Set<LocalId>[] } {
        const n = this.body.blocks.length;
        const use: Set<LocalId>[] = this.body.blocks.map(() => new Set());
        const def: Set<LocalId>[] = this.body.blocks.map(() => new Set());

        for (const b of this.body.blocks) {
            const defSoFar = new Set<LocalId>();
            for (const stmt of b.statements) {
                for (const u of usesOf(stmt)) {
                    if (!defSoFar.has(u)) use[b.id].add(u);
                }
                for (const d of defsOf(stmt)) {
                    def[b.id].add(d);
                    defSoFar.add(d);
                }
            }
        }

        const liveIn: Set<LocalId>[] = this.body.blocks.map(() => new Set());
        const liveOut: Set<LocalId>[] = this.body.blocks.map(() => new Set());

        let changed = true;
        while (changed) {
            changed = false;
            for (const b of this.body.blocks) {
                const out = new Set<LocalId>();
                for (const s of successors(b)) {
                    for (const l of liveIn[s]) out.add(l);
                }
                const inSet = new Set(use[b.id]);
                for (const l of out) {
                    if (!def[b.id].has(l)) inSet.add(l);
                }
                if (!sameSet(inSet, liveIn[b.id]) || !sameSet(out, liveOut[b.id])) {
                    liveIn[b.id] = inSet;
                    liveOut[b.id] = out;
                    changed = true;
                }
            }
        }
        return { liveIn, liveOut };
    }

    /**
     * 检查"活跃持久 loans"与当前语句的冲突：
     *   - 活跃 loans 彼此冲突（同 place 两个 mut / mut+shared）
     *   - 语句破坏性写入（assign/drop/consume）被活跃 loan 占用的 place
     *   - 语句自身的临时借用（量子门实参）与活跃 loan 冲突
     */
    private checkLoanConflicts(
        stmt: MirStatement,
        activeByPlace: Map<LocalId, { mut: number; shared: number }>,
    ): void {
        for (const [place, c] of activeByPlace) {
            if (c.mut >= 2) {
                this.err('E0499',
                    `cannot borrow '${this.name(place)}' as mutable more than once at the same time.`, place);
            }
            if (c.mut >= 1 && c.shared >= 1) {
                this.err('E0499',
                    `cannot borrow '${this.name(place)}' as mutable and immutable at the same time.`, place);
            }
        }

        for (const p of writesOf(stmt)) {
            if (activeByPlace.has(p)) {
                this.err('E0506',
                    `cannot assign to '${this.name(p)}' while it is borrowed.`, p);
            }
        }

        // 持久 loan 产生点本身（`tmp = Ref`）不在此处算作临时借用
        const isPersistentRef = stmt.kind === 'Assign' && stmt.rvalue.kind === 'Ref';
        for (const a of statementAccess(stmt)) {
            if (a.kind !== 'borrow' || isPersistentRef) continue;
            const p = rootLocal(a.place);
            const e = activeByPlace.get(p);
            if (e && (a.mut || e.mut >= 1)) {
                this.err('E0499',
                    `cannot borrow '${this.name(p)}' while it is already borrowed.`, p);
            }
        }
    }

    /** 语句内冲突检查：同一语句内的多个可变借用 / mut+shared / 借用+写入 */
    private checkIntraStatement(stmt: MirStatement): void {
        const mutBorrows = new Map<LocalId, number>();
        const sharedBorrows = new Map<LocalId, number>();
        const writes = new Map<LocalId, number>();

        // 持久 loan 产生点（`tmp = Ref`）由 loan 检查处理，语句内跳过其 borrow
        const isPersistentRef = stmt.kind === 'Assign' && stmt.rvalue.kind === 'Ref';

        for (const a of statementAccess(stmt)) {
            const r = rootLocal(a.place);
            if (a.kind === 'borrow') {
                if (isPersistentRef) continue;
                const m = a.mut ? mutBorrows : sharedBorrows;
                m.set(r, (m.get(r) ?? 0) + 1);
            }
        }

        if (stmt.kind === 'Assign' || stmt.kind === 'Drop' || stmt.kind === 'Consume') {
            const r = rootLocal(stmt.place);
            writes.set(r, (writes.get(r) ?? 0) + 1);
        }

        for (const [r, c] of mutBorrows) {
            if (c > 1) {
                this.err('E0499',
                    `cannot borrow '${this.name(r)}' as mutable more than once at the same time.`, r);
            }
            if ((sharedBorrows.get(r) ?? 0) > 0) {
                this.err('E0499',
                    `cannot borrow '${this.name(r)}' as mutable and immutable at the same time.`, r);
            }
            if ((writes.get(r) ?? 0) > 0) {
                this.err('E0506',
                    `cannot assign to '${this.name(r)}' while it is mutably borrowed.`, r);
            }
        }
        for (const [r] of sharedBorrows) {
            if ((writes.get(r) ?? 0) > 0) {
                this.err('E0506',
                    `cannot assign to '${this.name(r)}' while it is borrowed as immutable.`, r);
            }
        }
    }

    private isLinear(id: LocalId): boolean {
        return this.body.locals[id].linear;
    }

    private name(id: LocalId): string {
        return this.body.locals[id].name ?? `<temp${id}>`;
    }
}

function sameSet(a: Set<LocalId>, b: Set<LocalId>): boolean {
    if (a.size !== b.size) return false;
    for (const x of a) if (!b.has(x)) return false;
    return true;
}