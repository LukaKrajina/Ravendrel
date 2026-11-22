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






interface Loan {
    place: LocalId;
    mut: boolean;
    temp: LocalId;
}


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

function usesOf(stmt: MirStatement): LocalId[] {
    return statementAccess(stmt).map(a => a.place.local);
}

function defsOf(stmt: MirStatement): LocalId[] {
    if (stmt.kind === 'Assign') return [stmt.place.local];
    return [];
}

function writesOf(stmt: MirStatement): LocalId[] {
    if (stmt.kind === 'Assign' || stmt.kind === 'Drop' || stmt.kind === 'Consume') {
        return [stmt.place.local];
    }
    return [];
}


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

    private checkLinearity(): void {
        const n = this.body.blocks.length;
        const preds = buildPredecessors(this.body);

        const live: Set<LocalId>[] = this.body.blocks.map(() => new Set());
        const consumed: Set<LocalId>[] = this.body.blocks.map(() => new Set());

        const work = new Set<BlockId>([0]);
        let iterations = 0;
        const maxIter = n * 4 + 16;

        while (work.size > 0 && iterations++ < maxIter) {
            const id = work.values().next().value as BlockId;
            work.delete(id);

            const inLive = new Set<LocalId>();
            const inConsumed = new Set<LocalId>();
            if (id === 0) {
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
                        continue;
                    }
                    if (curConsumed.has(acc.place.local)) {
                        this.err('E-Q001',
                            `Quantum linearity violation: '${this.name(acc.place.local)}' was already consumed; Qubit/QObject cannot be cloned or measured twice.`,
                            acc.place.local);
                    }
                    curConsumed.add(acc.place.local);
                    curLive.delete(acc.place.local);
                }
                if (stmt.kind === 'Assign' && this.isLinear(stmt.place.local)) {
                    curLive.add(stmt.place.local);
                    curConsumed.delete(stmt.place.local);
                }
            }

            const term = this.body.blocks[id].terminator;
            if (term && term.kind === 'Return') {
                if (term.value) {
                    for (const acc of operandAccess(term.value)) {
                        if (acc.kind === 'borrow') continue;
                        curLive.delete(acc.place.local);
                        curConsumed.add(acc.place.local);
                    }
                }
                for (const l of curLive) {
                    const ty = this.body.locals[l].ty;
                    if (ty === 'QuantumRegister' || ty === 'DiracState' || ty === 'BellState')
                        continue;
                    this.err('E-Q002',
                        `Quantum linearity violation: '${this.name(l)}' is allocated but never measured or released on this path.`,
                        l);
                }
            }

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

    private checkBorrows(): void {
        const loans = this.collectLoans();
        const loanByTemp = new Map<LocalId, Loan>();
        for (const l of loans) loanByTemp.set(l.temp, l);

        const { liveOut } = this.computeLiveness();

        for (const block of this.body.blocks) {
            const live = new Set<LocalId>(liveOut[block.id]);

            for (let i = block.statements.length - 1; i >= 0; --i) {
                const stmt = block.statements[i];

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

                for (const d of defsOf(stmt)) live.delete(d);
                for (const u of usesOf(stmt)) live.add(u);
            }
        }
    }

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

    private checkIntraStatement(stmt: MirStatement): void {
        const mutBorrows = new Map<LocalId, number>();
        const sharedBorrows = new Map<LocalId, number>();
        const writes = new Map<LocalId, number>();

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