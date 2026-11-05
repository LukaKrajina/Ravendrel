// ============================================================================
// TopologyBuilder —— 多维标签函数的执行拓扑构建
//
// 把带 @layer(time, thread, coord[, cost][, deadline]) 的函数聚合为一张
// 多维执行拓扑：
//   - time   ：锚点时间（块在其 coord 时序链上的启动槽位）
//   - thread ：逻辑线程 id（同线程串行、异线程可并行）
//   - coord  ：N 维运行层坐标（块在多维拓扑空间的初始位置）
//
// 块间关系从标签自动推导：
//   coord 相同 → 叠加（stack，按 time 组成时序链）
//   coord 不同 → 平行（parallel，空间并置）
//
// 块内调用传播延迟（逻辑时钟模型）：
//   子函数启动时刻 = 父函数锚点 + 调用点之前的累计延迟 Δt
//   Δt = Σ δ(前置语句) + Σ cost(前置子函数调用)
//
// ============================================================================

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

// ============================================================================
// 追踪可观测量表 —— 保真级别的物理依据
//
// 单次软克隆在物理上不可能：测 Z 基得 b 后测量后态为 |b⟩，逆推只能得回 |0⟩，
// 相位信息不可恢复。故追踪采用三级阶梯，按「抽取是否扰动态」分类：
//
//   L0 (qnd)     Peierls 定理：Pauli 族可观测量在其本征基下测量不扰动态。
//                → 值精确、态完好、零扰动、O(1)、可无限次。
//   L2 (relabel) 被追踪量**本身就是经典量**（对象固有属性，或早已坍缩的计数），
//                无需抽取电路 → 值精确、O(1) 自由、态从不被触碰。
//   L1 (shadow)  经典阴影（Huang-Kueng-Preskill, Nature Physics 16, 1050, 2020）：
//                需基旋转 + 测量，副本是经典的故可任意复制（不违反不可克隆），
//                但值是统计的（范数 ‖O‖²_shadow = 3^k），态经 uncompute/重制备近似复原。
//
// 注意 measure_z/x/y 归入 L1 而非 L2：虽然被测比特退化为经典基态「看起来」精确，
// 但原态 α|0⟩+β|1⟩ 的相位已不可恢复，按最弱保证归类才诚实。
// ============================================================================

/** 可观测量 → 所属族（'pauli' | 'classical' | 'measure'） */
export const TRACKABLE_OBSERVABLES: Readonly<Record<string, 'pauli' | 'classical' | 'measure'>> = {
    // L0：Pauli 族非破坏期望（Peierls 定理，零扰动）
    expect_z: 'pauli',
    expect_x: 'pauli',
    expect_y: 'pauli',
    // L2：纯经典量，无需抽取电路
    num_qubits: 'classical',
    shannon4: 'classical',
    shannon8: 'classical',
    amplitude: 'classical',
    // L1：需测量的统计级读出
    entropy: 'measure',
    fidelity: 'measure',
    attention: 'measure',
    measure_z: 'measure',
    measure_x: 'measure',
    measure_y: 'measure',
};

/** 族 → 保真级别 */
function levelOfFamily(fam: 'pauli' | 'classical' | 'measure'): number {
    if (fam === 'pauli') return 0;       // L0 qnd
    if (fam === 'classical') return 2;   // L2 relabel
    return 1;                            // L1 shadow
}

const LEVEL_NAME = ['L0/qnd', 'L1/shadow', 'L2/relabel'];

/**
 * 返回量子态（QObject / Qubit）的内建函数全集 —— E-TRK002 判据 ③。
 * 与 ir.ts 中这些函数的返回类型声明保持一致；新增量子态产生内建时
 * 两处必须同步（这是唯一一处人工同步点）。
 */
const QUANTUM_PRODUCING_BUILTINS: ReadonlySet<string> = new Set([
    'alloc',
    'basis_state',
    'encode_text', 'encode_image', 'rvdl_encode_string',
    'mind_read',
    'qrc_probe', 'qrc_predict',
    'qchain_coin_mint',
]);

