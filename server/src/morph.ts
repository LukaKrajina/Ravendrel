/**
 * morph —— rvdl 的「态射宏」系统（全局 · 卫生即规范不变性 · 宏即用户定义的内建）。
 *
 * 范式要点（详见 docs/rvdl-language-manual.md 的「morph 态射宏」章节）：
 *  - **全局**：morph 是顶层声明，与内建函数同命名空间；不允许局部 morph。
 *  - **卫生 = 规范不变性**：模板引入的绑定（let / fn / 参数 / 类型声明）在每次
 *    展开时取新原子（α-重命名），使展开结果在「重命名」下不可区分 —— 类比规范场
 *    （Gabbay–Pitts 名义集合：α-等价 = 群轨道，取新名 = 群作用）。模板里的自由名
 *    （引用全局）保持不变，绝不会被使用处的局部变量捕获。
 *  - **限深递归**：宏可展开出宏，但受 MAX_RECURSION 限制，防止无限展开。
 *  - **通用元变量**：expr / stmt / items / type / ident / literal / tt。
 *
 * 展开发生在**词法层**（parse 之前）：把整段 token 流构建为 token 树，先收集
 * 顶层 morph 声明，再对 `name!(...)` 调用做不动点展开，最后拍平回 token 交给
 * Lexer.fromTokens 重解析。因此对 Parser / AST 零侵入。
 */
import { Token, TokenType } from './lexer';

// ────────────────────────────────────────────────────────────────────────────
// Token 树
// ────────────────────────────────────────────────────────────────────────────

/** 括号分组节点（token 树的内层结构），保留开/闭括号的原始 token 以还原位置 */
export interface TokenTreeGroup {
    kind: 'group';
    openTok: Token;
    closeTok: Token;
    tokens: TokenTree[];
}

export type TokenTree = Token | TokenTreeGroup;

function isGroup(t: TokenTree | undefined): t is TokenTreeGroup {
    return t !== undefined && (t as TokenTreeGroup).kind === 'group';
}
function isLeaf(t: TokenTree | undefined): t is Token {
    return t !== undefined && !isGroup(t);
}

// ────────────────────────────────────────────────────────────────────────────
// 元变量种类
// ────────────────────────────────────────────────────────────────────────────

export type MorphKind = 'expr' | 'stmt' | 'items' | 'type' | 'ident' | 'literal' | 'tt';
const MORPH_KINDS = new Set(['expr', 'stmt', 'items', 'type', 'ident', 'literal', 'tt']);

// ────────────────────────────────────────────────────────────────────────────
// 模式 / 模板的结构化表示
// ────────────────────────────────────────────────────────────────────────────

type PatMeta = { kind: 'meta'; name: string; mkind: MorphKind };
type PatTok = { kind: 'tok'; token: Token };
type PatGroup = { kind: 'group'; openTok: Token; closeTok: Token; items: PatItem[] };
type PatRepeat = { kind: 'repeat'; inner: PatItem[]; sep: Token | null; quantifier: '*' | '+' };
type PatItem = PatMeta | PatTok | PatGroup | PatRepeat;

type TplMeta = { kind: 'meta'; name: string };
type TplStringify = { kind: 'stringify'; name: string };
type TplTok = { kind: 'tok'; token: Token };
type TplGroup = { kind: 'group'; openTok: Token; closeTok: Token; items: TplItem[] };
type TplRepeat = { kind: 'repeat'; inner: TplItem[]; sep: Token | null; quantifier: '*' | '+' };
type TplItem = TplMeta | TplStringify | TplTok | TplGroup | TplRepeat;

interface MorphMacro {
    name: string;
    params: PatItem[];
    template: TplItem[];
    /** 原始模板 token 树（卫生分析用） */
    templateRaw: TokenTree[];
}

interface MatchEnv {
    outer: Map<string, TokenTree[]>;
    /** 每个重复组（按出现顺序）对应的一组「一轮匹配作用域」 */
    repeatBinds: Map<string, TokenTree[]>[][];
}

const MAX_RECURSION = 64;

// 卫生重命名用到的全局计数器（每次展开取新「规范」）
let gaugeCounter = 0;
function freshGauge(): number {
    return ++gaugeCounter;
}

