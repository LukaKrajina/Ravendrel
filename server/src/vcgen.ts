import { Program, FunctionDeclaration, Statement, Expression } from './ast';
import { Type, parseType, typeToString } from './types';

export type SExpr =
    | { kind: 'var'; name: string; sort?: string }
    | { kind: 'const'; type: string; value: string }
    | { kind: 'app'; op: string; args: SExpr[] };

export interface Obligation {
    id: string;
    antecedent: SExpr;
    consequent: SExpr;
}

const TRUE: SExpr = { kind: 'const', type: 'Bool', value: 'true' };
const FALSE: SExpr = { kind: 'const', type: 'Bool', value: 'false' };

function typeToSort(t: Type | undefined): string {
    if (!t) return 'Int';
    if (t.kind === 'float') return 'Real';
    if (t.kind === 'bool') return 'Bool';
    if (t.kind === 'string' || t.kind === 'char') return 'String';
    return 'Int';
}

function not(s: SExpr): SExpr {
    return { kind: 'app', op: '!', args: [s] };
}

function and(a: SExpr, b: SExpr): SExpr {
    return { kind: 'app', op: '&&', args: [a, b] };
}

function or(a: SExpr, b: SExpr): SExpr {
    return { kind: 'app', op: '||', args: [a, b] };
}

function implies(a: SExpr, b: SExpr): SExpr {
    return or(not(a), b);
}

function eq(a: SExpr, b: SExpr): SExpr {
    return { kind: 'app', op: '==', args: [a, b] };
}

export class VCGenerator {
    private obligations: Obligation[] = [];
    private obligationCounter: number = 0;
    private typeEnv: Map<string, Type> = new Map();

    public generate(ast: Program): Obligation[] {
        this.obligations = [];
        this.obligationCounter = 0;
        this.typeEnv.clear();
        for (const node of ast.body) {
            if (node.type === 'FunctionDeclaration') {
                if (node.requires.length > 0 || node.ensures.length > 0) {
                    this.verifyFunction(node);
                }
            }
        }
        return this.obligations;
    }

    private verifyFunction(fn: FunctionDeclaration): void {
        this.typeEnv.clear();
        for (const p of fn.params) {
            this.typeEnv.set(p.name, parseType(p.type));
        }
        const P = this.conjoin(fn.requires);
        const Q = this.conjoin(fn.ensures);
        const result = this.wpBlock(fn.body, Q);

        this.obligations.push({
            id: fn.name + '::implies',
            antecedent: P,
            consequent: result.formula
        });

        for (const o of result.obligations) {
            this.obligations.push(o);
        }
    }

    private conjoin(exprs: Expression[]): SExpr {
        if (exprs.length === 0) return TRUE;
        let acc = this.exprToS(exprs[0]);
        for (let i = 1; i < exprs.length; i++) {
            acc = and(acc, this.exprToS(exprs[i]));
        }
        return acc;
    }

    private exprToS(expr: Expression): SExpr {
        switch (expr.type) {
            case 'NumberLiteral':
                return {
                    kind: 'const',
                    type: expr.isFloat ? 'Real' : 'Int',
                    value: String(expr.value)
                };
            case 'StringLiteral':
                return { kind: 'const', type: 'String', value: JSON.stringify(expr.value) };
            case 'CharLiteral':
                return { kind: 'const', type: 'String', value: JSON.stringify(expr.value) };
            case 'Identifier': {
                const ty = this.typeEnv.get(expr.name);
                return { kind: 'var', name: expr.name, sort: typeToSort(ty) };
            }
            case 'ResultExpr':
                return { kind: 'var', name: 'result', sort: 'Int' };
            case 'BinaryExpression':
                return { kind: 'app', op: expr.operator, args: [this.exprToS(expr.left), this.exprToS(expr.right)] };
            case 'LogicalExpression':
                return { kind: 'app', op: expr.operator, args: [this.exprToS(expr.left), this.exprToS(expr.right)] };
            case 'UnaryExpression':
                return { kind: 'app', op: expr.operator, args: [this.exprToS(expr.argument)] };
            default:
                return { kind: 'var', name: 'uninterpreted' };
        }
    }

    private substitute(s: SExpr, name: string, e: SExpr): SExpr {
        if (s.kind === 'var' && s.name === name) {
            return e;
        }
        if (s.kind === 'app') {
            return { kind: 'app', op: s.op, args: s.args.map(a => this.substitute(a, name, e)) };
        }
        return s;
    }

