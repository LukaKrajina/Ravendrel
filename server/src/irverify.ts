// ============================================================================
// IR 自校验：对 ir.ts 生成的 LLVM IR 文本做结构化校验。
//
// 覆盖的不变量：
//   每个 `define` 函数含 `entry:` 基本块；
//   每个基本块以终结符（ret/br/switch/indirectbr/unreachable）结束；
//   SSA 支配性质：每个 `%N` 使用点之前必有 `%N = ...` 定义（按文本顺序近似）；
//   指令目标寄存器（`%N = ...`）不与既有定义重复（SSA 单赋值）。
//
// 目的是在生成期就捕获字符串拼错/基本块漏终结符等错误，而非让 daemon 的
// LLVM parse 兜底。
// ============================================================================

export interface IRDiagnostic {
    line: number;
    message: string;
}

const TERMINATORS = new Set(['ret', 'br', 'switch', 'indirectbr', 'unreachable', 'resume']);

// 命名类型（opaque / 晶格 / form / 闭包 / env）在函数体内以 `%Name*` 出现，不是寄存器。
const TYPE_NAMES = new Set(['Qubit', 'QObject', 'QModel', 'QReservoir', 'Lattice']);
const isTypeRef = (raw: string): boolean => {
    const name = raw.startsWith('%') ? raw.slice(1) : raw;
    return TYPE_NAMES.has(name) || name.startsWith('form.') || name.startsWith('closure.') || name.startsWith('env.');
};

/** 支配分析所需的基本块视图（由 verifyIR 在扫描时填充） */
interface IRBlock {
    name: string;
    /** 块内定义的寄存器 → 首次定义行号 */
    defs: Map<string, number>;
    /** 块内的寄存器使用 */
    uses: { reg: string; line: number }[];
    /** 后继块名（从终结符 br/switch 的 `label %X` 提取） */
    succs: string[];
}

/**
 * SSA 支配性质检查（真正的支配关系，而非「按文本顺序」近似）。
 *
 * 不变量：寄存器在其定义块**支配**的所有块中可见。若使用点所在块不被定义块
 * 支配，即为真正的 SSA 违规 —— 典型情形是「某分支内定义、另一分支未定义，
 * 却在汇合块使用该寄存器」，此时按文本顺序检查会漏报（定义文本上确实靠前）。
 *
 * 为降低误报：
 *   - 仅在可达块上检查（不可达块的支配集无意义）；
 *   - 同块内的使用交给既有的文本顺序检查（此处跳过）；
 *   - 函数参数（%argN / %env）不在任何块内定义，此处跳过（它们支配所有块）。
 */