/** 关联边种类 → 中文说明（用于 E-SHU002 的可读消息） */
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
    /** 警告通道：不阻断编译/执行的诚实告警（如 W-SHU001）。
     *  与 errors 分开是因为二者语义不同：error 必须修，warning 只是告知
     *  「这里会发生文档化的降级」。混在一个数组里会让 warning 也阻断执行 ——
     *  那正是 W-SHU001 曾经的处境（文档承诺退化为经典影子混合，实际程序被毙）。 */
    private warnings: TopologyError[] = [];

    public build(program: Program): { topology: Topology; errors: TopologyError[]; warnings: TopologyError[] } {
        this.errors = [];
        this.warnings = [];

        const blocks: Block[] = [];

        // MMI 模块（含 export 函数）：export 函数经 MMI_INVOKE 直接调用，@layer 辅助函数
        // 只是被调用的子函数、不参与拓扑调度。跳过拓扑收集与 coord 占用检查，
        // 避免组件函数之间 coord 相同的「假冲突」（如 perceive 与 chaos_drift 均 coord=0）。
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

        // 收集 @layer 函数；显式函数必须带标签（E-TOP001）。
        // 编译器合成函数（@[undo]/@[steer] 生成的 <name>_undo/_steer）、
        // @[gate] 子函数（被调用的门单元，非拓扑入口）、以及 export 函数
        // （模块对外接口，经 MMI_INVOKE 直接调用，不参与拓扑调度）均跳过。
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

        // 无拓扑块（脚本模式 / 全部报错）：返回空拓扑。
        if (blocks.length === 0) {
            return {
                topology: { shape: { time: 1, thread: 1, coord: [] }, blocks: [], edges: [], callGraph: [], shuttleLinks: [], trackingScopes: [] },
                errors: this.errors,
                warnings: this.warnings,
            };
        }

        // 坐标维度一致性校验（E-TOP002）
        this.validateCoordDims(blocks);

        // 0.5 未知 @layer 键（E-TOP008）——原为静默丢弃
        this.reportUnknownKeys(blocks);
        // 0.6 负值键（E-TOP009）——解析层统一接受符号后必须在语义层把关
        this.reportNegativeKeys(blocks);

        // 1. 运行形状聚合
        const shape = this.aggregateShape(blocks);

        // 2. 块间推导（平行 / 叠加）
        const edges = this.deriveEdges(blocks);

        // 3. 调用传播延迟累计（逻辑时钟）
        const { callGraph, finishOf } = this.buildCallGraph(blocks);

        // 4. 时间束缚校验（deadline）
        this.checkDeadlines(blocks, finishOf);

        // 5. 穿梭关联求解（复用 edges 的五类关联边）
        const shuttleLinks = this.resolveShuttleLinks(blocks, edges);

        // 6. 追踪作用域登记（软克隆影子）
        //    量子源集合（返回 Qubit/QObject 的用户函数）供 E-TRK002 的
        //    「源可解析性」检查使用 —— 只认得出直接构造会误报合法程序。
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

    // ---- E-TOP008：未知 @layer 键 ------------------------------------------
    /**
     * 原 `parseLayerTag` 的 else-if 链缺 else 分支 → 未知键被静默丢弃
     * （如 `thread2=1` 无声失效）。穿梭/追踪引入 8 个新键后，这种静默失败
     * 会变成真实的用户困惑来源，故改为显式报错。
     */
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

    // ---- E-TOP009：@layer 键取值为负但语义要求非负 --------------------------
    /**
     * 词法层把负号独立为 `Minus`，解析层为支持 `shuttle_strategy=-1`
     * （**文档化的默认值**）而统一接受有符号数值。于是 `time=-1` / `cost=-1`
     * 从「无码的 Parser Error 崩溃」变成「能解析」，必须在此补上语义校验，
     * 否则会从崩溃退化为**静默的错误拓扑**（负 cost 会让
     * `window = [time, time+cost]` 反向，负 time 会让分层排序失真）。
     *
     * 无语义的负数在此拒绝；`shuttle_*` / `track_*` 的负数各有专门诊断
     * （E-SHU003 / E-SHU004 / E-SHU005 / E-SHU006 / E-TRK001 / E-TRK004）。
     * `coord` 允许负数：coord 是格点**地址**而非计数，L1 距离对负数同样良定义。
     */
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

    // ---- 校验：曲面体坐标维度（放宽原 E-TOP002）----------------------------
    // 曲面体允许 coord 维度不同：低维 ↔ 高维块构成 projection（投影）边。
    // 原 E-TOP002 强制维度一致，已放宽——不同维度的块不再报错，由 deriveEdges
    // 推导为 projection 边。此处保留占位，便于后续诊断「维度差 > 1」等异常投影。
    private validateCoordDims(blocks: Block[]): void {
        // 曲面体坐标：维度不同是合法的（低维 ↔ 高维构成 projection 投影边），
        // 因此不做维度一致性校验（原 E-TOP002 已放宽）。
        //
        // 但「维度差 > 1」属异常投影：跨越一个以上维度的指数/对数映射在几何上
        // 没有良定义的投影路径（相邻维度间才有自然的投影对应），故诊断 E-TOP007。
        if (blocks.length < 2) return;

        let minDim = Number.POSITIVE_INFINITY;
        let maxDim = 0;
        for (const b of blocks) {
            const d = b.layer.coord.length;
            if (d < minDim) minDim = d;
            if (d > maxDim) maxDim = d;
        }

        if (maxDim - minDim > 1) {
            // 以维度最小的块作为诊断定位点。
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

    // ---- 运行形状聚合 -------------------------------------------------------
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

    // ---- 块间推导（平行 / 叠加）--------------------------------------------
    private deriveEdges(blocks: Block[]): TopologyEdge[] {
        const edges: TopologyEdge[] = [];
        const key = (c: number[]) => c.join(',');

        // 按 coord 分组
        const groups = new Map<string, Block[]>();
        for (const b of blocks) {
            const k = key(b.layer.coord);
            if (!groups.has(k)) groups.set(k, []);
            groups.get(k)!.push(b);
        }

        // 组内：叠加（按 time 组成时序链）
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

                    // 叠加（stack）：仅同 dual 同 thread 的块构成时序链，
                    // 检查 time 冲突（E-TOP003）与 gap（E-TOP005）
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
                    // 曲面体几何边：反转（T-对偶，dual 标记不同）
                    if (!sameDual) {
                        edges.push({ from: a.name, to: b.name, kind: 'inversion' });
                    }
                    // 曲面体几何边：交叉（跨线程测地线交叉）
                    if (!sameThread) {
                        edges.push({ from: a.name, to: b.name, kind: 'crossing' });
                    }
                }
            }
        }

        // 跨组：曲面体几何边（投影 / 平行传输）
        for (let i = 0; i < blocks.length; i++) {
            for (let j = i + 1; j < blocks.length; j++) {
                const A = blocks[i];
                const B = blocks[j];
                if (key(A.layer.coord) !== key(B.layer.coord)) {
                    // 维度不同 → 投影（低维 ↔ 高维，指数/对数映射）
                    // 维度相同 → 平行（沿测地线平行传输）
                    const kind = A.layer.coord.length !== B.layer.coord.length
                        ? 'projection' : 'parallel';
                    edges.push({ from: A.name, to: B.name, kind });
                }
            }
        }

        return edges;
    }

    // ---- 调用传播延迟累计（逻辑时钟模型，核心）----------------------------
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
                    // 先求值实参（可能推进 τ），再调用函数本身
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
                    return τ; // 字面量 / 标识符 / native / result 等叶子
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
                        τ = propagateStmts(owner, s.body, τ);          // 保守：单次迭代
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

    // ---- 时间束缚校验（E-TOP006）-------------------------------------------
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

    // =========================================================================
    // 「穿梭」：关联层数据渗透
    // =========================================================================

    /**
     * 求解穿梭链接。
     *
     * 对每个带 `shuttle=(c,…)` 的块 b：
     *   1. 按目标 coord 查出全部匹配块（目标缺失 → E-SHU001）
     *   2. 在既有五类拓扑边集合中查 b↔peer 是否**存在关联边**；
     *      存在才允许穿梭（这正是「前提两个或多个拓扑层有关联」的实现），
     *      否则报 E-SHU002
     *   3. 关联度 w = exp(−d_L1(coord_b, coord_peer) / lambda)，**恒严格为正**，
     *      编译期与运行期均不设截断阈值（这是「哪怕关联度极小也有效」的严格保证，
     *      与注意力 softmax 丢弃小权重根本不同）
     *   4. 规划轮次：rounds=0 时 auto，取**所选策略**的解析深度（见
     *      `strategyDepth()`）——蝶式 ⌈log₂span⌉ / 环式 n / 树式 ⌈log₂(n+1)⌉ /
     *      Grover 1 / c001 蝶点 1。rounds>0 时用户值即权威值。
     *
     *      span = ∏ᵢ (coord_max[i] − min(0, coord_min[i]) + 1)（各维格点真实跨度）。
     *
     * phase 决定渗透相对共享点的时机：0=仅前 / 1=仅后 / 2=前后（默认，双侧恢复算子）。
     */
    private resolveShuttleLinks(blocks: Block[], edges: TopologyEdge[]): ShuttleLink[] {
        const links: ShuttleLink[] = [];
        const key = (c: number[]) => c.join(',');

        // 超立方体跨度：span = ∏ᵢ (coord_max[i] − min(0, coord_min[i]) + 1)
        //
        // 各维取「格点跨度」：coord 是格点**地址**而非计数，允许为负（L1 距离
        // 对负地址同样良定义）。当全部 coord ≥ 0 且最小值恰为 0 时，本式退化为
        // 文档中的 span = ∏(coord_max[i]+1)（既有行为不变）。
        //
        // 若不减去 min(0, coord_min)，全负地址的拓扑会得到 span=1 → rounds=0：
        // 穿梭**静默地一轮都不做**。这正是本设计要根除的静默失败。
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
        // ── 策略解析深度：见下方 strategyDepth()（Ta.hpp `depth_of()` 的编译期对照）──

        /**
         * 所选策略的解析深度（auto 轮次）。
         *
         * 为什么必须按策略分：轮次既是**执行量**（`shuttle_rounds` 的实际轮数）
         * 也是**诊断量**（`E-SHU007` 拿它和 `deadline` 比）。若统一套用蝶式公式，
         * 环式的实际轮数是 n、诊断却按 ⌈log₂span⌉ 算 —— 二者在 n 稍大时就差一个
         * 数量级，deadline 检查会**系统性地低估**代价。
         *
         * 五个式子与运行期 `Ta.hpp` 的 `depth_of()` 必须逐字一致
         * （由 runtime/tests/shuttle_topology_parity.cpp 钉住）。
         *
         * `span <= 1` 的退化拓扑：声明了对端就意味着至少一次交换，
         * 解析为 0 轮会让整个机制**静默失效**，故蝶式下取 max(1, ·)。
         *
         * c001 蝶点（4）恒 1 轮：汇聚与辐射在同一轮的双侧相位内完成，
         * 深度与 span / 对端数全部无关（零附加资源，区别于需 2ⁿ 寄存器的 Grover）。
         */
        const strategyDepth = (s: number, peerCount: number, sp: number): number => {
            switch (s) {
                case 1: return peerCount;                                  // 环式：绕环一圈
                case 2: return Math.ceil(Math.log2(peerCount + 1));        // 树式：2^d−1 ≥ n
                case 3: return peerCount === 0 ? 0 : 1;                    // Grover：一步全局混合
                case 4: return peerCount === 0 ? 0 : 1;                    // c001 蝶点：压缩成点再辐射，恒 1 轮
                default: return sp <= 1 ? 1 : Math.ceil(Math.log2(sp));    // 蝶式（含 auto 的默认）
            }
        };

        // 邻接查找：块对 → 关联边种类（无向）
        const adj = new Map<string, TopologyEdge['kind']>();
        const pairKey = (a: string, b: string) => (a < b ? `${a} ${b}` : `${b} ${a}`);
        for (const e of edges) {
            if (e.kind === 'happens-before') continue;
            if (!adj.has(pairKey(e.from, e.to))) adj.set(pairKey(e.from, e.to), e.kind);
        }

        for (const b of blocks) {
            const sh = b.layer.shuttle;
            if (!sh) continue;

            // ---- 参数校验 ------------------------------------------------
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

            // ---- 解析对端 ------------------------------------------------
            const peers: ShuttlePeer[] = [];
            const seen = new Set<string>();
            let matchedSelfOnly = true;   // 到目前为止，目标只匹配到源块自身
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
                    if (m.name === b.name) continue;               // 不穿梭到自己
                    if (seen.has(m.name)) continue;
                    const via = adj.get(pairKey(b.name, m.name));
                    if (via === undefined) {
                        // 目标块存在，但与本块之间没有关联边 → 违反「前提…有关联」
                        this.errors.push({
                            code: 'E-SHU002',
                            message: `Shuttle Error: block '${b.name}' shuttles to '${m.name}' `
                                + `(coord(${target.join(',')})) but the two blocks share no topology edge `
                                + `(no stack/parallel/projection/inversion/crossing relation).`,
                            line: b.fn.line, column: b.fn.column,
                        });
                        continue;
                    }
                    // 同 coord + 同 time 会与 E-TOP003 的占用冲突：提前给出可读错误
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
                    // 关联度核（对数空间）：logWeight = −d/λ 对任意距离都有限。
                    // 线性形式 exp(logWeight) 在 d/λ ≳ 746 时双精度下溢为 0 ——
                    // 那是数值零而非语义截断；路由一律用 logWeight，故对端永不丢失。
                    const logW = lambda > 0 ? -(d / lambda) : 0;
                    const w = lambda > 0 ? Math.exp(logW) : 0;
                    peers.push({ name: m.name, coord: m.layer.coord.slice(), logWeight: logW, weight: w, distance: d, via });
                }
            }

            if (peers.length === 0) {
                // 目标 coord 有匹配块，但没有**可用的**对端。
                //
                // 注意：按 deriveEdges 的完备性，任意两个 coord 不同的块之间
                // 必定存在 parallel 或 projection 边，因此「两块之间完全没有
                // 关联边」这一子情形在当前推导下**不可达**——上面的
                // E-SHU002 分支属防御性检查。真正可达的失败是：
                //   (a) 目标 coord 只匹配到源块自身（自我穿梭）；
                //   (b) 每个匹配都因 E-SHU008（同 coord 同 time）被拒。
                // 二者都报 E-SHU002，并把真实原因写进消息。
                // 如果 matches 为空，E-SHU001 已经在上面报过了，不再重复。
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

            // 按块名排序，保证权重序列与 targets 的对应关系确定性
            peers.sort((x, y) => (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));

            // 轮次：0 = auto（超立方体位数）；负数已报错，按 auto 处理
            const effectiveRounds = rounds > 0 ? rounds : strategyDepth(strategy, peers.length, span);
            // 每个相位各需追加的时间槽（phase=0/1/2 均可为 0）
            const slotsPerPhase: [number, number] = phase === 2
                ? [effectiveRounds, effectiveRounds]
                : phase === 1 ? [0, effectiveRounds] : [effectiveRounds, 0];

            // ---- E-SHU007：规划轮次超出 deadline -------------------------
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

            // ---- W-SHU001：请求相干穿梭但本块未声明 @track ------------------
            // 穿梭的量子路径要「读取对端态而不消耗它」。量子力学禁止克隆，
            // 读取（测量）又会破坏态 —— 唯一合法出路是 QND 读出（L0）或
            // 经典阴影（L1），即追踪（docs §5）。
            //
            // 未声明 @track 时该出路不存在 → 量子相干路径不可用，运行期退化为
            // 经典影子混合（若对端也无影子则根本无法渗透）。此处诚实告警，
            // **不静默伪造成功**。
            //
            // 走 **warning 通道**而非 error：这是文档化的降级路径（运行期
            // 照常执行、按经典影子混合），不是必须修的错误。若混进 errors，
            // server.ts / cli.ts 的 `errors.length > 0` 会把程序毙掉 ——
            // 文档承诺的「退化」永远走不到，告警本身成了阻塞。
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

    // =========================================================================
    // 「追踪」：软克隆经典影子作用域
    // =========================================================================

    // ---- E-TRK002：块作用域内是否存在可解析的量子源 ------------------------
    /**
     * 量子源 = 以下任一形式：
     *   ① `Qubit` / `QObject`（含 `cap<Qubit>` / `cap<QObject>`）类型的参数；
     *   ② 量子构造 `new QuantumRegister(...)` / `new DiracState(...)` / `new BellState(...)`；
     *   ③ 返回量子态的内建：`alloc` / `basis_state` / `encode_text` / `encode_image` /
     *      `rvdl_encode_string` / `mind_read` / `qrc_probe` / `qrc_predict` / `qchain_coin_mint`；
     *   ④ 返回 `Qubit` / `QObject` 的用户函数调用（调用方提供 `quantumProducers`）。
     *
     * 判据刻意保守（宁可漏报不可误报）：只在这些**确定的**量子态产生点全部缺失
     * 时才报 E-TRK002。漏报的后果是运行期影子为空（有 W-SHU001 等告警兜底）；
     * 误报的后果是把合法程序挡在编译期外 —— 后者昂贵得多。
     */
    private hasQuantumSource(fn: FunctionDeclaration, quantumProducers: Set<string>): boolean {
        for (const p of fn.params ?? []) {
            const t = (p.type ?? '').trim();
            if (t === 'Qubit' || t === 'QObject') return true;
            if (/^cap\s*<\s*(Qubit|QObject)\s*>$/i.test(t)) return true;
        }
        // 通配遍历函数体：不逐一枚举语句/表达式形态 —— 枚举漏一种就漏一个源
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

    /**
     * 登记追踪作用域。
     *
     * 影子槽类型为**经典标量**（float/int32/double），绝非 QObject——
     * 否则直接违反 borrow.ts 的 E-Q001(no-cloning)/E-Q002(linear leak)。
     * 这与子系统码「规范量子比特被测量、逻辑量子比特存活」同构。
     *
     * 作用域窗口 = [time, time+cost)，即 `@layer` 标签所附的功能周期；
     * 周期外不可见 → 不构成持久克隆。
     *
     * E-TRK002（追踪源不可解析）：track 的每个可观测量都要求一个量子源
     * （Qubit/QObject）。若块的作用域内**不存在任何**量子源 —— 没有量子类型
     * 参数、没有量子构造、没有 alloc()、没有返回量子态的用户函数调用 ——
     * 则可观测量无从求值（如对 int32 调 expect_z），报 E-TRK002 而不是
     * 静默登记一个永远填不满的影子槽。
     */
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

            // E-TRK002：追踪源在块作用域内不可解析（文档 §7.2 早已列出、
            // 此前未实现的诊断）。没有量子源却声明 track → 影子槽永远填不满，
            // 那是一条「看起来在工作」的静默空转。
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

            // auto 按族选级；强制模式与该级能力不符时报 E-TRK003
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
                // 强制模式**覆盖** auto 选级（docs §4.2）：
                //   track_mode=2（shadow）→ 一律 L1；=3（relabel）→ 一律 L2；
                //   =1（qnd）→ 已在上方排除非 Pauli 族，故 nat 必为 0。
                // 若此处仍用 nat，`track_mode=3` 就会对 entropy 下发 L1 —— 与
                // 运行期（Ta.hpp 的 TrackingPlanner 按 mode 强制）不一致。
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

/** coord 最大维度（各块 coord 长度取最大） */
function shapeCoordDim(blocks: Block[]): number {
    let m = 0;
    for (const b of blocks) m = Math.max(m, b.layer.coord.length);
    return m;
}

/** 两个 coord 的 L1 编码距离 Σ|aᵢ−bᵢ|（维度不同则按 0 补齐，对齐 logical_qubit_layout 的码距口径） */
function l1Distance(a: number[], b: number[]): number {
    const n = Math.max(a.length, b.length);
    let d = 0;
    for (let i = 0; i < n; i++) d += Math.abs((a[i] ?? 0) - (b[i] ?? 0));
    return d;
}

/** 便捷入口：AST -> Topology */
export function buildTopology(program: Program): { topology: Topology; errors: TopologyError[]; warnings: TopologyError[] } {
    return new TopologyBuilder().build(program);
}