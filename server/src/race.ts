/**
 * Q-Digest —— 量子感知的静态数据竞争检测
 *
 *   digest = "线程执行历史"的抽象。静态分析用 digest 索引抽象状态；对共享
 *   变量的每次访问记录 (access, digest)；竞争检测用 may-happen-in-parallel
 *   (MHP) 谓词 ‖?‖ 判定两访问能否并行。若任一 digest 能证明两访问必串行，
 *   则该对访问被排除；否则若至少一次是写，则报告潜在竞争。
 *   然而对于量子比特来说，它受 no-cloning 约束，且纠缠具有传递性——对 q1 的操作会影响与 q1
 *   纠缠的 q3。因此 digest 额外记录"该线程可影响到的量子比特闭包"。跨线程
 *   的破坏性操作（测量/释放）触及同一纠缠闭包、且无共同锁时，构成量子竞争。
 */

// ---------------------------------------------------------------------------
// 基础类型
// ---------------------------------------------------------------------------

export interface Digest {
    /** 线程 id */
    tid: string;
    /** 当前持有的互斥锁集合（lockset digest） */
    locks: ReadonlySet<string>;
    /** 该线程可影响到的量子比特闭包（quantum digest，含纠缠传递） */
    qubits: ReadonlySet<string>;
}

export interface Access {
    /** 共享位置（全局变量名 / 量子寄存器 id 等） */
    variable: string;
    isWrite: boolean;
    digest: Digest;
    /** 本次访问会破坏性触及的量子比特（测量/释放，会坍缩态） */
    destructiveQubits: ReadonlySet<string>;
    /** 访问点在源码中的位置（用于诊断定位） */
    line: number;
    column: number;
}

export interface Race {
    variable: string;
    threadA: string;
    threadB: string;
    quantum: boolean;
    reason: string;
    /** 竞争发生的位置（取两个访问中较靠前者） */
    line: number;
    column: number;
}

// ---------------------------------------------------------------------------
// MHP 谓词
// ---------------------------------------------------------------------------

function intersects(a: ReadonlySet<string>, b: ReadonlySet<string>): boolean {
    for (const x of a) if (b.has(x)) return true;
    return false;
}

/**
 * may-happen-in-parallel 谓词。
 * 返回 true 表示"可能并行"（⊤），false 表示"必串行"。
 *
 * 组合判据（满足任一即必串行）：
 *   (1) 同一线程
 *   (2) 锁集相交（互斥，不可能同时进入临界区）
 */
export function mhp(a: Digest, b: Digest): boolean {
    if (a.tid === b.tid) return false;
    if (intersects(a.locks, b.locks)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 纠缠闭包（并查集）
// ---------------------------------------------------------------------------

/**
 * 把纠缠对集合膨胀为等价闭包。
 * 返回 Map：每个代表元的闭包内成员集合。
 * 纠缠是传递的：q1~q2 且 q2~q3 => q1~q3。
 */
export function entanglementClosure(
    pairs: ReadonlyArray<readonly [string, string]>,
): Map<string, Set<string>> {
    const parent = new Map<string, string>();

    const find = (x: string): string => {
        let root = x;
        while (parent.get(root) !== undefined && parent.get(root) !== root) {
            root = parent.get(root)!;
        }
        // 路径压缩
        let cur = x;
        while (parent.get(cur) !== undefined && parent.get(cur) !== cur) {
            const nxt = parent.get(cur)!;
            parent.set(cur, root);
            cur = nxt;
        }
        return root;
    };

    const union = (x: string, y: string): void => {
        const rx = find(x), ry = find(y);
        if (rx !== ry) parent.set(ry, rx);
    };

    for (const [a, b] of pairs) {
        if (!parent.has(a)) parent.set(a, a);
        if (!parent.has(b)) parent.set(b, b);
        union(a, b);
    }

    const groups = new Map<string, Set<string>>();
    for (const [a, b] of pairs) {
        const root = find(a);
        if (!groups.has(root)) groups.set(root, new Set());
        groups.get(root)!.add(a);
        groups.get(root)!.add(b);
    }
    return groups;
}

// ---------------------------------------------------------------------------
// 竞争检测
// ---------------------------------------------------------------------------

/** 经典竞争检测（锁集 + 线程 id digest） */
export function detectRaces(accesses: Access[]): Race[] {
    const races: Race[] = [];
    for (let i = 0; i < accesses.length; i++) {
        for (let j = i + 1; j < accesses.length; j++) {
            const a = accesses[i], b = accesses[j];
            if (a.variable !== b.variable) continue;
            if (!a.isWrite && !b.isWrite) continue;       // 双读无竞争
            if (!mhp(a.digest, b.digest)) continue;        // 必串行
            races.push({
                variable: a.variable,
                threadA: a.digest.tid,
                threadB: b.digest.tid,
                quantum: false,
                reason: 'may happen in parallel and at least one is a write',
                line: a.line,
                column: a.column,
            });
        }
    }
    return races;
}

/**
 * 量子竞争检测：跨线程的破坏性操作触及纠缠闭包内的同一量子比特。
 * 这是 no-cloning / 坍缩语义下的竞争：两个线程同时对纠缠比特做测量或释放。
 */
export function detectQuantumRaces(accesses: Access[]): Race[] {
    const races: Race[] = [];
    for (let i = 0; i < accesses.length; i++) {
        for (let j = i + 1; j < accesses.length; j++) {
            const a = accesses[i], b = accesses[j];
            if (!mhp(a.digest, b.digest)) continue;
            // 任一破坏性操作触及对方闭包内的量子比特 -> 可能并发坍缩
            const overlap =
                intersects(a.destructiveQubits, b.digest.qubits) ||
                intersects(b.destructiveQubits, a.digest.qubits);
            if (!overlap) continue;
            races.push({
                variable: 'qubit-closure',
                threadA: a.digest.tid,
                threadB: b.digest.tid,
                quantum: true,
                reason: 'destructive qubit operation may collide on an entangled closure without a common lock',
                line: a.line,
                column: a.column,
            });
        }
    }
    return races;
}