    private wpBlock(stmts: Statement[], q: SExpr): { formula: SExpr; obligations: Obligation[] } {
        return this.wpBlockWithExits(stmts, q, q, q);
    }

    private wpBlockWithExits(stmts: Statement[], qNormal: SExpr, qBreak: SExpr, qContinue: SExpr): { formula: SExpr; obligations: Obligation[] } {
        const obligations: Obligation[] = [];
        let cur = qNormal;
        for (let i = stmts.length - 1; i >= 0; i--) {
            const r = this.wpStmt(stmts[i], cur, qBreak, qContinue);
            cur = r.formula;
            obligations.push(...r.obligations);
        }
        return { formula: cur, obligations };
    }

    private wpStmt(stmt: Statement, q: SExpr, qBreak: SExpr, qContinue: SExpr): { formula: SExpr; obligations: Obligation[] } {
        switch (stmt.type) {
            case 'VariableDeclaration': {
                const e = this.exprToS(stmt.value);
                if (stmt.varType && stmt.varType !== 'auto' && stmt.varType !== 'let') {
                    this.typeEnv.set(stmt.identifier, parseType(stmt.varType));
                }
                return { formula: this.substitute(q, stmt.identifier, e), obligations: [] };
            }
            case 'VariableDeclarationList': {
                let cur = q;
                const obligations: Obligation[] = [];
                for (let i = stmt.declarations.length - 1; i >= 0; i--) {
                    const r = this.wpStmt(stmt.declarations[i], cur, qBreak, qContinue);
                    cur = r.formula;
                    obligations.push(...r.obligations);
                }
                return { formula: cur, obligations };
            }
            case 'AssignmentStatement': {
                const e = this.exprToS(stmt.value);
                return { formula: this.substitute(q, stmt.name, e), obligations: [] };
            }
            case 'ReturnStatement': {
                const e = this.exprToS(stmt.argument);
                return { formula: this.substitute(q, 'result', e), obligations: [] };
            }
            case 'ExpressionStatement':
                return { formula: q, obligations: [] };

            case 'IfStatement': {
                const b = this.exprToS(stmt.condition);
                const w1 = this.wpBlockWithExits(stmt.consequent, q, qBreak, qContinue);
                let w2: { formula: SExpr; obligations: Obligation[] };
                if (stmt.alternate && stmt.alternate.length > 0) {
                    w2 = this.wpBlockWithExits(stmt.alternate, q, qBreak, qContinue);
                } else {
                    w2 = { formula: q, obligations: [] };
                }
                return {
                    formula: and(implies(b, w1.formula), implies(not(b), w2.formula)),
                    obligations: [...w1.obligations, ...w2.obligations]
                };
            }

            case 'UnsafeBlock': {
                return this.wpBlockWithExits(stmt.body, q, qBreak, qContinue);
            }

            case 'RouteStatement': {
                const d = this.exprToS(stmt.discriminant);
                const obligations: Obligation[] = [];
                let acc: SExpr = TRUE;
                const neqAll: SExpr[] = [];
                for (const c of stmt.cases) {
                    const v = this.exprToS(c.value);
                    const w = this.wpBlockWithExits(c.body, q, qBreak, qContinue);
                    obligations.push(...w.obligations);
                    acc = and(acc, implies(eq(d, v), w.formula));
                    neqAll.push(not(eq(d, v)));
                }
                if (stmt.fallback) {
                    const wf = this.wpBlockWithExits(stmt.fallback, q, qBreak, qContinue);
                    obligations.push(...wf.obligations);
                    const fallbackCond = neqAll.reduce((a, b) => and(a, b), TRUE);
                    acc = and(acc, implies(fallbackCond, wf.formula));
                }
                return { formula: acc, obligations };
            }

            case 'SpinStatement': {
                const bodyWp = this.wpBlockWithExits(stmt.body, q, qBreak, qContinue);
                return { formula: q, obligations: bodyWp.obligations };
            }

            case 'WhileStatement': {
                if (!stmt.invariant || stmt.invariant.length === 0) {
                    return { formula: q, obligations: [] };
                }
                const I = this.conjoin(stmt.invariant);
                const b = this.exprToS(stmt.condition);
                const notB: SExpr = not(b);

                const body = this.wpBlockWithExits(stmt.body, I, q, I);
                const obligations: Obligation[] = [];
                const n = this.obligationCounter++;
                obligations.push({
                    id: `while_${n}::exit`,
                    antecedent: and(I, notB),
                    consequent: q
                });
                obligations.push({
                    id: `while_${n}::preserve`,
                    antecedent: and(I, b),
                    consequent: body.formula
                });
                obligations.push(...body.obligations);
                return { formula: I, obligations };
            }
            case 'ForStatement': {
                if (!stmt.invariant || stmt.invariant.length === 0) {
                    return { formula: q, obligations: [] };
                }
                const I = this.conjoin(stmt.invariant);
                const b = stmt.condition ? this.exprToS(stmt.condition) : TRUE;
                const notB: SExpr = not(b);

                const loopBody = [...stmt.body];
                if (stmt.update) loopBody.push(stmt.update);
                const bodyWp = this.wpBlockWithExits(loopBody, I, q, I);

                const obligations: Obligation[] = [];
                const n = this.obligationCounter++;
                obligations.push({
                    id: `for_${n}::exit`,
                    antecedent: and(I, notB),
                    consequent: q
                });
                obligations.push({
                    id: `for_${n}::preserve`,
                    antecedent: and(I, b),
                    consequent: bodyWp.formula
                });
                obligations.push(...bodyWp.obligations);

                let formula: SExpr = I;
                if (stmt.init) {
                    const initWp = this.wpStmt(stmt.init, I, q, q);
                    formula = initWp.formula;
                    obligations.push(...initWp.obligations);
                }
                return { formula, obligations };
            }
            case 'BreakStatement':
                return { formula: qBreak, obligations: [] };
            case 'ContinueStatement':
                return { formula: qContinue, obligations: [] };
            case 'FunctionDeclaration':
                return { formula: q, obligations: [] };
            case 'SpawnStatement':
            case 'EntangleStatement':
                return { formula: q, obligations: [] };
            default:
                return { formula: q, obligations: [] };
        }
    }

