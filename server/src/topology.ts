import {
    Program,
    FunctionDeclaration,
    LayerTag,
    Topology,
    TopologyEdge,
    TopologyCallEdge,
    ShuttleLink,
    ShuttlePeer,
    TrackingScope,
    TrackingMode,
    ShuttleStrategy,
} from './ast';

export interface TopologyError {
    code: string;
    message: string;
    line: number;
    column: number;
}

interface Block {
    name: string;
    layer: LayerTag;
    fn: FunctionDeclaration;
}


export const TRACKABLE_OBSERVABLES: Readonly<Record<string, 'pauli' | 'classical' | 'measure'>> = {
    expect_z: 'pauli',
    expect_x: 'pauli',
    expect_y: 'pauli',
    num_qubits: 'classical',
    shannon4: 'classical',
    shannon8: 'classical',
    amplitude: 'classical',
    entropy: 'measure',
    fidelity: 'measure',
    attention: 'measure',
    measure_z: 'measure',
    measure_x: 'measure',
    measure_y: 'measure',
};

function levelOfFamily(fam: 'pauli' | 'classical' | 'measure'): number {
    if (fam === 'pauli') return 0;
    if (fam === 'classical') return 2;
    return 1;
}

const LEVEL_NAME = ['L0/qnd', 'L1/shadow', 'L2/relabel'];





const QUANTUM_PRODUCING_BUILTINS: ReadonlySet<string> = new Set([
    'alloc',
    'basis_state',
    'encode_text', 'encode_image', 'rvdl_encode_string',
    'mind_read',
    'qrc_probe', 'qrc_predict',
    'qchain_coin_mint',
]);

const EDGE_LABEL: Readonly<Record<TopologyEdge['kind'], string>> = {
    stack: '叠加(stack)',
    parallel: '平行(parallel)',
    projection: '投影(projection)',
    inversion: '反转(inversion)',
    crossing: '交叉(crossing)',
    'happens-before': '时序(happens-before)',
};


export class TopologyBuilder {
    private errors: TopologyError[] = [];



    private warnings: TopologyError[] = [];

    public build(program: Program): { topology: Topology; errors: TopologyError[]; warnings: TopologyError[] } {
        this.errors = [];
        this.warnings = [];

        const blocks: Block[] = [];

        const hasExport = program.body.some(
            n => n.type === 'FunctionDeclaration' && (n as FunctionDeclaration).isExport
        );
        if (hasExport) {
            return {
                topology: { shape: { time: 1, thread: 1, coord: [] }, blocks: [], edges: [], callGraph: [], shuttleLinks: [], trackingScopes: [] },
                errors: this.errors,
                warnings: this.warnings,
            };
        }

        for (const node of program.body) {
            if (node.type !== 'FunctionDeclaration') continue;
            const fn = node as FunctionDeclaration;
            if (fn.synthetic || fn.quantum?.isGate || fn.isExport) continue;
            if (!fn.layer) {
                this.errors.push({
                    code: 'E-TOP001',
                    message: `Topology Error: function '${fn.name}' must carry an @layer(time, thread, coord) tag.`,
                    line: fn.line,
                    column: fn.column,
                });
            } else {
                blocks.push({ name: fn.name, layer: fn.layer, fn });
            }
        }

        if (blocks.length === 0) {
            return {
                topology: { shape: { time: 1, thread: 1, coord: [] }, blocks: [], edges: [], callGraph: [], shuttleLinks: [], trackingScopes: [] },
                errors: this.errors,
                warnings: this.warnings,
            };
        }

        this.validateCoordDims(blocks);

        this.reportUnknownKeys(blocks);
        this.reportNegativeKeys(blocks);

        const shape = this.aggregateShape(blocks);

        const edges = this.deriveEdges(blocks);

        const { callGraph, finishOf } = this.buildCallGraph(blocks);

        this.checkDeadlines(blocks, finishOf);

        const shuttleLinks = this.resolveShuttleLinks(blocks, edges);

        const quantumProducers = new Set<string>();
        for (const node of program.body) {
            if (node.type !== 'FunctionDeclaration') continue;
            const fn = node as FunctionDeclaration;
            if (fn.synthetic) continue;
            const rt = (fn.returnType ?? '').trim();
            if (rt === 'Qubit' || rt === 'QObject') quantumProducers.add(fn.name);
        }
        const trackingScopes = this.buildTrackingScopes(blocks, quantumProducers);

        const topology: Topology = {
            shape,
            blocks: blocks.map(b => ({ name: b.name, layer: b.layer })),
            edges,
            callGraph,
            shuttleLinks,
            trackingScopes,
        };
        return { topology, errors: this.errors, warnings: this.warnings };
    }





