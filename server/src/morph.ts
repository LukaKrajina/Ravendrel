import { Token, TokenType } from './lexer';


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


export type MorphKind = 'expr' | 'stmt' | 'items' | 'type' | 'ident' | 'literal' | 'tt';
const MORPH_KINDS = new Set(['expr', 'stmt', 'items', 'type', 'ident', 'literal', 'tt']);


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
    templateRaw: TokenTree[];
}

interface MatchEnv {
    outer: Map<string, TokenTree[]>;
    repeatBinds: Map<string, TokenTree[]>[][];
}

const MAX_RECURSION = 64;

let gaugeCounter = 0;
function freshGauge(): number {
    return ++gaugeCounter;
}


export function expandMorphTokens(tokens: Token[]): Token[] {
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


function sameToken(a: TokenTree | undefined, b: Token): boolean {
    return isLeaf(a) && a.type === b.type && a.value === b.value;
}

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


const BINDER_TYPE_KEYWORDS = new Set([
    'int8', 'int16', 'int32', 'int64',
    'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'complex64', 'complex128', 'string', 'char',
    'Qubit', 'QObject', 'QModel', 'DiracState', 'BellState', 'QuantumRegister',
    'cap', 'lattice', 'arr'
]);







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


function serializeTokens(tree: TokenTree[]): string {
    return tree
        .map(t => (isGroup(t) ? t.openTok.value + serializeTokens(t.tokens) + t.closeTok.value : t.value))
        .join(' ');
}


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
                i += 2;
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