    public toProtocol(obligations: Obligation[]): string {
        const lines: string[] = [];
        for (const o of obligations) {
            lines.push(`OBLIGATION ${o.id}`);
            lines.push(`ANTE ${this.sexprToString(o.antecedent)}`);
            lines.push(`CONSE ${this.sexprToString(o.consequent)}`);
            lines.push(`END_OBLIGATION`);
        }
        return lines.join('\n');
    }

    private sexprToString(s: SExpr): string {
        if (s.kind === 'var') return `( var ${s.name} )`;
        if (s.kind === 'const') return `( const ${s.type} ${s.value} )`;
        return `( ${s.op} ${s.args.map(a => this.sexprToString(a)).join(' ')} )`;
    }

    public toSmtLib(obligations: Obligation[]): string {
        const lines: string[] = ['(set-logic ALL)', ''];
        for (const o of obligations) {
            const vars = new Map<string, string>();
            this.collectVars(o.antecedent, vars);
            this.collectVars(o.consequent, vars);

            lines.push(`; obligation ${o.id}`);
            for (const [v, sort] of vars) {
                lines.push(`(declare-const ${v} ${sort})`);
            }
            lines.push(`(assert (and ${this.sexprToSmtLib(o.antecedent)} (not ${this.sexprToSmtLib(o.consequent)})))`);
            lines.push('(check-sat)');
            lines.push('');
        }
        return lines.join('\n');
    }

    private collectVars(s: SExpr, vars: Map<string, string>): void {
        if (s.kind === 'var') {
            if (!vars.has(s.name)) {
                vars.set(s.name, s.sort ?? 'Int');
            }
            return;
        }
        if (s.kind === 'app') {
            s.args.forEach(a => this.collectVars(a, vars));
        }
    }

    private sexprToSmtLib(s: SExpr): string {
        if (s.kind === 'var') return s.name;
        if (s.kind === 'const') return s.value;
        const op = s.op;
        if (op === '!') return `(not ${this.sexprToSmtLib(s.args[0])})`;
        if (op === '-' && s.args.length === 1) return `(- ${this.sexprToSmtLib(s.args[0])})`;
        if (op === '!=') return `(not (= ${this.sexprToSmtLib(s.args[0])} ${this.sexprToSmtLib(s.args[1])}))`;
        if (op === '&&') return `(and ${s.args.map(a => this.sexprToSmtLib(a)).join(' ')})`;
        if (op === '||') return `(or ${s.args.map(a => this.sexprToSmtLib(a)).join(' ')})`;
        const smtOpMap: Record<string, string> = {
            '==': '=', '+': '+', '-': '-', '*': '*', '/': 'div',
            '<': '<', '<=': '<=', '>': '>', '>=': '>='
        };
        const smtOp = smtOpMap[op] ?? op;
        return `(${smtOp} ${s.args.map(a => this.sexprToSmtLib(a)).join(' ')})`;
    }
}