    private reportUnknownKeys(blocks: Block[]): void {
        for (const b of blocks) {
            for (const uk of b.layer.unknownKeys ?? []) {
                this.errors.push({
                    code: 'E-TOP008',
                    message: `Topology Error: unknown @layer key '${uk.key}' on '${b.name}'. `
                        + `Valid keys: time, thread, coord, cost, deadline, dual, `
                        + `shuttle, shuttle_lambda, shuttle_phase, shuttle_rounds, shuttle_strategy, `
                        + `track, track_mode, track_slots.`,
                    line: uk.line,
                    column: uk.column,
                });
            }
        }
    }











    private reportNegativeKeys(blocks: Block[]): void {
        const check = (b: Block, key: string, v: number | undefined) => {
            if (v !== undefined && v < 0) {
                this.errors.push({
                    code: 'E-TOP009',
                    message: `Topology Error: @layer key '${key}' must be non-negative on `
                        + `'${b.name}' (got ${v}).`,
                    line: b.fn.line,
                    column: b.fn.column,
                });
            }
        };
        for (const b of blocks) {
            check(b, 'time', b.layer.time);
            check(b, 'thread', b.layer.thread);
            check(b, 'cost', b.layer.cost);
            check(b, 'deadline', b.layer.deadline);
        }
    }

    private validateCoordDims(blocks: Block[]): void {
        if (blocks.length < 2) return;

        let minDim = Number.POSITIVE_INFINITY;
        let maxDim = 0;
        for (const b of blocks) {
            const d = b.layer.coord.length;
            if (d < minDim) minDim = d;
            if (d > maxDim) maxDim = d;
        }

        if (maxDim - minDim > 1) {
            const low = blocks.find(b => b.layer.coord.length === minDim)!;
            this.errors.push({
                code: 'E-TOP007',
                message: `Topology Error: coord dimension spread is ${maxDim - minDim} (>1) `
                    + `(min ${minDim}D, max ${maxDim}D); projection across more than one `
                    + `dimension is not well-defined.`,
                line: low.fn.line,
                column: low.fn.column,
            });
        }
    }

    private aggregateShape(blocks: Block[]): { time: number; thread: number; coord: number[] } {
        let maxTime = 0;
        let maxThread = 0;
        const coordMax: number[] = [];
        for (const b of blocks) {
            const t = b.layer.time ?? 0;
            if (t > maxTime) maxTime = t;
            if (b.layer.thread > maxThread) maxThread = b.layer.thread;
            for (let i = 0; i < b.layer.coord.length; i++) {
                coordMax[i] = Math.max(coordMax[i] ?? 0, b.layer.coord[i]);
            }
        }
        return { time: maxTime + 1, thread: maxThread + 1, coord: coordMax.map(c => c + 1) };
    }

