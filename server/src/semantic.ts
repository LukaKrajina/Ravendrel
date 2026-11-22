import { Program, Statement, Expression, MemberExpression, Item, FormDecl, TraitDecl, ImplDecl, ModuleDecl, TemplateDecl, FnDecl, GlobalVarDecl } from "./ast";
import { buildMir } from "./mir";
import { BorrowChecker } from "./borrow";
import { detectRaces, detectQuantumRaces, entanglementClosure, Access, Digest } from "./race";
import { buildTopology } from "./topology";
import { synthesizeGates } from "./gate-synth";
import { Type, T, cap, func, array, parseType, typeToString, typeEquals, isAssignable, isNumeric, isNumericCoercible, isConditionType } from "./types";




function isDoubleBuffer(t: Type): boolean {
    if (t.kind === 'cap') return t.inner.kind === 'float' && t.inner.width === 64;
    if (t.kind === 'array') return t.elem.kind === 'float' && t.elem.width === 64;
    return false;
}

function isTopLevelItem(node: any): node is Item {
    if (node.type === 'VariableDeclarationList') {
        return node.varType !== 'auto' && node.varType !== 'let';
    }
    return ['ModuleDecl', 'UseDecl', 'FormDecl', 'FlavorDecl', 'ImplDecl', 'TraitDecl', 'TemplateDecl', 'ImportDecl', 'RequiresDecl', 'ExternDecl', 'GlobalVarDecl'].includes(node.type);
}

const GATE_NAMES = new Set(['h', 'x', 'y', 'z', 'rz', 'rx', 'ry', 's', 't',
    'cnot', 'toffoli', 'swap', 'qft', 'iqft', 'braid',
    'measure_x', 'measure_y',
    'cx', 'ch', 'crz', 'cswap', 'c_toffoli', 'cqft', 'cbraid']);

export interface SemanticError {
    message: string;
    line: number;
    column: number;
    length: number;
}

export class SemanticAnalyzer {
    private symbolMap: Map<string, Type> = new Map();
    public errors: SemanticError[] = [];



    public warnings: SemanticError[] = [];
    private forms: Map<string, FormDecl> = new Map();
    private traits: Map<string, TraitDecl> = new Map();
    private impls: ImplDecl[] = [];
    private modules: Map<string, ModuleDecl> = new Map();
    private templates: TemplateDecl[] = [];
    private parentOf: Map<string, string> = new Map();
    private childOf: Map<string, string> = new Map();
    private loopDepth: number = 0;
    private currentReturnType: Type = T.void;
    private measuredQubits: Set<string> = new Set();
    private measuredQubitScopes: Set<string>[] = [];
    private declaredGateVars: Set<string> = new Set();
    private usedGates: Set<string> = new Set();
    private fixedSymbols: Set<string> = new Set();
    private flavorMembers: Map<string, number> = new Map();
    private externSigs: Map<string, { params: Type[]; ret: Type }> = new Map();
    private userFuncSigs: Map<string, { params: Type[]; ret: Type }> = new Map();
    private globalSymbols: Map<string, Type> = new Map();

    private resetDeclarations() {
        this.forms.clear();
        this.traits.clear();
        this.impls = [];
        this.modules.clear();
        this.templates = [];
        this.parentOf.clear();
        this.childOf.clear();
        this.externSigs.clear();
        this.userFuncSigs.clear();
    }