// ────────────────────────────────────────────────────────────────────────────
// 入口：展开整段 token 流
// ────────────────────────────────────────────────────────────────────────────

export function expandMorphTokens(tokens: Token[]): Token[] {
    // 快路径：既无 morph 声明、也无 `name!(...)` 调用 → 原样返回（不干扰既有程序）。
    if (!hasMorphSyntax(tokens)) {
        return tokens;
    }

    const tree = buildTree(tokens);
    const macros = new Map<string, MorphMacro>();
    const rest = collectMorphs(tree, macros);

    let cur = rest;
    for (let depth = 0; depth < MAX_RECURSION; depth++) {
        const r = expandTree(cur, macros);
        if (!r.changed) {
            return flattenTree(r.tokens);
        }
        cur = r.tokens;
    }
    throw new Error(`morph: recursion limit (${MAX_RECURSION}) exceeded`);
}

/** 是否存在 morph 相关语法：`morph` 声明，或 `name!(...)` 调用形态。 */
function hasMorphSyntax(tokens: Token[]): boolean {
    for (let i = 0; i < tokens.length; i++) {
        const t = tokens[i];
        if (t.type === TokenType.Keyword && t.value === 'morph') return true;
        if (t.type === TokenType.Identifier &&
            tokens[i + 1] && tokens[i + 1].type === TokenType.Bang &&
            tokens[i + 2] && tokens[i + 2].type === TokenType.OpenParen) {
            return true;
        }
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Token 树构建 / 拍平
// ────────────────────────────────────────────────────────────────────────────

function buildTree(tokens: Token[]): TokenTree[] {
    const root: TokenTree[] = [];
    const stack: TokenTreeGroup[] = [];
    const isOpen = (t: Token) =>
        t.type === TokenType.OpenParen || t.type === TokenType.OpenBracket || t.type === TokenType.OpenBrace;
    const isClose = (t: Token) =>
        t.type === TokenType.CloseParen || t.type === TokenType.CloseBracket || t.type === TokenType.CloseBrace;

    for (const t of tokens) {
        if (isOpen(t)) {
            const g: TokenTreeGroup = { kind: 'group', openTok: t, closeTok: t, tokens: [] };
            (stack.length ? stack[stack.length - 1].tokens : root).push(g);
            stack.push(g);
        } else if (isClose(t)) {
            const g = stack.pop();
            if (!g) {
                throw new Error(`morph: unmatched '${t.value}' at line ${t.line}, col ${t.column}`);
            }
            if (!matchingDelimiter(g.openTok.value, t.value)) {
                throw new Error(`morph: mismatched delimiter '${t.value}' at line ${t.line}, col ${t.column}`);
            }
            g.closeTok = t;
        } else {
            (stack.length ? stack[stack.length - 1].tokens : root).push(t);
        }
    }
    if (stack.length) {
        throw new Error('morph: unterminated delimiter in source');
    }
    return root;
}

function matchingDelimiter(open: string, close: string): boolean {
    return (open === '(' && close === ')') || (open === '[' && close === ']') || (open === '{' && close === '}');
}

function flattenTree(tree: TokenTree[]): Token[] {
    const out: Token[] = [];
    for (const t of tree) {
        if (isGroup(t)) {
            out.push(t.openTok);
            out.push(...flattenTree(t.tokens));
            out.push(t.closeTok);
        } else {
            out.push(t);
        }
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 收集顶层 morph 声明
// ────────────────────────────────────────────────────────────────────────────

function collectMorphs(tree: TokenTree[], macros: Map<string, MorphMacro>): TokenTree[] {
    const out: TokenTree[] = [];
    let i = 0;
    while (i < tree.length) {
        const t = tree[i];
        if (isLeaf(t) && t.type === TokenType.Keyword && t.value === 'morph') {
            const nameTok = tree[i + 1];
            const paramGroup = tree[i + 2];
            const bodyGroup = tree[i + 3];
            if (!nameTok || !isLeaf(nameTok) || nameTok.type !== TokenType.Identifier) {
                throw new Error(`morph: expected macro name after 'morph' at line ${t.line}, col ${t.column}`);
            }
            if (!isGroup(paramGroup) || paramGroup.openTok.value !== '(') {
                throw new Error(`morph '${nameTok.value}': expected '(' parameter list at line ${t.line}`);
            }
            if (!isGroup(bodyGroup) || bodyGroup.openTok.value !== '{') {
                throw new Error(`morph '${nameTok.value}': expected '{' template body at line ${t.line}`);
            }
            if (macros.has(nameTok.value)) {
                throw new Error(`morph: duplicate morph '${nameTok.value}' at line ${t.line}, col ${t.column}`);
            }
            macros.set(nameTok.value, {
                name: nameTok.value,
                params: parsePattern(paramGroup.tokens),
                template: parseTemplate(bodyGroup.tokens),
                templateRaw: bodyGroup.tokens
            });
            i += 4;
            continue;
        }
        out.push(t);
        i++;
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 模式 / 模板解析
// ────────────────────────────────────────────────────────────────────────────

function isQuantifier(t: TokenTree): boolean {
    return isLeaf(t) && (t.type === TokenType.Star || t.type === TokenType.Plus);
}

function parsePattern(tokens: TokenTree[]): PatItem[] {
    const items: PatItem[] = [];
    let i = 0;
    while (i < tokens.length) {
        const t = tokens[i];
        if (isGroup(t)) {
            items.push({ kind: 'group', openTok: t.openTok, closeTok: t.closeTok, items: parsePattern(t.tokens) });
            i++;
            continue;
        }
        if (t.type === TokenType.Dollar) {
            const next = tokens[i + 1];
            if (isGroup(next) && next.openTok.value === '(') {
                const inner = parsePattern(next.tokens);
                i += 2;
                let sep: Token | null = null;
                if (i < tokens.length && isLeaf(tokens[i]) && !isQuantifier(tokens[i])) {
                    sep = tokens[i] as Token;
                    i++;
                }
                const q = tokens[i];
                if (!isQuantifier(q)) {
                    throw new Error(`morph: expected '*' or '+' after repetition at line ${t.line}`);
                }
                i++;
                items.push({ kind: 'repeat', inner, sep, quantifier: (q as Token).type === TokenType.Star ? '*' : '+' });
                continue;
            }
            if (isLeaf(next) && next.type === TokenType.Identifier) {
                const colon = tokens[i + 2];
                const kindTok = tokens[i + 3];
                if (!isLeaf(colon) || colon.type !== TokenType.Colon) {
                    throw new Error(`morph: expected ':' in metavariable at line ${t.line}`);
                }
                if (!isLeaf(kindTok) || !MORPH_KINDS.has(kindTok.value)) {
                    throw new Error(`morph: unknown metavariable kind '${isLeaf(kindTok) ? kindTok.value : '?'}' at line ${t.line}`);
                }
                items.push({ kind: 'meta', name: next.value, mkind: kindTok.value as MorphKind });
                i += 4;
                continue;
            }
            throw new Error(`morph: malformed metavariable at line ${t.line}`);
        }
        items.push({ kind: 'tok', token: t });
        i++;
    }
    return items;
}

function parseTemplate(tokens: TokenTree[]): TplItem[] {
    const items: TplItem[] = [];
    let i = 0;
    while (i < tokens.length) {
        const t = tokens[i];
        if (isGroup(t)) {
            items.push({ kind: 'group', openTok: t.openTok, closeTok: t.closeTok, items: parseTemplate(t.tokens) });
            i++;
            continue;
        }
        if (t.type === TokenType.Dollar) {
            const next = tokens[i + 1];
            if (isGroup(next) && next.openTok.value === '(') {
                const inner = parseTemplate(next.tokens);
                i += 2;
                let sep: Token | null = null;
                if (i < tokens.length && isLeaf(tokens[i]) && !isQuantifier(tokens[i])) {
                    sep = tokens[i] as Token;
                    i++;
                }
                const q = tokens[i];
                if (!isQuantifier(q)) {
                    throw new Error(`morph: expected '*' or '+' after repetition at line ${t.line}`);
                }
                i++;
                items.push({ kind: 'repeat', inner, sep, quantifier: (q as Token).type === TokenType.Star ? '*' : '+' });
                continue;
            }
            if (isLeaf(next) && next.type === TokenType.Identifier) {
                items.push({ kind: 'meta', name: next.value });
                i += 2;
                continue;
            }
            throw new Error(`morph: malformed metavariable at line ${t.line}`);
        }
        if (t.type === TokenType.Hash) {
            const dollar = tokens[i + 1];
            const nameTok = tokens[i + 2];
            if (isLeaf(dollar) && dollar.type === TokenType.Dollar && isLeaf(nameTok) && nameTok.type === TokenType.Identifier) {
                items.push({ kind: 'stringify', name: nameTok.value });
                i += 3;
                continue;
            }
            throw new Error(`morph: expected '#$name' stringify at line ${t.line}`);
        }
        items.push({ kind: 'tok', token: t });
        i++;
    }
    return items;
}

// ────────────────────────────────────────────────────────────────────────────
// 匹配（模式 → 实参 token 树）
// ────────────────────────────────────────────────────────────────────────────

function sameToken(a: TokenTree | undefined, b: Token): boolean {
    return isLeaf(a) && a.type === b.type && a.value === b.value;
}

/** 捕获一个 metavariable 的 token 片段；返回捕获结果与新的位置。 */
function captureMeta(mkind: MorphKind, args: TokenTree[], pos: number): { tokens: TokenTree[]; newPos: number } {
    if (mkind === 'tt') {
        if (pos >= args.length) {
            throw new Error('morph: expected token tree for `:tt`');
        }
        return { tokens: [args[pos]], newPos: pos + 1 };
    }
    if (mkind === 'ident') {
        const t = args[pos];
        if (!t || !isLeaf(t) || t.type !== TokenType.Identifier) {
            throw new Error('morph: expected `:ident`');
        }
        return { tokens: [t], newPos: pos + 1 };
    }
    if (mkind === 'literal') {
        const t = args[pos];
        if (!t || !isLeaf(t) || (t.type !== TokenType.Number && t.type !== TokenType.String)) {
            throw new Error('morph: expected `:literal`');
        }
        return { tokens: [t], newPos: pos + 1 };
    }
    if (mkind === 'type') {
        const captured: TokenTree[] = [];
        const t = args[pos];
        if (!t || !isLeaf(t) || (t.type !== TokenType.Identifier && t.type !== TokenType.Keyword)) {
            throw new Error('morph: expected `:type`');
        }
        captured.push(t);
        let p = pos + 1;
        // 泛型后缀：cap<T> / arr<T,N> / lattice<T,B> / fn<...>
        if (p < args.length && isLeaf(args[p]) && (args[p] as Token).type === TokenType.LessThan) {
            let depth = 0;
            while (p < args.length) {
                const cur = args[p];
                if (isLeaf(cur) && (cur as Token).type === TokenType.LessThan) depth++;
                if (isLeaf(cur) && (cur as Token).type === TokenType.GreaterThan) {
                    depth--;
                    captured.push(cur);
                    p++;
                    if (depth === 0) break;
                    continue;
                }
                captured.push(cur);
                p++;
            }
        }
        return { tokens: captured, newPos: p };
    }
    // expr / stmt / items：捕获直到顶层分隔符或参数末尾。
    const captured: TokenTree[] = [];
    let p = pos;
    while (p < args.length) {
        const cur = args[p];
        if (isLeaf(cur)) {
            const ty = (cur as Token).type;
            if (ty === TokenType.Comma) break;
            if (mkind === 'stmt' && ty === TokenType.Semicolon) break;
        }
        captured.push(cur);
        p++;
    }
    return { tokens: captured, newPos: p };
}

function matchItems(items: PatItem[], args: TokenTree[], pos: number, env: MatchEnv): number {
    let p = pos;
    for (const it of items) {
        if (it.kind === 'tok') {
            if (!sameToken(args[p], it.token)) {
                throw new Error(`morph: unexpected token at position ${p}`);
            }
            p++;
        } else if (it.kind === 'group') {
            const cur = args[p];
            if (!isGroup(cur) || cur.openTok.value !== it.openTok.value) {
                throw new Error(`morph: expected '${it.openTok.value}' at position ${p}`);
            }
            const innerEnd = matchItems(it.items, cur.tokens, 0, env);
            if (innerEnd !== cur.tokens.length) {
                throw new Error('morph: pattern group did not consume all tokens');
            }
            p++;
        } else if (it.kind === 'meta') {
            const { tokens, newPos } = captureMeta(it.mkind, args, p);
            env.outer.set(it.name, tokens);
            p = newPos;
        } else {
            // repeat
            const iterations: Map<string, TokenTree[]>[] = [];
            let rp = p;
            for (;;) {
                const sub: MatchEnv = { outer: new Map(), repeatBinds: [] };
                let np: number;
                try {
                    np = matchItems(it.inner, args, rp, sub);
                } catch {
                    break;
                }
                if (np === rp) break;
                iterations.push(sub.outer);
                rp = np;
                if (it.sep) {
                    if (sameToken(args[rp], it.sep)) {
                        rp++;
                        continue;
                    }
                    break;
                }
                // 无分隔符：靠下一轮是否还能匹配来推进。
            }
            if (iterations.length === 0 && it.quantifier === '+') {
                throw new Error('morph: repetition `+` requires at least one match');
            }
            env.repeatBinds.push(iterations);
            p = rp;
        }
    }
    return p;
}

// ────────────────────────────────────────────────────────────────────────────
// 实例化（模板 → token 树）
// ────────────────────────────────────────────────────────────────────────────

function lookupScoped(scopes: Map<string, TokenTree[]>[], name: string): TokenTree[] | undefined {
    for (let i = scopes.length - 1; i >= 0; i--) {
        const v = scopes[i].get(name);
        if (v !== undefined) return v;
    }
    return undefined;
}

function instantiate(macro: MorphMacro, argTokens: TokenTree[]): TokenTree[] {
    const env: MatchEnv = { outer: new Map(), repeatBinds: [] };
    const end = matchItems(macro.params, argTokens, 0, env);
    if (end !== argTokens.length) {
        throw new Error(`morph '${macro.name}': too many arguments`);
    }
    const introduced = collectIntroduced(macro.templateRaw);
    const gauge = freshGauge();
    return inst(macro.template, env, gauge, introduced, [env.outer], { i: 0 });
}

interface Cursor {
    i: number;
}

function inst(
    items: TplItem[],
    env: MatchEnv,
    gauge: number,
    introduced: Set<string>,
    scopes: Map<string, TokenTree[]>[],
    cursor: Cursor
): TokenTree[] {
    const out: TokenTree[] = [];
    for (const it of items) {
        if (it.kind === 'tok') {
            out.push(applyHygiene(it.token, introduced, gauge));
        } else if (it.kind === 'group') {
            out.push({
                kind: 'group',
                openTok: it.openTok,
                closeTok: it.closeTok,
                tokens: inst(it.items, env, gauge, introduced, scopes, cursor)
            });
        } else if (it.kind === 'stringify') {
            const captured = lookupScoped(scopes, it.name);
            if (captured === undefined) {
                throw new Error(`morph: unbound metavariable '$${it.name}' in stringify`);
            }
            const src = serializeTokens(captured);
            out.push({ type: TokenType.String, value: src, line: 0, column: 0, length: src.length + 2 });
        } else if (it.kind === 'meta') {
            const captured = lookupScoped(scopes, it.name);
            if (captured === undefined) {
                throw new Error(`morph: unbound metavariable '$${it.name}'`);
            }
            out.push(...captured);
        } else {
            const iterations = env.repeatBinds[cursor.i++];
            if (!iterations) {
                throw new Error('morph: repetition mismatch between pattern and template');
            }
            for (const iterScope of iterations) {
                out.push(...inst(it.inner, env, gauge, introduced, [...scopes, iterScope], cursor));
            }
        }
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 卫生 = 规范不变性（模板引入绑定 → 取新原子）
// ────────────────────────────────────────────────────────────────────────────

/** 绑定位置上的类型关键字（声明 / 参数的类型前导） */
const BINDER_TYPE_KEYWORDS = new Set([
    'int8', 'int16', 'int32', 'int64',
    'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'complex64', 'complex128', 'string', 'char',
    'Qubit', 'QObject', 'QModel', 'DiracState', 'BellState', 'QuantumRegister',
    'cap', 'lattice', 'arr'
]);

/**
 * 收集模板引入的绑定名（保守启发式，覆盖 rvdl 核心绑定形态）：
 *   - `let x = ...` / `auto x = ...`
 *   - `fixed TYPE x = ...`
 *   - `fn name(...)` → name + 参数名（参数由递归进入 `(...)` 组，命中 `TYPE p` 规则）
 *   - `TYPE name = ...` / `TYPE name(...)` → name + 参数名
 */
function collectIntroduced(tree: TokenTree[]): Set<string> {
    const names = new Set<string>();
    scan(tree);
    return names;

    function scan(tts: TokenTree[]): void {
        for (let i = 0; i < tts.length; i++) {
            const t = tts[i];
            if (isGroup(t)) {
                scan(t.tokens);
                continue;
            }
            // 元变量 $name 与字符串化 #$name：跳过，其名字不是绑定。
            if (t.type === TokenType.Dollar || t.type === TokenType.Hash) {
                i++;
                continue;
            }
            if (t.type === TokenType.Keyword && (t.value === 'let' || t.value === 'auto')) {
                const n = tts[i + 1];
                if (n && isLeaf(n) && n.type === TokenType.Identifier) names.add(n.value);
                continue;
            }
            if (t.type === TokenType.Keyword && t.value === 'fixed') {
                const n = tts[i + 2];
                if (n && isLeaf(n) && n.type === TokenType.Identifier) names.add(n.value);
                continue;
            }
            if (t.type === TokenType.Keyword && t.value === 'fn') {
                const n = tts[i + 1];
                if (n && isLeaf(n) && n.type === TokenType.Identifier) names.add(n.value);
                continue;
            }
            if (t.type === TokenType.Keyword && BINDER_TYPE_KEYWORDS.has(t.value)) {
                const n = tts[i + 1];
                if (n && isLeaf(n) && n.type === TokenType.Identifier) names.add(n.value);
                continue;
            }
        }
    }
}

function applyHygiene(tok: Token, introduced: Set<string>, gauge: number): Token {
    if (tok.type === TokenType.Identifier && introduced.has(tok.value)) {
        return { ...tok, value: tok.value + '__m' + gauge };
    }
    return tok;
}

// ────────────────────────────────────────────────────────────────────────────
// 字符串化（#$name → 捕获片段的源文本）
// ────────────────────────────────────────────────────────────────────────────

function serializeTokens(tree: TokenTree[]): string {
    return tree
        .map(t => (isGroup(t) ? t.openTok.value + serializeTokens(t.tokens) + t.closeTok.value : t.value))
        .join(' ');
}

// ────────────────────────────────────────────────────────────────────────────
// 不动点展开（递归遍历 token 树，替换 `name!(...)`）
// ────────────────────────────────────────────────────────────────────────────

function expandTree(tree: TokenTree[], macros: Map<string, MorphMacro>): { tokens: TokenTree[]; changed: boolean } {
    let changed = false;
    const out: TokenTree[] = [];
    for (let i = 0; i < tree.length; i++) {
        const t = tree[i];
        if (isGroup(t)) {
            const r = expandTree(t.tokens, macros);
            changed = changed || r.changed;
            out.push({ ...t, tokens: r.tokens });
        } else if (t.type === TokenType.Identifier) {
            const bang = tree[i + 1];
            const open = tree[i + 2];
            if (isLeaf(bang) && bang.type === TokenType.Bang && isGroup(open) && open.openTok.value === '(') {
                const macro = macros.get(t.value);
                if (!macro) {
                    throw new Error(`morph: undefined morph '${t.value}' at line ${t.line}, col ${t.column}`);
                }
                out.push(...instantiate(macro, open.tokens));
                i += 2; // 跳过 `!` 与参数组
                changed = true;
            } else {
                out.push(t);
            }
        } else {
            out.push(t);
        }
    }
    return { tokens: out, changed };
}