    private deriveEdges(blocks: Block[]): TopologyEdge[] {
        const edges: TopologyEdge[] = [];
        const key = (c: number[]) => c.join(',');

        const groups = new Map<string, Block[]>();
        for (const b of blocks) {
            const k = key(b.layer.coord);
            if (!groups.has(k)) groups.set(k, []);
            groups.get(k)!.push(b);
        }

        for (const [, group] of groups) {
            group.sort((a, b) => (a.layer.time ?? 0) - (b.layer.time ?? 0));
            for (let i = 0; i < group.length; i++) {
                for (let j = i + 1; j < group.length; j++) {
                    const a = group[i];
                    const b = group[j];
                    const ta = a.layer.time ?? 0;
                    const tb = b.layer.time ?? 0;
                    const sameDual = !!a.layer.dual === !!b.layer.dual;
                    const sameThread = a.layer.thread === b.layer.thread;

                    if (sameDual && sameThread) {
                        if (ta === tb) {
                            this.errors.push({
                                code: 'E-TOP003',
                                message: `Topology Error: duplicate occupancy at coord (${key(a.layer.coord)}), time ${ta}: '${a.name}' and '${b.name}'.`,
                                line: b.fn.line,
                                column: b.fn.column,
                            });
                        } else if (tb - ta > 1) {
                            this.errors.push({
                                code: 'E-TOP005',
                                message: `Topology Error: gap in stack chain at coord (${key(a.layer.coord)}) between time ${ta} ('${a.name}') and ${tb} ('${b.name}').`,
                                line: b.fn.line,
                                column: b.fn.column,
                            });
                        }
                        edges.push({ from: a.name, to: b.name, kind: 'stack' });
                    }
                    if (!sameDual) {
                        edges.push({ from: a.name, to: b.name, kind: 'inversion' });
                    }
                    if (!sameThread) {
                        edges.push({ from: a.name, to: b.name, kind: 'crossing' });
                    }
                }
            }
        }

        for (let i = 0; i < blocks.length; i++) {
            for (let j = i + 1; j < blocks.length; j++) {
                const A = blocks[i];
                const B = blocks[j];
                if (key(A.layer.coord) !== key(B.layer.coord)) {
                    const kind = A.layer.coord.length !== B.layer.coord.length
                        ? 'projection' : 'parallel';
                    edges.push({ from: A.name, to: B.name, kind });
                }
            }
        }

        return edges;
    }