    private collectDeclarations(items: (Statement | Item)[], prefix: string) {
        for (const node of items) {
            if (node.type === 'FormDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                if (this.forms.has(fullName)) {
                    this.errors.push({ message: `Duplicate form '${fullName}'.`, line: node.line, column: node.column, length: node.name.length });
                }
                this.forms.set(fullName, node);
            } else if (node.type === 'FlavorDecl') {
                let next = 0;
                for (const m of node.members) {
                    const value = m.value ?? next;
                    this.flavorMembers.set(m.name, value);
                    next = value + 1;
                }
            } else if (node.type === 'TraitDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                if (this.traits.has(fullName)) {
                    this.errors.push({ message: `Duplicate trait '${fullName}'.`, line: node.line, column: node.column, length: node.name.length });
                }
                this.traits.set(fullName, node);
            } else if (node.type === 'ImplDecl') {
                this.impls.push(node);
            } else if (node.type === 'TemplateDecl') {
                this.templates.push(node);
            } else if (node.type === 'ModuleDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.modules.set(fullName, node);
                this.collectDeclarations(node.body, fullName);
            } else if (node.type === 'ExternDecl') {
                this.externSigs.set(node.name, { params: node.params.map(p => parseType(p.type)), ret: parseType(node.returnType) });
            } else if (node.type === 'FunctionDeclaration') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.userFuncSigs.set(fullName, {
                    params: node.params.map(p => parseType(p.type)),
                    ret: parseType(node.returnType),
                });
            } else if (node.type === 'GlobalVarDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                if (this.globalSymbols.has(fullName)) {
                    this.errors.push({ message: `Duplicate global '${fullName}'.`, line: node.line, column: node.column, length: node.name.length });
                }
                this.globalSymbols.set(fullName, parseType(node.varType));
                if (node.isFixed) this.fixedSymbols.add(fullName);
            } else if (node.type === 'VariableDeclarationList') {
                if (node.varType === 'auto' || node.varType === 'let') continue;
                for (const d of node.declarations) {
                    const fullName = prefix ? prefix + '::' + d.identifier : d.identifier;
                    if (this.globalSymbols.has(fullName)) {
                        this.errors.push({ message: `Duplicate global '${fullName}'.`, line: d.line, column: d.column, length: d.identifier.length });
                    }
                    this.globalSymbols.set(fullName, parseType(d.varType));
                    if (d.isFixed) this.fixedSymbols.add(fullName);
                }
            }
        }
    }

    private checkInheritance() {
        for (const [name, form] of this.forms) {
            if (form.inherits) this.checkSingleInherit(name, form.inherits.base, form.inherits.ranks, 'form', form);
        }
        for (const [name, trait] of this.traits) {
            if (trait.inherits) this.checkSingleInherit(name, trait.inherits.base, trait.inherits.ranks, 'trait', trait);
        }
    }

    private checkSingleInherit(child: string, parent: string, ranks: string[], kind: string, decl: FormDecl | TraitDecl) {
        if (this.parentOf.has(child)) {
            this.errors.push({ message: `Inheritance Error: '${child}' already inherits '${this.parentOf.get(child)}'. Single inheritance only.`, line: decl.line, column: decl.column, length: child.length });
        }

        if (this.childOf.has(parent)) {
            this.errors.push({ message: `Inheritance Error: '${parent}' is already inherited by '${this.childOf.get(parent)}'. Each ${kind} can be inherited only once.`, line: decl.line, column: decl.column, length: parent.length });
        }

        const parentExists = this.forms.has(parent) || this.traits.has(parent);
        if (!parentExists) {
            this.errors.push({ message: `Inheritance Error: undefined parent ${kind} '${parent}'.`, line: decl.line, column: decl.column, length: parent.length });
        }
        this.parentOf.set(child, parent);
        this.childOf.set(parent, child);

        if (ranks.length > 0) {
            const parentDecl = this.forms.get(parent) || this.traits.get(parent);
            if (parentDecl) {
                const parentRanks = new Set(parentDecl.ranks.map(r => r.name));
                for (const r of ranks) {
                    if (!parentRanks.has(r)) {
                        this.errors.push({ message: `Rank Error: parent '${parent}' has no rank '${r}'.`, line: decl.line, column: decl.column, length: r.length });
                    }
                }
            }
        }
    }

    private checkTraitImpls() {
        for (const impl of this.impls) {
            if (!impl.traitName) continue;
            const trait = this.traits.get(impl.traitName);
            if (!trait) {
                this.errors.push({ message: `Impl Error: undefined trait '${impl.traitName}'.`, line: impl.line, column: impl.column, length: impl.traitName.length });
                continue;
            }
            if (!this.forms.has(impl.target)) {
                this.errors.push({ message: `Impl Error: undefined form '${impl.target}'.`, line: impl.line, column: impl.column, length: impl.target.length });
                continue;
            }
            const traitMethods = this.collectTraitMethods(trait);
            const implNames = new Set(impl.methods.map(m => m.name));
            for (const tm of traitMethods) {
                if (!implNames.has(tm.name)) {
                    this.errors.push({ message: `Impl Error: missing implementation of trait method '${tm.name}' for form '${impl.target}'.`, line: impl.line, column: impl.column, length: tm.name.length });
                }
            }
        }
    }

    private collectTraitMethods(trait: TraitDecl): FnDecl[] {
        const methods: FnDecl[] = [];
        for (const rank of trait.ranks) {
            methods.push(...rank.methods);
        }
        return methods;
    }

    private findFieldType(formName: string, fieldName: string): Type | null {
        const form = this.forms.get(formName);
        if (!form) return null;
        if (form.inherits && form.inherits.base) {
            const parent = this.findFieldType(form.inherits.base, fieldName);
            if (parent) return parent;
        }
        for (const rank of form.ranks) {
            for (const f of rank.fields) {
                if (f.name === fieldName) return parseType(f.type);
            }
        }
        return null;
    }
    public analyze(program: Program) {
        this.symbolMap.clear();
        this.errors = [];
        this.measuredQubits.clear();
        this.measuredQubitScopes = [];
        this.declaredGateVars.clear();
        this.usedGates.clear();
        this.resetDeclarations();
        this.collectDeclarations(program.body, '');
        this.checkInheritance();
        this.checkTraitImpls();
        for (const node of program.body) {
            if (node.type === 'GlobalVarDecl') {
                const g = node as GlobalVarDecl;
                const exprType = this.visitExpression(g.value);
                const targetType = parseType(g.varType);
                if (exprType.kind !== 'unknown' && !isAssignable(targetType, exprType)) {
                    this.errors.push({
                        message: `Type Error: global '${g.name}' expects '${typeToString(targetType)}', got '${typeToString(exprType)}'.`,
                        line: g.line, column: g.column, length: g.name.length,
                    });
                }
            } else if (node.type === 'VariableDeclarationList' && node.varType !== 'auto' && node.varType !== 'let') {
                for (const d of node.declarations) {
                    const exprType = this.visitExpression(d.value);
                    const targetType = parseType(d.varType);
                    if (exprType.kind !== 'unknown' && !isAssignable(targetType, exprType)) {
                        this.errors.push({
                            message: `Type Error: global '${d.identifier}' expects '${typeToString(targetType)}', got '${typeToString(exprType)}'.`,
                            line: d.line, column: d.column, length: d.identifier.length,
                        });
                    }
                }
            }
        }
        program.body.forEach(stmt => {
            if (isTopLevelItem(stmt)) return;
            this.visitStatement(stmt as Statement);
        });

        this.runQuantumAttrs(program);

        this.runPhysicalAttrs(program);

        const synthWarnings = synthesizeGates(program);
        for (const w of synthWarnings) {
            this.errors.push({ message: w, line: 0, column: 0, length: 1 });
        }

        this.runBorrowCheck(program);

        this.runRaceDetection(program);

        this.runTopology(program);
    }





    private runQuantumAttrs(program: Program): void {
        for (const node of program.body) {
            if (node.type !== 'FunctionDeclaration') continue;
            const fn = node as any;
            const q = fn.quantum;
            if (!q) continue;

            const hasMeasure = this.containsMeasureCall(fn.body);

            if ((q.unitary || q.isGate) && hasMeasure) {
                const tag = q.unitary ? '@[unitary]' : '@[gate]';
                this.errors.push({
                    message: `[E-QUNI] ${tag} function '${fn.name}' must be reversible and cannot measure`,
                    line: fn.line, column: fn.column, length: 1,
                });
            }
            if ((q.undo || q.steer) && !q.isGate && !q.unitary) {
                this.errors.push({
                    message: `[E-QSYN] @[undo]/@[steer] on '${fn.name}' requires @[gate] or @[unitary]`,
                    line: fn.line, column: fn.column, length: 1,
                });
            }
        }
    }

    private containsMeasureCall(node: any): boolean {
        if (!node || typeof node !== 'object') return false;
        if (node.type === 'FunctionCall') {
            const n = node.name;
            if (n === 'measure' || n === 'measure_x' || n === 'measure_y') return true;
        }
        for (const key of Object.keys(node)) {
            const val = node[key];
            if (Array.isArray(val)) {
                for (const item of val) {
                    if (this.containsMeasureCall(item)) return true;
                }
            } else if (val && typeof val === 'object') {
                if (this.containsMeasureCall(val)) return true;
            }
        }
        return false;
    }





    private runPhysicalAttrs(program: Program): void {
        const NOISE_MODELS = new Set(['depolarizing', 'amplitude_damping', 'phase_damping', 'bit_flip']);
        for (const node of program.body) {
            if (node.type !== 'FunctionDeclaration') continue;
            const fn = node as any;
            const p = fn.physical;
            if (!p) continue;

            if (p.coherence) {
                const { t1, t2 } = p.coherence;
                if (t1 <= 0 || t2 <= 0 || t2 > t1) {
                    this.errors.push({
                        message: `[E-PHY] @[coherence] on '${fn.name}' requires 0 < T2 <= T1`,
                        line: fn.line, column: fn.column, length: 1,
                    });
                }
            }
            if (p.noise && !NOISE_MODELS.has(p.noise)) {
                this.errors.push({
                    message: `[E-PHY] @[noise] on '${fn.name}': unknown model '${p.noise}'`,
                    line: fn.line, column: fn.column, length: 1,
                });
            }
        }
    }




    private runTopology(program: Program): void {
        const { errors, warnings } = buildTopology(program);
        for (const e of errors) {
            this.errors.push({
                message: `[${e.code}] ${e.message}`,
                line: e.line,
                column: e.column,
                length: 1,
            });
        }
        for (const w of warnings) {
            this.warnings.push({
                message: `[${w.code}] ${w.message}`,
                line: w.line,
                column: w.column,
                length: 1,
            });
        }
    }





    private runBorrowCheck(program: Program): void {
        let mir;
        try {
            mir = buildMir(program);
        } catch (err: any) {
            console.warn(`[borrow] MIR lowering skipped: ${err?.message ?? err}`);
            return;
        }
        for (const body of mir.bodies) {
            for (const e of new BorrowChecker(body).check()) {
                this.errors.push({
                    message: `[${e.code}] ${e.message}`,
                    line: e.line,
                    column: e.column,
                    length: 1,
                });
            }
        }
    }











    private runRaceDetection(program: Program): void {
        const functions = program.body.filter(
            s => s.type === 'FunctionDeclaration' && Array.isArray((s as any).body) &&
            !(s as any).synthetic && !(s as any).quantum?.isGate
        ) as any[];
        if (functions.length === 0) return;

        interface RawAccess {
            tid: string;
            variable: string;
            isWrite: boolean;
            destructive: string[];
            locks: string[];
            line: number;
            column: number;
        }
        const classic: RawAccess[] = [];
        const quantum: RawAccess[] = [];
        const entanglePairs: [string, string][] = [];
        const threadQubits: Map<string, Set<string>> = new Map();
        let tidCounter = 0;

        const touchQubit = (tid: string, q: string): void => {
            if (q === 'mem') return;
            if (!threadQubits.has(tid)) threadQubits.set(tid, new Set());
            threadQubits.get(tid)!.add(q);
        };
        const varName = (e: any): string => (e && e.type === 'Identifier' ? e.name : 'mem');

        const threadLocks: Map<string, Set<string>> = new Map();
        const lockOf = (tid: string): Set<string> => {
            if (!threadLocks.has(tid)) threadLocks.set(tid, new Set());
            return threadLocks.get(tid)!;
        };
        const globalNames = new Set(this.globalSymbols.keys());
        const lockName = (e: any, argMap: Map<string, string>, tid: string): string => {
            if (e && e.type === 'Identifier') {
                if (globalNames.has(e.name)) return e.name;
                if (argMap.has(e.name)) return argMap.get(e.name)!;
                return tid + '.' + e.name;
            }
            return 'mem';
        };

        const lockEffects: Map<string, { acquires: string[]; releases: string[] }> = new Map();
        for (const fn of functions) {
            const acquires: string[] = [];
            const releases: string[] = [];
            const collectLockOps = (stmts: any[]): void => {
                for (const s of stmts) {
                    if (!s) continue;
                    if (s.type === 'ExpressionStatement' && s.expression && s.expression.type === 'FunctionCall') {
                        const n = s.expression.name;
                        if (n === 'sync_lock') acquires.push(varName(s.expression.arguments[0]));
                        else if (n === 'sync_unlock') releases.push(varName(s.expression.arguments[0]));
                    }
                    for (const k of ['body', 'consequent', 'alternate', 'elseBody', 'fallback']) {
                        if (Array.isArray(s[k])) collectLockOps(s[k]);
                    }
                    if (s.cases) for (const c of s.cases) if (c.body) collectLockOps(c.body);
                }
            };
            collectLockOps(fn.body);
            lockEffects.set(fn.name, { acquires, releases });
        }

        const GATES = new Set(['h', 'x', 'y', 'z', 'rz', 'rx', 'ry', 's', 't',
            'cnot', 'toffoli', 'swap', 'qft', 'braid']);
        const MEASURES = new Set(['measure', 'measure_x', 'measure_y']);
        const SYNC = new Set(['sync_load', 'sync_store', 'sync_add', 'sync_cas']);

        const walkExpr = (expr: any, tid: string, argMap: Map<string, string>): void => {
            if (!expr) return;
            switch (expr.type) {
                case 'FunctionCall': {
                    const name = expr.name;
                    if (name === 'sync_lock') lockOf(tid).add(lockName(expr.arguments[0], argMap, tid));
                    if (name === 'sync_unlock') lockOf(tid).delete(lockName(expr.arguments[0], argMap, tid));
                    if (lockEffects.has(name)) {
                        const eff = lockEffects.get(name)!;
                        const fnDef = functions.find(f => f.name === name);
                        const map = new Map<string, string>();
                        if (fnDef) {
                            fnDef.params.forEach((p: any, i: number) => {
                                if (expr.arguments[i]) map.set(p.name, lockName(expr.arguments[i], argMap, tid));
                            });
                        }
                        for (const l of eff.acquires) lockOf(tid).add(map.get(l) ?? l);
                        for (const l of eff.releases) lockOf(tid).delete(map.get(l) ?? l);
                    }
                    if (SYNC.has(name)) {
                        classic.push({
                            tid,
                            variable: varName(expr.arguments[0]),
                            isWrite: name !== 'sync_load',
                            destructive: [],
                            locks: [...lockOf(tid)],
                            line: expr.line,
                            column: expr.column,
                        });
                    }
                    if (MEASURES.has(name)) {
                        const q = varName(expr.arguments[0]);
                        touchQubit(tid, q);
                        quantum.push({ tid, variable: 'qubit:' + q, isWrite: true, destructive: [q], locks: [...lockOf(tid)], line: expr.line, column: expr.column });
                    }
                    if (GATES.has(name)) {
                        for (const a of expr.arguments) touchQubit(tid, varName(a));
                        quantum.push({
                            tid,
                            variable: 'qubit:' + varName(expr.arguments[0]),
                            isWrite: false,
                            destructive: [],
                            locks: [...lockOf(tid)],
                            line: expr.line,
                            column: expr.column,
                        });
                    }
                    expr.arguments.forEach((a: any) => walkExpr(a, tid, argMap));
                    return;
                }
                case 'BinaryExpression': walkExpr(expr.left, tid, argMap); walkExpr(expr.right, tid, argMap); return;
                case 'LogicalExpression': walkExpr(expr.left, tid, argMap); walkExpr(expr.right, tid, argMap); return;
                case 'UnaryExpression': walkExpr(expr.argument, tid, argMap); return;
                case 'Dereference': walkExpr(expr.target, tid, argMap); return;
                case 'AddressOf': walkExpr(expr.target, tid, argMap); return;
                case 'MemberExpression': walkExpr(expr.object, tid, argMap); expr.arguments.forEach((a: any) => walkExpr(a, tid, argMap)); return;
                case 'NewExpression': expr.arguments.forEach((a: any) => walkExpr(a, tid, argMap)); return;
                case 'IndexExpression': walkExpr(expr.object, tid, argMap); expr.indices.forEach((a: any) => walkExpr(a, tid, argMap)); return;
                case 'FuseExpression':
                    walkExpr(expr.discriminant, tid, argMap);
                    expr.arms.forEach((arm: any) => { walkExpr(arm.pattern, tid, argMap); walkExpr(arm.value, tid, argMap); });
                    return;
                case 'FunctionExpression': walkStmts(expr.body, tid, argMap); return;
                default: return;
            }
        };

        const walkStmts = (stmts: any[], tid: string, argMap: Map<string, string>): void => {
            for (const s of stmts) {
                if (!s) continue;
                switch (s.type) {
                    case 'SpawnStatement': walkStmts(s.body, 't' + (++tidCounter), new Map()); break;
                    case 'EntangleStatement': {
                        const l = varName(s.left), r = varName(s.right);
                        if (l !== 'mem' && r !== 'mem') entanglePairs.push([l, r]);
                        touchQubit(tid, l); touchQubit(tid, r);
                        walkExpr(s.left, tid, argMap); walkExpr(s.right, tid, argMap);
                        break;
                    }
                    case 'VariableDeclaration': walkExpr(s.value, tid, argMap); break;
                    case 'VariableDeclarationList': for (const d of s.declarations) walkExpr(d.value, tid, argMap); break;
                    case 'ExpressionStatement': walkExpr(s.expression, tid, argMap); break;
                    case 'AssignmentStatement': walkExpr(s.target, tid, argMap); walkExpr(s.value, tid, argMap); break;
                    case 'ReturnStatement': walkExpr(s.argument, tid, argMap); break;
                    case 'IfStatement': walkExpr(s.condition, tid, argMap); walkStmts(s.consequent, tid, argMap); if (s.alternate) walkStmts(s.alternate, tid, argMap); break;
                    case 'WhileStatement': walkExpr(s.condition, tid, argMap); walkStmts(s.body, tid, argMap); if (s.elseBody) walkStmts(s.elseBody, tid, argMap); break;
                    case 'ForStatement': if (s.init) walkStmts([s.init], tid, argMap); if (s.condition) walkExpr(s.condition, tid, argMap); walkStmts(s.body, tid, argMap); if (s.update) walkStmts([s.update], tid, argMap); break;
                    case 'RouteStatement': walkExpr(s.discriminant, tid, argMap); s.cases.forEach((c: any) => { walkExpr(c.value, tid, argMap); walkStmts(c.body, tid, argMap); }); if (s.fallback) walkStmts(s.fallback, tid, argMap); break;
                    case 'SpinStatement': walkStmts(s.body, tid, argMap); walkExpr(s.condition, tid, argMap); break;
                    case 'UnsafeBlock': walkStmts(s.body, tid, argMap); break;
                    default: break;
                }
            }
        };

        for (const fn of functions) {
            walkStmts(fn.body, fn.name, new Map());
        }

        const closure = entanglementClosure(entanglePairs);
        const closureOf = (q: string): Set<string> => {
            for (const g of closure.values()) if (g.has(q)) return g;
            return new Set([q]);
        };

        const threadClosure: Map<string, Set<string>> = new Map();
        for (const [tid, qs] of threadQubits) {
            const acc = new Set<string>();
            for (const q of qs) closureOf(q).forEach(x => acc.add(x));
            threadClosure.set(tid, acc);
        }

        const makeDigest = (tid: string, locks: string[]): Digest => ({
            tid,
            locks: new Set(locks),
            qubits: threadClosure.get(tid) ?? new Set<string>(),
        });

        const classicAccesses: Access[] = classic.map(a => ({
            variable: a.variable,
            isWrite: a.isWrite,
            digest: makeDigest(a.tid, a.locks),
            destructiveQubits: new Set<string>(),
            line: a.line,
            column: a.column,
        }));
        const quantumAccesses: Access[] = quantum.map(a => ({
            variable: a.variable,
            isWrite: a.isWrite,
            digest: makeDigest(a.tid, a.locks),
            destructiveQubits: new Set(a.destructive),
            line: a.line,
            column: a.column,
        }));

        for (const r of detectRaces(classicAccesses)) {
            this.errors.push({
                message: `Race Warning [Q-Digest]: shared '${r.variable}' accessed by '${r.threadA}' and '${r.threadB}' without a common lock (${r.reason}).`,
                line: r.line, column: r.column, length: 1,
            });
        }
        for (const r of detectQuantumRaces(quantumAccesses)) {
            this.errors.push({
                message: `Quantum Race [Q-Digest]: ${r.reason} (threads '${r.threadA}' vs '${r.threadB}').`,
                line: r.line, column: r.column, length: 1,
            });
        }
    }

    private visitStatement(stmt: Statement) {
        if (stmt.type === 'VariableDeclarationList') {
            for (const d of stmt.declarations) this.visitStatement(d);
            return;
        }
        if (stmt.type === 'VariableDeclaration') {
            const exprType = this.visitExpression(stmt.value);

            const inferredType: Type = stmt.varType === 'auto' ? exprType : parseType(stmt.varType);

            if (stmt.varType !== 'auto' && !isAssignable(inferredType, exprType)) {
                this.errors.push({
                    message: `Type Error: Cannot assign expression of type '${typeToString(exprType)}' to variable of type '${stmt.varType}'.`,
                    line: stmt.value.line,
                    column: stmt.value.column,
                    length: stmt.value.length
                });
            }

            if (stmt.value.type === 'Identifier') {
                const sourceVarType = this.symbolMap.get(stmt.value.name);
                if (sourceVarType && sourceVarType.kind === 'quantum' && sourceVarType.cls === 'Qubit') {
                    this.errors.push({
                        message: `Quantum Violation: Cannot copy Qubit '${stmt.value.name}'. Ravendrel statically enforces the No-Cloning Theorem.`,
                        line: stmt.value.line,
                        column: stmt.value.column,
                        length: stmt.value.length
                    });
                }
            }

            this.symbolMap.set(stmt.identifier, inferredType);
            if (stmt.isFixed) {
                this.fixedSymbols.add(stmt.identifier);
            }

            if (GATE_NAMES.has(stmt.identifier)) {
                this.declaredGateVars.add(stmt.identifier);
                if (this.usedGates.has(stmt.identifier)) {
                    this.errors.push({
                        message: `Ambiguity Warning: '${stmt.identifier}' is used both as a quantum gate and as a variable.`,
                        line: stmt.line,
                        column: stmt.column,
                        length: stmt.identifier.length
                    });
                }
            }
        }
        else if (stmt.type === 'ExpressionStatement') {
            this.visitExpression(stmt.expression);
        }
        else if (stmt.type === 'AssignmentStatement') {
            if (stmt.target) {
                if (stmt.target.type === 'Dereference') {
                    this.visitExpression(stmt.target.target);
                    this.visitExpression(stmt.value);
                    return;
                }
                if (stmt.target.type === 'IndexExpression') {
                    const elemType = this.visitExpression(stmt.target);
                    const valType = this.visitExpression(stmt.value);
                    if (elemType.kind !== 'unknown' && valType.kind !== 'unknown' && !typeEquals(elemType, valType)) {
                        this.errors.push({
                            message: `Type Error: Cannot assign '${typeToString(valType)}' to lattice element of type '${typeToString(elemType)}'.`,
                            line: stmt.line,
                            column: stmt.column,
                            length: 1
                        });
                    }
                    return;
                }
                const objType = this.visitExpression((stmt.target as MemberExpression).object);
                this.visitExpression(stmt.value);
                if (objType.kind === 'unknown') {
                    this.errors.push({
                        message: `Reference Error: Undefined object in field assignment.`,
                        line: stmt.line,
                        column: stmt.column,
                        length: 1
                    });
                }
                return;
            }
            const exprType = this.visitExpression(stmt.value);
            if (this.fixedSymbols.has(stmt.name)) {
                this.errors.push({
                    message: `Type Error: cannot reassign fixed value '${stmt.name}'.`,
                    line: stmt.line,
                    column: stmt.column,
                    length: stmt.name.length
                });
            }
            const targetType = this.symbolMap.get(stmt.name) ?? this.globalSymbols.get(stmt.name);
            if (!targetType) {
                this.errors.push({
                    message: `Reference Error: Undefined variable '${stmt.name}'.`,
                    line: stmt.line,
                    column: stmt.column,
                    length: stmt.name.length
                });
            } else if (!isAssignable(targetType, exprType) && exprType.kind !== 'unknown') {
                this.errors.push({
                    message: `Type Error: Cannot reassign variable '${stmt.name}' of type '${typeToString(targetType)}' to '${typeToString(exprType)}'.`,
                    line: stmt.value.line,
                    column: stmt.value.column,
                    length: stmt.value.length
                });
            }
        }
        else if (stmt.type === 'WhileStatement') {
            const condType = this.visitExpression(stmt.condition);
            if (!isConditionType(condType)) {
                this.errors.push({
                    message: `Type Error: while condition must be boolean/numeric, got '${typeToString(condType)}'.`,
                    line: stmt.condition.line,
                    column: stmt.condition.column,
                    length: stmt.condition.length
                });
            }
            if (stmt.invariant) {
                for (const inv of stmt.invariant) {
                    const t = this.visitExpression(inv);
                    if (!isConditionType(t)) {
                        this.errors.push({
                            message: `Contract Error: 'invariant' condition must be boolean, got '${typeToString(t)}'.`,
                            line: inv.line,
                            column: inv.column,
                            length: inv.length
                        });
                    }
                }
            }
            this.loopDepth++;
            stmt.body.forEach(s => this.visitStatement(s));
            this.loopDepth--;
            if (stmt.elseBody) {
                stmt.elseBody.forEach(s => this.visitStatement(s));
            }
        }
        else if (stmt.type === 'ForStatement') {
            if (stmt.init) this.visitStatement(stmt.init);
            if (stmt.condition) {
                const condType = this.visitExpression(stmt.condition);
                if (!isConditionType(condType)) {
                    this.errors.push({
                        message: `Type Error: for condition must be boolean/numeric, got '${typeToString(condType)}'.`,
                        line: stmt.condition.line,
                        column: stmt.condition.column,
                        length: stmt.condition.length
                    });
                }
            }
            this.loopDepth++;
            stmt.body.forEach(s => this.visitStatement(s));
            this.loopDepth--;
            if (stmt.update) this.visitStatement(stmt.update);
        }
        else if (stmt.type === 'IfStatement') {
            const condType = this.visitExpression(stmt.condition);
            if (!isConditionType(condType)) {
                this.errors.push({
                    message: `Type Error: if condition must be boolean/numeric, got '${typeToString(condType)}'.`,
                    line: stmt.condition.line,
                    column: stmt.condition.column,
                    length: stmt.condition.length
                });
            }
            stmt.consequent.forEach(s => this.visitStatement(s));
            if (stmt.alternate) {
                stmt.alternate.forEach(s => this.visitStatement(s));
            }
        }
        else if (stmt.type === 'UnsafeBlock') {
            stmt.body.forEach(s => this.visitStatement(s));
        }
        else if (stmt.type === 'RouteStatement') {
            this.visitExpression(stmt.discriminant);
            stmt.cases.forEach(c => {
                this.visitExpression(c.value);
                c.body.forEach(s => this.visitStatement(s));
            });
            if (stmt.fallback) {
                stmt.fallback.forEach(s => this.visitStatement(s));
            }
        }
        else if (stmt.type === 'SpinStatement') {
            const condType = this.visitExpression(stmt.condition);
            if (!isConditionType(condType)) {
                this.errors.push({
                    message: `Type Error: spin condition must be boolean/numeric, got '${typeToString(condType)}'.`,
                    line: stmt.condition.line,
                    column: stmt.condition.column,
                    length: stmt.condition.length
                });
            }
            this.loopDepth++;
            stmt.body.forEach(s => this.visitStatement(s));
            this.loopDepth--;
        }
        else if (stmt.type === 'SpawnStatement') {
            stmt.body.forEach(s => this.visitStatement(s));
        }
        else if (stmt.type === 'EntangleStatement') {
            for (const operand of [stmt.left, stmt.right]) {
                const t = this.visitExpression(operand);
                if (!(t.kind === 'quantum' && t.cls === 'Qubit') && t.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'entangle' expects Qubit operands, got '${typeToString(t)}'.`,
                        line: operand.line,
                        column: operand.column,
                        length: operand.length
                    });
                }
            }
        }
        else if (stmt.type === 'AsmStatement') {
        }
        else if (stmt.type === 'BreakStatement') {
            if (this.loopDepth === 0) {
                this.errors.push({
                    message: `Reference Error: 'break' outside of loop.`,
                    line: stmt.line,
                    column: stmt.column,
                    length: stmt.length
                });
            }
        }
        else if (stmt.type === 'ContinueStatement') {
            if (this.loopDepth === 0) {
                this.errors.push({
                    message: `Reference Error: 'continue' outside of loop.`,
                    line: stmt.line,
                    column: stmt.column,
                    length: stmt.length
                });
            }
        }
        else if (stmt.type === 'FunctionDeclaration') {
            this.symbolMap.clear();
            for (const p of stmt.params) {
                this.symbolMap.set(p.name, parseType(p.type));
            }
            this.currentReturnType = parseType(stmt.returnType);
            if (!stmt.returnType || ['auto', 'let', 'unknown'].includes(stmt.returnType)) {
                this.errors.push({
                    message: `Type Error: function '${stmt.name}' return type must be a definite type, got '${stmt.returnType ?? 'void'}'.`,
                    line: stmt.line,
                    column: stmt.column,
                    length: stmt.name.length
                });
            }
            for (const req of stmt.requires) {
                const t = this.visitExpression(req);
                if (!isConditionType(t)) {
                    this.errors.push({
                        message: `Contract Error: 'requires' condition must be boolean, got '${typeToString(t)}'.`,
                        line: req.line,
                        column: req.column,
                        length: req.length
                    });
                }
            }
            for (const ens of stmt.ensures) {
                const t = this.visitExpression(ens);
                if (!isConditionType(t)) {
                    this.errors.push({
                        message: `Contract Error: 'ensures' condition must be boolean, got '${typeToString(t)}'.`,
                        line: ens.line,
                        column: ens.column,
                        length: ens.length
                    });
                }
            }
            this.pushQubitScope();
            stmt.body.forEach(s => this.visitStatement(s));
            this.popQubitScope();
        }
        else if (stmt.type === 'ReturnStatement') {
            this.visitExpression(stmt.argument);
        }
    }

    private pushQubitScope(): void {
        this.measuredQubitScopes.push(this.measuredQubits);
        this.measuredQubits = new Set();
    }

    private popQubitScope(): void {
        const prev = this.measuredQubitScopes.pop();
        if (prev) { this.measuredQubits = prev; }
    }

    private checkQubitArgs(expr: any, expectedCount: number): void {
        if (expr.arguments.length !== expectedCount) {
            this.errors.push({
                message: `Signature Error: '${expr.name}' expects ${expectedCount} Qubit argument(s).`,
                line: expr.line, column: expr.column, length: expr.length
            });
            return;
        }
        for (const arg of expr.arguments) {
            const t = this.visitExpression(arg);
            if (!(t.kind === 'quantum' && t.cls === 'Qubit') && t.kind !== 'unknown') {
                this.errors.push({
                    message: `Type Error: '${expr.name}' expects Qubit arguments, got '${typeToString(t)}'.`,
                    line: arg.line, column: arg.column, length: arg.length
                });
            }
        }
    }

    private visitExpression(expr: Expression): Type {
        if (expr.type === 'ResultExpr') {
            return this.currentReturnType;
        }

        if (expr.type === 'FunctionExpression') {
            const savedSymbols = this.symbolMap;
            const savedReturnType = this.currentReturnType;
            this.symbolMap = new Map(savedSymbols);
            for (const p of expr.params) {
                this.symbolMap.set(p.name, parseType(p.type));
            }
            let retType: Type = expr.returnType ? parseType(expr.returnType) : T.void;
            if (!expr.returnType) {
                for (const s of expr.body) {
                    if (s.type === 'ReturnStatement') {
                        retType = this.visitExpression(s.argument);
                        break;
                    }
                }
            }
            this.currentReturnType = retType;
            this.pushQubitScope();
            expr.body.forEach(s => this.visitStatement(s));
            this.popQubitScope();
            this.symbolMap = savedSymbols;
            this.currentReturnType = savedReturnType;
            return func(expr.params.map(p => parseType(p.type)), retType);
        }

        if (expr.type === 'NumberLiteral') {
            return expr.isFloat ? T.double : T.int32;
        }

        if (expr.type === 'StringLiteral') return T.string;
        if (expr.type === 'CharLiteral') return T.char;
        if (expr.type === 'NullLiteral') return T.null;

        if (expr.type === 'Dereference') {
            const t = this.visitExpression(expr.target);
            if (t.kind === 'cap') return t.inner;
            return T.unknown;
        }

        if (expr.type === 'AddressOf') {
            if (expr.target.type === 'Identifier') {
                const t = this.symbolMap.get(expr.target.name);
                if (t) {
                    return cap(t);
                }
                const g = this.globalSymbols.get(expr.target.name);
                if (g) {
                    return cap(g);
                }
                const uf = this.userFuncSigs.get(expr.target.name);
                if (uf) {
                    return func(uf.params, uf.ret);
                }
                const ef = this.externSigs.get(expr.target.name);
                if (ef) {
                    return func(ef.params, ef.ret);
                }
            }
            return cap(T.uint8);
        }

        if (expr.type === 'FuseExpression') {
            this.visitExpression(expr.discriminant);
            let resultType: Type = T.unknown;
            for (const arm of expr.arms) {
                if (arm.pattern) this.visitExpression(arm.pattern);
                const t = this.visitExpression(arm.value);
                if (resultType.kind === 'unknown') resultType = t;
                else if (!typeEquals(resultType, t) && t.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: fuse arms must have the same type, got '${typeToString(resultType)}' and '${typeToString(t)}'.`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                }
            }
            return resultType;
        }

        if (expr.type === 'NativeExpression') {
            for (const op of expr.operands) this.visitExpression(op);
            return T.void;
        }

        if (expr.type === 'ArrayLiteral') {
            let elemType: Type = T.unknown;
            expr.elements.forEach((e, i) => {
                const t = this.visitExpression(e);
                if (i === 0) elemType = t;
                else if (elemType.kind !== 'unknown' && t.kind !== 'unknown' && !typeEquals(elemType, t)) {
                    this.errors.push({
                        message: `Type Error: array literal elements must share one type, got '${typeToString(elemType)}' and '${typeToString(t)}'.`,
                        line: e.line, column: e.column, length: e.length
                    });
                }
            });
            return array(elemType, expr.elements.length);
        }

        if (expr.type === 'Identifier') {
            if (this.measuredQubits.has(expr.name)) {
                this.errors.push({
                    message: `Quantum Violation: Qubit '${expr.name}' used after measurement. Measurement collapses and consumes the qubit.`,
                    line: expr.line,
                    column: expr.column,
                    length: expr.length
                });
                return T.unknown;
            }
            if (this.flavorMembers.has(expr.name)) {
                return T.int32;
            }
            if (this.globalSymbols.has(expr.name)) {
                return this.globalSymbols.get(expr.name)!;
            }
            if (!this.symbolMap.has(expr.name)) {
                this.errors.push({
                    message: `Reference Error: Undefined variable '${expr.name}'.`,
                    line: expr.line,
                    column: expr.column,
                    length: expr.length
                });
                return T.unknown;
            }
            return this.symbolMap.get(expr.name)!;
        }

        if (expr.type === 'FunctionCall') {
            if (GATE_NAMES.has(expr.name)) {
                this.usedGates.add(expr.name);
                if (this.declaredGateVars.has(expr.name)) {
                    this.errors.push({
                        message: `Ambiguity Warning: '${expr.name}' is used both as a quantum gate and as a variable.`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                }
            }
            if (expr.name === 'alloc') return T.qubit;

            if (expr.name === 'basis_state') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'basis_state' expects 3 arguments (theta: double, phi: double, value: int32).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.qobject;
            }

            if (expr.name === 'lattice_rank' || expr.name === 'lattice_size' || expr.name === 'lattice_boundary') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }

            if (expr.name === 'strlen') {
                if (expr.arguments.length >= 1) this.visitExpression(expr.arguments[0]);
                return T.int32;
            }
            if (expr.name === 'kglobals_addr') {
                return T.int32;
            }

            if (expr.name === 'cgui_init' || expr.name === 'cgui_should_close' ||
                expr.name === 'cgui_button' || expr.name === 'cgui_mouse_x' ||
                expr.name === 'cgui_mouse_y' || expr.name === 'cgui_mouse_left_clicked' ||
                expr.name === 'cgui_width' || expr.name === 'cgui_height') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }

            if (expr.name === 'cgui_begin_frame' || expr.name === 'cgui_end_frame' ||
                expr.name === 'cgui_text' || expr.name === 'cgui_text_int' ||
                expr.name === 'cgui_beep' || expr.name === 'cgui_panel' ||
                expr.name === 'cgui_panel_end' || expr.name === 'cgui_row' ||
                expr.name === 'cgfx_rect' ||
                expr.name === 'cgfx_line' || expr.name === 'cgfx_polyline' ||
                expr.name === 'cgfx_ellipse' ||
                expr.name === 'cgfx_triangle' || expr.name === 'cgfx_rect_a' ||
                expr.name === 'cgfx_line_a' || expr.name === 'cgfx_polyline_a' ||
                expr.name === 'cgfx_ellipse_a' ||
                expr.name === 'cgfx_triangle_a') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }





            if (expr.name === 'h' || expr.name === 'x' || expr.name === 'y' ||
                expr.name === 'z' || expr.name === 's' || expr.name === 't') {
                this.checkQubitArgs(expr, 1);
                return T.void;
            }
            if (expr.name === 'rz' || expr.name === 'rx' || expr.name === 'ry') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects 2 arguments (qubit, angle).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.void;
            }
            if (expr.name === 'cnot' || expr.name === 'swap' || expr.name === 'braid') {
                this.checkQubitArgs(expr, 2);
                return T.void;
            }
            if (expr.name === 'toffoli') {
                this.checkQubitArgs(expr, 3);
                return T.void;
            }
            if (expr.name === 'cx' || expr.name === 'ch') {
                this.checkQubitArgs(expr, 2);
                return T.void;
            }
            if (expr.name === 'crz') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'crz' expects 3 arguments (control, target, angle).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.void;
            }
            if (expr.name === 'cswap') {
                this.checkQubitArgs(expr, 3);
                return T.void;
            }
            if (expr.name === 'c_toffoli') {
                this.checkQubitArgs(expr, 4);
                return T.void;
            }
            if (expr.name === 'qft' || expr.name === 'iqft') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects 1 argument (num_qubits: int).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.void;
            }
            if (expr.name === 'cqft') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: 'cqft' expects 2 arguments (control: Qubit, num_qubits: int).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.void;
            }
            if (expr.name === 'cbraid') {
                this.checkQubitArgs(expr, 3);
                return T.void;
            }
            if (expr.name === 'measure_x' || expr.name === 'measure_y') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects exactly 1 Qubit argument.`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                    return T.int32;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (!(argType.kind === 'quantum' && argType.cls === 'Qubit') && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: '${expr.name}' expects a Qubit, but received '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                if (expr.arguments[0].type === 'Identifier') {
                    const qname = (expr.arguments[0] as any).name;
                    this.symbolMap.delete(qname);
                    this.measuredQubits.add(qname);
                }
                return T.int32;
            }

            if (expr.name === 'measure') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'measure' expects exactly 1 argument.`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.int32;
                }

                const argType = this.visitExpression(expr.arguments[0]);
                if (!(argType.kind === 'quantum' && argType.cls === 'Qubit') && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'measure' expects a Qubit, but received '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }

                if (expr.arguments[0].type === 'Identifier') {
                    const qname = (expr.arguments[0] as any).name;
                    this.symbolMap.delete(qname);
                    this.measuredQubits.add(qname);
                }

                return T.int32;
            }

            if (expr.name === 'encode_text') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'encode_text' expects 1 string argument.`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qobject;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'encode_text' expects string, got '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                return T.qobject;
            }

            if (expr.name === 'encode_amplitudes') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: 'encode_amplitudes' expects 2 arguments (text: string, num_qubits: int32).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qobject;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'encode_amplitudes' expects string, got '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                this.visitExpression(expr.arguments[1]);
                return T.qobject;
            }

            if (expr.name === 'encode_image') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: 'encode_image' expects 2 arguments (path: string, num_qubits: int32).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qobject;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'encode_image' expects string path, got '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                this.visitExpression(expr.arguments[1]);
                return T.qobject;
            }

            if (expr.name === 'encode_pixel') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: 'encode_pixel' expects 2 arguments (ref: string, num_qubits: int32).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qobject;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'encode_pixel' expects string image reference (path or data URL), got '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                this.visitExpression(expr.arguments[1]);
                return T.qobject;
            }

            if (expr.name === 'qobj_num_qubits') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'qobj_num_qubits' expects 1 argument (obj: QObject).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.int32;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (!(argType.kind === 'quantum' && argType.cls === 'QObject') && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'qobj_num_qubits' expects QObject, got '${typeToString(argType)}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                return T.int32;
            }

            if (expr.name === 'encode_adaptive') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'encode_adaptive' expects 1 string argument.`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qobject;
                }
                const argType = this.visitExpression(expr.arguments[0]);
                if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: 'encode_adaptive' expects string, got '${argType}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                return T.qobject;
            }

            if (expr.name === 'qlm_invoke') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'qlm_invoke' expects 3 arguments (data: QObject, epochs: int, lr: double).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                    return T.qmodel;
                }
                const arg0Type = this.visitExpression(expr.arguments[0]);
                if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QObject') && arg0Type.kind !== 'unknown') {
                    this.errors.push({
                        message: `Type Error: First argument of 'qlm_invoke' must be QObject, got '${arg0Type}'.`,
                        line: expr.arguments[0].line,
                        column: expr.arguments[0].column,
                        length: expr.arguments[0].length
                    });
                }
                return T.qmodel;
            }

            if (expr.name === 'qlm_load') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'qlm_load' expects 1 argument (path: string).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.qmodel;
            }

            if (expr.name === 'rvdl_encode_string') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'rvdl_encode_string' expects 1 argument (prompt: string).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.qobject;
            }

            if (expr.name === 'qlm_forward') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: 'qlm_forward' expects 2 arguments (model: QModel, data: QObject).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.void;
            }

            if (expr.name === 'rvdl_decode_string') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'rvdl_decode_string' expects 1 argument (data: QObject).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                return T.string;
            }

            if (expr.name === 'qrc_new') {
                if (expr.arguments.length !== 2 && expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'qrc_new' expects 2 arguments (qubits: int32, layers: int32) or 3 (qubits, layers, out_dim: int32).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.qreservoir;
            }

            if (expr.name === 'qrc_train') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'qrc_train' expects 3 arguments (res: QReservoir, epochs: int32, lr: double).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QReservoir') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of 'qrc_train' must be QReservoir, got '${arg0Type}'.`,
                            line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length
                        });
                    }
                    this.visitExpression(expr.arguments[1]);
                    this.visitExpression(expr.arguments[2]);
                }
                return T.void;
            }

            if (expr.name === 'qrc_train_ex') {
                if (expr.arguments.length !== 8) {
                    this.errors.push({
                        message: `Signature Error: 'qrc_train_ex' expects 8 arguments (res: QReservoir, epochs: int32, lr: double, inputs: cap<double>, targets: cap<double>, n_samples: int32, n_features: int32, n_outputs: int32).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QReservoir') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of 'qrc_train_ex' must be QReservoir, got '${typeToString(arg0Type)}'.`,
                            line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length
                        });
                    }
                    for (const idx of [3, 4]) {
                        const bt = this.visitExpression(expr.arguments[idx]);
                        if (bt.kind !== 'unknown' && !isDoubleBuffer(bt)) {
                            this.errors.push({
                                message: `Type Error: Argument ${idx + 1} of 'qrc_train_ex' (${idx === 3 ? 'inputs' : 'targets'}) must be cap<double> or arr<double, N>, got '${typeToString(bt)}'.`,
                                line: expr.arguments[idx].line, column: expr.arguments[idx].column, length: expr.arguments[idx].length
                            });
                        }
                    }
                    for (const idx of [1, 2, 5, 6, 7]) this.visitExpression(expr.arguments[idx]);
                }
                return T.void;
            }

            if (expr.name === 'qrc_probe' || expr.name === 'qrc_predict') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects 2 arguments (res: QReservoir, data: QObject).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QReservoir') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of '${expr.name}' must be QReservoir, got '${arg0Type}'.`,
                            line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length
                        });
                    }
                    this.visitExpression(expr.arguments[1]);
                }
                return T.qobject;
            }

            if (expr.name === 'qrc_release') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'qrc_release' expects 1 argument (res: QReservoir).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QReservoir') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of 'qrc_release' must be QReservoir, got '${arg0Type}'.`,
                            line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length
                        });
                    }
                }
                return T.void;
            }

            if (expr.name === 'mind_read') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'mind_read' expects 1 argument (modality: string).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const argType = this.visitExpression(expr.arguments[0]);
                    if (argType.kind !== 'string' && argType.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: 'mind_read' expects a string modality, got '${argType}'.`,
                            line: expr.arguments[0].line,
                            column: expr.arguments[0].column,
                            length: expr.arguments[0].length
                        });
                    }
                }
                return T.qobject;
            }

            if (expr.name === 'mind_train') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'mind_train' expects 3 arguments (state: QObject, epochs: int, lr: double).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QObject') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of 'mind_train' must be QObject, got '${arg0Type}'.`,
                            line: expr.arguments[0].line,
                            column: expr.arguments[0].column,
                            length: expr.arguments[0].length
                        });
                    }
                }
                return T.void;
            }

            if (expr.name === 'mind_feedback') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'mind_feedback' expects 1 argument (state: QObject).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const argType = this.visitExpression(expr.arguments[0]);
                    if (!(argType.kind === 'quantum' && argType.cls === 'QObject') && argType.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: 'mind_feedback' expects a QObject, got '${argType}'.`,
                            line: expr.arguments[0].line,
                            column: expr.arguments[0].column,
                            length: expr.arguments[0].length
                        });
                    }
                }
                return T.void;
            }

            if (expr.name === 'veda_qlm_train') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({
                        message: `Signature Error: 'veda_qlm_train' expects 3 arguments (state: QObject, epochs: int, lr: double).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const arg0Type = this.visitExpression(expr.arguments[0]);
                    if (!(arg0Type.kind === 'quantum' && arg0Type.cls === 'QObject') && arg0Type.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: First argument of 'veda_qlm_train' must be QObject, got '${arg0Type}'.`,
                            line: expr.arguments[0].line,
                            column: expr.arguments[0].column,
                            length: expr.arguments[0].length
                        });
                    }
                }
                return T.void;
            }

            if (expr.name === 'qchain_wallet') {
                return T.string;
            }
            if (expr.name === 'qchain_balance') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: 'qchain_balance' expects 1 argument (address: string).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                } else {
                    const t = this.visitExpression(expr.arguments[0]);
                    if (t.kind !== 'string' && t.kind !== 'unknown') {
                        this.errors.push({ message: `Type Error: 'qchain_balance' expects a string address, got '${typeToString(t)}'.`, line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length });
                    }
                }
                return T.uint64;
            }
            if (expr.name === 'qchain_mint') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qchain_mint' expects 2 arguments (address: string, amount: uint64).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.void;
            }
            if (expr.name === 'qchain_transfer') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({ message: `Signature Error: 'qchain_transfer' expects 3 arguments (from: string, to: string, amount: uint64).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                    this.visitExpression(expr.arguments[2]);
                }
                return T.int32;
            }
            if (expr.name === 'qchain_mine' || expr.name === 'qchain_height' || expr.name === 'qchain_verify') {
                return T.int32;
            }
            if (expr.name === 'qchain_qkd') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qchain_qkd' expects 1 argument (rounds: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.string;
            }
            if (expr.name === 'qchain_qdba') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qchain_qdba' expects 1 argument (parties: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.int32;
            }
            if (expr.name === 'dla_dim') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'dla_dim' expects 2 arguments (spec: string, n_qubits: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.int32;
            }
            if (expr.name === 'qstate_entropy') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qstate_entropy' expects 1 argument (q: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.double;
            }
            if (expr.name === 'qstate_fidelity') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qstate_fidelity' expects 2 arguments (a: QObject, b: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'qstate_purity' || expr.name === 'qstate_linear_entropy') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 1 argument (q: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.double;
            }
            if (expr.name === 'qstate_renyi_entropy') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qstate_renyi_entropy' expects 2 arguments (q: QObject, alpha: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'math_dot') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({ message: `Signature Error: 'math_dot' expects 3 arguments (a: arr<double,N>, b: arr<double,N>, n: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'math_norm') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'math_norm' expects 2 arguments (a: arr<double,N>, n: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'qattention') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qattention' expects 2 arguments (q: QObject, k: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'shannon4') {
                if (expr.arguments.length !== 4) {
                    this.errors.push({ message: `Signature Error: 'shannon4' expects 4 arguments (n0..n3: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'shannon8') {
                if (expr.arguments.length !== 8) {
                    this.errors.push({ message: `Signature Error: 'shannon8' expects 8 arguments (n0..n7: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'retrocausal_imax' || expr.name === 'retrocausal_idoe' ||
                expr.name === 'retrocausal_q_capacity' || expr.name === 'retrocausal_c_capacity' ||
                expr.name === 'retrocausal_gain') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 2 arguments (kind: int32, p: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'retrocausal_q_one_shot' || expr.name === 'retrocausal_deformed') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 3 arguments (kind: int32, p: double, x: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'retrocausal_ctc_q_capacity' || expr.name === 'retrocausal_ctc_c_capacity' ||
                expr.name === 'retrocausal_ctc_gain') {
                if (expr.arguments.length !== 4) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 4 arguments (n: int32, theta: double, mu2: double, lambda: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    expr.arguments.forEach(a => this.visitExpression(a));
                }
                return T.double;
            }
            if (expr.name === 'retrocausal_ctc_dephasing') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'retrocausal_ctc_dephasing' expects 1 argument (theta: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.double;
            }
            if (expr.name === 'geodesic_distance') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'geodesic_distance' expects 2 arguments (a: QObject, b: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'inversion' || expr.name === 'hyperbolic_metric') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 1 argument (x: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.double;
            }
            if (expr.name === 'hyperbolic_distance') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'hyperbolic_distance' expects 2 arguments (x: double, y: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'qgate_h' || expr.name === 'qgate_x') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 2 arguments (qobj: QObject, index: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.void;
            }
            if (expr.name === 'qgate_rz') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({ message: `Signature Error: 'qgate_rz' expects 3 arguments (qobj: QObject, index: int32, angle: double).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                    this.visitExpression(expr.arguments[2]);
                }
                return T.void;
            }
            if (expr.name === 'qgate_cnot') {
                if (expr.arguments.length !== 3) {
                    this.errors.push({ message: `Signature Error: 'qgate_cnot' expects 3 arguments (qobj: QObject, control: int32, target: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                    this.visitExpression(expr.arguments[2]);
                }
                return T.void;
            }
            if (expr.name === 'qgate_cnot_pair') {
                if (expr.arguments.length !== 4) {
                    this.errors.push({ message: `Signature Error: 'qgate_cnot_pair' expects 4 arguments (a: QObject, ia: int32, b: QObject, ib: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                    this.visitExpression(expr.arguments[2]);
                    this.visitExpression(expr.arguments[3]);
                }
                return T.void;
            }
            if (expr.name === 'qexpect_z') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qexpect_z' expects 2 arguments (qobj: QObject, index: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.double;
            }
            if (expr.name === 'qmeasure') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qmeasure' expects 2 arguments (qobj: QObject, index: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.int32;
            }
            if (expr.name === 'qchain_coin_mint') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qchain_coin_mint' expects 1 argument (qubits: int32).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.qobject;
            }
            if (expr.name === 'qchain_coin_verify') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qchain_coin_verify' expects 1 argument (coin: QObject).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    const t = this.visitExpression(expr.arguments[0]);
                    if (!(t.kind === 'quantum' && t.cls === 'QObject') && t.kind !== 'unknown') {
                        this.errors.push({ message: `Type Error: 'qchain_coin_verify' expects a QObject coin, got '${typeToString(t)}'.`, line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length });
                    }
                }
                return T.int32;
            }

            if (expr.name === 'qchain_sha3' || expr.name === 'qchain_hash_unicode' ||
                expr.name === 'qchain_sign' || expr.name === 'qchain_sign_pubkey') {
                if (expr.name === 'qchain_sign_pubkey') {
                    return T.string;
                }
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 1 argument (msg: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    const t = this.visitExpression(expr.arguments[0]);
                    if (t.kind !== 'string' && t.kind !== 'unknown') {
                        this.errors.push({ message: `Type Error: '${expr.name}' expects a string message, got '${typeToString(t)}'.`, line: expr.arguments[0].line, column: expr.arguments[0].column, length: expr.arguments[0].length });
                    }
                }
                return T.string;
            }
            if (expr.name === 'qchain_hmac') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qchain_hmac' expects 2 arguments (key: string, msg: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.string;
            }
            if (expr.name === 'qchain_sign_verify') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qchain_sign_verify' expects 2 arguments (msg: string, sig: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.int32;
            }
            if (expr.name === 'qchain_mlkem_encaps') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({ message: `Signature Error: 'qchain_mlkem_encaps' expects 1 argument (pk: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                }
                return T.string;
            }
            if (expr.name === 'qchain_mlkem_decaps') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: 'qchain_mlkem_decaps' expects 2 arguments (sk: string, ct: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.string;
            }
            if (expr.name === 'qchain_causal_verify') {
                return T.int32;
            }
            if (expr.name === 'qchain_cipher_encrypt' || expr.name === 'qchain_cipher_decrypt') {
                if (expr.arguments.length !== 2) {
                    this.errors.push({ message: `Signature Error: '${expr.name}' expects 2 arguments (seed: uint64, data: string).`, line: expr.line, column: expr.column, length: expr.length });
                } else {
                    this.visitExpression(expr.arguments[0]);
                    this.visitExpression(expr.arguments[1]);
                }
                return T.string;
            }

            if (expr.name === 'rvdl_sys_call') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }
            if (expr.name === 'rvdl_sys_calld') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.double;
            }
            if (expr.name === 'rvdl_sys_log') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'rvdl_sys_logi') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }
            if (expr.name === 'rvdl_sys_callp') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return cap(T.uint8);
            }
            if (expr.name === 'rvdl_gc_free') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }

            if (expr.name === 'sync_load' || expr.name === 'sync_add' || expr.name === 'sync_cas') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }
            if (expr.name === 'sync_store') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'sync_lock') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }
            if (expr.name === 'sync_unlock') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'outb' || expr.name === 'outw' || expr.name === 'outl') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'inb' || expr.name === 'inw' || expr.name === 'inl') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }
            if (expr.name === 'read_cr0' || expr.name === 'read_cr2' ||
                expr.name === 'read_cr3' || expr.name === 'read_cr4' ||
                expr.name === 'rdmsr' || expr.name === 'rdtsc' ||
                expr.name === 'read_rflags' || expr.name === 'xgetbv') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.uint64;
            }
            if (expr.name === 'write_cr0' || expr.name === 'write_cr3' ||
                expr.name === 'wrmsr' || expr.name === 'invlpg' || expr.name === 'cpuid') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'volatile_load') {
                const t = this.visitExpression(expr.arguments[0]);
                expr.arguments.forEach(a => this.visitExpression(a));
                return t.kind === 'cap' ? t.inner : T.int32;
            }
            if (expr.name === 'volatile_store') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.void;
            }
            if (expr.name === 'complex') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.complex128;
            }
            if (expr.name === 'real' || expr.name === 'imag' || expr.name === 'cabs') {
                const z = this.visitExpression(expr.arguments[0]);
                expr.arguments.forEach(a => this.visitExpression(a));
                return z.kind === 'complex' && z.width === 32 ? T.float : T.double;
            }
            if (expr.name === 'conj') {
                const z = this.visitExpression(expr.arguments[0]);
                expr.arguments.forEach(a => this.visitExpression(a));
                return z.kind === 'complex' ? z : T.complex128;
            }
            if (expr.name === 'rvdl_gc_alloc') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return cap(T.int32);
            }
            if (expr.name === 'addr') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.int32;
            }

            const qmsFns: Record<string, { argc: number; args: string[] }> = {
                rvdl_qms_gap:   { argc: 3, args: ['int32', 'double', 'double'] },
                rvdl_mix_bound: { argc: 3, args: ['double', 'double', 'double'] },
                rvdl_qms_conc:  { argc: 2, args: ['double', 'double'] },
            };
            const qf = qmsFns[expr.name];
            if (qf) {
                if (expr.arguments.length !== qf.argc) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects ${qf.argc} argument(s).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.double;
            }

            const scalarMathFns: Record<string, { argc: number; args: string[] }> = {
                surrogate:    { argc: 3, args: ['double', 'double', 'double'] },
                tanh_quantize:{ argc: 3, args: ['double', 'double', 'int32'] },
                lif_step:     { argc: 4, args: ['double', 'double', 'double', 'double'] },
                mellowmax2:   { argc: 3, args: ['double', 'double', 'double'] },
                logsumexp2:   { argc: 3, args: ['double', 'double', 'double'] },
                boltzmann2:   { argc: 3, args: ['double', 'double', 'double'] },
                tnorm_luk:    { argc: 2, args: ['double', 'double'] },
                tnorm_prod:   { argc: 2, args: ['double', 'double'] },
                tnorm_godel:  { argc: 2, args: ['double', 'double'] },
                polymer_weight: { argc: 3, args: ['double', 'double', 'double'] },
                polymer_mix_bound: { argc: 2, args: ['double', 'double'] },
                moe_groups:          { argc: 1, args: ['double'] },
                moe_experts_per:     { argc: 1, args: ['double'] },
                moe_virtual_experts: { argc: 1, args: ['double'] },
                moe_savings:         { argc: 1, args: ['double'] },
                curvature_gate:      { argc: 1, args: ['double'] },
                dual_fuse:           { argc: 4, args: ['double', 'double', 'double', 'double'] },
                tau_alpha:           { argc: 1, args: ['double'] },
                tau_generational:    { argc: 2, args: ['double', 'double'] },
                tau_fold_depth:      { argc: 2, args: ['double', 'double'] },
                tau_gear_ratio:      { argc: 2, args: ['double', 'double'] },
                tau_route_cost:      { argc: 4, args: ['double', 'double', 'double', 'double'] },
                tau_route_price:     { argc: 3, args: ['double', 'double', 'double'] },
            };
            const sf = scalarMathFns[expr.name];
            if (sf) {
                if (expr.arguments.length !== sf.argc) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects ${sf.argc} argument(s).`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.double;
            }

            const externSig = this.externSigs.get(expr.name);
            if (externSig) {
                expr.arguments.forEach(a => this.visitExpression(a));
                return externSig.ret;
            }

            const fnType = this.symbolMap.get(expr.name);
            if (fnType && fnType.kind === 'func') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return fnType.ret;
            }

            const userSig = this.userFuncSigs.get(expr.name);
            if (userSig) {
                if (expr.arguments.length !== userSig.params.length) {
                    this.errors.push({
                        message: `Signature Error: '${expr.name}' expects ${userSig.params.length} argument(s), got ${expr.arguments.length}.`,
                        line: expr.line, column: expr.column, length: expr.length
                    });
                }
                expr.arguments.forEach((a, i) => {
                    const at = this.visitExpression(a);
                    if (i < userSig.params.length && !isAssignable(userSig.params[i], at) && at.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: argument ${i + 1} of '${expr.name}' expects '${typeToString(userSig.params[i])}', got '${typeToString(at)}'.`,
                            line: a.line, column: a.column, length: a.length
                        });
                    }
                });
                return userSig.ret;
            }
        }

        if (expr.type === 'MemberExpression') {
            const objType = this.visitExpression(expr.object);

            if (objType.kind === 'quantum' && objType.cls === 'QModel' && expr.property === 'export') {
                if (expr.arguments.length !== 1) {
                    this.errors.push({
                        message: `Signature Error: QModel.export expects 1 string argument (export path).`,
                        line: expr.line,
                        column: expr.column,
                        length: expr.length
                    });
                } else {
                    const pathType = this.visitExpression(expr.arguments[0]);
                    if (pathType.kind !== 'string' && pathType.kind !== 'unknown') {
                        this.errors.push({
                            message: `Type Error: Export path must be string, got '${typeToString(pathType)}'.`,
                            line: expr.arguments[0].line,
                            column: expr.arguments[0].column,
                            length: expr.arguments[0].length
                        });
                    }
                }
                return T.void;
            }

            if (objType.kind === 'quantum' && objType.cls === 'QObject' && expr.property === 'measure') return T.int32;

            if (!expr.isMethodCall && objType.kind === 'form') {
                const ft = this.findFieldType(objType.name, expr.property);
                if (ft) return ft;
            }
        }

        if (expr.type === 'BinaryExpression') {
            const l = this.visitExpression(expr.left);
            const r = this.visitExpression(expr.right);
            if (expr.operator === '<' || expr.operator === '==' ||
                expr.operator === '>' || expr.operator === '<=' ||
                expr.operator === '>=' || expr.operator === '!=') return T.bool;
            const ptrArith = expr.operator === '+' && l.kind === 'cap';
            const numericMix = isNumericCoercible(l, r);
            if (!typeEquals(l, r) && !numericMix && l.kind !== 'unknown' && r.kind !== 'unknown' && !ptrArith) {
                this.errors.push({
                    message: `Type Error: binary operator '${expr.operator}' on mismatched types '${typeToString(l)}' and '${typeToString(r)}'.`,
                    line: expr.line,
                    column: expr.column,
                    length: expr.length
                });
            }
            return l.kind === 'unknown' ? r : l;
        }

        if (expr.type === 'LogicalExpression') {
            const l = this.visitExpression(expr.left);
            const r = this.visitExpression(expr.right);
            if (!isConditionType(l)) {
                this.errors.push({
                    message: `Type Error: logical operator '${expr.operator}' requires boolean operands, got '${typeToString(l)}'.`,
                    line: expr.left.line,
                    column: expr.left.column,
                    length: expr.left.length
                });
            }
            if (!isConditionType(r)) {
                this.errors.push({
                    message: `Type Error: logical operator '${expr.operator}' requires boolean operands, got '${typeToString(r)}'.`,
                    line: expr.right.line,
                    column: expr.right.column,
                    length: expr.right.length
                });
            }
            return T.bool;
        }

        if (expr.type === 'UnaryExpression') {
            const t = this.visitExpression(expr.argument);
            if (expr.operator === '!') {
                if (!isConditionType(t)) {
                    this.errors.push({
                        message: `Type Error: unary '!' requires a boolean operand, got '${typeToString(t)}'.`,
                        line: expr.argument.line,
                        column: expr.argument.column,
                        length: expr.argument.length
                    });
                }
                return T.bool;
            }
            return t;
        }

        if (expr.type === 'IndexExpression') {
            const objType = this.visitExpression(expr.object);
            expr.indices.forEach(i => this.visitExpression(i));
            if (objType.kind === 'lattice') {
                return objType.elem;
            }
            if (objType.kind === 'string') {
                return T.char;
            }
            if (objType.kind === 'array') {
                return objType.elem;
            }
            this.errors.push({
                message: `Type Error: Indexing requires a lattice, array or string, got '${typeToString(objType)}'.`,
                line: expr.line,
                column: expr.column,
                length: expr.length
            });
            return T.unknown;
        }

        if (expr.type === 'NewExpression') {
            if (expr.className === 'QReservoir') {
                expr.arguments.forEach(a => this.visitExpression(a));
                return T.qreservoir;
            }
            if (expr.className === 'BellState' || expr.className === 'DiracState' || expr.className === 'QuantumRegister') {
                return T.qobject;
            }
            if (expr.className.startsWith('lattice<')) {
                expr.arguments.forEach(a => this.visitExpression(a));
                return parseType(expr.className);
            }
            const base = expr.className.split('<')[0];
            if (this.forms.has(base)) {
                return parseType(base);
            }
            this.errors.push({
                message: `Type Error: Unknown form '${expr.className}'.`,
                line: expr.line,
                column: expr.column,
                length: expr.length
            });
            return T.unknown;
        }
        return T.unknown;
    }
}