function checkDominance(blocks: IRBlock[], fnName: string): IRDiagnostic[] {
    const diags: IRDiagnostic[] = [];
    const n = blocks.length;
    if (n === 0) return diags;

    const index = new Map<string, number>();
    blocks.forEach((b, i) => index.set(b.name, i));

    // 前驱边
    const preds: number[][] = blocks.map(() => []);
    for (let i = 0; i < n; i++) {
        for (const s of blocks[i].succs) {
            const j = index.get(s);
            if (j !== undefined) preds[j].push(i);
        }
    }

    const entryIdx = index.get('entry') ?? 0;

    // 可达块（从 entry 出发）
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

    // 支配集迭代求不动点：
    //   Dom(entry) = {entry}；Dom(b) = {b} ∪ ⋂_{p ∈ preds(b)} Dom(p)
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

    // 寄存器 → 首次定义所在块
    const defBlock = new Map<string, number>();
    for (let i = 0; i < n; i++) {
        for (const r of blocks[i].defs.keys()) if (!defBlock.has(r)) defBlock.set(r, i);
    }

    for (let i = 0; i < n; i++) {
        if (!reachable.has(i)) continue; // 不可达块不检查
        for (const u of blocks[i].uses) {
            const d = defBlock.get(u.reg);
            if (d === undefined) continue;  // 参数 / 未在块内定义 → 既有检查负责
            if (d === i) continue;          // 同块 → 文本顺序检查负责
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

    // 第一遍：收集所有基本块标签名（用于区分 `br label %X` 中的标签 vs 寄存器）
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

    // ── 支配分析：当前函数的基本块视图（函数结束时交给 checkDominance）──
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
        // 一个基本块结束（下一个 label 或函数结束）时，校验终结符
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

        // 函数定义：define [linkage] ret @name(...) {
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
            // 函数参数（%arg0, %arg1, ...）在函数入口即已定义，加入 definedRegs，
            // 否则参数在 store 到 alloca 时会被误报 "used before definition"。
            // 精确匹配 %argN 参数名，避免误匹配命名类型（如 %Qubit*）。
            // lambda/spawn 线程函数的闭包环境参数 %env 也视为已定义。
            const args = s.match(/%arg\d+/g);
            if (args) for (const a of args) definedRegs.add(a);
            if (s.includes('%env')) definedRegs.add('%env');
            continue;
        }

        // declare 等全局声明：跳过
        if (s.startsWith('declare ') || s.startsWith('@') || s.startsWith('source_filename') || s.startsWith(';') || s.startsWith('target ') || s.startsWith('attributes ') || s.startsWith('module ') || s.startsWith('!')) {
            continue;
        }

        // 类型定义 / 全局变量：跳过
        if (s.startsWith('%') && s.includes('= type ') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= private') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= constant') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= global') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= linkonce') && !inFunction) continue;
        if (s.startsWith('@') && s.includes('= internal') && !inFunction) continue;

        if (!inFunction) continue;

        // 函数结束
        if (s === '}') {
            flushBlock(line);
            if (!hasEntry) {
                diagnostics.push({ line: fnStartLine, message: `function '${fnName}' is missing an 'entry:' block` });
            }
            // 真正的支配检查（补充「按文本顺序」近似的漏报）
            diagnostics.push(...checkDominance(blocks, fnName));
            inFunction = false;
            continue;
        }

        // 基本块标签：label:
        const labelMatch = s.match(/^([\w.]+):$/);
        if (labelMatch) {
            flushBlock(line);
            if (labelMatch[1] === 'entry') hasEntry = true;
            blockHasInstruction = false;
            blockTerminated = false;
            // 开启新块（供支配分析）
            curBlock = { name: labelMatch[1], defs: new Map(), uses: [], succs: [] };
            blocks.push(curBlock);
            continue;
        }

        // 指令
        const instruction = s;
        const firstToken = instruction.split(/\s+/)[0];

        // 定义形如 %N = ... ：SSA 单赋值检查 + 记录定义
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

        // 使用：扫描指令中的 %N（排除定义位置本身与命名类型引用）。
        // 用 %[\w.]+ 匹配，使 `%env.XXX`/`%closure.XXX` 等带点类型名作为整体识别，
        // 而非被拆成 `%env` 误判为寄存器。
        // 先剥离双引号字符串内容（内联汇编模板 / 约束串里的 %cr3、%rsp 等 x86 寄存器
        // 不应被当作 LLVM SSA 寄存器），再扫描寄存器引用。
        const strippedInstruction = instruction.replace(/"[^"]*"/g, '""');
        const uses = strippedInstruction.match(/%[\w.]+/g) ?? [];
        for (const u of uses) {
            if (u === definedName) continue; // 定义位置
            if (isTypeRef(u)) continue;       // 命名类型（%Qubit* 等），非寄存器
            if (allLabels.has(u.slice(1))) continue; // 基本块标签（br label %X）
            curBlock?.uses.push({ reg: u, line });   // 供支配分析
            if (!definedRegs.has(u)) {
                diagnostics.push({ line, message: `SSA violation: register '${u}' used before definition in function '${fnName}'` });
            }
        }

        // 终结符判定
        if (TERMINATORS.has(firstToken)) {
            blockTerminated = true;
            // 提取后继（br label %X / br i1 %c, label %A, label %B / switch ... label %D）
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