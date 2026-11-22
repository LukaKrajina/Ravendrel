export interface IRDiagnostic {
    line: number;
    message: string;
}

const TERMINATORS = new Set(['ret', 'br', 'switch', 'indirectbr', 'unreachable', 'resume']);

const TYPE_NAMES = new Set(['Qubit', 'QObject', 'QModel', 'QReservoir', 'Lattice']);
const isTypeRef = (raw: string): boolean => {
    const name = raw.startsWith('%') ? raw.slice(1) : raw;
    return TYPE_NAMES.has(name) || name.startsWith('form.') || name.startsWith('closure.') || name.startsWith('env.');
};

interface IRBlock {
    name: string;
    defs: Map<string, number>;
    uses: { reg: string; line: number }[];
    succs: string[];
}












function checkDominance(blocks: IRBlock[], fnName: string): IRDiagnostic[] {
    const diags: IRDiagnostic[] = [];
    const n = blocks.length;
    if (n === 0) return diags;

    const index = new Map<string, number>();
    blocks.forEach((b, i) => index.set(b.name, i));

    const preds: number[][] = blocks.map(() => []);
    for (let i = 0; i < n; i++) {
        for (const s of blocks[i].succs) {
            const j = index.get(s);
            if (j !== undefined) preds[j].push(i);
        }
    }

    const entryIdx = index.get('entry') ?? 0;

    const reachable = new Set<number>();
    const stack: number[] = [entryIdx];
    while (stack.length) {
        const v = stack.pop()!;
        if (reachable.has(v)) continue;
        reachable.add(v);
        for (const s of blocks[v].succs) {
            const j = index.get(s);
            if (j !== undefined && !reachable.has(j)) stack.push(j);
        }
    }

    const all = new Set<number>();
    for (let i = 0; i < n; i++) all.add(i);
    const dom: Set<number>[] = blocks.map((_, i) => new Set(i === entryIdx ? [entryIdx] : all));

    let changed = true;
    while (changed) {
        changed = false;
        for (let i = 0; i < n; i++) {
            if (i === entryIdx) continue;
            let inter: Set<number> | null = null;
            for (const p of preds[i]) {
                if (inter === null) inter = new Set(dom[p]);
                else for (const v of Array.from(inter)) if (!dom[p].has(v)) inter.delete(v);
            }
            const next = new Set<number>(inter ?? new Set<number>());
            next.add(i);
            if (next.size !== dom[i].size || Array.from(next).some(v => !dom[i].has(v))) {
                dom[i] = next;
                changed = true;
            }
        }
    }

    const defBlock = new Map<string, number>();
    for (let i = 0; i < n; i++) {
        for (const r of blocks[i].defs.keys()) if (!defBlock.has(r)) defBlock.set(r, i);
    }

    for (let i = 0; i < n; i++) {
        if (!reachable.has(i)) continue;
        for (const u of blocks[i].uses) {
            const d = defBlock.get(u.reg);
            if (d === undefined) continue;
            if (d === i) continue;
            if (!dom[i].has(d)) {
                diags.push({
                    line: u.line,
                    message: `SSA violation: register '${u.reg}' defined in block '${blocks[d].name}' ` +
                        `does not dominate its use in block '${blocks[i].name}' in function '${fnName}'`,
                });
            }
        }
    }
    return diags;
}