    private buildCallGraph(blocks: Block[]): { callGraph: TopologyCallEdge[]; finishOf: Map<string, number> } {
        const callGraph: TopologyCallEdge[] = [];
        const finishOf = new Map<string, number>();

        const costOf = new Map<string, number>();
        const anchor = new Map<string, number>();
        const nameSet = new Set<string>();
        for (const b of blocks) {
            costOf.set(b.name, b.layer.cost ?? 1);
            anchor.set(b.name, b.layer.time ?? 0);
            nameSet.add(b.name);
        }

        const propagateExpr = (owner: string, expr: any, τ: number): number => {
            if (!expr) return τ;
            switch (expr.type) {
                case 'FunctionCall': {
                    for (const a of expr.arguments) τ = propagateExpr(owner, a, τ);
                    if (nameSet.has(expr.name)) {
                        callGraph.push({
                            caller: owner,
                            callee: expr.name,
                            startAt: τ,
                            deltaT: τ - (anchor.get(owner) ?? 0),
                        });
                        return τ + (costOf.get(expr.name) ?? 1);
                    }
                    return τ;
                }
                case 'BinaryExpression':
                    τ = propagateExpr(owner, expr.left, τ);
                    return propagateExpr(owner, expr.right, τ);
                case 'LogicalExpression':
                    τ = propagateExpr(owner, expr.left, τ);
                    return propagateExpr(owner, expr.right, τ);
                case 'UnaryExpression':
                    return propagateExpr(owner, expr.argument, τ);
                case 'Dereference':
                    return propagateExpr(owner, expr.target, τ);
                case 'AddressOf':
                    return propagateExpr(owner, expr.target, τ);
                case 'MemberExpression':
                    for (const a of expr.arguments) τ = propagateExpr(owner, a, τ);
                    return propagateExpr(owner, expr.object, τ);
                case 'NewExpression':
                    for (const a of expr.arguments) τ = propagateExpr(owner, a, τ);
                    return τ;
                case 'IndexExpression':
                    for (const a of expr.indices) τ = propagateExpr(owner, a, τ);
                    return propagateExpr(owner, expr.object, τ);
                case 'FuseExpression':
                    τ = propagateExpr(owner, expr.discriminant, τ);
                    for (const arm of expr.arms) {
                        if (arm.pattern) τ = propagateExpr(owner, arm.pattern, τ);
                        τ = propagateExpr(owner, arm.value, τ);
                    }
                    return τ;
                case 'FunctionExpression':
                    return propagateStmts(owner, expr.body, τ);
                default:
                    return τ;
            }
        };

        const propagateStmts = (owner: string, stmts: any[], τ: number): number => {
            for (const s of stmts) {
                if (!s) continue;
                switch (s.type) {
                    case 'VariableDeclaration':
                        τ = propagateExpr(owner, s.value, τ) + 1;
                        break;
                    case 'VariableDeclarationList':
                        for (const d of s.declarations) τ = propagateExpr(owner, d.value, τ) + 1;
                        break;
                    case 'ExpressionStatement':
                        τ = propagateExpr(owner, s.expression, τ);
                        break;
                    case 'AssignmentStatement':
                        if (s.target) τ = propagateExpr(owner, s.target, τ);
                        τ = propagateExpr(owner, s.value, τ) + 1;
                        break;
                    case 'ReturnStatement':
                        τ = propagateExpr(owner, s.argument, τ);
                        break;
                    case 'IfStatement': {
                        τ = propagateExpr(owner, s.condition, τ);
                        const c = propagateStmts(owner, s.consequent, τ);
                        const a = s.alternate ? propagateStmts(owner, s.alternate, τ) : τ;
                        τ = Math.max(c, a);
                        break;
                    }
                    case 'WhileStatement':
                        τ = propagateExpr(owner, s.condition, τ);
                        τ = propagateStmts(owner, s.body, τ);
                        if (s.elseBody) τ = propagateStmts(owner, s.elseBody, τ);
                        break;
                    case 'ForStatement':
                        if (s.init) τ = propagateStmts(owner, [s.init], τ);
                        if (s.condition) τ = propagateExpr(owner, s.condition, τ);
                        τ = propagateStmts(owner, s.body, τ);
                        if (s.update) τ = propagateStmts(owner, [s.update], τ);
                        break;
                    case 'RouteStatement': {
                        τ = propagateExpr(owner, s.discriminant, τ);
                        let worst = τ;
                        for (const c of s.cases) worst = Math.max(worst, propagateStmts(owner, c.body, τ));
                        if (s.fallback) worst = Math.max(worst, propagateStmts(owner, s.fallback, τ));
                        τ = worst;
                        break;
                    }
                    case 'SpinStatement':
                        τ = propagateStmts(owner, s.body, τ);
                        τ = propagateExpr(owner, s.condition, τ);
                        break;
                    case 'UnsafeBlock':
                        τ = propagateStmts(owner, s.body, τ);
                        break;
                    case 'SpawnStatement':
                        τ = propagateStmts(owner, s.body, τ);
                        break;
                    case 'EntangleStatement':
                        τ = propagateExpr(owner, s.left, τ);
                        τ = propagateExpr(owner, s.right, τ) + 1;
                        break;
                    case 'BreakStatement':
                    case 'ContinueStatement':
                    case 'FunctionDeclaration':
                        break;
                    default:
                        break;
                }
            }
            return τ;
        };

        for (const b of blocks) {
            const finish = propagateStmts(b.name, b.fn.body, b.layer.time ?? 0);
            finishOf.set(b.name, finish);
        }

        return { callGraph, finishOf };
    }

    private checkDeadlines(blocks: Block[], finishOf: Map<string, number>): void {
        for (const b of blocks) {
            const deadline = b.layer.deadline;
            if (deadline === undefined) continue;
            const start = b.layer.time ?? 0;
            const finish = finishOf.get(b.name) ?? start;
            if (finish - start > deadline) {
                this.errors.push({
                    code: 'E-TOP006',
                    message: `Topology Error: block '${b.name}' exceeds deadline ${deadline} (finishes at τ=${finish}, anchor=${start}).`,
                    line: b.fn.line,
                    column: b.fn.column,
                });
            }
        }
    }




















