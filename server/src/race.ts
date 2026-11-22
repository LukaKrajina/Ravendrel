export interface Digest {
    tid: string;
    locks: ReadonlySet<string>;
    qubits: ReadonlySet<string>;
}

export interface Access {
    variable: string;
    isWrite: boolean;
    digest: Digest;
    destructiveQubits: ReadonlySet<string>;
    line: number;
    column: number;
}

export interface Race {
    variable: string;
    threadA: string;
    threadB: string;
    quantum: boolean;
    reason: string;
    line: number;
    column: number;
}


function intersects(a: ReadonlySet<string>, b: ReadonlySet<string>): boolean {
    for (const x of a) if (b.has(x)) return true;
    return false;
}








export function mhp(a: Digest, b: Digest): boolean {
    if (a.tid === b.tid) return false;
    if (intersects(a.locks, b.locks)) return false;
    return true;
}






export function entanglementClosure(
    pairs: ReadonlyArray<readonly [string, string]>,
): Map<string, Set<string>> {
    const parent = new Map<string, string>();

    const find = (x: string): string => {
        let root = x;
        while (parent.get(root) !== undefined && parent.get(root) !== root) {
            root = parent.get(root)!;
        }
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


export function detectRaces(accesses: Access[]): Race[] {
    const races: Race[] = [];
    for (let i = 0; i < accesses.length; i++) {
        for (let j = i + 1; j < accesses.length; j++) {
            const a = accesses[i], b = accesses[j];
            if (a.variable !== b.variable) continue;
            if (!a.isWrite && !b.isWrite) continue;
            if (!mhp(a.digest, b.digest)) continue;
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




export function detectQuantumRaces(accesses: Access[]): Race[] {
    const races: Race[] = [];
    for (let i = 0; i < accesses.length; i++) {
        for (let j = i + 1; j < accesses.length; j++) {
            const a = accesses[i], b = accesses[j];
            if (!mhp(a.digest, b.digest)) continue;
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