export function verifyIR(ir: string): IRDiagnostic[] {
    const diagnostics: IRDiagnostic[] = [];
    const lines = ir.split('\n');

    const allLabels = new Set<string>();
    for (const raw of lines) {
        const s = raw.trim();
        const lm = s.match(/^([\w.]+):$/);
        if (lm && lm[1] !== 'entry') allLabels.add(lm[1]);
    }

    let inFunction = false;
    let fnName = '';
    let fnStartLine = 0;
    let hasEntry = false;
    let blockTerminated = false;
    let blockHasInstruction = false;
    const definedRegs = new Set<string>();

    let blocks: IRBlock[] = [];
    let curBlock: IRBlock | null = null;
    const ensureBlock = (): IRBlock => {
        if (!curBlock) {
            curBlock = {
                name: blocks.length === 0 ? 'entry' : `__implicit_${blocks.length}`,
                defs: new Map(), uses: [], succs: [],
            };
            blocks.push(curBlock);
        }
        return curBlock;
    };

    const flushBlock = (line: number) => {
        if (!blockTerminated && blockHasInstruction) {
            diagnostics.push({
                line,
                message: `block in function '${fnName}' is not terminated (missing ret/br/...)`,
            });
        }
        blockTerminated = false;
        blockHasInstruction = false;
    };

    for (let i = 0; i < lines.length; i++) {
        const raw = lines[i];
        const line = i + 1;
        const s = raw.trim();

        const defMatch = s.match(/^define\s+[\w%*.\-\[\]]+\s+@([\w.]+)\s*\(/);
        if (defMatch) {
            if (inFunction) flushBlock(line);
            inFunction = true;
            fnName = defMatch[1];
            fnStartLine = line;
            hasEntry = false;
            blockTerminated = false;
            blockHasInstruction = false;
            definedRegs.clear();
            blocks = [];
            curBlock = null;
            const args = s.match(/%arg\d+/g);
            if (args) for (const a of args) definedRegs.add(a);
            if (s.includes('%env')) definedRegs.add('%env');
            continue;
        }

        if (s.startsWith('declare ') || s.startsWith('@') || s.startsWith('source_filename') || s.startsWith(';') || s.startsWith('target ') || s.startsWith('attributes ') || s.startsWith('module ') || s.startsWith('!')) {
            continue;
        }

        if (s.startsWith('%') && s.includes('= type ') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= private') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= constant') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= global') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= linkonce') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= internal') && !inFunction) continue;

        if (!inFunction) continue;

        if (s === '}') {
            flushBlock(line);
            if (!hasEntry) {
                diagnostics.push({ line: fnStartLine, message: `function '${fnName}' is missing an 'entry:' block` });
            }
            diagnostics.push(...checkDominance(blocks, fnName));
            inFunction = false;
            continue;
        }

        const labelMatch = s.match(/^([\w.]+):$/);
        if (labelMatch) {
            flushBlock(line);
            if (labelMatch[1] === 'entry') hasEntry = true;
            blockHasInstruction = false;
            blockTerminated = false;
            curBlock = { name: labelMatch[1], defs: new Map(), uses: [], succs: [] };
            blocks.push(curBlock);
            continue;
        }

        const instruction = s;
        const firstToken = instruction.split(/\s+/)[0];

        const dm = instruction.match(/^(%\w+)\s*=/);
        let definedName = '';
        if (dm) {
            definedName = dm[1];
            if (definedRegs.has(definedName)) {
                diagnostics.push({ line, message: `SSA violation: register '${definedName}' defined more than once` });
            }
            definedRegs.add(definedName);
            const b = ensureBlock();
            if (!b.defs.has(definedName)) b.defs.set(definedName, line);
        } else {
            ensureBlock();
        }

        const strippedInstruction = instruction.replace(/"[^"]*"/g, '""');
        const uses = strippedInstruction.match(/%[\w.]+/g) ?? [];
        for (const u of uses) {
            if (u === definedName) continue;
            if (isTypeRef(u)) continue;
            if (allLabels.has(u.slice(1))) continue;
            curBlock?.uses.push({ reg: u, line });
            if (!definedRegs.has(u)) {
                diagnostics.push({ line, message: `SSA violation: register '${u}' used before definition in function '${fnName}'` });
            }
        }

        if (TERMINATORS.has(firstToken)) {
            blockTerminated = true;
            if (curBlock) {
                const succs = s.match(/label\s+%([\w.]+)/g) ?? [];
                for (const m of succs) {
                    const name = m.replace(/label\s+%/, '');
                    if (!curBlock.succs.includes(name)) curBlock.succs.push(name);
                }
            }
        }
        blockHasInstruction = true;
    }

    if (inFunction) {
        flushBlock(lines.length);
        if (!hasEntry) {
            diagnostics.push({ line: fnStartLine, message: `function '${fnName}' is missing an 'entry:' block` });
        }
        diagnostics.push(...checkDominance(blocks, fnName));
    }

    return diagnostics;
}