    private resolveShuttleLinks(blocks: Block[], edges: TopologyEdge[]): ShuttleLink[] {
        const links: ShuttleLink[] = [];
        const key = (c: number[]) => c.join(',');

        let span = 1;
        for (let i = 0; i < shapeCoordDim(blocks); i++) {
            let lo = 0, hi = 0, seen = false;
            for (const b of blocks) {
                const v = b.layer.coord[i] ?? 0;
                if (!seen) { lo = v; hi = v; seen = true; }
                else { lo = Math.min(lo, v); hi = Math.max(hi, v); }
            }
            span *= Math.max(1, hi - Math.min(0, lo) + 1);
        }

















        const strategyDepth = (s: number, peerCount: number, sp: number): number => {
            switch (s) {
                case 1: return peerCount;
                case 2: return Math.ceil(Math.log2(peerCount + 1));
                case 3: return peerCount === 0 ? 0 : 1;
                case 4: return peerCount === 0 ? 0 : 1;
                default: return sp <= 1 ? 1 : Math.ceil(Math.log2(sp));
            }
        };

        const adj = new Map<string, TopologyEdge['kind']>();
        const pairKey = (a: string, b: string) => (a < b ? `${a} ${b}` : `${b} ${a}`);
        for (const e of edges) {
            if (e.kind === 'happens-before') continue;
            if (!adj.has(pairKey(e.from, e.to))) adj.set(pairKey(e.from, e.to), e.kind);
        }

        for (const b of blocks) {
            const sh = b.layer.shuttle;
            if (!sh) continue;

            const lambda = sh.lambda ?? 1.0;
            if (!(lambda > 0)) {
                this.errors.push({
                    code: 'E-SHU003',
                    message: `Shuttle Error: block '${b.name}' has shuttle_lambda=${lambda}; `
                        + `the correlation kernel exp(-d/λ) must be strictly positive, so λ must be > 0.`,
                    line: b.fn.line, column: b.fn.column,
                });
            }
            const phase = sh.phase ?? 2;
            if (phase !== 0 && phase !== 1 && phase !== 2) {
                this.errors.push({
                    code: 'E-SHU004',
                    message: `Shuttle Error: block '${b.name}' has shuttle_phase=${phase}; expected 0 (pre), 1 (post) or 2 (both).`,
                    line: b.fn.line, column: b.fn.column,
                });
            }
            const rounds = sh.rounds ?? 0;
            if (rounds < 0) {
                this.errors.push({
                    code: 'E-SHU006',
                    message: `Shuttle Error: block '${b.name}' has shuttle_rounds=${rounds}; expected >= 0 (0 = auto).`,
                    line: b.fn.line, column: b.fn.column,
                });
            }
            const strategy = sh.strategy ?? ShuttleStrategy.Auto;
            if (![ShuttleStrategy.Auto, ShuttleStrategy.Butterfly, ShuttleStrategy.Ring,
                  ShuttleStrategy.Tree, ShuttleStrategy.Grover, ShuttleStrategy.C001Point].includes(strategy)) {
                this.errors.push({
                    code: 'E-SHU005',
                    message: `Shuttle Error: block '${b.name}' has shuttle_strategy=${strategy}; `
                        + `expected -1 (auto), 0 (butterfly), 1 (ring), 2 (tree), 3 (grover) or 4 (c001 point).`,
                    line: b.fn.line, column: b.fn.column,
                });
            }
            if (sh.targets.length === 0) {
                this.errors.push({
                    code: 'E-SHU001',
                    message: `Shuttle Error: block '${b.name}' declares shuttle tuning (λ/phase/rounds/strategy) `
                        + `but no shuttle=(coord,…) target.`,
                    line: b.fn.line, column: b.fn.column,
                });
                continue;
            }

            const peers: ShuttlePeer[] = [];
            const seen = new Set<string>();
            let matchedSelfOnly = true;
            for (const target of sh.targets) {
                const matches = blocks.filter(x => key(x.layer.coord) === key(target));
                if (matches.some(x => x.name !== b.name)) matchedSelfOnly = false;
                if (matches.length === 0) {
                    this.errors.push({
                        code: 'E-SHU001',
                        message: `Shuttle Error: block '${b.name}' shuttles to coord(${target.join(',')}) `
                            + `but no block occupies that coordinate.`,
                        line: b.fn.line, column: b.fn.column,
                    });
                    continue;
                }
                for (const m of matches) {
                    if (m.name === b.name) continue;
                    if (seen.has(m.name)) continue;
                    const via = adj.get(pairKey(b.name, m.name));
                    if (via === undefined) {
                        this.errors.push({
                            code: 'E-SHU002',
                            message: `Shuttle Error: block '${b.name}' shuttles to '${m.name}' `
                                + `(coord(${target.join(',')})) but the two blocks share no topology edge `
                                + `(no stack/parallel/projection/inversion/crossing relation).`,
                            line: b.fn.line, column: b.fn.column,
                        });
                        continue;
                    }
                    if (key(b.layer.coord) === key(m.layer.coord)
                        && (b.layer.time ?? 0) === (m.layer.time ?? 0)) {
                        this.errors.push({
                            code: 'E-SHU008',
                            message: `Shuttle Error: block '${b.name}' and shuttle peer '${m.name}' share `
                                + `coord(${b.layer.coord.join(',')}) at time ${b.layer.time ?? 0}; `
                                + `permeation peers must occupy distinct time slots (else E-TOP003).`,
                            line: b.fn.line, column: b.fn.column,
                        });
                        continue;
                    }
                    seen.add(m.name);
                    const d = l1Distance(b.layer.coord, m.layer.coord);
                    const logW = lambda > 0 ? -(d / lambda) : 0;
                    const w = lambda > 0 ? Math.exp(logW) : 0;
                    peers.push({ name: m.name, coord: m.layer.coord.slice(), logWeight: logW, weight: w, distance: d, via });
                }
            }

            if (peers.length === 0) {
                if (seen.size === 0 && !matchedSelfOnly) {
                    this.errors.push({
                        code: 'E-SHU002',
                        message: `Shuttle Error: block '${b.name}' declares a shuttle target that yields `
                            + `no admissible peer${matchedSelfOnly ? ' — the target coordinate is occupied only by the source block itself (self-shuttle)' : ''}. `
                            + `Permeation requires at least one related block.`,
                        line: b.fn.line, column: b.fn.column,
                    });
                }
                continue;
            }

            peers.sort((x, y) => (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));

            const effectiveRounds = rounds > 0 ? rounds : strategyDepth(strategy, peers.length, span);
            const slotsPerPhase: [number, number] = phase === 2
                ? [effectiveRounds, effectiveRounds]
                : phase === 1 ? [0, effectiveRounds] : [effectiveRounds, 0];

            const deadline = b.layer.deadline;
            if (deadline !== undefined) {
                const overhead = slotsPerPhase[0] + slotsPerPhase[1];
                if (overhead > deadline) {
                    this.errors.push({
                        code: 'E-SHU007',
                        message: `Shuttle Error: block '${b.name}' needs ${overhead} extra time slot(s) `
                            + `for ${effectiveRounds} shuttle round(s) (phase=${phase}), exceeding deadline ${deadline}.`,
                        line: b.fn.line, column: b.fn.column,
                    });
                }
            }

            links.push({
                from: b.name,
                peers,
                lambda,
                phase,
                rounds: effectiveRounds,
                strategy,
                slotsPerPhase,
            });

            if (!b.layer.track) {
                this.warnings.push({
                    code: 'W-SHU001',
                    message: `Shuttle Warning: block '${b.name}' requests coherent shuttle but declares no `
                        + `@track scope; the quantum-coherent path needs a classical shadow supply `
                        + `(see docs/rvdl-shuttle-tracking.md §5). Degrading to classical shadow mixing.`,
                    line: b.fn.line, column: b.fn.column,
                });
            }
        }
        return links;
    }













    private hasQuantumSource(fn: FunctionDeclaration, quantumProducers: Set<string>): boolean {
        for (const p of fn.params ?? []) {
            const t = (p.type ?? '').trim();
            if (t === 'Qubit' || t === 'QObject') return true;
            if (/^cap\s*<\s*(Qubit|QObject)\s*>$/i.test(t)) return true;
        }
        const seen = new Set<any>();
        const walk = (node: any): boolean => {
            if (!node || typeof node !== 'object' || seen.has(node)) return false;
            seen.add(node);
            if (Array.isArray(node)) {
                for (const item of node) if (walk(item)) return true;
                return false;
            }
            if (node.type === 'NewExpression') {
                const cn = String(node.className ?? '');
                if (cn === 'QuantumRegister' || cn === 'DiracState' || cn === 'BellState') return true;
            }
            if (node.type === 'FunctionCall') {
                const name = String(node.name ?? '');
                if (QUANTUM_PRODUCING_BUILTINS.has(name)) return true;
                if (quantumProducers.has(name)) return true;
            }
            for (const key of Object.keys(node)) {
                if (key === 'type' || key === 'line' || key === 'column') continue;
                if (walk((node as any)[key])) return true;
            }
            return false;
        };
        return walk(fn.body ?? []);
    }
















    private buildTrackingScopes(blocks: Block[], quantumProducers: Set<string>): TrackingScope[] {
        const scopes: TrackingScope[] = [];
        for (const b of blocks) {
            const tr = b.layer.track;
            if (!tr) continue;

            const slots = tr.slots ?? 1;
            if (tr.observables.length === 0) {
                this.errors.push({
                    code: 'E-TRK001',
                    message: `Tracking Error: block '${b.name}' declares track=(...) with no observable.`,
                    line: b.fn.line, column: b.fn.column,
                });
                continue;
            }
            if (!(slots >= 1)) {
                this.errors.push({
                    code: 'E-TRK001',
                    message: `Tracking Error: block '${b.name}' has track_slots=${slots}; expected >= 1.`,
                    line: b.fn.line, column: b.fn.column,
                });
                continue;
            }

            const mode = tr.mode ?? TrackingMode.Auto;
            if (![TrackingMode.Auto, TrackingMode.Qnd, TrackingMode.Shadow,
                  TrackingMode.Relabel].includes(mode as 0 | 1 | 2 | 3)) {
                this.errors.push({
                    code: 'E-TRK004',
                    message: `Tracking Error: block '${b.name}' has track_mode=${mode}; `
                        + `expected 0 (auto), 1 (qnd), 2 (shadow) or 3 (relabel).`,
                    line: b.fn.line, column: b.fn.column,
                });
                continue;
            }

            if (!this.hasQuantumSource(b.fn, quantumProducers)) {
                this.errors.push({
                    code: 'E-TRK002',
                    message: `Tracking Error: block '${b.name}' declares track=(${tr.observables.join(', ')}) `
                        + `but no quantum source is resolvable in its scope — expected a Qubit/QObject `
                        + `parameter, a quantum constructor (new QuantumRegister/DiracState/BellState), `
                        + `alloc(), or a call returning a quantum state.`,
                    line: b.fn.line, column: b.fn.column,
                });
                continue;
            }

            const level: number[] = [];
            let bad = false;
            for (const obs of tr.observables) {
                const fam = TRACKABLE_OBSERVABLES[obs];
                if (fam === undefined) {
                    this.errors.push({
                        code: 'E-TRK003',
                        message: `Tracking Error: block '${b.name}' tracks unknown observable '${obs}'. `
                            + `Known: ${Object.keys(TRACKABLE_OBSERVABLES).join(', ')}.`,
                        line: b.fn.line, column: b.fn.column,
                    });
                    bad = true;
                    continue;
                }
                const nat = levelOfFamily(fam);
                if (mode === TrackingMode.Qnd && nat !== 0) {
                    this.errors.push({
                        code: 'E-TRK003',
                        message: `Tracking Error: block '${b.name}' forces track_mode=1 (qnd) but observable `
                            + `'${obs}' is a ${fam}-type readout; only Pauli-family (expect_z/expect_x/expect_y) `
                            + `supports non-disturbing QND cloning (Peierls).`,
                        line: b.fn.line, column: b.fn.column,
                    });
                    bad = true;
                    continue;
                }
                level.push(
                    mode === TrackingMode.Shadow ? 1
                        : mode === TrackingMode.Relabel ? 2
                            : nat);
            }
            if (bad || level.length !== tr.observables.length) continue;

            const start = b.layer.time ?? 0;
            scopes.push({
                block: b.name,
                observables: tr.observables.slice(),
                mode,
                slots,
                level,
                window: [start, start + (b.layer.cost ?? 1)],
            });
        }
        return scopes;
    }
}

function shapeCoordDim(blocks: Block[]): number {
    let m = 0;
    for (const b of blocks) m = Math.max(m, b.layer.coord.length);
    return m;
}

function l1Distance(a: number[], b: number[]): number {
    const n = Math.max(a.length, b.length);
    let d = 0;
    for (let i = 0; i < n; i++) d += Math.abs((a[i] ?? 0) - (b[i] ?? 0));
    return d;
}

export function buildTopology(program: Program): { topology: Topology; errors: TopologyError[]; warnings: TopologyError[] } {
    return new TopologyBuilder().build(program);
}